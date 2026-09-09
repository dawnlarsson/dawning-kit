# Shell performance: remove work before changing instruction width

The shell copied every token into a lexer arena before the parser copied it
again, rescanned already-normalized arithmetic cursors, built glob patterns for
wholly quoted fields, and forked a helper for numeric `kill`. This pass removes
those costs through the existing storage, byte-span and utility interfaces.
The measured shared-assembly experiment is rejected; its zero-return control
is flat and important consumers regress.

The baseline is `c6ae6d5`. Recovery checkpoint `e626cdb` remains a separate
comparison: improvements relative to current HEAD do not automatically recover
older regressions. The frozen production source digest is
`dab083d7c00a1f47e6d8f029a1797ffb0853fd75b676ae5671693b9852e4d30c`.
Artifact root: `artifacts/shell-hardware-2026-09-09`.

## What changed

- Lexer tokens borrow counted source spans. The parser owns the stable,
  terminated spelling before input reuse. Removing the extra arena also removes
  growth, rebasing and three helper functions: 107 lexer lines disappear.
- Arithmetic precedence productions rely on their lower production's normalized
  cursor. Exact diagnostic counts remove 4.2 million zero-return span calls from
  each historical loop/function workload, before any assembly change.
- A nonempty field whose marks are all `MARK_QUOTED` uses `memory_span_byte` to
  prove that splitting and glob preparation are unnecessary. Its retained copy
  still owns the value before later arguments can mutate shell variables.
- Cached builtin names and parsed scalar assignment names omit guaranteed-miss
  delimiter searches. The instrumented loop's terminated byte searches fall from
  600,032 to 28 startup calls. This diagnostic counts terminated
  `string_first_of` only; it does not count bounded `memory_first_of` calls on
  genuine cache misses or subscripts. Scalar/cache-hit searches are removed by
  source proof. Isolated cycle gains for these two guards remain unestablished.
- Numeric `kill` calls the existing utility parser inside a saved-argv
  transaction. It flushes pending output before a potentially fatal self-signal
  and restores arguments before deferred traps. Job specifications retain the
  shell's job-table path. No synthetic helper SIGCHLD is generated.

- Ordinary terminal wildcard matches reuse the existing globstar result path.
  A directory entry already provides the needed name; literal suffixes,
  trailing slashes and recursion limits retain their checks. Five actual-call
  controls verify 32→0 and 1→0 terminal statx calls, with literal/suffix/slash
  counts unchanged. Readable but unsearchable final directories now agree with
  Bash. The [glob report](../../artifacts/shell-hardware-2026-09-09/parser/terminal-glob/report.md)
  records the GNU source, permission repair and exact compatibility policies.

The net production change is **−94 lines and −3 C functions**. The `.text`
section shrinks by 1,936 bytes; `.rodata`, `.data` and `.bss` sizes are unchanged.
The complete stripped ELF grows by 1,920 bytes (1,435,296→1,437,216), so this is
not a smaller-file claim. Normal and symbol-preserving builds have identical
measured allocated sections. The lexer frame shrinks from 56 to 32 bytes;
that 24-byte frame reduction does not change total BSS size. No new primitive
or ASM body ships, and `library.c` remains byte-identical to the baseline.

## Final before/after measurements

Native Linux 7.1.8, GCC 16.2.1, Ryzen 9 9950X, CPU 15 affinity (SMT sibling 31).
The core was selected from an untimed host-load sample. Agent build/timing jobs
were serialized, but the machine and sibling were not isolated. Eleven rotating
rounds after two warmups compare baseline, recovery checkpoint, final and the
installed reference. The maintained harness uses the real multicall argv0,
blocking child waits and exact status/stderr/stdout hashes on every invocation;
hashing and input construction are outside timing. Child user+system CPU and
wall samples are retained separately. No invalid subject was waived.

`--suite all --bytes 4194304` covers **108 distinct workloads**, including all
35 shell controls and 73 utility controls. The five historical shell controls
retain their old work counts; other throughput cases use 65,536 iterations and
filesystem/process cases use documented caps. The installed shell reference is
Bash 5.3.15. Figures below are median child CPU milliseconds from the same run.

| Shell case | c6ae before ms | Final ms | Speedup | Recovery B ms | Bash ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| parse | 9.219 | 9.060 | 1.02× | 7.898 | 50.682 |
| loop | 83.976 | 78.127 | 1.07× | 78.710 | 406.895 |
| function | 119.472 | 106.658 | 1.12× | 111.051 | 712.047 |
| arithmetic | 38.494 | 33.188 | 1.16× | 37.135 | 127.472 |
| scalar-short | 36.454 | 33.472 | 1.09× | 34.962 | 153.538 |
| scalar-long | 395.882 | 43.744 | 9.05× | 336.664 | 613.278 |
| glob | 61.697 | 21.103 | 2.92× | 59.814 | 41.461 |
| source | 12.932 | 9.633 | 1.34× | 12.978 | 9.742 |
| trap | 89.002 | 3.373 | 26.39× | 89.188 | 9.907 |
| alias | 131.465 | 130.860 | 1.00× | 132.272 | 49.212 |

The long quoted-field gain has an independent work explanation: final PMU
instructions fall 91.24% and user cycles fall 88.97%. The glob and numeric-kill
CPU gains also include eliminated kernel/process work; user-only PMU cycles
are not the total cost of their removed syscalls. The separate dense-glob
qualification improves 77.541→20.578ms (3.77×) and retains suffix/slash controls.

The earlier loop/function regression is recovered at the final medians. Simple
parsing remains **14.7% slower than recovery B**, even though it is slightly
faster than c6ae. Alias expansion remains **2.66× Bash's CPU time**. Only the
alias median is slower than Bash among these 35 cases; this is not an always-
faster guarantee across inputs or machine states.

All shell controls, including small/adverse ones:

| Case | Before ms | Final ms | Final/before | Median paired ratio | Faster rounds/11 |
| --- | ---: | ---: | ---: | ---: | ---: |
| parse | 9.219 | 9.060 | 0.9828 | 0.9849 | 10 |
| loop | 83.976 | 78.127 | 0.9303 | 0.9340 | 11 |
| parameter | 61.551 | 64.482 | 1.0476 | 1.0517 | 1 |
| function | 119.472 | 106.658 | 0.8927 | 0.8937 | 10 |
| redefine | 43.928 | 40.402 | 0.9197 | 0.9226 | 11 |
| startup | 0.124 | 0.119 | 0.9597 | 0.9683 | 6 |
| arithmetic | 38.494 | 33.188 | 0.8622 | 0.8628 | 11 |
| scalar-short | 36.454 | 33.472 | 0.9182 | 0.9175 | 11 |
| scalar-long | 395.882 | 43.744 | 0.1105 | 0.1107 | 11 |
| parameter-mix | 80.474 | 84.749 | 1.0531 | 1.0366 | 0 |
| fields | 43.019 | 42.469 | 0.9872 | 0.9865 | 11 |
| ifs-fields | 44.043 | 43.370 | 0.9847 | 0.9835 | 11 |
| positional | 102.730 | 101.839 | 0.9913 | 0.9948 | 7 |
| local-nameref | 84.466 | 84.370 | 0.9989 | 0.9949 | 10 |
| array-indexed | 43.117 | 39.057 | 0.9058 | 0.9026 | 11 |
| array-associative | 56.446 | 56.238 | 0.9963 | 0.9952 | 9 |
| builtin-format | 45.099 | 43.354 | 0.9613 | 0.9623 | 11 |
| case-pattern | 34.658 | 32.923 | 0.9499 | 0.9493 | 11 |
| regex-condition | 83.314 | 80.888 | 0.9709 | 0.9674 | 11 |
| glob | 61.697 | 21.103 | 0.3420 | 0.3420 | 11 |
| brace | 14.437 | 14.174 | 0.9818 | 0.9787 | 10 |
| read-lines | 139.102 | 136.714 | 0.9828 | 0.9853 | 11 |
| mapfile | 4.678 | 4.660 | 0.9962 | 1.0015 | 5 |
| command-substitution | 15.003 | 14.652 | 0.9766 | 0.9913 | 7 |
| pipeline | 26.188 | 26.237 | 1.0019 | 0.9970 | 7 |
| subshell | 9.484 | 9.277 | 0.9782 | 1.0125 | 2 |
| background-wait | 12.497 | 11.169 | 0.8937 | 1.0163 | 5 |
| redirect | 2.801 | 2.572 | 0.9182 | 0.9142 | 11 |
| source | 12.932 | 9.633 | 0.7449 | 0.7490 | 11 |
| trap | 89.002 | 3.373 | 0.0379 | 0.0384 | 11 |
| parse-arguments | 15.580 | 15.189 | 0.9749 | 0.9759 | 11 |
| parse-quotes | 44.975 | 42.140 | 0.9370 | 0.9383 | 11 |
| parse-grammar | 40.988 | 39.706 | 0.9687 | 0.9687 | 8 |
| parse-heredoc | 1.940 | 1.864 | 0.9608 | 0.9593 | 11 |
| alias | 131.465 | 130.860 | 0.9954 | 0.9876 | 8 |

Slower controls are retained, not described as noise and discarded. Parameter
CPU rises 4.76% (10/11 adverse pairs); parameter-mix rises 5.31% (11/11).
Among unchanged utilities, awk-fields rises 3.08% (11/11) and cut-suffix 3.58%
(10/11). Base32 and some codec median changes have much weaker paired agreement.
No aggregate score is used to hide these results. The full 108 rows, all raw
pairs and reference/binary identities are in
[all.json](../../artifacts/shell-hardware-2026-09-09/final/results/all.json) and
[the summary](../../artifacts/shell-hardware-2026-09-09/final/results/all-summary.json).

A separate five-round grouped user-PMU run records actual core cycles and
retired work, checking every output against Bash. All perf-stat records report
100.00% scheduled groups; no frequency-normalized TSC estimate is used.

| Case | Final/before user cycles | Final/before instructions |
| --- | ---: | ---: |
| parse | 0.9872 | 0.9784 |
| loop | 0.9325 | 0.9101 |
| function | 0.8946 | 0.9310 |
| scalar-short | 0.9130 | 0.8671 |
| scalar-long | 0.1103 | 0.0876 |
| parameter-mix | 0.8223 | 0.9666 |
| glob | 0.7831 | 0.9755 |
| trap | 0.1890 | 0.9147 |
| alias | 0.8548 | 0.9713 |

The PMU run and CPU-time matrix do not always agree on small or medium deltas:
parameter-mix is a clear example. They use separate execution windows and
launch/fixture contexts. Their raw results remain separate; neither is selected
to erase the other. Stable instruction/call deletions and large gains have
stronger support than a fine-grained ordering on this shared host.

## Interactive scope and controls

The terminal/editor probe was compiled and executed against both snapshots.
Both produce the **same binary SHA256**:
`f6d7c5e6d7d90f734f379e057e4e47f21cb21b062f540615828d87656305fdbb`.
All 84 batches validate exact cells, cursor/decoder state or document bytes,
and their pinned counter groups have equal enabled/running times. Six controls
cover ASCII/UTF8 scrolling, CSI overwrite, ASCII/UTF8 insertion/deletion and
CSI navigation. Setup/validation/output are outside the measured region.

The two sequential runs of this identical binary nevertheless differ by
roughly 1.4–2× in cycles, with essentially identical instruction counts. These
are **not optimization gains**: they demonstrate sensitivity to uncontrolled
machine/execution state. This pass establishes executable headless controls,
not reliable terminal hardware-floor gaps. Their sources, pins and raw batches
are in [terminal](../../artifacts/shell-hardware-2026-09-09/terminal/map.md) and
[final results](../../artifacts/shell-hardware-2026-09-09/final/results/).

## Hardware evidence and rejected assembly

The actual return histogram is critical: most hot whitespace spans inspect one
byte and return zero. Wider vector dispatch cannot erase that call or the
producer's redundant request. The new C composition leaves 5.7 million spans in
loop/function cases, down from 9.9 million. These counts pin the six-C composition
immediately before the separate glob fold; they are not a measured distribution
of user activity. Instrumented binaries are diagnostic only and never timed.

A one-byte-larger x86-64 `string_span` body makes the first rejection fall through
to return. Only 58 binary bytes change within its existing 80-byte slot;
4,064 other symbols retain their addresses/sizes and other section bytes match.
It passes 313,286 guard/oracle checks and 1,458 unmultiplexed PMU measurements.
Yet the zero-result control remains about 6 core cycles, equal to the empty-call
control in that overlapping-throughput experiment. Lengths 1 and 3 are flat;
length 7 costs approximately one extra cycle. The 35-case consumer screen slows
parameter-mix about 30%, grammar about 4% and alias expansion about 1%, with
all nine paired observations adverse. The candidate is rejected and library.c
remains byte-identical to `c6ae6d5`.

Two dependent L1 membership loads give a partial latency constraint for that
specific scalar decision. Speculation and independent calls can overlap them;
therefore neither that latency sum nor subtracting an empty-call cost gives a
universal cycles-per-call floor. The prior [hardware model](HARDWARE_FLOOR.md)
retains its explicit resource and residency assumptions.

## Correctness and reproduction

The final qualified stock passes:

- **60,169/60,169** checks across 21 legacy shell/utility suites.
- **1,547/1,547** focused exact cases: text, utilities, runtime, corrected quoted
  and assignment cases, arithmetic, kill, and 45 explicitly classified glob cases.
- **5,495/5,495** assertions in the repaired native shell, terminal and editor lanes.
- **684/684** generated lexer cases across command/file/stdin entry modes.
- **2,246/2,246** ARM64/RV64 lexer and shell smoke checks, with full shell builds
  and compiler/QEMU/ELF/binary identities retained. These are correctness tests.
- Library inventory/parity and coverage checks, 26 atlas unit tests and 80 browser
  assertions. No production library byte changed, so the prior foundational
  library qualification was not needlessly repeated.

The [joined validation report](../../artifacts/shell-hardware-2026-09-09/final/validation/report.md)
and [machine-readable result](../../artifacts/shell-hardware-2026-09-09/final/validation/final-validation.json)
retain each run and its disposition. These groups overlap and are not added
into one inflated total. The native shell
lane's standalone shell invocation omits 24 optional farm checks; those 24 pass
in the separately listed full-farm legacy suite. All initial failures remain
recorded. Only affected groups were rerun after test-driver/fixture repairs;
production source and the measured binary remained frozen throughout.

Source-pins manifest SHA256:
`68d07169bc0732278df2ed6179b21b5abd62a4ed51cb0f2257dd821c2653db12`.
Final test-pins manifest SHA256 after the two maintained test-runner repairs:
`71020421b7c8fb39615dee83e31f10a86abdb310460e3f3f7e0977b535d851b8`.
Earlier test manifests are retained with the runs that used them. The inventory
seal's production digest and the validation driver's canonical file-map digest
use different algorithms and are labeled separately.

The maintained shell lane now calls the real shell conformance suite after its
lexer and function tests; its previous general differential domain had no
specifications and always reported NOT RUN. The namespace-operand test compares
selected NS/TYPE identities while preserving utility status, rather than a live
number of duplicated per-process rows. Full original failures and namespace PID
evidence are retained. The artifact-only focused driver also had a Python
`glob.py` module-name collision; it was renamed and all previously blocked
cases were actually executed, not counted as passes from the failed import.

| Identity | SHA256 |
| --- | --- |
| c6ae baseline | `b2e2d78df400e71a325c6616962e3d4f121c1374f72aa21223d9e55a8d9e27f2` |
| Final stock | `a6bcf1c96376df5559872734367229e221826e1ec44d1b1ad51fe366a61dc210` |
| Final symbols | `0f95073901ececda521f27faaa6640036f00569db64714d6f51fe5791cdb2aac` |
| Unchanged library.c | `7f4527cf7a666be622611b1993510cc4cabbb6b2e767d63d9f33865aa8971f10` |

The frozen source/build manifests, scripts, rejected candidates, diagnostics,
all benchmark samples and exact results live under
[the retained artifact root](../../artifacts/shell-hardware-2026-09-09/).
The final stock reproduces the isolated accepted glob candidate byte-for-byte.
A normal Linux build is `sh kit/build programs/shell.c /tmp/moonwater-after`.
The maintained shell-only replay is:

```sh
python3 test/differential.py --harness engines \
  --suite shell --bytes 4194304 --rounds 11 --cpu 15 \
  --binary before=/path/to/c6ae6d5-shell --binary after=/tmp/moonwater-after \
  --time-reference --json shell-results.json
```

Use the retained final driver for the three-checkpoint 108-case run, exact
source/test pins, actual PMU counters and explicit reference policies. The source
map refresh removes 3 obsolete functions, remaps 304 IDs and downgrades 5 changed
bodies to family-based classification; it does not inflate complete-review claims.

## Scope and remaining gaps

The source inventory covers all 11 shell entry/language/interactive files:
44,329 baseline lines and 1,104 definitions. Each definition is mapped to a
semantic family and its known library callees. Detailed changed-path review,
exact tests, workload controls and headless interactive measurements have
separate evidence; inventory coverage is not complete body review or proof of
optimality. Multicall utilities have separate whole-tool controls.

The borrowed-token change still leaves unfinished-input inspection and final
lexing as separate phases. Alias replacement, repeated keyword classification
and deferred name materialization remain architectural leads. Removing them
requires measured experiments with continuation, heredoc, mutation and diagnostic
contracts. Parsing and aliases remain visible performance gaps, rather than
being hidden by an aggregate score.

Whole-shell hardware optimality is not established. Marker production/scanning,
retained argument copies and interpreter state transitions remain. Process and
file operations include OS work with different contracts from cache-resident
byte kernels. Physical key-to-photon, compositor, display, resize presentation,
PTY backpressure and kernel-console performance require a live device setup;
headless tests do not measure them. Native speed measurements here are x86-64
only; ARM64/RV64 emulation is correctness evidence.

| Shell surface | Before LOC | After LOC | Definitions before→after | Coverage |
| --- | ---: | ---: | ---: | --- |
| programs/shell.c | 697 | 697 | 6→6 | Entry/reader review; startup, script/stdin and process controls |
| src/sh/shell.c | 1,539 | 1,536 | 40→40 | Dispatch cache proof; all language/utility invocations |
| src/sh/lex.c | 1,406 | 1,299 | 28→25 | Counted spans, ownership/guards, generated transports |
| src/sh/parse.c | 2,710 | 2,710 | 68→68 | Grammar/storage review; parse/heredoc/function controls |
| src/sh/expand.c | 6,997 | 7,007 | 165→165 | Arithmetic, fields, parameters, patterns and glob qualifications |
| src/sh/exec.c | 10,301 | 10,307 | 245→245 | Assignments, dispatch, kill, processes, redirects and jobs |
| src/sh/builtin.c | 13,339 | 13,339 | 364→364 | 45-family runtime map; builtin/array/read/trap controls |
| src/sh/pty.c | 111 | 111 | 2→2 | Transaction review and maintained terminal tests; live I/O unmeasured |
| src/sh/term.c | 2,065 | 2,065 | 55→55 | Maintained terminal tests and exact headless PMU controls |
| src/sh/screen.c | 723 | 723 | 13→13 | Integration map and adapter tests; live presentation unmeasured |
| src/sh/edit.c | 4,441 | 4,441 | 118→118 | Maintained editor tests and exact headless PMU controls |
