#pragma once

#include <cstddef>

#include <magpie/export.hpp>

namespace magpie {

struct BuildInfo {
    const char* version;
    const char* platform;
    const char* compiler;
    const char* build_type;
    const char* sanitizer;
    std::size_t cache_line_size;
};

// Returns immutable compile-time metadata for diagnostics and consumer checks.
MAGPIE_EXPORT BuildInfo build_info() noexcept;
// Build-time queue selection; does not add a runtime scheduling policy.
MAGPIE_EXPORT const char* global_queue_backend() noexcept;

} // namespace magpie
