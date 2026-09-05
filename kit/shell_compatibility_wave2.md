# Shell compatibility and folding pass 2 — 2026-09-05

Baseline: `31a0425`. Sealed production: `abf0424`. This is a bounded repair
pass, not full Bash/dash conformance or a hardware-floor proof. Tests ran over
SSH on the Ryzen 9950X Arch host against Bash 5.3.15 and dash 0.5.13.4, with
GCC 16.2.1 and default inherited signal dispositions for trap-sensitive lanes.

## Closed gaps and shared logic

- Noninteractive Bash `BASH_ENV`: direct filename lookup, tilde/parameter/
  arithmetic/command expansion without field splitting, globbing or quote
  removal; spaces and literal quote bytes survive. Startup runs before opening
  the named script, so startup `cd` affects a relative script path. Positional
  parameters and `$0` are already available. Functions, EXIT traps, early exit,
  errexit, source return boundaries and incoming substitution status are tested.
- Bash one-command mode (`-t`, `set -o onecmd`): stops after the first complete
  input program, including blank/comment lines; it does not truncate `-c`.
- Bash physical mode (`-P`, `set -o physical`): shared `cd`/`pwd` honor explicit
  logical/physical overrides, CDPATH, readonly PWD/OLDPWD and unnamed directory
  behavior. Ordinary path joining stays on the existing tuned `path_join`;
  only its ambiguous maximum-length result uses checked joining.
- `PIPESTATUS[index]=value` follows Bash's selected-index behavior. Bare `set`
  reuses the existing array declaration serializer for indexed, associative,
  sparse and empty arrays. Lazy status bookkeeping remains intact.
- `interactive_comments` now changes interactive Bash parsing, while scripts
  and dash identities retain comments. Delimiter discovery shares the lexer
  scanner with command/backtick expansion. Fresh comments hide closing bytes
  in command substitutions; escaped newlines preserve lexical freshness.
- Unquoted heredoc continuations are joined before delimiter recognition.
  Quoted and tab-stripping heredocs remain distinct. Backtick expansion in
  heredocs uses the same quote-preserving expansion adapter as startup paths.
- A sourced file can be the first parsed input: nested-parser initialization
  now preserves the reserved zero node instead of silently dropping commands.

Startup policy was checked against the [GNU Bash startup documentation](https://www.gnu.org/software/bash/manual/html_node/Bash-Startup-Files.html)
and executable reference cases. No interactive/login profile support is
implied. Mismatched real/effective UID or GID suppress both startup-name
expansion and file execution; six child-only reference/candidate checks
verified UID, GID and combined mismatches. This is not an implementation or
validation of the full privileged-mode credential policy.

The document adapter emits literal runs directly into the existing token
arena. It initializes expansion scratch only for actual expansions; large
heredocs do not acquire another body-sized expansion/mark buffer. Basename
discovery is shared with multicall dispatch. No utility copies, alternative
parser, allocator, or new assembly implementation were introduced. The existing
assembly scans, bounded copy, environment lookup and path primitives are reused;
the library still contains 242 assembly routines.

## Performance evidence

These are bounded user-instruction measurements, not general shell speedups.
The host also runs unrelated workloads; elapsed-time claims are intentionally
limited. Control and candidate use the same compiler/flags within each probe.

### Literal entry

Nine CPU-0-pinned `perf stat -e instructions:u` samples per `-c :` invocation,
using equal-length temporary directory names and LTO-built binaries:

| Invocation name | Baseline median | Candidate median |
| --- | ---: | ---: |
| sh | 343 | 365 |
| bash | 348 | 716 |
| dash | 3536 | 406 |
| moonwater | 3531 | 431 |

The two large reductions remove an unnecessary utility-table search. Bash's
additional 368 instructions check the supplied environment for BASH_ENV before
the literal fast exit; startup functions/traps make skipping that check wrong.
The default sh increase is 22 instructions. None of these percentages is an
end-to-end launch-time measurement; kernel work is outside `instructions:u`.

### Shared document expansion

`kit/bench_shell_document.c` calls the existing here-body entry directly,
checks every resulting length and terminal byte, and prints an identical
checksum. Both revisions were built with the freestanding O2 flags from
`src/test/run`, using the same new probe. Seven CPU-0-pinned samples:

| Body / repetitions | Baseline median | Candidate median |
| --- | ---: | ---: |
| 16-byte literal / 20000 | 3610547 | 3430546 |
| 16-byte dollar suffix / 20000 | 24530921 | 24350919 |
| 65536-byte literal / 1000 | 248975470 | 248966467 |
| 65536-byte dollar suffix / 1000 | 250023906 | 250014901 |

The short literal path uses about 5% fewer instructions; the large-body path
is effectively unchanged. This probe isolates expansion, not parse/fork costs.

### Other controlled paths

- Isolated lexer/parser changes, 250000 ordinary 48-byte lines, CPU 2, nine
  samples: 1961860583 to 1964610585 instructions (+0.1402%), with identical
  472594247 branches. Cycles/task-clock showed no measured regression; this is
  not a broad throughput guarantee.
- 1000 `cd sub; pwd; cd ..; pwd` cycles: baseline logical 14562265 instructions;
  candidate logical 14649859 (+0.60% for policy/bounds checks). Candidate
  physical mode used 13747328, 6.16% below logical mode by avoiding logical
  directory validation. Physical mode changes semantics and is not forced on.
- Isolated variable changes, 200000 Bash commands without reading PIPESTATUS:
  862029623 to 862029913 instructions (+290 total). Default scalar `set`
  serialization cost +0.26%. The selected-index workload used fewer instructions
  but its previous answers were wrong, so it is not a same-semantics speedup.

## Validation and image

The final sealed tree passed 4691/4691 checks:

- Shell language/compatibility/pipeline/variables/startup/paths/lexer/expansion/
  builtin lanes: 3747/3747. New focused startup 56/56, paths 35/35, lexer 27/27;
  variable coverage grew to 47/47.
- Build/document kit: 61/61, including source and assembly inventory checks.
- Standard, storage_io, terminal and probe: 883/883. Storage I/O compiled and
  ran on native x86-64 and emulated AArch64/RV64; RV64 used the explicit
  IMAFD+Zicsr+Zicntr floor. Additional cross-built shell path smokes passed, but
  there is no claim of complete ARM/RV64 Bash/dash reference parity.

The SIGPIPE-sensitive pipeline fixture was corrected: a producer racing a
failed reader can legitimately exit successfully or receive SIGPIPE under
both shells. Redirect-state tests now use a non-writing producer; the readonly
test's producer explicitly survives PIPE and ignores its write failure. No
production status was changed to satisfy a scheduling accident.

Build #438 passed boot 48/48, including Spark-loaded BASH_ENV and one-command
mode, and KVM Canvas/terminal input-and-pixel checks 25/25.
Artifact: `dist/bootx64-shell-pass2.efi`, 9331712 bytes, SHA-256
`502525045ad388d6ade8001731a7adefbe8492b96009c506092ae739db4c7d09`.
Earlier images were preserved. The image grew by 4096 bytes from pass 1;
Spark's reported BSS remains 13611008 bytes. BSS is virtual capacity, not a
measurement of idle resident RAM.

Audit seal: 3782 production C functions, source SHA-256
`ed79d1ef7199b2ca665916002ed175ab54b889e55200a93d8f221faafbe810f7`.

## Remaining gaps

Four of the original nine feature-map gaps closed: physical mode, one-command
mode, interactive comments and BASH_ENV. Five original rows still report GAP:
history expansion (`-H`), keyword assignments (`-k`), POSIX mode, privileged
mode and exact readonly diagnostic wording.

The map now also includes four previously unenumerated nameref-array cases
(element read/write/unset and compound assignment), plus one shared parser gap:
a `)` in a heredoc body inside `$(...)` can terminate the substitution early.
That last case fails against dash under all three default names. Fixing it
requires sharing the parser's heredoc grammar, not another partial delimiter
parser. It must not be hidden by the otherwise-green existing POSIX rows.

Further known limits include nameref readonly-target status, huge ulimit
number behavior, interactive/login startup files, complete terminal/job-control
policy, and exhaustive option interactions. Syntax nesting is defensively
bounded at the existing 64-level expansion ceiling. The informative map is
not a conformance gate, and these lists are not exhaustive.

Reproduce the hard lanes with `sh src/test/run shell shell_compat shell_pipeline
shell_variables shell_startup shell_paths shell_lex expand builtin`; use
`sh kit/shell_gaps.sh /path/to/shell` for the explicitly unsupported map.
