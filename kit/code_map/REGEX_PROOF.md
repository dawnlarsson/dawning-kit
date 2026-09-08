# Counted regex graph: implementation and qualification

The qualified implementation replaces the old regex bytecode compiler and recursive
runner with one immutable expression graph and an iterative matcher. The
complete production patch removes **762 physical lines and 479 nonblank lines**.
Raw C source tokens including preprocessor bodies fall by only **4**: source-token
size is effectively unchanged. This is a reduction in repeated mechanisms and
line count, not a claim that the program contains substantially fewer tokens.

The old engine is removed. Counted groups keep one child plus bounds; they no
longer copy or relocate instructions. Saved programs reference their own prepared
metadata. AWK restores one pool mark, and shell conditionals compile above any
live transient program instead of copying compiler internals and metadata.
Grep borrows prepared hints directly and requests captures only when needed.
Prepared character sets use the same 256-byte representation as library spans.
No library or architecture assembly implementation changes.

## Complete source accounting

The baseline is commit `8f65aa64e07e21560bc7b347085199fe16bd8bf5`, source digest
`10d043364f0f3b40e34e09848621e1588ee11faa8b2e16209c37e32c2584a029`.
Counts include every line of the three changed existing production files and
all of `src/sh/regex_graph.c`; caller changes, declarations, macros and adapters
are charged. The moved literal-search helper receives no deletion credit.

| Production file | Old physical lines | New physical lines |
| --- | ---: | ---: |
|`src/sh/text.c`|19,759|18,059|
|`src/sh/exec.c`|10,566|10,536|
|`src/sh/awk.c`|6,190|6,181|
|`src/sh/regex_graph.c`|0|977|
|**Total**|**36,515**|**35,753**|

The final source digest is
`e7f3e866b9f2126aeec48d9e0ce4b69c939107d7a9c93dce696033e71c038e55`.
The whole production inventory falls from 193,288 to 192,526 physical lines.
Regression tests, documentation, atlas maintenance and proof tooling are counted
separately; they are not production savings. No statements were packed to meet
the line target. Ordinary lexer tokens increase 108 while directive tokens
fall 112; the combined result is 4 fewer.

## Behavior and limits

The implemented grammar retains BRE/ERE policy differences, ASCII case folding,
captures and backreferences, empty branches, nullable repetitions, virtual WORD
boundaries, exact-position searches and first-DFS capture priority at equal
endpoints. Exhaustion can remain pending after a successful match; exact callers
observe it at the existing policy boundary.

Independent review caught and fixed three representation costs that a normal
corpus missed:

- Two nodes per simple repeat initially halved capacity. The pool now has 8,192
  nodes; the old accepted 4,091-repeat boundary is covered explicitly.
- Repeated identity intervals now remain no-ops after the first simple repeat,
  as they were in the old compiler. Ten thousand intervals are tested.
- Mandatory finite repeats of an erased empty group remain empty. This prevents
  exponential work for `(){0}` followed by repeated `{2}` intervals.

Capacity increases are intentional: grouped `{256}` and 34 alternatives are now
accepted. The front-end regression test still requires loud refusal when 4,200
single-byte alternatives exceed the graph pool while fitting the pattern buffer.
Matching keeps 20,000 frames, choices and capture undo entries, plus a 100 million
work budget. These are bounded policies, not byte-for-byte reproductions of the
old recursion ceilings. Lowering all three arrays to 16,000 was rejected because
it truncated a clean old match with nine repeated captures.

A separate old defect is fixed: a literal shortcut after `(b){0}` used to expose
capture slots left by a previous pattern. Requested absent captures are cleared.
The production-floor regression reproduces the old stale slot and verifies the
new unset value. Direct failed compilation leaves the published descriptor and
pool mark intact; legacy transient recompilation still borrows its designated
unretained pool region.

## Verification

Final evidence lives under `artifacts/regex-proof-2026-09-08/`. It includes exact
source/binary hashes, versioned build logs, paired samples, generated corpora,
sanitizer attestations, and independent review. Earlier failed prototypes and
counterexamples remain distinguishable from the qualified version. The first
cross-architecture harness incorrectly compared identical applet-launch errors;
that evidence is excluded. The corrected `cross-final3-qualified.json` requires
explicit expected statuses, nonempty successful output and correct diagnostics
before equality can count as a pass. Its 84 cases pass. Deep compiler probes
bypass grep's separate pattern-length ceiling and pass five checks on each of
the three actual binaries (QEMU for ARM64/RISC-V).

| Check | Final result |
| --- | ---: |
| Text consumers | 34,505 / 34,505 |
| AWK | 8,949 / 8,949 |
| Shell | 1,776 / 1,776 |
| File utilities and generated path cases | 1,669 / 1,669 + 672 / 672 |
| Shared production-floor regression binary | 69,588 checks, zero failures |
| x64, ARM64 and RISC-V consumer comparisons | 84 / 84 |
| Primary direct oracle cases, optimized and sanitizers | 25,461 / 25,463 exact; two intended capacity increases |
| Nested graph cases, each build | 16,200 / 16,200 |
| Empty-alternative cases, each build | 4,860 / 4,860 |
| Ownership / pending exhaustion, each build | 419 / 419 and 18 / 18 |

All three architecture builds succeed without C diagnostics. The linker emits
only its usual serial-LTO scheduling note. Nine file-utility fault-injection
checks could not run because strace/ptrace was unavailable. Hosted capacity
coverage includes 100 cases and explicit identity/empty-chain counterexamples;
one sanitizer run timed out during a heavily concurrent launch, then passed in
0.059 seconds alone. Both observations are retained, rather than erasing the
first result. The earlier 6,509,160-case work/rollback comparison qualifies the
matcher optimization separately; it is not counted as final whole-engine coverage.

The final multicall binaries have these SHA-256 hashes:

| Architecture | SHA-256 |
| --- | --- |
| x64 | `5a86aea27fe6f327c6e0f2ae619b15ad91b9d8e0d0653e9897fdb977c04116b0` |
| ARM64 | `af71cbef148e07f6cd7c96f2045c3803923aae9f1e47ce704b7d5d7ae54edc13` |
| RISC-V | `5e431e7c191e27ce51e8dbcade9ce9a3e04644959e078fde087831935c9b1c0b` |

The hosted oracle harness uses common C adapters for library primitives; its
elapsed time is not a production performance measurement. Actual freestanding
consumer checks and benchmarks exercise the real assembly library. Two primary
oracle differences are the documented grouped-count capacity increases. The
source/token gate applies to the complete production patch; standalone test
fixtures and generated copies of the old engine are outside that patch.

## Performance and storage

Eleven paired native x64 rounds alternate binary order after exact output,
status and diagnostic qualification. Ratios below are candidate/baseline
median wall time; below 1 is faster. Inputs and raw samples are retained.

| Workload | Ratio |
| --- | ---: |
| literal/rare | 1.014 |
| literal/dense | 1.005 |
| literal/icase | 1.033 |
| required/prefilter | 1.038 |
| class/anchored | 0.874 |
| byte/repeat | 0.563 |
| group/fixed | 1.303 |
| group/ambiguous-longest | 0.383 |
| group/nullable | 0.414 |
| capture/backref | 0.892 |
| word/longest | 0.845 |
| sed/captures | 0.864 |
| sed/counted | 1.073 |
| awk/gsub | 0.972 |
| nl/retained | 0.980 |

The fixed-group workload is about 30% slower; counted sed substitutions are
about 7% slower. Other regex-heavy workloads improve in this sample. Tiny
literal and prefilter timings vary by a few percent and are not presented as
material speedups. This sample does not prove general optimality.

| Architecture | Binary text change | BSS change |
| --- | ---: | ---: |
| x64 | +864 bytes | +1,416,288 bytes |
| ARM64 | +1,408 bytes | +1,416,288 bytes |
| RISC-V | +928 bytes | +1,416,280 bytes |

The graph pool reserves 162,760 bytes and the three matcher arrays reserve
1,440,000 bytes. Net BSS grows about 1.35 MiB after the old state is removed.

The standalone microbenchmark uses five paired process runs, each reporting
the best of five CPU-time trials. Candidate/baseline ratios:

| Pattern | Compile | Match |
| --- | ---: | ---: |
| `needle` | 2.649 | 1.192 |
| `^[a-z]+[0-9]+$` | 0.952 | 0.819 |
| `a+` | 1.869 | 0.032 |
| `(ab){32}` | 0.149 | 1.163 |
| `(a\|aa)*` | 1.130 | 0.260 |
| `(a?)*b` | 1.331 | 0.372 |
| `(ab\|a)\1` | 1.587 | 0.855 |

A separate native workload with 2,000 lines of 1,000 bytes and nine repeated
captures produced identical 2,002,000-byte output. Sampling the executed
program's own `/proc/PID/status` high-water RSS gave 14,124 KiB for the baseline
and 14,988 KiB for the candidate. These are sampled workload values, not global
memory bounds; the BSS and array reservations above are exact.

Compilation and matching are measured separately in a proof-only freestanding
benchmark using the existing `kit/bench_measure.c`. Repeated grouped patterns
compile faster because they no longer expand their child. Simple patterns can
compile slower because graph construction and prepared metadata have fixed
costs. A fixed grouped match requires count continuations where the old program
executed copied linear instructions; that overhead is an explicit tradeoff.

The iterative matcher reserves more BSS and avoids input-dependent C recursion.
This does not establish universally lower memory use or optimal performance.
Exact per-architecture stack-frame sizes were not measured. Deep compile cases
are checked on the actual binaries; ARM64 and RISC-V runtime checks use QEMU,
so native performance on those architectures remains unmeasured.

The refreshed atlas classifies all 25 graph functions, removes 44 obsolete
entries and verifies 4,245 production symbols. The inventory seal, 26 map unit
tests and 80 desktop/mobile browser checks pass.

## Reproduce the maintained regressions

Build `programs/shell.c` with `sh kit/build`, create the normal multicall name farm,
and run `test/text.sh`, `test/awk.sh`, `test/files.sh`, and `test/shell.sh` against it.
Build and run `test/reuse_shell.c` for retained metadata, failed publication,
repeated-count capacity, erased groups and capture-slot reuse checks. Use
`CC=aarch64-linux-gnu-gcc` or `CC=riscv64-linux-gnu-gcc` for the corresponding builds.
`python3 kit/function_audit.py --check` verifies the refreshed inventory seal;
`python3 kit/code_map/build.py` rebuilds the classified map.
