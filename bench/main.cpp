#include "harness.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const auto config = magpie::bench::parse_config(argc, argv);
        // Warmup has its own pool and records; its totals are never measured.
        if (config.warmup > 0) {
            const auto begin = magpie::bench::Clock::now();
            do {
                (void)magpie::bench::run(config, config.warmup, false);
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
