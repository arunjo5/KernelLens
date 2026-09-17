# How the probes work

`latency.bpf.c` reads kernel events through raw tracepoints. It uses a generated
`vmlinux.h` and CO-RE so libbpf can find fields in the running kernel's data
structures. Small type definitions describe only the fields the probes need.
The code does not depend on CPU register layouts and supports Linux 6.x on
x86-64 and ARM64.

The full feature set needs kernel BTF, `CONFIG_BPF_SYSCALL`, tracing, cgroups,
and block-cgroup accounting. CO-RE handles field layout changes, but the probes
still depend on the argument order of each raw tracepoint.

## Scheduling delay

- `sched_wakeup` and `sched_wakeup_new` start the timer for the task being
  awakened. `sched_switch` stops it when that task gets CPU time.
- If a task leaves the CPU while still runnable (`preempt || prev_state == 0`),
  another timer starts. This captures waits after preemption or yielding.
  Sleeping tasks get a new timer when they wake up.
- Start times are stored by task pointer in a shared map, so a task can move
  between CPUs. `sched_process_exit` removes any leftover entry before the
  kernel frees the task.
- The PID filter uses the waiting task's host process ID (TGID) and includes
  all its threads. The cgroup v2 filter also checks the waiting task, regardless
  of which task woke it up. If both filters are set, both must match. Cgroup
  matching is exact and does not include child cgroups.
- The result is time spent ready to run but waiting for CPU time. This includes
  waits caused by cgroup CPU throttling. It does not directly measure sleep,
  garbage collection pauses, mutex or connection-pool waits, or request latency.

## Block latency

- `block_rq_issue` records a start time by request pointer. `block_rq_complete`
  stops the timer when `nr_bytes` covers the remaining bytes, or when the
  request has no `bio` (the kernel's block I/O descriptor). The event arrives
  before the kernel reduces `rq->__data_len`. Partial completions keep the
  timer running, and zero-byte flushes can complete normally.
- If a tracked request is issued again, it keeps its original start time and
  filter decision. The measurement includes time spent requeued, but excludes
  anything before the first accepted issue. It is not the full duration of an
  application's `fsync()` call.
- A PREFLUSH/FUA request can emit a completion for its data operation, then
  another after the flush sequence without a new issue event. The data is
  counted once. The later completion has no matching start. Physical flushes
  issued as separate requests get their own measurements, and a data sample
  does not include a later postflush sequence.
- The PID filter uses the host process ID of whoever issues the request. It
  cannot recover the original process when a worker issues asynchronous I/O.
  An excluded request can still be tracked if a later issue matches the filter.
- The cgroup filter uses the first bio's block-cgroup owner. This can include
  asynchronous writeback, depending on the filesystem and kernel accounting.
  Requests with no recorded owner, including some flushes without a bio, are
  excluded and increment `BLOCK_CGROUP_UNAVAILABLE`. Run without filters to
  inspect those device flushes.
- The allocation timestamp helps detect a reused request address if a final
  completion was missed. When kernel timestamp accounting is disabled, this
  field can be zero and these two events cannot detect that reuse. Each
  observed final completion removes the stored start time.
- Failed completions remain in the histogram and are also counted separately.
  Unmatched completions are only counted when filters are off, since requests
  excluded by a filter have no stored start time. An unmatched event does not by itself
  mean a measurement was lost. Flush sequencing can cause one too.

## Maps and losses

Each CPU has cumulative histograms with 64 buckets whose ranges double in
nanoseconds. One BPF program writes each histogram. Readers must add values
from every possible CPU, including CPUs that are currently offline. A live
read can catch fields partway through an update. The final read is stable
after the probes detach. `max_ns` is the highest value seen over the whole run,
so subtracting two snapshots does not give an interval maximum.

The start-time hash maps hold up to 131,072 tasks and 65,536 block requests.
A full map rejects new starts and increments a counter instead of replacing
older entries. If dropped-start or read-failure counters rise, the histograms
are missing measurements. Unmatched events can also appear just after attaching
to work already in progress. Events are aggregated in the kernel without a
ring buffer or a userspace message for each event.

## Kernel references

- [Linux 6.1 scheduler tracepoints](https://github.com/torvalds/linux/blob/v6.1/include/trace/events/sched.h)
  and [Linux 6.12 scheduler tracepoints](https://github.com/torvalds/linux/blob/v6.12/include/trace/events/sched.h)
  define `sched_switch(preempt, prev, next, prev_state)` and wakeup events.
- [Linux 6.1 block tracepoints](https://github.com/torvalds/linux/blob/v6.1/include/trace/events/block.h)
  and [Linux 6.12 block tracepoints](https://github.com/torvalds/linux/blob/v6.12/include/trace/events/block.h)
  define the issue arguments as `rq` and completion arguments as
  `rq, error, nr_bytes`.
- [Linux 6.1 block completion implementation](https://github.com/torvalds/linux/blob/v6.1/block/blk-mq.c)
  and [Linux 6.12 block completion implementation](https://github.com/torvalds/linux/blob/v6.12/block/blk-mq.c)
  show that completion events arrive before bytes are consumed. They also
  show when allocation timestamps can be zero.
- [Linux 6.1 block cgroup implementation](https://github.com/torvalds/linux/blob/v6.1/block/blk-cgroup.h)
  covers first-bio ownership and how cgroups affect request merging.
- [Linux 6.8 flush sequencing](https://github.com/torvalds/linux/blob/v6.8/block/blk-flush.c)
  shows the separate data and sequence completions. `blk_flush_restore_request()`
  and `blk_flush_complete_seq()` perform the second completion without reissuing
  the data request. Physical flush requests are allocated separately.
