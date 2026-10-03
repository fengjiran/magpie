#include "harness.hpp"

#include <magpie/build_info.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

#if defined(__linux__)
#include <sched.h>
#endif

namespace magpie::bench {
namespace {
std::uint64_t integer(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("expected an unsigned integer: " + value);
    }
    std::size_t end = 0;
    const auto result = std::stoull(value, &end);
    if (end != value.size()) {
        throw std::invalid_argument("invalid integer");
    }
    return result;
}
double real(const std::string& value) {
    std::size_t end = 0;
    const auto result = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(result)) {
        throw std::invalid_argument("expected a finite number");
    }
    return result;
}
std::vector<int> cpu_list(const std::string& value) {
    std::vector<int> result;
    std::istringstream input(value);
    std::string part;
    while (std::getline(input, part, ',')) {
        const auto cpu = integer(part);
        if (cpu > 65535) {
            throw std::invalid_argument("CPU id too large");
        }
        if (std::find(result.begin(), result.end(), static_cast<int>(cpu)) != result.end()) {
            throw std::invalid_argument("duplicate CPU id");
        }
        result.push_back(static_cast<int>(cpu));
    }
    if (result.empty()) {
        throw std::invalid_argument("empty CPU set");
    }
    return result;
}
std::string cpus_text(const std::vector<int>& cpus) {
    if (cpus.empty()) {
        return "unbound";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < cpus.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << cpus[i];
    }
    return out.str();
}
double cpu_clock(clockid_t id) {
    timespec time{};
    if (clock_gettime(id, &time) != 0) {
        throw std::runtime_error("CPU clock failed");
    }
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}
} // namespace

Config parse_config(int argc, char** argv) {
    Config c;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout
            << "magpie_bench --program NAME --mode pool|queue|wrap|same|cross|async|thread\n"
               "  --workers N --producers N --capacity N --payload empty|cpu|memory|atomic|sleep\n"
               "  --warmup SEC --duration SEC --arrival-rate TASKS_PER_SEC --seed N\n"
               "  --rejection abort|caller-runs --work N --sample-stride N --sample-capacity N\n"
               "  --worker-cpus ID,ID --producer-cpus ID,ID (Linux only)\n"
               "  --depth N --fanout N --task-count N --sleep-ms N --config-id TEXT\n";
        std::exit(0);
    }
    if ((argc - 1) % 2 != 0) {
        throw std::invalid_argument("arguments must be key/value pairs");
    }
    for (int i = 1; i < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--program") {
            c.program = value;
        } else if (key == "--mode") {
            c.mode = value;
        } else if (key == "--payload") {
            c.payload = value;
        } else if (key == "--rejection") {
            c.rejection = value;
        } else if (key == "--config-id") {
            c.config_id = value;
        } else if (key == "--workers") {
            c.workers = integer(value);
        } else if (key == "--producers") {
            c.producers = integer(value);
        } else if (key == "--capacity") {
            c.capacity = integer(value);
        } else if (key == "--work") {
            c.work = integer(value);
        } else if (key == "--seed") {
            c.seed = integer(value);
        } else if (key == "--sample-stride") {
            c.sample_stride = integer(value);
        } else if (key == "--sample-capacity") {
            c.sample_capacity = integer(value);
        } else if (key == "--depth") {
            c.depth = integer(value);
        } else if (key == "--fanout") {
            c.fanout = integer(value);
        } else if (key == "--task-count") {
            c.task_count = integer(value);
        } else if (key == "--warmup") {
            c.warmup = real(value);
        } else if (key == "--duration") {
            c.duration = real(value);
        } else if (key == "--arrival-rate") {
            c.arrival_rate = real(value);
        } else if (key == "--sleep-ms") {
            c.sleep_ms = real(value);
        } else if (key == "--worker-cpus") {
            c.worker_cpus = cpu_list(value);
        } else if (key == "--producer-cpus") {
            c.producer_cpus = cpu_list(value);
        } else {
            throw std::invalid_argument("unknown argument: " + key);
        }
    }
    const std::vector<std::string> programs = {
        "bench_empty_task", "bench_submit_path",   "bench_producer_scaling", "bench_fine_grain",
        "bench_burst",      "bench_latency",       "bench_idle_cpu",         "bench_baselines",
        "bench_alloc_cost", "bench_shutdown_drain"};
    if (std::find(programs.begin(), programs.end(), c.program) == programs.end()) {
        throw std::invalid_argument("unknown or M1-inapplicable program: " + c.program);
    }
    if (c.workers == 0 || c.workers > 256 || c.producers == 0 || c.producers > 256 ||
        c.capacity < 16 || c.capacity > (1U << 24) || (c.capacity & (c.capacity - 1)) != 0 ||
        c.duration <= 0 || c.duration > 3600 || c.warmup < 0 || c.warmup > 3600 ||
        c.arrival_rate < 0 || c.arrival_rate > 1e9 || c.sample_stride == 0 ||
        c.sample_capacity == 0 || c.sample_capacity > 1'000'000 || c.work > 1'000'000 ||
        c.depth > 16 || c.fanout == 0 || c.fanout > 4 || c.task_count == 0 ||
        c.task_count > 1'000'000 || c.sleep_ms < 0 || c.sleep_ms > 1000) {
        throw std::invalid_argument("configuration outside benchmark bounds");
    }
    if (c.rejection != "abort" && c.rejection != "caller-runs") {
        throw std::invalid_argument("benchmark rejection must be abort or caller-runs");
    }
    if (c.config_id.find_first_of("\t\r\n") != std::string::npos) {
        throw std::invalid_argument("invalid config id");
    }
    const std::vector<std::string> payloads = {"empty", "cpu", "memory", "atomic", "sleep"};
    if (std::find(payloads.begin(), payloads.end(), c.payload) == payloads.end()) {
        throw std::invalid_argument("unknown payload");
    }
    std::vector<std::string> modes{"pool"};
    if (c.program == "bench_submit_path") {
        modes = {"pool", "queue", "wrap"};
    }
    if (c.program == "bench_alloc_cost") {
        modes = {"same", "cross"};
    }
    if (c.program == "bench_baselines") {
        modes = {"pool", "async", "thread"};
    }
    if (std::find(modes.begin(), modes.end(), c.mode) == modes.end()) {
        throw std::invalid_argument("mode does not apply to this program");
    }
    if (c.program == "bench_empty_task" && (c.payload != "empty" || c.rejection != "abort")) {
        throw std::invalid_argument("empty-task requires empty payload and Abort");
    }
    if ((c.mode == "queue" || c.mode == "cross") && c.workers != 1) {
        throw std::invalid_argument("controlled queue requires exactly one consumer");
    }
    if ((c.program == "bench_burst" || c.program == "bench_shutdown_drain" ||
         c.program == "bench_idle_cpu" || c.program == "bench_baselines") &&
        c.producers != 1) {
        throw std::invalid_argument("this workload has one driver");
    }
    if (c.program == "bench_latency" && c.arrival_rate <= 0) {
        throw std::invalid_argument("latency requires a fixed positive arrival rate");
    }
    if (c.program == "bench_baselines" && c.capacity < c.workers) {
        throw std::invalid_argument("baseline batch requires capacity >= workers");
    }
    if (c.program == "bench_shutdown_drain" && c.capacity < c.task_count) {
        throw std::invalid_argument("shutdown fixture needs capacity >= task-count");
    }
    if (c.arrival_rate > 0 && c.mode != "pool") {
        throw std::invalid_argument("arrival-rate applies only to pool submission");
    }
    if (c.program == "bench_burst") {
        std::uint64_t nodes = 1;
        std::uint64_t level = 1;
        for (std::size_t depth = 0; depth < c.depth; ++depth) {
            level *= c.fanout;
            nodes += level;
            if (nodes > 1'000'000) {
                throw std::invalid_argument("tree exceeds one million nodes");
            }
        }
    }
    if (c.worker_cpus.empty() != c.producer_cpus.empty()) {
        throw std::invalid_argument("specify both worker and producer CPU sets");
    }
    const auto allowed = current_cpus();
    for (const auto* cpus : {&c.worker_cpus, &c.producer_cpus}) {
        for (int cpu : *cpus) {
            if (std::find(allowed.begin(), allowed.end(), cpu) == allowed.end()) {
                throw std::invalid_argument("CPU is outside allowed set");
            }
        }
    }
    return c;
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
double process_cpu() { return cpu_clock(CLOCK_PROCESS_CPUTIME_ID); }
double thread_cpu() { return cpu_clock(CLOCK_THREAD_CPUTIME_ID); }
std::vector<int> current_cpus() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        throw std::runtime_error("sched_getaffinity failed");
    }
    std::vector<int> result;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set)) {
            result.push_back(i);
        }
    }
    return result;
#else
    return {};
#endif
}
void set_cpus(const std::vector<int>& cpus) {
    if (cpus.empty()) {
        return;
    }
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : cpus) {
        if (cpu < 0 || cpu >= CPU_SETSIZE) {
            throw std::invalid_argument("CPU exceeds cpu_set_t");
        }
        CPU_SET(cpu, &set);
    }
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        throw std::runtime_error("sched_setaffinity failed");
    }
#else
    throw std::invalid_argument("explicit affinity requires Linux");
#endif
}
void pin_current(const std::vector<int>& cpus, std::size_t index) {
    if (!cpus.empty()) {
        set_cpus({cpus[index % cpus.size()]});
    }
}
ThreadPoolOptions pool_options(const Config& c) {
    ThreadPoolOptions options;
    options.worker_count = c.workers;
    options.global_queue_capacity = c.capacity;
    options.local_deque_capacity = 16;
    options.pin_to_cores = !c.worker_cpus.empty();
    options.rejection =
        c.rejection == "abort" ? RejectionPolicy::Abort : RejectionPolicy::CallerRuns;
    options.exception_handler = [](std::exception_ptr) { std::terminate(); };
    return options;
}
void check_identity(const ThreadPool::Stats& s) {
    if (s.pending != 0 || s.submitted != s.completed + s.discarded ||
        s.completed != s.worker_completed + s.inline_completed) {
        throw std::runtime_error("quiescent pool accounting failed");
    }
}
void Histogram::add(std::uint64_t ns) {
    ++count;
    sum_ns += ns;
    const auto us = static_cast<double>(ns) / 1000;
    if (us < 1) {
        ++underflow;
        ++buckets[0];
        return;
    }
    if (us >= 1e6) {
        ++overflow;
        return;
    }
    const auto index = static_cast<std::size_t>(std::log(us) / std::log(1e6) * 64);
    ++buckets[std::min(index, std::size_t{63})];
}
double Histogram::quantile(double fraction) const {
    if (count == 0) {
        return -1;
    }
    const auto target =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(count) * fraction));
    std::uint64_t accumulated = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        accumulated += buckets[i];
        if (accumulated >= target) {
            return std::pow(1e6, static_cast<double>(i + 1) / 64);
        }
    }
    return std::numeric_limits<double>::infinity();
}
void collect_samples(Result& r, const std::vector<std::vector<Sample>>& records) {
    for (const auto& producer : records) {
        for (const auto& sample : producer) {
            if (!sample.ready) {
                continue;
            }
            r.total_latency.add(sample.latency_ns);
            r.arrival_lag.add(sample.arrival_lag_ns);
            (sample.inline_execution ? r.inline_latency : r.worker_latency).add(sample.latency_ns);
            r.task_cpu_seconds += static_cast<double>(sample.cpu_ns) * 1e-9;
        }
    }
}
void print_result(const Config& c, const Result& r) {
    std::vector<std::pair<std::string, std::string>> fields;
    auto put = [&fields](std::string key, const auto& value) {
        std::ostringstream out;
        out << std::setprecision(12) << value;
        fields.emplace_back(std::move(key), out.str());
    };
    const auto info = build_info();
    put("program", c.program);
    put("mode", c.mode);
    put("config_hash", c.config_id);
    const bool execution = c.mode == "pool" || c.mode == "async" || c.mode == "thread";
    const bool ring = c.mode == "pool" || c.mode == "queue" || c.mode == "cross";
    put("backend", c.mode == "pool" ? (std::string(info.platform) == "Linux" ? "pthread-mutex-cv"
                                                                             : "generic-mutex-cv")
                                    : c.mode);
    put("window_kind", c.program == "bench_shutdown_drain" ? "finite-inventory" : "fixed-time");
    put("latency_definition",
        c.program == "bench_shutdown_drain" ? "task-body-wall" : "submit-before-wrap-to-body-end");
    put("compiler", info.compiler);
    put("build_type", info.build_type);
    put("sanitizer", info.sanitizer);
    put("cache_line", info.cache_line_size);
    put("producer_count", c.producers);
    put("worker_count", c.workers);
    put("producer_cpus", cpus_text(c.producer_cpus));
    put("worker_cpus", cpus_text(c.worker_cpus));
    put("global_slots", ring ? c.capacity : 0);
    put("local_slots", 0);
    put("total_slots", ring ? c.capacity : 0);
    put("payload", c.program == "bench_shutdown_drain" ? "sleep-inventory" : c.payload);
    put("work", c.work);
    put("seed", c.seed);
    put("rejection", c.rejection);
    put("warmup_s", c.warmup);
    put("requested_duration_s", c.duration);
    put("arrival_rate", c.arrival_rate);
    put("window_s", r.window_seconds);
    put("elapsed_s", r.elapsed_seconds);
    put("process_cpu_s", r.process_cpu_seconds);
    put("submit_cpu_s", r.submit_cpu_seconds);
    put("task_cpu_s", r.task_cpu_seconds);
    put("task_cpu_scope", c.program == "bench_shutdown_drain" ? "all-task-bodies" : "not-sampled");
    put("throughput_boundary", "measured-start-to-quiescence");
    const auto total = r.has_pool_stats ? r.stats.completed : r.accepted;
    const auto worker = r.has_pool_stats ? r.stats.worker_completed : r.accepted;
    const auto inlined = r.has_pool_stats ? r.stats.inline_completed : 0;
    put("worker_ops_s", execution ? static_cast<double>(worker) / r.elapsed_seconds : -1);
    put("inline_ops_s", static_cast<double>(inlined) / r.elapsed_seconds);
    put("total_ops_s", static_cast<double>(total) / r.elapsed_seconds);
    put("inline_ratio",
        total == 0 ? 0.0 : static_cast<double>(inlined) / static_cast<double>(total));
    put("idle_cpu_pct_per_worker",
        c.program == "bench_idle_cpu"
            ? 100 * r.process_cpu_seconds / r.elapsed_seconds / static_cast<double>(c.workers)
            : -1);
    const auto scheduled =
        c.arrival_rate > 0 ? static_cast<std::uint64_t>(std::ceil(c.arrival_rate * c.duration)) : 0;
    put("scheduled_arrivals", scheduled);
    put("missed_arrivals", scheduled > r.attempts ? scheduled - r.attempts : 0);
    put("attempts", r.attempts);
    put("accepted", r.accepted);
    put("refused", r.refused);
    put("cross_thread_destroyed", r.cross_thread_destroyed);
    put("producer_destroyed", r.producer_destroyed);
    put("peak_threads_bound", r.peak_threads_bound);
    put("sample_stride", c.sample_stride);
    put("sample_capacity_per_producer", c.sample_capacity);
    put("sample_dropped", r.sample_dropped);
    for (const auto& [prefix, histogram] : std::array<std::pair<const char*, const Histogram*>, 4>{
             {{"arrival_lag_", &r.arrival_lag},
              {"", &r.total_latency},
              {"worker_", &r.worker_latency},
              {"inline_", &r.inline_latency}}}) {
        put(std::string(prefix) + "samples", histogram->count);
        put(std::string(prefix) + "avg_us",
            histogram->count == 0
                ? -1.0
                : static_cast<double>(histogram->sum_ns / histogram->count / 1000));
        put(std::string(prefix) + "p50_us_upper", histogram->quantile(.5));
        put(std::string(prefix) + "p99_us_upper", histogram->quantile(.99));
        put(std::string(prefix) + "p999_us_upper", histogram->quantile(.999));
        put(std::string(prefix) + "hist_underflow", histogram->underflow);
        put(std::string(prefix) + "hist_overflow", histogram->overflow);
        for (std::size_t i = 0; i < 64; ++i) {
            put(std::string(prefix) + "bucket_" + std::to_string(i), histogram->buckets[i]);
        }
    }
    put("shutdown_s", r.shutdown_seconds);
    put("drain_s", r.drain_seconds);
    put("join_s", r.join_seconds);
    put("shutdown_cpu_s", r.shutdown_cpu_seconds);
    put("drain_cpu_s", r.drain_cpu_seconds);
    put("join_cpu_s", r.join_cpu_seconds);
    put("depth", c.depth);
    put("fanout", c.fanout);
    put("task_count", c.task_count);
    put("sleep_ms", c.sleep_ms);
    put("stats_scope",
        r.has_pool_stats
            ? "producers-joined-shutdown-drain;completion-totals-stable;wakes-may-finish-later"
            : "not-a-pool");
    const auto& s = r.stats;
    put("submitted", s.submitted);
    put("completed", s.completed);
    put("worker_completed", s.worker_completed);
    put("inline_completed", s.inline_completed);
    put("rejected", s.rejected);
    put("discarded", s.discarded);
    put("pending", s.pending);
    put("stolen", s.stolen);
    put("local_spills", s.local_spills);
    put("wakes", s.wakes);
    put("wake_threads", s.wake_threads ? std::to_string(*s.wake_threads) : "NA");
    put("post_wake_hit", s.post_wake_hit);
    put("post_wake_empty", s.post_wake_empty);
    put("pre_sleep_scan_hit", s.pre_sleep_scan_hit);
    put("pre_sleep_scan_empty", s.pre_sleep_scan_empty);
    put("notify_scan_slots", s.notify_scan_slots);
    put("protocol_attribution",
        r.has_pool_stats ? "gate,pending,submitted,alloc:present;epoch,WaitSlot,slot-seq:NA"
                         : "isolated-control;pool-protocol:NA");
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) {
            std::cout << '\t';
        }
        std::cout << fields[i].first;
    }
    std::cout << '\n';
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) {
            std::cout << '\t';
        }
        std::cout << fields[i].second;
    }
    std::cout << '\n';
}
} // namespace magpie::bench
