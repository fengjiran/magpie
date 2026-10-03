#include <magpie/mpmc_queue.hpp>
#include <gtest/gtest.h>
#include <limits>

TEST(MpmcQueueDeath, MpmcIndexLimit) {
    EXPECT_DEATH(
        {
            magpie::MPMCQueue<int> queue(2);
            queue.initialize_empty_at_for_test(std::numeric_limits<std::size_t>::max() - 1);
            (void)queue.enqueue(7);
        },
        "MPMC index limit");
    EXPECT_DEATH(
        {
            magpie::MPMCQueue<int> queue(2);
            queue.initialize_empty_at_for_test(std::numeric_limits<std::size_t>::max() - 1);
            int out = 0;
            (void)queue.dequeue(out);
        },
        "MPMC index limit");
}
TEST(MpmcQueueDeath, NullPointerPayload) {
    EXPECT_DEATH(
        {
            magpie::MPMCQueue<int*> queue(2);
            (void)queue.enqueue(nullptr);
        },
        "null MPMC");
}
