#include "../support/watchdog.hpp"
#include "event_count.hpp"
#include <atomic>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <thread>
#include <vector>

namespace {
using E = magpie::detail::EventCount;
using P = magpie::detail::EventCountHookPoint;
struct Pause {
    P point;
    std::latch reached{1}, release{1};
    std::atomic<bool> used{false};
    static void callback(P point, std::size_t, void* context) noexcept {
        auto& p = *static_cast<Pause*>(context);
        if (p.point == point && !p.used.exchange(true)) {
            p.reached.count_down();
            p.release.wait();
        }
    }
};
void dump(E& e) {
    auto s = e.snapshot();
    std::fprintf(stderr, "epoch=%llu registered=%llu scans=%llu waits=%llu returns=%llu\n",
                 static_cast<unsigned long long>(s.epoch),
                 static_cast<unsigned long long>(s.registered),
                 static_cast<unsigned long long>(s.scan_slots),
                 static_cast<unsigned long long>(s.wait_calls),
                 static_cast<unsigned long long>(s.wait_returns));
}
} // namespace
TEST(EventCount, SeparateWaitWords) {
    E e(2);
    EXPECT_NE(e.address_for_test(0), e.address_for_test(1));
    const auto old = e.prepare_wait();
    e.enter(0);
    e.notify_all();
    const auto fresh = e.prepare_wait();
    e.enter(1);
    EXPECT_EQ(e.word_for_test(0), E::Signaled);
    EXPECT_EQ(e.word_for_test(1), E::Waiting);
    EXPECT_FALSE(e.wait_registered(0, old));
    e.notify_all();
    EXPECT_FALSE(e.wait_registered(1, fresh));
    EXPECT_EQ(e.snapshot().registered, 0U);
    EXPECT_THROW(E(0), std::invalid_argument);
    EXPECT_THROW((void)E(std::numeric_limits<std::size_t>::max()), std::length_error);
}
TEST(EventCount, NotifyWithoutWaitersStillAdvancesEpoch) {
    E e(2);
    e.notify_one();
    e.notify_one();
    auto s = e.snapshot();
    EXPECT_EQ(s.epoch, 2U);
    EXPECT_EQ(s.scan_slots, 0U);
    EXPECT_EQ(s.wakes, 0U);
}
TEST(EventCount, WakeBeforeKernelWait) {
    Pause pause{P::BeforeBackendWait};
    magpie::detail::EventCountTestHooks hooks{Pause::callback, &pause};
    E e(1, &hooks);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::thread waiter([&] {
        auto key = e.prepare_wait();
        e.enter(0);
        EXPECT_TRUE(e.wait_registered(0, key));
    });
    pause.reached.wait();
    e.notify_one();
    pause.release.count_down();
    waiter.join();
    EXPECT_EQ(e.snapshot().registered, 0U);
    EXPECT_EQ(e.word_for_test(0), E::Idle);
#if MAGPIE_EVENTCOUNT_USE_FUTEX
    EXPECT_EQ(e.snapshot().eagain, 1U);
    EXPECT_EQ(e.snapshot().wait_syscalls, 1U);
#endif
}
TEST(EventCount, ConcurrentNotifyRegistration) {
    Pause pause{P::AfterWaiterIncrement};
    magpie::detail::EventCountTestHooks hooks{Pause::callback, &pause};
    E e(1, &hooks);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::thread waiter([&] {
        auto key = e.prepare_wait();
        e.enter(0);
        EXPECT_FALSE(e.wait_registered(0, key));
    });
    pause.reached.wait();
    e.notify_one();
    EXPECT_EQ(e.snapshot().wakes, 0U);
    pause.release.count_down();
    waiter.join();
    EXPECT_EQ(e.snapshot().registered, 0U);
}
TEST(EventCount, OldSignalCannotUndoNextRegistration) {
    Pause pause{P::AfterSignalBeforeWake};
    magpie::detail::EventCountTestHooks hooks{Pause::callback, &pause};
    E e(1, &hooks);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    e.enter(0);
    std::thread old([&] { e.notify_one(); });
    pause.reached.wait();
    e.leave(0);
    const auto key = e.prepare_wait();
    e.enter(0);
    std::thread fresh([&] { e.notify_one(); });
    while (e.snapshot().epoch != 2)
        std::this_thread::yield();
    pause.release.count_down();
    old.join();
    fresh.join();
    EXPECT_EQ(e.word_for_test(0), E::Signaled);
    EXPECT_FALSE(e.wait_registered(0, key));
    EXPECT_EQ(e.snapshot().registered, 0U);
}
TEST(EventCount, SpuriousReturnBalancesRegistration) {
    magpie::detail::EventCountTestHooks hooks;
    hooks.return_without_wait = [](std::size_t, void*) noexcept { return true; };
    E e(1, &hooks);
    const auto key = e.prepare_wait();
    e.enter(0);
    EXPECT_TRUE(e.wait_registered(0, key));
    EXPECT_EQ(e.snapshot().registered, 0U);
    EXPECT_EQ(e.snapshot().wait_calls, 1U);
    EXPECT_EQ(e.snapshot().wait_syscalls, 0U);
}
TEST(EventCount, RegistrationBalanced) {
    E e(3);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::atomic<unsigned> finished{0};
    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < 3; ++i)
        waiters.emplace_back([&, i] {
            for (unsigned round = 0; round < 1000; ++round) {
                auto key = e.prepare_wait();
                e.enter(i);
                e.wait_registered(i, key);
            }
            ++finished;
        });
    std::thread n1([&] {
        while (finished.load() != 3) {
            e.notify_one();
            std::this_thread::yield();
        }
    });
    std::thread n2([&] {
        while (finished.load() != 3) {
            e.notify_all();
            std::this_thread::yield();
        }
    });
    for (auto& thread : waiters)
        thread.join();
    n1.join();
    n2.join();
    EXPECT_EQ(e.snapshot().registered, 0U);
    for (unsigned i = 0; i < 3; ++i)
        EXPECT_EQ(e.word_for_test(i), E::Idle);
}
#if !MAGPIE_EVENTCOUNT_USE_FUTEX
TEST(EventCount, GenericPredicateHandshake) {
    Pause pause{P::BeforeKernelWait};
    magpie::detail::EventCountTestHooks hooks{Pause::callback, &pause};
    E e(1, &hooks);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::thread waiter([&] {
        auto key = e.prepare_wait();
        e.enter(0);
        e.wait_registered(0, key);
    });
    pause.reached.wait();
    std::thread notifier([&] { e.notify_one(); });
    while (e.snapshot().epoch == 0)
        std::this_thread::yield();
    pause.release.count_down();
    waiter.join();
    notifier.join();
    EXPECT_EQ(e.snapshot().registered, 0U);
    EXPECT_EQ(e.snapshot().wakes, 1U);
}
#endif

#if !MAGPIE_EVENTCOUNT_USE_FUTEX
TEST(EventCount, GenericSpuriousWakeKeepsPredicate) {
    std::latch entered(1);
    magpie::detail::EventCountTestHooks hooks{
        [](P point, std::size_t, void* context) noexcept {
            if (point == P::BeforeKernelWait) static_cast<std::latch*>(context)->count_down();
        }, &entered};
    E e(1, &hooks);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::thread waiter([&] {
        auto key = e.prepare_wait(); e.enter(0); e.wait_registered(0, key);
    });
    entered.wait();
    e.spurious_wake_for_test(0);
    // The helper has taken/released the same mutex after the waiter entered.
    // Without a state change the predicate cannot return, regardless of timing.
    EXPECT_EQ(e.snapshot().wait_returns, 0U);
    EXPECT_EQ(e.word_for_test(0), E::Waiting);
    e.notify_one(); waiter.join();
    EXPECT_EQ(e.snapshot().registered, 0U);
}
#endif
