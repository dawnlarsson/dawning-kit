# Performance recovery and the hardware gap

The recovery removes repeated regex interpretation, reader/output copies and
unnecessary shell searches. It adds no new primitive family. These changes
improve measured workloads; they do **not** establish hardware optimality.

## Reproducible snapshot

Production baseline is `4afee03`. The intervening `22a69a2` commit edits only the
root README. The recovery source seal is
`4690fc035019b719dcc31ba150cba6d34169786f871d273cb077a9eb9915e441`.

| Linux x86-64 binary | SHA-256 |
| --- | --- |
| Baseline stock | `1a5f94b2e4cca5de90c17ccfcee24a6022a162278bf0a83c3203603bcfa350b4` |
| Final recovery stock B | `eeb132734721be9161d62ffc99af870f481aa6874cb19bc3b5de804b9a59e329` |
| B with symbols | `45861eec9cc6634c89c33c6a48cc46c7aad776915ee0d3caa7935cad05df56c7` |

The normal and symbol builds have identical `.text`, `.rodata` and `.data`.
Native measurements use Linux 7.1.8, GCC 16.2.1 and a Ryzen 9 9950X, CPU4 affinity.
The SMT sibling is not isolated. Other agents' builds/timings were serialized
outside these performance windows. ARM64/RV64 emulation verifies behavior,
not native throughput. Canvas and the kernel have no live performance claim.

Artifacts are under
[`artifacts/performance-recovery-2026-09-08`](../../artifacts/performance-recovery-2026-09-08).
`integration/results/{build-manifest,stack,engines}.json` pin final B, source/test
hashes, binaries, references, raw samples and exact command arguments. Earlier
A results remain in `integration/results/candidate-a/`; A lacks only the final
number-formatting reservation change. Never mix the two binary identities.

## What changed

* `rx_fixed` proves complete deterministic BYTE/CAPTURE/exact-COUNT graphs and
  reuses prepared shared search assembly. The proof has the existing 8192-work
  ceiling. A separate complete-literal view preserves the original prefilter
  metadata. Captures, wrapper boundaries, pending exhaustion, small scratch and
  insufficient work budgets retain the VM, preserving complexity diagnostics.
  Unused literal capacity is neither cleared nor read.
* `text_line_next` accepts an already-scanned prefix and an explicit destination.
  Split records avoid a second scan; sed reads directly into its pattern space.
  Rev borrows input records and copies once into its final output reservation,
  then reverses there. Canonical hexdump formats directly into its output buffer.
* Prepared byte sets let `cat -v` span ordinary tabs/newlines. Numbered cat
  reserves the maximum decimal width, formats once and releases unused space,
  removing a duplicate digit-count traversal.
* Shell retained-body allocation searches backward for the highest fit using
  existing shared byte searches. Counted scalar assignment names skip bracket
  searching; validated names ending in `]` use a bounded search.
* x86-64 `memory_first_of` bypasses feature dispatch below 32 bytes. Its kernel
  body stays integer-only. The existing scalar zero-count check remains; zero
  length costs an extra userspace branch in exchange for the nonzero short wins.

The production change is **+99 physical LOC, +101 nonblank LOC and +708 lexer
tokens**, including comments. This is a measured performance/storage tradeoff,
not another source reduction. The ELF grows 3,264 bytes: `.text` +3,072,
`.rodata` +192, `.data` unchanged. `.bss` grows 328,928 bytes, mostly the complete
regex proof views. The inventory remains 255 assembly primitives/765 architecture
bodies and gains one C proof constructor (3,841 C entries total).

## Before/after

The maintained `kit/bench_engines.py` ran 37 stack cases at 16 MiB and 41 engine
cases at 4 MiB, two warmups and eleven alternating/reversed rounds. Every timed
run checks exact status, stderr and stdout SHA-256, with hashing outside the
measurement. Output is a real seekable file, not `/dev/null`. Deadlines use
SIGALRM around blocking communication, avoiding Python timed-wait polling.
The table gives median **child user+system CPU milliseconds**, not core cycles.

| Case | Before | Recovery B | Change | Installed reference |
| --- | ---: | ---: | ---: | ---: |
| shell/loop | 76.037 | 69.726 | -8.3% | 369.518 |
| shell/parameter | 56.053 | 53.328 | -4.9% | 272.016 |
| shell/function | 102.865 | 99.974 | -2.8% | 642.176 |
| shell/redefine | 52.283 | 38.873 | -25.6% | 135.427 |
| cat-number | 14.511 | 13.575 | -6.5% | 14.034 |
| cat-visible | 10.465 | 4.529 | -56.7% | 7.487 |
| grep-group8 | 7.404 | 2.987 | -59.7% | 3.165 |
| grep-group32 | 19.602 | 3.275 | -83.3% | 4.219 |
| cut-field | 15.623 | 14.646 | -6.3% | 15.109 |
| cut-trim-first | 8.629 | 8.684 | +0.6% | 11.835 |
| sed-literal | 32.188 | 30.581 | -5.0% | 93.180 |
| sed-capture | 57.012 | 56.570 | -0.8% | 307.490 |
| awk-fields | 55.657 | 54.886 | -1.4% | 82.276 |
| rev | 16.483 | 11.811 | -28.3% | 116.070 |
| hexdump | 33.660 | 31.612 | -6.1% | 1078.764 |
| od-hex | 39.779 | 39.671 | -0.3% | 845.490 |
| grep-literal | 1.413 | 1.457 | +3.1% | 6.399 |
| cksum | 1.335 | 1.396 | +4.6% | 1.538 |
| base2lsbf/encode/w0 | 3.724 | 3.859 | +3.6% | 13.025 |

All 78 cases matched the recorded reference output on every run. GNU grep is
3.12-modified, sed 4.10, gawk 5.4.1, coreutils 9.11, Bash 5.3.15 and util-linux
2.42.2; individual hashes are recorded. Every B child-CPU median is below its
reference in this matrix, but paste is effectively tied and this matrix does
not cover all inputs/options. The broader dense cat screen remains slower
than GNU on several combined flag modes.

Several unchanged controls move by a few percent on small inputs. A separate
64 MiB/21-pair A control run found grep, CRC and base64url decode within noise,
but base2lsbf encoding retained about a **1.1% slowdown** (within-run interval
1.0079–1.0134). Its codec instructions and tables are unchanged; a layout cause
is possible but unproven. B's smaller matrix also shows that case slower. This
is an unresolved regression, not a waived pass or evidence that all refactor
performance has been recovered.

## Profiles and rejected variants

A native PMU sampling run used pinned **A** symbol binaries, checking each
invocation's reference output. Regex group32's old VM occupied roughly 84% of
samples and disappears after the proof shortcut. Rev's copy self-share falls
from 70% to 6.5%; searches, reversal and record overhead remain. Cat-visible's
shared set-span reaches 93.5%, with the old per-byte walker nearly absent.
These are sampled self-shares, not absolute cycle savings or instruction
latencies. Raw profiles and attribution are in `integration/profiles-results/`.

Rejected variants include an additional short-search branch that hurt bulk
controls, an unconditional assignment-name scan that hurt scalar loops, and a
generic od reservation with no demonstrated benefit. The first wall-only
base32 regression came from timeout polling; the corrected harness records
both wall and child CPU and retains the earlier evidence as superseded.

## Correctness

Final B passes **60,138/60,138 assertions in all 21 maintained suites**,
**742/742 exact before/after cases**, and **288,938,519/288,938,519 foundational
assertions** in the standard/verify/regex/error/leaving/writer/reuse_shell/audit/
process/checksum/net/bowl lanes, including x86-64 and emulated ARM64/RV64.
The configured built-in kernel/Canvas archive and standalone utilities build
succeed. The kernel build retains unused-function and objtool fall-through
warnings; successful compilation does not establish their absence or live
kernel correctness. The code atlas passes 26 unit tests and 80 browser checks;
assembly inventory, source seal and specialization checks agree.

The validation orchestrator initially lacked the sibling `shell` symlink needed
by three multicall suites, then hit a symlink/work-directory name collision.
Those failed orchestration runs are retained. After fixing only the artifact
fixture layout, every maintained suite ran to completion with zero failures;
`integration/results/performance-validation-final/results.json` contains the
complete result. The unchanged exact drivers passed in the first run. That
run also observed the concurrent kernel build creating object/command files
and generated Canvas `.S` files; no production C/assembly source or subject
binary changed. The final maintained-suite run has no identity changes.

Focused qualifications separately retained are 589,352 short-search guard/boundary
checks; 160,073 regex comparisons in each optimized and ASAN/UBSAN build;
803 regex ownership/reuse checks per binary; 2,251,774 shell allocator assertions
under ASAN/UBSAN; 1,390 consumer parity cases; and 250 numbered-cat parity cases
plus 37 actual-library reservation bounds checks. Their overlapping denominators
are intentionally not combined into a misleading unique-case total.

## What would establish a hardware floor

For a fixed contract, CPU, input distribution, alignment and cache regime,
derive lower bounds from necessary traffic, issue resources and dependencies,
then measure actual core cycles and the remaining gap. The fastest control
loop is an attainable calibration, not proof of a theoretical minimum. TSC
ticks, end-to-end child CPU and sampled percentages are different quantities.

AMD documents two 512-bit loads and one 512-bit store per cycle for Zen 5.
For aligned steady-state L1 data this gives optimistic 4 KiB traffic bounds of
32 core cycles for a complete absent-byte scan and 64 for copy or in-place
reverse. Setup, dispatch, dependencies, tails and execution-pipe limits can
raise the operation's bound. Current reversal spends four lane-shuffle
instructions per 64 bytes, implying at least 128 cycles per 4 KiB under AMD's
instruction issue model. This identifies a concrete 512-bit permutation
candidate. [AMD Hot Chips slides](https://hc2024.hotchips.org/assets/program/conference/day2/24_HC2024.AMD.Cohen.Subramony.final.pdf#page=9),
[AMD optimization guide 58455](https://docs.amd.com/v/u/en-US/58455_1.00).

The detailed conditional model and native PMU probe results belong in
`hardware/` and `shared/hardware/`. Short-call latency, produced buffers,
L1/L2/LLC/memory footprints and whole-tool I/O require separate qualification.
The coverage inventory still has **191/255 primitives without isolated timing**;
even isolated timing alone does not establish a floor. Global optimality is
not claimed. The next architecture work is full-width reversal, dense formatted
output batching, and avoiding repeated record-level work where the regex proof
and resource contract permit a complete-block operation.
