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

The native ARM Linux VM's full shell/expand/builtin comparison is **not fully
green**: it has 23 existing reference mismatches with Ubuntu's dash
0.5.12-9ubuntu1 and bash 5.2.32-1ubuntu1.1 (Arch uses dash 0.5.13.4-1 and bash
5.3.15-1). A writable-tree control from `dc28f20` with the new tests passed
3373/3397; the candidate passed 3374/3397. Their output diff contains only the
fixed RSS regression (control retained 54068 KiB); every other mismatch is
identical. An initial read-only host-mounted run also broke a mkdir-based
fixture, so it was replaced by the writable-tree comparison. The VM was
returned to its previously stopped state after testing.

## Shell semantics checkpoint

The bounded semantics pass started from source baseline `11c0064`. It extended
the existing lexer and builtins for associative-assignment subscripts containing
unquoted whitespace, Bash-compatible `caller` source reporting, and the
20-column `shopt -o` listing, with negative coverage preserving ordinary word
splitting. On the native x86-64 `box`, the shell, expansion and builtin suites
passed 1725/1725, 1403/1403 and 249/249 checks respectively (3377/3377 total)
after resetting SSH-inherited signal dispositions.

The corrected, bounded POSIX gap probe reported no remaining GAP rows. That
means only the enumerated probe cases matched their references; it is not a
claim of complete POSIX conformance. Explicit Bash-policy gaps remain for
declare/readonly status behavior, noclobber status behavior, `lastpipe`, and
the larger Bash `ulimit -a` listing.

The lexer decision-byte change was checked on the Ryzen 9950X Arch host with a
CPU-pinned 200,000-line ordinary-word workload over seven samples. The
`11c0064` control and candidate measured median instruction counts of
694857388 and 694854219, with 44 page faults for each. This establishes no
measurable instruction overhead for that controlled hot path. Task-clock
results were noisy, so it is not evidence of a throughput improvement, a
broad shell benchmark, or a hardware-floor result.

# Boot and idle RAM follow-up — 2026-09-05

This closes the total-system measurement gap above with a controlled KVM
guest. The baseline artifact was `dist/bootx64.efi`, SHA-256
`68993ee7dcccf399300e73f8220c12cba5a27cdf8ccfb6410c83f8b7dac3ab83`:
Linux `7.2.0moonwater-25` build 433, built on `box` at 12:46:15 CEST.
The measured host image and final kernel configuration were read from the
checksum-isolated build at `/tmp/moonwater-dawning-kit-1638177572`. This
provenance identifies the artifact exactly; it does not claim that an
uncommitted source tree has the same contents.

Each guest used two host CPUs and KVM. The Canvas case added
`-vga none -device virtio-gpu-pci`; Canvas selected Virtual-1 at 2560x1080 and
50 Hz. The no-GPU case used `-vga none`. After 15 seconds the serial fixture
read `/proc/meminfo` and every `/proc/PID/status`, then powered off. A run is
reproduced by substituting the desired memory size and optional device in:

```
qemu-system-x86_64 -m 512M -smp 2 -cpu host -enable-kvm \
  -kernel dist/bootx64.efi -vga none -device virtio-gpu-pci \
  -no-reboot -display none -serial stdio \
  -append 'console=ttyS0 drm_client_lib.active=' < commands
```

The serial input was the following. The first line is sacrificial because this
console drops the beginning of its first piped line. Meminfo is collected
before the process loop so the loop's transient `cat` processes cannot change
the table above. ANSI control sequences and a leading `" $ "` prompt are
removed before parsing the last pair of each marker.

```
this line is eaten by the console
sleep 15
echo MW_MEMINFO_BEGIN
cat /proc/meminfo
echo MW_MEMINFO_END
echo MW_PROCESSES_BEGIN
for p in /proc/[0-9]*; do echo MW_PROCESS_$p; cat $p/status; done
echo MW_PROCESSES_END
echo MW_SLAB_BEGIN
cat /proc/slabinfo
echo MW_SLAB_END
echo MW_MODULES_BEGIN
cat /proc/modules
echo MW_MODULES_END
echo MW_MOUNTS_BEGIN
cat /proc/mounts
echo MW_MOUNTS_END
echo MW_BUDDY_BEGIN
cat /proc/buddyinfo
echo MW_BUDDY_END
poweroff
```

The figures below are KiB from one matched sweep. `Non-free` is
`MemTotal-MemFree`; `Unavailable` is `MemTotal-MemAvailable`, the number tools
commonly label used; `outside MemTotal` is the QEMU memory size minus
`MemTotal` and therefore includes the firmware hole as well as kernel-reserved
pages. `QEMU size-Free` adds that last column to `Non-free`.

| QEMU RAM | Canvas | MemTotal | Non-free | Unavailable | Outside MemTotal | QEMU size-Free |
| ---: | :---: | ---: | ---: | ---: | ---: | ---: |
| 128 MiB | no | 105508 | 14576 | 16540 | 25564 | 40140 |
| 128 MiB | yes | 105508 | 31012 | 32956 | 25564 | 56576 |
| 256 MiB | no | 234340 | 17584 | 21004 | 27804 | 45388 |
| 256 MiB | yes | 234340 | 36200 | 39616 | 27804 | 64004 |
| 512 MiB | no | 492004 | 18696 | 24456 | 32284 | 50980 |
| 512 MiB | yes | 492004 | 35364 | 41108 | 32284 | 67648 |
| 2048 MiB | no | 2037988 | 28844 | 104036 | 59164 | 88008 |
| 2048 MiB | yes | 2037988 | 51948 | 127092 | 59164 | 111112 |

The 2 GiB Canvas result reproduces a roughly 120 MiB `used` report, but it is
not a 120 MiB process or Canvas allocation. This guest set
`vm.min_free_kbytes` to 45056; zone watermarks keep those still-free pages out
of `MemAvailable`. Kernel reservation also grows with RAM because every
physical page needs metadata: the early memory report named 29420, 31660,
36140 and 63020 KiB reserved at 128, 256, 512 and 2048 MiB respectively. The
fixed image contribution in the same report was 16723 KiB code, 1042 KiB
read/write data, 3392 KiB read-only data, 1908 KiB init and 388 KiB BSS.
Lowering the watermark would make the displayed number smaller without
freeing an allocated page, while weakening the reserves used by reclaim and
high-order allocation, so it was not changed.

At 512 MiB, matched idle boots isolated Canvas itself. Without a GPU the
guest had 18564 KiB non-free; with the 2560x1080 output it had 37192 KiB.
The dominant fixed costs are observable rather than inferred: Cached/Shmem
rose by exactly 10800 KiB for the pinned 10240-byte-pitch scanout, and
VmallocUsed rose from 3036 to 6972 KiB for two text-pane mappings. Each pane
has a 479-column by 512-line cell ring of 1924 KiB. The old diagnostic
said `479x133`, the maximum visible grid rather than the allocated history;
the Canvas log now reports the allocation's true dimensions and KiB, and also
reports scanout KiB. The history, resolution, future-resize ceiling and
single-buffer rendering design are unchanged.

Userspace is not the missing bulk. At idle, init was 1316 KiB RSS; the
terminal was 3248 KiB including its 1924 KiB shared ring mapping; and the
interactive shell was about 1500 KiB. Their approximately 1296 KiB shared
executable mapping is the same physical object and must not be summed once per
process. The shell's roughly 14 MiB virtual size is reserved address space,
not resident RAM. Likewise, rootfs/devtmpfs `size=` mount options are limits,
not allocated memory.

Host process RSS is a third, different number. Twenty seconds into matched
512 MiB boots, QEMU itself was 126172 KiB RSS without the GPU and 141548 KiB
with it, while the guests reported the much smaller non-free figures above. KVM's
host mapping can retain pages touched during kernel decompression and init
after the guest has freed them, and QEMU contributes its own code and device
state. Host QEMU RSS is therefore not evidence of a guest leak or of the
bare-metal idle footprint; returning guest-free pages to a VM host is a
balloon/free-page-reporting policy question.

One real retained cost was removed. The default profile explicitly disabled
merging compatible SLUB caches, with no corresponding allocator-debug or
hardening policy. Seven paired 512 MiB Canvas boots of the same image, using
the kernel's `slab_merge` override for the candidate, reduced median Slab from
7536 to 7072 KiB and median SUnreclaim from 6940 to 6464 KiB. Five stabilized
2 GiB pairs independently reduced median Slab from 7680 to 7104 KiB and
SUnreclaim from 7072 to 6512 KiB. Total free-page counts varied by several
MiB as boot work and page caches settled, so no larger whole-system saving and
no boot-speed improvement is claimed. `CONFIG_SLAB_MERGE_DEFAULT=y` now makes
the measured lower-slab policy the default; `slab_nomerge` remains available
for allocator-debugging or cache-isolation runs.

This fixture has no physical discrete-GPU VRAM accounting and only one output.
Real multi-output memory adds one 32-bit scanout per output (pitch times
height), and hardware drivers may retain device-specific allocations. Those
must be measured on the reported machine before attributing its exact number.

## Shared storage reads and benchmark-driver correction

`storage_read` now zero-pads only the unread suffix after EOF or an error.
A successful positional read already writes every requested byte, so clearing
the entire destination first was duplicate work. `stdbuf` now shares this
reader for out-of-line ELF program tables, fixing interrupted and short reads;
its three private integer decoders also use the existing alias-safe unaligned
load primitive. No replacement utility or new assembly implementation was added.

The permanent `storage_io` fixture injects EINTR, short reads, EOF and hard
errors into the actual production path, checking guards, padding, pointer and
offset advancement, and the ELF handoff. The `11c0064` control fails five of
147 checks; the candidate passes all 147 on native x86-64 and emulated AArch64
and RV64. The storage/process/util-linux integration matrix passes 1512/1512.

Nine alternating before/after pairs on the Ryzen 9950X, pinned to CPU 0, used
the same warm memfd and benchmark source with `11c0064` versus the candidate
reader. File creation and initial touching were outside the timed interval;
every sample read 1 GiB and validated the result. Median counter ticks were
289711468 to 245615398 for 4096-byte reads (262144 iterations), and 191809455
to 164284725 for 65536-byte reads (16384 iterations): about 15.2% and 14.4%
less time in these controlled reader workloads. This is not a disk-throughput,
whole-installation or hardware-floor claim. Reproduce with
`kit/bench storage-read 4096 262144` or `kit/bench storage-read 65536 16384`;
only native runs provide performance evidence.

The benchmark driver previously discarded arguments after the lane name.
It now preserves them through native/taskset and emulated runners, with a
three-target argument-boundary fixture. Earlier reserve measurements above
used direct benchmark binaries with explicit arguments; the printed driver
commands now reproduce their intended sizes too. Remote build synchronization
also excludes `.claude` agent worktrees. The local kit fixtures pass 61/61.

## Bounded format accounting

The shared `printf` engine now rejects literal widths and precisions above
`INT_MAX` with `EOVERFLOW`, rejects an otherwise valid total length above the
same public return boundary, and returns minus one after an immediate stream
write failure. Oversized fields reuse the existing checked decimal cursor;
there is no second format-number parser. A bounded buffer retains only its
resident padding prefix and accounts for a padding suffix above 64 bytes in
one operation, instead of continuing one 64-byte iteration per requested
chunk after the destination is full. Widths around that cutoff and the
overflow/error paths have permanent regressions.

The formatter can preserve the required negative result but not yet the exact
downstream errno: `format_stream_write` and `stream_put_bytes` expose a byte
count, while the stream's raw syscall path does not translate the kernel error
into `errno`. Full POSIX write-error propagation therefore remains a known
stream-layer gap; this change does not replace the lost reason with a generic
one or claim that coverage.

`kit/bench format-bounds` preserves the exact two call shapes. `wide` measures
one `snprintf(NULL, 0, "%100000000d", 7)` per sample; `normal` measures 131072
calls per sample of `%08d:%-12s:%6.3f`. Each process takes nine internal
samples and reports their median. The default runs both, while
`kit/bench format-bounds wide` and `kit/bench format-bounds normal` isolate
them for counters.

The control was revision `a85adccf49c9bdc566643040b699e540bacc0e77`, and
the candidate differed only in the format source and benchmark. Both were
built by GCC 16.2.1 20260810 with the benchmark driver's freestanding static
`-O2` command. Five alternating `perf stat` processes were pinned to CPU 15
on the Ryzen 9950X. Each wide process therefore measured nine formatted
calls: median retired instructions fell from 309541271 to 166869, and median
cycles from 113588871 to 277445. Each ordinary process measured 1179648
formatted calls: median retired instructions rose from 4043451553 to
4146080946, a measured 2.54% correctness cost. Elapsed ticks on this shared
machine crossed CPU frequency states during the ordinary pairs, so no
ordinary elapsed-time improvement or no-regression claim is made. The linked
benchmark's text grew from 81840 to 81904 bytes, 64 bytes; data and BSS were
unchanged. Emulated ARM64 and RV64 runs validate benchmark behavior and
instruction shape only, not hardware performance.

## Boot candidate validation

The final production source at `25988a0` was synchronized into the isolated
kernel cache with `.claude`, `.git`, `linux`, `artifacts`, `fs` and `dist`
excluded from the transfer. The incremental build produced
`7.2.0moonwater-25` build 436, a 9327616-byte image with SHA-256
`a66994b85077a576af659e8495c663a10f0bfb40473931b46dc0b5d2a1f495aa`.
Its final kernel configuration contains `CONFIG_SLAB_MERGE_DEFAULT=y`.

The serial boot lane passed 42/42 and the QMP/pixel Canvas lane passed 25/25.
A separate 128 MiB Canvas boot reached an orderly poweroff and logged the
10800 KiB scanout plus both 479x512, 1924 KiB rings. The earlier 512 MiB image
with only the kernel/profile changes independently passed the same 42/42 and
25/25 lanes before the final userspace resync. `/proc/slabinfo` is disabled in
this production profile, so cache-by-cache attribution remains unavailable;
the report uses `/proc/meminfo` aggregates and the paired `slab_merge` boot
override instead.

## Final integration checkpoint

The final format/shell/expansion/builtin/kit run on `box` passed 6088/6088:
875 format checks on each of x86-64, AArch64 and RV64, 1750 shell checks,
1403 expansion checks, 249 builtin checks and 61 kit checks. The full shell
runner includes 25 environment-launcher cases absent from the narrower shell
agent run. The earlier stream, spool, storage-read, terminal and editor lanes
also passed; storage/process/util-linux passed 1512/1512 separately.

The first integrated shell attempt had four trap-list mismatches caused by the
Python launcher leaving SIGXFSZ ignored. The final launcher restored default
INT, QUIT, PIPE, XCPU and XFSZ dispositions and unblocked them immediately
before exec. All shell cases then passed; no production behavior was changed
to conceal the fixture inheritance. Broader native ARM shell-reference
limitations from the earlier pass remain as recorded above.

The reconciled source seal is
`1ee9dca2632912d2fd9a4417749639ddab136b696b09c56f33edee401c502768`:
3761 C functions and the unchanged 242 shared assembly routines. This pass
removed three private load decoders and added two shared-path helpers; it
does not claim additional routines have reached a hardware floor.

The verified final image was copied to `dist/bootx64-idle-pass.efi`; its SHA-256
matches the build-436 value above. The original `dist/bootx64.efi` remains
unchanged at the recorded baseline hash. These generated images are not Git
source files; implementation, tests, benchmarks and this report are committed.
