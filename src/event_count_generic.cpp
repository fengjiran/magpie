#include "event_count.hpp"

namespace magpie::detail {
void EventCount::backend_wait(std::size_t i) noexcept {
    auto& slot = slots_[i];
    std::unique_lock<std::mutex> lock(slot.mutex);
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    hook(EventCountHookPoint::BeforeKernelWait, i);
#endif
    slot.cv.wait(lock, [&] { return slot.word.load(std::memory_order_seq_cst) != Waiting; });
}
bool EventCount::backend_signal(std::size_t i) noexcept {
    auto& slot = slots_[i];
    std::lock_guard<std::mutex> lock(slot.mutex);
    std::uint32_t expected = Waiting;
    if (!slot.word.compare_exchange_strong(expected, Signaled, std::memory_order_seq_cst)) {
        return false;
    }
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
    hook(EventCountHookPoint::AfterSignalBeforeWake, i);
#endif
    slot.cv.notify_one();
    add(wakes_);
    return true;
}
#if defined(MAGPIE_ENABLE_TEST_HOOKS)
void EventCount::spurious_wake_for_test(std::size_t i) noexcept {
    check_index(i);
    std::lock_guard<std::mutex> lock(slots_[i].mutex);
    slots_[i].cv.notify_one();
}
#endif
} // namespace magpie::detail
