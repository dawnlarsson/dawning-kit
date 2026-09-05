# Shared-storage and RAM pass — 2026-09-05

These are component measurements on the native Ryzen 9950X Arch Linux `box`,
not a measurement of an entire booted Moonwater image. Virtual reservations,
resident pages, peak RSS, and retained reusable capacity are different costs.

## Shared mapping growth

`memory_reserve` in `src/library.c` now attempts Linux `mremap(MREMAP_MAYMOVE)`
before the existing allocate/copy/release fallback. The existing byte and array
store fronts automatically share this path; there is no second allocator or
new per-subsystem resize implementation. x86-64, AArch64 and RV64 use the same
ownership and failure contract. The capacity-hit path remains unchanged.

The owner must be mmap-backed, not a malloc block. Only live elements have to
survive growth; unused capacity has no zero-fill contract. The new cold-path
live-count check exposed a snapshot initializer marking a header prefix live
before reserving its backing storage; that caller now reserves first.

Fresh-process 128 MiB dense growth, nine alternating before/after pairs pinned
to CPU 0, measured only around reserve (allocation/touching precede timing):

| Median | Before | After |
| --- | ---: | ---: |
| Peak RSS, KiB | 262160 | 131088 |
| Counter ticks for growth | 70842500 | 115756 |

The benchmark is `kit/bench reserve 128`; `kit/bench reserve 128 sparse` checks
the sparse shape. Compare binaries built with the respective library revisions.
Run each sample in a new process. Low sparse-RSS results can be contaminated by
the launcher's inherited high-water mark and are not presented as live RAM.
Native `mincore` regression checks instead prove that untouched pages stay
nonresident through growth.

`src/test/reserve.c` covers capacity hits, arithmetic rejection, owner stability,
forced movement, multi-VMA fallback, allocation refusal and sparse residency.
Physical residency and address-space exhaustion require native Linux tests;
user-mode emulators explicitly report unsupported assertions as NOT RUN.

## Stream allocator-class fit

An automatic FILE buffer requests 4088 rather than 4096 payload bytes. Its
8-byte allocator tag now fits the 4096-byte shelf instead of the 5120-byte
shelf. Static standard-stream buffers and explicit caller buffer sizes are
unchanged; `setlinebuf` shares the compact automatic default.

With 4096 live, touched streams, VmSize fell from 24804 to 20708 KiB and RSS
from 22592 to 18496 KiB: exactly 4 MiB saved. Shelf reuse retains this saving
after close. A 1 GiB workload of 64-byte buffered writes measured 330.53M versus
330.00M user cycles (effectively unchanged), with 0.84% more instructions and
0.195% more writes. This is a RAM saving, not a claimed throughput improvement.

## Rejected allocator reclamation

Unconditionally discarding freed large-shelf pages was tested and removed.
For 384 touched approximately 128 KiB blocks, a page-safe variant cut freed
RSS from 51.2 to 26.6 MB, but total free cost rose from roughly 17K to 2.72M
counter ticks; reuse would also refault pages. The allocator remains unchanged.
A future reclamation policy needs measured hot/cold tracking, not a syscall on
every large free.

## HTTP response ownership

The fetcher now compacts the body inside its response allocation and transfers
that owner to the caller. It no longer allocates a second whole-body copy.
Failed responses leave the caller's previous buffer intact. Framing checks
also reject short bodies, conflicting length/encoding headers and overflowing
numbers; valid chunk extensions remain accepted. Reads and writes share the
existing retry/full-write primitives.

A 64 MiB loopback response over eleven trials measured peak RSS of 135496
versus 67912 KiB, with median wall time of 19529 versus 17165 microseconds.
Both binaries used the same new mapping-growth implementation, isolating the
HTTP ownership change. The server was a separate process and a native wait4
launcher collected child RSS to avoid fixture/launcher high-water pollution.

## Shell reuse and expansion scratch

The chained store now searches its inactive tail for a suitable block instead
of allocating a duplicate when only the immediate next block is too small.
Growth arithmetic is checked before mapping. Only the two moving expansion
buffers have an adaptive release policy: a completed expansion above 1 MiB
keeps them warm through the command, including compound commands that end in
small work. A following small completed command releases exceptional capacity.
Incomplete physical lines and nested eval/source/trap execution cannot trim
the outer command's live storage.

The 2/8 MiB command-substitution regression measured retained RSS of 54028
versus 21252 KiB (-60.7%) after a small successor command. Repeated 8 MiB
compound commands measured 4867703606 versus 4867703879 instructions and
46904 versus 46928 faults. A million-iteration small-builtin loop had 0.145%
more instructions; task-clock variation did not establish a throughput change.
Five repeated 30000-file globs had equivalent instruction/fault counts.

Arena reclamation, parser-array trimming and long-lived builtin-buffer
trimming were deliberately deferred. Inferring aggregate arena demand from
individual short words would make repeated large globs remap their tails.
The retained expansion-buffer policy observes completed expansions, not a
universal transient high-water mark; unusual large-intermediate/small-result
expansions still need separate profiling before any no-churn claim.

## Coverage limits

The assembly inventory still has 242 shared routines: this pass improves an
existing primitive rather than duplicating tuned utilities. The performance
manifest now has 60 isolated benchmark anchors; the other 182 routines remain
performance-unproven. A benchmark anchor alone is not a hardware-floor proof.
No total-system RAM or kernel-boot performance claim follows from these tests.
Remaining measurement work includes booted-image idle/process RSS, kernel slab
usage and Canvas/graphics residency, plus a throughput-safe allocator cold-page
policy. Those costs have not been established by this component pass.

Validation on `box`: the standard, allocator, reserve, stream, spool, net,
tools, files, storage, util, term, writer and slurp lanes passed 1340351 checks.
The verify/exact/known assembly and constant-folding lanes passed 289880107
checks across native x86-64 and emulated AArch64/RV64. Mapping semantics also
passed 95 native AArch64 Linux checks and 87 lifted Darwin fallback checks.
The final Arch shell/expand/builtin suites passed 3372/3372 with default signal
dispositions (SSH-inherited ignored signals otherwise distort trap fixtures),
and the build/document kit passed 59/59.
