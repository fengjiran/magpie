#include "harness.hpp"
#include <magpie/mpmc_queue.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace magpie::bench {
namespace {
thread_local bool producer_execution = false;

void keep_result(std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r"(value) : "memory");
#else
    volatile auto observed = value;
    (void)observed;
#endif
}
struct EmptyCallable {
    void operator()() const noexcept {}
};
struct Payload {
    enum class Kind { Empty, Cpu, Memory, Atomic, Sleep };
    static Kind classify(const std::string& name) {
        if (name == "empty") {
            return Kind::Empty;
        }
        if (name == "cpu") {
            return Kind::Cpu;
        }
        if (name == "memory") {
            return Kind::Memory;
        }
        if (name == "atomic") {
            return Kind::Atomic;
        }
        if (name == "sleep") {
            return Kind::Sleep;
        }
        throw std::invalid_argument("unknown payload kind");
    }
    explicit Payload(const Config& configuration)
        : config(configuration), kind(classify(configuration.payload)) {
        auto state = config.seed;
        for (auto& word : memory) {
            state = state * 6364136223846793005ULL + 1;
            word = state;
        }
    }
    void operator()(std::uint64_t id) const {
        if (kind == Kind::Cpu) {
            for (std::size_t i = 0; i < config.work; ++i) {
                id ^= id >> 12;
                id ^= id << 25;
                id ^= id >> 27;
                id *= 2685821657736338717ULL;
            }
            keep_result(id);
        } else if (kind == Kind::Memory) {
            // A 1 KiB immutable cache-resident working set, not a DRAM test.
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < memory.size(); ++i) {
                sum += memory[(i + static_cast<std::size_t>(id & 127)) & 127];
            }
            keep_result(sum);
        } else if (kind == Kind::Atomic) {
            shared_counter.fetch_add(1, std::memory_order_relaxed);
        } else if (kind == Kind::Sleep) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(config.sleep_ms));
        }
    }
    const Config& config;
    const Kind kind;
    std::array<std::uint64_t, 128> memory{};
    mutable std::atomic<std::uint64_t> shared_counter{0};
};

class PoolFixture {
  public:
    explicit PoolFixture(const Config& config) {
        const auto saved = current_cpus();
        set_cpus(config.worker_cpus);
        try {
            pool = std::make_unique<ThreadPool>(pool_options(config));
        } catch (...) {
            set_cpus(saved);
            throw;
        }
        set_cpus(saved);
        pin_current(config.producer_cpus, 0);
    }
    std::unique_ptr<ThreadPool> pool;
};

struct ProducerResult {
    std::uint64_t attempts = 0;
    std::uint64_t accepted = 0;
    std::uint64_t refused = 0;
    std::uint64_t samples = 0;
    std::uint64_t sample_dropped = 0;
    double cpu = 0;
    std::exception_ptr error;
};

void aggregate(Result& r, const std::vector<ProducerResult>& producers) {
    for (const auto& p : producers) {
        if (p.error) {
            std::rethrow_exception(p.error);
        }
        r.attempts += p.attempts;
        r.accepted += p.accepted;
        r.refused += p.refused;
        r.sample_dropped += p.sample_dropped;
        r.submit_cpu_seconds += p.cpu;
    }
}

void finish_pool(Result& r, PoolFixture& fixture, Clock::time_point begin, double cpu_begin) {
    fixture.pool->shutdown();
    fixture.pool->drain();
    r.elapsed_seconds = seconds(Clock::now() - begin);
    r.process_cpu_seconds = process_cpu() - cpu_begin;
    r.stats = fixture.pool->stats();
    r.has_pool_stats = true;
    check_identity(r.stats);
}

Result pool_run(const Config& c, double duration, bool measured) {
    PoolFixture fixture(c);
    Payload payload(c);
    std::vector<ProducerResult> producers(c.producers);
    std::vector<std::vector<Sample>> samples(c.producers);
    if (measured) {
        for (auto& records : samples) {
            records.resize(c.sample_capacity);
        }
    }
    std::latch ready(static_cast<std::ptrdiff_t>(c.producers));
    std::latch start(1);
    Clock::time_point begin{};
    Clock::time_point deadline{};
    std::vector<std::thread> threads;
    threads.reserve(c.producers);
    try {
        for (std::size_t producer = 0; producer < c.producers; ++producer) {
            threads.emplace_back([&, producer] {
                ProducerResult counters;
                try {
                    pin_current(c.producer_cpus, producer);
                } catch (...) {
                    counters.error = std::current_exception();
                }
                ready.count_down();
                start.wait();
                if (counters.error) {
                    producers[producer] = counters;
                    return;
                }
                producer_execution = true;
                try {
                    const auto cpu_start = thread_cpu();
                    std::uint64_t id = c.seed + producer;
                    while (Clock::now() < deadline) {
                        auto arrival = Clock::time_point{};
                        if (c.arrival_rate > 0) {
                            // Absolute deadlines avoid coordinated omission:
                            // late arrivals are submitted immediately, never rescheduled from now.
                            const double offset =
                                static_cast<double>(counters.attempts * c.producers + producer) /
                                c.arrival_rate;
                            arrival = begin + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>(offset));
                            if (arrival >= deadline) {
                                break;
                            }
                            std::this_thread::sleep_until(arrival);
                        }
                        Sample* sample = nullptr;
                        if (measured && counters.attempts % c.sample_stride == 0) {
                            if (counters.samples < c.sample_capacity) {
                                sample = &samples[producer][counters.samples++];
                            } else {
                                ++counters.sample_dropped;
                            }
                        }
                        const auto submitted_at =
                            sample != nullptr ? Clock::now() : Clock::time_point{};
                        if (sample != nullptr && c.arrival_rate > 0) {
                            sample->arrival_lag_ns = static_cast<std::uint64_t>(
                                std::max(std::int64_t{0},
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             submitted_at - arrival)
                                             .count()));
                        }
                        ++counters.attempts;
                        ++id;
                        try {
                            if (payload.kind == Payload::Kind::Empty && sample == nullptr) {
                                // Unsampled empty tasks are actual empty callables;
                                // payload dispatch/clock work must not become their workload.
                                fixture.pool->submit(EmptyCallable{});
                            } else {
                                fixture.pool->submit([&, id, sample, submitted_at] {
                                    payload(id);
                                    if (sample != nullptr) {
                                        sample->latency_ns = static_cast<std::uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                Clock::now() - submitted_at)
                                                .count());
                                        sample->inline_execution = producer_execution;
                                        sample->ready = true;
                                    }
                                });
                            }
                            ++counters.accepted;
                        } catch (const QueueFullError& error) {
                            if (error.reason != QueueFullReason::QueueFull) {
                                throw;
                            }
                            ++counters.refused;
                        }
                    }
                    counters.cpu = thread_cpu() - cpu_start;
                } catch (...) {
                    counters.error = std::current_exception();
                }
                producer_execution = false;
                // Publish once at thread exit; neighboring producers must not
                // repeatedly write adjacent heap counters in the measured loop.
                producers[producer] = counters;
            });
        }
    } catch (...) {
        deadline = Clock::now();
        start.count_down();
        for (auto& thread : threads) {
            thread.join();
        }
        throw;
    }
    ready.wait();
    begin = Clock::now();
    deadline = begin +
               std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(duration));
    const auto cpu_begin = process_cpu();
    start.count_down();
    for (auto& thread : threads) {
        thread.join();
    }
    Result r;
    r.window_seconds = seconds(Clock::now() - begin);
    finish_pool(r, fixture, begin, cpu_begin);
    aggregate(r, producers);
    if (r.accepted != r.stats.submitted || r.refused + r.accepted != r.attempts) {
        throw std::runtime_error("producer/pool totals disagree");
    }
    collect_samples(r, samples);
    r.peak_threads_bound = 1 + c.workers + c.producers;
    return r;
}

// This fixture isolates pointer-ring and cross-thread wrapping costs. It is
// deliberately separate from the private production queue; it has no gate,
// pending counter, notifications or user execution.
class ControlledRing {
  public:
    explicit ControlledRing(std::size_t capacity) : slots(capacity) {}
    bool push_borrowed(Task* value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (size == slots.size()) {
            return false;
        }
        enqueue(value);
        return true;
    }
    bool push_owned(std::unique_ptr<Task>& owner) {
        std::lock_guard<std::mutex> lock(mutex);
        if (size == slots.size()) {
            return false;
        }
        // Transfer before unlock: the consumer can destroy immediately after
        // publication, while the producer must no longer hold an owning pointer.
        enqueue(owner.release());
        return true;
    }
    bool pop(Task*& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (size == 0) {
            return false;
        }
        value = slots[head];
        slots[head] = nullptr;
        head = (head + 1) % slots.size();
        --size;
        return true;
    }

  private:
    void enqueue(Task* value) noexcept {
        slots[tail] = value;
        tail = (tail + 1) % slots.size();
        ++size;
    }
    std::mutex mutex;
    std::vector<Task*> slots;
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t size = 0;
};

Result controlled_run(const Config& c, double duration) {
    pin_current(c.producer_cpus, 0);
    const bool cross = c.mode == "cross";
    const bool mpmc = c.mode == "mpmc";
    const bool queue = c.mode == "queue" || mpmc || cross;
    ControlledRing ring(c.capacity);
    magpie::MPMCQueue<Task*> atomic_ring(c.capacity);
    auto pop = [&](Task*& value) { return mpmc ? atomic_ring.dequeue(value) : ring.pop(value); };
    TaskImpl<EmptyCallable> marker(EmptyCallable{});
    std::vector<ProducerResult> producers(c.producers);
    std::atomic<std::size_t> done{0};
    std::uint64_t consumed = 0;
    std::exception_ptr consumer_error;
    std::latch ready(static_cast<std::ptrdiff_t>(c.producers + (queue ? 1 : 0)));
    std::latch start(1);
    Clock::time_point deadline{};
    std::vector<std::thread> threads;
    threads.reserve(c.producers + 1);
    try {
        if (queue) {
            threads.emplace_back([&] {
                try {
                    pin_current(c.worker_cpus, 0);
                } catch (...) {
                    consumer_error = std::current_exception();
                }
                ready.count_down();
                start.wait();
                for (;;) {
                    Task* task = nullptr;
                    if (pop(task)) {
                        if (cross) {
                            delete task;
                        } else {
                            keep_result(reinterpret_cast<std::uintptr_t>(task));
                        }
                        ++consumed;
                    } else if (done.load(std::memory_order_acquire) == c.producers) {
                        // The last producer may have pushed after the empty
                        // observation. Acquire the completion flag, then probe again.
                        if (!pop(task)) {
                            break;
                        }
                        if (cross) {
                            delete task;
                        } else {
                            keep_result(reinterpret_cast<std::uintptr_t>(task));
                        }
                        ++consumed;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::size_t producer = 0; producer < c.producers; ++producer) {
            threads.emplace_back([&, producer] {
                ProducerResult p;
                try {
                    pin_current(c.producer_cpus, producer);
                } catch (...) {
                    p.error = std::current_exception();
                }
                ready.count_down();
                start.wait();
                if (!p.error) {
                    try {
                        const auto cpu_start = thread_cpu();
                        while (Clock::now() < deadline) {
                            ++p.attempts;
                            // Queue-only uses a stable non-owning sentinel, never dereferenced.
                            std::unique_ptr<Task> task;
                            if (c.mode != "queue" && !mpmc) {
                                task = std::make_unique<TaskImpl<EmptyCallable>>(EmptyCallable{});
                            }
                            if (!queue) {
                                keep_result(reinterpret_cast<std::uintptr_t>(task.get()));
                                ++p.accepted;
                            } else if (cross ? ring.push_owned(task)
                                             : (mpmc ? atomic_ring.enqueue(&marker) : ring.push_borrowed(&marker))) {
                                ++p.accepted;
                            } else {
                                ++p.refused;
                            }
                        }
                        p.cpu = thread_cpu() - cpu_start;
                    } catch (...) {
                        p.error = std::current_exception();
                    }
                }
                producers[producer] = p;
                done.fetch_add(1, std::memory_order_release);
            });
        }
    } catch (...) {
        const auto launched_producers = threads.size() - (queue && !threads.empty() ? 1 : 0);
        done.store(c.producers - launched_producers, std::memory_order_release);
        deadline = Clock::now();
        start.count_down();
        for (auto& thread : threads) {
            thread.join();
        }
        throw;
    }
    ready.wait();
    const auto begin = Clock::now();
    deadline = begin +
               std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(duration));
    const auto cpu_begin = process_cpu();
    start.count_down();
    for (auto& thread : threads) {
        thread.join();
    }
    Result r;
    r.window_seconds = seconds(Clock::now() - begin);
    r.elapsed_seconds = r.window_seconds;
    r.process_cpu_seconds = process_cpu() - cpu_begin;
    aggregate(r, producers);
    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }
    if (queue && consumed != r.accepted) {
        throw std::runtime_error("controlled queue conservation failed");
    }
    r.cross_thread_destroyed = cross ? consumed : 0;
    r.producer_destroyed = cross ? r.refused : (queue ? 0 : r.accepted);
    r.peak_threads_bound = 1 + c.producers + (queue ? 1 : 0);
    return r;
}

struct SpawnTask {
    ThreadPool* pool;
    const Payload* payload;
    std::size_t remaining;
    std::size_t fanout;
    std::uint64_t id;
    void operator()() const {
        (*payload)(id);
        if (remaining == 0) {
            return;
        }
        for (std::size_t child = 0; child < fanout; ++child) {
            pool->submit(SpawnTask{pool, payload, remaining - 1, fanout, id * fanout + child + 1});
        }
    }
};
Result burst_run(const Config& c, double duration) {
    PoolFixture fixture(c);
    Payload payload(c);
    const auto begin = Clock::now();
    const auto cpu_begin = process_cpu();
    Result r;
    std::uint64_t root = 0;
    do {
        fixture.pool->submit(SpawnTask{fixture.pool.get(), &payload, c.depth, c.fanout, ++root});
        // Children are submitted before their parent completes, so a zero
        // pending observation closes the whole tree. No worker waits on a future.
        fixture.pool->drain();
    } while (seconds(Clock::now() - begin) < duration);
    r.window_seconds = seconds(Clock::now() - begin);
    finish_pool(r, fixture, begin, cpu_begin);
    r.attempts = r.accepted = r.stats.submitted;
    r.peak_threads_bound = 1 + c.workers;
    return r;
}

Result idle_run(const Config& c, double duration) {
    PoolFixture fixture(c);
    const auto begin = Clock::now();
    const auto cpu_begin = process_cpu();
    std::this_thread::sleep_for(std::chrono::duration<double>(duration));
    Result r;
    r.window_seconds = r.elapsed_seconds = seconds(Clock::now() - begin);
    r.process_cpu_seconds = process_cpu() - cpu_begin;
    fixture.pool->shutdown();
    fixture.pool->drain();
    r.stats = fixture.pool->stats();
    r.has_pool_stats = true;
    check_identity(r.stats);
    r.peak_threads_bound = 1 + c.workers;
    return r;
}

Result baseline_run(const Config& c, double duration, bool measured) {
    std::unique_ptr<PoolFixture> fixture;
    if (c.mode == "pool") {
        fixture = std::make_unique<PoolFixture>(c);
    }
    pin_current(c.producer_cpus, 0);
    Payload payload(c);
    std::vector<std::vector<Sample>> samples(1);
    if (measured) {
        samples[0].resize(c.sample_capacity);
    }
    std::vector<std::future<void>> futures;
    std::vector<std::thread> threads;
    futures.reserve(c.workers);
    threads.reserve(c.workers);
    std::vector<std::exception_ptr> errors(c.workers);
    Result r;
    const auto begin = Clock::now();
    const auto cpu_begin = process_cpu();
    std::uint64_t used = 0;
    try {
        do {
            for (std::size_t worker = 0; worker < c.workers; ++worker) {
                Sample* sample = nullptr;
                if (measured && r.attempts % c.sample_stride == 0) {
                    if (used < c.sample_capacity) {
                        sample = &samples[0][used++];
                    } else {
                        ++r.sample_dropped;
                    }
                }
                const auto submitted_at = sample != nullptr ? Clock::now() : Clock::time_point{};
                const auto id = c.seed + r.attempts++;
                auto body = [&, worker, sample, submitted_at, id] {
                    try {
                        if (c.mode != "pool") {
                            pin_current(c.worker_cpus, worker);
                        }
                        payload(id);
                    } catch (...) {
                        errors[worker] = std::current_exception();
                    }
                    if (sample != nullptr) {
                        sample->latency_ns = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                                 submitted_at)
                                .count());
                        sample->ready = true;
                    }
                };
                if (c.mode == "pool") {
                    fixture->pool->submit(body);
                } else if (c.mode == "async") {
                    futures.push_back(std::async(std::launch::async, body));
                } else {
                    try {
                        threads.emplace_back(body);
                    } catch (...) {
                        for (auto& thread : threads) {
                            thread.join();
                        }
                        throw;
                    }
                }
            }
            if (fixture) {
                fixture->pool->drain();
            }
            for (auto& future : futures) {
                future.get();
            }
            for (auto& thread : threads) {
                thread.join();
            }
            futures.clear();
            threads.clear();
            for (const auto& error : errors) {
                if (error) {
                    std::rethrow_exception(error);
                }
            }
            r.accepted += c.workers;
        } while (seconds(Clock::now() - begin) < duration);
    } catch (...) {
        // Every body borrows payload/errors/records. Creation or submission
        // failure must finish existing bodies before those locals unwind.
        const auto original_error = std::current_exception();
        if (fixture) {
            fixture->pool->shutdown();
            fixture->pool->drain();
        }
        for (auto& future : futures) {
            if (future.valid()) {
                future.wait();
            }
        }
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        std::rethrow_exception(original_error);
    }
    r.window_seconds = r.elapsed_seconds = seconds(Clock::now() - begin);
    r.process_cpu_seconds = process_cpu() - cpu_begin;
    if (fixture) {
        finish_pool(r, *fixture, begin, cpu_begin);
    }
    r.peak_threads_bound = c.workers + 1;
    collect_samples(r, samples);
    return r;
}

Result shutdown_run(const Config& c) {
    PoolFixture fixture(c);
    std::vector<std::vector<Sample>> samples(1);
    samples[0].resize(c.task_count);
    std::latch release(1);
    try {
        for (std::size_t i = 0; i < c.task_count; ++i) {
            fixture.pool->submit([&, i] {
                release.wait();
                const auto begin = Clock::now();
                const auto cpu_begin = thread_cpu();
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(c.sleep_ms));
                auto& sample = samples[0][i];
                sample.cpu_ns =
                    static_cast<std::uint64_t>(std::max(0.0, thread_cpu() - cpu_begin) * 1e9);
                sample.latency_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin)
                        .count());
                sample.ready = true;
            });
        }
    } catch (...) {
        release.count_down();
        fixture.pool->shutdown();
        fixture.pool->drain();
        throw;
    }
    Result r;
    const auto begin = Clock::now();
    const auto cpu_begin = process_cpu();
    fixture.pool->shutdown();
    const auto after_stop = Clock::now();
    const auto cpu_stop = process_cpu();
    release.count_down();
    fixture.pool->drain();
    const auto after_drain = Clock::now();
    const auto cpu_drain = process_cpu();
    r.stats = fixture.pool->stats();
    check_identity(r.stats);
    fixture.pool.reset();
    const auto end = Clock::now();
    const auto cpu_end = process_cpu();
    r.has_pool_stats = true;
    r.window_seconds = r.elapsed_seconds = seconds(end - begin);
    r.process_cpu_seconds = cpu_end - cpu_begin;
    r.shutdown_seconds = seconds(after_stop - begin);
    r.drain_seconds = seconds(after_drain - after_stop);
    r.join_seconds = seconds(end - after_drain);
    r.shutdown_cpu_seconds = cpu_stop - cpu_begin;
    r.drain_cpu_seconds = cpu_drain - cpu_stop;
    r.join_cpu_seconds = cpu_end - cpu_drain;
    r.attempts = r.accepted = c.task_count;
    r.peak_threads_bound = 1 + c.workers;
    collect_samples(r, samples);
    // This fixture reports task-body wall time, not submission latency.
    return r;
}
} // namespace

Result run(const Config& c, double duration, bool measured) {
    if (c.program == "bench_idle_cpu") {
        return idle_run(c, duration);
    }
    if (c.program == "bench_burst") {
        return burst_run(c, duration);
    }
    if (c.program == "bench_shutdown_drain") {
        return shutdown_run(c);
    }
    if (c.program == "bench_alloc_cost" || c.mode == "queue" || c.mode == "mpmc" || c.mode == "wrap") {
        return controlled_run(c, duration);
    }
    if (c.program == "bench_baselines") {
        return baseline_run(c, duration, measured);
    }
    return pool_run(c, duration, measured);
}
} // namespace magpie::bench
