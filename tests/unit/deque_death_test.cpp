#include <gtest/gtest.h>
#include <limits>
#include <magpie/chase_lev_deque.hpp>
#include <magpie/thread_pool.hpp>

namespace {
struct Item : magpie::Task {
    void run() override {}
};
} // namespace
TEST(DequeDeath, DequeIndexLimit) {
    EXPECT_DEATH(
        {
            magpie::ChaseLevDeque q(4);
            Item item;
            q.initialize_empty_at_for_test(std::numeric_limits<std::int64_t>::max());
            (void)q.try_push(&item);
        },
        "deque index limit");
}
TEST(DequeDeath, NullTask) {
    EXPECT_DEATH(
        {
            magpie::ChaseLevDeque q(4);
            (void)q.try_push(nullptr);
        },
        "null deque");
}
