# IntentIO

IntentIO is a Linux-only C++20 I/O runtime for expressing application intent
and executing a group of file reads as one planned operation.

The first release focuses on a small, testable foundation:

- owned, offset-based file reads with explicit per-request errors;
- synchronous and optional native `io_uring` backends;
- logical I/O groups separated from physical backend operations;
- conservative adjacent-read coalescing, including configurable gaps and a
  maximum merged request size;
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

    const auto first = group.read(fd, 0, 256 * 1024);
    const auto second = group.read(fd, 256 * 1024, 256 * 1024);
    group.hint(intentio::AccessPattern::Sequential);

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
                                      |                  +-- synchronous pread
                                      |                  +-- native io_uring
                                      +-> result slices
```

The planner is deliberately conservative. It merges only requests for the
same file descriptor that are ordered, non-overlapping, within the configured
gap, and below the configured physical request limit. Overlap elimination and
cache policy are later milestones, not hidden behavior in this release.

## Benchmarks

Build the benchmark target and run, for example:

```sh
./build/intentio_benchmark --workload sequential --backend sync
./build/intentio_benchmark --workload adjacent --coalesce off
./build/intentio_benchmark --workload adjacent --coalesce on
./build/intentio_benchmark --workload random --backend auto
```

The benchmark reports median, p50, p95, p99 latency, throughput, physical
request count, and physical bytes. It uses a temporary file and does not claim
that an optimization is beneficial without measured output. Captured results
and interpretation live under `benchmarks/results/` and `docs/experiments/`.

## Current limitations

- `Group::submit()` is synchronous in this milestone, including when the
  io_uring backend is selected; coroutine integration is a later phase.
- Request deduplication, overlapping-range reuse, adaptive queue depth,
  prefetching, userspace caching, and direct-I/O policy are not implemented.
- The io_uring backend is intentionally small and uses one runtime-owned ring.
- Benchmark numbers are machine- and filesystem-dependent.

## License

IntentIO is released under the MIT license. See [LICENSE](LICENSE).
