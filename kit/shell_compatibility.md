# Shell compatibility checkpoint — 2026-09-05

## Scope and reference

This bounded pass started at source baseline `cf59680`. It tests one Moonwater
binary through four names: `bash` selects Bash policy, while `moonwater`, `sh`
and `dash` select the default dash-compatible policy. It is a compatibility
checkpoint for the cases below, not a claim of complete Bash or POSIX shell
conformance.

The exact differential was run natively on the Ryzen 9950X Arch host against
Bash 5.3.15-1 and dash 0.5.13.4-1. The candidate was built with GCC 16.2.1.
Every catchable inherited signal disposition was reset to default before the
test launcher execed `/bin/sh`; plain SSH otherwise carries ignored signals
into trap-related shell tests.

## Hard compatibility suite

`src/test/shell_compat.sh` is the failing gate. It creates only temporary
symlinks, scripts, command-hash fixtures and one controlled pseudo-terminal,
and bounds every shell invocation. The final native x86-64 result was 147/147:

| Group | Result | Covered surface |
| --- | ---: | --- |
| startup | 67/67 | `-ec`, `-ce`, `-eu -c`, `-o pipefail`, `-n`, `-nc`, `-s`/`+s`, `--`, missing `-c`, unknown and mode-specific flags, `+e`, `-i`/`+m`, PTY foreground ownership, command names and operands |
| mode | 17/17 | Bash-only identity, non-Bash identity through all three names, same-line noexec, non-Bash option rejection, functional `B` brace and `h` command-hash toggles, and cached `$-` invalidation |
| pipeline | 11/11 | default isolation, `lastpipe` state, `PIPESTATUS` publication and reset, pipefail, negation, redirect failures and readonly final-stage status |
| readonly | 9/9 | dynamic local lifetime, refusal status and restoration of the outer value in Bash and dash policies |
| expansion | 25/25 | mixed/nonblank IFS edges, quoted `$@` splicing, `$*`, assignment context, newline trimming and associative subscripts |
| parse | 18/18 | nested heredocs, continued and-or/pipeline input, escaped case patterns, tested errexit contexts and substitution status |

The functional option claim in this hard suite is intentionally narrow:
disabling and re-enabling `B` changes brace expansion, and disabling `h`
bypasses an explicitly seeded command hash before re-enabling it. Passing
those cases does not imply that the full Bash option inventory exists.

For ordinary cases stdout, stderr and exit status are byte-for-byte exact.
Three startup-error families have deliberately explicit diagnostic
normalization: nounset must mention the variable, missing `-c` must mention
`-c`, and an unknown `-Z` must mention `-Z`; stdout and status remain exact.
This avoids treating executable paths, line-number decoration and Bash's
multi-line usage appendix as shell-language behavior while still requiring a
specific diagnostic. Forced `-i` cases run without a terminal compare exact
stdout and status while ignoring reference job-control warnings. The positive
`-i -m` case instead uses a controlled PTY, requires the foreground child's
process group to own it, and removes only the PTY's carriage returns before an
exact Bash/candidate comparison. Other successful cases do not suppress or
normalize stderr.

The repeated `$-` path was checked separately because reconstructing ordered
flags can sit inside script loops. On CPU 2, over seven perf-stat samples of
300,000 expansions, the `cf59680` control measured 2484279747 instructions,
202.73 ms task-clock and 111 faults; the final candidate measured 2249980951,
192.97 ms and 107 faults. This rules out an instruction regression for that
controlled path. The comparison contains the whole bounded shell change set,
so it does not attribute the reduction to one function or claim a general
shell throughput improvement.

The variable/PIPESTATUS lane independently passed 37/37 focused checks, and
the combined shell-related lanes passed 3613/3613. Its 100,000-write,
nine-run comparison measured one active readonly mark at 908836195 versus
894436004 instructions (+1.61%), and 64 active marks at 908637043 versus
1499224193 (-39.4%); the no-mark path was +0.61%. These are bounded integrated
comparisons against `cf59680`, not universal assignment costs. Lazy
`PIPESTATUS` publication measured +0.153% on the simple loop and +0.139% on
the variable loop when comparing Bash and sh policy in the same binary. The
discarded eager and scalar-intermediate designs had measured +57.5% and
+36.3% respectively.

## Gap map

`kit/shell_gaps.sh` remains an informative map rather than a passing gate. Its
Bash rows now invoke a `bash`-named link, and every POSIX row checks the same
binary independently as `moonwater`, `sh` and `dash`. Controlled startup data
is used; the caller's login files are never read or changed.

All bounded POSIX rows currently match dash under all three names. This only
describes those enumerated rows and is not evidence of full POSIX conformance.
The map deliberately retains these Bash gaps:

- `set -H` history-expansion option state (interactive expansion itself is
  not claimed by this row);
- `set -k` keyword-assignment behavior;
- `set -P` physical-directory option state;
- `set -o posix` option state and policy;
- privileged `set -p` option state (without changing credentials);
- startup `-t`/one-command behavior;
- functional `interactive_comments` behavior after the shopt is disabled;
- noninteractive `BASH_ENV` startup-file loading; and
- Bash's exact `declare -r` diagnostic wording (the exit status now matches).

That list is the map's current inventory, not an exhaustive list of every
known Bash difference. Adjacent bounded gaps remain for assignment-specific
`PIPESTATUS[4]=9` key publication, bare `set` serialization of arrays (the
current scalar form differs from Bash array notation), and huge `ulimit`
numbers, which are rejected rather than wrapped to Bash's unlimited value.

The `BASH_ENV` row points at a generated temporary file. Privileged behavior
is only reported as absent; the suite does not change credentials or exercise
unsafe host policy. Interactive profile/login-file loading, full job-control
terminal behavior, exhaustive option interactions, other Bash startup files,
ARM reference parity and unbounded grammar fuzzing remain outside this pass.
The current exact `-c :`/`true`/`false` entry remains ahead of environment and
personality initialization because those literals cannot observe either. Any
future `BASH_ENV` implementation must revisit that ordering: a startup file
can install functions or traps that make even a literal command observable.

## Reproduction

Build the shell, then run:

```sh
sh src/test/shell_compat.sh /path/to/moonwater-shell
sh kit/shell_gaps.sh /path/to/moonwater-shell
```

The first command must exit nonzero for any hard compatibility regression.
The second prints each intentionally unsupported row as `GAP` and exits after
reporting the complete bounded map.
