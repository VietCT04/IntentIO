# IntentIO

IntentIO is a Linux-only C++20 I/O runtime for expressing application intent
and executing a group of file reads as one planned operation.

The first release focuses on a small, testable foundation:

- owned, offset-based file reads with explicit per-request errors;
- synchronous, bounded threaded-`pread`, and optional native `io_uring`
  backends;
- logical I/O groups separated from physical backend operations;
- conservative adjacent-read coalescing, including configurable gaps and a
  maximum merged request size;
- exact duplicate elimination and bounded overlap reuse within a group;
- intent-aware planning for access pattern, reuse, and latency class;
- backend-independent fixed execution limits and experimental adaptive
  concurrency;
- nonblocking `Runtime::submit(Group&)` submissions with independent result
  handles;
- a reproducible benchmark harness and experiment notes.

IntentIO is not a replacement for raw `io_uring`. It adds a planning layer
that can understand that several application requests belong together, while
keeping the public API independent of Linux's io_uring structures.

## Requirements

- Linux
- C++20 compiler
- CMake 3.20 or newer

The native io_uring backend uses the Linux kernel UAPI directly and therefore
does not require liburing. `Backend::Auto` prefers io_uring and falls back to
the bounded threaded-`pread` backend if the running kernel or sandbox does not
allow `io_uring_setup`.

## Build and test

```sh
cmake -S . -B build -DINTENTIO_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizers are available as opt-in CMake configurations:

```sh
cmake -S . -B build-asan -DINTENTIO_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DINTENTIO_ENABLE_TSAN=ON
```

The ThreadSanitizer configuration is intended for supported toolchains and is
separate from AddressSanitizer/UndefinedBehaviorSanitizer.

## Basic example

```cpp
#include <intentio/intentio.hpp>

#include <iostream>

int main() {
    intentio::Runtime io;
    auto group = io.create_group();

    group.read(fd, 0, 256 * 1024);
    group.read(fd, 256 * 1024, 256 * 1024);
    group.access_pattern(intentio::AccessPattern::Sequential);
    group.reuse_hint(intentio::ReuseHint::ScanOnce);
    group.latency_class(intentio::LatencyClass::Throughput);

    const auto results = group.submit();
    for (const auto& result : results) {
        if (!result.ok()) {
            std::cerr << "request " << result.request_id << ": "
                      << result.error.message() << '\n';
            return 1;
        }
        std::cout << "request " << result.request_id << " read "
                  << result.bytes_read() << " bytes\n";
    }
}
```

Results own their bytes, and a short successful result represents EOF rather
than an error. Invalid descriptors and backend failures are reported in the
corresponding `ReadResult::error`.

## Architecture

```text
                         IntentIO Runtime

Group A ----┐
Group B ----+--> planner --> physical I/O scheduler
Group C ----┘                    |
                         adaptive/fixed concurrency
                            +-----+-----+
                            |           |
                     threaded pread  io_uring
```

Planning determines what physical I/O exists, scheduling determines which
ready request runs next, and concurrency determines how many requests may be
in flight globally. The threaded backend uses a runtime-owned worker set and a
fair pull-based scheduler, so independent groups can overlap without giving
each group its own queue-depth budget.

Logical and physical I/O are deliberately separate. For example:

```text
Application issues:

R1 [0, 1MB)
R2 [256KB, 512KB)
R3 [512KB, 768KB)

         |
         v

IntentIO Planner

         |
         v

P1 [0, 1MB)

         |
         v

one physical read; three logical result slices
```

The planner sorts requests by file descriptor and offset, then builds bounded
coverage ranges. Exact duplicates and overlapping ranges can share a physical
read when deduplication is enabled. Adjacent and small-gap ranges are handled
by the separate coalescing policy. `max_request_bytes` limits both union and
coalescing growth, so distant requests cannot become one pathological read.

Intent is advisory. The resolved policy always enforces a maximum merged size
and read-amplification budget. Use `INTENTIO_TRACE_PLAN=1` or
`RuntimeOptions::trace_plan` to print the selected policy and merge decisions.

`Group::submit()` remains a blocking compatibility API. For concurrent
application work, submit several groups through one runtime and wait on their
independent handles:

```cpp
auto a = runtime.submit(group_a);
auto b = runtime.submit(group_b);
auto c = runtime.submit(group_c);

const auto results_a = a.get();
const auto results_b = b.get();
const auto results_c = c.get();
```

The current public result type owns an independent `std::vector<std::byte>`
for each logical request. This preserves simple, safe API semantics; the
planner still avoids redundant physical I/O, while copy cost is reported as
`result_copy_bytes` by the Phase 4 benchmark.

## Feature status

Implemented: synchronous reads, bounded threaded-`pread` execution, global
multi-group scheduling, independent submissions, fairness-aware latency
priority, optional native io_uring execution, logical groups, adjacent
coalescing, exact deduplication, contained and partial overlap reuse, metrics,
tests, and benchmarks.

Intent-aware policy, amplification budgets, merge-decision metrics, and plan
tracing are implemented and documented in EXP-005.

Experimental: adaptive concurrency and io_uring performance comparisons
require a host that permits the relevant backend. The current development
container returns `EPERM` from `io_uring_setup`; `Backend::Auto` therefore uses
threaded `pread` here. The adaptive controller is deliberately conservative
and is not expected to improve every workload.

Planned: intent-driven cache policy, prefetching, O_DIRECT policy, zero-copy
result storage, and coroutine APIs.

## Benchmarks

Build the benchmark target and run, for example:

```sh
./build/intentio_benchmark --workload sequential --backend sync
./build/intentio_benchmark --workload adjacent --coalesce off
./build/intentio_benchmark --workload adjacent --coalesce on
./build/intentio_benchmark --workload random --backend auto

./build/intentio_phase4_benchmark --workload duplicates --dedup on
./build/intentio_phase4_benchmark --workload duplicates --dedup off
./build/intentio_phase4_benchmark --workload partial --requests 128
./build/intentio_phase5_benchmark --workload all --intent all
./build/intentio_phase6_benchmark --workload all --backend threaded --mode fixed
./build/intentio_phase6_benchmark --workload all --backend threaded --mode adaptive
./build/intentio_phase7_benchmark --mode executor --workers 8 --producers 4
./build/intentio_phase7_benchmark --mode executor --workers 8 --qd 8 \
  --producers 4 --executor-batch-size 4
./build/intentio_phase7_benchmark --mode adaptive --workload random4k \
  --workers 32 --max-qd 32 --producers 4 --groups 128 --requests 256
./build/intentio_phase7_benchmark --mode changing --workers 32 --max-qd 32
./build/intentio_phase7_benchmark --mode fairness --workers 8 --qd 8
./build/intentio_planner_benchmark

# On a normal Linux host, collect the full matrix and environment metadata.
./scripts/run_host_benchmarks.sh
```

The benchmark reports median, p50, p95, p99 latency, throughput, CPU time,
logical and physical request/byte counts, deduplication savings, read
amplification, and result-copy bytes. The Phase 6 benchmark additionally
reports fixed queue-depth matrices, adaptive controller windows, EWMA latency,
completion rate, concurrency evolution, CPU time, and context switches. The
Phase 7 benchmark adds synthetic executor load, producer scaling, concurrent
group fixed/adaptive runs, changing-workload time series, fairness comparison,
queue wait, execution, wake-up, and completion-dispatch metrics, configurable
completion batches (measured at sizes 1, 2, 4, 8, and 16), plus sparse
versus materialized file metadata. The
planner-only benchmark performs no disk I/O and reports nanoseconds per request
for groups from 8 through 8192. These tools use temporary files and do not
claim that an optimization is beneficial without measured output. Captured
results and interpretation live under `benchmarks/results/` and
`docs/experiments/`.

## Current limitations

- `Group::submit()` is still blocking; `Runtime::submit(Group&)` is the
  nonblocking submission API for the threaded executor. Synchronous and
  currently unvalidated io_uring execution retain their blocking backend path.
- The global scheduler applies `RuntimeOptions::execution.max_in_flight` across
  all groups, not once per group. Latency-sensitive work receives bounded
  priority bursts and cannot permanently starve normal work.
- Prefetching, userspace caching, direct-I/O policy, zero-copy result storage,
  and coroutine APIs are not implemented.
- The io_uring backend is intentionally small and uses one runtime-owned ring;
  multi-group scheduling is validated here on threaded `pread` because this
  environment rejects io_uring setup.
- Benchmark numbers are machine- and filesystem-dependent.

## License

IntentIO is released under the MIT license. See [LICENSE](LICENSE).
