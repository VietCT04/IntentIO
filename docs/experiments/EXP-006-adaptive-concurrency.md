# EXP-006 — concurrent execution and adaptive I/O concurrency

## Question

Can IntentIO execute independent physical reads concurrently while keeping the
execution layer independent from planning, and can a small controller adjust
the in-flight limit without making intent-aware planning unsafe?

The implementation separates the two decisions:

```text
logical requests -> planning policy -> physical ranges
                                  \
                                   -> execution policy -> backend concurrency
```

`ExecutionPolicy::max_in_flight` is a backend-independent fixed limit.
`AdaptiveExecutionOptions` selects an intent-aware controller preset, but it
does not change coalescing, deduplication, or the physical plan.

## Implementation

The threaded backend owns a persistent, runtime-scoped worker set. Workers
wait on a condition variable, execute `pread` for one physical request, and
replenish work from the completion path. The queue is bounded by the effective
in-flight limit, and the backend has no detached threads, global pool, or
per-request thread creation. Runtime destruction signals the workers and joins
them cleanly.

The native io_uring backend applies the same execution limit to its submission
batches. `Backend::Auto` tries io_uring first and selects threaded `pread` when
`io_uring_setup` is unavailable. A Runtime serializes calls to its state while
one group is executing; concurrency in this phase is within a physical-read
batch.

The adaptive controller uses an EWMA (alpha 0.25) of completion latency and
completion rate. After each observation window it additively increases the
limit while the baseline is healthy, halves it on latency growth or throughput
regression, and applies cooldown after a decrease. It ignores windows that do
not have enough physical work to exercise the current limit. Default intent
presets are:

| intent | max | latency growth limit | observation window | behavior |
| --- | ---: | ---: | ---: | --- |
| unknown | 32 | 1.50x | 16 | balanced probe |
| sequential | 64 | 1.50x | 16 | more willing to grow |
| random | 32 | 1.35x | 16 | conservative |
| latency-sensitive | 8 | 1.15x | 16 | quick backoff |
| throughput | 128 | 1.75x | 16 | most aggressive |

The fixed benchmark matrix uses a global limit of 1, 2, 4, 8, 16, 32, 64,
and 128 with 128 workers. The adaptive runs use a global maximum of 128.

## Test setup

The recorded host was Linux with GCC 15.2 and a sparse temporary file. The
benchmark process records `getrusage` CPU time and voluntary/involuntary
context switches in addition to median, p95, and p99 latency. Each fixed case
used 64 logical requests and 3 repetitions; adaptive cases used 16
repetitions, except the changing-workload phases, which used 8. The raw CSV is
[`phase6-threaded-2026-09-29.csv`](../../benchmarks/results/phase6-threaded-2026-09-29.csv).

io_uring was probed explicitly and returned `EPERM` from `io_uring_setup`, so
the recorded matrix is threaded `pread`. This is an environment limitation,
not evidence that the io_uring implementation is slower or faster.

## Fixed concurrency results

The table shows median latency in milliseconds for QD 1 through 128. The
minimum is workload-dependent and is not necessarily the largest queue depth.

| workload | QD 1 | QD 2 | QD 4 | QD 8 | QD 16 | QD 32 | QD 64 | QD 128 | best |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| random 4K | 1.087 | 1.070 | 1.112 | 0.729 | 0.794 | 0.712 | 0.658 | 0.516 | 0.516 @128 |
| random 64K | 5.344 | 4.454 | 4.639 | 4.798 | 3.099 | 5.119 | 4.260 | 2.660 | 2.660 @128 |
| large sequential | 81.648 | 77.622 | 72.215 | 76.593 | 73.328 | 75.331 | 75.566 | 73.320 | 72.215 @4 |
| intent-shaped, unknown | 4.708 | 4.649 | 3.492 | 4.565 | 5.216 | 3.524 | 3.453 | 2.732 | 2.732 @128 |
| mixed sizes | 9.969 | 9.274 | 9.731 | 8.983 | 9.315 | 8.586 | 7.972 | 9.524 | 7.972 @64 |

The results show useful concurrency for small and mixed reads, but no smooth
monotonic scaling. Sequential reads are already dominated by the storage path;
larger worker counts add CPU and scheduling work without a reliable latency
benefit.

## Adaptive results

| workload / intent | median ms | p95 ms | increases | decreases | peak | windows | average QD |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| random 4K / unknown | 0.748 | 1.099 | 8 | 5 | 4 | 16 | 1.875 |
| random 64K / unknown | 4.079 | 4.799 | 8 | 4 | 3 | 16 | 1.688 |
| sequential / unknown | 74.308 | 79.255 | 6 | 5 | 2 | 16 | 1.375 |
| intent-shaped / unknown | 3.784 | 5.589 | 7 | 5 | 3 | 16 | 1.562 |
| mixed / unknown | 9.063 | 12.182 | 8 | 4 | 3 | 16 | 1.688 |
| intent-shaped / sequential | 1.167 | 4.220 | 1 | 0 | 2 | 1 | 1.062 |
| intent-shaped / latency | 3.901 | 5.525 | 6 | 4 | 4 | 16 | 1.625 |
| intent-shaped / throughput | 1.866 | 4.673 | 1 | 0 | 2 | 1 | 1.062 |

The changing workload keeps one Runtime alive while moving through random 4K,
sequential, and random 64K phases. Its queue-depth evolutions were:

```text
random4k       2|1|1|2|3|1|1|2
sequential     2|3|1|1|2|1|1|2
random64k      2|3|4|5|6|7|3|1
```

The controller responds to changing observations, but the small-window
behavior is intentionally cautious and should not be interpreted as an
optimality guarantee.

## Planner × execution interaction

The intent-shaped workload uses 64 KiB reads separated by 16 KiB gaps. With
unknown intent, the planner rejects those gaps and produces 64 physical reads
per repetition. With sequential or throughput intent, the planner accepts
larger unions: the 16-repetition adaptive run reports 16 physical reads for
1024 logical reads, 83,623,936 physical bytes, and 1.246 read amplification.

That interaction is important: a more aggressive planner can reduce the
amount of work so much that the controller cannot observe a full concurrency
window. The controller therefore leaves the sequential and throughput cases at
QD 1 for most repetitions and only probes QD 2. Execution policy remains
separate from planning policy, and the benchmark exposes both metrics so a
faster result is not incorrectly attributed to concurrency alone.

## CPU and scheduling cost

Context switches rise and vary with queue depth. For example, the recorded
random-4K fixed cases range from 775 voluntary / 33 involuntary switches at QD
128 to 1200 / 38 at QD 4, while the sequential cases range from 552 / 118 at
QD 128 to 886 / 104 at QD 4. CPU time for sequential reads is also materially
higher than for 4K reads, reaching about 0.48 seconds for the QD 128 fixed
case. These are process-level `getrusage` measurements on a sparse temporary
file, so they are useful for detecting scheduler overhead but are not a device
benchmark.

## Correctness and race validation

- Strict C++20 builds with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Werror` passed for the library, tests, Phase 6 benchmark, and existing
  benchmark targets.
- The synchronous and threaded paths are compared against a naive `pread`
  oracle for invalid descriptors, EOF, duplicate descriptors, random offsets,
  zero-length reads, all planner intents, and concurrency 1/2/4/8.
- The adaptive controller is covered by repeated multi-window tests.
- A 1000-iteration construct/submit/destroy lifecycle test passed with one
  worker.
- AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer test runs
  both passed in this environment.
- io_uring correctness and performance remain unvalidated here because the
  kernel rejected ring creation with `EPERM`.

## Decision

**KEEP WITH LIMITATIONS.** The bounded threaded backend provides clean
concurrency and backend parity, and the adaptive controller is observable and
safe under changing workload and planner output. The matrix does not support a
universal queue-depth recommendation: storage behavior, workload shape, and
planner amplification dominate. The Runtime-level serialization and the
threaded queue's mutex/condition-variable path are the main remaining
execution costs.

Recommended next phase: **E — executor/worker-pool optimization**. Profile and
reduce queue-lock, wake-up, and worker handoff overhead before adding another
I/O feature. Do not implement that phase as part of EXP-006.
