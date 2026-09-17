// SPDX-License-Identifier: GPL-2.0-or-later
#include "latency/histogram.hpp"
#include "latency/metrics.hpp"
#include "latency_shared.h"
#include "latency.skel.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
constexpr std::array<const char*, LATENCY_DIAG_COUNT> diagnostic_names{
    "sched_start_dropped", "sched_unmatched", "sched_duplicate_wakeup",
    "sched_read_failed", "sched_cgroup_unavailable", "sched_exit_cleanup",
    "block_start_dropped", "block_unmatched", "block_reissue",
    "block_partial_completion", "block_cgroup_unavailable", "block_completion_error",
    "block_stale_start", "block_read_failed", "clock_regression"};
struct Options {
    std::uint32_t pid{};
    std::uint64_t cgroup_id{};
    int cgroup_fd{-1};
    double duration{}, interval{1.0};
    std::uint16_t port{9108};
    bool metrics{true}, verbose{};
    std::string output;
};
std::uint64_t integer(std::string_view text, std::uint64_t max) {
    std::uint64_t value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data()+text.size() || value > max)
        throw std::runtime_error("Invalid unsigned integer: " + std::string(text));
    return value;
}
double seconds(const std::string& value, double minimum) {
    std::size_t end{};
    double result = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(result) || result < minimum || result > 604800)
        throw std::runtime_error("Duration must be finite and between " + std::to_string(minimum) + " and 604800 seconds");
    return result;
}
void usage() {
    std::cout << "latency-tracer [--pid HOST_TGID] [--cgroup /sys/fs/cgroup/PATH]\n"
      "  [--duration SECONDS] [--interval SECONDS] [--json-output FILE]\n"
      "  [--listen 127.0.0.1:PORT | --no-metrics] [--verbose]\n\n"
      "Linux 6.x CO-RE scheduling and block I/O latency. Filters combine with AND.\n"
      "PID uses the initial host namespace. Cgroup v2 matching is exact, not recursive.\n"
      "Block PID is the issue-context TGID; cgroup is the first bio's owner.\n"
      "CLI quantiles are log2 bucket UPPER BOUNDS in microseconds, per interval.\n"
      "Prometheus and JSON histograms are cumulative nanosecond measurements.\n"
      "Duration 0 runs until SIGINT/SIGTERM. /metrics binds loopback only.\n";
}
Options parse(int argc, char** argv) {
    Options options;
    for (int i=1; i<argc; ++i) {
        std::string option = argv[i];
        auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("Missing value for " + option); return argv[i]; };
        if (option == "--help" || option == "-h") { usage(); std::exit(0); }
        if (option == "--pid") {
            options.pid = static_cast<std::uint32_t>(integer(value(), std::numeric_limits<std::int32_t>::max()));
            if (!options.pid) throw std::runtime_error("--pid must be positive; omit it to select all processes");
        } else if (option == "--cgroup") {
            auto path = value();
            if (options.cgroup_fd >= 0) throw std::runtime_error("Specify --cgroup once");
            options.cgroup_fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (options.cgroup_fd < 0) throw std::runtime_error("Open cgroup: " + std::string(std::strerror(errno)));
            struct stat info{}; struct statfs fs{};
            if (fstat(options.cgroup_fd, &info) || fstatfs(options.cgroup_fd, &fs) || fs.f_type != 0x63677270)
                throw std::runtime_error("--cgroup must name an existing cgroup v2 directory");
            options.cgroup_id = info.st_ino;
        } else if (option == "--duration") options.duration = seconds(value(), 0);
        else if (option == "--interval") options.interval = seconds(value(), 0.05);
        else if (option == "--json-output") options.output = value();
        else if (option == "--listen") {
            const auto address = value(); constexpr std::string_view prefix = "127.0.0.1:";
            if (!address.starts_with(prefix)) throw std::runtime_error("--listen supports 127.0.0.1:PORT only");
            options.port = static_cast<std::uint16_t>(integer(std::string_view(address).substr(prefix.size()), 65535));
        } else if (option == "--no-metrics") options.metrics = false;
        else if (option == "--verbose") options.verbose = true;
        else throw std::runtime_error("Unknown argument: " + option);
    }
    return options;
}
bool verbose_log{};
int libbpf_log(libbpf_print_level level, const char* format, va_list args) {
    if (level == LIBBPF_DEBUG && !verbose_log) return 0;
    return vfprintf(stderr, format, args);
}
using Histograms = std::array<latency::Histogram, LATENCY_KIND_COUNT>;
class MapReader {
public:
    explicit MapReader(latency_bpf* bpf) : hist_fd_(bpf_map__fd(bpf->maps.histograms)),
        diag_fd_(bpf_map__fd(bpf->maps.diagnostics)) {
        const auto n = libbpf_num_possible_cpus();
        if (n <= 0) throw std::runtime_error("Cannot determine possible CPUs");
        hist_.resize(static_cast<std::size_t>(n)); diag_.resize(static_cast<std::size_t>(n));
    }
    Histograms histograms() {
        Histograms output;
        for (std::uint32_t key=0; key<LATENCY_KIND_COUNT; ++key) {
            if (bpf_map_lookup_elem(hist_fd_, &key, hist_.data())) throw std::runtime_error("Read histogram: " + std::string(std::strerror(errno)));
            for (const auto& raw : hist_) {
                latency::Histogram cpu;
                std::copy(std::begin(raw.buckets), std::end(raw.buckets), cpu.buckets.begin());
                cpu.samples = raw.samples; cpu.total_ns = raw.total_ns; cpu.max_ns = raw.max_ns;
                output[key].merge(cpu);
            }
        }
        return output;
    }
    std::array<std::uint64_t, LATENCY_DIAG_COUNT> diagnostics() {
        std::array<std::uint64_t, LATENCY_DIAG_COUNT> output{};
        for (std::uint32_t key=0; key<LATENCY_DIAG_COUNT; ++key) {
            if (bpf_map_lookup_elem(diag_fd_, &key, diag_.data())) throw std::runtime_error("Read diagnostics: " + std::string(std::strerror(errno)));
            for (auto value : diag_) output[key] += value;
        }
        return output;
    }
private:
    int hist_fd_, diag_fd_;
    std::vector<latency_histogram> hist_;
    std::vector<std::uint64_t> diag_;
};
void json_histogram(std::ostream& out, const latency::Histogram& histogram) {
    out << "{\"samples\":" << histogram.samples << ",\"total_ns\":" << histogram.total_ns
        << ",\"max_ns\":" << histogram.max_ns << ",\"buckets\":[";
    for (std::size_t i=0; i<histogram.buckets.size(); ++i) { if (i) out << ','; out << histogram.buckets[i]; }
    out << "]}";
}
std::string quantile(const latency::Histogram& histogram, unsigned percentile) {
    auto value = histogram.percentile_upper_bound(percentile);
    if (!value) return "-";
    std::ostringstream out; out << std::fixed << std::setprecision(3) << static_cast<double>(*value)/1000.0;
    return out.str();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        verbose_log = options.verbose; libbpf_set_print(libbpf_log);
        struct utsname system{};
        if (uname(&system)) throw std::runtime_error("uname failed");
        if (std::string_view(system.release).substr(0,2) != "6.")
            throw std::runtime_error("This version requires Linux 6.x raw tracepoint ABI");
        // Some kernel configurations still require a raised memlock limit.
        const rlimit limit{RLIM_INFINITY, RLIM_INFINITY};
        (void)setrlimit(RLIMIT_MEMLOCK, &limit);
        std::unique_ptr<latency_bpf, decltype(&latency_bpf__destroy)> bpf(latency_bpf__open(), latency_bpf__destroy);
        if (!bpf) throw std::runtime_error("Open BPF skeleton failed");
        bpf->rodata->target_tgid = options.pid; bpf->rodata->target_cgroup_id = options.cgroup_id;
        if (latency_bpf__load(bpf.get())) throw std::runtime_error("BPF load failed; run as root on a Linux 6.x host with kernel BTF and CONFIG_BPF_SYSCALL");
        std::ofstream json;
        if (!options.output.empty()) {
            json.open(options.output, std::ios::out | std::ios::trunc);
            if (!json) throw std::runtime_error("Cannot open JSON output file");
        }
        latency::SnapshotStore snapshots;
        std::unique_ptr<latency::MetricsServer> server;
        if (options.metrics) {
            server = std::make_unique<latency::MetricsServer>(options.port, [&] { return snapshots.snapshot(); });
            server->start();
        }
        std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
        if (latency_bpf__attach(bpf.get())) throw std::runtime_error("Attach failed; need sched wakeup/wakeup_new/switch/exit and block issue/complete raw tracepoints");
        MapReader reader(bpf.get());
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto next = start;
        Histograms previous;
        std::uint64_t resets{};
        if (json.is_open()) {
            json << "{\"type\":\"metadata\",\"kernel\":\"" << system.release << "\",\"architecture\":\"" << system.machine
                << "\",\"pid\":" << options.pid << ",\"cgroup_id\":" << options.cgroup_id
                << ",\"bucket_unit\":\"ns\",\"bucket_rule\":\"floor_log2\",\"cumulative\":true}\n";
            json.flush();
        }
        std::cerr << "READY pid=" << options.pid << " cgroup=" << options.cgroup_id;
        if (server) std::cerr << " metrics=http://127.0.0.1:" << server->port() << "/metrics";
        std::cerr << '\n';
        std::cout << "elapsed_s probe samples p50_upper_us p95_upper_us p99_upper_us\n";
        auto sample = [&](bool final) {
            const auto current = reader.histograms();
            const auto diagnostics = reader.diagnostics();
            const double elapsed = std::chrono::duration<double>(Clock::now()-start).count();
            for (std::size_t kind=0; kind<current.size(); ++kind) {
                const auto delta = latency::histogram_delta(current[kind], previous[kind]);
                if (delta.reset) ++resets;
                const auto& h = delta.histogram;
                std::cout << std::fixed << std::setprecision(3) << elapsed << ' ' << (kind ? "block" : "sched") << ' '
                    << h.samples << ' ' << quantile(h,50) << ' ' << quantile(h,95) << ' ' << quantile(h,99) << '\n';
            }
            snapshots.publish({current[LATENCY_SCHED], current[LATENCY_BLOCK], resets});
            previous = current;
            if (json.is_open()) {
                json << "{\"type\":\"sample\",\"elapsed_seconds\":" << std::setprecision(9) << elapsed
                    << ",\"final\":" << (final ? "true" : "false") << ",\"sched\":";
                json_histogram(json, current[LATENCY_SCHED]); json << ",\"block\":"; json_histogram(json, current[LATENCY_BLOCK]);
                json << ",\"diagnostics\":{";
                for (std::size_t i=0;i<diagnostics.size();++i) { if(i) json << ','; json << '"' << diagnostic_names[i] << "\":" << diagnostics[i]; }
                json << "}}\n"; json.flush();
                if (!json) throw std::runtime_error("Writing JSON output failed");
            }
            if (final) for (std::size_t i=0;i<diagnostics.size();++i)
                std::cerr << diagnostic_names[i] << '=' << diagnostics[i] << '\n';
            std::cout.flush();
        };
        while (!interrupted) {
            next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.interval));
            auto deadline = next;
            if (options.duration > 0) deadline = std::min(deadline, start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.duration)));
            while (!interrupted && Clock::now() < deadline)
                std::this_thread::sleep_for(std::min(std::chrono::milliseconds(20), std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()) + std::chrono::milliseconds(1)));
            if (interrupted || (options.duration > 0 && std::chrono::duration<double>(Clock::now()-start).count() >= options.duration)) break;
            sample(false);
        }
        latency_bpf__detach(bpf.get()); // Final read is stable after all producers have stopped.
        sample(true);
        if (server) server->stop();
        if (options.cgroup_fd >= 0) close(options.cgroup_fd);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "latency-tracer: " << error.what() << '\n';
        return 1;
    }
}
