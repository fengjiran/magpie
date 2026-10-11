#include "../support/watchdog.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <gtest/gtest.h>
#include <latch>
#include <magpie/build_config.hpp>
#include <magpie/detail/test_hooks.hpp>
#include <magpie/thread_pool.hpp>
#include <memory>
#include <thread>
#include <vector>

// 标定形态下测试 target 由 CMake 传入映射宏；未传入时使用主设计 §16.2 默认。
#if !defined(MAGPIE_BULK_LIMIT)
#define MAGPIE_BULK_LIMIT 32
#endif

namespace {
using Point = magpie::detail::PoolTestHookPoint;
struct State {
    std::atomic<std::size_t> pending{0}, gate{0}, largest_batch{0};
    std::atomic<std::uint64_t> victims{0};
    std::array<std::atomic<std::uint64_t>, 4> per_worker_victims{};
    void snapshot(const magpie::detail::PoolTestHooks::Snapshot& s) noexcept {
        pending.store(s.pending);
        gate.store(s.gate);
    }
    void dump() const noexcept {
        std::fprintf(stderr, "last snapshot pending=%zu gate=%zu batch=%zu victim-mask=%llu\n",
                     pending.load(), gate.load(), largest_batch.load(),
                     static_cast<unsigned long long>(victims.load()));
    }
};
void hook(Point point, std::size_t index, void* context) noexcept {
    auto& s = *static_cast<State*>(context);
    thread_local std::size_t scanner = 4;
    if (point == Point::BeforeFullScan) { scanner = index; }
    if (point == Point::BatchTransferred) {
        auto old = s.largest_batch.load();
        while (old < index && !s.largest_batch.compare_exchange_weak(old, index)) {
        }
    }
    if (point == Point::BeforeVictimProbe && index < 64) {
        s.victims.fetch_or(std::uint64_t{1} << index);
        if (scanner < 4) { s.per_worker_victims[scanner].fetch_or(std::uint64_t{1} << index); }
    }
}
struct Install {
    explicit Install(const magpie::detail::PoolTestHooks& h) {
        magpie::detail::install_pool_test_hooks(&h);
    }
    ~Install() { magpie::detail::install_pool_test_hooks(nullptr); }
};
magpie::ThreadPoolOptions opts(std::size_t workers = 1, std::size_t local = 16) {
    magpie::ThreadPoolOptions o;
    o.worker_count = workers;
    o.local_deque_capacity = local;
    o.global_queue_capacity = 16;
    o.rejection = magpie::RejectionPolicy::Abort;
    return o;
}
void full_case(std::size_t capacity) {
    State state;
    magpie::detail::PoolTestHooks hooks{hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<State*>(ctx)->snapshot(s);
    };
    Install install(hooks);
    magpie::test::Watchdog wd([&] { state.dump(); });
    magpie::ThreadPool pool(opts(1, capacity));
    std::latch parent_running{1}, external_full{1};
    auto seen = std::make_unique<std::atomic<unsigned>[]>(capacity + 2);
    std::atomic<unsigned> external_seen{0};
    std::atomic<bool> guarded{false};
    pool.submit([&] {
        parent_running.count_down();
        external_full.wait();
        for (std::size_t i = 0; i < capacity; ++i)
            pool.submit([&, i] { ++seen[i]; });
        pool.submit([&] {
            ++seen[capacity];
            // Both queues remain full while the owner is inside CallerRuns.
            pool.submit([&] { ++seen[capacity + 1]; });
            try {
                pool.drain();
            } catch (const std::logic_error&) {
                guarded.store(true);
            }
        });
    });
    parent_running.wait();
    for (unsigned i = 0; i < 16; ++i)
        pool.submit([&] { ++external_seen; });
    external_full.count_down();
    pool.drain();
    pool.shutdown();
    for (std::size_t i = 0; i < capacity + 2; ++i)
        EXPECT_EQ(seen[i].load(), 1U);
    EXPECT_EQ(external_seen.load(), 16U);
    EXPECT_TRUE(guarded.load());
    auto s = pool.stats();
    EXPECT_EQ(s.pending, 0U);
    EXPECT_EQ(s.inline_completed, 2U);
    EXPECT_EQ(s.local_spills, 2U);
    EXPECT_EQ(s.submitted, s.completed + s.discarded);
}
} // namespace
TEST(WorkStealing, LocalOverflowTaskConservation) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    full_case(16);
}
TEST(WorkStealing, LocalOverflowDefaultCapacity) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    full_case(1024);
}

TEST(WorkStealing, BulkReceiveCapacityBound) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    State state;
    magpie::detail::PoolTestHooks hooks{hook, nullptr, &state};
    Install install(hooks);
    magpie::test::Watchdog wd([&] { state.dump(); });
    magpie::ThreadPool pool(opts());
    std::latch running{1}, release{1};
    std::atomic<unsigned> done{0};
    pool.submit([&] {
        running.count_down();
        release.wait();
    });
    running.wait();
    for (unsigned i = 0; i < 16; ++i)
        pool.submit([&] { ++done; });
    release.count_down();
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(done.load(), 16U);
    // 批接收量按本地容量钳制：上限为 min(BULK_LIMIT, local_capacity)。
    EXPECT_EQ(state.largest_batch.load(),
              std::min<unsigned>(MAGPIE_BULK_LIMIT, 16U));
}
TEST(WorkStealing, LocalFullForwardsOnlyIncomingTask) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    State state;
    magpie::detail::PoolTestHooks hooks{hook, nullptr, &state};
    Install install(hooks);
    magpie::test::Watchdog wd([&] { state.dump(); });
    magpie::ThreadPool pool(opts());
    std::atomic<unsigned> done{0};
    pool.submit([&] {
        for (unsigned i = 0; i < 17; ++i)
            pool.submit([&] { ++done; });
        // Local deque full, global has room; no old local item was moved.
        EXPECT_EQ(pool.stats().local_spills, 1U);
        EXPECT_EQ(pool.stats().inline_completed, 0U);
    });
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(done.load(), 17U);
    EXPECT_EQ(pool.stats().inline_completed, 0U);
}
TEST(WorkStealing, CrossPoolSubmitAndShutdownRejectChild) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    State state;
    magpie::test::Watchdog wd([&] { state.dump(); });
    magpie::ThreadPool a(opts()), b(opts());
    std::latch entered{1}, release{1};
    std::atomic<bool> guarded{false};
    std::atomic<unsigned> cross{0};
    a.submit([&] {
        b.submit([&] { ++cross; });
        entered.count_down();
        release.wait();
        try {
            a.submit([] {});
        } catch (const magpie::QueueFullError& e) {
            guarded.store(e.reason == magpie::QueueFullReason::ShuttingDown);
        }
    });
    entered.wait();
    a.shutdown();
    release.count_down();
    a.drain();
    b.drain();
    b.shutdown();
    EXPECT_TRUE(guarded.load());
    EXPECT_EQ(cross.load(), 1U);
    EXPECT_EQ(b.stats().submitted, 1U);
}
TEST(WorkStealing, FullScanCoversAllVictims) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    State state;
    magpie::detail::PoolTestHooks hooks{hook, nullptr, &state};
    Install install(hooks);
    magpie::test::Watchdog wd([&] { state.dump(); });
    {
        magpie::ThreadPool pool(opts(4));
        // Every worker must reach a full scan while running. M5 may exit
        // immediately after stopping, without performing another scan.
        for (unsigned worker = 0; worker < 4; ++worker) {
            while (state.per_worker_victims[worker].load() != (15U ^ (1U << worker)))
                std::this_thread::yield();
        }
        pool.shutdown();
        pool.drain();
    } // Join every worker before examining its complete final scan.
    EXPECT_EQ(state.victims.load(), 15U);
    for (unsigned worker = 0; worker < 4; ++worker) {
        EXPECT_EQ(state.per_worker_victims[worker].load(), 15U ^ (1U << worker));
    }
}
TEST(WorkStealing, IdleThiefExecutesLocalChildren) {
    if (!MAGPIE_ENABLE_WORK_STEALING)
        GTEST_SKIP();
    State state;
    magpie::detail::PoolTestHooks hooks{hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<State*>(ctx)->snapshot(s);
    };
    Install install(hooks);
    magpie::test::Watchdog wd([&] { state.dump(); });
    magpie::ThreadPool pool(opts(2));
    std::latch children_done{16};
    std::atomic<unsigned> done{0};
    pool.submit([&] {
        for (unsigned i = 0; i < 16; ++i)
            pool.submit([&] {
                ++done;
                children_done.count_down();
            });
        // Not a future/helping wait: deterministic fixture holds the owner so
        // the other worker must claim local items. Watchdog bounds the fixture.
        children_done.wait();
    });
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(done.load(), 16U);
    EXPECT_GT(pool.stats().stolen, 0U);
}
