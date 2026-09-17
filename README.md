# KernelLens

KernelLens is an eBPF tracer for investigating slow Linux services. It measures how long tasks wait for CPU time and how long storage requests take. It prints p50, p95, and p99 timings and exports histograms for Prometheus.

The probes are written in C. A C++20 collector loads them through libbpf. CO-RE support lets the probes adapt to kernel field layouts.

## What it measures

- CPU scheduling delay measures how long a task waits from wakeup to running. It also covers waits after preemption or yielding.
- Block I/O latency measures time from a request being issued to its final completion, including requeues.

Each probe records a histogram with 64 timing buckets per CPU. The collector combines them and reports the change since the previous reading. It does not measure full request latency, application queues, or database locks.

## Build

You need Linux 6.x on x86_64 or ARM64 with kernel type information at `/sys/kernel/btf/vmlinux`. Building requires CMake 3.22 or newer, Ninja, C and C++20 compilers, clang with BPF support, bpftool, pkg-config, and the libbpf development package at version 1.0 or newer.

On Ubuntu 24.04, run `bash scripts/setup-linux.sh` to install the dependencies, then build the tracer.

```sh
cmake --preset release
cmake --build --preset release
```

The executable is `build/latency-tracer`. CMake also generates the kernel header, BPF object, and libbpf skeleton in `build/generated`.

On macOS, add `-DLATENCY_BUILD_BPF=OFF` to the configure command to build only the histogram and metrics library. Tracing requires Linux.

## Test

The test preset enables AddressSanitizer and UndefinedBehaviorSanitizer. Tests cover histogram boundaries, percentiles, counter resets, and the metrics endpoint. CMake uses an installed Catch2 v3 or downloads a checksum-verified copy of Catch2 3.7.1.

```sh
cmake --preset test
cmake --build --preset test
ctest --preset test
```

Use the release build for kernel checks on a Linux test host or VM. These checks require root, writable cgroup v2, and Python 3.10 or newer. They create temporary workloads to check PID and cgroup filters, scheduling under contention, and disk I/O.

```sh
sudo python3 scripts/test-kernel.py --tracer ./build/latency-tracer
```

Results stay in `.local/kernel-smoke`. Use `--io-dir` to select a filesystem that supports direct I/O if `/var/tmp` does not.

## Run

Run as root on the Linux host. For containers, use the PID visible on the host or the container's cgroup v2 path.

```sh
sudo ./build/latency-tracer --duration 30
sudo ./build/latency-tracer --pid 1234 --duration 30 --json-output trace.jsonl
sudo ./build/latency-tracer --cgroup /sys/fs/cgroup/system.slice/example.service --duration 60
```

The `READY` message means the probes are attached. Reports appear once per second by default. Use `--interval` to change that, `--duration 0` to run until Ctrl+C, and `--help` for all options.

To read Prometheus metrics, open another terminal while the tracer is running.

```sh
curl --fail http://127.0.0.1:9108/metrics
```

The endpoint binds to localhost. Use `--listen 127.0.0.1:PORT` to change its port or `--no-metrics` to disable it. The histogram names are `latency_tracer_sched_delay_seconds` and `latency_tracer_block_io_seconds`, each with `_bucket`, `_sum`, and `_count` values.

## Reading the output

CLI percentiles are the upper edges of timing buckets, in microseconds. They are approximate and can be nearly twice the actual latency. An empty histogram prints `-`. Prometheus histograms are cumulative and use seconds. JSON Lines output keeps cumulative nanosecond measurements and includes a final reading after the probes detach.

The PID filter includes all threads in the process. Cgroup matching is exact and excludes child cgroups. With both filters set, an event must match both. For I/O, the PID belongs to the issuer, which may be a kernel worker. The cgroup filter uses the request's first bio and skips requests with unknown ownership, including some flushes.

Live map readings can overlap with new measurements. Check the stderr or JSON diagnostics for dropped measurements and unknown I/O owners. An empty filtered histogram does not always mean no I/O occurred.

See [how the probes work](bpf/README.md) for details about filtering, request completion, and missing measurements.

## Apple Silicon VM

The optional helper runs Ubuntu 24.04 ARM64 with native hardware virtualization, 4 vCPUs, 8 GiB RAM, and a 30 GiB disk. It needs Python 3.11 or newer and Homebrew QEMU installed under `/opt/homebrew`.

```sh
brew install qemu cmake ninja
python3 -m venv .local/python
.local/python/bin/pip install pycdlib==1.20.0
.local/python/bin/python scripts/vm.py prepare
.local/python/bin/python scripts/vm.py start
```

Once the guest has booted, check SSH and copy the project into it.

```sh
.local/python/bin/python scripts/vm.py ssh uname -a
python3 scripts/sync-vm.py
.local/python/bin/python scripts/vm.py ssh
```

In the guest, run `cd /home/bench/latency-tracer`, then use the Linux setup and build commands above. To shut down the VM, run this on your Mac.

```sh
.local/python/bin/python scripts/vm.py stop
```

The helper checks the pinned Ubuntu image's SHA-256 checksum. Disks, keys, and logs stay in the ignored `.local/vm-arm64` directory. SSH is available only through `127.0.0.1:22222`.
