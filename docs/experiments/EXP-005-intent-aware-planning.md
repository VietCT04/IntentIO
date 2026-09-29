# EXP-005 — intent-aware planning and cost-aware merging

## Hypothesis

Access pattern, reuse, and latency hints should change physical planning in a
deterministic way. Sequential and throughput groups should tolerate bounded
gaps when the extra bytes fit a read-amplification budget. Random and
latency-sensitive groups should remain conservative. Intent must not disable
exact overlap deduplication, and misuse of an aggressive hint must not create
unbounded reads.

## Public API and policy

Groups now expose three independent hint dimensions:

```cpp
group.access_pattern(intentio::AccessPattern::Sequential);
group.reuse_hint(intentio::ReuseHint::ScanOnce);
group.latency_class(intentio::LatencyClass::Throughput);
```

The resolver is separate from the range planner. It maps the hints to a
`PlanningPolicy` containing:

- maximum merge gap;
- maximum merged request size;
- maximum read amplification;
- independent coalescing and deduplication switches.

Latency-sensitive policy takes precedence over access pattern. Throughput can
be combined with sequential or random access, and reuse hints adjust the
result conservatively or toward a scan/throughput policy. Existing runtime
coalescing and request-size settings remain hard global caps. The resolver is
pure and deterministic for the same options and hints.

The built-in policy presets are:

| Preset | Max gap | Max merged size | Max amplification |
| --- | ---: | ---: | ---: |
| Unknown | 4 KiB | 8 MiB | 1.50x |
| Sequential | 64 KiB | 8 MiB | 1.75x |
| Random | 0 | 1 MiB | 1.10x |
| LatencySensitive | 0 | 256 KiB | 1.10x |
| Throughput | 32 KiB | 16 MiB | 2.00x |

`RuntimeOptions::coalescing.max_gap_bytes` defaults to a 64 KiB global cap,
and `max_request_bytes` defaults to 8 MiB; both can only restrict these
presets. Coalescing and deduplication are enabled by default.

## Planner behavior

The sorted same-file sweep evaluates adjacent candidates separately from the
resolved policy:

- overlapping candidates use deduplication, regardless of access pattern;
- non-overlapping candidates use coalescing and the policy's gap limit;
- every accepted candidate must fit the maximum union size and amplification
  budget, computed as merged physical bytes divided by newly covered useful
  bytes;
- rejection counters distinguish amplification, size, gap, and disabled-policy
  decisions.

`merge_candidates` and `merges_accepted` make the decision surface visible.
`INTENTIO_TRACE_PLAN=1` or `RuntimeOptions::trace_plan` prints the hints,
resolved policy, and each candidate decision to the runtime trace stream.

## Correctness validation

The strict build uses GCC 15.2 with C++20, `-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Werror`. The complete unit suite passes, including the
Phase 5 intent-policy cases and the differential check:

```text
seed=482193
groups=3000
requests/group=1..24
```

The differential test compares optimized results with an independent `pread`
baseline across request order, multiple descriptors, invalid descriptors,
overlaps, gaps, EOF short reads, and zero-length reads. ASan/UBSan also passes
with leak detection disabled because LeakSanitizer cannot run under this
container's ptrace setup. Each generated group is replayed under all five
intent cases: Unknown, Sequential, Random, LatencySensitive, and Throughput.
The preserved regression seed runs 3,000 groups; fixed seeds `917331` and
`381917` add 500 groups each.

Plan tracing was manually verified for a sequential two-range group; it emits
the resolved 64 KiB gap policy and an accepted candidate with amplification
`1.125`.

## Benchmark environment

- Linux `7.0.0-31-generic`
- GCC `15.2.0`
- synchronous `pread` backend
- temporary sparse files
- 32 requests per group, 5 repetitions
- planner-only benchmark: 200 repetitions, groups 8 through 8192
- `io_uring_setup` returns `EPERM` in this container
- CMake is unavailable, so the host matrix script exits with its actionable
  setup message; direct strict builds were used for validation

Run the host matrix on a machine with CMake and io_uring permission with:

```sh
./scripts/run_host_benchmarks.sh
```

The raw Phase 5 matrix is in
`benchmarks/results/phase5-sync-2026-09-29.csv`; the planner-only capture is
in `benchmarks/results/phase5-planner-2026-09-29.csv`.

## Raw result highlights

| Workload | Intent | Physical reads | Accepted | Rejected: amp | Rejected: size | Rejected: gap | Read amplification |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sequential | unknown | 160 | 0 | 0 | 0 | 155 | 1.000 |
| sequential | sequential | 5 | 155 | 0 | 0 | 0 | 1.242 |
| sequential | random | 160 | 0 | 0 | 0 | 155 | 1.000 |
| latency | sequential | 10 | 150 | 0 | 5 | 0 | 1.060 |
| latency | latency-sensitive | 160 | 0 | 0 | 0 | 155 | 1.000 |
| misuse | sequential | 160 | 0 | 155 | 0 | 0 | 1.000 |
| misuse | throughput | 160 | 0 | 155 | 0 | 0 | 1.000 |

The misuse workload declares sequential/throughput intent over 4 KiB reads
separated by 32 KiB gaps. All 155 merge candidates are rejected by the
amplification budget, so the hint cannot turn sparse random work into a large
physical read. Exact overlap deduplication remains independent of these gap
decisions and is covered by the unit tests.

The planner-only capture ranges from 278.764 to 652.672 ns/request across all
intents and sizes. The 8,192-item groups are 370.050–385.008 ns/request, and
groups from 128 onward stay below 400 ns/request. The small Unknown groups are
the noisy upper end; this is comparable to the Phase 4 baseline of roughly
284–547 ns/request and does not approach multi-microsecond planning. Random
and latency-sensitive policies produce more physical ranges in the synthetic
overlap/gap workload, while preserving the same unique logical byte count.

The latency workload contains one 4 KiB request followed by 256 KiB neighbors.
LatencySensitive keeps the small request separate; Throughput and Sequential
accept some bounded unions. `Group::submit()` currently completes the group
synchronously, so these numbers describe whole-group planning and execution,
not urgent-request completion latency. A per-request latency claim waits for a
future asynchronous completion API.

## Interpretation

The experiment confirms the intended policy boundary: intent changes planning
where the workload supports it, but cost limits and explicit rejection metrics
prevent speculative over-reading. The result is a useful policy layer, not a
claim about end-to-end asynchronous performance. That claim requires a host
where io_uring can be created and must include queue-depth and device-level
measurements.

## Decision

**KEEP WITH LIMITATIONS.** Keep the public intent API, deterministic resolver,
amplification budget, independent deduplication path, rejection metrics, and
trace output. Do not add caching, prefetching, O_DIRECT, or adaptive behavior
inside this phase.

**Exactly one recommended next phase: C — adaptive concurrency/queue depth.**
The policy layer now provides a stable physical-work signal, and the next
high-value question is whether an allowed io_uring host benefits from adapting
in-flight depth by workload and intent. First reproduce this matrix on such a
host; do not infer queue-depth benefits from these synchronous results.
