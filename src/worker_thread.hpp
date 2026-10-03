#pragma once

#include <cstddef>

#if defined(__linux__)
#include <pthread.h>
#else
#include <thread>
#endif

namespace magpie::detail {

class WorkerThread final {
public:
    using Entry = void (*)(void*) noexcept;

    WorkerThread() noexcept = default;
    ~WorkerThread() noexcept;

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    void start(Entry entry, void* argument, std::size_t stack_size, bool pin_to_cores,
               std::size_t worker_index);
    void join() noexcept;

private:
#if defined(__linux__)
    static void* pthread_trampoline(void* argument) noexcept;

    pthread_t thread_{};
    Entry entry_ = nullptr;
    void* argument_ = nullptr;
    bool started_ = false;
#else
    std::thread thread_;
#endif
};

}  // namespace magpie::detail
