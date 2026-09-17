#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace latency {

inline constexpr std::size_t histogram_buckets = 64;

[[nodiscard]] std::size_t bucket_index(std::uint64_t nanoseconds) noexcept;
[[nodiscard]] std::uint64_t bucket_upper_bound(std::size_t index);

struct Histogram {
    std::array<std::uint64_t, histogram_buckets> buckets{};
    std::uint64_t samples{};
    std::uint64_t total_ns{};
    std::uint64_t max_ns{};
    bool saturated{};

    void observe(std::uint64_t nanoseconds) noexcept;
    void merge(const Histogram& other) noexcept;
    [[nodiscard]] std::uint64_t bucket_count() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    percentile_upper_bound(unsigned percentile) const;
};

[[nodiscard]] Histogram merge_histograms(std::span<const Histogram> histograms) noexcept;

struct HistogramDelta {
    Histogram histogram;
    bool reset{};
};

// Interval maxima cannot be recovered, so max_ns is zero.
[[nodiscard]] HistogramDelta histogram_delta(const Histogram& current,
                                             const Histogram& previous) noexcept;

}
