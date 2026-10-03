#pragma once

#if !defined(MAGPIE_ENABLE_TEST_HOOKS)
#error "magpie test hooks are available only in test-enabled builds"
#endif

#include "magpie/export.hpp"

#include <cstddef>

namespace magpie::detail {

enum class PoolTestHookPoint {
    SubmitAfterGate,
    DrainPredicateFalse,
    PendingZeroBeforeDrainLock,
    WorkerBeforeWait,
    WorkerExit,
    BeforeWorkerCreate,
    ShutdownAfterStop,
};

struct PoolTestHooks {
    void (*on_point)(PoolTestHookPoint point, std::size_t index, void* context) noexcept =
        nullptr;
    int (*before_worker_create)(std::size_t index, void* context) noexcept = nullptr;
    void* context = nullptr;
};

MAGPIE_EXPORT void install_pool_test_hooks(const PoolTestHooks* hooks) noexcept;

}  // namespace magpie::detail
