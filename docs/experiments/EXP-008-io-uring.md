# EXP-008 — native io_uring validation

## Question

Can a correctly initialized native Linux `io_uring` backend outperform the
threaded `pread` reference for IntentIO's physical plans without changing
planner semantics, global concurrency semantics, or per-request errors?

This experiment was stopped at capability probing because the current host
does not permit native ring creation. No fallback run is labeled as io_uring
data.

## Host capability probe

The probe was run from the repository on 2026-09-29:

| field | value |
| --- | --- |
| kernel | `7.0.0-31-generic` |
| CPU | 12th Gen Intel Core i5-12500H, 16 online CPUs |
| memory | 23,765,256 kB |
| filesystem | magic `0xef53` |
| storage device | `259:7`, sysfs path ending in `nvme0n1p7` |
| `io_uring_setup` | `EPERM` / Operation not permitted |
| native io_uring | unavailable |

The standalone capability report is [`phase8-capability-2026-09-29.txt`](../../benchmarks/results/phase8-capability-2026-09-29.txt). It prints the
kernel, filesystem, device identity, CPU count, memory, compiled UAPI setup
flags, setup feature bits when available, and ring entries when available.
Because setup failed before parameters were returned, supported feature bits,
supported setup flags, and ring sizes are reported as unavailable rather than
inferred from the kernel version.

## Existing architecture inspection

The current backend is in [`src/backends/io_uring_backend.cpp`](../../src/backends/io_uring_backend.cpp)
and uses the Linux UAPI directly; liburing is not required.

- `IoUringBackend::Impl` owns one ring fd, SQ/CQ mappings, SQEs, and ring
  offsets for the Runtime lifetime.
- `RuntimeState` owns one `IoUringBackend` and protects blocking backend
  execution with `blocking_backend_mutex`.
- Therefore many application threads may submit groups, but only one caller at
  a time issues work to this runtime-owned ring. The implementation does not
  have concurrent application-thread SQ ownership.
- SQEs are prepared in a batch bounded by the ring SQ capacity and
  `ExecutionPolicy::max_in_flight`; one `io_uring_enter` submits that batch.
- CQEs are consumed by the same issuing thread, waiting for one completion when
  the CQ is empty and advancing the mapped CQ head after each CQE.
- `user_data` is the physical-read vector index, so completion lookup is a
  direct bounds-checked index rather than an unordered map.
- Negative CQE results become per-read `std::error_code` values; successful CQE
  results become byte counts.
- The backend uses normal file descriptors and ordinary owned buffers. It does
  not use registered files, registered buffers, SQPOLL, IOPOLL, taskrun flags,
  fixed buffers, or fixed-file SQEs.
- Explicit `Backend::IoUring` reports initialization failure. `Backend::Auto`
  catches initialization `system_error` and selects threaded `pread`; benchmark
  output reports the actual selected backend.

This is a conservative single-issuer-by-architecture design, but it does not
enable `IORING_SETUP_SINGLE_ISSUER` because the host cannot create even a
plain ring and the flag's constraints have not been validated.

## Benchmark ladder status

All native rows below are **SKIP — io_uring unavailable (`EPERM`)**. No
threaded fallback row is included in a native result set.

| configuration | status |
| --- | --- |
| U0 plain io_uring | SKIP |
| U1 submission batching | SKIP |
| U2 completion batching | SKIP |
| U3 `SINGLE_ISSUER` | SKIP |
| U4 taskrun flags | SKIP |
| U5 registered files | SKIP |
| U6 registered buffers | SKIP |
| U7 registered files + buffers | SKIP |
| U8 SQPOLL | SKIP |

Consequently there are no Phase 8 baseline, submission-batch,
completion-batch, taskrun, fixed-file, fixed-buffer, combined-registration, or
SQPOLL CSVs. The capability text file is the raw Phase 8 artifact for this
host. A future permitted host must first capture this report, then run plain
io_uring before enabling one experimental mechanism at a time.

## Correctness parity gate

The native differential suite was not entered because initialization failed.
The existing suite still validates synchronous and threaded results for EOF,
short reads, invalid descriptors, zero-length reads, multiple files,
duplicates, overlap reuse, coalescing, concurrent groups, error isolation, and
runtime destruction. Explicit native construction fails clearly with
`io_uring_setup: Operation not permitted`; `Auto` selects threaded `pread` and
does not mislabel its backend.

Native byte-for-byte parity, small-ring wraparound, SQ backpressure, CQ
pressure, and native runtime-destruction behavior remain unvalidated until a
host permits setup.

## Adaptive controller interaction

No backend-specific adaptive controller was added. The existing global
controller remains backend-independent and produces a single
`max_in_flight` value. The threaded backend consumes that value today; the
io_uring backend's blocking execution path already receives the same resolved
policy, but no native run was allowed to verify its behavior under changing
workloads.

The following quantities remain intentionally separate for the eventual host
matrix:

```text
planning merge size
global max in-flight
SQ ring size
submission batch size
completion batch size
registered buffer count
```

## CPU, syscall, and performance measurements

There are no native performance measurements, syscall counts, or `perf`
profiles to interpret. Running the existing benchmark with explicit
`--backend io_uring` exits with the setup error before emitting a result. This
is required to avoid reporting threaded fallback as an io_uring result.

The Phase 7 threaded reference remains the only measured backend in this
container. Its queue-wait, execution, completion-dispatch, CPU, and context
switch measurements are documented separately in EXP-007 and are not used as
an io_uring comparison.

## Optional mechanisms not attempted

Registered files and registered buffers were not implemented because there is
no permitted baseline ring against which to measure their independent effects.
SQPOLL, SQ affinity, IOPOLL, COOP_TASKRUN, DEFER_TASKRUN, ring-size tuning,
fixed-file caching, fixed-buffer pooling, SQ/CQ pressure behavior, and
`user_data` alternatives were likewise not changed speculatively.

This leaves the existing normal buffered-I/O path intact and avoids claiming
support for features the host could not expose. The existing benchmark
`--materialize-file` mode remains available for a future permitted host; this
container's Phase 8 probe did not perform device-performance measurements.

## Validation

- `intentio-info` builds with strict C++20 warnings and reports the exact
  `EPERM` probe result.
- The prior strict library/test/benchmark builds remain the reference baseline.
- ASan/UBSan and TSAN passed for the existing Phase 7 implementation before
  this capability-only change.
- No native io_uring correctness or sanitizer claim is made.
- The host benchmark script now captures the capability report before any
  backend matrix and retains explicit native-backend gating.

## Decision

**KEEP WITH LIMITATIONS.** Keep the current small, plain UAPI implementation
and the threaded `pread` reference. Do not change `Backend::Auto` based on
performance: no native io_uring configuration was available to compare, and
fallback output must not be presented as native results. Revisit optimization
only on a host that permits `io_uring_setup`, beginning with plain io_uring and
the same physical plans, files, cache state, and global in-flight limits.

## Next phase

**E — backend auto-selection / online backend adaptation.** After native
io_uring is validated on a permitted host, use measured workload/backend
behavior to decide whether selection should remain static or adapt online.
This phase is not implemented here.
