#include "event_count.hpp"
#include <gtest/gtest.h>
#include <limits>
using E = magpie::detail::EventCount;
TEST(EventCountDeath, EpochLimit) {
    EXPECT_DEATH(
        {
            E e(1);
            e.initialize_epoch_for_test(std::numeric_limits<std::uint64_t>::max());
            e.notify_one();
        },
        "epoch limit");
}
TEST(EventCountDeath, RegistrationContract) {
    EXPECT_DEATH(
        {
            E e(1);
            e.enter(0);
            e.enter(0);
        },
        "nested EventCount");
    EXPECT_DEATH(
        {
            E e(1);
            e.leave(0);
        },
        "unbalanced EventCount");
    EXPECT_DEATH(
        {
            E e(1);
            e.enter(1);
        },
        "slot index");
    EXPECT_DEATH(
        {
            E e(1);
            e.enter(0);
        },
        "registered EventCount destruction");
}
