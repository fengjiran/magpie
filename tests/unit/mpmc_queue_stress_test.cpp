#include <magpie/mpmc_queue.hpp>
#include <gtest/gtest.h>
#include "../support/watchdog.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

TEST(MpmcQueueStress, MultiProdMultiCons) {
    constexpr std::size_t count = 1000000;
    magpie::MPMCQueue<std::uint64_t> queue(16);
    auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
    std::atomic<std::size_t> done{0}, invalid{0};
    magpie::test::Watchdog watchdog([&] { queue.dump_for_test(); std::fprintf(stderr, "producers_done=%zu invalid_ids=%zu\n", done.load(), invalid.load()); }, std::chrono::seconds(120));
    std::vector<std::thread> threads;
    for (std::size_t p = 0; p < 4; ++p) {
        threads.emplace_back([&, p] {
            for (std::size_t id = p; id < count; id += 4) {
                while (!queue.enqueue(id)) {
                    std::this_thread::yield();
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t c = 0; c < 4; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                std::uint64_t id = 0;
                bool got = queue.dequeue(id);
                if (!got && done.load(std::memory_order_acquire) == 4) {
                    got = queue.dequeue(id);
                    if (!got) {
                        break;
                    }
                }
                if (got) {
                    if (id >= count) {
                        invalid.fetch_add(1);
                    } else {
                        seen[id].fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(invalid.load(), 0U);
    std::size_t bad = 0;
    for (std::size_t id = 0; id < count; ++id) {
        if (seen[id].load() != 1) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0U);
}
