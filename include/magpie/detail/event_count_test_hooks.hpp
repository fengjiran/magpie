#pragma once
#if !defined(MAGPIE_ENABLE_TEST_HOOKS)
#error "EventCount hooks are only available in test-enabled builds"
#endif
#include <cstddef>
namespace magpie::detail {
enum class EventCountHookPoint {
    AfterWaiterIncrement,
    AfterRegistered,
    BeforeEpochCheck,
    BeforeBackendWait,
    BeforeKernelWait,
    AfterSignalBeforeWake,
    AfterLeave,
    AfterBackendReturn
};
struct EventCountTestHooks {
    void (*on_point)(EventCountHookPoint, std::size_t, void*) noexcept = nullptr;
    void* context = nullptr;
    // Simulates a backend spurious return; never records a fake syscall.
    bool (*return_without_wait)(std::size_t, void*) noexcept = nullptr;
};
} // namespace magpie::detail
