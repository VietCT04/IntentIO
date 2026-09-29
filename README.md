# IntentIO

IntentIO is a Linux-only C++20 I/O runtime for expressing application intent
and executing a group of file reads as one planned operation.

The first release focuses on a small, testable foundation:

- owned, offset-based file reads with explicit per-request errors;
- synchronous and optional native `io_uring` backends;
- logical I/O groups separated from physical backend operations;
- conservative adjacent-read coalescing, including configurable gaps and a
  maximum merged request size;
- exact duplicate elimination and bounded overlap reuse within a group;
- intent-aware planning for access pattern, reuse, and latency class;
- a reproducible benchmark harness and experiment notes.

IntentIO is not a replacement for raw `io_uring`. It adds a planning layer
that can understand that several application requests belong together, while
keeping the public API independent of Linux's io_uring structures.

## Requirements

- Linux
- C++20 compiler
- CMake 3.20 or newer

The native io_uring backend uses the Linux kernel UAPI directly and therefore
does not require liburing. `Backend::Auto` falls back to synchronous reads if
the running kernel or sandbox does not allow `io_uring_setup`.

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
public API -> logical group -> planner -> physical reads -> backend
    +-> intent hints -> policy resolver -> range planner
                                              |       |
                                              |       +-- deduplicate overlaps
                                              +---------- coalesce gaps
                                                      |
                                      physical reads -> backend
```

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

The current public result type owns an independent `std::vector<std::byte>`
for each logical request. This preserves simple, safe API semantics; the
planner still avoids redundant physical I/O, while copy cost is reported as
`result_copy_bytes` by the Phase 4 benchmark.

## Feature status

Implemented: synchronous reads, optional native io_uring execution, logical
groups, adjacent coalescing, exact deduplication, contained and partial
overlap reuse, metrics, tests, and benchmarks.

Intent-aware policy, amplification budgets, merge-decision metrics, and plan
tracing are implemented and documented in EXP-005.

Experimental: io_uring performance comparisons require a host that permits
`io_uring_setup`; the current development container returns `EPERM`.

Planned: intent-driven cache policy, adaptive concurrency, prefetching,
O_DIRECT policy, and coroutine APIs.

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
./build/intentio_planner_benchmark

# On a normal Linux host, collect the full matrix and environment metadata.
./scripts/run_host_benchmarks.sh
```

The benchmark reports median, p50, p95, p99 latency, throughput, CPU time,
logical and physical request/byte counts, deduplication savings, read
amplification, and result-copy bytes. The planner-only benchmark performs no
disk I/O and reports nanoseconds per request for groups from 8 through 8192.
These tools use temporary files and do not claim that an optimization is
beneficial without measured output. Captured results and interpretation live
under `benchmarks/results/` and `docs/experiments/`.

## Current limitations

- `Group::submit()` is synchronous in this milestone, including when the
  io_uring backend is selected; coroutine integration is a later phase.
- Adaptive queue depth, prefetching, userspace caching, direct-I/O policy, and
  coroutine APIs are not implemented.
- The io_uring backend is intentionally small and uses one runtime-owned ring.
- Benchmark numbers are machine- and filesystem-dependent.

## License

IntentIO is released under the MIT license. See [LICENSE](LICENSE).
