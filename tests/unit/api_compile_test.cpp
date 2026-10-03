#include <magpie/thread_pool.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

namespace {

struct LvalueOnlyCallable {
    int operator()() & { return 1; }
};

struct RvalueOnlyCallable {
    int operator()() && { return 1; }
};

struct NoncopyableLvalueCallable {
    NoncopyableLvalueCallable() = default;
    NoncopyableLvalueCallable(const NoncopyableLvalueCallable&) = delete;
    int operator()() & { return 1; }
};

static_assert(magpie::TaskCallable<LvalueOnlyCallable>);
static_assert(!magpie::TaskCallable<RvalueOnlyCallable>);
static_assert(magpie::TaskCallable<std::unique_ptr<int>> == false);
static_assert(!magpie::TaskCallable<NoncopyableLvalueCallable&>);
static_assert(std::same_as<
              decltype(std::declval<magpie::ThreadPool&>().submit_async([] { return 1; })),
              std::future<int>>);

}  // namespace

TEST(MagpieApiCompile, AcceptsLvalueOnlyAndMoveOnlyCallables) {
    magpie::ThreadPoolOptions options;
    options.worker_count = 1;
    magpie::ThreadPool pool(options);

    LvalueOnlyCallable lvalue;
    auto lvalue_result = pool.submit_async(lvalue);
    auto move_only_result = pool.submit_async([value = std::make_unique<int>(9)] {
        return *value;
    });

    EXPECT_EQ(lvalue_result.get(), 1);
    EXPECT_EQ(move_only_result.get(), 9);
    pool.drain();
}
