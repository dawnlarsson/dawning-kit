# Hardware bounds and measured gaps

The final pass gets selected shared kernels close to conditional hardware
bounds, with measured tool gains and unresolved shell regressions. The later
[shell performance pass](SHELL_HARDWARE.md) records the shell-specific follow-up;
the measurements below retain their original frozen source. See
[Final frozen candidate](#final-frozen-candidate) for the complete outcome.

A reference tool is a compatibility and practical performance comparison. The
target here is the remaining gap to a stated hardware bound for the same
operation, input and machine state. Recovery checkpoint `e626cdb` is qualified
separately in [PERFORMANCE_RECOVERY.md](PERFORMANCE_RECOVERY.md).

## Model

On the measured Ryzen 9 9950X (Zen 5), AMD documents two 512-bit loads and one 512-bit
store per core cycle. For aligned steady-state L1 data, a first traffic model is
`cycles >= max(required_read_bytes / 128, required_write_bytes / 64)`. Add
execution-pipe, dispatch/retirement and dependency constraints; take the maximum
of independent resource demands, not their sum. Instruction fusion and multi-op
instructions mean retired instruction count divided by eight is not a sound
standalone bound. [AMD Hot Chips presentation](https://hc2024.hotchips.org/assets/program/conference/day2/24_HC2024.AMD.Cohen.Subramony.final.pdf#page=9),
[AMD Software Optimization Guide 58455](https://docs.amd.com/v/u/en-US/58455_1.00).

This is a conditional model using published resource limits, not a proof about
undocumented execution mechanisms. It assumes sufficient independent work,
no splits/faults, warm code/TLB and L1 residency; it omits setup, tails, feature
dispatch, call/return and computation. These omissions make the traffic model
optimistic. A complete absent-byte scan must examine every byte; an early hit
needs its certified prefix. An odd in-place reversal need not touch its middle
byte. General substring search can skip bytes, so a full-input scan bound is
not a universal per-input lower bound for every search algorithm.

L2 adds a documented 64B/cycle L2–L1 transfer limit in each direction. LLC/DRAM
need their own transfer, ownership and physical-bus model. Capacity alone does
not establish residency. Cold destinations can incur write allocation; warm
owned destinations differ. Page-transfer I/O can avoid a userspace copy, so
whole-tool bounds must follow the actual transport contract.

The full primary-source model, exact workbook rows, source hashes and retained
documents are in
[`hardware/floor-model.md`](../../artifacts/performance-recovery-2026-09-08/hardware/floor-model.md).
AMD instruction timings are estimates, and unavailable latency entries remain
unknown. A measured traffic-only loop is an **attainable calibration**, an upper
bound on the best possible elapsed time for that loop’s own contract, not
proof of the theoretical minimum. It does not upper-bound a richer search or
translation operation that the loop does not implement.

## Actual core cycles

The native Linux probe uses grouped pinned `cycles:u` and `instructions:u`, with
setup, page faults, validation and printing outside measurement. All 1,729 raw
rows pass equal enabled/running-time checks, rejecting multiplexed groups.
Counter reset does not reset cumulative enabled/running time: those fields
validate scheduling only and are never used to infer frequency. TSC ticks are
recorded separately and never relabeled core cycles.

Seven batches per condition rotate measurement order. One MFENCE before each
batch drains warmup stores; another drains measured stores before stopping.
This is a sustained store-capacity contract, stronger than a bare function
return. Empty loop/call controls share those boundaries. Their cost cannot be
subtracted and called isolated callee latency because the work can overlap.

Native baseline probe SHA256:
`55add28092d5658260aa9030b5e65644a2edcf9409eaf6dadc0545c678a2c86f`.
The source is recovery B, library SHA256
`0123e0542b6ca25c44b11ba8fe387f1e7347f64c9757500b58a40cbccc483d3d`.
CPU 4 affinity uses the 9950X with 48 KiB L1D, 1 MiB L2 and 32 MiB shared LLC;
CPU 20 is its SMT sibling. Agent timing/build windows were serialized, but the
machine was not isolated. No native ARM64/RV64 speed is inferred from emulation.

| Aligned 8192-byte operation, L1 fit | Actual core cycles | Traffic bound | Recovery B instruction-shape bound |
| --- | ---: | ---: | ---: |
| Absent-byte search | 91.70 | 64 | 72 |
| Disjoint copy | 130.05 | 128 | At least 128 |
| In-place reverse | 262.06 | 128 | 256 |
| Arbitrary-table translation | 6993.55 | 128 | 6144 |

Copy is within about 2% of this conditional traffic bound. This establishes a
small bulk-L1 gap in the measured contract, not optimality for short calls,
alignments, cold memory or other CPUs. Baseline reversal is close to its own
instruction-shape bound but twice its traffic bound: changing its permutation
width has more scope than scheduling the same four lane-shuffle operations.
Baseline translation similarly sits near its scalar issue limit (eight loads and four
stores per four bytes); the 54.6× ratio to its weaker traffic bound is **not** a
promised speedup. Register-table vector lookup changes that instruction shape.

The historical inventory probe sometimes reverses uniform data. Its reversal
traffic column is therefore a constraint on the full-store implementation, not
a necessary-write bound for every measured input: equal reflected pairs need
no rewrite. The isolated reverse qualification uses a varied corpus with every
reflected pair different at 4/8 KiB. Final combined qualification explicitly
initializes that property for every reversal measurement. Translation uses a
nonidentity XOR-0x80 map, which changes every byte on every invocation; an
identity-map input would need a different bound.

Median cycles per useful input byte show which costs survive larger footprints:

| Span; intended capacity regime | Search miss | Copy | Reverse | Translate |
| --- | ---: | ---: | ---: | ---: |
| 8KiB; L1 | 0.01119 | 0.01588 | 0.03199 | 0.85371 |
| 256KiB; L2 | 0.02146 | 0.03283 | 0.03144 | 0.88953 |
| 8MiB; LLC | 0.03482 | 0.06612 | 0.03878 | 0.88302 |
| 256MiB; beyond LLC | 0.09411 | 0.29849 | 0.20544 | 0.89358 |

These names describe working-set capacity, not measured cache-hit rates; no
cache-miss counters were collected. Translation remaining near 0.85–0.89cycles
per byte supports an execution/lookup bottleneck. Copy and reversal become
cache/fabric-sensitive. Filtered SMBIOS records two 64-bit DDR5 channels configured at 6000 MT/s
(32 GiB per populated channel), giving an ideal raw bus ceiling of **96 GB/s**.
This is firmware-reported configuration, not a measured running memory/fabric
clock. Traffic that actually crosses DRAM is bounded by transferred bytes /
96 GB/s under that configuration. Logical input bytes alone do not establish
those transfers: residency, ownership, write allocation and writeback matter.
The single-core fabric limit and a complete numerical DRAM gap remain
unestablished. The filtered record is retained in
[`memory-configuration.txt`](../../artifacts/performance-recovery-2026-09-08/hardware/memory-configuration.txt).

For 14-byte copies, the same byte producer costs 17.08 cycles with an independent
copy source and 22.02 with the just-produced source. A word producer gives 14.04
versus 19.02. The source dependence is measurable even when the settled copy
itself overlaps the control loop. Removing intermediate buffers can therefore
matter more than changing the small-copy body. Short search replays approximate
earlier observed size histograms with 1024 quantiles; constructed first/last/miss
cases form an envelope because actual hit positions were not captured.

Reproduction scripts, disassembly, counters, exact size histograms and summaries:
[`shared/hardware`](../../artifacts/performance-recovery-2026-09-08/shared/hardware/README.md),
[`measured report`](../../artifacts/performance-recovery-2026-09-08/shared/hardware/RESULTS.md).

## Decisions from the gap

The qualified reversal uses two 64-byte VBMI permutations from 2048 bytes
upward. Its final aligned 4 KiB PMU result is **132.57 → 65.71 core cycles**,
against the conditional 64-cycle traffic bound. The 128- and 512-byte entry
thresholds were rejected for short/misaligned regressions. All 128 tested
alignment/rotation conditions improve at 2048 and 2049 bytes; the retained
2047-byte path can still cost about two extra cycles from dispatch/layout.
Real 4 KiB-record rev improves about 4.7%, demonstrating that the kernel
speedup and whole-tool speedup differ. Source cost is 15 lines and 32 table
bytes. Exact bounds/fallback checks pass 1,375,423 assertions and all 17
whole-tool cases match their references. The qualified candidate identity and
raw evidence are in the [reverse report](../../artifacts/performance-recovery-2026-09-08/hardware/reverse/report.md).

The qualified translation uses two 128-entry register lookups and selects by
the original byte’s high bit. Its contiguous scalar fallthrough removes the
first prototype’s six-byte regression: the repeated rotating six-byte test
returns from 8.40 to 8.40 cycles, with the wide body kept out of line. The final
8 KiB result is **8,254 → 261 core cycles**, about 31.6× faster in that probe.
The three required FP1/2 operations give an optimistic partial constraint of
192 cycles per 8 KiB, so the measured ratio is 1.36×; this is not a complete
attainable floor. The masked register merge uses FP0/1/2/3 and cannot be charged
as another compulsory FP1/2 operation. Alias checks retain the old four-byte
load/store grouping when input and table overlap. All 1,031,025 isolated
assertions and 22,822 maintained checks per native binary pass. Source cost
is 30 lines, including the explicit readable-256-byte-table contract. This
paired probe has its own baseline placement and binary identities; it is
distinct from the earlier 6,993.55-cycle inventory probe. See the
[qualified translation report](../../artifacts/performance-recovery-2026-09-08/hardware/translate/layout/README.md).

The bounded class-span candidate is **rejected for this pass**. Its final
8-byte scalar prefix followed by register-table lookup improves an aligned
4 KiB scan from 3,108.78 to 193.96 core cycles. The required lookup/mask
operations imply an optimistic partial execution constraint of 128 cycles;
this is not a complete attainable call floor. Long-span consumers improve
roughly 1.6–2.6×, but dense `cat -bAs` slows 2.92% and a steady short shell
split loop slows 4.11% in the final scaled paired run. These composition costs
remain after two bounded layout/prefix revisions. The original bounded and
unbounded span implementations remain in place. The rejected source, guarded
fixture and every timing sample are retained in the
[span experiment](../../artifacts/performance-recovery-2026-09-08/hardware/span/report.md).
Short calls and actual span lengths must determine a future dispatch design;
an aligned throughput gain alone is insufficient. Kernel paths stay outside
vector state.

Dense formatted cat has a different gap: per-byte output calls repeatedly update
the shared used counter and check capacity. A local output cursor and prepared
identity spans can remove those operations before any new assembly is needed.
The accepted local-cursor CAT path costs 98 C lines and 2,624 allocated
code/data bytes; the smaller ELF file reflects reduced alignment padding,
not reduced instructions. It passes 1,464 exact cases plus 20 bounds/wrap
checks. Dense modes improve 1.46–5.70×; plain and CAT_SHOW keep their existing
paths and scaled controls remain approximately unchanged.

Its output-failure path needs care: the failure flag records status, but later
writes can still succeed, so discarding the remainder would change behavior.

A floor claim must name the operation, source/binary identity, CPU, alignment,
input/size distribution, cache/ownership state and completion contract. Then
report the strongest applicable bound, actual core cycles and remaining gap.
The coverage inventory still has 191/255 primitives without isolated timing;
measured kernels do not establish a global optimum for the stack.

A local native Darwin build was also attempted. Both recovery B and the
current source fail with the same first 20 diagnostics (including missing
futex definitions and unsupported Darwin aliases). This is a baseline target
limitation, not successful native ARM64 qualification; Linux ARM64 emulation
and the configured Linux builds provide the portable checks for this pass.

Local Clang Linux syntax checks pass for ARM64. X86-64 and RV64 report the
same five and seven errors as recovery B, respectively (existing compiler
builtin compatibility and nested auto-type macros). These auxiliary checks
do not replace the repository’s GCC Linux builds and cross-architecture
verification.

## First combined candidate: regression isolation

The accepted changes add **143 physical/nonblank production lines** and 712
lexer tokens to recovery B: 45 library lines and 98 CAT lines. The normal ELF
is 896 bytes smaller because of alignment padding, while allocated `.text`
grows 1,408 bytes and `.rodata` grows 1,600 bytes: **3,008 more code/data bytes**.
`.data` and `.bss` are unchanged. These are measured performance tradeoffs,
not a code-size reduction. Assembly string tokens are not instruction counts.

- Production source digest: `6a16eafd022ab5db3d77c5113a3c1a1120df5ba4f20e445310854a36e4339eea`.
- Stock SHA256: `9dd5c8301a4f9eac9446b765cb9a4af406349ec02398496fbdb92b568b6135e5`.
- Symbol build SHA256: `140e7d4da01cee63e2a77ecdd9d8eb8f171fd564e1aeaaebb20646e0929be04b`.
- Library SHA256: `bb289a2fdae77ed76045ec9b6be0eafb723e72fbe434446ad6f89ec169ee09cc`.

All 81 production files match the frozen native build manifest. Normal and
symbol builds have identical `.text`, `.rodata` and `.data` sections. The
shell, standalone utilities and configured built-in kernel/Canvas archive
build successfully. Existing unused-function/objtool warnings are retained;
no live kernel or Canvas performance was measured. The inventory remains
255 shared primitives, 765 architecture bodies and 84 aliases, with 3,841 C
entries. Atlas checks pass 26 unit cases and 80 browser checks.

The 78-case matrix is exact, but the first combination fails performance
acceptance: shell parse, sed capture and seq integer regress about 16–18%, and
short shell loops regress about 7%. All unchanged C bodies retain the same
normalized instructions, registers, calls and branches. These results are
held for diagnosis, not presented as a qualified final candidate.

A controlled alignment variant adds `.balign 32` before x86-64
`memory_copy_apart`. Internal `string_copy` padding shrinks by the same 16
bytes, so 3,408 later symbol addresses, total text size and data sections stay
unchanged. This alone restores sed capture from 65.622 to 56.291 ms CPU,
matching recovery B’s 56.392 ms. The one-line fix is accepted. Parser and sequence
regressions required separate investigation; no particular hardware event is
inferred from address placement alone. See the
[alignment isolation](../../artifacts/performance-recovery-2026-09-08/hardware/integration/copy-alignment/README.md).


Restoring all 1,381 data/BSS symbols to their recovery-B addresses does **not**
remove the remaining regressions. Text/RO symbol addresses and instruction
topology stay fixed; only data-address relocations change. Nine-case exact
paired evidence is retained in the
[data-placement rejection](../../artifacts/performance-recovery-2026-09-08/hardware/integration/data-placement/README.md).
No fixed-address or writable-section-layout change is justified by this test.


## Decimal carry and execution-context controls

The accepted decimal-series change makes the common non-carry path fall through
and moves the existing carry block behind the existing tail exit. No new
instructions execute per emitted record. The complete normal ELF differs from
the aligned candidate in only 121 bytes, all inside the same 656-byte routine;
every other file byte and every symbol address/size is identical.

Seven paired PMU groups show 3,672,493 → 2,428,656 cycles (33.9% lower) and
50,386 → 2,778 branch misses (94.5% lower), with identical retired instruction
and branch counts. All 2,677,038 maintained fixed-number checks pass on native
x86-64 and emulated ARM64/RV64. These counters establish the branch behavior
change for the measured workload; they do not prove a complete decimal-output
floor. See the [decimal qualification](../../artifacts/performance-recovery-2026-09-08/hardware/integration/decimal-fallthrough/README.md).

Separate executable files initially showed an additional parser slowdown after
this decimal-only change. A diagnostic executable with the entire decimal
routine replaced by traps still completes the parser workload, establishing
that the parser does not architecturally execute those changed bytes. Fresh
copies and hardlinks did not consistently eliminate the difference. A stronger
control overwrites one owned executable inode/path with each exact subject
image, verifies the full SHA256 and warms that subject once before each timed
run. Image replacement, verification and warmup stay outside the timer.
Physical page mappings are not observed or claimed to be identical.

In that 15-round control, the extra decimal-only parser difference disappears:
aligned and reordered candidates take 8.126 and 8.111 ms CPU. The remaining
recovery-B → reordered costs persist: parser +14.61%, shell loop +5.95%,
function loop +5.79%. Integer seq improves 6.23%; sed capture is +1.30% with
overlapping ranges. Exact statuses, stdout hashes and stderr match for all
runs. This narrows the inference to execution context for the extra anomaly;
it neither identifies the microarchitectural mechanism nor removes the
remaining recovery-checkpoint regressions. Raw evidence is retained under
[same-image](../../artifacts/performance-recovery-2026-09-08/hardware/integration/results/same-image/timings.json)
and [clone-controls](../../artifacts/performance-recovery-2026-09-08/hardware/integration/results/clone-controls/).

## Rejected parser fold

Caching keyword classification in existing token padding removes 25 source
lines without enlarging tokens. Independent lifecycle/grammar review and the
shell, lexer and alias checks pass. However, parser retired instructions fall
3.16% while CPU time worsens 9.55% and core cycles worsen 10.18%. Short arguments
also incur 1.61% more instructions from eager classification. The candidate is
rejected; production parser code is unchanged. Fewer instructions and fewer
lines do not establish lower latency. See the [retained rejection](../../artifacts/performance-recovery-2026-09-08/shell/keyword-cache/report.md).


## Final frozen candidate

This pass retains the measured reverse/translation kernels, CAT cursor batching,
copy-entry alignment and decimal carry fallthrough. It rejects the bounded-span
and keyword-cache prototypes. **This is a partial hardware optimization result,
not a regression-free or globally optimal stack.** The unresolved parser and
shell-loop costs below remain acceptance limitations.

- Production source digest: `7fca743f4fae01df406889f619cb18bcc1349adef934b7175c9c18afccfaa9f4`.
- Stock SHA256: `b2e2d78df400e71a325c6616962e3d4f121c1374f72aa21223d9e55a8d9e27f2`.
- Symbol build SHA256: `5b21a980c212112b20416c110b281e32fc57adf7537181385ac87e80c2f333a4`.
- Library SHA256: `7f4527cf7a666be622611b1993510cc4cabbb6b2e767d63d9f33865aa8971f10`.

All 81 production source hashes match the native build manifest. Normal and
symbol builds have identical allocated text/read-only/writable data sections.
The final rebuild is byte-identical to the independently reviewed decimal
candidate. Production cost versus recovery B is **145 physical/nonblank lines
and 714 lexer tokens**: 47 library lines and 98 CAT lines. Allocated code/data
grows 3,008 bytes (.text +1,408; .rodata +1,600); .data/.bss do not grow. The
normal ELF shrinks 896 bytes through padding changes. This is an explicit
performance-versus-size tradeoff.

Final broad PMU source SHA256:
`d476e23c4e5fcce118be506e2cd2f8ba217b781897f7fcf18a355e68933bdafa`;
probe binary SHA256:
`88542ff8d397f624cd03c6357d30d3326c822d8b10f656b1d82cdb6c1594af18`.
All 1,729 rows are valid unmultiplexed groups. Reversal now explicitly starts
from repeated 0…255 bytes: all reflected pairs differ at every measured size.
Both orientations require every byte to change, including after warmup.
Translation retains the XOR-0x80 table, changing every byte on every call.

| Final aligned 8 KiB operation, L1 fit | Core cycles | Applicable optimistic constraint | Measured / constraint |
| --- | ---: | ---: | ---: |
| Full absent-byte scan | 84.58 | 64, necessary read traffic | 1.32× |
| Disjoint copy | 130.05 | 128, full-store traffic | 1.02× |
| In-place reverse, every reflected byte differs | 136.70 | 128, necessary write traffic | 1.07× |
| Nonidentity table translation | 257.05 | 192, partial constraint for this vector lookup sequence | 1.34× |

These include the stated loop/call/store-drain contract and exclude setup
outside the measured batch; controls are not subtracted. The changed reverse
corpus makes the final result a valid absolute bound comparison. Its speedup
claim comes from the separately paired, varied-corpus reverse experiment,
not a ratio against the historical uniform inventory run. The 192-cycle
translation constraint applies to this sequence of lookup/mask operations;
a different algorithm or a simpler table could change it. The operation-wide
traffic constraint is weaker at 128 cycles.

The final 78-case end-to-end matrix uses 11 alternating rounds per binary,
16 MiB stack inputs and 4 MiB engine inputs. Every timed invocation matches
status, stderr and stdout SHA256; validation and hashing are outside timing.
Selected child-CPU medians versus recovery B:

| Workload | Recovery B, ms | Final, ms | Change |
| --- | ---: | ---: | ---: |
| tr case translation | 5.313 | 2.674 | −49.7% |
| seq integer | 0.954 | 0.898 | −5.9% |
| seq padded | 0.290 | 0.273 | −5.9% |
| cat numbering | 13.448 | 12.942 | −3.8% |
| sed captures | 56.740 | 56.027 | −1.3% |
| shell parsing | 7.094 | 8.284 | +16.8% |
| shell loop | 69.329 | 74.002 | +6.7% |
| shell function loop | 98.673 | 104.369 | +5.8% |

The parser/loop regressions recur in controlled runs with unchanged logical
work. Their exact hardware cause is unresolved. Other small increases remain
in the full table (for example sed literal +1.8% and UUID JSON +5.2%, the latter
about 21 µs); noisy short codec medians must be read with paired samples.
All 78 candidate CPU medians are below the installed references in this matrix,
with paste effectively tied at 1.002×. This is not a universal tool-speed claim:
the separate dense CAT matrix still includes modes slower than GNU, and kernel,
Canvas, native ARM64/RV64 and many input/cache regimes lack performance proof.

The retained [final evidence](../../artifacts/performance-recovery-2026-09-08/hardware/final/)
contains source/ELF identities, complete per-run timings, raw PMU groups, the
probe source, disassembly and build logs. Raw speedup tables for separately
qualified reverse/translate/CAT variants remain labeled as isolated experiments.


## Final correctness qualification

The frozen candidate passes all **60,153/60,153** checks in 21 maintained
consumer/shell suites and **742/742** exact differential cases against the
pre-recovery `4afee03` binary (123 text, 177 utilities, 442 runtime). The isolated
util-linux pair passes 1,002/1,002 for both candidate and baseline. There are no
failed suite runs and candidate/source identity checks remain unchanged. The
[final validation report](../../artifacts/performance-recovery-2026-09-08/hardware/integration/validation/results/report.md)
retains all tallies, exact cases and source/binary pins.

The final foundational run passes **288,972,068/288,972,068** assertions across
standard/assembly verification, regex, errors, exit behavior, writer ordering,
shared utility formatting, builtin repairs, processes, checksums, networking and
Bowl parsing. Native x86-64 and emulated Linux ARM64/RV64 all pass. All 100 build
manifest files, including generated build files, and the stock binary match
before and after. The earlier decimal-specific run adds its separately reported
2,677,038 fixed-number assertions; these overlapping test sets are not combined
into a misleading unique-case count. See the [final foundational log](../../artifacts/performance-recovery-2026-09-08/hardware/final/results/foundation.log).

The shell, standalone utilities and configured built-in Linux kernel/Canvas
archive build successfully. The new guards exercise the real assembly symbols,
all detected/forced-off x86 feature tiers, inaccessible zero-length pointers,
both alias directions, arbitrary/noninjective maps and buffer boundaries.
Cross-architecture results describe the tested Linux/QEMU configurations;
the guard fixture uses 4 KiB pages. No physical ARM64/RV64 or live kernel/Canvas
performance claim is made. Existing kernel unused-function/objtool warnings
and baseline Darwin build limitations are retained in the evidence.

The final atlas passes 26 unit and 80 browser checks; inventory, source seal,
performance-evidence and specialization checks pass. A [final independent source
review](../../artifacts/performance-recovery-2026-09-08/hardware/final/source-review.md)
finds no correctness blocker; a separate [hardware-model review](../../artifacts/performance-recovery-2026-09-08/hardware/review/report.md)
checks the bounds and inference limits. The performance inventory still leaves
**191/255 primitives without isolated timing**, and isolated timing itself is
insufficient to prove a hardware floor.
