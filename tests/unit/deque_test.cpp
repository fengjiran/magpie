#include "../support/watchdog.hpp"
#include <array>
#include <atomic>
#include <deque>
#include <gtest/gtest.h>
#include <latch>
#include <magpie/chase_lev_deque.hpp>
#include <magpie/thread_pool.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace {
struct Item final : magpie::Task {
    explicit Item(unsigned n = 0) : id(n) {}
    void run() override {}
    unsigned id;
};
struct Pause {
    std::latch entered{1}, release{1};
    std::atomic<bool> taken{false};
};
void pause_first(std::int64_t, void* context) noexcept {
    auto& p = *static_cast<Pause*>(context);
    if (!p.taken.exchange(true)) {
        p.entered.count_down();
        p.release.wait();
    }
}
} // namespace

TEST(Deque, PushPopSequential) {
    magpie::ChaseLevDeque q(4);
    std::array<Item, 4> items;
    for (unsigned i = 0; i < 4; ++i) {
        items[i].id = i;
        ASSERT_TRUE(q.try_push(&items[i]));
    }
    EXPECT_FALSE(q.try_push(&items[0]));
    for (unsigned i = 4; i > 0; --i) {
        EXPECT_EQ(q.pop(), &items[i - 1]);
    }
    EXPECT_EQ(q.pop(), nullptr);
    EXPECT_EQ(q.steal(), nullptr);
    EXPECT_THROW(magpie::ChaseLevDeque(3), std::invalid_argument);
    EXPECT_THROW(magpie::ChaseLevDeque(std::size_t{1} << 63), std::length_error);
}
TEST(Deque, TwoThreadPushSteal) {
    magpie::ChaseLevDeque q(4);
    std::array<Item, 2000> items;
    std::atomic<unsigned> produced{0};
    std::vector<unsigned> observed;
    observed.reserve(items.size());
    magpie::test::Watchdog wd([&] { q.dump_for_test(); });
    std::thread thief([&] {
        while (observed.size() < items.size()) {
            if (auto* task = q.steal()) {
                observed.push_back(static_cast<Item*>(task)->id);
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (unsigned i = 0; i < items.size(); ++i) {
        items[i].id = i;
        while (!q.try_push(&items[i])) {
            std::this_thread::yield();
        }
        ++produced;
    }
    thief.join();
    ASSERT_EQ(observed.size(), items.size());
    for (unsigned i = 0; i < items.size(); ++i) {
        EXPECT_EQ(observed[i], i);
    }
}
TEST(Deque, StealAcrossOwnerPops) {
    Pause p;
    magpie::detail::DequeTestHooks hooks{pause_first, nullptr, &p};
    magpie::ChaseLevDeque q(8, &hooks);
    std::array<Item, 8> items;
    for (unsigned i = 0; i < 8; ++i) {
        items[i].id = i;
        ASSERT_TRUE(q.try_push(&items[i]));
    }
    magpie::test::Watchdog wd([&] { q.dump_for_test(); });
    magpie::Task* stolen = nullptr;
    std::thread thief([&] { stolen = q.steal(); });
    p.entered.wait();
    std::array<magpie::Task*, 5> popped{};
    for (unsigned i = 0; i < 5; ++i) {
        popped[i] = q.pop();
    }
    p.release.count_down();
    thief.join();
    EXPECT_EQ(stolen, &items[0]);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(popped[i], &items[7 - i]);
    }
    EXPECT_EQ(q.pop(), &items[2]);
    EXPECT_EQ(q.pop(), &items[1]);
    EXPECT_EQ(q.pop(), nullptr);
}
TEST(Deque, LastElementCasFailureRestoresBottom) {
    struct Race {
        Pause thief;
        Pause owner;
    } state;
    magpie::detail::DequeTestHooks hooks{
        [](std::int64_t n, void* ctx) noexcept { pause_first(n, &static_cast<Race*>(ctx)->thief); },
        [](std::int64_t n, void* ctx) noexcept { pause_first(n, &static_cast<Race*>(ctx)->owner); },
        &state};
    magpie::ChaseLevDeque q(4, &hooks);
    auto* item = new Item(1);
    ASSERT_TRUE(q.try_push(item));
    magpie::Task* popped = nullptr;
    magpie::Task* stolen = nullptr;
    magpie::test::Watchdog wd([&] { q.dump_for_test(); });
    std::thread thief([&] { stolen = q.steal(); });
    state.thief.entered.wait();
    std::thread owner([&] { popped = q.pop(); });
    state.owner.entered.wait();
    state.thief.release.count_down();
    thief.join();
    EXPECT_EQ(stolen, item);
    delete stolen;
    state.owner.release.count_down();
    owner.join();
    EXPECT_EQ(popped, nullptr);
    EXPECT_EQ(q.indices_for_test()[0], 1);
    EXPECT_EQ(q.indices_for_test()[1], 1);
    EXPECT_EQ(q.pop(), nullptr);
    Item next(2);
    ASSERT_TRUE(q.try_push(&next));
    EXPECT_EQ(q.pop(), &next);
}
TEST(Deque, StealFromFullDequeDeterministic) {
    Pause p;
    magpie::detail::DequeTestHooks hooks{pause_first, nullptr, &p};
    magpie::ChaseLevDeque q(4, &hooks);
    std::array<std::unique_ptr<Item>, 4> old;
    for (unsigned i = 0; i < 4; ++i) {
        old[i] = std::make_unique<Item>(i);
        ASSERT_TRUE(q.try_push(old[i].get()));
    }
    magpie::Task* lost = nullptr;
    magpie::test::Watchdog wd([&] { q.dump_for_test(); });
    std::thread slow([&] { lost = q.steal(); });
    p.entered.wait();
    EXPECT_EQ(q.steal(), old[0].get());
    old[0].reset();
    for (unsigned i = 4; i > 1; --i) {
        EXPECT_EQ(q.pop(), old[i - 1].get());
        old[i - 1].reset();
    }
    std::array<Item, 4> newer;
    for (unsigned i = 0; i < 4; ++i) {
        newer[i].id = i + 4;
        EXPECT_TRUE(q.try_push(&newer[i]));
    }
    p.release.count_down();
    slow.join();
    EXPECT_EQ(lost, nullptr); // candidate was freed and the same slots reused.
    for (unsigned i = 4; i > 0; --i) {
        EXPECT_EQ(q.pop(), &newer[i - 1]);
    }
    EXPECT_EQ(q.pop(), nullptr);
}
TEST(Deque, DifferentialRandomOps) {
    magpie::ChaseLevDeque q(16);
    std::array<Item, 30000> items;
    std::deque<magpie::Task*> model;
    std::uint64_t seed = 0x9e3779b9;
    unsigned next = 0;
    for (unsigned op = 0; op < 30000; ++op) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        if (seed % 3 == 0) {
            auto* item = &items[next++];
            const bool expected = model.size() < 16;
            EXPECT_EQ(q.try_push(item), expected);
            if (expected)
                model.push_back(item);
        } else if (seed % 3 == 1) {
            auto* expected = model.empty() ? nullptr : model.back();
            EXPECT_EQ(q.pop(), expected);
            if (expected)
                model.pop_back();
        } else {
            auto* expected = model.empty() ? nullptr : model.front();
            EXPECT_EQ(q.steal(), expected);
            if (expected)
                model.pop_front();
        }
        // This test is quiescent, so a stable boundary is actually available.
        auto indices = q.indices_for_test();
        EXPECT_EQ(indices[1] - indices[0], static_cast<std::int64_t>(model.size()));
    }
}

TEST(Deque, LinearizabilitySmoke) {
    // A bounded history oracle, independent of top/bottom implementation.
    // Preserve call-before-return precedence. Steal may return unavailable;
    // successful claims must still match the single head/tail model exactly.
    enum class Kind { Push, Pop, Steal };
    struct Op {
        Kind kind;
        magpie::Task* argument = nullptr;
        magpie::Task* result = nullptr;
        bool pushed = false;
        unsigned call = 0, returned = 0;
    };
    std::atomic<unsigned> current_round{0};
    magpie::test::Watchdog watchdog([&] {
        std::fprintf(stderr, "deque history round=%u\n", current_round.load());
    });
    for (unsigned round = 0; round < 100; ++round) {
        current_round.store(round);
        magpie::ChaseLevDeque queue(4);
        std::array<Item, 4> items;
        for (unsigned i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.try_push(&items[i]));
        }
        std::array<Op, 7> history{{{Kind::Pop},
                                   {Kind::Push, &items[3]},
                                   {Kind::Pop},
                                   {Kind::Steal},
                                   {Kind::Steal},
                                   {Kind::Steal},
                                   {Kind::Steal}}};
        std::atomic<unsigned> sequence{0};
        std::latch start(1);
        auto operate = [&](unsigned index) {
            auto& op = history[index];
            op.call = sequence.fetch_add(1);
            if (op.kind == Kind::Push) {
                op.pushed = queue.try_push(op.argument);
            } else {
                op.result = op.kind == Kind::Pop ? queue.pop() : queue.steal();
            }
            op.returned = sequence.fetch_add(1);
        };
        std::thread owner([&] {
            start.wait();
            for (unsigned i = 0; i < 3; ++i)
                operate(i);
        });
        std::thread thief1([&] {
            start.wait();
            operate(3);
            operate(4);
        });
        std::thread thief2([&] {
            start.wait();
            operate(5);
            operate(6);
        });
        start.count_down();
        owner.join();
        thief1.join();
        thief2.join();
        auto search = [&](auto&& self, unsigned used, std::deque<magpie::Task*> model) -> bool {
            if (used == 127) {
                return true;
            }
            for (unsigned i = 0; i < history.size(); ++i) {
                if (used & (1U << i)) {
                    continue;
                }
                const auto& op = history[i];
                bool eligible = true;
                for (unsigned j = 0; j < history.size(); ++j) {
                    if (!(used & (1U << j)) && history[j].returned < op.call) {
                        eligible = false;
                        break;
                    }
                }
                if (!eligible) {
                    continue;
                }
                auto next = model;
                if (op.kind == Kind::Push) {
                    if (op.pushed) {
                        if (next.size() == 4) {
                            continue;
                        }
                        next.push_back(op.argument);
                    } // failed push is unavailable, not an instantaneous full assertion.
                } else if (op.result) {
                    if (next.empty() ||
                        op.result != (op.kind == Kind::Pop ? next.back() : next.front())) {
                        continue;
                    }
                    if (op.kind == Kind::Pop) {
                        next.pop_back();
                    } else {
                        next.pop_front();
                    }
                } else if (op.kind == Kind::Pop && !next.empty()) {
                    continue;
                }
                if (self(self, used | (1U << i), std::move(next))) {
                    return true;
                }
            }
            return false;
        };
        const bool valid = search(search, 0, {&items[0], &items[1], &items[2]});
        if (!valid) {
            for (const auto& op : history) {
                std::fprintf(stderr,
                             "history kind=%u argument=%p result=%p pushed=%d call=%u return=%u\n",
                             static_cast<unsigned>(op.kind), static_cast<void*>(op.argument),
                             static_cast<void*>(op.result), op.pushed, op.call, op.returned);
            }
        }
        ASSERT_TRUE(valid) << "round=" << round;
    }
}

TEST(Deque, SignedIndexBoundary) {
    magpie::ChaseLevDeque queue(4);
    Item item;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    queue.initialize_empty_at_for_test(maximum - 1);
    ASSERT_TRUE(queue.try_push(&item));
    EXPECT_EQ(queue.steal(), &item);
    EXPECT_EQ(queue.pop(), nullptr);
    EXPECT_EQ(queue.steal(), nullptr);
    EXPECT_EQ(queue.indices_for_test()[0], maximum);
    EXPECT_EQ(queue.indices_for_test()[1], maximum);
}
