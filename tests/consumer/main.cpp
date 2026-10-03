#include <magpie/build_config.hpp>
#include <magpie/build_info.hpp>
#include <magpie/thread_pool.hpp>

#include <cstring>

int main() {
    static_assert(__cplusplus >= 202002L, "magpie consumers require C++20");

    const magpie::BuildInfo info = magpie::build_info();
    if (info.version == nullptr || info.platform == nullptr || info.compiler == nullptr ||
        info.build_type == nullptr || info.sanitizer == nullptr) {
        return 1;
    }
    if (std::strcmp(info.version, MAGPIE_BUILD_VERSION) != 0 ||
        std::strcmp(info.platform, MAGPIE_BUILD_PLATFORM) != 0 ||
        std::strcmp(info.compiler, MAGPIE_BUILD_COMPILER) != 0 ||
        info.cache_line_size != MAGPIE_CACHE_LINE) {
        return 2;
    }
    magpie::ThreadPoolOptions options;
    options.worker_count = 1;
    magpie::ThreadPool pool(options);
    auto future = pool.submit_async([] { return 42; });
    pool.submit([] {});
    pool.drain();
    if (future.get() != 42) {
        return 3;
    }
    pool.shutdown();
    return 0;
}
