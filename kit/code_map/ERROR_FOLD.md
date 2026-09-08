# Shared diagnostic reporting

The error-handling pass removes **1,401 production lines and 830 C lexer
tokens** against `e8155be6b2b0d07ed12486d2ab5e0abb57f26fca`. The resulting source
digest is `d1d85a493f8394abf7d80b5516495e6579613ef42e6598c7dbb774d4c12e80f2`.

`string_report(writer, result, format, ...)` adds one entry to each architecture
in `library.c`. It shares `string_format`'s parser and conversion bodies, then
returns the supplied signed 32-bit result. The selected writer continues to
own flushing, write retries and transport. There are 427 production call sites.

This folds repeated diagnostic/return pairs in shell startup, builtins,
execution, file tools, process tools, util-linux and checksum paths. The
`file_fail`, `shell_diagnostic` and `exec_error` aliases disappear. Text guards
reuse the false result from their existing reporting boundary; AWK terminating
paths declare their control-flow contract and cold reporting stays out of line.
The physical-line reduction includes braces made unnecessary when two
statements become one.

| Production source | Before | After | Change |
| --- | ---: | ---: | ---: |
| Physical lines | 192,407 | 191,006 | −1,401 |
| Nonblank lines | 164,821 | 163,440 | −1,381 |
| C lexer tokens | 724,414 | 723,584 | −830 |

The scope totals include the 27 lines added to `library.c`: file/util-linux
consumers remove 796 lines, shell/runtime/startup remove 487, and AWK/text/tools
remove 145. Maintained tests, atlas annotations and this report are separate
from production counts. No production C function entry was removed; the three
removed aliases were preprocessor names.

The normal native Linux x86-64 LTO build uses the same compiler and build flags
for both versions. Its ELF shrinks **5,696 bytes**. `.text` shrinks 5,632 bytes,
`.rodata` shrinks 80 bytes, `.data` is unchanged, and layout increases `.bss` by
64 bytes. The final ELF SHA-256 is
`41da3a4b071b33d6b4964003c3f75c174fda83d3f7bf9004c72f9f2276c66a40`.
The AWK OOM helper shrinks from 169 to 23 instruction bytes; `text_error` from
247 to 67, and `dd_complain` from 484 to 141.

Deleting every short wrapper would increase other costs. Expanding
`ul_bad_usage` and `file_missing` at their 295 physical call expressions saves
eight lines but adds 1,715 tokens, 7,922 source bytes and 3,136 binary bytes.
Both now use `string_report`, while retaining the compact shared message shape.

Other retained boundaries carry observable behavior: text diagnostics flush
text output and retry stderr writes; platform `log_error` flushes its own log
and performs its existing write policy; expansion diagnostics do not flush that
log. AWK OOM and fatal errors have different output ordering. Shell failure
helpers carry status, cleanup and special-builtin rules. Bowl uses stdout and
launch cleanup, and kernel/Canvas logging carries severity, rate limiting and
kernel execution constraints. Raw syscall error-window checks, errno/TLS
translation and the distinct `perror` implementations retain their contracts.

Validation on the final combined binary includes 622 exact before/after
diagnostic, output-order and exit-status comparisons. The shared formatter tests
cover result extremes, unknown/trailing percent behavior, writer call counts,
GP/FP argument spills and recursive writers on x86-64, ARM64 and RISC-V.
The full verify/error/leaving/writer run passes 287,453,846 assertions; standard,
kernel vector-state, architecture parity and hardened-entry checks pass 194.
The integrated reuse, builtin, process, checksum, net, Bowl and function-storage
checks pass 650,203 assertions. Foreign architecture runs use QEMU for
correctness, with no claim of native performance.

The new differential runner is not a complete green suite: six domain specs
are absent, so those lanes generate zero cases and correctly report NOT RUN.
Its text domain has 180 failures across ten GNU-difference classes. All 1,305
cases have identical baseline/current status, output hashes, effects, timeout
state and difference class. These failures predate the reporting fold.
Eight migrated diagnostics also retain pre-existing unsupported `%c` formats;
no unsupported format was introduced by this pass.

The existing domain suites were also run directly. Text passes 34,522 checks
on both binaries, and file/tools/util-linux/process/checksum/storage pass 11,662
on each. The final binary passes 8,949 AWK checks and 4,985 shell/runtime checks
(including 28 process cases overlapping the utility coverage). Initial missing
farm fixtures were corrected and affected suites rerun. One concurrent live
namespace-listing mismatch is retained in the evidence; twenty live and twenty
isolated repetitions per binary, then serial full util-linux runs, pass.

A native paired formatter smoke used nine alternating runs with a consuming
writer. Median candidate/baseline ratios were 0.954 for empty, 1.012 for literal,
0.999 for integer and 1.006 for mixed formats. No material regression appeared
in this bounded test; small timing differences are not demonstrated gains.

The inventory seal, performance/specialization manifests and source map are
refreshed. Atlas validation passes 26 parser/accounting tests and 80 browser
checks. Twenty changed C entries conservatively retain only family-level review
because the complete surrounding bodies were not re-reviewed during this pass.

Local reproducible scripts, exact cases, raw timings, compiler sizes and reports
are in [the pass artifacts](../../artifacts/error-fold-2026-09-08/), including
[source accounting](../../artifacts/error-fold-2026-09-08/accounting.json),
[utility evidence](../../artifacts/error-fold-2026-09-08/utilities/report.md),
[text and ABI evidence](../../artifacts/error-fold-2026-09-08/text/report.md), and
[runtime review](../../artifacts/error-fold-2026-09-08/runtime/report.md).
The separate integration reports record the
[utility](../../artifacts/error-fold-2026-09-08/utilities/integration/report.md),
[text](../../artifacts/error-fold-2026-09-08/text/integration/report.md) and
[shell](../../artifacts/error-fold-2026-09-08/runtime/integration-legacy/report.md)
qualification, including original failures and reruns.
