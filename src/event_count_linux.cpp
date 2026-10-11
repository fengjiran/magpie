#include "event_count.hpp"
#include <cerrno>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace magpie::detail {
// This is a Linux ABI dependency, not a portable C++ representation claim.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == 4);
static_assert(alignof(std::atomic<std::uint32_t>) >= 4);
void EventCount::backend_wait(std::size_t i) noexcept {
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    hook(EventCountHookPoint::BeforeKernelWait, i);
#endif
    add(wait_syscalls_);
    const auto result = ::syscall(SYS_futex, &slots_[i].word, FUTEX_WAIT_PRIVATE,
                                  static_cast<std::uint32_t>(Waiting), nullptr, nullptr, 0);
    if (result == 0) {
        return;
    }
    const int error = errno;
    if (error == EAGAIN) {
        add(eagain_);
        return;
    }
    if (error == EINTR) {
        add(eintr_);
        return;
    }
    fail("FUTEX_WAIT_PRIVATE failed", error);
}
bool EventCount::backend_signal(std::size_t i) noexcept {
    std::uint32_t expected = Waiting;
    if (!slots_[i].word.compare_exchange_strong(expected, Signaled, std::memory_order_seq_cst)) {
        return false;
    }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    hook(EventCountHookPoint::AfterSignalBeforeWake, i);
#endif
    const auto result =
        ::syscall(SYS_futex, &slots_[i].word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    if (result < 0) {
        fail("FUTEX_WAKE_PRIVATE failed", errno);
    }
    add(wakes_);
    add(wake_threads_, static_cast<std::uint64_t>(result));
    return true;
}
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
void EventCount::spurious_wake_for_test(std::size_t i) noexcept {
    check_index(i);
    if (::syscall(SYS_futex, &slots_[i].word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) < 0) {
        fail("test futex wake failed", errno);
    }
}
#endif
} // namespace magpie::detail
