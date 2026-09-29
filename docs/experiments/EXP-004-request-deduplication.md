# EXP-004 — request deduplication and overlap reuse

## Hypothesis

Requests visible in one logical group can often share a physical read. Exact
duplicates should collapse to one read, contained subranges should reuse their
parent range, and partial overlaps should become one bounded union. Random
non-overlapping requests should not pay for unnecessary physical-range work.

## Existing behavior

Phase 3 represented each logical request as a physical read unless it was
ordered and adjacent (or within the configured gap). Overlapping ranges were
left independent. Logical results were already copied into independent owned
vectors, so the public ownership contract did not need to change.

## Design

The planner now performs one sorted same-fd range sweep. It distinguishes two
policies:

- `coalescing.enabled` merges non-overlapping ranges within `max_gap_bytes`;
- `deduplication.enabled` merges overlapping, contained, and exact-duplicate
  ranges.

Both policies obey `max_request_bytes`. Each physical range retains one slice
for every logical request, including requests submitted out of order. The
executor still copies each slice into its own `ReadResult::data`; this favors
safe, unsurprising ownership over an unmeasured zero-copy API change.

New metrics are cumulative per runtime:

```text
unique_logical_bytes
requests_deduplicated
physical_reads_avoided_by_dedup
bytes_avoided_by_dedup
read_amplification = physical_bytes / unique_logical_bytes
```

## Correctness validation

The test suite includes exact duplicates, contained ranges, partial overlaps,
EOF-crossing ranges, maximum-union limits, different file descriptors, and a
deterministic differential test:

```text
seed=482193
groups=3000
requests/group=1..24
```

Each randomized optimized result is compared against an independent `pread`
baseline for bytes, EOF short reads, request ordering, and errors. The test
suite passes under strict warnings and ASan/UBSan.

## Benchmark environment

- Linux `7.0.0-31-generic`
- GCC `15.2.0`
- synchronous `pread` backend
- temporary sparse files
- 5 repetitions for the captured Phase 4 samples
- io_uring unavailable in this container: `io_uring_setup` returns `EPERM`

Run the host matrix with:

```sh
./scripts/run_host_benchmarks.sh
```

That script records kernel, CPU, filesystem, storage source, io_uring
availability, benchmark CSVs, and planner-only CSVs. It prints an actionable
message and exits cleanly when CMake is unavailable.

## Raw results

The benchmark reports `median_ms`, `p95_ms`, `p99_ms`, CPU time, result-copy
bytes, logical and physical counts, dedup savings, and read amplification.
Representative synchronous results from this host:

| Workload | Group | Dedup | Median | p95 | Physical reads | Unique bytes | Physical bytes | Read amplification |
| --- | ---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| exact duplicates | 4096 | on | 9.872 ms | 9.878 ms | 5 | 20 KiB | 20 KiB | 1.000 |
| exact duplicates | 4096 | off | 23.374 ms | 23.522 ms | 20480 | 20 KiB | 80 MiB | 4096.000 |
| hot set | 32 | on | 0.105 ms | 0.113 ms | 60 | 460 KiB | 460 KiB | 1.000 |
| hot set | 128 | on | 0.515 ms | 0.524 ms | 45 | 1.094 MiB | 1.094 MiB | 1.000 |
| hot set | 256 | on | 0.617 ms | 0.623 ms | 15 | 1.211 MiB | 1.211 MiB | 1.000 |
| nested | 32 | on | 1.201 ms | 1.202 ms | 5 | 5 MiB | 5 MiB | 1.000 |
| partial overlap | 32 | on | 2.654 ms | 3.005 ms | 5 | 10.3125 MiB | 10.3125 MiB | 1.000 |
| non-overlap control | 128 | on | 0.623 ms | 0.696 ms | 640 | 2.5 MiB | 2.5 MiB | 1.000 |
| mixed | 128 | on | 0.736 ms | 0.766 ms | 285 | 1.697 MiB | 1.697 MiB | 1.000 |
| non-overlap with 8 KiB gaps | 128 | on | 0.497 ms | 1.272 ms | 5 | 2.5 MiB | 7.461 MiB | 2.984 |

The exact-duplicate case reduced physical reads from 20480 to 5 across five
groups and reduced median latency by about 58%. The non-overlap control issued
the same 640 physical reads with deduplication enabled, showing no request
count regression when there is nothing to reuse. Configured gaps deliberately
show byte amplification near 3x; this is why the maximum-size and gap policies
remain explicit.

## Planner overhead

The planner-only benchmark performs no disk I/O and runs 200 repetitions per
group size:

| Group size | Total | ns/request | Physical ranges |
| ---: | ---: | ---: | ---: |
| 8 | 0.875 ms | 547.036 | 1 |
| 32 | 3.103 ms | 484.892 | 1 |
| 128 | 9.731 ms | 380.101 | 1 |
| 512 | 29.114 ms | 284.315 | 1 |
| 2048 | 128.779 ms | 314.403 | 2 |
| 8192 | 621.246 ms | 379.178 | 3 |

The sweep is sort-dominated at small group sizes and remains roughly linear
per request over this range after sorting. No custom allocator or tree was
introduced.

## Memory-copy cost

The public API still returns one independent vector per logical request. In the
4096-duplicate run this means 80 MiB of logical result bytes were copied even
though only 20 KiB were physically read. That copy cost is visible as
`result_copy_bytes`; zero-copy backing storage remains an explicit future API
decision rather than an assumption.

## Interpretation

The Phase 4 optimization materially reduces redundant physical work for exact,
contained, and partial-overlap groups. The control workload does not show a
physical-read regression, while the gapped workload demonstrates that policy
limits still matter. Results are synchronous temporary-file measurements and
must not be generalized to io_uring performance.

## Decision

**KEEP WITH LIMITATIONS.** Keep the bounded sorted sweep and its metrics. Keep
the current copying result ownership until a zero-copy design is justified by
profiling and API review. The next optimization should be selected from
measured evidence; do not proceed automatically to the next phase.
