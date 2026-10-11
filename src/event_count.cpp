#include "event_count.hpp"
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>

#if defined(MAGPIE_ENABLE_TEST_HOOKS)
#define EVC_HOOK(point, index) hook(EventCountHookPoint::point, index)
#else
#define EVC_HOOK(point, index) ((void)0)
#endif

namespace magpie::detail {
EventCount::EventCount(std::size_t count
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
                       ,
                       const EventCountTestHooks* hooks
#endif
                       )
    : count_(validate(count)), slots_(std::make_unique<WaitSlot[]>(count_))
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
      ,
      hooks_(hooks)
#endif
{
}
EventCount::~EventCount() noexcept {
    if (waiters_.load(std::memory_order_relaxed) != 0) {
        fail("registered EventCount destruction");
    }
}
std::size_t EventCount::validate(std::size_t count) {
    if (count == 0) {
        throw std::invalid_argument("EventCount needs at least one slot");
    }
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(WaitSlot)) {
        throw std::length_error("EventCount slot allocation overflow");
    }
    return count;
}
void EventCount::add(std::atomic<std::uint64_t>& value, std::uint64_t amount) noexcept {
    auto old = value.load(std::memory_order_relaxed);
    for (;;) {
        const auto room = std::numeric_limits<std::uint64_t>::max() - old;
        const auto next = old + (amount < room ? amount : room);
        if (value.compare_exchange_weak(old, next, std::memory_order_relaxed)) {
            return;
        }
    }
}
[[noreturn]] void EventCount::fail(const char* message, int error) noexcept {
    std::fprintf(stderr, "magpie: %s (errno=%d)\n", message, error);
    std::terminate();
}
void EventCount::check_index(std::size_t i) const noexcept {
    if (i >= count_) {
        fail("EventCount slot index");
    }
}
std::uint64_t EventCount::prepare_wait() const noexcept {
    return epoch_.load(std::memory_order_seq_cst);
}
void EventCount::enter(std::size_t i) noexcept {
    check_index(i);
    if (slots_[i].word.load(std::memory_order_seq_cst) != Idle) {
        fail("nested EventCount registration");
    }
    if (waiters_.fetch_add(1, std::memory_order_seq_cst) >= count_) {
        fail("EventCount waiter overflow");
    }
    EVC_HOOK(AfterWaiterIncrement, i);
    slots_[i].word.store(Waiting, std::memory_order_seq_cst);
    EVC_HOOK(AfterRegistered, i);
}
void EventCount::leave(std::size_t i) noexcept {
    check_index(i);
    if (slots_[i].word.exchange(Idle, std::memory_order_seq_cst) == Idle) {
        fail("unbalanced EventCount leave");
    }
    if (waiters_.fetch_sub(1, std::memory_order_seq_cst) == 0) {
        fail("EventCount waiter underflow");
    }
    EVC_HOOK(AfterLeave, i);
}
bool EventCount::wait_registered(std::size_t i, std::uint64_t key) noexcept {
    check_index(i);
    EVC_HOOK(BeforeEpochCheck, i);
    const bool wait = epoch_.load(std::memory_order_seq_cst) == key;
    if (wait) {
        EVC_HOOK(BeforeBackendWait, i);
        add(wait_calls_);
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
        if (!(hooks_ && hooks_->return_without_wait &&
              hooks_->return_without_wait(i, hooks_->context)))
#endif
        {
            backend_wait(i);
        }
        add(wait_returns_);
        EVC_HOOK(AfterBackendReturn, i);
    }
    leave(i);
    return wait;
}
void EventCount::advance_epoch() noexcept {
    auto e = epoch_.load(std::memory_order_seq_cst);
    for (;;) {
        if (e == std::numeric_limits<std::uint64_t>::max()) {
            fail("EventCount epoch limit");
        }
        if (epoch_.compare_exchange_weak(e, e + 1, std::memory_order_seq_cst)) {
            return;
        }
    }
}
void EventCount::notify_one() noexcept {
    advance_epoch();
    if (waiters_.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    auto i = cursor_.fetch_add(1, std::memory_order_relaxed) % count_;
    for (std::size_t n = 0; n < count_; ++n) {
        add(scans_);
        if (backend_signal(i)) {
            return;
        }
        if (++i == count_) {
            i = 0;
        }
    }
}
void EventCount::notify_all() noexcept {
    advance_epoch();
    for (std::size_t i = 0; i < count_; ++i) {
        add(scans_);
        (void)backend_signal(i);
    }
}
EventCount::Snapshot EventCount::snapshot() const noexcept {
    return {epoch_.load(),        waiters_.load(),    scans_.load(),        wakes_.load(),
            wake_threads_.load(), wait_calls_.load(), wait_returns_.load(), wait_syscalls_.load(),
            eagain_.load(),       eintr_.load()};
}
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
void EventCount::hook(EventCountHookPoint point, std::size_t i) const noexcept {
    if (hooks_ && hooks_->on_point) {
        hooks_->on_point(point, i, hooks_->context);
    }
}
std::uint32_t EventCount::word_for_test(std::size_t i) const noexcept {
    check_index(i);
    return slots_[i].word.load();
}
const void* EventCount::address_for_test(std::size_t i) const noexcept {
    check_index(i);
    return &slots_[i].word;
}
void EventCount::initialize_epoch_for_test(std::uint64_t e) noexcept {
    if (waiters_.load() != 0) {
        fail("EventCount test initialization with waiters");
    }
    epoch_.store(e);
}
#endif
} // namespace magpie::detail
