#pragma once
#include <atomic>
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#endif
namespace magpie::detail {
inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
} // namespace magpie::detail
