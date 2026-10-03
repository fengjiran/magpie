#include <magpie/thread_pool.hpp>
#include <magpie/detail/test_hooks.hpp>
#include <magpie/build_config.hpp>
#include <gtest/gtest.h>
#include "../support/watchdog.hpp"

#include <atomic>
#include <cstdio>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
#include <memory>

namespace {
using Point = magpie::detail::PoolTestHookPoint;
struct Trace {
    std::atomic<std::size_t> gate{0}, pending{0}, enqueue{0}, dequeue{0};
    std::atomic<bool> stopping{false};
    void record(const magpie::detail::PoolTestHooks::Snapshot& s) noexcept {
        gate.store(s.gate);
        pending.store(s.pending);
        enqueue.store(s.enqueue_position);
        dequeue.store(s.dequeue_position);
        stopping.store(s.stopping);
    }
    void dump() const noexcept {
        std::fprintf(stderr,
                     "last atomic hook snapshot: stopping=%d gate=%zu pending=%zu enqueue=%zu "
                     "dequeue=%zu; thread stacks unavailable in portable watchdog\n",
                     stopping.load(), gate.load(), pending.load(), enqueue.load(), dequeue.load());
    }
};
struct Install {
    explicit Install(const magpie::detail::PoolTestHooks& hooks) {
        magpie::detail::install_pool_test_hooks(&hooks);
    }
    ~Install() { magpie::detail::install_pool_test_hooks(nullptr); }
};
magpie::ThreadPoolOptions options(magpie::RejectionPolicy policy) {
    magpie::ThreadPoolOptions o;
    o.worker_count = 1;
    o.global_queue_capacity = 16;
    o.rejection = policy;
    return o;
}
struct Hole {
    Trace trace;
    std::latch claimed{1}, release{1}, worker_waiting{1}, reparked{1};
    std::atomic<bool> second_published{false}, repark_recorded{false};
    std::atomic<bool> waiting_recorded{false};
};
void hole_hook(Point p, std::size_t position, void* context) noexcept {
    auto& state = *static_cast<Hole*>(context);
    if (p == Point::MpmcEnqueueClaim && position == 0) {
        state.claimed.count_down();
        state.release.wait();
    }
    if (p == Point::MpmcPublished && position == 1) {
        state.second_published.store(true);
    }
    if (p == Point::WorkerBeforeWait && state.second_published.load() &&
        !state.repark_recorded.exchange(true)) {
        state.reparked.count_down();
    }
    if (p == Point::WorkerBeforeWait && !state.waiting_recorded.exchange(true)) {
        state.worker_waiting.count_down();
    }
}
} // namespace

TEST(ThreadPoolMpmc, ProducerHoleRecoveryNotifiesParkedWorker) {
    if (std::string(MAGPIE_BUILD_QUEUE_BACKEND) != "mpmc") {
        GTEST_SKIP();
    }
    Hole state;
    magpie::detail::PoolTestHooks hooks{hole_hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<Hole*>(ctx)->trace.record(s);
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { state.trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::Abort));
    state.worker_waiting.wait();
    std::atomic<unsigned> executed{0};
    std::thread first([&] { pool.submit([&] { executed.fetch_or(1); }); });
    state.claimed.wait();
    pool.submit([&] { executed.fetch_or(2); });
    state.reparked.wait();
    // Later publication alone cannot bypass the unpublished head.
    EXPECT_EQ(executed.load(), 0U);
    state.release.count_down();
    first.join();
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(executed.load(), 3U);
    EXPECT_EQ(pool.stats().pending, 0U);
}

TEST(ThreadPoolMpmc, DiscardDoesNotSkipUnpublishedHead) {
    if (std::string(MAGPIE_BUILD_QUEUE_BACKEND) != "mpmc") {
        GTEST_SKIP();
    }
    Hole state;
    magpie::detail::PoolTestHooks hooks{hole_hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<Hole*>(ctx)->trace.record(s);
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { state.trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::DiscardOldest));
    std::atomic<unsigned> completed{0};
    std::thread first([&] { pool.submit([&] { ++completed; }); });
    state.claimed.wait();
    for (unsigned i = 1; i < 16; ++i) {
        pool.submit([&] { ++completed; });
    }
    EXPECT_THROW(pool.submit([] {}), magpie::QueueFullError);
    EXPECT_EQ(pool.stats().discarded, 0U);
    state.release.count_down();
    first.join();
    pool.shutdown();
    pool.drain();
    EXPECT_EQ(completed.load(), 16U);
    EXPECT_EQ(pool.stats().submitted, 16U);
    EXPECT_EQ(pool.stats().pending, 0U);
}

namespace {
struct ReplaceOnDiscard {
    magpie::ThreadPool* pool;
    bool armed = true;
    ReplaceOnDiscard(magpie::ThreadPool* p) : pool(p) {}
    ReplaceOnDiscard(ReplaceOnDiscard&& other) noexcept : pool(other.pool), armed(other.armed) {
        other.armed = false;
    }
    ReplaceOnDiscard(const ReplaceOnDiscard&) = delete;
    void operator()() noexcept { armed = false; }
    ~ReplaceOnDiscard() noexcept {
        if (armed) {
            try {
                pool->submit([] {});
            } catch (...) {
                std::terminate();
            }
        }
    }
};
} // namespace
TEST(ThreadPoolMpmc, DiscardRetryExhaustionRollsBackNewTask) {
    if (std::string(MAGPIE_BUILD_QUEUE_BACKEND) != "mpmc") {
        GTEST_SKIP();
    }
    Trace trace;
    magpie::detail::PoolTestHooks hooks{nullptr, nullptr, &trace};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<Trace*>(ctx)->record(s);
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::DiscardOldest));
    std::latch running{1}, release{1};
    pool.submit([&] {
        running.count_down();
        release.wait();
    });
    running.wait();
    for (unsigned i = 0; i < 16; ++i) {
        pool.submit(ReplaceOnDiscard(&pool));
    }
    // Destruction runs outside the queue and fills each newly freed slot.
    EXPECT_THROW(pool.submit([] {}), magpie::QueueFullError);
    EXPECT_EQ(pool.stats().discarded, 2U);
    EXPECT_EQ(pool.stats().pending, 17U);
    release.count_down();
    pool.drain();
    pool.shutdown();
    const auto s = pool.stats();
    EXPECT_EQ(s.pending, 0U);
    EXPECT_EQ(s.submitted, s.completed + s.discarded);
    EXPECT_EQ(s.submitted, 19U);
}

TEST(ThreadPoolMpmc, DiscardUnderLoad) {
    Trace trace;
    magpie::detail::PoolTestHooks hooks{nullptr, nullptr, &trace};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<Trace*>(ctx)->record(s);
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::DiscardOldest));
    std::atomic<unsigned> invalid{0};
    constexpr unsigned count = 10000;
    auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
    struct Id {
        unsigned id;
        std::atomic<unsigned>* seen;
        std::atomic<unsigned>* invalid;
        void operator()() const noexcept {
            if (seen[id].fetch_or(1) != 0) {
                ++*invalid;
            }
        }
    };
    // Each capture retirement checks the execution set; every constructed
    // future is either fulfilled, broken, or rejected with a caller-side error.
    std::vector<std::future<unsigned>> futures;
    futures.reserve(count);
    unsigned refused = 0;
    for (unsigned id = 0; id < count; ++id) {
        try {
            futures.push_back(pool.submit_async([&, id] {
                Id{id, seen.get(), &invalid}();
                return id;
            }));
        } catch (const magpie::QueueFullError&) {
            ++refused;
        }
    }
    pool.shutdown();
    pool.drain();
    unsigned fulfilled = 0, broken = 0;
    for (auto& f : futures) {
        try {
            const auto id = f.get();
            EXPECT_EQ(seen[id].load(), 1U);
            ++fulfilled;
        } catch (const std::future_error& e) {
            EXPECT_EQ(e.code(), std::make_error_code(std::future_errc::broken_promise));
            ++broken;
        }
    }
    EXPECT_EQ(invalid.load(), 0U);
    EXPECT_EQ(fulfilled + broken + refused, count);
    const auto s = pool.stats();
    EXPECT_EQ(s.submitted, fulfilled + broken);
    EXPECT_EQ(s.discarded, broken);
    EXPECT_EQ(s.completed, fulfilled);
    EXPECT_EQ(s.pending, 0U);
}

namespace {
struct ConsumerHole {
    Trace trace;
    std::latch claimed{1}, release{1};
};
void consumer_hole_hook(Point point, std::size_t position, void* context) noexcept {
    if (point == Point::MpmcDequeueClaim && position == 1) {
        auto& hole = *static_cast<ConsumerHole*>(context);
        hole.claimed.count_down();
        hole.release.wait();
    }
}
} // namespace
TEST(ThreadPoolMpmc, ConsumerReservationMakesSlotUnavailable) {
    if (std::string(MAGPIE_BUILD_QUEUE_BACKEND) != "mpmc") {
        GTEST_SKIP();
    }
    ConsumerHole state;
    magpie::detail::PoolTestHooks hooks{consumer_hole_hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& s, void* ctx) noexcept {
        static_cast<ConsumerHole*>(ctx)->trace.record(s);
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { state.trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::Abort));
    std::latch running{1}, release_first{1};
    std::atomic<unsigned> executed{0};
    pool.submit([&] {
        running.count_down();
        release_first.wait();
        ++executed;
    });
    running.wait();
    for (unsigned i = 0; i < 16; ++i) {
        pool.submit([&] { ++executed; });
    }
    release_first.count_down();
    state.claimed.wait();
    EXPECT_THROW(pool.submit([] {}), magpie::QueueFullError);
    EXPECT_EQ(pool.stats().pending, 16U);
    state.release.count_down();
    pool.drain();
    pool.shutdown();
    EXPECT_EQ(executed.load(), 17U);
    EXPECT_EQ(pool.stats().pending, 0U);
}

namespace {
struct Rollback {
    Trace trace;
    std::latch rejected{1}, release_rejection{1}, retired{1}, drainer_observed{1};
    std::atomic<bool> paused{false}, retired_recorded{false}, predicate_recorded{false};
};
void rollback_hook(Point point, std::size_t, void* context) noexcept {
    auto& state = *static_cast<Rollback*>(context);
    if (point == Point::MpmcRejectBeforeRollback) {
        state.paused.store(true);
        state.rejected.count_down();
        state.release_rejection.wait();
    }
    if (point == Point::DrainPredicateFalse && !state.predicate_recorded.exchange(true)) {
        state.drainer_observed.count_down();
    }
}
} // namespace
TEST(ThreadPoolMpmc, RejectionRollbackZeroNotifiesWaitingDrainer) {
    if (std::string(MAGPIE_BUILD_QUEUE_BACKEND) != "mpmc") {
        GTEST_SKIP();
    }
    Rollback state;
    magpie::detail::PoolTestHooks hooks{rollback_hook, nullptr, &state};
    hooks.on_snapshot = [](const auto& snapshot, void* context) noexcept {
        auto& s = *static_cast<Rollback*>(context);
        s.trace.record(snapshot);
        if (s.paused.load() && snapshot.pending == 1 && snapshot.dequeue_position == 17 &&
            !s.retired_recorded.exchange(true)) {
            s.retired.count_down();
        }
    };
    Install install(hooks);
    magpie::test::Watchdog watchdog([&] { state.trace.dump(); });
    magpie::ThreadPool pool(options(magpie::RejectionPolicy::Abort));
    std::latch running{1}, release_first{1};
    pool.submit([&] {
        running.count_down();
        release_first.wait();
    });
    running.wait();
    for (unsigned i = 0; i < 16; ++i) {
        pool.submit([] {});
    }
    std::atomic<bool> refused{false}, drained{false};
    std::thread rejecting([&] {
        try {
            pool.submit([] {});
        } catch (const magpie::QueueFullError&) {
            refused.store(true);
        }
    });
    state.rejected.wait();
    release_first.count_down();
    state.retired.wait();
    EXPECT_EQ(pool.stats().pending, 1U);
    std::thread drainer([&] {
        pool.drain();
        drained.store(true);
    });
    state.drainer_observed.wait();
    state.release_rejection.count_down();
    rejecting.join();
    drainer.join();
    pool.shutdown();
    EXPECT_TRUE(refused.load());
    EXPECT_TRUE(drained.load());
    EXPECT_EQ(pool.stats().pending, 0U);
}
