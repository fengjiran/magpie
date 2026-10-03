#include "worker_thread.hpp"

#include <cstdio>
#include <exception>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

#include <array>
#endif

namespace magpie::detail {

WorkerThread::~WorkerThread() noexcept {
    join();
}

#if defined(__linux__)
void* WorkerThread::pthread_trampoline(void* argument) noexcept {
    auto* worker = static_cast<WorkerThread*>(argument);
    worker->entry_(worker->argument_);
    return nullptr;
}
#endif

void WorkerThread::start(Entry entry, void* argument, std::size_t stack_size,
                         bool pin_to_cores, std::size_t worker_index) {
#if defined(__linux__)
    pthread_attr_t attributes;
    pthread_attr_t* attributes_ptr = nullptr;
    const int attr_status = pthread_attr_init(&attributes);
    if (attr_status == 0) {
        attributes_ptr = &attributes;
        if (stack_size != 0) {
            const auto minimum_stack_size = static_cast<std::size_t>(PTHREAD_STACK_MIN);
            if (stack_size < minimum_stack_size) {
                std::fprintf(stderr,
                             "magpie: worker_stack_size is below PTHREAD_STACK_MIN; using default stack\n");
            } else {
                const int stack_status = pthread_attr_setstacksize(&attributes, stack_size);
                if (stack_status != 0) {
                    std::fprintf(stderr,
                                 "magpie: pthread_attr_setstacksize failed (%d); using default stack\n",
                                 stack_status);
                }
            }
        }
    } else {
        std::fprintf(stderr,
                     "magpie: pthread_attr_init failed (%d); using default worker attributes\n",
                     attr_status);
    }

    cpu_set_t desired_affinity;
    CPU_ZERO(&desired_affinity);
    bool apply_affinity = false;
    if (pin_to_cores) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        const int affinity_status = sched_getaffinity(0, sizeof(allowed), &allowed);
        if (affinity_status == 0) {
            std::array<int, CPU_SETSIZE> cpus{};
            std::size_t cpu_count = 0;
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &allowed)) {
                    cpus[cpu_count++] = cpu;
                }
            }
            if (cpu_count != 0) {
                CPU_SET(cpus[worker_index % cpu_count], &desired_affinity);
                apply_affinity = true;
            } else {
                std::fprintf(stderr, "magpie: no allowed CPU found; worker is unpinned\n");
            }
        } else {
            std::fprintf(stderr, "magpie: sched_getaffinity failed; worker is unpinned\n");
        }
    }

    entry_ = entry;
    argument_ = argument;
    const int create_status = pthread_create(&thread_, attributes_ptr,
                                            &WorkerThread::pthread_trampoline, this);
    if (attributes_ptr != nullptr) {
        const int destroy_status = pthread_attr_destroy(&attributes);
        if (destroy_status != 0) {
            std::fprintf(stderr, "magpie: pthread_attr_destroy failed (%d)\n", destroy_status);
        }
    }
    if (create_status != 0) {
        throw std::system_error(create_status, std::generic_category(), "pthread_create");
    }
    started_ = true;
    if (apply_affinity) {
        const int affinity_status = pthread_setaffinity_np(
            thread_, sizeof(desired_affinity), &desired_affinity);
        if (affinity_status != 0) {
            std::fprintf(stderr,
                         "magpie: pthread_setaffinity_np failed (%d); worker is unpinned\n",
                         affinity_status);
        }
    }
#else
    if (stack_size != 0) {
        std::fprintf(stderr,
                     "magpie: generic std::thread backend ignores worker_stack_size\n");
    }
    if (pin_to_cores) {
        std::fprintf(stderr,
                     "magpie: generic std::thread backend ignores pin_to_cores\n");
    }
    thread_ = std::thread(entry, argument);
    (void)worker_index;
#endif
}

void WorkerThread::join() noexcept {
#if defined(__linux__)
    if (started_) {
        const int status = pthread_join(thread_, nullptr);
        if (status != 0) {
            std::fprintf(stderr, "magpie: pthread_join failed (%d)\n", status);
            std::terminate();
        }
        started_ = false;
    }
#else
    if (thread_.joinable()) {
        try {
            thread_.join();
        } catch (...) {
            std::fprintf(stderr, "magpie: std::thread::join failed\n");
            std::terminate();
        }
    }
#endif
}

}  // namespace magpie::detail
