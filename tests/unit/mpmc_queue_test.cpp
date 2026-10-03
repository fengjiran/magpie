#include <magpie/mpmc_queue.hpp>
#include <gtest/gtest.h>
#include "../support/watchdog.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {
struct ThrowingCopy {
    ThrowingCopy() noexcept = default;
    ThrowingCopy(const ThrowingCopy&) noexcept(false) {}
};
static_assert(magpie::MpmcPayload<std::uint64_t>);
static_assert(magpie::MpmcPayload<int*>);
static_assert(!magpie::MpmcPayload<ThrowingCopy>);

struct Stall {
    std::latch claimed{1};
    std::latch release{1};
    std::atomic<std::size_t> position{999};
};
void pause_zero(std::size_t position, void* context) noexcept {
    if (position != 0) {
        return;
    }
    auto& stall = *static_cast<Stall*>(context);
    stall.position.store(position);
    stall.claimed.count_down();
    stall.release.wait();
}
} // namespace

TEST(MpmcQueue, SingleThreadFifo) {
    magpie::MPMCQueue<std::uint64_t> queue(2);
    std::uint64_t out = 123;
    EXPECT_FALSE(queue.dequeue(out));
    EXPECT_EQ(out, 123U);
    ASSERT_TRUE(queue.enqueue(7));
    ASSERT_TRUE(queue.enqueue(9));
    EXPECT_FALSE(queue.enqueue(11));
    ASSERT_TRUE(queue.dequeue(out));
    EXPECT_EQ(out, 7U);
    ASSERT_TRUE(queue.dequeue(out));
    EXPECT_EQ(out, 9U);
    EXPECT_FALSE(queue.dequeue(out));
    EXPECT_THROW(magpie::MPMCQueue<int>(3), std::invalid_argument);
    EXPECT_THROW(magpie::MPMCQueue<int>(1), std::invalid_argument);
    EXPECT_THROW(magpie::MPMCQueue<int>(std::size_t{1} << 63), std::length_error);
}

TEST(MpmcQueue, WrapAround) {
    magpie::MPMCQueue<std::uint64_t> queue(4);
    for (std::uint64_t base = 0; base < 10000; base += 4) {
        for (std::uint64_t i = 0; i < 4; ++i) {
            ASSERT_TRUE(queue.enqueue(base + i));
        }
        std::array<std::uint64_t, 7> out{};
        EXPECT_EQ(queue.dequeue_bulk(out.data(), out.size()), 4U);
        for (std::uint64_t i = 0; i < 4; ++i) {
            EXPECT_EQ(out[i], base + i);
        }
    }
}

TEST(MpmcQueue, ProducerClaimStall) {
    Stall stall;
    magpie::detail::MpmcTestHooks hooks{pause_zero, nullptr, &stall};
    magpie::MPMCQueue<std::uint64_t> queue(2, &hooks);
    magpie::test::Watchdog watchdog([&] { queue.dump_for_test(); });
    bool first_ok = false;
    std::thread first([&] { first_ok = queue.enqueue(7); });
    stall.claimed.wait();
    const bool second_ok = queue.enqueue(9);
    std::uint64_t out = 123;
    const bool unavailable = !queue.dequeue(out);
    const bool preserved = out == 123;
    stall.release.count_down();
    first.join();
    ASSERT_TRUE(first_ok);
    ASSERT_TRUE(second_ok);
    EXPECT_TRUE(unavailable);
    EXPECT_TRUE(preserved);
    ASSERT_TRUE(queue.dequeue(out));
    EXPECT_EQ(out, 7U);
    ASSERT_TRUE(queue.dequeue(out));
    EXPECT_EQ(out, 9U);
    EXPECT_FALSE(queue.dequeue(out));
}

TEST(MpmcQueue, ConsumerClaimStall) {
    Stall stall;
    magpie::detail::MpmcTestHooks hooks{nullptr, pause_zero, &stall};
    magpie::MPMCQueue<std::uint64_t> queue(2, &hooks);
    magpie::test::Watchdog watchdog([&] { queue.dump_for_test(); });
    ASSERT_TRUE(queue.enqueue(7));
    ASSERT_TRUE(queue.enqueue(9));
    std::uint64_t first_value = 0;
    bool first_ok = false;
    std::thread first([&] { first_ok = queue.dequeue(first_value); });
    stall.claimed.wait();
    std::uint64_t second_value = 0;
    const bool second_ok = queue.dequeue(second_value);
    // Both positions can have been consumed; slot 0 still belongs to the
    // suspended consumer. A logical-size oracle would incorrectly allow reuse.
    const bool unavailable = !queue.enqueue(11);
    stall.release.count_down();
    first.join();
    ASSERT_TRUE(first_ok);
    EXPECT_EQ(first_value, 7U);
    ASSERT_TRUE(second_ok);
    EXPECT_EQ(second_value, 9U);
    EXPECT_TRUE(unavailable);
    ASSERT_TRUE(queue.enqueue(11));
    ASSERT_TRUE(queue.dequeue(second_value));
    EXPECT_EQ(second_value, 11U);
}

namespace {
thread_local std::uint64_t enqueued_id = 0;
thread_local std::size_t dequeued_position = 0;
struct History {
    std::vector<std::atomic<std::uint64_t>> produced;
    std::vector<std::atomic<std::uint64_t>> consumed;
    std::vector<std::atomic<std::uint64_t>> produced_ids;
    std::vector<std::atomic<std::uint64_t>> consumed_ids;
    explicit History(std::size_t count)
        : produced(count), consumed(count), produced_ids(count), consumed_ids(count) {
        for (auto& x : produced) {
            x.store(0);
        }
        for (auto& x : consumed) {
            x.store(0);
        }
    }
};
void produced(std::size_t position, void* context) noexcept {
    auto& h = *static_cast<History*>(context);
    if (position >= h.produced.size()) {
        std::terminate();
    }
    h.produced[position].fetch_add(1);
    h.produced_ids[position].store(enqueued_id + 1);
}
void consumed(std::size_t position, void* context) noexcept {
    auto& h = *static_cast<History*>(context);
    if (position >= h.consumed.size()) {
        std::terminate();
    }
    h.consumed[position].fetch_add(1);
    dequeued_position = position;
}
} // namespace

TEST(MpmcQueue, RelaxedQueueHistory) {
    constexpr std::size_t count = 4000;
    History history(count);
    magpie::detail::MpmcTestHooks hooks{produced, consumed, &history};
    magpie::MPMCQueue<std::uint64_t> queue(4, &hooks);
    magpie::test::Watchdog watchdog([&] { queue.dump_for_test(); });
    std::array<std::vector<std::uint64_t>, 2> results;
    std::atomic<std::size_t> done{0};
    std::vector<std::thread> threads;
    for (std::size_t p = 0; p < 2; ++p) {
        threads.emplace_back([&, p] {
            for (std::size_t id = p; id < count; id += 2) {
                enqueued_id = id;
                while (!queue.enqueue(id)) {
                    std::this_thread::yield();
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::size_t c = 0; c < 2; ++c) {
        results[c].reserve(count);
        threads.emplace_back([&, c] {
            for (;;) {
                std::uint64_t id = 0;
                if (queue.dequeue(id)) {
                    history.consumed_ids[dequeued_position].store(id + 1);
                    results[c].push_back(id);
                } else if (done.load(std::memory_order_acquire) == 2) {
                    // Producer completion closes all reservation holes. A final
                    // probe is needed after acquiring completion, not before it.
                    if (!queue.dequeue(id)) {
                        break;
                    }
                    history.consumed_ids[dequeued_position].store(id + 1);
                    results[c].push_back(id);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::vector<unsigned> seen(count);
    for (const auto& values : results) {
        for (auto id : values) {
            ASSERT_LT(id, count);
            ++seen[id];
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(seen[i], 1U);
        EXPECT_EQ(history.produced[i].load(), 1U);
        EXPECT_EQ(history.consumed[i].load(), 1U);
        EXPECT_EQ(history.produced_ids[i].load(), history.consumed_ids[i].load());
    }
}

TEST(MpmcQueue, PointerValueApiAndZeroBulk) {
    magpie::MPMCQueue<int*> queue(2);
    int value = 7;
    int* output = nullptr;
    EXPECT_EQ(queue.dequeue_bulk(nullptr, 0), 0U);
    ASSERT_TRUE(queue.enqueue(&value));
    ASSERT_TRUE(queue.dequeue(output));
    EXPECT_EQ(output, &value);
}
