/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include "latency_shared.h"

/* Type flavors cover optional structs missing from vmlinux.h.
 * libbpf matches names before "___" and relocates field accesses. */
struct kernfs_node___latency {
    __u64 id;
} __attribute__((preserve_access_index));

struct cgroup___latency {
    struct kernfs_node___latency *kn;
} __attribute__((preserve_access_index));

struct css_set___latency {
    struct cgroup___latency *dfl_cgrp;
} __attribute__((preserve_access_index));

struct task_struct___latency {
    int pid;
    int tgid;
    struct css_set___latency *cgroups;
} __attribute__((preserve_access_index));

struct cgroup_subsys_state___latency {
    struct cgroup___latency *cgroup;
} __attribute__((preserve_access_index));

struct blkcg___latency {
    struct cgroup_subsys_state___latency css;
} __attribute__((preserve_access_index));

struct blkcg_gq___latency {
    struct blkcg___latency *blkcg;
} __attribute__((preserve_access_index));

struct bio___latency {
    struct blkcg_gq___latency *bi_blkg;
} __attribute__((preserve_access_index));

struct request___latency {
    unsigned int __data_len;
    struct bio___latency *bio;
    __u64 start_time_ns;
} __attribute__((preserve_access_index));

char LICENSE[] SEC("license") = "GPL";

/* Set host TGID and cgroup v2 ID before loading. Both filters must match. */
const volatile __u32 target_tgid = 0;
const volatile __u64 target_cgroup_id = 0;

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, LATENCY_KIND_COUNT);
    __type(key, __u32);
    __type(value, struct latency_histogram);
} histograms SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, LATENCY_DIAG_COUNT);
    __type(key, __u32);
    __type(value, __u64);
} diagnostics SEC(".maps");

/* Shared across CPUs for task migration. The exit hook cleans up before reuse. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, LATENCY_MAX_TRACKED_TASKS);
    __type(key, __u64);
    __type(value, __u64);
} sched_starts SEC(".maps");

struct block_start {
    __u64 timestamp_ns;
    __u64 allocation_ns;
};

/* HASH reports capacity failures without evicting in-flight I/O.
 * Request pointers distinguish concurrent requests to the same sector. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, LATENCY_MAX_TRACKED_REQUESTS);
    __type(key, __u64);
    __type(value, struct block_start);
} block_starts SEC(".maps");

static __always_inline void diagnose(__u32 reason)
{
    __u64 *value = bpf_map_lookup_elem(&diagnostics, &reason);
    if (value)
        __sync_fetch_and_add(value, 1);
}

static __always_inline __u32 bucket_for_ns(__u64 value)
{
    __u32 index = 0;
    if (value >> 32) { value >>= 32; index += 32; }
    if (value >> 16) { value >>= 16; index += 16; }
    if (value >> 8)  { value >>= 8;  index += 8; }
    if (value >> 4)  { value >>= 4;  index += 4; }
    if (value >> 2)  { value >>= 2;  index += 2; }
    if (value >> 1)  { index += 1; }
    return index;
}

static __always_inline void record_latency(__u32 kind, __u64 start, __u64 now)
{
    if (now < start) {
        diagnose(LATENCY_DIAG_CLOCK_REGRESSION);
        return;
    }

    struct latency_histogram *hist = bpf_map_lookup_elem(&histograms, &kind);
    if (!hist)
        return;

    __u64 delta = now - start;
    __u32 bucket = bucket_for_ns(delta);
    if (bucket >= LATENCY_BUCKETS)
        bucket = LATENCY_BUCKETS - 1;
    __sync_fetch_and_add(&hist->buckets[bucket], 1);
    __sync_fetch_and_add(&hist->total_ns, delta);
    __sync_fetch_and_add(&hist->samples, 1);
    /* Each kind has one writer per CPU, with recursion and preemption blocked. */
    if (delta > hist->max_ns)
        hist->max_ns = delta;
}

static __always_inline bool task_matches(struct task_struct___latency *task)
{
    __u32 pid = 0;
    if (BPF_CORE_READ_INTO(&pid, task, pid)) {
        diagnose(LATENCY_DIAG_SCHED_READ_FAILED);
        return false;
    }
    if (pid == 0) /* Skip idle tasks. */
        return false;

    if (target_tgid) {
        __u32 tgid = 0;
        if (BPF_CORE_READ_INTO(&tgid, task, tgid)) {
            diagnose(LATENCY_DIAG_SCHED_READ_FAILED);
            return false;
        }
        if (tgid != target_tgid)
            return false;
    }

    if (target_cgroup_id) {
        __u64 cgroup_id = 0;
        /* The waker may belong to a different cgroup than the queued task. */
        if (BPF_CORE_READ_INTO(&cgroup_id, task, cgroups, dfl_cgrp, kn, id) ||
            !cgroup_id) {
            diagnose(LATENCY_DIAG_SCHED_CGROUP_UNAVAILABLE);
            return false;
        }
        if (cgroup_id != target_cgroup_id)
            return false;
    }
    return true;
}

static __always_inline int enqueue_task(struct task_struct___latency *task, __u64 now)
{
    if (!task_matches(task))
        return 0;

    __u64 key = (__u64)task;
    /* Keep the first wakeup timestamp. */
    long result = bpf_map_update_elem(&sched_starts, &key, &now, BPF_NOEXIST);
    if (result == -17) /* -EEXIST */
        diagnose(LATENCY_DIAG_SCHED_DUPLICATE_WAKEUP);
    else if (result)
        diagnose(LATENCY_DIAG_SCHED_START_DROPPED);
    return 0;
}

SEC("raw_tp/sched_wakeup")
int on_sched_wakeup(struct bpf_raw_tracepoint_args *ctx)
{
    return enqueue_task((struct task_struct___latency *)ctx->args[0], bpf_ktime_get_ns());
}

SEC("raw_tp/sched_wakeup_new")
int on_sched_wakeup_new(struct bpf_raw_tracepoint_args *ctx)
{
    return enqueue_task((struct task_struct___latency *)ctx->args[0], bpf_ktime_get_ns());
}

SEC("raw_tp/sched_switch")
int on_sched_switch(struct bpf_raw_tracepoint_args *ctx)
{
    /* Linux 6.x passes preempt, prev, next, then prev_state.
     * The saved state avoids racing a reread of prev->__state. */
    bool preempt = ctx->args[0];
    struct task_struct___latency *prev = (struct task_struct___latency *)ctx->args[1];
    struct task_struct___latency *next = (struct task_struct___latency *)ctx->args[2];
    __u32 prev_state = (__u32)ctx->args[3];
    __u64 now = bpf_ktime_get_ns();

    /* Preempted or yielding tasks can run again without another wakeup. */
    if (preempt || prev_state == 0)
        enqueue_task(prev, now);

    __u64 key = (__u64)next;
    __u64 *timestamp = bpf_map_lookup_elem(&sched_starts, &key);
    if (!timestamp) {
        if (task_matches(next))
            diagnose(LATENCY_DIAG_SCHED_UNMATCHED);
        return 0;
    }
    __u64 start = *timestamp;
    bpf_map_delete_elem(&sched_starts, &key);
    record_latency(LATENCY_SCHED, start, now);
    return 0;
}

SEC("raw_tp/sched_process_exit")
int on_sched_process_exit(struct bpf_raw_tracepoint_args *ctx)
{
    __u64 key = ctx->args[0];
    if (bpf_map_delete_elem(&sched_starts, &key) == 0)
        diagnose(LATENCY_DIAG_SCHED_EXIT_CLEANUP);
    return 0;
}

static __always_inline bool request_matches(struct request___latency *request)
{
    /* The issuer may be a kernel worker rather than the submitting process. */
    if (target_tgid && (__u32)(bpf_get_current_pid_tgid() >> 32) != target_tgid)
        return false;

    if (target_cgroup_id) {
        __u64 cgroup_id = 0;
        /* Use the first bio's owner. Flushes without a bio have unknown ownership. */
        if (BPF_CORE_READ_INTO(&cgroup_id, request, bio, bi_blkg, blkcg,
                              css.cgroup, kn, id) || !cgroup_id) {
            diagnose(LATENCY_DIAG_BLOCK_CGROUP_UNAVAILABLE);
            return false;
        }
        if (cgroup_id != target_cgroup_id)
            return false;
    }
    return true;
}

SEC("raw_tp/block_rq_issue")
int on_block_issue(struct bpf_raw_tracepoint_args *ctx)
{
    struct request___latency *request = (struct request___latency *)ctx->args[0];
    __u64 key = (__u64)request;
    struct block_start value = { .timestamp_ns = bpf_ktime_get_ns() };
    if (BPF_CORE_READ_INTO(&value.allocation_ns, request, start_time_ns)) {
        diagnose(LATENCY_DIAG_BLOCK_READ_FAILED);
        return 0;
    }

    struct block_start *existing = bpf_map_lookup_elem(&block_starts, &key);
    if (existing) {
        if (existing->allocation_ns == value.allocation_ns) {
            /* Reissues keep the first timestamp and filter decision. */
            diagnose(LATENCY_DIAG_BLOCK_REISSUE);
            return 0;
        }
        /* Drop a reused address. Reuse is undetectable when timestamp
         * accounting is disabled and allocation_ns stays zero. */
        bpf_map_delete_elem(&block_starts, &key);
        diagnose(LATENCY_DIAG_BLOCK_STALE_START);
    }

    if (!request_matches(request))
        return 0;
    if (bpf_map_update_elem(&block_starts, &key, &value, BPF_NOEXIST))
        diagnose(LATENCY_DIAG_BLOCK_START_DROPPED);
    return 0;
}

SEC("raw_tp/block_rq_complete")
int on_block_complete(struct bpf_raw_tracepoint_args *ctx)
{
    /* Linux 6.x passes rq, error, then nr_bytes. */
    struct request___latency *request = (struct request___latency *)ctx->args[0];
    __u64 key = (__u64)request;
    struct block_start *found = bpf_map_lookup_elem(&block_starts, &key);
    if (!found) {
        if (!target_tgid && !target_cgroup_id)
            diagnose(LATENCY_DIAG_BLOCK_UNMATCHED);
        return 0;
    }
    struct block_start start = *found;
    __u64 allocation_ns = 0;
    __u32 remaining = 0;
    struct bio___latency *bio = NULL;
    if (BPF_CORE_READ_INTO(&allocation_ns, request, start_time_ns) ||
        BPF_CORE_READ_INTO(&remaining, request, __data_len) ||
        BPF_CORE_READ_INTO(&bio, request, bio)) {
        bpf_map_delete_elem(&block_starts, &key);
        diagnose(LATENCY_DIAG_BLOCK_READ_FAILED);
        return 0;
    }
    if (allocation_ns != start.allocation_ns) {
        bpf_map_delete_elem(&block_starts, &key);
        diagnose(LATENCY_DIAG_BLOCK_STALE_START);
        return 0;
    }

    if (ctx->args[1])
        diagnose(LATENCY_DIAG_BLOCK_COMPLETION_ERROR);

    /* This fires before blk_update_request consumes nr_bytes, even on errors.
     * Wait for all bytes unless this is a no-bio or zero-byte request. */
    if (bio && (__u32)ctx->args[2] < remaining) {
        diagnose(LATENCY_DIAG_BLOCK_PARTIAL_COMPLETION);
        return 0;
    }
    bpf_map_delete_elem(&block_starts, &key);
    record_latency(LATENCY_BLOCK, start.timestamp_ns, bpf_ktime_get_ns());
    return 0;
}
