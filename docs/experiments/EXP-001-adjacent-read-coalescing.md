# EXP-001 — adjacent-read coalescing

## Hypothesis

Merging ordered, adjacent logical reads into one physical read should reduce
the number of `pread` operations for workloads where the application already
knows the requests are part of one sequential operation. It should not change
the logical result bytes, and it should not merge random requests.

## Implementation

The Phase 3 planner sorts requests by file descriptor and offset, then merges
only same-fd, non-overlapping ranges. The merge window is configurable and is
zero by default. A maximum physical request size prevents an unbounded merge.
Each physical request stores slices that map its bytes back to the original
logical result order.

## Test setup

- Linux kernel: `7.0.0-31-generic`
- backend: synchronous `pread`
- temporary file, 2 MiB plus guard space
- 256 logical reads per group, 4 KiB each
- 9 measured repetitions
- cold-cache state was not claimed; these are local development measurements
- compiler: GCC 15.2.0

Commands:

```sh
./build/intentio_benchmark --workload adjacent --backend sync \
  --requests 256 --block-size 4096 --repetitions 9 --coalesce on
./build/intentio_benchmark --workload adjacent --backend sync \
  --requests 256 --block-size 4096 --repetitions 9 --coalesce off
```

## Results

| Workload | Coalescing | Median | p95 | Throughput | Physical reads | Merged logical requests |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| adjacent | off | 1.233 ms | 1.237 ms | 811.325 MiB/s | 2304 | 0 |
| adjacent | on | 0.971 ms | 0.999 ms | 1029.825 MiB/s | 9 | 2295 |

The complete CSV is in
[`benchmarks/results/phase-0-3-sync-2026-09-29.csv`](../../benchmarks/results/phase-0-3-sync-2026-09-29.csv).

## Interpretation

This run reduced the measured median latency by approximately 21% for this
small adjacent workload and reduced physical operations by 256x. The result is
directional rather than a general performance claim: the file was temporary,
the cache state was not controlled, and the benchmark group was intentionally
adjacent.

Random requests remained at 2304 physical reads with coalescing enabled in the
same run set. Unit tests also compare every optimized result with the expected
independent-read bytes.

## Decision

**KEEP.** The optimization is correct, independently measurable, and useful on
the targeted adjacent-read workload. Continue to benchmark it against mixed
and device-backed workloads before broadening the merge policy.
