#include "../support/watchdog.hpp"
#include "event_count.hpp"
#include <atomic>
#include <csignal>
#include <gtest/gtest.h>
#include <pthread.h>
#include <thread>

namespace {
using E = magpie::detail::EventCount;
void caught_signal(int) {}
void dump(E& e) {
    const auto s = e.snapshot();
    std::fprintf(stderr, "futex calls=%llu returns=%llu EINTR=%llu registered=%llu\n",
                 static_cast<unsigned long long>(s.wait_syscalls),
                 static_cast<unsigned long long>(s.wait_returns),
                 static_cast<unsigned long long>(s.eintr),
                 static_cast<unsigned long long>(s.registered));
}
} // namespace
TEST(EventCountLinux, RealSignalInterruptsKernelWait) {
    struct sigaction action{}, old{};
    action.sa_handler = caught_signal;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(sigaction(SIGUSR1, &action, &old), 0);
    struct Restore {
        struct sigaction saved;
        ~Restore() { sigaction(SIGUSR1, &saved, nullptr); }
    } restore{old};
    E e(1);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        EXPECT_EQ(pthread_sigmask(SIG_UNBLOCK, &mask, nullptr), 0);
        auto key = e.prepare_wait();
        e.enter(0);
        e.wait_registered(0, key);
        done.store(true);
    });
    while (e.snapshot().wait_syscalls == 0)
        std::this_thread::yield();
    while (!done.load()) {
        (void)pthread_kill(waiter.native_handle(), SIGUSR1);
        std::this_thread::yield();
    }
    waiter.join();
    EXPECT_EQ(e.snapshot().eintr, 1U);
    EXPECT_EQ(e.snapshot().registered, 0U);
}
TEST(EventCountLinux, SpuriousWakeWithoutWordChange) {
    E e(1);
    magpie::test::Watchdog watchdog([&] { dump(e); });
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        auto key = e.prepare_wait();
        e.enter(0);
        e.wait_registered(0, key);
        done.store(true);
    });
    while (e.snapshot().wait_syscalls == 0)
        std::this_thread::yield();
    while (!done.load()) {
        e.spurious_wake_for_test(0);
        std::this_thread::yield();
    }
    waiter.join();
    EXPECT_EQ(e.snapshot().wait_returns, 1U);
    EXPECT_EQ(e.snapshot().registered, 0U);
    EXPECT_EQ(e.snapshot().wakes, 0U); // helper is deliberately not a protocol notification.
}
