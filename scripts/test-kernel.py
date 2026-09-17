#!/usr/bin/env python3
"""Check BPF probes and filters on Linux. Run as root."""

from __future__ import annotations

import argparse
import json
import mmap
import os
from pathlib import Path
import platform
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


def worker(path: str, cpu: int, spin: bool) -> None:
    os.sched_setaffinity(0, {cpu})
    stopped = False

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    if spin:
        print("WORKER_READY", flush=True)
        if not sys.stdin.buffer.read(1):
            return
        value = 1
        while not stopped:
            value = (value * 1664525 + 1013904223) & 0xFFFFFFFF
        return

    # mmap supplies the alignment required by O_DIRECT.
    flags = os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_DIRECT | os.O_DSYNC
    fd = os.open(path, flags, 0o600)
    payload = mmap.mmap(-1, 4096)
    payload[:] = b"L" * 4096
    operations = 0
    try:
        # Check direct I/O support before reporting readiness.
        if os.pwrite(fd, payload, 0) != 4096:
            raise RuntimeError("short direct write")
        os.fsync(fd)
        print("WORKER_READY", flush=True)
        while not stopped:
            token = sys.stdin.buffer.read(1)
            if not token:
                break
            until = time.monotonic() + 0.0005
            value = 1
            while time.monotonic() < until:
                value = (value * 1664525 + 1013904223) & 0xFFFFFFFF
            count = os.pwrite(fd, payload, (operations % 1024) * 4096)
            if count != 4096:
                raise RuntimeError("short direct write")
            os.fsync(fd)
            operations += 1
        print(json.dumps({"direct_write_fsync_operations": operations}), flush=True)
    finally:
        payload.close()
        os.close(fd)


def terminate(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


class Child:
    def __init__(self, group: Path, io_path: Path, cpu: int, spin: bool = False):
        def enter_group() -> None:
            (group / "cgroup.procs").write_text(str(os.getpid()))

        command = [sys.executable, str(Path(__file__).resolve()), "--worker",
                   str(io_path), "--cpu", str(cpu)]
        if spin:
            command.append("--spin")
        # No launcher threads run during Popen, which uses preexec_fn.
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=False, preexec_fn=enter_group)
        self.spin = spin
        self.operations = 0
        self.error = b""
        ready: queue.Queue[bytes] = queue.Queue()
        reader = threading.Thread(target=lambda: ready.put(self.process.stdout.readline()),
                                  daemon=True)
        reader.start()
        try:
            line = ready.get(timeout=10)
        except queue.Empty:
            terminate(self.process)
            raise RuntimeError("worker did not become ready within 10 seconds")
        reader.join(timeout=1)
        if line.strip() != b"WORKER_READY":
            terminate(self.process)
            error = self.process.stderr.read().decode(errors="replace")
            raise RuntimeError("direct-I/O worker failed; choose a disk-backed --io-dir: " + error)
        os.set_blocking(self.process.stdin.fileno(), False)

    def wake(self) -> None:
        if self.process.poll() is not None:
            raise RuntimeError("workload exited during tracing")
        try:
            os.write(self.process.stdin.fileno(), b"W")
        except BlockingIOError:
            pass

    def close(self) -> dict:
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        if self.spin:
            terminate(self.process)
        else:
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                terminate(self.process)
        output = self.process.stdout.read().decode(errors="replace")
        error = self.process.stderr.read().decode(errors="replace")
        if self.process.returncode not in (0, -signal.SIGTERM):
            raise RuntimeError(f"worker exited {self.process.returncode}: {error}")
        return json.loads(output) if output.strip() else {}


def trace_case(tracer: Path, output: Path, name: str, filters: list[str],
               seconds: float, child: Child, wake: bool = True) -> dict:
    trace_path = output / f"{name}.jsonl"
    command = [str(tracer), "--duration", str(seconds), "--interval", "1",
               "--json-output", str(trace_path), "--no-metrics", *filters]
    messages: queue.Queue[str] = queue.Queue()
    stderr_lines: list[str] = []
    with (output / f"{name}.stdout").open("w") as stdout:
        process = subprocess.Popen(command, stdout=stdout, stderr=subprocess.PIPE, text=True)

        def drain_stderr() -> None:
            for line in process.stderr:
                stderr_lines.append(line)
                messages.put(line)

        reader = threading.Thread(target=drain_stderr, daemon=True)
        reader.start()
        try:
            deadline = time.monotonic() + 15
            while True:
                if time.monotonic() >= deadline:
                    raise RuntimeError("tracer never printed READY")
                try:
                    line = messages.get(timeout=0.1)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError("tracer failed to attach: " + "".join(stderr_lines))
                    continue
                if line.startswith("READY "):
                    break
            deadline = time.monotonic() + seconds + 8
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    raise RuntimeError("tracer exceeded bounded duration")
                if wake:
                    child.wake()
                time.sleep(0.005)
            if process.returncode:
                raise RuntimeError(f"tracer exited {process.returncode}: {''.join(stderr_lines)}")
        finally:
            terminate(process)
            reader.join(timeout=2)
            (output / f"{name}.stderr").write_text("".join(stderr_lines))

    records = [json.loads(line) for line in trace_path.read_text().splitlines()]
    samples = [record for record in records if record.get("type") == "sample"]
    if not samples or not samples[-1].get("final"):
        raise AssertionError(f"{name}: missing detached final snapshot")
    final = samples[-1]
    for kind in ("sched", "block"):
        histogram = final[kind]
        bins = histogram["buckets"]
        if len(bins) != 64 or sum(bins) != histogram["samples"]:
            raise AssertionError(f"{name}: invalid final {kind} bucket/count invariant")
        previous = [0] * 64
        previous_total = 0
        for sample in samples:
            current = sample[kind]
            if any(a > b for a, b in zip(previous, current["buckets"])):
                raise AssertionError(f"{name}: cumulative {kind} bucket regressed")
            if current["total_ns"] < previous_total:
                raise AssertionError(f"{name}: cumulative {kind} sum regressed")
            previous = current["buckets"]
            previous_total = current["total_ns"]
        if histogram["samples"] and not 0 < histogram["max_ns"] <= histogram["total_ns"]:
            raise AssertionError(f"{name}: invalid final {kind} maximum/sum")
    critical = ("sched_start_dropped", "block_start_dropped", "sched_read_failed",
                "block_read_failed", "sched_cgroup_unavailable", "block_stale_start",
                "clock_regression", "block_completion_error")
    failures = {key: final["diagnostics"].get(key, 0) for key in critical
                if final["diagnostics"].get(key, 0)}
    if failures:
        raise AssertionError(f"{name}: critical BPF diagnostics: {failures}")
    result = {"name": name, "command": command, "sched_samples": final["sched"]["samples"],
              "block_samples": final["block"]["samples"], "diagnostics": final["diagnostics"]}
    print(json.dumps(result), flush=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer", type=Path, default=Path("build/latency-tracer"))
    parser.add_argument("--output-dir", type=Path, default=Path(".local/kernel-smoke"))
    parser.add_argument("--io-dir", type=Path, default=Path("/var/tmp"))
    parser.add_argument("--duration", type=float, default=2)
    parser.add_argument("--worker", help=argparse.SUPPRESS)
    parser.add_argument("--cpu", type=int, default=0, help=argparse.SUPPRESS)
    parser.add_argument("--spin", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args.worker, args.cpu, args.spin)
        return 0
    if sys.platform != "linux" or os.geteuid() != 0:
        parser.error("run as root inside the Linux benchmark VM")
    if not 1 <= args.duration <= 10:
        parser.error("--duration must be between 1 and 10 seconds")
    tracer = args.tracer.resolve(strict=True)
    cgroup_root = Path("/sys/fs/cgroup")
    if not (cgroup_root / "cgroup.controllers").is_file():
        parser.error("a writable cgroup v2 hierarchy is required")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cpu = min(os.sched_getaffinity(0))
    groups: list[Path] = []
    children: list[Child] = []
    results: list[dict] = []
    report = {"kernel": platform.release(), "architecture": platform.machine(),
              "duration_per_case_seconds": args.duration, "cpu": cpu,
              "parent_cgroup": Path("/proc/self/cgroup").read_text(), "cases": results}
    try:
        for suffix in ("target", "empty"):
            group = cgroup_root / f"latency-smoke-{os.getpid()}-{suffix}"
            group.mkdir()
            groups.append(group)
        target, empty = groups
        report["target_cgroup_id"] = target.stat().st_ino
        report["empty_cgroup_id"] = empty.stat().st_ino
        with tempfile.TemporaryDirectory(prefix="latency-smoke-", dir=args.io_dir) as temporary:
            io_dir = Path(temporary)
            child = Child(target, io_dir / "io.dat", cpu)
            children.append(child)
            pid = child.process.pid
            cases = [
                ("unfiltered", []),
                ("pid", ["--pid", str(pid)]),
                ("cgroup_external_waker", ["--cgroup", str(target)]),
                ("pid_mismatch", ["--pid", "2147483647"]),
                ("and_filter_mismatch", ["--pid", str(pid), "--cgroup", str(empty)]),
            ]
            for name, filters in cases:
                result = trace_case(tracer, output, name, filters, args.duration, child)
                results.append(result)
                if "mismatch" in name:
                    if result["sched_samples"] or result["block_samples"]:
                        raise AssertionError(f"{name}: nonmatching filters captured samples")
                elif not result["sched_samples"]:
                    raise AssertionError(f"{name}: selected task had no scheduling samples")
                if name == "unfiltered" and not result["block_samples"]:
                    raise AssertionError("direct writes produced no unfiltered block samples")
            report["io_worker"] = child.close()
            children.remove(child)
            if not report["io_worker"].get("direct_write_fsync_operations"):
                raise AssertionError("I/O workload completed no operations")

            # Start both spinners before attaching so samples require
            # runnable preemption tracking.
            spinning = []
            for index in range(2):
                child = Child(target, io_dir / f"spin{index}", cpu, spin=True)
                spinning.append(child)
                children.append(child)
            for child in spinning:
                child.wake()
            time.sleep(0.15)
            result = trace_case(tracer, output, "runnable_preemption",
                                ["--pid", str(spinning[0].process.pid)], args.duration,
                                spinning[0], wake=False)
            results.append(result)
            if result["sched_samples"] < 3:
                raise AssertionError("continuously runnable task had fewer than 3 scheduling intervals")
            for child in spinning:
                child.close()
                children.remove(child)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        for child in children:
            terminate(child.process)
        for group in reversed(groups):
            try:
                group.rmdir()
            except OSError as error:
                report.setdefault("cleanup_errors", []).append(str(error))
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(results)} kernel integration cases; artifacts: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, AssertionError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
