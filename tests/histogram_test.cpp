#include "latency/histogram.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

using latency::Histogram;

TEST_CASE("Nanosecond log2 buckets cover zero, powers of two, and uint64 limits") {
    REQUIRE(latency::bucket_index(0) == 0);
    REQUIRE(latency::bucket_index(1) == 0);
    REQUIRE(latency::bucket_upper_bound(0) == 1);
    for (std::size_t i = 1; i < latency::histogram_buckets; ++i) {
        const auto lower = std::uint64_t{1} << i;
        REQUIRE(latency::bucket_index(lower - 1) == i - 1);
        REQUIRE(latency::bucket_index(lower) == i);
        REQUIRE(latency::bucket_index(latency::bucket_upper_bound(i)) == i);
    }
    REQUIRE(latency::bucket_upper_bound(63) == std::numeric_limits<std::uint64_t>::max());
    REQUIRE_THROWS_AS(latency::bucket_upper_bound(64), std::out_of_range);
}

TEST_CASE("Empty histograms have no invented percentiles") {
    const Histogram histogram;
    REQUIRE(histogram.bucket_count() == 0);
    REQUIRE_FALSE(histogram.percentile_upper_bound(0));
    REQUIRE_FALSE(histogram.percentile_upper_bound(50));
    REQUIRE_FALSE(histogram.percentile_upper_bound(100));
    REQUIRE_THROWS_AS(histogram.percentile_upper_bound(101), std::invalid_argument);
    REQUIRE(latency::merge_histograms({}).samples == 0);
}

TEST_CASE("Percentiles report bucket bounds and nearest ranks without interpolation") {
    Histogram histogram;
    for (std::uint64_t i = 0; i < 100; ++i) histogram.observe(i);
    REQUIRE(histogram.samples == 100);
    REQUIRE(histogram.total_ns == 4950);
    REQUIRE(histogram.max_ns == 99);
    REQUIRE(histogram.percentile_upper_bound(0) == 1);
    REQUIRE(histogram.percentile_upper_bound(7) == 7);
    REQUIRE(histogram.percentile_upper_bound(50) == 63);
    REQUIRE(histogram.percentile_upper_bound(95) == 127);
    REQUIRE(histogram.percentile_upper_bound(99) == 127);
    REQUIRE(histogram.percentile_upper_bound(100) == 127);

    Histogram three;
    three.observe(1);
    three.observe(8);
    three.observe(32);
    REQUIRE(three.percentile_upper_bound(33) == 1);
    REQUIRE(three.percentile_upper_bound(34) == 15);
    REQUIRE(three.percentile_upper_bound(99) == 63);
}

TEST_CASE("Per-CPU merging is equivalent to observing the same event stream") {
    std::array<Histogram, 8> cpus;
    Histogram reference;
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const auto value = (i * 7919) % 100000;
        cpus[i % cpus.size()].observe(value);
        reference.observe(value);
    }
    const auto merged = latency::merge_histograms(cpus);
    REQUIRE(merged.buckets == reference.buckets);
    REQUIRE(merged.samples == reference.samples);
    REQUIRE(merged.total_ns == reference.total_ns);
    REQUIRE(merged.max_ns == reference.max_ns);
    REQUIRE(merged.percentile_upper_bound(99) == reference.percentile_upper_bound(99));
    REQUIRE_FALSE(merged.saturated);
}

TEST_CASE("Cumulative differences preserve events without clearing counters") {
    Histogram previous;
    previous.observe(5);
    previous.observe(31);
    auto current = previous;
    current.observe(9);
    current.observe(1000);
    const auto delta = latency::histogram_delta(current, previous);
    REQUIRE_FALSE(delta.reset);
    REQUIRE(delta.histogram.samples == 2);
    REQUIRE(delta.histogram.total_ns == 1009);
    REQUIRE(delta.histogram.buckets[3] == 1);
    REQUIRE(delta.histogram.buckets[9] == 1);
    REQUIRE(delta.histogram.max_ns == 0);
    REQUIRE(delta.histogram.percentile_upper_bound(99) == 1023);
    REQUIRE(latency::histogram_delta(current, current).histogram.samples == 0);
}

TEST_CASE("Counter regression detects restarts, sum wrap, and individual bucket resets") {
    Histogram previous;
    previous.observe(100);
    previous.observe(1000);

    SECTION("Complete restart") {
        Histogram current;
        current.observe(1);
        const auto delta = latency::histogram_delta(current, previous);
        REQUIRE(delta.reset);
        REQUIRE(delta.histogram.buckets == current.buckets);
        REQUIRE(delta.histogram.samples == 1);
        REQUIRE(delta.histogram.total_ns == 1);
        REQUIRE(delta.histogram.max_ns == 0);
    }
    SECTION("Reset in one bucket despite rising sample and duration totals") {
        auto current = previous;
        current.buckets[6] = 0;
        current.observe(1'000'000);
        const auto delta = latency::histogram_delta(current, previous);
        REQUIRE(delta.reset);
        REQUIRE(delta.histogram.buckets == current.buckets);
    }
    SECTION("Duration sum wrap without a bucket reset") {
        auto current = previous;
        current.total_ns = 10;
        REQUIRE(latency::histogram_delta(current, previous).reset);
    }
}

TEST_CASE("Saturating aggregation and percentile ranks never wrap uint64") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    Histogram histogram;
    histogram.buckets[0] = maximum - 1;
    histogram.samples = maximum - 1;
    histogram.total_ns = maximum - 1;
    histogram.observe(1);
    REQUIRE_FALSE(histogram.saturated);
    histogram.observe(1);
    REQUIRE(histogram.saturated);
    REQUIRE(histogram.buckets[0] == maximum);
    REQUIRE(histogram.samples == maximum);
    REQUIRE(histogram.total_ns == maximum);
    REQUIRE(histogram.percentile_upper_bound(100) == 1);

    Histogram second;
    second.observe(8);
    histogram.merge(second);
    REQUIRE(histogram.bucket_count() == maximum);
    REQUIRE(histogram.total_ns == maximum);
    REQUIRE(histogram.samples == maximum);
    REQUIRE(histogram.buckets[3] == 1);
    REQUIRE(histogram.percentile_upper_bound(99) == 1);
    REQUIRE(latency::histogram_delta(histogram, histogram).histogram.saturated);

    Histogram sum_only;
    sum_only.observe(maximum);
    sum_only.observe(maximum);
    REQUIRE(sum_only.saturated);
    REQUIRE(sum_only.samples == 2);
    REQUIRE(sum_only.total_ns == maximum);
    REQUIRE(sum_only.percentile_upper_bound(50) == maximum);
}
