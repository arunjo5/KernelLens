#include "latency/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace latency {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto socket_timeout = std::chrono::milliseconds(250);

class Socket {
public:
    explicit Socket(int fd = -1) noexcept : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) ::close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

void configure_socket(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fd_flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        throw std::system_error(errno, std::generic_category(), "configure metrics socket");
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "disable SIGPIPE");
    }
#endif
}

bool wait_socket(int fd, short events, Clock::time_point deadline,
                 const std::atomic<bool>& running) noexcept {
    while (running.load(std::memory_order_relaxed)) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining, 50)));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return false;
        if (result > 0) return (descriptor.revents & events) != 0;
    }
    return false;
}

void send_response(int fd, std::string_view status, std::string_view body,
                   const std::atomic<bool>& running) {
    std::string response = "HTTP/1.1 ";
    response += status;
    response += "\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\nConnection: close\r\nContent-Length: ";
    response += std::to_string(body.size());
    response += "\r\n\r\n";
    response += body;
    const auto deadline = Clock::now() + socket_timeout;
    std::size_t offset = 0;
    while (offset < response.size() && wait_socket(fd, POLLOUT, deadline, running)) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto count = ::send(fd, response.data() + offset, response.size() - offset, send_flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return;
        offset += static_cast<std::size_t>(count);
    }
}

void serve_client(int fd, const MetricsServer::SnapshotProvider& provider,
                  const std::atomic<bool>& running) {
    constexpr std::size_t maximum_header_bytes = 8192;
    const auto deadline = Clock::now() + socket_timeout;
    std::string request;
    request.reserve(1024);
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() >= maximum_header_bytes) {
            send_response(fd, "431 Request Header Fields Too Large", "header too large\n", running);
            return;
        }
        if (!wait_socket(fd, POLLIN, deadline, running)) return;
        char buffer[1024];
        const auto count = ::recv(fd, buffer,
                                  std::min(sizeof(buffer), maximum_header_bytes - request.size()), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return;
        request.append(buffer, static_cast<std::size_t>(count));
    }
    const auto line_end = request.find("\r\n");
    const std::string_view line(request.data(), line_end);
    const auto first_space = line.find(' ');
    const auto second_space = first_space == std::string_view::npos
        ? std::string_view::npos : line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        (line.substr(second_space + 1) != "HTTP/1.1" && line.substr(second_space + 1) != "HTTP/1.0")) {
        send_response(fd, "400 Bad Request", "malformed request\n", running);
        return;
    }
    if (line.substr(0, first_space) != "GET") {
        send_response(fd, "405 Method Not Allowed", "GET required\n", running);
        return;
    }
    if (line.substr(first_space + 1, second_space - first_space - 1) != "/metrics") {
        send_response(fd, "404 Not Found", "not found\n", running);
        return;
    }
    try {
        send_response(fd, "200 OK", prometheus_metrics(provider()), running);
    } catch (const std::exception&) {
        send_response(fd, "500 Internal Server Error", "snapshot unavailable\n", running);
    }
}

// Avoid rounding nanosecond boundaries through double.
std::string seconds(std::uint64_t nanoseconds) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << nanoseconds / 1'000'000'000 << '.' << std::setfill('0') << std::setw(9)
           << nanoseconds % 1'000'000'000;
    return output.str();
}

void append_histogram(std::ostringstream& output, std::string_view name,
                       std::string_view help, const Histogram& histogram) {
    output << "# HELP " << name << ' ' << help << '\n';
    output << "# TYPE " << name << " histogram\n";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < histogram_buckets; ++i) {
        cumulative += std::min(histogram.buckets[i],
                               std::numeric_limits<std::uint64_t>::max() - cumulative);
        output << name << "_bucket{le=\"" << seconds(bucket_upper_bound(i)) << "\"} "
               << cumulative << '\n';
    }
    output << name << "_bucket{le=\"+Inf\"} " << cumulative << '\n';
    output << name << "_sum " << seconds(histogram.total_ns) << '\n';
    // Map reads can race updates. Summing buckets keeps +Inf equal to _count.
    output << name << "_count " << cumulative << '\n';
}

} // namespace

void SnapshotStore::publish(MetricsSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(snapshot);
}

MetricsSnapshot SnapshotStore::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::string prometheus_metrics(const MetricsSnapshot& snapshot) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    append_histogram(output, "latency_tracer_sched_delay_seconds",
                     "Task scheduling delay from wakeup or preemption to running.", snapshot.scheduling);
    append_histogram(output, "latency_tracer_block_io_seconds",
                     "Block request latency from issue to final completion.", snapshot.block_io);
    output << "# HELP latency_tracer_counter_resets_total Observed cumulative histogram counter regressions.\n"
              "# TYPE latency_tracer_counter_resets_total counter\n"
              "latency_tracer_counter_resets_total " << snapshot.resets << '\n';
    output << "# HELP latency_tracer_histogram_saturated Histogram aggregation reached uint64 capacity.\n"
              "# TYPE latency_tracer_histogram_saturated gauge\n"
              "latency_tracer_histogram_saturated{probe=\"sched\"} " << (snapshot.scheduling.saturated ? 1 : 0) << '\n'
           << "latency_tracer_histogram_saturated{probe=\"block\"} " << (snapshot.block_io.saturated ? 1 : 0) << '\n';
    return output.str();
}

struct MetricsServer::Impl {
    explicit Impl(std::uint16_t port, SnapshotProvider source)
        : requested_port(port), provider(std::move(source)) {
        if (!provider) throw std::invalid_argument("metrics snapshot provider is required");
    }

    const std::uint16_t requested_port;
    SnapshotProvider provider;
    std::atomic<std::uint16_t> bound_port{};
    std::atomic<bool> running{};
    std::mutex lifecycle;
    Socket listener;
    std::thread worker;

    void run() noexcept {
        while (running.load(std::memory_order_relaxed)) {
            if (!wait_socket(listener.get(), POLLIN, Clock::now() + socket_timeout, running)) continue;
            Socket client(::accept(listener.get(), nullptr, nullptr));
            if (client.get() < 0) continue;
            try {
                configure_socket(client.get());
                serve_client(client.get(), provider, running);
            } catch (...) {
                // Keep collecting if a client fails.
            }
        }
    }
};

MetricsServer::MetricsServer(std::uint16_t port, SnapshotProvider provider)
    : impl_(std::make_unique<Impl>(port, std::move(provider))) {}

MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::start() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->running.load(std::memory_order_relaxed)) return;
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "create metrics socket");
    }
    configure_socket(listener.get());
    const int enabled = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        throw std::system_error(errno, std::generic_category(), "configure metrics listener");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(impl_->requested_port);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(listener.get(), 16) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind/listen metrics socket");
    }
    socklen_t length = sizeof(address);
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        throw std::system_error(errno, std::generic_category(), "get metrics port");
    }
    impl_->listener = std::move(listener);
    impl_->bound_port.store(ntohs(address.sin_port), std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_relaxed);
    try {
        impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    } catch (...) {
        impl_->running.store(false, std::memory_order_relaxed);
        impl_->listener = Socket{};
        throw;
    }
}

void MetricsServer::stop() noexcept {
    std::lock_guard lock(impl_->lifecycle);
    impl_->running.store(false, std::memory_order_relaxed);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->listener = Socket{};
}

std::uint16_t MetricsServer::port() const noexcept {
    return impl_->bound_port.load(std::memory_order_relaxed);
}

} // namespace latency
