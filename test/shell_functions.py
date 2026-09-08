#!/usr/bin/env python3
"""Check persistent function storage using the compiled shell and GNU Bash.

Run: python3 test/shell_functions.py --shell /path/to/shell [--out report.json]
Capacity-error cases use explicit bounded-shell expectations; the remaining
cases compare stdout, stderr and status with Bash. No source is extracted.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--shell', required=True)
parser.add_argument('--out')
parser.add_argument('--runner', default='')
parser.add_argument('--observe', action='store_true',
                    help='record a baseline without requiring it to pass')
args = parser.parse_args()

cases = {
    'alternating': '''i=0; while test "$i" -lt 10000; do
eval 'a(){ :; }; b(){ :; }' || break
i=$((i+1)); done; printf '%s\\n' "$i"; a; b''',
    'varying_sizes': '''i=0; while test "$i" -lt 2000; do
case $((i%3)) in
0) eval 'a(){ : A A A A; }; b(){ : B; }; c(){ : C C; }';;
1) eval 'a(){ : A; }; c(){ : C; }; b(){ : B B B B; }';;
2) eval 'b(){ : B B; }; a(){ : A A; }; c(){ : C C C C; }';;
esac
test "$?" = 0 || break
a; b; c; i=$((i+1)); done; printf '%s\\n' "$i"''',
    'active_redefinition': '''install(){ f(){ f(){ new=$((new+1)); }; old=$((old+1)); }; }
i=0; old=0; new=0
while test "$i" -lt 2000; do install && f && f || break; i=$((i+1)); done
printf '%s:%s:%s\\n' "$i" "$old" "$new"''',
    'active_unset': '''install(){ f(){ unset -f f; tail=$((tail+1)); }; }
i=0; tail=0
while test "$i" -lt 2000; do install && f || break; i=$((i+1)); done
printf '%s:%s\\n' "$i" "$tail"; command -v f; :''',
    'recursive_versions': '''install(){ f(){
if test "$1" -gt 0; then f "$(( $1-1 ))"; else
f(){ newer=$((newer+1)); }
fi
older=$((older+1))
}; }
i=0; older=0; newer=0
while test "$i" -lt 1000; do install && f 12 && f || break; i=$((i+1)); done
printf '%s:%s:%s\\n' "$i" "$older" "$newer"''',
    'nested_kept_heredoc': '''outer(){ inner(){ cat <<'BODY'
held $literal text
BODY
}; }
outer; unset -f outer
i=0; while test "$i" -lt 1200; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"; inner
text=$(declare -f inner); unset -f inner; eval "$text"; inner''',
    'return_trap': '''set -T
trap 'f(){ result=new; }' RETURN
f(){ result=old; }; f
trap - RETURN
printf '%s\\n' "$result"; f; printf '%s\\n' "$result"
i=0; while test "$i" -lt 1000; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"''',
    'alias_definition_time': '''shopt -s expand_aliases
alias saved='printf "defined\\n"'
eval 'f(){ saved; }'
unalias saved
i=0; while test "$i" -lt 1000; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"; f
text=$(declare -f f); unset -f f; eval "$text"; f''',
    'compound_word_reuse': '''i=0
while test "$i" -lt 1000; do
eval 'a(){ local x=(one two); test "${x[1]}" = two; }; b(){ : "x=(wrong)"; }' || break
a || break
unset -f a
eval 'a(){ local x="(one two)"; test "$x" = "(one two)"; }' || break
a || break
i=$((i+1)); done
printf '%s\\n' "$i"''',
    'frontier_reclaimed': '''i=0; while test "$i" -lt 1000; do
eval 'a(){ :; }; b(){ :; }; c(){ :; }' || break
unset -f b a c
i=$((i+1)); done
printf '%s\\n' "$i"
eval '''+shlex.quote('; '.join(': x' for _ in range(180)))+'''
printf 'parsed:%s\\n' "$?"''',
    'exported_redefinition': '''i=0; f(){ :; }; export -f f
while test "$i" -lt 1000; do
eval 'a(){ :; }; f(){ printf "exported\\n"; }; b(){ :; }' || break
i=$((i+1)); done
printf '%s\\n' "$i"; /bin/bash -c f''',
}
# An inactive large definition can contribute its own capacity to replacement.
large = 'x' * 6200
cases['large_same_size_replacement'] = (
    'i=0; while test "$i" -lt 100; do eval '
    + shlex.quote('a(){ : ' + large + '; }')
    + ' || break; i=$((i+1)); done; printf "%s\\n" "$i"; a')

# Reservation can fail after earlier arenas were reserved. Measurement can
# also reject the new body before reservation; both must preserve the old one.
cases['failed_copy_preserves_definition'] = (
    'a(){ printf "old\\n"; }; b(){ : ' + 'x' * 4000 + '; }\n'
    + 'eval ' + shlex.quote('a(){ : ' + 'y' * 5000 + '; }')
    + ' 2>/dev/null\nprintf "reject:%s\\n" "$?"; a; b')
cases['failed_measure_preserves_definition'] = (
    'a(){ printf "old\\n"; }\n'
    + 'eval ' + shlex.quote('a(){ : ' + 'z' * 8192 + '; }')
    + ' 2>/dev/null\nprintf "reject:%s\\n" "$?"; a')
special_expected = {
    'failed_copy_preserves_definition': (0, 'reject:1\nold\n', ''),
    'failed_measure_preserves_definition': (0, 'reject:1\nold\n', ''),
}

results = []
for name, script in cases.items():
    if args.runner:
        command = shlex.split(args.runner) + ['-0', 'bash', args.shell, '-c', script]
        executable = None
    else:
        command = ['bash', '-c', script]
        executable = args.shell
    run = subprocess.run(command, executable=executable, capture_output=True,
                         text=True, timeout=60)
    if name in special_expected:
        wanted = special_expected[name]
    else:
        reference = subprocess.run(['/bin/bash', '-c', script], capture_output=True,
                                   text=True, timeout=60)
        wanted = (reference.returncode, reference.stdout, reference.stderr)
    got = (run.returncode, run.stdout, run.stderr)
    results.append(dict(name=name, script=script, wanted=wanted, got=got,
                        passed=got == wanted))
    print(name, 'PASS' if got == wanted else 'FAIL', repr(run.stdout), flush=True)

if args.out:
    report = dict(binary_sha256=hashlib.sha256(pathlib.Path(args.shell).read_bytes()).hexdigest(),
                  shell=args.shell, runner=args.runner, cases=results)
    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
passed = sum(row['passed'] for row in results)
print(f'function-storage {passed} of {len(results)}')
if os.environ.get('TEST_TALLY'):
    with open(os.environ['TEST_TALLY'], 'a') as tally:
        tally.write(f'function-storage {passed} {len(results)}\n')
if not args.observe:
    assert passed == len(results)
