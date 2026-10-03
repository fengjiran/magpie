#include <magpie/thread_pool.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <latch>
#include <thread>

TEST(ThreadPoolDeath, DestructorControlGuardDeath) {
    EXPECT_DEATH(
        {
            magpie::ThreadPoolOptions options;
            options.worker_count = 1;
            auto* pool = new magpie::ThreadPool(options);
            std::latch entered(1);
            std::latch release(1);
            pool->submit([pool, &entered, &release] {
                entered.count_down();
                release.wait();
                delete pool;
            });
            entered.wait();
            release.count_down();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        },
        "");
}
