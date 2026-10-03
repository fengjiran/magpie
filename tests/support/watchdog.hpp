#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

namespace magpie::test {
// The test binary itself is a CTest child process. On a hung join/wait, this
// observer dumps only independent/atomic state and exits the process; its
// destructor never tries to join the hung test thread. CTest is a second bound.
class Watchdog final {
  public:
    explicit Watchdog(std::function<void()> dump,
                      std::chrono::seconds timeout = std::chrono::seconds(10))
        : thread_([this, dump = std::move(dump), timeout] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (condition_.wait_for(lock, timeout, [this] { return finished_; })) {
                  return;
              }
              lock.unlock();
              std::fputs("magpie test watchdog timeout\n", stderr);
              try {
                  dump();
              } catch (...) {
                  std::fputs("diagnostic callback failed\n", stderr);
              }
              std::fflush(stderr);
              std::_Exit(124);
          }) {}
    ~Watchdog() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        condition_.notify_one();
        thread_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::thread thread_;
};
} // namespace magpie::test
