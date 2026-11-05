#include "harness.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        auto config = magpie::bench::parse_config(argc, argv);
        if (config.samples_output.empty()) {
            if (const auto *path = std::getenv("MAGPIE_BENCH_SAMPLES_OUT")) {
                config.samples_output = path;
            }
        }
        // Warmup has its own pool and records; its totals are never measured.
        if (config.warmup > 0) {
            auto warmup = config;
            warmup.samples_output.clear();
            const auto begin = magpie::bench::Clock::now();
            do {
                (void)magpie::bench::run(warmup, config.warmup, false);
            } while (magpie::bench::seconds(magpie::bench::Clock::now() - begin) < config.warmup);
        }
        const auto result = magpie::bench::run(config, config.duration, true);
        magpie::bench::print_result(config, result);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
