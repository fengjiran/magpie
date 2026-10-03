#include <magpie/build_config.hpp>
#include <magpie/build_info.hpp>

#include <gtest/gtest.h>

TEST(BuildInfoTest, ReportsTheConfigurationUsedToBuildTheLibrary) {
    const magpie::BuildInfo info = magpie::build_info();

    ASSERT_NE(info.version, nullptr);
    ASSERT_NE(info.platform, nullptr);
    ASSERT_NE(info.compiler, nullptr);
    ASSERT_NE(info.build_type, nullptr);
    ASSERT_NE(info.sanitizer, nullptr);
    EXPECT_STREQ(info.version, MAGPIE_BUILD_VERSION);
    EXPECT_STREQ(info.platform, MAGPIE_BUILD_PLATFORM);
    EXPECT_STREQ(info.compiler, MAGPIE_BUILD_COMPILER);
    EXPECT_STREQ(info.build_type, MAGPIE_BUILD_TYPE);
    EXPECT_STREQ(info.sanitizer, MAGPIE_BUILD_SANITIZER);
    EXPECT_EQ(info.cache_line_size, MAGPIE_CACHE_LINE);
}

TEST(BuildInfoTest, CachelineConfigurationIsPowerOfTwo) {
    const auto size = magpie::build_info().cache_line_size;
    EXPECT_GE(size, alignof(std::max_align_t));
    EXPECT_EQ(size & (size - 1), 0U);
}
