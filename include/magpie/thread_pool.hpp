#pragma once

#include "magpie/export.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace magpie {

class Task {
public:
    virtual ~Task() noexcept = default;
    virtual void run() = 0;
};

template <class F>
class TaskImpl final : public Task {
public:
    template <class G>
        requires std::constructible_from<F, G&&>
    explicit TaskImpl(G&& f) : callable_(std::forward<G>(f)) {}

    void run() override { (void)std::invoke(callable_); }

private:
    F callable_;
};

template <class F>
concept TaskCallable =
    std::constructible_from<std::decay_t<F>, F&&> &&
    std::invocable<std::decay_t<F>&> &&
    std::is_nothrow_destructible_v<std::decay_t<F>>;

enum class RejectionPolicy { CallerRuns, Abort, DiscardOldest };
enum class QueueFullReason { QueueFull, ShuttingDown };

class MAGPIE_EXPORT QueueFullError final : public std::runtime_error {
public:
    explicit QueueFullError(QueueFullReason error_reason);

    const QueueFullReason reason;
};

struct ThreadPoolOptions {
    std::size_t worker_count = 0;
    std::size_t global_queue_capacity = 4096;
    std::size_t local_deque_capacity = 1024;
    RejectionPolicy rejection = RejectionPolicy::CallerRuns;
    bool pin_to_cores = false;
    std::size_t worker_stack_size = 0;
    std::function<void(std::exception_ptr)> exception_handler;
};

class MAGPIE_EXPORT ThreadPool {
public:
    explicit ThreadPool(const ThreadPoolOptions& options = {});
    ~ThreadPool() noexcept;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template <TaskCallable F>
    void submit(F&& f) {
        submit_task(new TaskImpl<std::decay_t<F>>(std::forward<F>(f)));
    }

    template <TaskCallable F>
    auto submit_async(F&& f)
        -> std::future<std::invoke_result_t<std::decay_t<F>&>> {
        using Result = std::invoke_result_t<std::decay_t<F>&>;
        std::packaged_task<Result()> packaged(std::forward<F>(f));
        auto future = packaged.get_future();
        submit_task(new TaskImpl<std::packaged_task<Result()>>(std::move(packaged)));
        return future;
    }

    void shutdown();
    void drain();
    [[nodiscard]] std::size_t worker_count() const noexcept;

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t rejected = 0;
        std::uint64_t discarded = 0;
        std::uint64_t completed = 0;
        std::uint64_t worker_completed = 0;
        std::uint64_t inline_completed = 0;
        std::uint64_t pending = 0;
        std::uint64_t stolen = 0;
        std::uint64_t local_spills = 0;
        std::uint64_t wakes = 0;
        std::optional<std::uint64_t> wake_threads;
        std::uint64_t post_wake_hit = 0;
        std::uint64_t post_wake_empty = 0;
        std::uint64_t pre_sleep_scan_hit = 0;
        std::uint64_t pre_sleep_scan_empty = 0;
        std::uint64_t notify_scan_slots = 0;
    };

    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void submit_task(Task* task);
};

}  // namespace magpie
