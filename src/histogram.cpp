#include "latency/histogram.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace latency {
namespace {

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right,
                            bool& saturated) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (right > maximum - left) {
        saturated = true;
        return maximum;
    }
    return left + right;
}

} // namespace

std::size_t bucket_index(std::uint64_t nanoseconds) noexcept {
    return nanoseconds == 0 ? 0 : static_cast<std::size_t>(std::bit_width(nanoseconds) - 1);
}

std::uint64_t bucket_upper_bound(std::size_t index) {
    if (index >= histogram_buckets) {
        throw std::out_of_range("histogram bucket index must be less than 64");
    }
    if (index == histogram_buckets - 1) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (std::uint64_t{1} << (index + 1)) - 1;
}

void Histogram::observe(std::uint64_t nanoseconds) noexcept {
    auto& bucket = buckets[bucket_index(nanoseconds)];
    bucket = add_saturated(bucket, 1, saturated);
    samples = add_saturated(samples, 1, saturated);
    total_ns = add_saturated(total_ns, nanoseconds, saturated);
    max_ns = std::max(max_ns, nanoseconds);
}

void Histogram::merge(const Histogram& other) noexcept {
    saturated = saturated || other.saturated;
    for (std::size_t i = 0; i < histogram_buckets; ++i) {
        buckets[i] = add_saturated(buckets[i], other.buckets[i], saturated);
    }
    samples = add_saturated(samples, other.samples, saturated);
    total_ns = add_saturated(total_ns, other.total_ns, saturated);
    max_ns = std::max(max_ns, other.max_ns);
}

std::uint64_t Histogram::bucket_count() const noexcept {
    std::uint64_t count = 0;
    bool ignored = false;
    for (const auto bucket : buckets) {
        count = add_saturated(count, bucket, ignored);
    }
    return count;
}

std::optional<std::uint64_t>
Histogram::percentile_upper_bound(unsigned percentile) const {
    if (percentile > 100) {
        throw std::invalid_argument("percentile must be between 0 and 100");
    }
    const auto count = bucket_count();
    if (count == 0) {
        return std::nullopt;
    }
    // Round the rank up without overflowing count * percentile.
    const auto rank = std::max<std::uint64_t>(
        1, (count / 100) * percentile + ((count % 100) * percentile + 99) / 100);
    std::uint64_t cumulative = 0;
    bool ignored = false;
    for (std::size_t i = 0; i < histogram_buckets; ++i) {
        cumulative = add_saturated(cumulative, buckets[i], ignored);
        if (cumulative >= rank) {
            return bucket_upper_bound(i);
        }
    }
    return std::nullopt;
}

Histogram merge_histograms(std::span<const Histogram> histograms) noexcept {
    Histogram merged;
    for (const auto& histogram : histograms) {
        merged.merge(histogram);
    }
    return merged;
}

HistogramDelta histogram_delta(const Histogram& current,
                               const Histogram& previous) noexcept {
    bool reset = current.samples < previous.samples || current.total_ns < previous.total_ns;
    for (std::size_t i = 0; i < histogram_buckets; ++i) {
        reset = reset || current.buckets[i] < previous.buckets[i];
    }
    if (reset) {
        auto result = current;
        result.max_ns = 0;
        return {result, true};
    }
    Histogram result;
    result.saturated = current.saturated || previous.saturated;
    result.samples = current.samples - previous.samples;
    result.total_ns = current.total_ns - previous.total_ns;
    for (std::size_t i = 0; i < histogram_buckets; ++i) {
        result.buckets[i] = current.buckets[i] - previous.buckets[i];
    }
    return {result, false};
}

} // namespace latency
