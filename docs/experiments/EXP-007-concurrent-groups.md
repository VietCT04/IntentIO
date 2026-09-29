# EXP-007 — concurrent groups and executor overhead

## Question

Can one `Runtime` execute multiple independent groups concurrently without
serializing submissions at the runtime boundary, while preserving planner
independence, global in-flight limits, adaptive control, error isolation, and
bounded latency for latency-sensitive work?

The experiment also asks whether the executor itself—not the planner or the
storage device—is the limiting cost for tiny synthetic tasks.

## Baseline profile

Phase 6 used a Runtime-wide execution mutex. That made the threaded executor
concurrent inside one group, but serialized independent `Group::submit()` calls
at the Runtime boundary. Before changing the queue data structures, the
executor-only synthetic benchmark used invalid file descriptors and 1-byte
reads, so the measurement was dominated by scheduling and completion overhead.

On the recorded host, `perf stat` for 2,048 synthetic tasks reported 18.51 ms
elapsed time, 36.49 ms task-clock, about 25.86 million cycles, and about 60.54
million instructions. The perf context-switch counter was zero for this short
run, so process `getrusage` context-switch fields in the CSV are the more useful
cross-run comparison. The executor metrics showed execution time in the low
millisecond range while queue wait and completion dispatch were tens of
milliseconds, which justified measuring completion batching.

## Design

The implementation separates three lifetimes and responsibilities:

```text
Runtime shared state
  ├─ planner and policy metrics
  ├─ one physical-work scheduler/backend
  └─ one adaptive controller observing all completed groups

SubmissionState per group
  ├─ physical plan and logical result storage
  ├─ progress, in-flight, and latency accounting
  └─ completion condition variable/callback
```

`Runtime::submit(Group&)` returns a move-only `Submission`. The threaded
backend owns persistent workers, and `Submission::get()` waits only for that
submission. `Group::submit()` remains the blocking compatibility wrapper.

The scheduler has one global active-work deque. Workers pull physical reads
round-robin across active submissions, so no group owns a private worker quota.
The effective `ExecutionPolicy::max_in_flight` is global across all groups.
Latency-sensitive submissions receive bounded priority bursts (at most four
before a normal submission is selected when normal work is available); this is
a fairness guard, not an unbounded priority promise.

The adaptive controller observes completed work through the shared Runtime
state. Its window counter, EWMA, rate, and effective concurrency therefore
continue across groups and across changing workload phases instead of being
reset for every submission.

## Executor metrics

The threaded executor now records, cumulatively and in the current snapshot:

- tasks submitted and completed;
- current and maximum queued depth;
- queue-wait, physical execution, and completion-dispatch nanoseconds;
- worker wakeups and spurious wakeups;
- worker busy/idle observations; and
- peak global in-flight tasks.

The metrics are intentionally diagnostic. They are not presented as device
latency, and queue wait is separated from the measured `pread` duration.

## Queue optimization decision

Profiling showed that completion dispatch was material for tiny tasks, so the
executor implements Option A: an opt-in completion batch size. The default is
still `RuntimeOptions::executor_batch_size = 1` to preserve the lowest-latency
behavior while the policy is experimental. Batches are capped by the global
in-flight limit and callback-before-ready ordering is preserved.

The synthetic run used 8 workers, global QD 8, 4 producers, 32 groups, and 64
tasks per group:

| batch | wall ms | tasks/s | completion p50 ms | CPU s | dispatch ns |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 15.812 | 129,519 | 14.198 | 0.030 | 69,089,404 |
| 2 | 9.701 | 211,105 | 7.379 | 0.020 | 30,126,172 |
| 4 | 8.536 | 239,916 | 5.696 | 0.017 | 20,585,116 |
| 8 | 10.002 | 204,766 | 7.754 | 0.019 | 4,237,667 |
| 16 | 11.499 | 178,097 | 8.999 | 0.023 | 4,163,039 |

Batch 4 was best for this synthetic point. Larger batches reduce dispatch
accounting but increase producer and completion latency because a worker can
hold more work before the next completion boundary. The result supports an
opt-in knob and further tuning, not a universal default change. The compact
batch CSV is [`phase7-batch-2026-09-29.csv`](../../benchmarks/results/phase7-batch-2026-09-29.csv).

## Concurrent-group correctness

The test suite now covers:

- multiple producers submitting independent groups to one Runtime;
- one invalid-descriptor submission completing with an error while valid and
  large submissions continue;
- a submission surviving destruction of its Runtime through the threaded
  backend lifetime handle;
- randomized differential checking against direct `pread` results;
- global in-flight accounting and peak queue depth; and
- construct/submit/destroy lifecycle behavior.

The scheduler completes callbacks before publishing the submission-ready state.
That ordering is required: otherwise a caller can destroy its last Runtime
owner immediately after `wait()` while the worker is still executing the
Runtime completion callback.

Strict C++20 builds passed with `-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Werror`. AddressSanitizer/UBSan and ThreadSanitizer test runs also
passed after the batching change. The planner-only benchmark still builds and
runs independently; no planner data structure or merge policy was changed in
this phase.

## Fixed concurrent-group matrix

Each row used 128 workers, 4 producers, 8 groups, 64 requests per group, the
threaded backend, batch size 1, and a sparse temporary file. The full compact
matrix is [`phase7-fixed-2026-09-29.csv`](../../benchmarks/results/phase7-fixed-2026-09-29.csv).

| workload | best observed p50 | QD | observation |
| --- | ---: | ---: | --- |
| random 4 KiB | 3.474 ms | 64 | high QD helped this run, but QD 128 was slower |
| random 64 KiB | 8.361 ms | 32 | p95 varied more than p50 at higher QD |
| sequential 1 MiB | 199.278 ms | 32 | storage/read path dominated; no monotonic scaling |
| mixed sizes | 21.220 ms | 32 | QD 2–32 materially beat QD 1 |

The matrix does not support a single queue-depth recommendation. The executor
metrics make queueing and completion work visible, but the sparse-file page
cache and shared host scheduling remain confounders.

## Producer scaling

The producer matrix covers producers 1, 2, 4, 8, and 16 and worker counts 1,
2, 4, 8, 16, 32, 64, and 128. The full synthetic executor CSV is
[`phase7-executor-2026-09-29.csv`](../../benchmarks/results/phase7-executor-2026-09-29.csv).

At 8 workers, increasing producers from 1 to 16 produced 140,324, 132,098,
134,103, 111,835, and 130,395 tasks/s respectively. The p50 completion latency
was 9.650, 12.580, 13.457, 17.293, and 14.395 ms. More producer threads did
not improve this tiny-task workload; queue depth and scheduler contention rose.
The 1..128 worker sweep is similarly non-monotonic and reaches higher CPU and
context-switch cost at the largest worker counts.

## Adaptive and changing workloads

One Runtime was retained through four sustained phases with 16 workers, max QD
16, 4 producers, 32 groups, and 128 requests per group. The controller window
counter continued from 32 to 64 to 96 to 128, demonstrating global rather than
per-group reset behavior:

| phase | wall ms | p50 / p95 ms | current / peak QD | EWMA ms | completion rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| random 4 KiB | 81.043 | 71.610 / 73.389 | 1 / 2 | 0.002 | 1,882.044 |
| sequential | 5,096.613 | 3,742.558 / 3,888.201 | 2 / 2 | 0.155 | 37.808 |
| random 64 KiB | 266.948 | 193.334 / 205.712 | 1 / 2 | 0.011 | 721.418 |
| mixed | 534.698 | 422.672 / 445.115 | 1 / 2 | 0.019 | 331.113 |

The controller is conservative on this host and workload mix. That is useful
evidence of stability, not proof of optimality. The time-series output preserves
queue depth and controller observations for later tuning. The compact phase
summary is [`phase7-changing-2026-09-29.csv`](../../benchmarks/results/phase7-changing-2026-09-29.csv).

## Fairness and error isolation

The fairness run submitted one large throughput group followed by 16 small
groups. With 8 workers, QD 4, and batch size 1, all 16 small groups completed
before the large group in both the latency-sensitive and default cases. The
latency-sensitive p50/p95 was 0.056/0.129 ms versus 0.059/0.090 ms for the
default case. This run demonstrates no starvation, but it does not establish a
latency win for priority; the result is appropriately treated as a bounded
fairness comparison. See [`phase7-fairness-2026-09-29.csv`](../../benchmarks/results/phase7-fairness-2026-09-29.csv).

Invalid descriptors are isolated to their own `ReadResult` errors. They do not
cancel unrelated submissions, and lifecycle tests verify that worker shutdown
joins cleanly. The threaded tests pass under ThreadSanitizer in this
environment.

## Sparse versus materialized files

The Phase 7 benchmark has `--materialize-file`, which writes every file byte
before measuring, in addition to its default sparse `ftruncate` file. The
recorded compact matrices use sparse files and label their cache state
`warm-buffered`: normal `pread` still populates and uses the page cache. Neither
mode is a cold-cache or device-isolation benchmark. Materialization makes file
allocation and first-touch behavior less synthetic, but does not remove page
cache effects; cold-cache claims require a separately controlled host.

## Decision

**KEEP the global multi-group threaded scheduler, with limitations.** The
Runtime-wide group serialization was removed, independent submissions work,
global in-flight accounting is enforced, adaptive state is shared across
groups, and fairness/error/lifecycle behavior is validated. The executor
metrics and profile justify retaining the opt-in completion batch knob, but the
default remains batch 1 because larger batches trade dispatch overhead for
latency and are workload-dependent. The planner remains independent and was
not optimized in this phase.

io_uring could not be validated on this host because `io_uring_setup` returned
`EPERM`; therefore no io_uring performance conclusion is drawn. CMake was also
unavailable in this container, so validation used direct strict compiler
invocations; the CMake target and host benchmark script were updated for a
normal Linux host.

## Next phase

**E — io_uring-specific optimization after host validation.** First validate
native io_uring on a permitted Linux host, then profile its submission and
completion path against this threaded baseline. This phase does not implement
that optimization.
