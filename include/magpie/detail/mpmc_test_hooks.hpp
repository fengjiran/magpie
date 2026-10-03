#pragma once
#if !defined(MAGPIE_ENABLE_TEST_HOOKS)
#error "MPMC test hooks require a test-enabled translation unit"
#endif
#include <cstddef>
namespace magpie::detail {
struct MpmcTestHooks {
    void (*after_enqueue_claim)(std::size_t position, void* context) noexcept = nullptr;
    void (*after_dequeue_claim)(std::size_t position, void* context) noexcept = nullptr;
    void* context = nullptr;
    void (*after_enqueue_publish)(std::size_t position, void* context) noexcept = nullptr;
};
} // namespace magpie::detail
