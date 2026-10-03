#include <magpie/thread_pool.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

TEST(ThreadPoolStress, ConcurrentProducersPreserveAcceptedTaskCount) {
    magpie::ThreadPoolOptions options;
    options.worker_count = 4;
    options.global_queue_capacity = 128;
    options.local_deque_capacity = 16;
    options.rejection = magpie::RejectionPolicy::CallerRuns;
    magpie::ThreadPool pool(options);

    constexpr std::size_t producer_count = 4;
    constexpr std::size_t tasks_per_producer = 250'000;
    constexpr std::size_t total_tasks = producer_count * tasks_per_producer;
    auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(total_tasks);
    for (std::size_t id = 0; id < total_tasks; ++id) {
        seen[id].store(0, std::memory_order_relaxed);
    }
    std::atomic<std::size_t> duplicates{0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &pool, &seen, &duplicates] {
            const auto first_id = producer * tasks_per_producer;
            for (std::size_t offset = 0; offset < tasks_per_producer; ++offset) {
                const auto id = first_id + offset;
                auto* const observations = seen.get();
                pool.submit([id, observations, &duplicates] {
                    const auto previous = observations[id].fetch_add(
                        1, std::memory_order_relaxed);
                    if (previous != 0) {
                        duplicates.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    pool.drain();

    EXPECT_EQ(duplicates.load(std::memory_order_relaxed), 0U);
    std::size_t invalid_ids = 0;
    for (std::size_t id = 0; id < total_tasks; ++id) {
        if (seen[id].load(std::memory_order_relaxed) != 1U) {
            ++invalid_ids;
        }
    }
    EXPECT_EQ(invalid_ids, 0U);
    const auto stats = pool.stats();
    EXPECT_EQ(stats.pending, 0U);
    EXPECT_EQ(stats.submitted, total_tasks);
    EXPECT_EQ(stats.submitted, stats.completed + stats.discarded);
    EXPECT_EQ(stats.completed, stats.worker_completed + stats.inline_completed);
}
