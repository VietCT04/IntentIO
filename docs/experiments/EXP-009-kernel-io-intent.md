# EXP-009: Kernel-aware I/O intent

Date: 2026-09-29  
Host: Linux 7.0.0-31-generic, ext4 (`0xef53`), 16 CPUs, 23.8 GiB RAM  
Starting commit: `4cad447` (`Add io_uring capability probe and phase 8 report`)

## Question

Can IntentIO carry application intent beyond its own range planner and make
Linux treat the resulting physical reads differently, without changing the
borrowed-file-descriptor API or making correctness depend on advisory hints?

The answer from this phase is: yes, the plumbing is useful and measurable, but
the performance effect is workload- and cache-state-dependent. It should remain
an explicit, separately observable policy rather than a universal optimization.

## Why kernel-visible intent matters

IntentIO already distinguishes sequential/random access, reuse-likely versus
scan-once data, and latency class. The planner uses that information to decide
which logical requests can share physical reads. Linux has a second decision to
make: how much readahead to use and whether newly read scan data should remain
in the page cache. Phase 9 carries the same intent through that second boundary:

```text
application intent
       |                    |
       v                    v
PlanningPolicy       KernelIOPolicy
       |                    |
       +------ physical read +------+
                         |           |
                    buffered     DONTCACHE
                    + fadvise
```

This is not direct I/O. `RWF_DONTCACHE` is still a buffered read operation with
different cache treatment for compatible filesystems.

## Linux mechanisms investigated

The mechanisms were kept separate:

- `posix_fadvise`: advisory access-pattern information on a relevant fd/range.
- `RWF_DONTCACHE`: an optional `preadv2` read flag for scan-once physical reads.
- `O_DIRECT`: investigated only as an ownership/alignment boundary. No
  arbitrary borrowed fd is changed and no O_DIRECT executor was implemented.

Advice is non-fatal. A failed `posix_fadvise` increments a metric and the
ordinary read still runs. An unsupported DONTCACHE operation falls back to
buffered `pread`, with separate unsupported and fallback counters.

## File ownership and planning boundary

The existing API continues to borrow caller-owned integer fds. IntentIO does
not close them, reopen them through `/proc/self/fd`, add `O_DIRECT`, or mutate
their flags. A single fd can occur in multiple groups, and one group can contain
multiple fds. The planner already sorts/group physical work by fd and offset;
the kernel policy is resolved once per submission and applied when each physical
read is executed.

This means two groups sharing an fd can issue different advice in succession.
Linux advice is inherently associated with the open file description and may
therefore interact under concurrency. Phase 9 deliberately avoids a global fd
metadata cache or a hidden “last intent wins” state machine. The policy is
local, visible in tracing, and correctness does not depend on which advisory
call happened most recently.

## Policy architecture

The public opt-in controls are:

```cpp
enum class FileAdvice { None, Sequential, Random, NoReuse };
enum class CachePolicy { Normal, DontCache };

struct KernelIOOptions {
    bool enabled{true};
    bool enable_fadvise{true};
    bool enable_dontcache{true};
};
```

The internal `KernelIOPolicy` contains exactly two decisions:

```cpp
struct KernelIOPolicy {
    FileAdvice access_advice;
    CachePolicy cache_policy;
};
```

The resolver is deterministic and conservative:

| Application intent | Kernel policy |
| --- | --- |
| Unknown | no advice, normal buffered read |
| Sequential | `POSIX_FADV_SEQUENTIAL` |
| Random | `POSIX_FADV_RANDOM` |
| ScanOnce, DONTCACHE enabled | `RWF_DONTCACHE` |
| ScanOnce, DONTCACHE disabled | `POSIX_FADV_NOREUSE` when no access advice is already selected |
| disabled kernel policy | no advice, normal buffered read |

The Phase 9 benchmark disables planner coalescing and deduplication so that the
kernel-policy matrix isolates physical read behavior. The normal library path
still resolves the kernel policy after planning intent is resolved and before
physical execution; planner limits and logical slicing remain unchanged.

Tracing now shows both the planner policy and:

```text
Kernel I/O policy: advice=SEQUENTIAL cache=DONTCACHE
```

## Capability and metadata

`intentio-info` performs a real probe on a temporary file in the requested
directory rather than inferring support from the kernel version. On this host:

```text
rwf_dontcache=usable=yes bytes=4096
statx_dioalign=supported memory_alignment=4 offset_alignment=512
io_uring_available=no
io_uring_setup_errno=1 (Operation not permitted)
```

The repository filesystem was ext4 on `nvme0n1p7`. The benchmark records kernel,
filesystem, file state (materialized or sparse), warm-buffered status, CPU,
fault counters, policy counters, and request geometry in each CSV.

## `posix_fadvise` results

The fadvise matrix used 64 materialized sequential reads, queue depth 16,
16 workers, and physical request sizes 4 KiB, 64 KiB, 256 KiB, and 1 MiB.
Advice calls succeeded for every physical read; no advice failures were
recorded.

| request size | baseline p50/p95 ms | SEQUENTIAL p50/p95 ms | NOREUSE p50/p95 ms |
| ---: | ---: | ---: | ---: |
| 4 KiB | 1.471 / 2.824 | 0.917 / 1.765 | 0.927 / 2.194 |
| 64 KiB | 3.021 / 4.954 | 2.718 / 4.622 | 2.673 / 4.001 |
| 256 KiB | 5.770 / 9.975 | 5.747 / 9.946 | 5.778 / 9.629 |
| 1 MiB | 24.562 / 38.534 | 22.976 / 36.408 | 21.840 / 37.218 |

These are single-run host measurements, not a claim of a general speedup.
They show that advice is accepted and observable, while the size of the effect
is modest and variable. The implementation therefore retains fadvise as an
optional advisory layer rather than treating it as a correctness or default
performance requirement.

### RANDOM

For 128 materialized random requests at queue depth 16, `POSIX_FADV_RANDOM`
was not uniformly beneficial:

| workload | baseline p50/p95 ms | RANDOM p50/p95 ms | NOREUSE p50/p95 ms | DONTCACHE p50/p95 ms |
| --- | ---: | ---: | ---: | ---: |
| random 4 KiB | 1.806 / 3.577 | 1.903 / 4.023 | 2.688 / 5.289 | 2.477 / 4.654 |
| random 64 KiB | 4.499 / 9.335 | 6.663 / 11.688 | 5.988 / 10.065 | 5.042 / 7.696 |
| mixed | 17.761 / 22.442 | 16.312 / 21.117 | 15.357 / 19.944 | 16.446 / 21.190 |

The mixed workload happened to favor the advisory variants in this run, while
RANDOM lost on the two pure random cases. This is evidence against blindly
applying one access hint to every workload.

### NOREUSE

The NOREUSE rows above are the explicit fallback experiment with DONTCACHE
disabled. They issued 64 or 128 successful `POSIX_FADV_NOREUSE` calls as
appropriate and performed the same logical and physical reads as baseline.
NOREUSE is kept as a conservative alternative for callers that disable the
DONTCACHE path; it is not represented as a cache eviction guarantee.

## `RWF_DONTCACHE` capability and implementation

The capability probe succeeded on the repository filesystem. The threaded and
synchronous buffered execution path now uses one-iovec `preadv2` with
`RWF_DONTCACHE`, retries `EINTR`, preserves short-read/EOF semantics, and
falls back only when the filesystem reports `EOPNOTSUPP`/`ENOTSUP` before any
bytes are returned. Metrics distinguish:

```text
attempted, succeeded, unsupported, fallbacks,
buffered_reads, bytes_dontcache, bytes_buffered
```

The recorded DONTCACHE matrix had zero unsupported operations and zero
fallbacks: every attempted physical read succeeded with the flag. The 1 MiB
sequential run completed 128 attempts/128 successes; p50/p95 were 43.756/78.834
ms versus 43.307/73.443 ms for that run's baseline. For random 64 KiB,
DONTCACHE measured 5.042/7.696 ms versus baseline 4.499/9.335 ms. The result
is workload-sensitive, as expected for a cache policy.

The operation is intentionally not described as `O_DIRECT`: it remains a
buffered fd read, and the logical bytes returned are the same.

## Cache-pollution benchmark and hot-set preservation

The benchmark warms a 4 MiB HOT file with baseline buffered 4 KiB reads, then
scans a separate 64 MiB SCAN file with 1 MiB reads at queue depth 16. It runs
baseline, ScanOnce+NOREUSE, and ScanOnce+DONTCACHE for both materialized and
sparse files. `mmap` + `mincore` is used only by the benchmark diagnostic; it is
not part of normal IntentIO execution.

On this host, HOT residency was 1.000 before and 1.000 after every policy in
both file-state variants. HOT p50 changed within normal single-run noise:

| file state | policy | HOT p50 before/after ms | HOT residency before/after |
| --- | --- | ---: | ---: |
| materialized | baseline | 17.454 / 22.647 | 1.000 / 1.000 |
| materialized | NOREUSE | 14.458 / 15.518 | 1.000 / 1.000 |
| materialized | DONTCACHE | 17.312 / 16.066 | 1.000 / 1.000 |
| sparse | baseline | 14.401 / 16.896 | 1.000 / 1.000 |
| sparse | NOREUSE | 13.475 / 19.822 | 1.000 / 1.000 |
| sparse | DONTCACHE | 13.640 / 18.612 | 1.000 / 1.000 |

There was no measurable HOT eviction in this bounded experiment. That is a
useful negative result: a 64 MiB stream on a host with roughly 23 GiB of RAM is
not enough pressure to demonstrate page-cache protection. It does not disprove
DONTCACHE behavior under a larger working set.

`mincore` reports page-cache residency for the mapped file pages; it does not
measure device-cache state, readahead queues, writeback, or all kernel eviction
decisions. The readings are therefore diagnostic evidence, not a universal
cache model. Existing cached-page behavior is intentionally not made a
correctness assumption: a DONTCACHE read may still observe already-cached
content, and the result remains valid whether pages stay resident or are
pruned.

## O_DIRECT capability and alignment

No O_DIRECT execution path was safely implemented in Phase 9. The existing API
borrows arbitrary fds, so adding `fcntl(F_SETFL, O_DIRECT)`, reopening through
`/proc/self/fd`, or silently duplicating and changing the open-file behavior
would be an unacceptable API mutation. There is no managed `intentio::File`
abstraction yet, no alignment allocator, and no direct-I/O benchmark result.

The capability boundary was still measured with `STATX_DIOALIGN`: this file
reported 4-byte memory alignment and 512-byte offset alignment. Those values
are recorded rather than hard-coded as a universal direct-I/O contract.

## Planner interaction

The Phase 9 isolation matrix used one logical request per physical read with
coalescing and deduplication disabled. This makes the fadvise/DONTCACHE counts
and bytes directly interpretable. The full runtime still preserves the earlier
planner pipeline: logical requests are resolved into bounded physical ranges,
then the resolved `KernelIOPolicy` is applied to each physical range and the
results are sliced back to logical requests. Kernel policy changes how Linux
treats the physical read; it does not change merge limits, amplification
limits, ordering, or result bytes.

## Concurrency interaction

The representative concurrency matrix used 64 workers and queue depths 1, 4,
16, and 64 for random 4 KiB and sequential 1 MiB scans. Results were not
monotonic: for random 4 KiB, baseline p50 ranged from 5.063 to 6.743 ms and
DONTCACHE from 4.245 to 5.730 ms; for sequential 1 MiB, baseline p50 ranged
from 33.578 to 37.275 ms and DONTCACHE from 32.468 to 33.688 ms. Queue depth
therefore belongs in any future cache-policy evaluation.

## Incorrect-intent behavior

ScanOnce is an advisory performance choice, not a data-lifetime or correctness
contract. The differential test suite exercises valid bytes, EOF, partial
reads, invalid descriptors, zero-length reads, overlapping/duplicate planner
cases, and concurrent threaded execution while kernel policies are enabled.
The Phase 9 benchmark also rereads the HOT set after the scan. A caller that
marks immediately reused data as ScanOnce can pay extra cache misses or lose
the benefit of warm pages, but IntentIO still returns the same bytes and
errors. No result ordering or logical slicing path depends on cache residency.

## CPU cost and faults

The benchmark records process user+system CPU time and `getrusage` minor and
major faults. The materialized matrices recorded zero major faults; minor
faults increased with the amount of data touched, especially for 1 MiB scans.
The results do not show a universal CPU win: DONTCACHE changes the syscall path
and can add overhead for small reads even when wall time happens to improve at
one queue depth.

## Correctness and sanitizers

Strict C++20 builds pass the existing test suite plus the kernel-intent tests.
The new tests verify successful SEQUENTIAL advice accounting, NOREUSE mapping
when DONTCACHE is disabled, and DONTCACHE success/fallback accounting while
checking returned bytes. The direct `preadv2` path handles `EINTR`, short
reads, EOF, invalid fd errors, and unsupported-filesystem fallback without
changing logical result semantics.

The final validation matrix includes strict compilation, `git diff --check`,
ASan+UBSan, and ThreadSanitizer. CMake is not available in this development
environment, so equivalent direct compiler invocations are used for the
validation binaries; the CMake targets and host benchmark script are updated
for a normal Linux build host.

## Decision gate

1. **Exact `KernelIOPolicy` design:** separate internal `{FileAdvice, CachePolicy}` with public `KernelIOOptions` feature switches; one policy is resolved per submission and applied per physical read.
2. **Intent-to-kernel mapping:** Sequential → `FADV_SEQUENTIAL`; Random → `FADV_RANDOM`; ScanOnce → DONTCACHE when enabled, otherwise NOREUSE when no access advice is selected; Unknown → no advice/normal.
3. **fadvise capability/results:** `posix_fadvise` calls succeeded in the recorded matrix; failure is metricized and non-fatal.
4. **SEQUENTIAL benchmark:** p50/p95 improved in this run at 4 KiB and 1 MiB, with smaller differences at 64/256 KiB; it remains advisory, not guaranteed.
5. **RANDOM benchmark:** RANDOM was slower on pure 4 KiB and 64 KiB random rows in this run and slightly better on mixed; no universal win.
6. **NOREUSE benchmark:** valid and observable, with modest variable latency differences; a conservative fallback when DONTCACHE is disabled.
7. **RWF_DONTCACHE availability:** usable on the repository ext4 filesystem for the 4 KiB capability probe.
8. **DONTCACHE benchmark:** 128/128 successes in the recorded scan matrices, zero unsupported/fallback rows; performance varied by workload and QD.
9. **Cache-pollution benchmark:** HOT 4 MiB versus SCAN 64 MiB, materialized and sparse, QD 16, with mincore diagnostics.
10. **Hot-set preservation:** HOT residency stayed 100% before and after every recorded scan; no eviction benefit was measurable under this RAM/cache pressure.
11. **`mincore` observations:** it is useful for mapped page-cache residency diagnostics, but not a device-cache or universal eviction oracle.
12. **STATX_DIOALIGN:** supported; memory alignment 4 bytes, offset alignment 512 bytes on the probe file.
13. **Whether O_DIRECT was safely implemented:** no; no borrowed fd was mutated and no managed File API was added.
14. **O_DIRECT results:** none; only the alignment capability boundary was recorded.
15. **Planner × cache-policy interaction:** the isolated matrix disables coalescing/deduplication; the full pipeline applies kernel policy after bounded physical planning and preserves logical bytes.
16. **Concurrency interaction:** QD 1/4/16/64 produced non-monotonic, workload-dependent results; policy conclusions cannot be made at one QD.
17. **Incorrect-hint behavior:** performance may degrade when ScanOnce data is immediately reused, but bytes, EOF, errors, ordering, and slicing remain correct.
18. **Sanitizer/correctness status:** strict tests, ASan+UBSan, and TSAN validation pass using the available direct-build workflow.
19. **Decision:** **KEEP WITH LIMITATIONS** — keep the separate policy and metrics, retain fadvise and DONTCACHE as explicit intent-driven mechanisms, and make no universal speed or eviction claim.

## Recommended next phase

**E. Native io_uring validation when a permitted host becomes available.**

Phase 9 is complete at the kernel-visible intent boundary. The current host
returns `EPERM` from `io_uring_setup`, so native io_uring performance and the
interaction between kernel intent and native submissions must be measured on a
host that permits the setup call before any backend policy is changed.
