#include "../support/watchdog.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <magpie/chase_lev_deque.hpp>
#include <magpie/thread_pool.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace {
struct Item final : magpie::Task {
    void run() override {}
    std::size_t id = 0;
};
} // namespace
TEST(DequeStress, LastElementRaceAndSlotReuse) {
    constexpr std::size_t count = 1000000;
    magpie::ChaseLevDeque q(4);
    auto items = std::make_unique<Item[]>(count);
    auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
    std::atomic<bool> done{false};
    std::atomic<std::size_t> invalid{0};
    auto observe = [&](magpie::Task* task) {
        auto id = static_cast<Item*>(task)->id;
        if (id >= count) {
            ++invalid;
        } else {
            seen[id].fetch_add(1);
        }
    };
    magpie::test::Watchdog wd([&] { q.dump_for_test(); }, std::chrono::seconds(120));
    std::vector<std::thread> thieves;
    for (unsigned t = 0; t < 3; ++t) {
        thieves.emplace_back([&] {
            for (;;) {
                if (auto* task = q.steal()) {
                    observe(task);
                } else if (done.load(std::memory_order_acquire)) {
                    // Publisher completion precedes this second probe.
                    if (auto* task = q.steal()) {
                        observe(task);
                    } else
                        break;
                } else
                    std::this_thread::yield();
            }
        });
    }
    for (std::size_t i = 0; i < count; ++i) {
        items[i].id = i;
        while (!q.try_push(&items[i])) {
            if (auto* t = q.pop())
                observe(t);
        }
        if (i % 2 == 0) {
            if (auto* t = q.pop())
                observe(t);
        }
    }
    while (auto* t = q.pop())
        observe(t);
    done.store(true, std::memory_order_release);
    for (auto& t : thieves)
        t.join();
    EXPECT_EQ(invalid.load(), 0U);
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (seen[i].load() != 1)
            ++wrong;
    }
    EXPECT_EQ(wrong, 0U);
}
