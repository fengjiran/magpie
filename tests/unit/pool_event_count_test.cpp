#include "../support/watchdog.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <latch>
#include <magpie/detail/test_hooks.hpp>
#include <magpie/thread_pool.hpp>
#include <thread>

// 标定形态下测试 target 由 CMake 传入映射宏；未传入时使用主设计 §16.2 默认。
#if !defined(MAGPIE_BULK_LIMIT)
#define MAGPIE_BULK_LIMIT 32
#endif

namespace {
using P = magpie::detail::PoolTestHookPoint;
struct State {
    P pause_point;
    std::latch reached{1}, release{1};
    std::atomic<bool> used{false};
    std::atomic<std::uint64_t> epoch{0}, registered{0}, pending{0}, exits{0};
    static void point(P p, std::size_t, void* context) noexcept {
        auto& s = *static_cast<State*>(context);
        if (p == P::WorkerExit)
            ++s.exits;
        if (p == s.pause_point && !s.used.exchange(true)) {
            s.reached.count_down();
            s.release.wait();
        }
    }
    static void snapshot(const magpie::detail::PoolTestHooks::Snapshot& value,
                         void* context) noexcept {
        auto& s = *static_cast<State*>(context);
        s.epoch.store(value.epoch);
        s.registered.store(value.registered_waiters);
        s.pending.store(value.pending);
    }
    void dump() const {
        std::fprintf(stderr, "pool epoch=%llu registered=%llu pending=%llu exits=%llu\n",
                     static_cast<unsigned long long>(epoch.load()),
                     static_cast<unsigned long long>(registered.load()),
                     static_cast<unsigned long long>(pending.load()),
                     static_cast<unsigned long long>(exits.load()));
    }
};
struct Install {
    magpie::detail::PoolTestHooks hooks;
    explicit Install(State& s) : hooks{State::point, nullptr, &s, State::snapshot} {
        magpie::detail::install_pool_test_hooks(&hooks);
    }
    ~Install() { magpie::detail::install_pool_test_hooks(nullptr); }
};
magpie::ThreadPoolOptions options(unsigned workers = 1) {
    magpie::ThreadPoolOptions o;
    o.worker_count = workers;
    o.local_deque_capacity = 16;
    o.global_queue_capacity = 16;
    return o;
}
void stop_race(P point) {
    State state{point};
    Install hooks(state);
    magpie::test::Watchdog watchdog([&] { state.dump(); });
    {
        magpie::ThreadPool pool(options());
        state.reached.wait();
        pool.shutdown();
        state.release.count_down();
        pool.drain();
    }
    EXPECT_EQ(state.registered.load(), 0U);
    EXPECT_EQ(state.exits.load(), 1U);
}
} // namespace
TEST(PoolEventCount, ShutdownBeforeRegistration) { stop_race(P::BeforeEventPrepare); }
TEST(PoolEventCount, ShutdownAfterRegisteredCheck) { stop_race(P::AfterEventStopCheck); }
TEST(PoolEventCount, ShutdownAfterEpochRecheck) { stop_race(P::EventBeforeBackendWait); }
TEST(PoolEventCount, FinalScanUnregistersBeforeUserCode) {
    State state{P::AfterEventStopCheck};
    Install hooks(state);
    magpie::test::Watchdog watchdog([&] { state.dump(); });
    magpie::ThreadPool pool(options());
    state.reached.wait();
    std::atomic<bool> unregistered{false};
    pool.submit([&] { unregistered.store(pool.stats().registered_waiters == 0); });
    state.release.count_down();
    pool.drain();
    pool.shutdown();
    EXPECT_TRUE(unregistered.load());
    EXPECT_GE(pool.stats().pre_sleep_scan_hit, 1U);
}
TEST(PoolEventCount, SpawnWhileIdleWakeup) {
    State state{P::EventBeforeBackendWait};
    Install hooks(state);
    magpie::test::Watchdog watchdog([&] { state.dump(); });
    magpie::ThreadPool pool(options(2));
    state.reached.wait();
    state.release.count_down();
    std::latch children{16};
    std::atomic<unsigned> count{0};
    pool.submit([&] {
        for (unsigned i = 0; i < 16; ++i)
            pool.submit([&] {
                ++count;
                children.count_down();
            });
        children.wait();
    });
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(count.load(), 16U);
    EXPECT_GT(pool.stats().epoch, 0U);
}
TEST(PoolEventCount, DrainDuringShutdown) {
    State state{P::EventBeforeBackendWait};
    Install hooks(state);
    magpie::test::Watchdog watchdog([&] { state.dump(); });
    magpie::ThreadPool pool(options(4));
    state.reached.wait();
    state.release.count_down();
    std::latch body{1}, release{1};
    std::atomic<bool> drained{false};
    pool.submit([&] {
        body.count_down();
        release.wait();
    });
    body.wait();
    pool.shutdown();
    std::thread drainer([&] {
        pool.drain();
        drained.store(true);
    });
    EXPECT_FALSE(drained.load());
    release.count_down();
    drainer.join();
    EXPECT_TRUE(drained.load());
    EXPECT_EQ(pool.stats().pending, 0U);
}

TEST(PoolEventCount, FullFinalScanBeforeEveryPark) {
    struct Scan {
        std::array<std::atomic<unsigned>, 4> masks{}, boundaries{};
        std::atomic<unsigned> invalid{0};
        static void hook(P point, std::size_t index, void* context) noexcept {
            auto& s = *static_cast<Scan*>(context);
            thread_local unsigned worker = 4;
            if (point == P::EventRegistered) {
                worker = static_cast<unsigned>(index);
                s.masks[worker].store(0);
            }
            if (point == P::BeforeVictimProbe && worker < 4)
                s.masks[worker].fetch_or(1U << index);
            if (point == P::WorkerBeforeWait) {
                if (s.masks[index].load() != (15U ^ (1U << index)))
                    ++s.invalid;
                ++s.boundaries[index];
            }
            if (point == P::EventAfterLeave)
                worker = 4;
        }
    } state;
    magpie::detail::PoolTestHooks hooks{Scan::hook, nullptr, &state};
    magpie::detail::install_pool_test_hooks(&hooks);
    struct Reset {
        ~Reset() { magpie::detail::install_pool_test_hooks(nullptr); }
    } reset;
    magpie::test::Watchdog watchdog(
        [&] { std::fprintf(stderr, "invalid scans=%u\n", state.invalid.load()); });
    {
        magpie::ThreadPool pool(options(4));
        for (unsigned i = 0; i < 4; ++i)
            while (state.boundaries[i].load() == 0)
                std::this_thread::yield();
        for (unsigned round = 0; round < 50; ++round) {
            pool.submit([] {});
            pool.drain();
        }
        pool.shutdown();
    }
    EXPECT_EQ(state.invalid.load(), 0U);
}

namespace {
struct ExecutionClaim {
    std::latch idle{2}, claimed{1}, release_claim{1}, peer_observed{1};
    std::atomic<bool> armed{false}, observed{false};
    std::atomic<unsigned> owner{2}, peer_probes{0}, stolen_claims{0}, executed{0};
    bool from_victim = false;

    void observe_peer() noexcept {
        if (!observed.exchange(true)) { peer_observed.count_down(); }
    }
    static void hook(P point, std::size_t index, void* context) noexcept {
        auto& s = *static_cast<ExecutionClaim*>(context);
        thread_local unsigned worker = 2;
        if (point == P::BeforeEventPrepare && worker == 2) {
            worker = static_cast<unsigned>(index);
            s.idle.count_down();
        }
        if (!s.armed.load()) { return; }
        if (point == P::DequeStealBeforeCas && !s.from_victim) {
            // In the old implementation a peer could steal the republished
            // singleton before its receiver reached BatchTransferred.
            s.claimed.wait();
        }
        if (point == P::BatchTransferred) {
            unsigned unset = 2;
            if (s.owner.compare_exchange_strong(unset, worker)) {
                s.claimed.count_down();
                s.release_claim.wait();
            } else if (worker != s.owner.load()) {
                ++s.stolen_claims;
                s.observe_peer();
            }
        }
        if (point == P::BeforeVictimProbe && s.owner.load() < 2 &&
            worker != s.owner.load() && ++s.peer_probes >= 2) {
            // Reaching the next probe proves that the previous steal returned
            // while the receiver was still paused. No timing assumption needed.
            s.observe_peer();
        }
    }
};

void execution_claim_during_shutdown(bool from_victim) {
    ExecutionClaim state;
    state.from_victim = from_victim;
    magpie::detail::PoolTestHooks hooks{ExecutionClaim::hook, nullptr, &state};
    magpie::detail::install_pool_test_hooks(&hooks);
    struct Reset {
        ~Reset() { magpie::detail::install_pool_test_hooks(nullptr); }
    } reset;
    magpie::test::Watchdog watchdog([&] {
        std::fprintf(stderr, "claim owner=%u peer-probes=%u stolen-claims=%u executed=%u\n",
                     state.owner.load(), state.peer_probes.load(), state.stolen_claims.load(),
                     state.executed.load());
    });
    std::latch release_parent{1};
    {
        magpie::ThreadPool pool(options(2));
        state.idle.wait();
        if (from_victim) {
            pool.submit([&] {
                state.armed.store(true);
                pool.submit([&] { ++state.executed; });
                release_parent.wait();
            });
        } else {
            state.armed.store(true);
            pool.submit([&] { ++state.executed; });
        }
        state.claimed.wait();
        pool.shutdown();
        if (from_victim) { release_parent.count_down(); }
        state.peer_observed.wait();
        EXPECT_EQ(state.stolen_claims.load(), 0U);
        EXPECT_EQ(state.executed.load(), 0U);
        EXPECT_EQ(pool.stats().pending, 1U);
        state.release_claim.count_down();
        pool.drain();
        EXPECT_EQ(state.executed.load(), 1U);
        EXPECT_EQ(pool.stats().pending, 0U);
        EXPECT_EQ(pool.stats().submitted, from_victim ? 2U : 1U);
        EXPECT_EQ(pool.stats().completed, pool.stats().submitted);
    }
}
} // namespace

TEST(PoolEventCount, GlobalBatchKeepsExecutionClaimDuringShutdown) {
    execution_claim_during_shutdown(false);
}
TEST(PoolEventCount, StolenBatchKeepsExecutionClaimDuringShutdown) {
    execution_claim_during_shutdown(true);
}

TEST(PoolEventCount, BatchRemainderWakesParkedWorker) {
    // Fixture 依赖一次全局 bulk 领取至少 16 个票据（index==17 为最后一项）；
    // BULK_LIMIT<16 的标定形态下该前提不成立，由默认形态守护本语义。
    if (MAGPIE_BULK_LIMIT < 16)
        GTEST_SKIP();
    struct Transfer {
        std::latch last_claimed{1}, allow_last{1}, waiter_seen{1};
        std::atomic<bool> collecting{false}, parked_recorded{false};
        std::atomic<unsigned> leaves{0};
        static void hook(P point, std::size_t index, void* context) noexcept {
            auto& s = *static_cast<Transfer*>(context);
            // Two running roots precede the 16 queued tasks. At ticket 17 all
            // queue positions are owned but no remainder is locally published.
            if (point == P::MpmcDequeueClaim && index == 17) {
                s.collecting.store(true);
                s.last_claimed.count_down();
                s.allow_last.wait();
            }
            if (point == P::WorkerBeforeWait && s.collecting.load() &&
                !s.parked_recorded.exchange(true)) {
                s.waiter_seen.count_down();
            }
        }
    } state;
    magpie::detail::PoolTestHooks hooks{Transfer::hook, nullptr, &state};
    magpie::detail::install_pool_test_hooks(&hooks);
    struct Reset {
        ~Reset() { magpie::detail::install_pool_test_hooks(nullptr); }
    } reset;
    magpie::test::Watchdog watchdog([&] {
        std::fprintf(stderr, "batch collecting=%d parked=%d leaves=%u\n",
                     state.collecting.load(), state.parked_recorded.load(), state.leaves.load());
    });
    std::latch a_started{1}, release_a{1}, b_started{1}, release_b{1};
    std::latch first_started{1}, release_first{1}, leaves_done{15};
    magpie::ThreadPool pool(options(2));
    pool.submit([&] { a_started.count_down(); release_a.wait(); });
    a_started.wait();
    pool.submit([&] { b_started.count_down(); release_b.wait(); });
    b_started.wait();
    pool.submit([&] { first_started.count_down(); release_first.wait(); });
    for (unsigned i = 0; i < 15; ++i) {
        pool.submit([&] { ++state.leaves; leaves_done.count_down(); });
    }
    release_a.count_down();
    state.last_claimed.wait();
    release_b.count_down();
    state.waiter_seen.wait();
    state.allow_last.count_down();
    first_started.wait();
    leaves_done.wait();
    EXPECT_EQ(state.leaves.load(), 15U);
    release_first.count_down();
    pool.shutdown();
    pool.drain();
    EXPECT_EQ(pool.stats().pending, 0U);
    EXPECT_EQ(pool.stats().completed, 18U);
}
