/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATENCY_SHARED_H
#define LATENCY_SHARED_H

/* vmlinux.h supplies these types for BPF; Linux userspace uses linux/types.h. */
#ifndef __VMLINUX_H__
#ifdef __linux__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
#endif
#endif

#define LATENCY_BUCKETS 64
#define LATENCY_MAX_TRACKED_TASKS 131072
#define LATENCY_MAX_TRACKED_REQUESTS 65536

enum latency_kind {
    LATENCY_SCHED = 0,
    LATENCY_BLOCK = 1,
    LATENCY_KIND_COUNT = 2,
};

/* Per-CPU, cumulative. Bucket 0 contains 0..1 ns; bucket i contains
 * 2^i..2^(i+1)-1 ns, including UINT64_MAX in bucket 63. No reset while attached.
 * Readers must merge every possible CPU, including CPUs currently offline.
 * An active-map snapshot is not transactional across fields or CPUs.
 * max_ns is a lifetime maximum, and cannot be subtracted for interval reports.
 */
struct latency_histogram {
    __u64 buckets[LATENCY_BUCKETS];
    __u64 samples;
    __u64 total_ns;
    __u64 max_ns;
};

/* One per-CPU u64 ARRAY entry per diagnostic. These are cumulative counters.
 * Unmatched scheduler switches include tasks already runnable at attachment.
 * Unmatched block completions are counted ONLY without PID/cgroup filters;
 * otherwise unrelated requests are expected to have no start-map entry.
 */
enum latency_diagnostic {
    LATENCY_DIAG_SCHED_START_DROPPED = 0,
    LATENCY_DIAG_SCHED_UNMATCHED,
    LATENCY_DIAG_SCHED_DUPLICATE_WAKEUP,
    LATENCY_DIAG_SCHED_READ_FAILED,
    LATENCY_DIAG_SCHED_CGROUP_UNAVAILABLE,
    LATENCY_DIAG_SCHED_EXIT_CLEANUP,
    LATENCY_DIAG_BLOCK_START_DROPPED,
    LATENCY_DIAG_BLOCK_UNMATCHED,
    LATENCY_DIAG_BLOCK_REISSUE,
    LATENCY_DIAG_BLOCK_PARTIAL_COMPLETION,
    LATENCY_DIAG_BLOCK_CGROUP_UNAVAILABLE,
    LATENCY_DIAG_BLOCK_COMPLETION_ERROR,
    LATENCY_DIAG_BLOCK_STALE_START,
    LATENCY_DIAG_BLOCK_READ_FAILED,
    LATENCY_DIAG_CLOCK_REGRESSION,
    LATENCY_DIAG_COUNT,
};

#endif
