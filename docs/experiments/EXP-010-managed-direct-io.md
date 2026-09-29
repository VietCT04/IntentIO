# EXP-010: Managed files and safe direct I/O

Date: 2026-09-29  
Status: KEEP WITH LIMITATIONS  
Host: Linux 7.0.0-31-generic, ext4 (`0xef53`), 16 CPUs, about 23.8 GiB RAM,
NVMe partition `nvme0n1p7`

## Question

Can IntentIO safely choose between buffered reads, `RWF_DONTCACHE`, and
`O_DIRECT` when it owns the file, while preserving the borrowed-descriptor API?
The answer for this phase is yes for aligned, read-mostly managed requests.
The measured performance answer is workload- and host-dependent: direct I/O
was correct but slower in the recorded matrix, so it remains an explicit
experimental mechanism rather than a default recommendation.

## Why the ownership boundary matters

The existing `group.read(fd, offset, size)` API borrows an integer descriptor.
It must not mutate descriptor flags, reopen `/proc/self/fd`, or assume that a
new direct descriptor refers to the same pathname. Phase 10 therefore adds an
explicit ownership model instead of changing borrowed-fd behavior.

## Managed `File` architecture

`intentio::File` is move-only and read-oriented:

```cpp
auto file = intentio::File::open("dataset.bin");
auto group = io.create_group();
group.read(file, offset, size);
group.io_cache_mode(intentio::IOCacheMode::Direct);
```

The public object owns a shared internal `FileState`. That state owns the
normal `O_RDONLY|O_CLOEXEC` descriptor and, when available, an independently
opened `O_DIRECT` descriptor. A managed request stores a
`std::shared_ptr<FileState>`, so a submitted group retains the descriptors
until all physical work and result slicing finish. Moving or destroying the
public `File` cannot create a dangling physical request. The destructor closes
the direct descriptor and then the buffered descriptor exactly once.

The direct and buffered descriptors are checked with `fstat` device/inode
identity before direct use. The abstraction is intended for read-mostly or
immutable files; it does not solve concurrent pathname replacement, truncation,
or writes between the independent opens.

## Capability discovery and alignment

`File::capabilities()` exposes:

```text
direct_io_supported
memory_alignment
offset_alignment
read_offset_alignment
dontcache_supported
```

The implementation asks `statx` for `STATX_DIOALIGN` and requests
`STATX_DIO_READ_ALIGN` when the platform headers provide it. It does not
hard-code 512 or 4096. On this host the tested file reported memory alignment
4 and offset alignment 512; the read-offset field was not returned, so the
implementation conservatively used the observed offset alignment for reads.
The independent direct descriptor and all three nonzero alignments are
required before `direct_io_supported` becomes true. `intentio-info` reported:

```text
managed_file_open=yes
rwf_dontcache=usable=yes bytes=4096
statx_dioalign=supported memory_alignment=4 offset_alignment=512
direct_io=supported=yes
io_uring_available=no (io_uring_setup errno=1, EPERM)
```

The physical direct target is a move-only `AlignedBuffer` allocated with
`posix_memalign`. It normalizes the requested alignment to a valid allocator
alignment, owns and frees its allocation through RAII, and is checked by unit
tests for alignment and move transfer. Direct eligibility is checked against
the managed file's actual capability values at the physical-request level.

## Policy and fallback semantics

`IOCacheMode` is explicit and separate from planning and execution policy:

```text
PlanningPolicy  -> which logical requests become physical ranges
ExecutionPolicy -> how many physical ranges run concurrently
KernelIOPolicy  -> buffered, DONTCACHE, or direct mechanism
```

The default `Auto` resolver retains the Phase 9 behavior: `ScanOnce` selects
`DONTCACHE` when enabled, and otherwise uses its existing advice fallback. It
does not silently turn `ScanOnce` into Direct. `Buffered`, `DontCache`, and
`Direct` are available as explicit group overrides.

Explicit Direct on a borrowed descriptor, an unsupported managed file, or an
unaligned physical range returns a typed `operation_not_supported` or
`invalid_argument` error. Auto may reject a direct candidate, increments the
direct rejection/alignment counters, and returns to the policy it would have
used without the candidate. No physical range expansion is performed.

The benchmark-only threshold experiment sets `KernelIOOptions`
`auto_direct_threshold_bytes`. For `Auto` managed requests at or above the
threshold, Direct is attempted; below it, the original Auto choice is used.
The threshold is not enabled by default and is not a production recommendation.

## Correctness and resource checks

The test suite compares managed buffered and direct bytes, tests multiple
aligned offsets, EOF with a 512 KiB + 123 byte file, explicit alignment
failure, borrowed-fd rejection, managed lifetime after public `File` and
`Runtime` scope exit, four concurrent managed direct submissions, planner and
duplicate paths, and aligned-buffer moves. The direct EOF case uses an aligned
offset beyond the final full block and returns the same 123-byte partial result
as buffered I/O.

The managed-file test also opens and destroys 1,000 managed files and checks
`/proc/self/fd` returns to its baseline. Sanitizer and race validation is
listed in the final gate below. Direct mode is intentionally not available on
borrowed descriptors, and the existing borrowed read path remains unchanged.

## Benchmark method

The Phase 10 benchmark uses the same materialized temporary-file model,
logical ranges, planner settings, worker counts, and queue-depth settings for
Buffered, DONTCACHE, and Direct. It records wall time, p50/p95/p99, CPU time,
minor/major faults, voluntary/involuntary context switches, logical and
physical reads, mechanism counters, and adaptive-controller counters. Direct
unsupported hosts produce `SKIP` rows rather than silently measuring buffered
I/O. Raw CSVs are in `benchmarks/results/`.

Representative rows below use QD 16 on the host above; they are observations,
not portable performance claims.

| Workload | Mechanism | Request | p50 ms | p95 ms | Physical reads | Notes |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| sequential | Buffered | 4 KiB | 0.635 | 0.986 | 64 | lowest small-read latency in this run |
| sequential | DONTCACHE | 4 KiB | 0.940 | 1.508 | 64 | functional, not faster here |
| sequential | Direct | 4 KiB | 3.836 | 3.920 | 64 | 64/64 direct successes |
| sequential | Buffered | 1 MiB | 23.193 | 31.036 | 64 | page-cache path |
| sequential | DONTCACHE | 1 MiB | 24.285 | 32.377 | 64 | no retained cache bytes |
| sequential | Direct | 1 MiB | 51.833 | 87.894 | 64 | 64 MiB direct bytes |
| random 4 KiB | Buffered | 4 KiB | 1.102 | 1.819 | 128 | baseline |
| random 4 KiB | DONTCACHE | 4 KiB | 1.227 | 1.903 | 128 | close to buffered |
| random 4 KiB | Direct | 4 KiB | 66.388 | 130.939 | 128 | substantially slower here |

The complete sequential matrix covers 4 KiB, 64 KiB, 256 KiB, 1 MiB, and 4 MiB
at QD 1/4/16/64. Random aligned 4 KiB and 64 KiB matrices cover the same QDs.
The recorded direct path is correct and supported, but did not win any of the
measured representative rows.

## Planner, duplicates, concurrency, and adaptive behavior

The planner-shaped workload issued 128 logical 64 KiB reads and produced two
physical reads totaling 8,904,704 bytes. At QD 16, representative p50 values
were 12.936 ms Buffered, 19.626 ms DONTCACHE, and 32.556 ms Direct. Direct
still operated on planner output; it did not require a separate scheduler.

The duplicate-heavy workload reduced 4,096 logical reads to one physical
4 KiB read. Representative p50 values were 29.057 ms Buffered, 22.006 ms
DONTCACHE, and 27.468 ms Direct. After deduplication, mechanism choice affects
only the remaining physical read.

Producer/concurrency runs used four producers and QD 1/4/16/64 for random and
sequential workloads. All supported direct rows completed with direct success
counts equal to attempts. The adaptive controller was not retuned: its
existing controller ran for Buffered, DONTCACHE, and Direct. In the recorded
64-request, 1 MiB sequential run, p50 was 21.837 ms Buffered, 22.486 ms
DONTCACHE, and 67.090 ms Direct; each run reported four controller windows and
the same bounded controller shape. No Direct-specific controller was added.

## CPU, cache pressure, and reread trade-offs

The benchmark records user+system CPU from `getrusage`, context switches, and
fault counts. Direct did not show a universal CPU advantage: some direct rows
had fewer voluntary context switches, but their wall time was higher. No
`perf` counters are required for the default harness, so cycles and
instructions are not claimed.

For the safe default cache-pressure run (4 MiB HOT, 64 MiB SCAN, on a host with
about 23.8 GiB RAM), HOT residency was 1.000 before and after for Buffered,
DONTCACHE, and Direct; SCAN residency was also 1.000. This confirms the
instrumentation but does not create meaningful system-wide pressure. The
`--cache-pressure-size` option permits a controlled larger experiment on a
host where that is appropriate.

For the immediate 64 × 1 MiB reread, representative p50 values were:

```text
                 first     second
Buffered          24.969    23.688 ms
DONTCACHE         27.241    27.415 ms
Direct            60.596    45.568 ms
```

The direct path did not obtain a page-cache reread benefit. This is a central
trade-off, not evidence that Direct is universally wrong.

## Automatic threshold experiment

The benchmark compares thresholds 64 KiB, 256 KiB, and 1 MiB over the five
sequential request sizes. Threshold rows encode the threshold in the CSV
`workload` label (`sequential-threshold-<bytes>`), while retaining the common
52-column schema. In the captured run, 4 KiB rows stayed on DONTCACHE for all
thresholds; requests at or above each threshold attempted and completed direct
reads. The 64 KiB threshold therefore selected Direct for more rows, but the
direct rows remained slower than the Buffered/DONTCACHE baselines. No robust
threshold win was established, so Auto remains conservative and Direct remains
opt-in.

## Final decision gate

1. Public `File` API: added move-only `intentio::File`, `FileCapabilities`, and `group.read(file, ...)`.
2. Internal ownership model: shared `FileState` keeps buffered/direct descriptors alive through submissions.
3. Direct capability values: current host observed memory 4, offset 512, and Direct supported; read-offset field was not returned and conservatively follows offset alignment.
4. Alignment implementation: `statx` capability discovery plus `posix_memalign` RAII buffers; no hard-coded alignment.
5. Buffered-fd compatibility: borrowed integer-fd reads remain supported and are not mutated, reopened, or upgraded to Direct.
6. Direct correctness results: aligned 4 KiB through 1 MiB reads, offsets, planner output, duplicate reduction, and concurrent groups matched buffered logical bytes.
7. EOF behavior: aligned Direct reads extending beyond EOF returned the same short successful result as buffered I/O.
8. Alignment fallback behavior: Auto falls back and records the rejection; explicit Direct returns an alignment error.
9. Sequential Buffered vs DONTCACHE vs Direct: all three were measured at 4 KiB/64 KiB/256 KiB/1 MiB/4 MiB and QD 1/4/16/64; Direct was slower on this host.
10. Random comparison: aligned 4 KiB and 64 KiB matrices completed; Direct was substantially slower in the captured random rows.
11. Planner-shaped comparison: 128 logical reads reduced to two physical reads for all mechanisms; Direct remained correct but slower.
12. Duplicate-heavy comparison: 4,096 logical reads reduced to one physical read; mechanism differences were correspondingly small and workload-dependent.
13. Concurrency comparison: four producers and QD 1/4/16/64 completed representative managed Direct runs without lifetime or correctness failures.
14. Adaptive-controller behavior: existing controller was reused unchanged for all three mechanisms; no Direct-specific retuning was introduced.
15. CPU/context-switch differences: CPU, faults, and voluntary/involuntary context switches are recorded; no universal CPU win was established.
16. Reread penalty: Direct showed no page-cache reread benefit and was slower in the representative second scan.
17. Cache-pressure / HOT-set result: safe default 4 MiB HOT/64 MiB SCAN did not evict HOT on this large-RAM host; configurable scan size is available.
18. Automatic size-threshold result: 64 KiB/256 KiB/1 MiB points selected Direct only for eligible large requests, with no robust win; default Auto remains unchanged.
19. Sanitizer / TSAN status: strict warnings and normal tests pass; ASan/UBSan pass with leak detection disabled because this host makes LeakSanitizer abort under ptrace, and TSAN passes.
20. fd/resource-leak status: 1,000 managed open/close iterations returned `/proc/self/fd` to baseline; move/lifetime tests passed and no double-close was observed.
21. Decision: **KEEP WITH LIMITATIONS**. Managed ownership is a sound foundation, but Direct stays experimental, aligned-only, and opt-in.

## Next phase recommendation

**C. aligned-range expansion for Direct I/O.** This is the explicitly deferred
step needed to make more unaligned logical requests eligible, and it deserves
its own measured treatment of amplification, EOF handling, and result slicing.
It is not implemented in Phase 10.
