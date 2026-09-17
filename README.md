# KernelLens

KernelLens is an eBPF project for measuring CPU scheduling delays and block I/O latency in Linux services. It uses C for the kernel probes and C++20 with libbpf
for the collector.

This initial setup contains the build configuration and shared data types. The
probes and collector will follow in later commits.

## Build setup

You need CMake 3.22 or newer, Ninja, and a C++20 compiler.

```sh
cmake --preset release
cmake --build --preset release
```

There is no tracer executable yet. These commands configure the project for the
code that follows.
