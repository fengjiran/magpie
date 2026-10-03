#include "magpie/thread_pool.hpp"

#include "worker_thread.hpp"

#if defined(MAGPIE_USE_MPMC)
#include "magpie/mpmc_queue.hpp"
#endif

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#include "magpie/detail/test_hooks.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace magpie {
namespace {

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
enum class HookPoint {
    SubmitAfterGate,
    DrainPredicateFalse,
    PendingZeroBeforeDrainLock,
    WorkerBeforeWait,
    WorkerExit,
    BeforeWorkerCreate,
    ShutdownAfterStop,
    MpmcEnqueueClaim,
    MpmcDequeueClaim,
    MpmcPublished,
    MpmcRejectBeforeRollback,
};
#endif

struct ExecutionFrame {
    const ThreadPool* pool;
    ExecutionFrame* previous;
};

thread_local ExecutionFrame* current_execution_frame = nullptr;

struct OwnerFrame {
    const ThreadPool* pool;
    void* worker;
    OwnerFrame* previous;
};

thread_local OwnerFrame* current_owner_frame = nullptr;

class ExecutionScope final {
public:
    explicit ExecutionScope(const ThreadPool* pool) noexcept
        : frame_{pool, current_execution_frame} {
        current_execution_frame = &frame_;
    }

    ~ExecutionScope() noexcept { current_execution_frame = frame_.previous; }

    ExecutionScope(const ExecutionScope&) = delete;
    ExecutionScope& operator=(const ExecutionScope&) = delete;

private:
    ExecutionFrame frame_;
};

class OwnerScope final {
public:
    OwnerScope(const ThreadPool* pool, void* worker) noexcept
        : frame_{pool, worker, current_owner_frame} {
        current_owner_frame = &frame_;
    }

    ~OwnerScope() noexcept { current_owner_frame = frame_.previous; }

    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    OwnerFrame frame_;
};

[[nodiscard]] bool executing_pool(const ThreadPool* pool) noexcept {
    for (auto* frame = current_execution_frame; frame != nullptr; frame = frame->previous) {
        if (frame->pool == pool) {
            return true;
        }
    }
    return false;
}

void require_external_control(const ThreadPool* pool) {
    if (executing_pool(pool)) {
        throw std::logic_error("synchronous pool control inside its execution context");
    }
}

[[nodiscard]] std::size_t normalize_capacity(std::size_t requested,
                                             std::size_t slot_bytes) {
    constexpr std::size_t minimum = 16;
    if (requested < minimum || slot_bytes == 0) {
        throw std::invalid_argument("queue capacity must be at least 16");
    }
    const auto max_capacity = std::min(
        std::numeric_limits<std::size_t>::max() / slot_bytes,
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    std::size_t capacity = 1;
    while (capacity < requested) {
        if (capacity > max_capacity / 2) {
            throw std::length_error("queue capacity overflow");
        }
        capacity *= 2;
    }
    if (capacity > max_capacity) {
        throw std::length_error("queue allocation size overflow");
    }
    return capacity;
}

[[nodiscard]] std::size_t effective_worker_count(std::size_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    const auto hardware_hint = std::thread::hardware_concurrency();
    return hardware_hint == 0 ? 1 : static_cast<std::size_t>(hardware_hint);
}

void increment_saturated(std::atomic<std::uint64_t>& counter) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max() &&
           !counter.compare_exchange_weak(current, current + 1,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

void report_unhandled_exception(const char* context) noexcept {
    std::fprintf(stderr, "magpie: exception in %s\n", context);
}

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
std::atomic<const detail::PoolTestHooks*> test_hooks{nullptr};

void invoke_test_hook(HookPoint point, std::size_t index = 0) noexcept {
    const auto* hooks = test_hooks.load(std::memory_order_acquire);
    if (hooks != nullptr && hooks->on_point != nullptr) {
        hooks->on_point(static_cast<detail::PoolTestHookPoint>(point), index, hooks->context);
    }
}

[[nodiscard]] int test_worker_create_error(std::size_t index) noexcept {
    const auto* hooks = test_hooks.load(std::memory_order_acquire);
    if (hooks != nullptr && hooks->before_worker_create != nullptr) {
        return hooks->before_worker_create(index, hooks->context);
    }
    return 0;
}
#define MAGPIE_TEST_HOOK(point, index) test_hook(HookPoint::point, index)
#define MAGPIE_TEST_WORKER_CREATE_ERROR(index) test_worker_create_error(index)
#else
#define MAGPIE_TEST_HOOK(point, index) ((void)0)
#define MAGPIE_TEST_WORKER_CREATE_ERROR(index) 0
#endif

}  // namespace

struct ThreadPool::Impl {
    ThreadPool* owner;
    struct TaskOwner;

    struct WorkerCtx {
        WorkerCtx(std::size_t worker_index, Impl* implementation)
            : index(worker_index), owner(implementation) {}

        std::size_t index;
        Impl* owner;
        detail::WorkerThread thread;
    };

    struct HandlerHolder {
        HandlerHolder(ThreadPool* pool, const std::function<void(std::exception_ptr)>& source)
            : owner(pool) {
            ExecutionScope scope(owner);
            callback = source;
        }

        ~HandlerHolder() noexcept {
            ExecutionScope scope(owner);
            callback = nullptr;
        }

        ThreadPool* owner;
        std::function<void(std::exception_ptr)> callback;
    };

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    void test_hook(HookPoint point, std::size_t index) noexcept {
        const auto* hooks = test_hooks.load(std::memory_order_acquire);
        if (hooks && hooks->on_snapshot) {
            std::size_t enq = 0, deq = 0;
#if defined(MAGPIE_USE_MPMC)
            const auto positions = global_queue.positions_for_test();
            enq = positions[0]; deq = positions[1];
#endif
            hooks->on_snapshot({stopping.load(), in_flight_submitters.load(), pending.load(), enq, deq}, hooks->context);
        }
        invoke_test_hook(point, index);
    }
#endif

    explicit Impl(ThreadPool* pool_owner, const ThreadPoolOptions& source)
        : owner(pool_owner), options(copy_and_validate_options(source)),
          queue_capacity(normalize_capacity(options.global_queue_capacity, global_slot_size)),
          local_capacity(normalize_capacity(options.local_deque_capacity, sizeof(Task*))),
          worker_total(effective_worker_count(options.worker_count)),
#if defined(MAGPIE_USE_MPMC)
          global_queue(queue_capacity
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                       , &queue_hooks
#endif
                       ),
#else
          queue_slots(queue_capacity),
#endif
          handler(pool_owner, source.exception_handler) {

        const auto worker_limit = std::vector<std::unique_ptr<WorkerCtx>>{}.max_size();
        if (worker_total > worker_limit ||
            worker_total > std::numeric_limits<std::size_t>::max() / sizeof(WorkerCtx)) {
            throw std::length_error("worker count exceeds platform allocation limits");
        }

        workers.reserve(worker_total);
        for (std::size_t i = 0; i < worker_total; ++i) {
            workers.push_back(std::make_unique<WorkerCtx>(i, this));
        }

        try {
            for (std::size_t i = 0; i < worker_total; ++i) {
                MAGPIE_TEST_HOOK(BeforeWorkerCreate, i);
                const int injected_error = MAGPIE_TEST_WORKER_CREATE_ERROR(i);
                if (injected_error != 0) {
                    throw std::system_error(injected_error, std::generic_category(),
                                            "injected worker creation failure");
                }
                workers[i]->thread.start(&Impl::worker_entry, workers[i].get(),
                                         options.worker_stack_size, options.pin_to_cores, i);
            }
        } catch (...) {
            const auto original_error = std::current_exception();
            publish_worker_stop();
            join_started_workers();
            std::rethrow_exception(original_error);
        }
    }

    ~Impl() noexcept = default;

    [[nodiscard]] static ThreadPoolOptions copy_and_validate_options(
        const ThreadPoolOptions& source) {
        switch (source.rejection) {
            case RejectionPolicy::CallerRuns:
            case RejectionPolicy::Abort:
            case RejectionPolicy::DiscardOldest:
                break;
            default:
                throw std::invalid_argument("unknown rejection policy");
        }
        ThreadPoolOptions result;
        result.worker_count = source.worker_count;
        result.global_queue_capacity = source.global_queue_capacity;
        result.local_deque_capacity = source.local_deque_capacity;
        result.rejection = source.rejection;
        result.pin_to_cores = source.pin_to_cores;
        result.worker_stack_size = source.worker_stack_size;
        return result;
    }

    [[nodiscard]] bool is_worker_thread() const noexcept {
        return current_owner_frame != nullptr && current_owner_frame->pool == owner;
    }

    void destroy_task(Task* task) noexcept {
        if (task == nullptr) {
            return;
        }
        ExecutionScope scope(owner);
        delete task;
    }

    void run_one(Task* task, bool worker_execution) noexcept {
        {
            ExecutionScope scope(owner);
            try {
                task->run();
            } catch (...) {
                const auto error = std::current_exception();
                if (handler.callback) {
                    try {
                        handler.callback(error);
                    } catch (...) {
                        report_unhandled_exception("exception handler");
                    }
                } else {
                    report_unhandled_exception("task");
                }
            }
            delete task;
        }

        increment_saturated(completed);
        if (worker_execution) {
            increment_saturated(worker_completed);
        } else {
            increment_saturated(inline_completed);
        }
        dec_pending();
    }

    void add_pending() noexcept {
        auto current = pending.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max()) {
                std::fprintf(stderr, "magpie: pending count overflow\n");
                std::terminate();
            }
            if (pending.compare_exchange_weak(current, current + 1,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void dec_pending() noexcept {
        const auto old = pending.fetch_sub(1, std::memory_order_acq_rel);
        if (old == 0) {
            std::fprintf(stderr, "magpie: pending count underflow\n");
            std::terminate();
        }
        if (old != 1) {
            return;
        }

        MAGPIE_TEST_HOOK(PendingZeroBeforeDrainLock, 0);
        {
            std::lock_guard<std::mutex> lock(drain_mutex);
            drain_cv.notify_all();
        }
        if (stopping.load(std::memory_order_seq_cst)) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            notify_queue_all();
        }
    }

    void submit(Task* task) {
        TaskOwner task_owner{this, task};
        GateGuard gate{this};

        if (stopping.load(std::memory_order_seq_cst)) {
            increment_saturated(rejected);
            task_owner.destroy();
            throw QueueFullError(QueueFullReason::ShuttingDown);
        }
        MAGPIE_TEST_HOOK(SubmitAfterGate, 0);

        add_pending();
#if defined(MAGPIE_USE_MPMC)
        submit_mpmc(task_owner);
#else
        bool pending_reserved = true;
        Task* discarded_task = nullptr;
        bool queued = false;
        try {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue_size < queue_capacity) {
                enqueue_locked(task);
                queued = true;
            } else if (!is_worker_thread() &&
                       options.rejection == RejectionPolicy::DiscardOldest) {
                discarded_task = dequeue_locked();
                enqueue_locked(task);
                queued = true;
            }
            if (queued) {
                // Relinquish ownership before unlocking makes the published pointer visible.
                (void)task_owner.release();
            }
        } catch (...) {
            task_owner.destroy();
            dec_pending();
            pending_reserved = false;
            throw;
        }

        if (queued) {
            pending_reserved = false;
            increment_saturated(submitted);
            if (discarded_task != nullptr) {
                increment_saturated(rejected);
                increment_saturated(discarded);
            }
            notify_queue_one();
            if (discarded_task != nullptr) {
                destroy_task(discarded_task);
                dec_pending();
            }
            return;
        }

        increment_saturated(rejected);
        const bool must_run_inline = is_worker_thread() ||
                                     options.rejection == RejectionPolicy::CallerRuns;
        if (must_run_inline) {
            (void)task_owner.release();
            pending_reserved = false;
            increment_saturated(submitted);
            run_one(task, false);
            return;
        }

        task_owner.destroy();
        if (pending_reserved) {
            dec_pending();
            pending_reserved = false;
        }
        throw QueueFullError(QueueFullReason::QueueFull);
#endif
    }

#if defined(MAGPIE_USE_MPMC)
    bool publish_mpmc(TaskOwner& task_owner) noexcept {
        if (!global_queue.enqueue(task_owner.task)) { return false; }
        // The consumer may already have deleted Task. Only overwrite the owner
        // field; do not read/return the published pointer after successful try.
        task_owner.forget();
        increment_saturated(submitted);
        // The last probe and CV wait hold this same mutex. Publication precedes
        // acquiring it, so a reservation-hole recovery cannot lose its wake.
        std::lock_guard<std::mutex> lock(queue_mutex);
        notify_queue_one();
        return true;
    }

    void submit_mpmc(TaskOwner& task_owner) {
        if (publish_mpmc(task_owner)) { return; }
        increment_saturated(rejected);
        if (is_worker_thread() || options.rejection == RejectionPolicy::CallerRuns) {
            auto* task = task_owner.release();
            increment_saturated(submitted);
            run_one(task, false);
            return;
        }
        if (options.rejection == RejectionPolicy::DiscardOldest) {
            for (std::size_t retry = 0; retry < discard_retry_limit; ++retry) {
                Task* dropped = nullptr;
                if (!global_queue.discard_oldest(dropped)) { break; }
                increment_saturated(discarded);
                destroy_task(dropped);
                dec_pending();
                if (publish_mpmc(task_owner)) { return; }
            }
        }
        MAGPIE_TEST_HOOK(MpmcRejectBeforeRollback, 0);
        task_owner.destroy();
        dec_pending();
        throw QueueFullError(QueueFullReason::QueueFull);
    }
#endif

    void enter_gate() {
        auto current = in_flight_submitters.load(std::memory_order_seq_cst);
        for (;;) {
            if (current == std::numeric_limits<std::uint64_t>::max()) {
                std::fprintf(stderr, "magpie: submission gate overflow\n");
                std::terminate();
            }
            if (in_flight_submitters.compare_exchange_weak(
                    current, current + 1, std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                return;
            }
        }
    }

    void leave_gate() noexcept {
        const auto old = in_flight_submitters.fetch_sub(1, std::memory_order_seq_cst);
        if (old == 0) {
            std::fprintf(stderr, "magpie: submission gate underflow\n");
            std::terminate();
        }
        if (old == 1) {
            if (stopping.load(std::memory_order_seq_cst)) {
                std::lock_guard<std::mutex> lock(queue_mutex);
                notify_queue_all();
            }
        }
    }

    struct GateGuard {
        explicit GateGuard(Impl* implementation) : impl(implementation) { impl->enter_gate(); }
        ~GateGuard() noexcept { impl->leave_gate(); }
        GateGuard(const GateGuard&) = delete;
        GateGuard& operator=(const GateGuard&) = delete;
        Impl* impl;
    };

    struct TaskOwner {
        Impl* impl;
        Task* task;

        ~TaskOwner() noexcept { destroy(); }
        void destroy() noexcept {
            if (task != nullptr) {
                impl->destroy_task(task);
                task = nullptr;
            }
        }
        void forget() noexcept { task = nullptr; }
        [[nodiscard]] Task* release() noexcept {
            auto* result = task;
            task = nullptr;
            return result;
        }
    };

    void shutdown() {
        require_external_control(owner);
        publish_worker_stop();
        MAGPIE_TEST_HOOK(ShutdownAfterStop, 0);
        while (in_flight_submitters.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
    }

    void publish_worker_stop() noexcept {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stopping.store(true, std::memory_order_seq_cst);
        notify_queue_all();
    }

    void drain() {
        require_external_control(owner);
        std::unique_lock<std::mutex> lock(drain_mutex);
        while (pending.load(std::memory_order_acquire) != 0) {
            MAGPIE_TEST_HOOK(DrainPredicateFalse, 0);
            drain_cv.wait(lock);
        }
    }

    void join_started_workers() noexcept {
        for (auto& worker : workers) {
            worker->thread.join();
        }
    }

    static void worker_entry(void* argument) noexcept {
        auto* context = static_cast<WorkerCtx*>(argument);
        auto* implementation = context->owner;
        try {
            implementation->worker_loop(*context);
        } catch (...) {
            report_unhandled_exception("worker loop");
            std::terminate();
        }
    }

    void worker_loop(WorkerCtx& context) {
        OwnerScope owner_scope(owner, &context);
        for (;;) {
            Task* task = nullptr;
#if defined(MAGPIE_USE_MPMC)
            if (!global_queue.dequeue(task)) {
                std::unique_lock<std::mutex> lock(queue_mutex);
                for (;;) {
                    if (global_queue.dequeue(task)) { break; }
                    if (can_exit()) { break; }
                    MAGPIE_TEST_HOOK(WorkerBeforeWait, context.index);
                    queue_cv.wait(lock);
                }
                if (task == nullptr) { break; }
            }
#else
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                while (queue_size == 0 && !can_exit()) {
                    MAGPIE_TEST_HOOK(WorkerBeforeWait, context.index);
                    queue_cv.wait(lock);
                }
                if (queue_size == 0 && can_exit()) { break; }
                task = dequeue_locked();
            }
#endif
            // All queue operations and the parking mutex are finished first.
            run_one(task, true);
        }
        MAGPIE_TEST_HOOK(WorkerExit, context.index);
    }

    bool can_exit() const noexcept {
        return stopping.load(std::memory_order_seq_cst) &&
            in_flight_submitters.load(std::memory_order_seq_cst) == 0 &&
            pending.load(std::memory_order_acquire) == 0;
    }

#if !defined(MAGPIE_USE_MPMC)
    void enqueue_locked(Task* task) noexcept {
        queue_slots[queue_tail] = task;
        queue_tail = (queue_tail + 1) % queue_capacity;
        ++queue_size;
    }

#endif

    void notify_queue_one() noexcept {
        queue_cv.notify_one();
        increment_saturated(wakes);
    }

    void notify_queue_all() noexcept {
        queue_cv.notify_all();
        increment_saturated(wakes);
    }

#if !defined(MAGPIE_USE_MPMC)
    [[nodiscard]] Task* dequeue_locked() noexcept {
        auto* task = queue_slots[queue_head];
        queue_slots[queue_head] = nullptr;
        queue_head = (queue_head + 1) % queue_capacity;
        --queue_size;
        return task;
    }

#endif

    ThreadPoolOptions options;
    const std::size_t queue_capacity;
    const std::size_t local_capacity;
    const std::size_t worker_total;
#if defined(MAGPIE_USE_MPMC)
    static constexpr auto global_slot_size = MPMCQueue<Task*>::slot_size;
    static constexpr std::size_t discard_retry_limit = 2;
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    detail::MpmcTestHooks queue_hooks{
        [](std::size_t position, void* context) noexcept { static_cast<Impl*>(context)->test_hook(HookPoint::MpmcEnqueueClaim, position); },
        [](std::size_t position, void* context) noexcept { static_cast<Impl*>(context)->test_hook(HookPoint::MpmcDequeueClaim, position); },
        this,
        [](std::size_t position, void* context) noexcept { static_cast<Impl*>(context)->test_hook(HookPoint::MpmcPublished, position); }};
#endif
    MPMCQueue<Task*> global_queue;
#else
    static constexpr auto global_slot_size = sizeof(Task*);
    std::vector<Task*> queue_slots;
#endif
    std::vector<std::unique_ptr<WorkerCtx>> workers;
#if !defined(MAGPIE_USE_MPMC)
    std::size_t queue_head = 0;
    std::size_t queue_tail = 0;
    std::size_t queue_size = 0;
#endif
    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> in_flight_submitters{0};
    std::atomic<std::uint64_t> pending{0};
    std::mutex drain_mutex;
    std::condition_variable drain_cv;
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> discarded{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> worker_completed{0};
    std::atomic<std::uint64_t> inline_completed{0};
    std::atomic<std::uint64_t> wakes{0};
    HandlerHolder handler;
};

QueueFullError::QueueFullError(QueueFullReason error_reason)
    : std::runtime_error(error_reason == QueueFullReason::QueueFull
                             ? "queue unavailable"
                             : "thread pool is shutting down"),
      reason(error_reason) {}

ThreadPool::ThreadPool(const ThreadPoolOptions& options)
    : impl_(nullptr) {
    impl_ = std::make_unique<Impl>(this, options);
}

ThreadPool::~ThreadPool() noexcept {
    if (executing_pool(this)) {
        std::terminate();
    }
    if (impl_ != nullptr) {
        try {
            impl_->shutdown();
            impl_->drain();
        } catch (...) {
            std::fprintf(stderr, "magpie: shutdown failed in destructor\n");
            std::terminate();
        }
        impl_->join_started_workers();
    }
}

void ThreadPool::submit_task(Task* task) {
    if (impl_ == nullptr) {
        ExecutionScope scope(this);
        delete task;
        throw std::logic_error("thread pool is not initialized");
    }
    impl_->submit(task);
}

void ThreadPool::shutdown() {
    require_external_control(this);
    if (impl_ == nullptr) {
        throw std::logic_error("thread pool is not initialized");
    }
    impl_->shutdown();
}

void ThreadPool::drain() {
    require_external_control(this);
    if (impl_ == nullptr) {
        throw std::logic_error("thread pool is not initialized");
    }
    impl_->drain();
}

std::size_t ThreadPool::worker_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->worker_total;
}

ThreadPool::Stats ThreadPool::stats() const noexcept {
    Stats result;
    if (impl_ == nullptr) {
        return result;
    }
    result.submitted = impl_->submitted.load(std::memory_order_relaxed);
    result.rejected = impl_->rejected.load(std::memory_order_relaxed);
    result.discarded = impl_->discarded.load(std::memory_order_relaxed);
    result.completed = impl_->completed.load(std::memory_order_relaxed);
    result.worker_completed = impl_->worker_completed.load(std::memory_order_relaxed);
    result.inline_completed = impl_->inline_completed.load(std::memory_order_relaxed);
    result.pending = impl_->pending.load(std::memory_order_relaxed);
    result.wakes = impl_->wakes.load(std::memory_order_relaxed);
    return result;
}

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
namespace detail {
void install_pool_test_hooks(const PoolTestHooks* hooks) noexcept {
    test_hooks.store(hooks, std::memory_order_release);
}
}  // namespace detail
#endif

}  // namespace magpie
