#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <magpie/build_config.hpp>
#include <memory>
#if !MAGPIE_EVENTCOUNT_USE_FUTEX
#include <condition_variable>
#include <mutex>
#endif
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#include <magpie/detail/event_count_test_hooks.hpp>
#endif

namespace magpie::detail {

// Each index has exactly one registering worker. Slots outlive all waiters and
// notifiers; the pool constructs this object before starting any worker.
class EventCount final {
public:
    enum : std::uint32_t { Idle = 0, Waiting = 1, Signaled = 2 };
    struct Snapshot {
        std::uint64_t epoch, registered, scan_slots, wakes, wake_threads;
        std::uint64_t wait_calls, wait_returns, wait_syscalls, eagain, eintr;
    };
    explicit EventCount(std::size_t count
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                        ,
                        const EventCountTestHooks* hooks = nullptr
#endif
    );
    ~EventCount() noexcept;
    EventCount(const EventCount&) = delete;
    EventCount& operator=(const EventCount&) = delete;
    std::uint64_t prepare_wait() const noexcept;
    void enter(std::size_t index) noexcept;
    void leave(std::size_t index) noexcept;
    // All paths unregister. True means backend returned, including EAGAIN,
    // EINTR or a spurious return, not necessarily that a kernel thread slept.
    bool wait_registered(std::size_t index, std::uint64_t key) noexcept;
    void notify_one() noexcept;
    void notify_all() noexcept;
    Snapshot snapshot() const noexcept;
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    std::uint32_t word_for_test(std::size_t index) const noexcept;
    const void* address_for_test(std::size_t index) const noexcept;
    void initialize_epoch_for_test(std::uint64_t epoch) noexcept;
    void spurious_wake_for_test(std::size_t index) noexcept;
#endif

private:
    struct alignas(MAGPIE_CACHE_LINE) WaitSlot {
        std::atomic<std::uint32_t> word{Idle};
#if !MAGPIE_EVENTCOUNT_USE_FUTEX
        std::mutex mutex;
        std::condition_variable cv;
#endif
    };
    static std::size_t validate(std::size_t count);
    static void add(std::atomic<std::uint64_t>& value, std::uint64_t amount = 1) noexcept;
    [[noreturn]] static void fail(const char* message, int error = 0) noexcept;
    void check_index(std::size_t index) const noexcept;
    void advance_epoch() noexcept;
    void backend_wait(std::size_t index) noexcept;
    bool backend_signal(std::size_t index) noexcept;
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    void hook(EventCountHookPoint point, std::size_t index) const noexcept;
#endif
    const std::size_t count_;
    std::unique_ptr<WaitSlot[]> slots_;
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::uint64_t> epoch_{0};
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::size_t> waiters_{0};
    alignas(MAGPIE_CACHE_LINE) std::atomic<std::size_t> cursor_{0};
    std::atomic<std::uint64_t> scans_{0}, wakes_{0}, wake_threads_{0};
    std::atomic<std::uint64_t> wait_calls_{0}, wait_returns_{0}, wait_syscalls_{0};
    std::atomic<std::uint64_t> eagain_{0}, eintr_{0};
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    const EventCountTestHooks* hooks_;
#endif
};

} // namespace magpie::detail
