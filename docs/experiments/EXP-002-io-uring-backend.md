# EXP-002 — native io_uring backend

## Hypothesis

An io_uring executor should be able to submit multiple physical reads through
one runtime-owned ring while keeping the public API independent of raw
io_uring types. It must preserve the same bytes and per-request errors as the
synchronous backend. When io_uring is unavailable, the runtime should still
have a concurrent backend available rather than silently reducing `Auto` to
serial execution.

## Implementation

IntentIO uses the Linux `linux/io_uring.h` UAPI and syscalls directly, so the
library does not require liburing headers. The backend maps the submission and
completion rings, batches up to the configured queue depth, owns read buffers
until CQE processing finishes, and converts negative CQE results into
`std::error_code` values. `ExecutionPolicy::max_in_flight` caps each batch.
`Backend::Auto` falls back to the bounded threaded-`pread` backend when ring
creation is not permitted.

## Test setup and result

The host compiler and kernel headers were available, but the task sandbox
returned `EPERM` from `io_uring_setup`. Consequently:

```text
Backend::IoUring -> explicit construction reports Operation not permitted
Backend::Auto    -> selects threaded-pread backend and passes correctness tests
```

No io_uring throughput number is recorded because the backend could not be
created on this host. The implementation is present and the benchmark exposes
`--backend io_uring|auto` for a host that permits it. The host-local fallback
is covered by the threaded backend tests and Phase 6 benchmark.

## Decision

**KEEP, not performance-validated yet.** Correctness of the fallback path is
covered locally. A real io_uring-vs-pread comparison must be run on a Linux
host with the required kernel permission before making a performance claim.
