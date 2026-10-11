#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <magpie/build_config.hpp>
#include <memory>
#include <stdexcept>
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#include <magpie/detail/deque_test_hooks.hpp>
#endif

namespace magpie {
class Task;

// Single owner push/pop, multiple thieves. No Task execution/destruction here.
class ChaseLevDeque final {
public:
    static_assert(std::atomic<Task*>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    explicit ChaseLevDeque(std::size_t capacity
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                           ,
                           const detail::DequeTestHooks* hooks = nullptr
#endif
                           )
        : capacity_(validate(capacity)), mask_(capacity_ - 1),
          slots_(std::make_unique<std::atomic<Task*>[]>(capacity_))
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
          ,
          hooks_(hooks)
#endif
    {
    }
    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    bool try_push(Task* task) noexcept {
        if (task == nullptr) {
            fail("null deque Task");
        }
        const auto b = bottom_.load(std::memory_order_relaxed);
        const auto t = top_.load(std::memory_order_acquire);
        // Owner is between operations, so its bottom is nonnegative. No
        // arbitrary two-atomic snapshot is asserted as a stable invariant.
        if (b < 0 || t < 0) {
            fail("invalid deque index");
        }
        if (b - t >= static_cast<std::int64_t>(capacity_)) {
            return false;
        }
        if (b == std::numeric_limits<std::int64_t>::max()) {
            fail("deque index limit");
        }
        slots_[static_cast<std::size_t>(b) & mask_].store(task, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    Task* pop() noexcept {
        const auto previous = bottom_.load(std::memory_order_relaxed);
        if (previous < 0) {
            fail("invalid deque owner index");
        }
        const auto b = previous - 1; // 0 -> -1 is the valid empty-pop intermediate state.
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto t = top_.load(std::memory_order_relaxed);
        Task* candidate = nullptr;
        if (t <= b) {
            candidate = slots_[static_cast<std::size_t>(b) & mask_].load(std::memory_order_relaxed);
            if (t == b) {
                if (t == std::numeric_limits<std::int64_t>::max()) {
                    fail("deque index limit");
                }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                if (hooks_ && hooks_->pop_before_last_cas) {
                    hooks_->pop_before_last_cas(b, hooks_->context);
                }
#endif
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                  std::memory_order_seq_cst)) {
                    candidate = nullptr;
                }
                // Failed CAS overwrites t. Restoration uses the stable owner b.
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        } else {
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return candidate;
    }

    Task* steal() noexcept {
        auto t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto b = bottom_.load(std::memory_order_acquire);
        if (t >= b) {
            return nullptr;
        }
        Task* candidate =
            slots_[static_cast<std::size_t>(t) & mask_].load(std::memory_order_relaxed);
        if (t == std::numeric_limits<std::int64_t>::max()) {
            fail("deque index limit");
        }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
        if (hooks_ && hooks_->steal_before_cas) {
            hooks_->steal_before_cas(t, hooks_->context);
        }
#endif
        if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_seq_cst)) {
            return nullptr; // Do not inspect, dereference or destroy the failed
                            // candidate.
        }
        return candidate;
    }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    std::array<std::int64_t, 2> indices_for_test() const noexcept {
        return {top_.load(std::memory_order_relaxed), bottom_.load(std::memory_order_relaxed)};
    }
    void initialize_empty_at_for_test(std::int64_t index) noexcept {
        if (index < 0) {
            fail("invalid deque test index");
        }
        top_.store(index, std::memory_order_relaxed);
        bottom_.store(index, std::memory_order_relaxed);
    }
    void dump_for_test() const noexcept {
        const auto p = indices_for_test();
        std::fprintf(stderr, "deque top=%lld bottom=%lld capacity=%zu (approximate snapshot)\n",
                     static_cast<long long>(p[0]), static_cast<long long>(p[1]), capacity_);
    }
#endif
private:
    static std::size_t validate(std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("deque capacity must be a power of two >=2");
        }
        if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(std::atomic<Task*>) ||
            capacity > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::length_error("deque allocation/index overflow");
        }
        return capacity;
    }
    [[noreturn]] static void fail(const char* message) noexcept {
        std::fprintf(stderr, "magpie: %s\n", message);
        std::terminate();
    }
    const std::size_t capacity_, mask_;
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::int64_t> top_{0};
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::int64_t> bottom_{0};
    std::unique_ptr<std::atomic<Task*>[]> slots_;
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    const detail::DequeTestHooks* hooks_;
#endif
};
} // namespace magpie
