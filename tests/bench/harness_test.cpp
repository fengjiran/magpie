#include "../../bench/harness.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>

TEST(BenchmarkHistogram, BoundariesAndOverflowRemainVisible) {
    magpie::bench::Histogram histogram;
    histogram.add(0);
    histogram.add(999);
    histogram.add(1000);
    histogram.add(999999999);
    histogram.add(1000000000);
    EXPECT_EQ(histogram.count, 5U);
    EXPECT_EQ(histogram.underflow, 2U);
    EXPECT_EQ(histogram.overflow, 1U);
    EXPECT_EQ(std::accumulate(histogram.buckets.begin(), histogram.buckets.end(), 0ULL) +
                  histogram.overflow,
              histogram.count);
    EXPECT_TRUE(std::isinf(histogram.quantile(.99)));
    EXPECT_GE(histogram.quantile(.5), 1.0);
    EXPECT_LT(histogram.quantile(.5), 2.0);
}

TEST(BenchmarkHistogram, QuantileReportsConservativeBucketUpperBound) {
    magpie::bench::Histogram histogram;
    histogram.add(10000);
    const auto p99 = histogram.quantile(.99);
    EXPECT_GE(p99, 10.0);
    EXPECT_LE(p99, 10.0 * std::pow(1e6, 1.0 / 64));
}

TEST(BenchmarkSamples, OnlyCompletedRecordsEnterExecutionGroups) {
    std::vector<std::vector<magpie::bench::Sample>> samples(2);
    samples[0].resize(3);
    samples[0][0].ready = true;
    samples[0][0].latency_ns = 10000;
    samples[0][1].ready = true;
    samples[0][1].inline_execution = true;
    samples[0][1].latency_ns = 20000;
    // A rejected attempt owns a record but has no completion.
    samples[0][2].latency_ns = 9999999;
    magpie::bench::Result result;
    magpie::bench::collect_samples(result, samples);
    EXPECT_EQ(result.total_latency.count, 2U);
    EXPECT_EQ(result.worker_latency.count, 1U);
    EXPECT_EQ(result.inline_latency.count, 1U);
    EXPECT_EQ(result.total_latency.sum_ns, 30000);
}

TEST(BenchmarkAccounting, InvalidQuiescentTotalsFail) {
    magpie::ThreadPool::Stats stats;
    stats.submitted = 2;
    stats.completed = stats.worker_completed = 2;
    EXPECT_NO_THROW(magpie::bench::check_identity(stats));
    stats.pending = 1;
    EXPECT_THROW(magpie::bench::check_identity(stats), std::runtime_error);
    stats.pending = 0;
    stats.inline_completed = 1;
    EXPECT_THROW(magpie::bench::check_identity(stats), std::runtime_error);
}
