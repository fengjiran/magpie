#include <magpie/build_config.hpp>
#include <magpie/build_info.hpp>

#include <cstring>

int main() {
    static_assert(__cplusplus >= 202002L, "magpie consumers require C++20");

#if defined(MAGPIE_EXPECT_SHARED) && defined(MAGPIE_STATIC_DEFINE)
#error "shared-library consumers must not inherit the static-library definition"
#elif !defined(MAGPIE_EXPECT_SHARED) && !defined(MAGPIE_STATIC_DEFINE)
#error "static-library consumers must inherit the static-library definition"
#endif

    const magpie::BuildInfo info = magpie::build_info();
    return info.version != nullptr && info.platform != nullptr && info.compiler != nullptr &&
                   info.build_type != nullptr && info.sanitizer != nullptr &&
                   std::strcmp(info.version, MAGPIE_BUILD_VERSION) == 0 &&
                   std::strcmp(info.platform, MAGPIE_BUILD_PLATFORM) == 0 &&
                   std::strcmp(info.compiler, MAGPIE_BUILD_COMPILER) == 0 &&
                   std::strcmp(info.build_type, MAGPIE_BUILD_TYPE) == 0 &&
                   info.cache_line_size == MAGPIE_CACHE_LINE
               ? 0
               : 1;
}
