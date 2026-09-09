# Diagnostic wrapper removal — 2026-09-08

This pass deletes `awk_out_of_memory` and **54 other C functions**, including
two generated functions, after the earlier diagnostic fold retained wrappers.
Four kernel logging macros and the now-unused `SHELL_ASSIGNER` generator are
also deleted. Every caller is migrated; a whole-production source scan finds
zero references to the deleted names, including declarations, directives and
comments. No replacement application wrapper functions or wrapper macros were
introduced.

Baseline: `3997cd8eb710fbc947e11baf4f370dde4b2cf229`.
Final production inventory digest:
`d8de689ff75b9ffc10a557d80fa7f306d628c06533fe6fdb4c16766fbbd6785a`.

## Shared implementation and preserved policies

`library.c` now owns `string_diagnostic`, `writer_stderr` and
`writer_stderr_once`, implemented on x86-64, ARM64 and RV64. The diagnostic
entry accepts a data descriptor selecting the writer, optional preflush and
address of the live command prefix. It calls the preflush once, reads the
prefix afterwards, and preserves the signed caller result while using the
existing `string_report` formatter. The text layer contains a constant
descriptor instead of `text_error`, `text_error_raw` and `text_refuse`.

The two stderr entries share length normalization. Zero length means a
terminated string; positive lengths are exact, including embedded NUL.
`writer_stderr` uses the existing positive-partial-write loop, which stops on
zero or negative writes and does **not** retry EINTR. `writer_stderr_once`
performs exactly one raw write, even for an empty string. Neither flushes
buffered output. This preserves the distinct text and shell-expansion
transport policies.

Callers retain their exit, cleanup and state transitions explicitly. AWK OOM
still flushes text and leaves with 2; runtime fatal errors first flush AWK's
other streams. Syntax failures preserve output bytes and status 1 while
formatting the line number directly. That last change alters writer chunking;
this pass does not claim identical syscall boundaries for every diagnostic.
Counted sort lines and shell variable names retain counted writes. Kernel
logging retains Linux writers, severity, per-call-site rate limits and prefixes.
The standalone utilities entry also replaces a stale `file_fail` call left
after its previous removal.

## Source and binary accounting

| Measurement | Before | After | Change |
| --- | ---: | ---: | ---: |
| Production physical lines | 191,006 | 190,557 | **−449** |
| Production nonblank lines | 163,440 | 163,068 | −372 |
| Production lexer tokens | 723,584 | 729,322 | **+5,738** |
| Production C entries | 3,895 | 3,840 | −55 |
| Library assembly routines | 252 | 255 | +3 |
| Normal x86-64 ELF bytes | 1,421,680 | 1,432,928 | **+11,248** |
| `.text` bytes | 1,218,965 | 1,229,829 | +10,864 |
| `.rodata` bytes | 184,152 | 184,576 | +424 |
| `.data` bytes | 13,528 | 13,480 | −48 |
| `.bss` bytes | 15,064,016 | 15,064,064 | +48 |

Removing the wrappers reduces definitions and physical lines but duplicates
explicit policy arguments and some flush/exit sequences at callers. This is
not a token-size or binary-size win, and no runtime speedup is claimed. The
normal and unstripped candidate builds have byte-identical `.text` sections.
The source totals include the new library code and inventory comments.

## Audit coverage and remaining operations

Three agents audited disjoint consumer scopes. The final inventory covers all
81 production source files, with 3,594 ordinary C bodies, 146 generated entries
and 100 C aliases. The independent integration scan enumerated every ordinary
body, selected 2,026 small bodies without depending on names, then rechecked
97 short diagnostic/error-related leads. The lexer sees brace-less statements;
assembly and generated entries are separately inventoried.

The scope ledgers give individual reasons for retained operations: 95 utility
candidates, 54 runtime candidates and 44 text/AWK candidates. These counts use
different selection rules and are not a combined coverage percentage. The
remaining functions perform allocation, parsing, descriptor operations,
cleanup, state changes or actual classification. Examples include
`awk_take`, overflow-checked `awk_size_add`, `exec_abort_line`, first-error
latches in `expr_stop`/`ls_limit`, and the distinct errno maps in `file_reason`
and `net_refused`. Public libc exports retain their errno/return-value ABI.
The shared `system_failed` range predicate and `system_error_message` table
perform actual error classification; they are not private report forwarders.

The removed-symbol list and all candidate bodies/dispositions are preserved
under [the local audit artifacts](../../artifacts/error-wrappers-2026-09-08/):
`removed-symbols.json`, `root-inventory.json`, `root-remainder.txt`, and the
`utilities/`, `runtime/` and `text/` reports. This establishes removal of the
reviewed thin diagnostic wrappers, not global algorithmic optimality.

The atlas removes all 55 obsolete C annotations, adds the three reviewed
assembly entries and remaps 2,329 locations. Twelve changed bodies are
conservatively returned to family classification: changed paths were reviewed,
but their complete surrounding bodies were not re-reviewed. Configuration
variants are matched by preserved lexical order. The refreshed map contains
4,193 production symbols and 1,575 support entries.

## Validation

- **742/742 exact baseline comparisons** of output and status: utilities 177,
  text/AWK 123, runtime 442. Cases include real allocation failure, flush
  ordering, runtime/syntax/usage failures, readonly assignments, job errors,
  binary/empty CMP inputs, DD summaries and output-file refusal.
- **60,090/60,090 maintained legacy candidate checks**: text 34,522; AWK 8,949;
  file/tools/util-linux/process/checksum/storage 11,662; shell/runtime 4,957.
  The utility baseline passed 11,661/11,662: its known live `lsns` membership
  comparison fluctuated. The candidate passed that case and all exact cases.
- **288,104,729/288,104,729 foundation assertions** across `standard`, `verify`,
  `error`, `leaving`, `writer`, `reuse_shell`, `audit`, `process`, `checksum`,
  `net` and `bowl`. x86-64 ran natively; ARM64/RV64 ran under QEMU. Includes
  kernel vector exclusion, no-platform and hardened-entry checks.
- Focused shared-report tests passed **9,744 assertions** across the three
  architectures, checking signed result extremes, optional/empty subjects,
  prefix mutation during preflush, callback counts and actual fd2 I/O.
  Independent assembly review checked stack alignment, saved arguments,
  indirect-call hardening and return conventions.
- **2,040 getopt cases**, allocator overflow/success probe, **1,057 hosted
  file sanitizer assertions**, 35 hosted sed cases and the maintained utility
  regression fixture pass.
- Main shell and standalone utilities builds pass. The actual kernel core,
  Canvas C and drawing assembly compile into `built-in.a` against the available
  Linux tree. An initial external `.ko` attempt reached modpost and failed
  because this integration is configured built-in (duplicate exports,
  unexported kernel internals and no module license). The correct built-in
  build passes, with unused-helper and existing-formatter objtool warnings.
  No live kernel/DRM run or complete kernel-image link was performed.
- Library inventory: **255 routines at full three-architecture parity**, zero
  C bodies/objects or replacement body macros. Performance and specialization
  manifests validate. Atlas tests: **26 Python tests and 80 browser checks**.
  `git diff --check` passes.

Candidate SHA-256:
`1a5f94b2e4cca5de90c17ccfcee24a6022a162278bf0a83c3203603bcfa350b4`.
Baseline SHA-256:
`41da3a4b071b33d6b4964003c3f75c174fda83d3f7bf9004c72f9f2276c66a40`.
Build, size and foundation logs are under `integration-results/`; the scope
reports retain commands, individual case results and source/binary hashes.
