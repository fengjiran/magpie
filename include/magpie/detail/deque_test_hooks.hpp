#pragma once
#if !defined(MAGPIE_ENABLE_TEST_HOOKS)
#error "deque test hooks require test-enabled translation units"
#endif
#include <cstdint>
namespace magpie::detail {
struct DequeTestHooks {
    void (*steal_before_cas)(std::int64_t top, void* context) noexcept = nullptr;
    void (*pop_before_last_cas)(std::int64_t bottom, void* context) noexcept = nullptr;
    void* context = nullptr;
};
} // namespace magpie::detail
