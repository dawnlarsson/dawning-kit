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
symlinks, scripts and command-hash fixtures, and bounds each shell invocation
to five seconds. The final native x86-64 result was 126/126:

| Group | Result | Covered surface |
| --- | ---: | --- |
| startup | 53/53 | `-ec`, `-ce`, `-eu -c`, `-o pipefail`, `-n`, `-nc`, `-s`, `--`, missing `-c`, unknown flags, `+e`, command names and operands |
| mode | 10/10 | Bash-only identity, non-Bash identity through all three names, same-line noexec, functional `B` brace and `h` command-hash toggles |
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
specific diagnostic. No successful case suppresses or normalizes stderr.

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

The `BASH_ENV` row points at a generated temporary file. Privileged behavior
is only reported as absent; the suite does not change credentials or exercise
unsafe host policy. Interactive profile/login-file loading, full job-control
terminal behavior, exhaustive option interactions, other Bash startup files,
ARM reference parity and unbounded grammar fuzzing remain outside this pass.

## Reproduction

Build the shell, then run:

```sh
sh src/test/shell_compat.sh /path/to/moonwater-shell
sh kit/shell_gaps.sh /path/to/moonwater-shell
```

The first command must exit nonzero for any hard compatibility regression.
The second prints each intentionally unsupported row as `GAP` and exits after
reporting the complete bounded map.
