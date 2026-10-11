#include <magpie/build_info.hpp>

#include <magpie/build_config.hpp>

static_assert(MAGPIE_CACHE_LINE >= alignof(std::max_align_t));
static_assert((MAGPIE_CACHE_LINE & (MAGPIE_CACHE_LINE - 1)) == 0);

namespace magpie {

const char* global_queue_backend() noexcept { return MAGPIE_BUILD_QUEUE_BACKEND; }

const char* scheduling_backend() noexcept {
#if MAGPIE_ENABLE_WORK_STEALING
    return "work-stealing";
#else
    return "global";
#endif
}

const char* parking_backend() noexcept { return MAGPIE_BUILD_PARKING_BACKEND; }
const char* event_count_backend() noexcept { return MAGPIE_BUILD_EVENTCOUNT_BACKEND; }

BuildInfo build_info() noexcept {
    return {
        MAGPIE_BUILD_VERSION,
        MAGPIE_BUILD_PLATFORM,
        MAGPIE_BUILD_COMPILER,
        MAGPIE_BUILD_TYPE,
        MAGPIE_BUILD_SANITIZER,
        MAGPIE_CACHE_LINE,
    };
}

} // namespace magpie
