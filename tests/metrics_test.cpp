#include "latency/metrics.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace {

class Client {
public:
    explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
        if (fd_ < 0) throw std::runtime_error("test socket failed");
        const timeval timeout{2, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            throw std::runtime_error("test connect failed");
        }
    }
    ~Client() { ::close(fd_); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    void send(std::string_view request) const {
        std::size_t offset = 0;
        while (offset < request.size()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto count = ::send(fd_, request.data() + offset, request.size() - offset, flags);
            if (count <= 0) throw std::runtime_error("test send failed");
            offset += static_cast<std::size_t>(count);
        }
    }
    [[nodiscard]] std::string read() const {
        std::string result;
        char buffer[4096];
        while (true) {
            const auto count = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (count == 0) return result;
            if (count < 0) throw std::runtime_error("test receive failed or timed out");
            result.append(buffer, static_cast<std::size_t>(count));
        }
    }

private:
    int fd_;
};

std::string request(std::uint16_t port,
                    std::string_view text = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n") {
    Client client(port);
    client.send(text);
    return client.read();
}

}

TEST_CASE("Prometheus histogram buckets are cumulative and include matching count and Inf") {
    latency::MetricsSnapshot snapshot;
    snapshot.scheduling.observe(0);
    snapshot.scheduling.observe(1);
    snapshot.scheduling.observe(2);
    snapshot.scheduling.observe(8);
    snapshot.block_io.observe(1'000'000'000);
    snapshot.resets = 3;
    const auto output = latency::prometheus_metrics(snapshot);
    REQUIRE(output.find("# TYPE latency_tracer_sched_delay_seconds histogram\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"0.000000001\"} 2\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"0.000000003\"} 3\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"0.000000007\"} 3\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"0.000000015\"} 4\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"18446744073.709551615\"} 4\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"+Inf\"} 4\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_count 4\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_sum 0.000000011\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_block_io_seconds_sum 1.000000000\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_counter_resets_total 3\n") != std::string::npos);
    REQUIRE(output.back() == '\n');
}

TEST_CASE("Empty and inconsistent raw sample counters still produce valid histogram counts") {
    latency::MetricsSnapshot snapshot;
    auto output = latency::prometheus_metrics(snapshot);
    REQUIRE(output.find("latency_tracer_block_io_seconds_count 0\n") != std::string::npos);
    snapshot.scheduling.observe(4);
    snapshot.scheduling.samples = 7; // Simulate a concurrent map update.
    output = latency::prometheus_metrics(snapshot);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_bucket{le=\"+Inf\"} 1\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_count 1\n") != std::string::npos);
    snapshot.scheduling.buckets[0] = std::numeric_limits<std::uint64_t>::max();
    snapshot.scheduling.saturated = true;
    output = latency::prometheus_metrics(snapshot);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_count 18446744073709551615\n") != std::string::npos);
    REQUIRE(output.find("latency_tracer_histogram_saturated{probe=\"sched\"} 1\n") != std::string::npos);
}

TEST_CASE("Metrics text uses the Prometheus decimal point regardless of process locale") {
    class CommaPunctuation final : public std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };
    struct RestoreLocale {
        std::locale previous = std::locale();
        ~RestoreLocale() { std::locale::global(previous); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new CommaPunctuation));
    latency::MetricsSnapshot snapshot;
    snapshot.scheduling.observe(1'234'567'890);
    const auto output = latency::prometheus_metrics(snapshot);
    REQUIRE(output.find("latency_tracer_sched_delay_seconds_sum 1.234567890\n") != std::string::npos);
    REQUIRE(output.find(',') == std::string::npos);
}

TEST_CASE("SnapshotStore provides coherent snapshots to concurrent readers") {
    latency::SnapshotStore store;
    std::atomic<bool> valid{true};
    std::vector<std::future<void>> readers;
    for (unsigned reader = 0; reader < 4; ++reader) {
        readers.emplace_back(std::async(std::launch::async, [&] {
            for (unsigned iteration = 0; iteration < 4000; ++iteration) {
                const auto value = store.snapshot();
                if (value.scheduling.samples != value.block_io.samples ||
                    value.scheduling.total_ns != value.block_io.total_ns ||
                    value.scheduling.buckets != value.block_io.buckets ||
                    value.resets != value.scheduling.samples) valid = false;
            }
        }));
    }
    latency::MetricsSnapshot snapshot;
    for (unsigned iteration = 0; iteration < 4000; ++iteration) {
        snapshot.scheduling.observe(iteration);
        snapshot.block_io = snapshot.scheduling;
        snapshot.resets = snapshot.scheduling.samples;
        store.publish(snapshot);
    }
    for (auto& reader : readers) reader.get();
    REQUIRE(valid.load());
    REQUIRE(store.snapshot().scheduling.samples == 4000);
}

TEST_CASE("Metrics endpoint returns a complete scrape and handles request errors") {
    latency::SnapshotStore store;
    latency::MetricsSnapshot snapshot;
    snapshot.scheduling.observe(1000);
    store.publish(snapshot);
    latency::MetricsServer server(0, [&] { return store.snapshot(); });
    server.start();
    REQUIRE(server.port() != 0);
    server.start();
    const auto response = request(server.port());
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(response.find("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n") != std::string::npos);
    REQUIRE(response.find("Connection: close\r\n") != std::string::npos);
    const auto body = latency::prometheus_metrics(snapshot);
    REQUIRE(response.find("Content-Length: " + std::to_string(body.size()) + "\r\n") != std::string::npos);
    REQUIRE(response.substr(response.find("\r\n\r\n") + 4) == body);
    REQUIRE(request(server.port(), "GET / HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404"));
    REQUIRE(request(server.port(), "POST /metrics HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 405"));
    REQUIRE(request(server.port(), "broken\r\n\r\n").starts_with("HTTP/1.1 400"));
    REQUIRE(request(server.port(), std::string(8192, 'x')).starts_with("HTTP/1.1 431"));
    REQUIRE(request(server.port(), "GET /metrics HTTP/1.0\r\n\r\n").starts_with("HTTP/1.1 200"));
    server.stop();
    server.stop();
}

TEST_CASE("Concurrent scrapes remain coherent while snapshots change") {
    latency::SnapshotStore store;
    latency::MetricsServer server(0, [&] { return store.snapshot(); });
    server.start();
    std::atomic<bool> update{true};
    auto writer = std::async(std::launch::async, [&] {
        latency::MetricsSnapshot snapshot;
        while (update.load()) {
            snapshot.scheduling.observe(10);
            snapshot.block_io = snapshot.scheduling;
            store.publish(snapshot);
        }
    });
    std::vector<std::future<bool>> readers;
    for (unsigned index = 0; index < 4; ++index) {
        readers.emplace_back(std::async(std::launch::async, [&] {
            for (unsigned iteration = 0; iteration < 10; ++iteration) {
                const auto response = request(server.port());
                if (!response.starts_with("HTTP/1.1 200")) return false;
                const auto extract = [&](std::string_view metric) {
                    const auto begin = response.find(metric);
                    if (begin == std::string::npos) throw std::runtime_error("missing metric");
                    const auto value = begin + metric.size();
                    return response.substr(value, response.find('\n', value) - value);
                };
                if (extract("latency_tracer_sched_delay_seconds_count ") !=
                    extract("latency_tracer_block_io_seconds_count ")) return false;
            }
            return true;
        }));
    }
    bool valid = true;
    try {
        for (auto& reader : readers) valid = reader.get() && valid;
    } catch (...) {
        update = false;
        throw;
    }
    update = false;
    writer.get();
    REQUIRE(valid);
}

TEST_CASE("Slow clients cannot block later scrapes or server shutdown indefinitely") {
    latency::MetricsServer server(0, [] { return latency::MetricsSnapshot{}; });
    server.start();
    Client slow(server.port());
    slow.send("GET /metrics HTTP/1.1\r\n");
    const auto before = std::chrono::steady_clock::now();
    REQUIRE(request(server.port()).starts_with("HTTP/1.1 200"));
    REQUIRE(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    Client slower(server.port());
    slower.send("G");
    const auto stopping = std::chrono::steady_clock::now();
    server.stop();
    REQUIRE(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(1));
}

TEST_CASE("Metrics server reports bind errors and survives snapshot provider errors") {
    REQUIRE_THROWS_AS(latency::MetricsServer(0, {}), std::invalid_argument);
    std::atomic<bool> fail{true};
    latency::MetricsServer server(0, [&] {
        if (fail.load()) throw std::runtime_error("provider failed");
        return latency::MetricsSnapshot{};
    });
    server.start();
    latency::MetricsServer collision(server.port(), [] { return latency::MetricsSnapshot{}; });
    REQUIRE_THROWS(collision.start());
    REQUIRE(request(server.port()).starts_with("HTTP/1.1 500"));
    fail = false;
    REQUIRE(request(server.port()).starts_with("HTTP/1.1 200"));
    server.stop();
    server.start();
    REQUIRE(request(server.port()).starts_with("HTTP/1.1 200"));
}
