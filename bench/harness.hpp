#pragma once

#include <magpie/thread_pool.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace magpie::bench {

using Clock = std::chrono::steady_clock;

struct Config {
    std::string program = "bench_empty_task";
    std::string mode = "pool";
    std::string payload = "empty";
    std::string rejection = "abort";
    std::string config_id = "direct-unhashed";
    std::size_t workers = 1;
    std::size_t producers = 1;
    std::size_t capacity = 4096;
    double warmup = 2;
    double duration = 10;
    double arrival_rate = 0;
    std::uint64_t seed = 20260929;
    std::size_t work = 64;
    std::size_t sample_stride = 1024;
    std::size_t sample_capacity = 65536;
    std::size_t depth = 4;
    std::size_t fanout = 2;
    std::size_t task_count = 1000;
    double sleep_ms = 10;
    std::vector<int> worker_cpus;
    std::vector<int> producer_cpus;
};

struct Sample {
    std::uint64_t latency_ns = 0;
    std::uint64_t cpu_ns = 0;
    std::uint64_t arrival_lag_ns = 0;
    bool inline_execution = false;
    bool ready = false;
};

// Quantiles report bucket upper bounds, not exact timestamp quantiles.
struct Histogram {
    std::array<std::uint64_t, 64> buckets{};
    std::uint64_t underflow = 0;
    std::uint64_t overflow = 0;
    std::uint64_t count = 0;
    long double sum_ns = 0;
    void add(std::uint64_t ns);
    [[nodiscard]] double quantile(double fraction) const;
};

struct Result {
    ThreadPool::Stats stats;
    bool has_pool_stats = false;
    double window_seconds = 0;
    double elapsed_seconds = 0;
    double process_cpu_seconds = 0;
    double submit_cpu_seconds = 0;
    double task_cpu_seconds = 0;
    double shutdown_seconds = 0;
    double drain_seconds = 0;
    double join_seconds = 0;
    double shutdown_cpu_seconds = 0;
    double drain_cpu_seconds = 0;
    double join_cpu_seconds = 0;
    std::uint64_t attempts = 0;
    std::uint64_t accepted = 0;
    std::uint64_t refused = 0;
    std::uint64_t cross_thread_destroyed = 0;
    std::uint64_t producer_destroyed = 0;
    std::uint64_t sample_dropped = 0;
    std::size_t peak_threads_bound = 0;
    Histogram arrival_lag;
    Histogram total_latency;
    Histogram worker_latency;
    Histogram inline_latency;
};

[[nodiscard]] Config parse_config(int argc, char** argv);
[[nodiscard]] double seconds(Clock::duration duration);
[[nodiscard]] double process_cpu();
[[nodiscard]] double thread_cpu();
[[nodiscard]] std::vector<int> current_cpus();
void set_cpus(const std::vector<int>& cpus);
void pin_current(const std::vector<int>& cpus, std::size_t index);
[[nodiscard]] ThreadPoolOptions pool_options(const Config& config);
void check_identity(const ThreadPool::Stats& stats);
void collect_samples(Result& result, const std::vector<std::vector<Sample>>& samples);
void print_result(const Config& config, const Result& result);
[[nodiscard]] Result run(const Config& config, double duration, bool measured);

} // namespace magpie::bench
