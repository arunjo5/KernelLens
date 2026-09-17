#pragma once

#include "latency/histogram.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace latency {

struct MetricsSnapshot {
    Histogram scheduling;
    Histogram block_io;
    std::uint64_t resets{};
};

// The HTTP thread reads copies, not live collector state.
class SnapshotStore {
public:
    void publish(MetricsSnapshot snapshot);
    [[nodiscard]] MetricsSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    MetricsSnapshot snapshot_;
};

[[nodiscard]] std::string prometheus_metrics(const MetricsSnapshot& snapshot);

// Serves /metrics on loopback, one client at a time, with 8 KiB headers and
// 250 ms socket deadlines. The snapshot callback must return promptly.
class MetricsServer {
public:
    using SnapshotProvider = std::function<MetricsSnapshot()>;

    MetricsServer(std::uint16_t port, SnapshotProvider provider);
    ~MetricsServer();
    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace latency
