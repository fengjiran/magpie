#include <magpie/thread_pool.hpp>

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#include <magpie/detail/test_hooks.hpp>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace {

magpie::ThreadPoolOptions one_worker_options(magpie::RejectionPolicy policy =
                                                magpie::RejectionPolicy::CallerRuns) {
    magpie::ThreadPoolOptions options;
    options.worker_count = 1;
    options.global_queue_capacity = 16;
    options.local_deque_capacity = 16;
    options.rejection = policy;
    return options;
}

void occupy_worker_and_fill_queue(magpie::ThreadPool& pool, std::latch& started,
                                  std::latch& release) {
    pool.submit([&started, &release] {
        started.count_down();
        release.wait();
    });
    started.wait();
    for (int i = 0; i < 16; ++i) {
        pool.submit([] {});
    }
}

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
struct HookInstall final {
    explicit HookInstall(const magpie::detail::PoolTestHooks& configured) {
        magpie::detail::install_pool_test_hooks(&configured);
    }
    ~HookInstall() { magpie::detail::install_pool_test_hooks(nullptr); }
    HookInstall(const HookInstall&) = delete;
    HookInstall& operator=(const HookInstall&) = delete;
};

struct GateRaceState {
    std::mutex mutex;
    std::condition_variable condition;
    bool submit_paused = false;
    bool release_submit = false;
    bool shutdown_stopped = false;
};

void gate_race_hook(magpie::detail::PoolTestHookPoint point, std::size_t,
                    void* opaque) noexcept {
    auto& state = *static_cast<GateRaceState*>(opaque);
    std::unique_lock<std::mutex> lock(state.mutex);
    if (point == magpie::detail::PoolTestHookPoint::SubmitAfterGate) {
        state.submit_paused = true;
        state.condition.notify_all();
        state.condition.wait(lock, [&state] { return state.release_submit; });
    } else if (point == magpie::detail::PoolTestHookPoint::ShutdownAfterStop) {
        state.shutdown_stopped = true;
        state.condition.notify_all();
    }
}

struct DrainRaceState {
    std::latch drain_predicate_seen{1};
    std::latch release_drain{1};
    std::latch pending_zero_seen{1};
    std::latch release_pending_zero{1};
};

void drain_race_hook(magpie::detail::PoolTestHookPoint point, std::size_t,
                     void* opaque) noexcept {
    auto& state = *static_cast<DrainRaceState*>(opaque);
    if (point == magpie::detail::PoolTestHookPoint::DrainPredicateFalse) {
        state.drain_predicate_seen.count_down();
        state.release_drain.wait();
    } else if (point == magpie::detail::PoolTestHookPoint::PendingZeroBeforeDrainLock) {
        state.pending_zero_seen.count_down();
        state.release_pending_zero.wait();
    }
}

struct ConstructorFailureState {
    std::mutex mutex;
    std::condition_variable condition;
    bool first_worker_idle = false;
    bool first_worker_exited = false;
};

void constructor_failure_hook(magpie::detail::PoolTestHookPoint point, std::size_t index,
                              void* opaque) noexcept {
    auto& state = *static_cast<ConstructorFailureState*>(opaque);
    std::lock_guard<std::mutex> lock(state.mutex);
    if (index == 0 && point == magpie::detail::PoolTestHookPoint::WorkerBeforeWait) {
        state.first_worker_idle = true;
        state.condition.notify_all();
    } else if (index == 0 && point == magpie::detail::PoolTestHookPoint::WorkerExit) {
        state.first_worker_exited = true;
        state.condition.notify_all();
    }
}

int fail_second_worker(std::size_t index, void* opaque) noexcept {
    if (index != 1) {
        return 0;
    }
    auto& state = *static_cast<ConstructorFailureState*>(opaque);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&state] { return state.first_worker_idle; });
    return EAGAIN;
}
#endif

struct ShutdownOnDestroy {
    ShutdownOnDestroy(magpie::ThreadPool* pool, std::atomic<bool>* observed) noexcept
        : pool(pool), observed(observed) {}
    ShutdownOnDestroy(ShutdownOnDestroy&& other) noexcept
        : pool(other.pool), observed(other.observed), armed(other.armed) {
        other.armed = false;
    }
    ShutdownOnDestroy(const ShutdownOnDestroy&) = delete;
    ShutdownOnDestroy& operator=(const ShutdownOnDestroy&) = delete;
    ~ShutdownOnDestroy() noexcept {
        if (armed) {
            try {
                pool->shutdown();
            } catch (const std::logic_error&) {
                observed->store(true, std::memory_order_relaxed);
            }
        }
    }
    void operator()() & noexcept {}

    magpie::ThreadPool* pool;
    std::atomic<bool>* observed;
    bool armed = true;
};

struct HandlerCaptureDestructor {
    HandlerCaptureDestructor(magpie::ThreadPool** pool,
                             std::atomic<bool>* armed,
                             std::atomic<bool>* guarded) noexcept
        : pool(pool), armed(armed), guarded(guarded) {}
    HandlerCaptureDestructor(const HandlerCaptureDestructor& other) noexcept
        : pool(other.pool), armed(other.armed), guarded(other.guarded), owns(true) {}
    HandlerCaptureDestructor(HandlerCaptureDestructor&& other) noexcept
        : pool(other.pool), armed(other.armed), guarded(other.guarded), owns(other.owns) {
        other.owns = false;
    }
    ~HandlerCaptureDestructor() noexcept {
        if (owns && armed->load(std::memory_order_relaxed)) {
            try {
                (*pool)->drain();
            } catch (const std::logic_error&) {
                guarded->store(true, std::memory_order_relaxed);
            }
            try {
                (void)(*pool)->stats();
                (void)(*pool)->worker_count();
                (*pool)->shutdown();
            } catch (const std::logic_error&) {
                guarded->store(true, std::memory_order_relaxed);
            }
        }
    }
    void operator()(std::exception_ptr) const noexcept {}

    magpie::ThreadPool** pool;
    std::atomic<bool>* armed;
    std::atomic<bool>* guarded;
    bool owns = false;
};

TEST(ThreadPool, SubmitAndDrain) {
    auto options = one_worker_options();
    magpie::ThreadPool pool(options);
    std::atomic<int> sum{0};
    for (int i = 0; i < 100; ++i) {
        pool.submit([&sum] { sum.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.drain();
    EXPECT_EQ(sum.load(std::memory_order_relaxed), 100);
    const auto stats = pool.stats();
    EXPECT_EQ(stats.pending, 0U);
    EXPECT_GT(stats.wakes, 0U);
    EXPECT_FALSE(stats.wake_threads.has_value());
    EXPECT_EQ(stats.submitted, stats.completed + stats.discarded);
    EXPECT_EQ(stats.completed, stats.worker_completed + stats.inline_completed);
}

TEST(ThreadPool, StatsQuiescentIdentity) {
    auto options = one_worker_options();
    options.global_queue_capacity = 128;
    magpie::ThreadPool pool(options);
    for (int i = 0; i < 64; ++i) {
        pool.submit([] {});
    }
    pool.drain();
    const auto stats = pool.stats();
    EXPECT_EQ(stats.submitted, 64U);
    EXPECT_EQ(stats.submitted, stats.completed + stats.discarded);
    EXPECT_EQ(stats.completed, stats.worker_completed + stats.inline_completed);
    EXPECT_EQ(stats.pending, 0U);
    EXPECT_EQ(stats.rejected, 0U);
}

TEST(ThreadPool, FutureException) {
    auto options = one_worker_options();
    std::atomic<int> handled{0};
    options.exception_handler = [&handled](std::exception_ptr) {
        handled.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("handler exception");
    };
    magpie::ThreadPool pool(options);

    auto future = pool.submit_async([]() -> int { throw std::runtime_error("future error"); });
    EXPECT_THROW(future.get(), std::runtime_error);
    pool.submit([] { throw std::runtime_error("bare task error"); });
    pool.drain();
    EXPECT_EQ(handled.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(pool.stats().completed, 2U);
}

TEST(ThreadPool, ExceptionHandlerCannotSynchronouslyControlItsPool) {
    magpie::ThreadPool* pool_ptr = nullptr;
    std::atomic<bool> guarded{false};
    auto options = one_worker_options();
    options.exception_handler = [&pool_ptr, &guarded](std::exception_ptr) {
        try {
            pool_ptr->shutdown();
        } catch (const std::logic_error&) {
            guarded.store(true, std::memory_order_relaxed);
        }
    };
    magpie::ThreadPool pool(options);
    pool_ptr = &pool;
    pool.submit([] { throw std::runtime_error("task error"); });
    pool.drain();
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
}

TEST(ThreadPool, PoolOwnedHandlerCaptureDestructorHasExecutionGuard) {
    alignas(magpie::ThreadPool) std::byte storage[sizeof(magpie::ThreadPool)];
    auto* const pool_address = reinterpret_cast<magpie::ThreadPool*>(storage);
    magpie::ThreadPool* pool_ptr = pool_address;
    std::atomic<bool> armed{false};
    std::atomic<bool> guarded{false};
    magpie::ThreadPoolOptions options = one_worker_options();
    options.exception_handler = HandlerCaptureDestructor(&pool_ptr, &armed, &guarded);
    auto* pool = ::new (static_cast<void*>(storage)) magpie::ThreadPool(options);
    options.exception_handler = nullptr;
    guarded.store(false, std::memory_order_relaxed);
    armed.store(true, std::memory_order_relaxed);
    pool->shutdown();
    pool->drain();
    pool->~ThreadPool();
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
}

TEST(ThreadPool, RejectionPoliciesCallerRuns) {
    auto options = one_worker_options();
    magpie::ThreadPool pool(options);
    std::latch started(1);
    std::latch release(1);
    occupy_worker_and_fill_queue(pool, started, release);

    auto result = pool.submit_async([] { return 23; });
    EXPECT_EQ(result.get(), 23);
    EXPECT_EQ(pool.stats().rejected, 1U);
    EXPECT_EQ(pool.stats().inline_completed, 1U);

    release.count_down();
    pool.drain();
}

TEST(ThreadPool, RejectionRollbackNotifies) {
    auto options = one_worker_options(magpie::RejectionPolicy::Abort);
    magpie::ThreadPool pool(options);
    std::latch started(1);
    std::latch release(1);
    occupy_worker_and_fill_queue(pool, started, release);

    std::atomic<bool> destructor_guarded{false};
    try {
        pool.submit(ShutdownOnDestroy(&pool, &destructor_guarded));
        FAIL() << "full queue submission should throw";
    } catch (const magpie::QueueFullError& error) {
        EXPECT_EQ(error.reason, magpie::QueueFullReason::QueueFull);
    }
    EXPECT_TRUE(destructor_guarded.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.stats().pending, 17U);

    release.count_down();
    pool.drain();
    const auto stats = pool.stats();
    EXPECT_EQ(stats.pending, 0U);
    EXPECT_EQ(stats.submitted, stats.completed + stats.discarded);
}

TEST(ThreadPool, DiscardedFutureBrokenPromise) {
    auto options = one_worker_options(magpie::RejectionPolicy::DiscardOldest);
    magpie::ThreadPool pool(options);
    std::latch started(1);
    std::latch release(1);
    pool.submit([&started, &release] {
        started.count_down();
        release.wait();
    });
    started.wait();

    auto discarded_future = pool.submit_async([] { return 1; });
    for (int i = 1; i < 16; ++i) {
        pool.submit([] {});
    }
    auto retained_future = pool.submit_async([] { return 2; });

    try {
        (void)discarded_future.get();
        FAIL() << "discarded packaged task must break its promise";
    } catch (const std::future_error& error) {
        EXPECT_EQ(error.code(), std::make_error_code(std::future_errc::broken_promise));
    }
    EXPECT_EQ(pool.stats().rejected, 1U);
    EXPECT_EQ(pool.stats().discarded, 1U);

    release.count_down();
    pool.drain();
    EXPECT_EQ(retained_future.get(), 2);
    const auto stats = pool.stats();
    EXPECT_EQ(stats.submitted, stats.completed + stats.discarded);
    EXPECT_EQ(stats.pending, 0U);
}

TEST(ThreadPool, SubmitAfterShutdownReportsReason) {
    magpie::ThreadPool pool(one_worker_options());
    pool.shutdown();
    try {
        pool.submit([] {});
        FAIL() << "submit after shutdown should throw";
    } catch (const magpie::QueueFullError& error) {
        EXPECT_EQ(error.reason, magpie::QueueFullReason::ShuttingDown);
    }
}

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
TEST(ThreadPool, ShutdownGateRace) {
    GateRaceState state;
    const magpie::detail::PoolTestHooks hooks{gate_race_hook, nullptr, &state};
    HookInstall installed(hooks);
    magpie::ThreadPool pool(one_worker_options());
    std::atomic<int> ran{0};

    std::thread submitter([&] { pool.submit([&ran] { ran.fetch_add(1); }); });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait(lock, [&state] { return state.submit_paused; });
    }
    std::atomic<bool> shutdown_done{false};
    std::thread stopper([&] {
        pool.shutdown();
        shutdown_done.store(true, std::memory_order_release);
    });
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.condition.wait(lock, [&state] { return state.shutdown_stopped; });
        EXPECT_FALSE(shutdown_done.load(std::memory_order_acquire));
        state.release_submit = true;
        state.condition.notify_all();
    }
    submitter.join();
    stopper.join();
    pool.drain();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_TRUE(shutdown_done.load(std::memory_order_acquire));
}

TEST(ThreadPool, DrainNotifyRace) {
    DrainRaceState state;
    const magpie::detail::PoolTestHooks hooks{drain_race_hook, nullptr, &state};
    HookInstall installed(hooks);
    magpie::ThreadPool pool(one_worker_options());
    std::latch task_started(1);
    std::latch release_task(1);
    pool.submit([&] {
        task_started.count_down();
        release_task.wait();
    });
    task_started.wait();

    std::atomic<bool> drain_done{false};
    std::thread drainer([&] {
        pool.drain();
        drain_done.store(true, std::memory_order_release);
    });
    state.drain_predicate_seen.wait();
    release_task.count_down();
    state.pending_zero_seen.wait();
    state.release_drain.count_down();
    state.release_pending_zero.count_down();
    drainer.join();
    EXPECT_TRUE(drain_done.load(std::memory_order_acquire));
    EXPECT_EQ(pool.stats().pending, 0U);
}
#endif

TEST(ThreadPool, CallerRunsControlGuard) {
    magpie::ThreadPool pool(one_worker_options());
    std::latch started(1);
    std::latch release(1);
    occupy_worker_and_fill_queue(pool, started, release);
    std::atomic<bool> guarded{false};

    pool.submit([&] {
        try {
            pool.shutdown();
        } catch (const std::logic_error&) {
            guarded.store(true, std::memory_order_relaxed);
        }
    });
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
    release.count_down();
    pool.drain();
}

TEST(ThreadPool, WorkerControlGuard) {
    magpie::ThreadPool pool(one_worker_options());
    std::atomic<bool> guarded{false};
    pool.submit([&] {
        try {
            pool.shutdown();
        } catch (const std::logic_error&) {
            guarded.store(true, std::memory_order_relaxed);
        }
    });
    pool.drain();
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
    pool.shutdown();
}

TEST(ThreadPool, WorkerSubmitsChild) {
    magpie::ThreadPool pool(one_worker_options());
    std::atomic<int> result{0};
    pool.submit([&] { pool.submit([&] { result.store(7, std::memory_order_relaxed); }); });
    pool.drain();
    EXPECT_EQ(result.load(std::memory_order_relaxed), 7);
}

TEST(ThreadPool, WorkerFullQueueFallsBackToCallerRunsUnderAbortPolicy) {
    auto options = one_worker_options(magpie::RejectionPolicy::Abort);
    magpie::ThreadPool pool(options);
    std::latch worker_entered(1);
    std::latch worker_may_submit(1);
    std::atomic<bool> child_ran{false};
    pool.submit([&] {
        worker_entered.count_down();
        worker_may_submit.wait();
        pool.submit([&] { child_ran.store(true, std::memory_order_relaxed); });
    });
    worker_entered.wait();
    for (int i = 0; i < 16; ++i) {
        pool.submit([] {});
    }
    worker_may_submit.count_down();
    pool.drain();
    EXPECT_TRUE(child_ran.load(std::memory_order_relaxed));
    EXPECT_EQ(pool.stats().rejected, 1U);
    EXPECT_EQ(pool.stats().inline_completed, 1U);
}

TEST(ThreadPool, ExecutedCaptureDestructorRunsInsideExecutionFrame) {
    magpie::ThreadPool pool(one_worker_options());
    std::atomic<bool> guarded{false};
    pool.submit([guard = ShutdownOnDestroy(&pool, &guarded)]() mutable { (void)guard; });
    pool.drain();
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
}

TEST(ThreadPool, CapturedDestructorControlGuard) {
    auto options = one_worker_options(magpie::RejectionPolicy::DiscardOldest);
    magpie::ThreadPool pool(options);
    std::latch started(1);
    std::latch release(1);
    pool.submit([&started, &release] {
        started.count_down();
        release.wait();
    });
    started.wait();
    std::atomic<bool> guarded{false};
    pool.submit([guard = ShutdownOnDestroy(&pool, &guarded)]() mutable { (void)guard; });
    for (int i = 1; i < 16; ++i) {
        pool.submit([] {});
    }
    pool.submit([] {});
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
    release.count_down();
    pool.drain();
}

TEST(ThreadPool, NestedExecutionControlGuard) {
    magpie::ThreadPool outer(one_worker_options());
    magpie::ThreadPool inner(one_worker_options());
    std::latch inner_started(1);
    std::latch release_inner(1);
    inner.submit([&inner_started, &release_inner] {
        inner_started.count_down();
        release_inner.wait();
    });
    inner_started.wait();
    for (int i = 0; i < 16; ++i) {
        inner.submit([] {});
    }

    std::atomic<bool> guarded{false};
    outer.submit([&] {
        inner.submit([&] {
            try {
                outer.shutdown();
            } catch (const std::logic_error&) {
                guarded.store(true, std::memory_order_relaxed);
            }
        });
    });
    outer.drain();
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
    release_inner.count_down();
    inner.drain();
}

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
TEST(ThreadPool, ConstructorFailureRollback) {
    ConstructorFailureState state;
    const magpie::detail::PoolTestHooks hooks{constructor_failure_hook,
                                              fail_second_worker, &state};
    HookInstall installed(hooks);
    auto options = one_worker_options();
    options.worker_count = 2;

    EXPECT_THROW(magpie::ThreadPool pool(options), std::system_error);
    std::lock_guard<std::mutex> lock(state.mutex);
    EXPECT_TRUE(state.first_worker_idle);
    EXPECT_TRUE(state.first_worker_exited);
}

TEST(ThreadPool, ConstructorFailureDestroysPoolOwnedHandlerUnderGuard) {
    ConstructorFailureState state;
    const magpie::detail::PoolTestHooks hooks{constructor_failure_hook,
                                              fail_second_worker, &state};
    HookInstall installed(hooks);
    alignas(magpie::ThreadPool) std::byte storage[sizeof(magpie::ThreadPool)];
    auto* const pool_address = reinterpret_cast<magpie::ThreadPool*>(storage);
    magpie::ThreadPool* pool_ptr = pool_address;
    std::atomic<bool> guarded{false};
    auto options = one_worker_options();
    options.worker_count = 2;
    std::atomic<bool> armed{false};
    options.exception_handler = HandlerCaptureDestructor(&pool_ptr, &armed, &guarded);
    armed.store(true, std::memory_order_relaxed);

    EXPECT_THROW(::new (static_cast<void*>(storage)) magpie::ThreadPool(options),
                 std::system_error);
    armed.store(false, std::memory_order_relaxed);
    options.exception_handler = nullptr;
    EXPECT_TRUE(guarded.load(std::memory_order_relaxed));
    EXPECT_TRUE(state.first_worker_exited);
}
#endif

TEST(ThreadPool, OptionsValidation) {
    auto options = one_worker_options();
    options.global_queue_capacity = 15;
    EXPECT_THROW(magpie::ThreadPool pool(options), std::invalid_argument);

    options = one_worker_options();
    options.local_deque_capacity = 0;
    EXPECT_THROW(magpie::ThreadPool pool(options), std::invalid_argument);

    options = one_worker_options();
    options.global_queue_capacity = std::numeric_limits<std::size_t>::max();
    EXPECT_THROW(magpie::ThreadPool pool(options), std::length_error);

    options = one_worker_options();
    options.rejection = static_cast<magpie::RejectionPolicy>(255);
    EXPECT_THROW(magpie::ThreadPool pool(options), std::invalid_argument);

    options = one_worker_options();
    options.worker_count = 0;
    magpie::ThreadPool pool(options);
    EXPECT_GE(pool.worker_count(), 1U);
}

}  // namespace
