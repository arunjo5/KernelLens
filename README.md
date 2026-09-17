# KernelLens

KernelLens is an eBPF project for measuring CPU scheduling delays and block I/O latency in Linux services. The kernel probes are written in C and use libbpf CO-RE to adapt to kernel field layouts.

The probes and their build setup are included here. The C++20 collector will follow in a later commit, so there is no tracer executable yet.

## Probes

- CPU scheduling delay measures how long a task waits from wakeup to running. It also covers waits after preemption or yielding.
- Block I/O latency measures time from a request being issued to its final completion, including requeues.

Each probe records a histogram with 64 timing buckets per CPU. PID and cgroup v2 filters can be set before loading the probes. For I/O, the PID belongs to the task issuing the request, which may be a kernel worker. The cgroup filter uses the request's first bio and skips requests with no known owner.

## Build

You need Linux 6.x on x86_64 or ARM64 with kernel type information at `/sys/kernel/btf/vmlinux`. Building requires CMake 3.22 or newer, Ninja, C and C++20 compilers, clang with BPF support, bpftool, pkg-config, and the libbpf development package at version 1.0 or newer.

```sh
cmake --preset release
cmake --build --preset release
```

The build generates `vmlinux.h`, `latency.bpf.o`, and `latency.skel.h` in `build/generated`. The object contains the probes and the skeleton lets the future collector load them through libbpf. Building does not attach the probes to the kernel.

On other operating systems, use `cmake --preset release -DLATENCY_BUILD_BPF=OFF` to configure only the shared headers.
