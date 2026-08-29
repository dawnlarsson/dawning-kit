#!/bin/sh
#
#       The shell: what POSIX asks of it, what it answers to, and what it
#       does not have.
#
#           sh src/test/shell.sh [shell] [directory of the names it answers to]
#
#       Every case in the first section is a fragment from the shell command
#       language in POSIX XCU section 2. Each is run through dash, which is
#       taken as the reference because it is the smallest thing that is
#       actually correct, and through the shell being tested; agreeing with
#       dash is passing.
#
#       That is deliberately a harder bar than "does not crash": a shell that
#       prints nothing agrees with nothing, and a shell that prints something
#       almost right fails loudly instead of being counted.
#
#       Four kinds of case, because four things are being asked:
#
#         check     the two shells write the same bytes to standard output.
#         answer    the same bytes and the same exit status. The status a
#                   script leaves behind is what every caller of it reads, and
#                   for a long time nothing here looked at it.
#         differs   ours and dash disagree, and this is exactly what ours says
#                   today. Written down rather than left out, so that closing
#                   the gap fails here and says so.
#         absent    a name the shell has no answer for, and the status it
#                   gives instead.
#
set -e

subject=${1:-/tmp/mwsh}
names=${2:-}
reference=${3:-/bin/dash}

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }

# Agreeing with dash is the whole of what passing means here, so without one
# there is nothing to compare against and saying so is better than inventing
# an answer.
[ -x "$reference" ] || { echo "  shell        no $reference, skipped"; exit 0; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0
current=""
section_name=""
section_pass=0
section_total=0

section()
{
        [ -z "$section_name" ] ||
                printf '  %-12s %s of %s\n' \
                        "$section_name" "$section_pass" "$section_total"

        [ -z "$section_name" ] || [ -z "${TEST_TALLY:-}" ] ||
                printf '%s %s %s\n' \
                        "$section_name" "$section_pass" "$section_total" \
                        >> "$TEST_TALLY"

        section_name=$1
        section_pass=0
        section_total=0
}

group() { current=$1; }

won()
{
        pass=$((pass + 1))
        section_pass=$((section_pass + 1))
        section_total=$((section_total + 1))
}

lost()
{
        fail=$((fail + 1))
        section_total=$((section_total + 1))
        printf '  %-14s %-26s %s\n' "$current" "$1" "$2"
}

# A case is a name and a fragment. The fragment is fed to both shells on
# standard input, which is how a script arrives, and the two outputs compared.
#       Set for the cases that ask what a shell does with a signal that was
#       already ignored before it started. The outer sh is the same one on
#       both sides and does nothing but establish that and hand over, so the
#       only difference the case can measure is what the shell under test
#       makes of what it was handed.
entry_ignored=""

run_shell()
{
        # Bounded, because a shell under test is exactly the kind of thing
        # that loops forever, and a suite that hangs tells you nothing about
        # which case did it.
        if [ -n "$entry_ignored" ]; then
                timeout 5 sh -c 'trap "" INT QUIT; exec "$0"' "$1" \
                        < "$work/case.sh" > "$2" 2>/dev/null
        else
                timeout 5 "$1" < "$work/case.sh" > "$2" 2>/dev/null
        fi
}

run_both()
{
        printf '%s\n' "$*" > "$work/case.sh"

        if run_shell "$reference" "$work/want"; then
                want_status=0
        else
                want_status=$?
        fi

        if run_shell "$subject" "$work/got"; then
                got_status=0
        else
                got_status=$?
        fi
}

shown() { head -c 40 "$1" | tr '\n' '|'; }

check()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got"; then
                won
                return 0
        fi

        lost "$name" "want $(shown "$work/want")   got $(shown "$work/got")"
}

answer()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

bash_answer()
{
        name=$1
        shift

        [ -x /bin/bash ] || {
                lost "$name" "/bin/bash is required for a Bash extension case"
                return 0
        }

        held_reference=$reference
        reference=/bin/bash
        run_both "$@"
        reference=$held_reference

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "bash $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

# One row in the explicit Bash parity ledger. It records the unsupported
# answer exactly and also requires Bash to answer differently; implementing a
# row therefore fails with an instruction to move it into bash_answer.
bash_remaining()
{
        name=$1
        recorded=$2
        recorded_status=$3
        shift 3

        [ -x /bin/bash ] || {
                lost "$name" "/bin/bash is required for the Bash parity ledger"
                return 0
        }

        held_reference=$reference
        reference=/bin/bash
        run_both "$@"
        reference=$held_reference
        got_ours=$(shown "$work/got")

        if [ "$got_ours" != "$recorded" ] ||
                [ "$got_status" != "$recorded_status" ]; then
                lost "$name" \
                        "remaining ${recorded}[$recorded_status], now ${got_ours}[$got_status]"
                return 0
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                lost "$name" "agrees with Bash now -- move it to supported"
                return 0
        fi

        won
}

# A deliberate extension has no POSIX reference answer. Check its bytes and
# status directly so a future lexer change cannot reinterpret it as another
# valid command while the dash-comparison lanes remain about POSIX.
expected()
{
        name=$1
        expected_output=$2
        expected_status=$3
        shift 3

        printf '%s\n' "$*" > "$work/case.sh"

        if run_shell "$subject" "$work/got"; then
                got_status=0
        else
                got_status=$?
        fi

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$expected_output" ] &&
                [ "$got_status" = "$expected_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "expected ${expected_output}[$expected_status], got ${got_ours}[$got_status]"
}

# Expansion errors are different at a terminal: they end the current input
# line but leave the shell there to read the next one. A pipe cannot exercise
# that branch, so give the subject a real pseudo-terminal with echo disabled
# and verify the diagnostic, status, recovery, and absence of both commands
# that followed the failure on its line.
interactive_fatal()
{
        name=$1
        diagnostic=$2
        setup=$3
        command=$4
        behavior=${5:-abort}
        recovery=${6:-:}
        recovered=${7:-}

        if python3 - "$subject" "$diagnostic" "$setup" "$command" \
                "$behavior" "$recovery" "$recovered" <<'PY'
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time

subject, diagnostic, setup, command, behavior, recovery, recovered = sys.argv[1:]
master, slave = pty.openpty()
settings = termios.tcgetattr(slave)
settings[3] &= ~termios.ECHO
termios.tcsetattr(slave, termios.TCSANOW, settings)

process = subprocess.Popen(
    [subject], stdin=slave, stdout=slave, stderr=slave, close_fds=True
)
os.close(slave)

script = "\n".join(
    (
        "echo START",
        setup,
        command,
        "echo STATUS:$?",
        recovery,
        "echo NEXT",
        "exit",
        "",
    )
).encode()
sent = 0

while sent < len(script):
    sent += os.write(master, script[sent:])

data = bytearray()
deadline = time.monotonic() + 5

while time.monotonic() < deadline:
    ready, _, _ = select.select([master], [], [], 0.1)

    if ready:
        try:
            data.extend(os.read(master, 4096))
        except OSError:
            break

    if process.poll() is not None:
        break

if process.poll() is None:
    process.kill()

process.wait()
os.close(master)

plain = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", bytes(data)).replace(
    b"\r", b""
)
good = (
    b"START\n" in plain
    and diagnostic.encode() in plain
    and b"NEXT\n" in plain
    and b"RAN-BAD" not in plain
)

if behavior == "abort":
    good = good and b"STATUS:2\n" in plain and b"SAME-LINE" not in plain
else:
    good = good and b"STATUS:0\n" in plain and b"SAME-LINE\n" in plain

if recovered:
    good = good and recovered.encode() + b"\n" in plain

if not good:
    sys.stderr.buffer.write(plain)
    raise SystemExit(1)
PY
        then
                won
                return 0
        fi

        lost "$name" "interactive failure did not abort and recover"
}

# A signal caught during the expansion that fails belongs after that aborted
# line and before the next command. It must not be lost, run as the failed
# command's tail, or wait until after the recovery command.
interactive_fatal_trap()
{
        if python3 - "$subject" <<'PY'
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time

master, slave = pty.openpty()
settings = termios.tcgetattr(slave)
settings[3] &= ~termios.ECHO
termios.tcsetattr(slave, termios.TCSANOW, settings)
process = subprocess.Popen(
    [sys.argv[1]], stdin=slave, stdout=slave, stderr=slave, close_fds=True
)
os.close(slave)

script = b"""trap 'echo CAUGHT' USR1
unset x
echo "$(kill -USR1 $$)" "${x:?boom}"; echo SAME-LINE
echo STATUS:$?
echo NEXT
exit
"""
os.write(master, script)
data = bytearray()
deadline = time.monotonic() + 5

while time.monotonic() < deadline:
    ready, _, _ = select.select([master], [], [], 0.1)
    if ready:
        try:
            data.extend(os.read(master, 4096))
        except OSError:
            break
    if process.poll() is not None:
        break

if process.poll() is None:
    process.kill()
process.wait()
os.close(master)

plain = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", bytes(data)).replace(
    b"\r", b""
)
caught = plain.find(b"CAUGHT\n")
status = plain.find(b"STATUS:2\n")
good = (
    b"x: boom" in plain
    and caught >= 0
    and status > caught
    and b"NEXT\n" in plain
    and b"SAME-LINE" not in plain
)

if not good:
    sys.stderr.buffer.write(plain)
    raise SystemExit(1)
PY
        then
                won
        else
                lost 'interactive fatal trap order' 'expected CAUGHT before STATUS:2'
        fi
}

# Every descriptor below the limit except stdin/out/err is available here.
# A literal source at the highest usable slot used to force hidden saves above
# the limit even though lower slots were free.
low_fd_literal_sources()
{
        for limit in 7 8 9 10 11 12 13 14 15 16
        do
                fd=$((limit - 1))
                file=$work/low-fd-$limit
                got=$(printf '%s\n' \
                        "ulimit -n $limit; exec $fd>$file; echo literal >&$fd; echo STATUS:\$?; exec $fd>&-; cat $file; rm $file" |
                        "$subject" 2>&1)

                if [ "$got" != 'STATUS:0
literal' ]
                then
                        lost 'low fd literal sources' \
                                "limit $limit fd $fd expected STATUS:0/literal, got $got"
                        return
                fi
        done

        won
}

# What ours says where dash says something else. The recorded output has its
# newlines written as | so a case stays one line. Both halves are checked: if
# ours starts agreeing with dash the case fails and moves up into answer, and
# if ours changes to a third thing it fails too.
differs()
{
        name=$1
        recorded=$2
        recorded_status=$3
        shift 3
        run_both "$@"

        got_ours=$(shown "$work/got")

        if [ "$got_ours" != "$recorded" ] || [ "$got_status" != "$recorded_status" ]; then
                lost "$name" "recorded ${recorded}[$recorded_status]   now ${got_ours}[$got_status]"
                return 0
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                lost "$name" "agrees with dash now -- move it into answer"
                return 0
        fi

        won
}

# A name with nothing behind it. PATH is emptied first, so what is being
# asked is whether the shell has the thing itself rather than whether this
# machine happens to have one.
absent()
{
        name=$1
        want=$2
        shift 2
        printf 'PATH=\n%s\n' "$*" > "$work/case.sh"

        timeout 5 "$subject" < "$work/case.sh" > "$work/got" 2>/dev/null || true

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$want" ]; then
                won
                return 0
        fi

        lost "$name" "expected $want, got $got_ours"
}

# A script named on argv is a different entry path from the standard-input
# cases below. Run an actual file through both interpreters so ignoring argv
# cannot look like a successful empty script, and keep each part of the
# process contract separate enough that a failure names what was lost.
entry_input=""

run_script_entry()
{
        interpreter=$1
        output=$2
        shift 2

        if [ -n "$entry_input" ]; then
                printf '%s' "$entry_input" |
                        timeout 5 "$interpreter" "$work/entry.sh" "$@" \
                                > "$output" 2>/dev/null
        else
                timeout 5 "$interpreter" "$work/entry.sh" "$@" \
                        > "$output" 2>/dev/null
        fi
}

script_answer()
{
        name=$1
        source=$2
        shift 2

        printf '%s\n' "$source" > "$work/entry.sh"

        if run_script_entry "$reference" "$work/want" "$@"; then
                want_status=0
        else
                want_status=$?
        fi

        if run_script_entry "$subject" "$work/got" "$@"; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

# A no-operand shell consumes descriptor zero and reports `s` in `$-`. Keep a
# literal pipe entry alongside the general redirected-fragment runner: this is
# the way system launchers and init jobs normally supply generated source.
piped_entry_answer()
{
        name=$1
        source=$2

        if printf '%s\n' "$source" | timeout 5 "$reference" \
                > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi

        if printf '%s\n' "$source" | timeout 5 "$subject" \
                > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

section entry

# -c is the entry path everything that is not a person uses: system(), the
# shell a Makefile runs a recipe with, find -exec sh -c, xargs sh -c. It is a
# third path through the argument handling, distinct from a named script and
# from standard input, and it used to read "-c" as a file name.
run_command_entry()
{
        interpreter=$1
        output=$2
        shift 2

        timeout 5 "$interpreter" -c "$@" > "$output" 2>/dev/null
}

command_answer()
{
        name=$1
        shift

        if run_command_entry "$reference" "$work/want" "$@"; then
                want_status=0
        else
                want_status=$?
        fi

        if run_command_entry "$subject" "$work/got" "$@"; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

# The process environment is part of the entry contract and needs launch-time
# control, which a fragment fed on stdin cannot provide.
environment_command_answer()
{
        name=$1
        command=$2
        shift 2

        if timeout 5 env "$@" "$reference" -c "$command" > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi

        if timeout 5 env "$@" "$subject" -c "$command" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

environment_command_expected()
{
        name=$1
        expected_output=$2
        expected_status=$3
        command=$4
        shift 4

        printf '%s' "$expected_output" > "$work/want"

        if timeout 5 env "$@" "$subject" -c "$command" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$expected_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$expected_status]   got $(shown "$work/got")[$got_status]"
}

environment_many_answer()
{
        name=$1
        amount=$2
        command=$3
        interpreter=$reference

        for output in want got; do
                set -- -i
                at=0

                while [ "$at" -lt "$amount" ]; do
                        set -- "$@" "MW_MANY_$at=value"
                        at=$((at + 1))
                done

                if timeout 5 env "$@" "$interpreter" -c "$command" \
                        > "$work/$output" 2>/dev/null; then
                        status=0
                else
                        status=$?
                fi

                if [ "$output" = want ]; then
                        want_status=$status
                        interpreter=$subject
                else
                        got_status=$status
                fi
        done

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

group script
script_answer 'script starts with a comment' '# only a comment
echo after'
script_answer 'script starts with a shebang' '#!/bin/sh
echo after'
script_answer 'script name'  'printf "%s\n" "$0"'
script_answer 'script count' 'printf "%s\n" "$#"' one two
script_answer 'script first' 'printf "%s\n" "$1"' alpha beta
script_answer 'script status' 'exit 37'
script_answer 'script flags' 'printf "%s\n" "$-"'
piped_entry_answer 'piped stdin flags' 'printf "%s\n" "$-"'
script_answer 'script stdin option' 'set -o | while read name state; do [ "$name" = stdin ] && echo "$state"; done'
entry_input='payload
'
script_answer 'script keeps stdin' 'IFS= read -r value; printf "%s\n" "$value"'
entry_input=""

# The kernel only recognizes a script directly when it starts with #!, but a
# shell is required to recover from ENOEXEC and interpret executable text
# itself. Cover direct and PATH lookup, arguments, status, and the exec builtin
# so none of the three launch routes quietly regresses to the kernel rule.
answer 'executable text without shebang' 'p=/tmp/no-shebang.$$; printf '\''printf "plain:%s:%s\\n" "$0" "$1"\nexit 23\n'\'' > "$p"; chmod +x "$p"; "$p" one; s=$?; rm -f "$p"; echo "$s"'
answer 'PATH text without shebang' 'd=/tmp/no-shebang-path.$$; mkdir "$d"; printf '\''printf "path:%s:%s\\n" "${0##*/}" "$1"\n'\'' > "$d/plain-script"; chmod +x "$d/plain-script"; PATH=$d:/bin plain-script two; s=$?; rm -f "$d/plain-script"; rmdir "$d"; echo "$s"'
answer 'exec text without shebang' 'p=/tmp/no-shebang-exec.$$; printf '\''printf "exec:%s:%s\\n" "${0##*/}" "$1"\nexit 19\n'\'' > "$p"; chmod +x "$p"; exec "$p" three'

# The monitor is the shell's real integration workload: a ten-kilobyte script
# beginning with a shebang, then nested functions, locals, substitutions,
# pipelines and external tools. A fragment suite did not catch the parser's
# zero-token first-line crash, so one complete frame is permanent coverage.
if [ "$(uname -s)" = Linux ]; then
        if timeout 20 "$subject" programs/monitor.sh 0.05 1 \
                > "$work/monitor.out" 2> "$work/monitor.err"
        then
                monitor_status=0
        else
                monitor_status=$?
        fi

        if [ "$monitor_status" = 0 ] && [ -s "$work/monitor.out" ] &&
                [ ! -s "$work/monitor.err" ]; then
                won
        else
                lost 'monitor one frame' \
                        "status $monitor_status, stdout $(wc -c < "$work/monitor.out"), stderr $(wc -c < "$work/monitor.err")"
        fi

        # stty must query the input terminal, not its piped stdout. Give the
        # subject a deliberately non-default PTY so a hard-coded 24x80 answer
        # cannot pass.
        pty_size=$(script -qec "stty rows 37 cols 111; $subject -c 'stty size'" \
                /dev/null 2>/dev/null | tr -d '\r')
        if [ "$pty_size" = "37 111" ]; then
                won
        else
                lost 'stty size from pty' "expected 37 111, got $(printf '%s' "$pty_size")"
        fi
fi

group command
command_answer 'command plain'    'echo hello'
command_answer 'command status'   'exit 7'
command_answer 'command name'     'printf "%s\n" "$0"' myname
command_answer 'command count'    'printf "%s\n" "$#"' myname one two
command_answer 'command first'    'printf "%s\n" "$1"' myname alpha beta
# With no name operand $0 is unspecified and every shell fills it with its own
# path, so the two can only agree that there is one.
command_answer 'command default name is set' '[ -n "$0" ] && echo yes || echo no'
command_answer 'command pipeline' 'echo abc | rev'
command_answer 'command arithmetic' 'x=5; printf "%s\n" "$((x * 3))"'
command_answer 'command several lines' 'echo one
echo two
echo three'
command_answer 'command empty'    ''
command_answer 'command trailing newline' 'echo one
'

group environment
environment_command_answer 'custom environment' \
        'printf "%s|%s|%s\n" "$MW_CUSTOM" "$HOME" "$PATH"' \
        MW_CUSTOM=from-parent HOME=/inherited-home PATH=/inherited-path
environment_command_answer 'replace inherited' \
        'MW_CUSTOM=replaced; printf "%s\n" "$MW_CUSTOM"' \
        MW_CUSTOM=from-parent
environment_command_answer 'inherited replacement stays exported' \
        'MW_CUSTOM=replaced; /bin/sh -c '\''printf "%s\n" "$MW_CUSTOM"'\''' \
        MW_CUSTOM=from-parent
environment_command_answer 'unset then assignment is local' \
        'unset MW_CUSTOM; MW_CUSTOM=local; /bin/sh -c '\''printf "%s\n" "${MW_CUSTOM-unset}"'\''' \
        MW_CUSTOM=from-parent
environment_command_answer 'export inherited replacement' \
        'export MW_CUSTOM=replaced; /bin/sh -c '\''printf "%s\n" "$MW_CUSTOM"'\''' \
        MW_CUSTOM=from-parent
environment_command_answer 'unset inherited' \
        'unset MW_CUSTOM; /bin/sh -c '\''printf "%s\n" "${MW_CUSTOM-unset}"'\''' \
        MW_CUSTOM=from-parent
environment_command_answer 'inherited becomes readonly' \
        'readonly MW_CUSTOM; MW_CUSTOM=replaced; echo after' \
        MW_CUSTOM=from-parent
environment_command_answer 'cannot unset inherited readonly' \
        'readonly MW_CUSTOM; unset MW_CUSTOM; echo after' \
        MW_CUSTOM=from-parent
environment_command_answer 'cannot export over inherited readonly' \
        'readonly MW_CUSTOM; export MW_CUSTOM=replaced; echo after' \
        MW_CUSTOM=from-parent
environment_command_expected 'empty environment defaults' \
        'path|shell|1|pwd|unset
' 0 \
        'printf "%s|%s|%s|%s|%s\n" "${PATH:+path}" "${SHELL:+shell}" "$OPTIND" "${PWD:+pwd}" "${HOME-unset}"' \
        -i
environment_many_answer 'six hundred inherited variables' 600 \
        'set | grep '\''^MW_MANY_[0-9][0-9]*='\'' | wc -l'

# A literal word far past what the lexer and parser used to hold -- eight
# kilobytes of token text, sixteen of parsed text -- so this is the line
# length itself being tested rather than an expansion that happens to be long.
# It used to be truncated silently and then, once the reader grew, refused.
long_word=$(awk 'BEGIN { s = ""; for (i = 0; i < 5000; i++) s = s "0123456789"; print s }')
command_answer 'a literal word of fifty thousand' "printf '%s\n' $long_word | wc -c"

# Variable values use the same unbounded stable storage as the line.  These
# cross both ceilings that used to exist: 1023 bytes while expanding and 8192
# bytes for the entire environment.
middling_value=$(awk 'BEGIN { s = ""; for (i = 0; i < 2000; i++) s = s "x"; print s }')
command_answer 'a two thousand byte variable' \
        "x=$middling_value; printf '%s\n' \"\${#x}\""
command_answer 'a fifty thousand byte variable' \
        "x=$long_word; printf '%s\n' \"\${#x}\""
command_answer 'a long exported value reaches a child' \
        "x=$long_word; export x; sh -c 'printf \"%s\\n\" \"\${#x}\"'"
command_answer 'a long value survives unset beside it' \
        "before=$middling_value; gone=$long_word; after=$middling_value; unset gone; printf '%s %s %s\n' \"\${#before}\" \"\${gone-unset}\" \"\${#after}\""
command_answer 'a long value survives a subshell' \
        "x=$long_word; (printf '%s ' \"\${#x}\"); printf '%s\n' \"\${#x}\""
command_answer 'eval sees a long value' \
        "x=$long_word; eval 'printf \"%s\\n\" \"\${#x}\"'"
command_answer 'dot sees a long value' \
        "x=$long_word; f=\$(mktemp); echo 'printf \"%s\\n\" \"\${#x}\"' >\"\$f\"; . \"\$f\"; rm -f \"\$f\""
command_answer 'local restores a long value' \
        "x=$long_word; f() { local x=short; printf '%s ' \"\$x\"; }; f; printf '%s\n' \"\${#x}\""

many_variables=$(awk 'BEGIN { for (i = 0; i < 200; i++) { printf "export V%03d=", i; for (j = 0; j < 80; j++) printf "%d", i % 10; printf ";" } }')
command_answer 'two hundred exported variables' \
        "$many_variables set | grep '^V[0-9][0-9][0-9]=' | wc -l"
command_answer 'unset in a large environment is stable' \
        "$many_variables unset V100; printf '%s %s %s\n' \"\${#V099}\" \"\${V100-unset}\" \"\${#V101}\""
command_answer 'many command assignments survive and restore' \
        'A=1 B=2 C=3 D=4 E=5 F=6 G=7 H=8 /bin/sh -c '\''printf "%s\n" "$A$B$C$D$E$F$G$H"'\''; printf "[%s][%s]\n" "${A-unset}" "${H-unset}"'

section posix
group quoting
check 'single'          "echo 'a b'"
check 'double'          'echo "a b"'
check 'backslash'       'echo a\ b'
check 'quoted dollar'   "echo '\$x'"
check 'escaped quote'   'echo "a\"b"'
check 'empty'           'echo ""'
check 'adjacent'        'echo a"b"c'
# A substitution inside double quotes carries quotes of its own, and reading
# one of them as the close ended the word at the first blank inside it.
check 'sub in quotes'   'echo "$(echo "a b")"'
check 'sub then more'   'echo "pre $(echo "x  y") post"'
check 'backtick quotes' 'echo "`echo "a b"`"'

group parameters
check 'plain'           'x=1; echo $x'
check 'braced'          'x=1; echo ${x}'
check 'default'         'echo ${u-def}'
check 'default colon'   'echo ${u:-def}'
check 'assign'          'echo ${u:=def}$u'
check 'alternate'       'x=1; echo ${x:+yes}'
check 'length'          'x=abcd; echo ${#x}'
check 'suffix'          'x=a.b.c; echo ${x%.*}'
check 'suffix greedy'   'x=a.b.c; echo ${x%%.*}'
check 'prefix'          'x=a.b.c; echo ${x#*.}'
check 'prefix greedy'   'x=a.b.c; echo ${x##*.}'
check 'unset'           'echo "[$nosuch]"'
check 'positional'      'set -- a b c; echo $2'
check 'count'           'set -- a b c; echo $#'
check 'all'             'set -- a b; echo $@'
check 'status'          'true; echo $?'
check 'status false'    'false; echo $?'
check 'status 255'      '(exit 255); echo $?'
check 'stdin flag'      'printf "%s\n" "$-"'
check 'stdin option'    'set -o | while read name state; do [ "$name" = stdin ] && echo "$state"; done'
answer 'flags follow set' 'set -euxC; before=$-; set +exC; after=$-; printf "%s:%s\n" "$before" "$after"'
answer 'named flags follow set' 'set -o noglob; set -o ignoreeof; set -o stdin; before=$-; set +o noglob; set +o ignoreeof; set +o stdin; printf "%s:%s\n" "$before" "$-"'
answer 'set rejects unknown lower option' 'set -q; echo after'

group expansion
check 'command sub'     'echo $(echo hi)'
check 'backtick'        'echo `echo hi`'
check 'nested sub'      'echo $(echo $(echo deep))'
check 'arithmetic'      'echo $((2 + 3))'
check 'arith vars'      'x=4; echo $((x * 2))'
check 'arith compare'   'echo $((3 > 2))'
check 'field split'     'x="a b c"; set -- $x; echo $#'
check 'glob'            'cd /; echo /de*'
check 'tilde'           'HOME=/tmp; echo ~'

group lines
check 'continuation'    'echo a\
b'
check 'quote across'    'echo "one
two"'
check 'single across'   "echo 'one
two'"
check 'sub across'      'v=$(echo a
echo b)
echo "$v"'
check 'backtick across' 'echo `
echo tick
`'
check 'arith across'    'echo $((
1 + 2
))'
check 'heredoc in sub'  'v=$(cat <<EOF
body
EOF
); echo "$v"'
check 'construct in sub' 'v=$(
if true
then
echo yes
fi
)
echo $v'
check 'quote in a body' 'f() {
  echo "$1"
}
f "two
lines"'

group control
check 'if true'         'if true; then echo yes; fi'
check 'if else'         'if false; then echo a; else echo b; fi'
check 'elif'            'if false; then echo a; elif true; then echo b; fi'
check 'while'           'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
check 'until'           'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
check 'for'             'for i in a b c; do echo $i; done'
check 'for positional'  'set -- x y; for i; do echo $i; done'
check 'case'            'case abc in a*) echo match;; *) echo no;; esac'
check 'break'           'for i in 1 2 3; do [ $i = 2 ] && break; echo $i; done'
check 'continue'        'for i in 1 2 3; do [ $i = 2 ] && continue; echo $i; done'
check 'subshell'        '(echo inside); echo outside'
check 'group'           '{ echo a; echo b; }'

group operators
check 'and'             'true && echo yes'
check 'or'              'false || echo yes'
check 'not'             'if ! false; then echo yes; fi'
check 'sequence'        'echo a; echo b'
check 'pipe'            'echo hello | cat'
check 'pipe chain'      'echo a | cat | cat'
# A pipeline cut short is not a shorter pipeline: the stage that becomes the
# last one writes where the next was going to read, so twenty stages used to
# answer with the sixteenth stage's work.
check 'twenty stages'   'echo a | sed s/a/b/ | sed s/b/c/ | sed s/c/d/ | sed s/d/e/ | sed s/e/f/ | sed s/f/g/ | sed s/g/h/ | sed s/h/i/ | sed s/i/j/ | sed s/j/k/ | sed s/k/l/ | sed s/l/m/ | sed s/m/n/ | sed s/n/o/ | sed s/o/p/ | sed s/p/q/ | sed s/q/r/ | sed s/r/s/ | cat'

group functions
check 'define call'     'f() { echo body; }; f'
check 'args'            'f() { echo $1; }; f arg'
check 'return'          'f() { return 3; }; f; echo $?'
bash_answer 'function keyword' 'function f { echo body; }; f'
bash_answer 'function parens' 'function f() { echo "$1"; }; f arg'
bash_answer 'function newline' 'function f
{
echo body
}
f'
bash_answer 'function nested' 'function outer { function inner { echo "$1"; }; inner nested; }; outer'
bash_answer 'quoted function is command' '"function" f { echo body; }'
bash_answer 'function missing name' 'function'
bash_answer 'function missing body' 'function f'
answer 'one hundred twenty eight functions' 'i=0; while [ "$i" -lt 128 ]; do eval "f$i() { echo $i; }"; i=$((i+1)); done; f0; f64; f127; f64() { echo new; }; f64; unset -f f65; type f65 >/dev/null 2>&1; echo $?'
answer 'function registry churn' 'i=0; while [ "$i" -lt 1000 ]; do eval "f$i() { :; }"; eval "unset -f f$i"; i=$((i+1)); done; final() { echo final; }; final'
answer 'long function name' 'function_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz() { echo long; }; function_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz; unset -f function_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz; type function_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz >/dev/null 2>&1; echo $?'
answer 'one thousand function calls with locals' 'n=outer; f() { local n=$1; if [ "$n" -eq 0 ]; then echo leaf; return; fi; f $((n-1)); }; f 1000; echo "$n"'
answer 'two hundred fifty six locals' 'f() { i=0; while [ "$i" -lt 256 ]; do eval "local v$i=$i"; i=$((i+1)); done; echo "$v0:$v127:$v255"; }; f; echo "${v0-unset}:${v255-unset}"'
answer 'long local name' 'local_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz=outer; f() { local local_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz=inner; echo "$local_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz"; }; f; echo "$local_name_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz"'
bash_answer 'function simple body rejected' 'function f echo body'

group redirection
check 'out'             'echo x > /tmp/pt1; cat /tmp/pt1'
check 'append'          'echo a > /tmp/pt2; echo b >> /tmp/pt2; cat /tmp/pt2'
answer 'noclobber and override' 'p=/tmp/pt-noclobber.$$; echo old > "$p"; set -C; echo new > "$p" 2>/dev/null; a=$?; before=$(cat "$p"); echo forced >| "$p"; b=$?; after=$(cat "$p"); rm -f "$p"; echo "$a:$before $b:$after"'
answer 'noclobber permits device' 'set -C; echo discarded > /dev/null; echo $?'
expected 'stdout and stderr' 'out|err|' 0 '{ echo out; echo err >&2; } &> /tmp/ptboth.$$; cat /tmp/ptboth.$$; rm /tmp/ptboth.$$'
expected 'both append' 'first|out|err|' 0 'echo first > /tmp/ptbotha.$$; { echo out; echo err >&2; } &>> /tmp/ptbotha.$$; cat /tmp/ptbotha.$$; rm /tmp/ptbotha.$$'
expected 'numeric is an arg' '2|' 0 'echo 2&> /tmp/ptbothn.$$; cat /tmp/ptbothn.$$; rm /tmp/ptbothn.$$'
expected 'both then stderr' 'out|divider|err|' 0 '{ echo out; echo err >&2; } &> /tmp/ptbotho.$$ 2> /tmp/ptbothe.$$; cat /tmp/ptbotho.$$; echo divider; cat /tmp/ptbothe.$$; rm /tmp/ptbotho.$$ /tmp/ptbothe.$$'
expected 'stderr then both' 'out|err|divider|' 0 '{ echo out; echo err >&2; } 2> /tmp/ptbothe.$$ &> /tmp/ptbotho.$$; cat /tmp/ptbotho.$$; echo divider; cat /tmp/ptbothe.$$; rm /tmp/ptbotho.$$ /tmp/ptbothe.$$'
expected 'both restores' 'outer-out|outer-err|inner-out|inner-err|' 0 '{ { echo inner-out; echo inner-err >&2; } &> /tmp/ptbothr.$$; echo outer-out; echo outer-err >&2; } 2>&1; cat /tmp/ptbothr.$$; rm /tmp/ptbothr.$$'
expected 'failed both restores' 'after|' 0 '{ echo hidden; } &> /no/such/ptboth/target; echo after'
expected 'low fd ordinary' 'after-out|after-err|in|' 0 '{ ulimit -n 10; echo in > /tmp/ptlowo.$$; echo after-out; echo after-err >&2; cat /tmp/ptlowo.$$; rm /tmp/ptlowo.$$; } 2>&1'
expected 'low fd both ten' 'after-out|after-err|out|err|' 0 '{ ulimit -n 10; { echo out; echo err >&2; } &> /tmp/ptlowb.$$; echo after-out; echo after-err >&2; cat /tmp/ptlowb.$$; rm /tmp/ptlowb.$$; } 2>&1'
expected 'low fd both eleven' 'after-out|after-err|out|err|' 0 '{ ulimit -n 11; { echo out; echo err >&2; } &> /tmp/ptlowe.$$; echo after-out; echo after-err >&2; cat /tmp/ptlowe.$$; rm /tmp/ptlowe.$$; } 2>&1'
expected 'low fd append' 'after-out|after-err|first|out|err|' 0 '{ ulimit -n 10; echo first > /tmp/ptlowa.$$; { echo out; echo err >&2; } &>> /tmp/ptlowa.$$; echo after-out; echo after-err >&2; cat /tmp/ptlowa.$$; rm /tmp/ptlowa.$$; } 2>&1'
low_fd_literal_sources
expected 'low fd nested eight' 'tail|one|two|middle|' 0 '{ ulimit -n 8; { { echo one; echo two >&2; } > /tmp/ptlni-a.$$ 2> /tmp/ptlni-b.$$; echo middle; } > /tmp/ptlni-c.$$ 2> /tmp/ptlni-d.$$; echo tail; cat /tmp/ptlni-a.$$ /tmp/ptlni-b.$$ /tmp/ptlni-c.$$ /tmp/ptlni-d.$$; rm /tmp/ptlni-a.$$ /tmp/ptlni-b.$$ /tmp/ptlni-c.$$ /tmp/ptlni-d.$$; } 2>&1'
check 'in'              'echo z > /tmp/pt3; cat < /tmp/pt3'
check 'stderr'          'ls /nonexistent 2>/dev/null; echo done'
check 'stderr to out'   'ls /nonexistent 2>&1 | wc -l'
check 'heredoc'         'cat <<EOF
line
EOF'
check 'heredoc quoted'  'cat <<"EOF"
$notexpanded
EOF'
answer 'heredoc default' 'cat <<EOF
${nosuch:-fallback}
EOF'
answer 'heredoc command' 'cat <<EOF
$(echo sub)
EOF'
bash_answer 'here string' 'cat <<< word'
bash_answer 'here string empty' 'x=; cat <<< "$x" | wc -c'
bash_answer 'here string holds fields' 'x="a  b"; cat <<< $x'
bash_answer 'here string expands' 'x=4; cat <<< "value=$x:$((x+1)):$(echo sub)"'
bash_answer 'here string last wins' 'cat <<< one <<< two'
bash_answer 'here string fd' 'cat 3<<< data <&3'
bash_answer 'here string pipeline' 'cat <<< abc | tr a-z A-Z'
bash_answer 'here string function' 'f() { read x; echo "[$x]"; }; f <<< "a b"'
bash_answer 'here string missing' 'cat <<<'

group builtins
check 'xargs joins'     'printf "a\nb\nc\n" | xargs echo'
check 'xargs one at a time' 'printf "1 2 3\n" | xargs -n 1 echo'
check 'cd pwd'          'cd /; pwd'
check 'test string'     '[ a = a ] && echo yes'
check 'test number'     '[ 2 -gt 1 ] && echo yes'
check 'test file'       '[ -d / ] && echo yes'
check 'shift'           'set -- a b c; shift; echo $1'
check 'unset'           'x=1; unset x; echo "[$x]"'
check 'export'          'export E=1; echo $E'
check 'read'            'echo data | { read v; echo $v; }'
check 'eval'            'eval echo hi'
check 'exit status'     'sh -c "exit 4" 2>/dev/null; echo $?'
check 'true false'      'true; echo $?; false; echo $?'
check 'printf'          'printf "%s-%s\n" a b'

group append assignment
bash_answer 'append unset' 'unset x; x+=ab; printf "[%s]\n" "$x"'
bash_answer 'append set' 'x=ab; x+=cd; printf "[%s]\n" "$x"'
bash_answer 'append quoted' 'x="a b"; x+=" c d"; printf "[%s]\n" "$x"'
bash_answer 'append expands' 'x=ab; y=cd; x+=$y; printf "[%s]\n" "$x"'
bash_answer 'append repeatedly' 'x=a; x+=b; x+=c; x+=d; printf "[%s]\n" "$x"'
bash_answer 'append command local' 'x=old; x+=new /bin/sh -c '\''printf "[%s]\n" "$x"'\''; printf "[%s]\n" "$x"'
bash_answer 'append special persists' 'x=old; x+=new export y=1; printf "[%s]\n" "$x"'
bash_answer 'append function local' 'x=old; f() { printf "[%s]\n" "$x"; }; x+=new f; printf "[%s]\n" "$x"'
bash_answer 'append long' "x=$long_word; x+=\$x; echo \${#x}"

group arithmetic
bash_answer 'arithmetic command true' '((1)); echo $?'
bash_answer 'arithmetic command false' '((0)); echo $?'
bash_answer 'arithmetic command assigns' 'x=1; ((x+=2)); echo "$x:$?"'
bash_answer 'arithmetic postfix' 'x=1; ((x++)); echo "$x:$?"'
bash_answer 'arithmetic prefix' 'x=1; ((++x)); echo "$x:$?"'
bash_answer 'arithmetic tested' 'x=0; if ((x+=2)); then echo "$x"; fi'
bash_answer 'arithmetic redirect' 'p=/tmp/bash-arith.$$; ((2>1)) > "$p"; echo "$?:$(wc -c < "$p")"; rm "$p"'
bash_answer 'arithmetic empty' '(( )); echo $?'
bash_answer 'arithmetic dollar operand' 'x=4; ((y=$x+2)); echo "$y:$?"'
bash_answer 'arithmetic braced operand' 'x=4; ((y=${x}+2)); echo "$y:$?"'
bash_answer 'arithmetic nested expansion' '((y=$((1+2))+3)); echo "$y:$?"'
bash_answer 'arithmetic command substitution' '((y=$(printf 3)+2)); echo "$y:$?"'
bash_answer 'arithmetic comma' 'x=0; ((x=1, x+=2)); echo "$x:$?"'
bash_answer 'arithmetic readonly' 'readonly x=1; ((x=2)); echo "$x:$?"'
bash_answer 'arithmetic errexit' 'set -e; ((0)); echo after'
bash_answer 'arithmetic syntax error' '((1+)); echo after'
bash_answer 'arithmetic never closed' '((1'
bash_answer 'c for loop' 'for ((i=0;i<3;i++)); do echo "$i"; done'
bash_answer 'c for descending' 'for ((i=3;i>0;i-=2)); do echo "$i"; done'
bash_answer 'c for empty parts' 'i=0; for ((;;)); do echo "$i"; ((++i==2)) && break; done'
bash_answer 'c for continue' 'for ((i=0;i<4;i++)); do ((i==1)) && continue; echo "$i"; done'
bash_answer 'c for continue updates' 'u=0; for ((i=0;i<3;i++,u++)); do continue; done; echo "$i:$u"'
bash_answer 'c for nested separator' 'for ((i=$(x=0; printf "%s" "$x"); i<2; i++)); do echo "$i"; done'
bash_answer 'c for nested' 'for ((i=0;i<2;i++)); do for ((j=0;j<2;j++)); do echo "$i$j"; done; done'
bash_answer 'c for repeated function' 'f() { for ((j=0;j<2;j++)); do :; done; }; for ((i=0;i<500;i++)); do f; done; echo "$i:$j"'

group double-brackets
bash_answer 'conditional nonempty' '[[ value ]]; echo $?'
bash_answer 'conditional empty' '[[ "" ]]; echo $?'
bash_answer 'conditional empty expansion' 'x=; [[ $x == "" ]]; echo $?'
bash_answer 'conditional no splitting' 'x="a b"; [[ $x == "a b" ]]; echo $?'
bash_answer 'conditional no globbing' 'd=$(mktemp -d); cd "$d"; : > one; x="*"; [[ $x == "*" ]]; echo $?; rm -rf "$d"'
bash_answer 'conditional string same' '[[ abc == abc ]]; echo $?'
bash_answer 'conditional string different' '[[ abc != abd ]]; echo $?'
bash_answer 'conditional pattern' '[[ abc == a* ]]; echo $?'
bash_answer 'conditional quoted pattern' '[[ abc == "a*" ]]; echo $?'
bash_answer 'conditional variable pattern' 'p="a*"; [[ abc == $p ]]; echo $?'
bash_answer 'conditional quoted variable pattern' 'p="a*"; [[ abc == "$p" ]]; echo $?'
bash_answer 'conditional mixed pattern quotes' '[[ abc == "a"* ]]; echo $?'
bash_answer 'conditional before' '[[ abc < abd ]]; echo $?'
bash_answer 'conditional after' '[[ abd > abc ]]; echo $?'
bash_answer 'conditional zero length' 'x=; [[ -z $x ]]; echo $?'
bash_answer 'conditional nonzero length' 'x=value; [[ -n $x ]]; echo $?'
bash_answer 'conditional integers' '[[ 12 -gt 3 && -2 -lt 0 && 4 -eq 4 ]]; echo $?'
bash_answer 'conditional integer variable' 'x=3; [[ x -eq 3 ]]; echo $?'
bash_answer 'conditional integer expression' '[[ 1+2 -eq 3 && 8/2 -ge 4 ]]; echo $?'
bash_answer 'conditional integer unset' 'unset x; [[ x -eq 0 ]]; echo $?'
bash_answer 'conditional integer nounset' 'set -u; unset x; [[ x -eq 0 ]]; echo after'
bash_answer 'conditional logical precedence' '[[ empty == full && x == x || y == y ]]; echo $?'
bash_answer 'conditional grouping' '[[ ( empty == full || x == x ) && ! z == q ]]; echo $?'
bash_answer 'conditional short circuit' 'p=/tmp/bash-condition.$$; rm -f "$p"; [[ yes == yes || $(touch "$p"; echo no) == yes ]]; [ -e "$p" ]; echo $?; rm -f "$p"'
bash_answer 'conditional command expansion' '[[ $(printf abc) == a* ]]; echo $?'
bash_answer 'conditional quoted closing' 'x="]]"; [[ $x == "]]" ]]; echo $?'
bash_answer 'conditional escaped closing' '[[ \]\] == "]]" ]]; echo $?'
bash_answer 'conditional adjacent equality is word' '[[ a==a ]]; echo $?'
bash_answer 'conditional adjacent conjunction' '[[ x&&y ]]; echo $?'
bash_answer 'conditional file predicates' 'p=/tmp/bash-condition.$$; printf x > "$p"; chmod 700 "$p"; ln -s "$p" "$p.l"; [[ -e $p && -f $p && -s $p && -r $p && -w $p && -x $p && -L $p.l && -d / ]]; echo $?; rm -f "$p" "$p.l"'
bash_answer 'conditional file ages' 'p=/tmp/bash-condition.$$; q=$p.q; printf x > "$p"; rm -f "$q"; [[ $p -nt $q && $q -ot $p ]]; echo $?; rm -f "$p"'
bash_answer 'conditional same file' 'p=/tmp/bash-condition.$$; q=$p.q; printf x > "$p"; ln "$p" "$q"; [[ $p -ef $q ]]; echo $?; rm -f "$p" "$q"'
bash_answer 'conditional variable set' 'x=; [[ -v x ]]; echo $?; unset x; [[ -v x ]]; echo $?'
bash_answer 'conditional option set' 'set -o pipefail; [[ -o pipefail ]]; echo $?'
bash_answer 'conditional redirect adjacent' 'p=/tmp/bash-condition.$$; [[ x == x ]]>"$p"; echo "$?:$(wc -c < "$p")"; rm -f "$p"'
bash_answer 'conditional tested' 'if [[ x == x ]]; then echo yes; fi'
bash_answer 'conditional errexit' 'set -e; [[ x == y ]]; echo after'
bash_answer 'conditional bad number' '[[ word -eq 1 ]]; echo after'
bash_answer 'conditional missing operand' '[[ x == ]]'
bash_answer 'conditional unknown operator' '[[ x -qq y ]]'
bash_answer 'conditional never closed' '[[ x == x'
bash_answer 'conditional multiline' '[[ x == x &&
   y == y ]]; echo $?'

group regex-conditional
bash_answer 'conditional regex substring' '[[ abc =~ b ]]; echo $?'
bash_answer 'conditional regex anchors' '[[ abc =~ ^a.*c$ ]]; echo $?; [[ zabc =~ ^a ]]; echo $?'
bash_answer 'conditional regex class' '[[ a123z =~ ^[[:alpha:]][[:digit:]]+[[:alpha:]]$ ]]; echo $?'
bash_answer 'conditional regex groups' '[[ abcdab =~ ^(ab|cd)+$ ]]; echo $?'
bash_answer 'conditional regex alternation' '[[ cd =~ ^(ab|cd)$ ]]; echo $?'
bash_answer 'conditional regex quoted literal' '[[ a.c =~ "a.c" ]]; echo $?; [[ abc =~ "a.c" ]]; echo $?'
bash_answer 'conditional regex quoted fragment' '[[ abc =~ ^"a".*c$ ]]; echo $?'
bash_answer 'conditional regex escaped dot' '[[ a.c =~ ^a\.c$ ]]; echo $?; [[ abc =~ ^a\.c$ ]]; echo $?'
bash_answer 'conditional regex variable' 'r="^a.*c$"; [[ abc =~ $r ]]; echo $?'
bash_answer 'conditional regex quoted variable' 'r="^a.*c$"; [[ abc =~ "$r" ]]; echo $?'
bash_answer 'conditional regex empty variable' 'r=; [[ abc =~ $r ]]; echo $?'
bash_answer 'conditional regex negated' '[[ ! abc =~ ^z ]]; echo $?'
bash_answer 'conditional regex logical' '[[ abc =~ ^a && abc =~ c$ || no =~ yes ]]; echo $?'
bash_answer 'conditional regex skipped invalid' '[[ x == x || x =~ [ ]]; echo $?'
bash_answer 'conditional regex invalid' '[[ x =~ [ ]]; echo "$?:after"'
bash_answer 'conditional regex missing rhs' '[[ x =~ ]]'
bash_answer 'conditional regex expansion skipped' 'p=/tmp/bash-regex.$$; rm -f "$p"; [[ x == x || x =~ $(touch "$p"; echo x) ]]; [ -e "$p" ]; echo $?; rm -f "$p"'
bash_answer 'conditional regex multicall state' 'p=/tmp/bash-regex.$$; printf "alpha\nbeta\n" > "$p"; grep alpha "$p"; [[ abc =~ ^a ]]; grep beta "$p"; sed -n "1p" "$p"; [[ xyz =~ z$ ]]; sed -n "2p" "$p"; rm -f "$p"'
bash_answer 'conditional regex repeated' 'i=0; while [ $i -lt 200 ]; do [[ abc123 =~ ^[a-z]+[0-9]+$ ]] || break; i=$((i+1)); done; echo "$i"'

check 'shift left'      'echo $((1<<4))'
check 'shift right'     'echo $((64>>3))'
check 'bit and'         'echo $((12&10))'
check 'bit or'          'echo $((12|3))'
check 'bit xor'         'echo $((12^10))'
check 'bit precedence'  'echo $((1|2&3)) $((2^3|4)) $((5&3|8))'
check 'bit not'         'echo $((7&~2))'
check 'shift chain'     'echo $((1<<3>>1))'
check 'plus equals'     'x=1; : $((x+=2)); echo $x'
check 'minus equals'    'x=9; : $((x-=4)); echo $x'
check 'times equals'    'x=3; : $((x*=4)); echo $x'
check 'divide equals'   'x=9; : $((x/=2)); echo $x'
check 'modulo equals'   'x=9; : $((x%=4)); echo $x'
check 'shift equals'    'x=1; : $((x<<=4)); echo $x'
check 'or equals'       'x=12; : $((x|=3)); echo $x'
check 'assign value'    'echo $((y=7)); echo $y'
check 'mixed'           'echo $((2+3*4)) $((0||1&&1))'
check 'ternary'         'echo $((1 ? 2 : 3)) $((0 ? 2 : 3))'
check 'ternary chained' 'echo $((0 ? 1 : 0 ? 2 : 3)) $((1 ? 2 : 3 ? 4 : 5))'
check 'ternary applied' 'x=5; echo $(( x > 3 ? x * 2 : 0 ))'
check 'hexadecimal'     'echo $((0x10)) $((0X1f))'
check 'octal'           'echo $((010)) $((0644))'
check 'base in a value' 'x=010; echo $((x)) $((x+1))'

group sourcing
check 'dot runs'        'echo echo sourced > /tmp/pd1; . /tmp/pd1'
check 'dot sets'        'echo x=42 > /tmp/pd2; . /tmp/pd2; echo $x'
check 'dot status'      'echo false > /tmp/pd3; . /tmp/pd3; echo $?'
check 'dot missing'     '. /tmp/nosuchfile 2>/dev/null; echo $?'
answer 'dot return stops source' 'p=/tmp/dot-return.$$; printf '\''echo in\nreturn 7\necho BAD\n'\'' > "$p"; . "$p"; s=$?; rm -f "$p"; echo "$s"'
answer 'dot return stops at source' 'p=/tmp/dot-function-return.$$; printf '\''echo in\nreturn 7\necho BAD\n'\'' > "$p"; f() { . "$p"; echo function-continues; }; f; s=$?; rm -f "$p"; echo "$s"'
answer 'dot break reaches loop' 'p=/tmp/dot-break.$$; printf '\''echo in\nbreak\necho BAD\n'\'' > "$p"; for i in a b; do . "$p"; echo LOOP-BAD; done; rm -f "$p"; echo after'
answer 'dot continue reaches loop' 'p=/tmp/dot-continue.$$; printf '\''echo in\ncontinue\necho BAD\n'\'' > "$p"; for i in a b; do . "$p"; echo LOOP-BAD; done; rm -f "$p"; echo after'
check 'dot path need not execute' 'p=/tmp/dot-path.$$; /bin/mkdir "$p"; printf '\''echo sourced\n'\'' > "$p/include"; /bin/chmod 600 "$p/include"; cd /; PATH=$p; . include; /bin/rm -f "$p/include"; /bin/rmdir "$p"'
check 'dot reads one stream' '{ printf '\''x=first\n#'\''; awk '\''BEGIN { for (i = 0; i < 5000; i++) printf "a" }'\''; printf '\''\necho "$x"\n'\''; } | "$0" -c '\''. /dev/stdin'\'''

group naming
check 'type builtin'    'type echo'
check 'type function'   'f() { :; }; type f'
check 'type missing'    'type nosuchthing 2>/dev/null; echo $?'
check 'command dash v'  'command -v echo'
check 'command runs'    'command echo hi'
check 'command v miss'  'command -v nosuchthing 2>/dev/null; echo $?'
check 'command p finds'  'PATH= command -pv sh | /bin/sed '\''s|.*/||'\'''
check 'command p runs'   'PATH= command -p sh -c '\''echo ok'\'''
check 'command v empty' 'command -v '\''\'' >/dev/null 2>&1; echo $?'
check 'type empty'      'type '\''\'' >/dev/null 2>&1; echo $?'
check 'hash empty'      'hash '\''\'' >/dev/null 2>&1; echo $?'

# Empty PATH fields are the current directory, including the first, middle,
# and last field. Clear the command cache between them so every field is
# actually searched rather than merely replaying the first answer.
check 'empty path fields' 'p=/tmp/empty-path.$$; /bin/mkdir "$p"; printf '\''#!/bin/sh\necho cwd\n'\'' > "$p/pathprobe"; /bin/chmod +x "$p/pathprobe"; cd "$p"; PATH=:/bin pathprobe; hash -r; PATH=/bin::/usr/bin pathprobe; hash -r; PATH=/bin: pathprobe; /bin/rm -f "$p/pathprobe"; /bin/rmdir "$p"'
bash_answer 'command v multiple names' 'command -v sh definitely_missing echo; echo $?'

group traps
check 'exit trap'       'trap "echo bye" EXIT; echo hi'
check 'exit trap code'  'trap "echo bye" EXIT; exit 3'
check 'exit trap gone'  'trap "echo bye" EXIT; trap - EXIT; echo hi'
check 'wait for all'    'true & wait; echo done'

#
#       The same language again, with the exit status looked at too.
#
#       A script's status is what its caller reads, and the section above
#       compares standard output alone -- which is how a shell that printed
#       the right thing and then said the wrong number about it went years
#       without anybody noticing.
#

section strict

group status
answer 'pipeline last'   'false | true; echo $?'
answer 'direct non executable' 'p=/tmp/not-executable.$$; printf '\''echo bad\n'\'' > "$p"; chmod 600 "$p"; "$p" 2>/dev/null; s=$?; rm -f "$p"; echo "$s"'
bash_answer 'path non executable' 'd=/tmp/not-executable.$$; mkdir "$d"; printf '\''echo bad\n'\'' > "$d/x"; chmod 600 "$d/x"; PATH=$d x 2>/dev/null; s=$?; /bin/rm -f "$d/x"; /bin/rmdir "$d"; echo "$s"'
answer 'exec missing is fatal' 'exec definitely_missing 2>/dev/null; echo BAD'
answer 'exec denied is fatal' 'p=/tmp/exec-denied.$$; printf '\''echo bad\n'\'' > "$p"; chmod 600 "$p"; exec "$p" 2>/dev/null; echo BAD'
answer 'pipeline fails'  'true | false; echo $?'
answer 'pipeline sixty four stages' 'p=true; i=1; while [ "$i" -lt 64 ]; do p="$p | true"; i=$((i+1)); done; eval "$p"; echo $?'
answer 'pipeline sixty five stages' 'p=true; i=1; while [ "$i" -lt 65 ]; do p="$p | true"; i=$((i+1)); done; eval "$p"; echo $?'
answer 'pipeline one hundred twenty eight stages' 'p=true; i=1; while [ "$i" -lt 128 ]; do p="$p | true"; i=$((i+1)); done; eval "$p"; echo $?'
expected 'pipefail left' '1|' 0 'set -o pipefail; false | true; echo $?'
expected 'pipefail right' '1|' 0 'set -o pipefail; true | false; echo $?'
expected 'pipefail rightmost' '7|' 0 'set -o pipefail; (exit 3) | (exit 7) | true; echo $?'
expected 'pipefail all pass' '0|' 0 'set -o pipefail; true | true | true; echo $?'
expected 'pipefail one hundred twenty eight' '7|' 0 'set -o pipefail; p="exit 7"; i=1; while [ "$i" -lt 128 ]; do p="$p | true"; i=$((i+1)); done; eval "$p"; echo $?'
expected 'pipefail disabled' '0|' 0 'set -o pipefail; set +o pipefail; false | true; echo $?'
expected 'pipefail inverted' '0|' 0 'set -o pipefail; ! false | true; echo $?'
expected 'pipefail signal' '143|' 0 'set -o pipefail; /bin/sh -c "kill -TERM \$\$" | true; echo $?'
expected 'pipefail errexit' '' 1 'set -eo pipefail; false | true; echo not-reached'
expected 'pipefail assignment' '1|' 0 'set -o pipefail; x=$(false | true); echo $?'
expected 'pipefail assignment right' '1|' 0 'set -o pipefail; x=$(true | false); echo $?'
expected 'pipefail assignment errexit' '' 1 'set -eo pipefail; x=$(false | true); echo not-reached'
expected 'pipefail tested' 'caught|after|' 0 'set -eo pipefail; false | true || echo caught; echo after'
expected 'pipefail sub value' 'value:1|' 0 'set -o pipefail; x=$(echo value; false | true); echo "$x:$?"'
expected 'last assignment sub' '7|' 0 'set -o pipefail; a=$(exit 3) b=$(false | true) c=$(exit 7); echo $?'
expected 'pipefail is listed' '1|' 0 'set -o pipefail; set -o | grep -c pipefail'
answer 'assignment sub status' 'x=$(exit 3); echo $?'
answer 'assignment sub errexit' 'set -e; x=$(false); echo not-reached'
answer 'builtin succeeds' 'false; echo hi; echo $?'
answer 'subshell exit'   '(exit 5); echo $?'
answer 'function return' 'f() { return 4; }; f; echo $?'
answer 'function body'   'f() { false; }; f; echo $?'
answer 'loop body'       'for i in 1; do false; done; echo $?'
answer 'branch taken'    'if true; then if false; then echo a; else echo b; fi; fi'
answer 'not a command'   'nosuchcommand12345; echo $?'
answer 'external exit 127' '/bin/sh -c "exit 127"; echo $?'

group interactive-fatal
interactive_fatal 'unsupported transform' 'bad substitution' 'x=ab' \
        'echo RAN-BAD "${x@Q}"; echo SAME-LINE'
interactive_fatal 'unsupported indirect' 'bad substitution' 'x=ab' \
        'echo RAN-BAD "${!x}"; echo SAME-LINE'
interactive_fatal 'parameter required' 'x: boom' 'unset x' \
        'echo RAN-BAD "${x:?boom}"; echo SAME-LINE'
interactive_fatal 'bad arithmetic' 'arithmetic: 1/0' ':' \
        'echo RAN-BAD "$((1/0))"; echo SAME-LINE'
interactive_fatal 'redirect expansion' 'bad substitution' 'x=ab' \
        'echo RAN-BAD >"${x@Q}"; echo SAME-LINE'
interactive_fatal 'for expansion' 'bad substitution' 'x=ab' \
        'for v in ${x@Q}; do echo RAN-BAD; done; echo SAME-LINE'
interactive_fatal 'case expansion' 'bad substitution' 'x=ab' \
        'case ${x@Q} in *) echo RAN-BAD;; esac; echo SAME-LINE'
interactive_fatal 'heredoc expansion' 'bad substitution' 'x=ab' \
        'cat <<EOF; echo SAME-LINE
${x@Q}
EOF' continue
interactive_fatal 'command substitution child' 'bad substitution' 'x=ab' \
        'value=$(echo "${x@Q}"
echo RAN-BAD >&2
)'
interactive_fatal 'nested fatal has no assignment' 'bad substitution' \
        'unset x; y=ab' 'echo RAN-BAD "${x:=${y@Q}}"; echo SAME-LINE' \
        abort 'echo X:${x+set}:${x-UNSET}' 'X::UNSET'
interactive_fatal 'pipeline child expansion' 'bad substitution' 'x=ab' \
        'echo RAN-BAD "${x@Q}" | cat; echo SAME-LINE' continue
interactive_fatal 'pipeline child redirect' 'x: boom' 'unset x' \
        'echo RAN-BAD >"${x:?boom}" | cat; echo SAME-LINE' continue
interactive_fatal_trap

group heredoc-isolation
expected 'required status' 'FIRST:2|AFTER|' 0 'unset x
cat <<EOF
${x:?boom}
EOF
echo FIRST:$?
echo AFTER'
expected 'assignment isolated' 'made|FIRST:0|X::UNSET|' 0 'unset x
cat <<EOF
${x:=made}
EOF
echo FIRST:$?
echo X:${x+set}:${x-UNSET}'
expected 'nested fatal isolated' 'FIRST:2|X::UNSET|AFTER|' 0 'unset x; y=ab
cat <<EOF
${x:=${y@Q}}
EOF
echo FIRST:$?
echo X:${x+set}:${x-UNSET}
echo AFTER'

# A builtin that cannot fail still has to say it did not, and each of these
# runs one after a failure so the old status is there to be left behind.
answer 'export says so'  'false; export E=1; echo $?'
answer 'pwd says so'     'false; pwd > /dev/null; echo $?'
answer 'shift says so'   'false; set -- a b; shift; echo $?'
answer 'read says so'    'false; echo x | { read v; echo $?; }'
answer 'trap says so'    'false; trap > /dev/null; echo $?'
answer 'test no words'   '[ ] ; echo $?'
answer 'test bad word'   '[ 1 -zz 2 ] 2>/dev/null; echo $?'
answer 'cd missing'      'cd /nosuchdir12345 2>/dev/null; echo $?'
answer 'cd missing runs' 'cd /nosuchdir12345 2>/dev/null || echo refused'
answer 'group status'    '{ exit 0; }; echo $?'
answer 'assignment'      'x=1; echo $?'
answer 'nothing at all'  ''
answer 'colon'           ': ; echo $?'
answer 'exit direct'     'exit 42'
answer 'exit bare'       'true; exit'
answer 'exit 300'        'exit 300'
answer 'and then or'     'true && false || true; echo $?'
answer 'while status'    'i=0; while [ $i -lt 2 ]; do i=$((i+1)); done; echo $?'
answer 'case no match'   'case x in a) echo a;; esac; echo $?'
answer 'nested sub'      'echo $(exit 2)$?; echo $?'

group quoting
answer 'quoted star'     'echo "*"'
answer 'nested quotes'   'echo "a'"'"'b"'
answer 'empty single'    "echo ''x''"
answer 'backslash dollar' 'echo \$x'
answer 'quote in word'   'echo ab"cd"ef'
answer 'quoted spaces'   'set -- "a  b"; echo $#'
answer 'unquoted spaces' 'set -- a  b; echo $#'
answer 'dollar in single' "echo '\$(echo hi)'"

group parameters
answer 'default nested'  'echo ${a:-${b:-x}}'
answer 'alternate unset' 'echo "[${u:+set}]"'
answer 'alternate empty' 'u=; echo "[${u:+set}]" "[${u+set}]"'
answer 'length unset'    'echo ${#nosuch}'
answer 'length braced'   'set -- a b c; echo ${#}'
answer 'star quoted'     'set -- a b; echo "$*"'
answer 'star ifs'        'IFS=-; set -- a b; echo "$*"'
answer 'dollar zero'     'echo ${0:+set}'
answer 'prefix no match' 'x=abc; echo ${x#z}'
answer 'suffix no match' 'x=abc; echo ${x%z}'
answer 'prefix all'      'x=abc; echo "[${x#abc}]"'
answer 'indirect eval'   'x=y; y=z; eval echo \$$x'
answer 'positional ten'  'set -- 1 2 3 4 5 6 7 8 9 10; echo ${10}'

# ${x?} exists to stop the script, so saying so and carrying on is the one
# thing it must not do. dash leaves 2 behind and runs the exit trap on the way.
answer 'unset is fatal'  'echo ${nosuch?}'
answer 'fatal with word' 'echo a; : ${nosuch?gone}; echo b'
answer 'fatal on empty'  'x=; echo ${x:?}'
answer 'set is not'      'x=1; echo ${x?}'
answer 'empty without colon' 'x=; echo "[${x?}]"'
answer 'fatal runs trap' 'trap "echo bye" EXIT; echo a; echo ${nosuch?}'
answer 'fatal in a sub'  'trap "echo bye" EXIT; echo "[$(echo ${u?})]"; echo after'
answer 'fatal sub alone' 'echo $(echo ${u?})x; echo after'

group splitting
answer 'ifs colon'       'IFS=:; x=a:b:c; set -- $x; echo $#'
answer 'ifs empty'       'IFS=; x="a b"; set -- $x; echo $#'
answer 'read two lines'  'printf "1\n2\n" | { read a; read b; echo "$a$b"; }'
answer 'read no newline' 'printf "x" | { read v; echo "[$v]"; }'
answer 'read extra'      'echo a b c | { read x y; echo "$y"; }'
answer 'read uses ifs'   'IFS=: ; echo a:b | { read x y; echo "$x-$y"; }'
answer 'read joins'      'printf "a\\\\b\n" | { read v; printf "[%s]\n" "$v"; }'
answer 'read raw keeps'  'printf "a\\\\b\n" | { read -r v; printf "[%s]\n" "$v"; }'

group arithmetic
answer 'unary minus'     'echo $((-5 + 2))'
answer 'unary plus'      'echo $((+5))'
answer 'logical not'     'echo $((!0)) $((!5))'
answer 'parentheses'     'echo $(((2+3)*4))'
answer 'modulo'          'echo $((7%3)) $((-7%3))'
answer 'divide negative' 'echo $((-7/2))'
answer 'equality'        'echo $((3==3)) $((3!=3))'
answer 'logical pair'    'echo $((0||3)) $((1&&0))'
answer 'shift and add'   'echo $((2+3<6)) $((1<<2+1))'
answer 'unset variable'  'echo $((nosuch+1))'
answer 'inner spaces'    'echo $(( 1 + 2 ))'
answer 'four terms'      'echo $((1+2*3-4/2))'
answer 'left to right'   'echo $((100/10/2))'
answer 'shift negative'  'echo $((-8>>1))'
answer 'past thirty two' 'echo $((1<<40))'
answer 'xor precedence'  'echo $((1^2&3)) $((1|2^3))'
answer 'comparison run'  'echo $((1<2)) $((2<=2)) $((3>=4))'
answer 'and skips assignment' 'x=0; echo $((0 && (x=1))) $x'
answer 'or skips assignment' 'x=0; echo $((1 || (x=1))) $x'
answer 'and skips division' 'echo $((0 && 1/0)); echo after'
answer 'or skips division' 'echo $((1 || 1/0)); echo after'
answer 'ternary takes one arm' 'x=0; echo $((1 ? (x=1) : (x=2))) $x'
answer 'ternary skips division' 'echo $((0 ? 1/0 : 7)); echo after'

group redirection
answer 'to stderr'       'echo x 1>&2 2>/dev/null; echo done'
answer 'stderr to pipe'  'sh -c "echo e 1>&2" 2>&1 | cat'
answer 'read write'      'echo abc > /tmp/sr1; exec 3<> /tmp/sr1; read v <&3; echo $v'
answer 'heredoc expand'  'x=1; cat <<EOF
$x
EOF'
answer 'heredoc escape'  'cat <<EOF
a\$b
EOF'
answer 'two heredocs'    'cat <<A; cat <<B
one
A
two
B'
answer 'nine heredocs'   'cat <<A; cat <<B; cat <<C; cat <<D; cat <<E; cat <<F; cat <<G; cat <<H; cat <<I
1
A
2
B
3
C
4
D
5
E
6
F
7
G
8
H
9
I'
answer 'many redirections' 'true 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null; echo $?'

# <<- takes every leading tab off every line of the body and off the line that
# ends it. The lexer knows << and not <<-, so the dash arrives as the front of
# the word behind it -- on its own when a blank follows and stuck to the
# delimiter when none does -- and both spellings have to mean the same thing.
answer 'dash heredoc'    'cat <<-EOF
	indented
		deeper
	EOF'
answer 'dash then blank' 'cat <<- EOF
	spaced
	EOF'
answer 'dash keeps spaces' 'cat <<-EOF
	  two spaces
	EOF'
answer 'dash expands'    'x=v; cat <<-EOF
	$x
	EOF'
answer 'dash quoted'     'x=v; cat <<-"EOF"
	$x
	EOF'
answer 'dash ends bare'  'cat <<-EOF
	body
EOF'
answer 'two dash bodies' 'cat <<-A; cat <<-B
	one
	A
	two
	B'
answer 'a dash delimiter' 'cat << -EOF
plain
-EOF'
answer 'plain keeps tabs' 'cat <<EOF
	kept
EOF'

# A here-document is how a script carries a file inside it, so the body has to
# be able to be one. Two hundred and fifty lines used to be the ceiling, and
# reaching it dropped the rest without a word; past what a pipe will hold the
# body is written by a child rather than into a pipe nothing is draining.
answer 'a long body'     '{ echo "cat <<\"END\""; i=0; while [ $i -lt 1000 ]; do echo "line $i padding padding padding"; i=$((i+1)); done; echo END; } > /tmp/gh1.$$
. /tmp/gh1.$$ > /tmp/gh2.$$
wc -l < /tmp/gh2.$$'
answer 'a body past a pipe' '{ echo "cat <<\"END\""; i=0; while [ $i -lt 1800 ]; do echo "line $i padding padding padding"; i=$((i+1)); done; echo END; } > /tmp/gh3.$$
. /tmp/gh3.$$ > /tmp/gh4.$$
wc -l < /tmp/gh4.$$'
answer 'order of words'  'echo x > /tmp/sr2 2>&1; cat /tmp/sr2'
answer 'loop redirected' 'for i in 1 2; do echo $i; done > /tmp/sr3; cat /tmp/sr3'
answer 'outer stderr contains failure' \
        '{ echo before; echo hidden > /no/such/redirection/target; echo after; } 2>/dev/null; echo tail'
answer 'closed stderr contains failure' \
        'echo hidden 2>&- > /no/such/redirection/target; echo after'
answer 'redirected stderr contains failure' \
        'p=/tmp/sr-failure.$$; echo hidden 2>"$p" > /no/such/redirection/target; s=$?; [ -s "$p" ] && echo contained; rm -f "$p"; echo "$s"'

# exec with nothing to run is there for its redirections, and those outlive
# the command. The descriptor is three because open hands back the lowest one
# free, which is three, and dup3 onto the descriptor it was given is an error
# rather than the no-op dup2 makes of it.
answer 'exec keeps a write' 'exec 3> /tmp/sr4.$$; echo kept >&3; exec 3>&-; cat /tmp/sr4.$$'
answer 'exec keeps a read' 'echo r > /tmp/sr5.$$; exec 3< /tmp/sr5.$$; read v <&3; echo $v'
answer 'exec four as well' 'exec 4> /tmp/sr6.$$; echo four >&4; exec 4>&-; cat /tmp/sr6.$$'
answer 'a command does not' 'echo a > /tmp/sr7.$$; true 3< /tmp/sr7.$$; read v <&3 2>/dev/null; echo "[$v]"'

group control
answer 'case alternates' 'case b in a|b) echo yes;; esac'
answer 'case escaped'    'case "a*b" in a\*b) echo yes;; esac'
answer 'case class'      'case 5 in [0-9]) echo digit;; esac'
answer 'case catch all'  'case x in *) echo any;; esac'

# case used to have a matcher of its own, which knew stars and ranges and not
# the twelve POSIX class names -- so a pattern that worked as a glob and in a
# ${x#...} trim did nothing in a case arm. There is one matcher now.
answer 'case digit class' 'case 5 in [[:digit:]]) echo yes;; *) echo no;; esac'
answer 'case alpha class' 'case a in [[:alpha:]]) echo yes;; *) echo no;; esac'
answer 'case upper class' 'case A in [[:upper:]]) echo yes;; *) echo no;; esac'
answer 'case lower class' 'case A in [[:lower:]]) echo yes;; *) echo no;; esac'
answer 'case space class' 'case " " in [[:space:]]) echo yes;; *) echo no;; esac'
answer 'case punct class' 'case - in [[:punct:]]) echo yes;; *) echo no;; esac'
answer 'case xdigit class' 'case f in [[:xdigit:]]) echo yes;; *) echo no;; esac'
answer 'case class negated' 'case a in [![:digit:]]) echo yes;; *) echo no;; esac'
answer 'case class among' 'case b in [[:digit:]abc]) echo yes;; *) echo no;; esac'
answer 'case class in a word' 'case a5z in a[[:digit:]]z) echo yes;; *) echo no;; esac'
answer 'case class with star' 'case abC in *[[:upper:]]*) echo yes;; *) echo no;; esac'
answer 'case unknown class' 'case a in [[:nosuch:]]) echo yes;; *) echo no;; esac'
answer 'case bracket alone' 'case "[" in [) echo yes;; *) echo no;; esac'
answer 'case bracket first' 'case "]" in []a]) echo yes;; *) echo no;; esac'
answer 'case dash last'   'case - in [a-]) echo yes;; *) echo no;; esac'
answer 'while read'      'printf "1\n2\n" | while read v; do echo "[$v]"; done'
answer 'until once'      'until true; do echo no; done; echo done'
answer 'for nothing'     'for i in; do echo $i; done; echo done'
# A reserved word is only reserved where a command name was expected, and the
# list of a for loop is not that place. The list ends at the separator, so
# "for i in then do; ..." walks two items and finds its own do after them.
answer 'for a word do'    'for i in then do; do echo $i; done'
answer 'for a word done'  'for i in done esac fi; do echo $i; done'
answer 'for wants the semi' 'for i in a b do echo x; done'
answer 'glob no match'   'cd /tmp; for i in nosuchglob*; do echo "$i"; done'

# A for loop expands its list exactly as a command expands its arguments:
# fields split, patterns matched, quotes honoured.
answer 'for splits'      'x="a b"; for i in $x; do echo "[$i]"; done'
answer 'for globs'       'cd /; for i in /et*; do echo "$i"; done'
answer 'for keeps quotes' 'x="a b"; for i in "$x" c; do echo "[$i]"; done'
answer 'for at is many'  'set -- "a b" c; for i in "$@"; do echo "[$i]"; done'
answer 'for star is one' 'set -- a b; for i in "$*"; do echo "[$i]"; done'
answer 'for ifs'         'IFS=:; y=a:b; for i in $y; do echo "[$i]"; done'
answer 'for unset makes none' 'for i in $nosuch; do echo no; done; echo done'
answer 'break two'       'for i in 1 2; do for j in a b; do break 2; done; echo $i; done; echo done'
answer 'continue two'    'for i in 1 2; do for j in a b; do continue 2; done; echo $i; done; echo done'
answer 'recursion'       'f() { [ $1 -gt 0 ] && { echo $1; f $(($1-1)); }; }; f 3'
answer 'recursion deep'  'f() { [ $1 -le 0 ] && { echo bottom; return; }; f $(($1-1)); }; f 100'
answer 'recursion locals' 'f() { local v=$1; [ $1 -le 0 ] && { echo bottom; return; }; f $(($1-1)); [ "$v" = "$1" ] || echo lost; }; f 120'
answer 'function args'   'f() { set -- x; echo $1; }; set -- y; f; echo $1'
answer 'function in one' 'f() { g() { echo inner; }; g; }; f'

# A reserved word is not a name, so it cannot be a function either: the word
# in front of a command is read as the keyword every time, which makes such a
# definition something that can never be reached.
answer 'function named if' 'if() { echo k; }; if'
answer 'function named for' 'for() { echo k; }'
answer 'function named done' 'done() { echo k; }'
answer 'function named in' 'in() { echo k; }'
answer 'a name with a digit' 'f2() { echo n; }; f2'
answer 'a name with a bar'  '_f() { echo u; }; _f'

# set -e, and the four places POSIX says it does not reach: the condition of
# an if or a loop, everything but the last of an && or || list, and a pipeline
# whose status is inverted. Every case here is one of those or its opposite,
# because a shell that exits too eagerly is as wrong as one that never does.
answer 'errexit stops'   'set -e; false; echo not reached'
answer 'errexit status'  'set -e; sh -c "exit 7"; echo not reached'
answer 'errexit if body' 'set -e; if true; then false; fi; echo not reached'
answer 'errexit if cond' 'set -e; if false; then echo a; fi; echo ok'
answer 'errexit elif cond' 'set -e; if false; then :; elif false; then :; fi; echo ok'
answer 'errexit while body' 'set -e; while true; do false; done; echo not reached'
answer 'errexit while cond' 'set -e; while false; do :; done; echo ok'
answer 'errexit until cond' 'set -e; until true; do :; done; echo ok'
answer 'errexit for body' 'set -e; for i in 1 2; do false; done; echo not reached'
answer 'errexit case body' 'set -e; case x in x) false;; esac; echo not reached'
answer 'errexit group'   'set -e; { false; }; echo not reached'
answer 'errexit and last' 'set -e; true && false; echo not reached'
answer 'errexit and middle' 'set -e; true && false && echo x; echo ok'
answer 'errexit or last'  'set -e; false || true; echo ok'
answer 'errexit inverted' 'set -e; ! true; echo ok'
answer 'errexit pipe last' 'set -e; true | false; echo not reached'
answer 'errexit pipe first' 'set -e; false | true; echo ok'
answer 'errexit subshell' 'set -e; (false); echo not reached'
answer 'errexit sub tested' 'set -e; if (false); then echo a; else echo b; fi; echo ok'
answer 'errexit function' 'set -e; f() { false; echo x; }; f; echo not reached'
answer 'errexit func tested' 'set -e; f() { false; }; if f; then echo a; else echo b; fi; echo ok'
answer 'errexit func or'  'set -e; f() { return 1; }; f || echo ok'
answer 'errexit eval'    'set -e; eval false; echo not reached'
answer 'errexit eval tested' 'set -e; if eval false; then echo a; else echo b; fi; echo ok'
answer 'errexit dot'     'set -e; echo false > /tmp/se1.$$; . /tmp/se1.$$; echo not reached'
answer 'errexit turned off' 'set -e; set +e; false; echo ok'
answer 'errexit runs the trap' 'set -e; trap "echo bye" EXIT; false; echo not reached'
answer 'errexit sub trap once' 'set -e; trap "echo bye" EXIT; (false); echo not reached'
answer 'errexit break'   'set -e; while true; do break; done; echo ok'
answer 'errexit in a sub' 'set -e; trap "echo bye" EXIT; echo "[$(false)]"; echo after'
answer 'errexit sub keeps going' 'set -e; echo "[$(false; echo x)]"; echo after'

# An assignment written in front of a command belongs to that command.
#
# It has to be visible to what runs -- a spawned program reads it out of the
# environment and a builtin reads it out of the same table -- and it has to be
# gone afterwards. The exception is the fifteen names POSIX calls special, in
# front of which the assignment stays; dash draws exactly that line and this
# checks both sides of it.

group prefixed
answer 'ordinary assignment is local' 'unset X; X=local; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'gone afterwards'  'x=old; x=new true; echo "[$x]"'
answer 'never set before' 'y=new true; echo "[${y-unset}]"'
answer 'exported stays'   'export E=keep; E=temp true; echo "[$E]"'
answer 'seen by the child' 'v=seen sh -c "echo [\$v]"'
answer 'seen by a builtin' 'IFS=: ; x=a:b; set -- $x; echo $#'
answer 'a function too'   'v=o; f() { echo "in $v"; }; v=n f; echo "out $v"'
answer 'special keeps it' 'v=o; v=n export Q=1; echo "[$v]"'
answer 'colon keeps it'   'v=o; v=n :; echo "[$v]"'
answer 'eval keeps it'    'v=o; v=n eval echo "in \$v"; echo "out $v"'
answer 'plain does not'   'v=o; v=n cd /; echo "[$v]"'
answer 'two of them'      'a=1; b=2; a=x b=y true; echo "$a$b"'
answer 'three prefixed arguments' 'a=1 b=2 c=3 printf "[%s][%s]\\n" x y; echo "[${a-unset}${b-unset}${c-unset}]"'
answer 'empty value back' 'v=; v=n true; echo "[$v]"'
answer 'not found either' 'v=o; v=n nosuchcommand12345 2>/dev/null; echo "[$v]"'
answer 'alone it stays'   'v=o; v=n; echo "[$v]"'
answer 'in a loop'        'v=o; for i in 1 2; do v=n true; done; echo "[$v]"'
answer 'temporary export does not stick' \
        'unset X; X=temp /bin/sh -c '\''echo "$X"'\''; X=after; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'exported prefix restores' \
        'export X=outer; X=temp /bin/sh -c '\''echo "$X"'\''; /bin/sh -c '\''echo "$X"'\'''
answer 'twenty prefixed assignments' \
        'A00=0 A01=1 A02=2 A03=3 A04=4 A05=5 A06=6 A07=7 A08=8 A09=9 A10=10 A11=11 A12=12 A13=13 A14=14 A15=15 A16=16 A17=17 A18=18 A19=19 /bin/sh -c '\''echo "$A00:$A09:$A19"'\''; echo "${A00-unset}:${A19-unset}"'

group export-state
answer 'export pending name' \
        'unset X; export X; X=one; /bin/sh -c '\''echo "$X"'\'''
answer 'exported empty value' \
        'unset X; export X=; /bin/sh -c '\''echo "[${X-unset}]"'\'''
answer 'unset clears export bit' \
        'export X=one; unset X; X=two; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'export rebuilds repeatedly' \
        'X=one; /bin/sh -c '\''echo ${X-unset}'\''; export X; /bin/sh -c '\''echo "$X"'\''; X=two; /bin/sh -c '\''echo "$X"'\''; unset X; /bin/sh -c '\''echo ${X-unset}'\''; X=three; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'function export is scoped with prefix' \
        'unset X; f() { export X; }; X=one f; echo "${X-unset}"; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'local inherits export bit' \
        'export X=outer; f() { local X=inner; /bin/sh -c '\''echo "$X"'\''; }; f; /bin/sh -c '\''echo "$X"'\'''
answer 'local restores unexported bit' \
        'X=outer; f() { local X=inner; export X; }; f; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'local absent stays absent' \
        'unset X; f() { local X=inner; export X; }; f; echo "${X-unset}"; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'eval prefix is temporary export' \
        'unset X; X=one eval '\''/bin/sh -c "echo \$X"'\''; echo "$X"; /bin/sh -c '\''echo ${X-unset}'\'''
answer 'dot prefix is not exported' \
        'unset X; f=/tmp/export-dot.$$; printf '\''/bin/sh -c '\''\''\''echo ${X-unset}'\''\''\''\n'\'' > "$f"; X=one . "$f"; echo "$X"; /bin/sh -c '\''echo ${X-unset}'\''; rm -f "$f"'
answer 'exec prefix is exported' \
        'unset X; X=one exec /bin/sh -c '\''echo "$X"'\'''
answer 'command prefix is exported' \
        'unset X; X=one command /bin/sh -c '\''echo "$X"'\''; echo "${X-unset}"'
answer 'subshell export is isolated' \
        'X=outer; (export X=inner; /bin/sh -c '\''echo "$X"'\''); /bin/sh -c '\''echo ${X-unset}'\'''
answer 'export list excludes locals' \
        'X=local; export Y=public; export -p | grep '\''^export [XY]='\'''
answer 'export list keeps unset name' \
        'unset X; export X; export -p | grep '\''^export X$'\'''
answer 'set list includes locals' \
        'X=local; set | grep '\''^X='\'' >/dev/null; echo $?'
answer 'env excludes locals' \
        'X=local; env | grep '\''^X='\''; echo $?'
answer 'env includes exports' \
        'export X=public; env | grep '\''^X='\'''
answer 'env follows unset' \
        'export X=public; unset X; env | grep '\''^X='\''; echo $?'
answer 'env sees command prefix' \
        'unset X; X=temp env | grep '\''^X='\''; echo "${X-unset}"'
answer 'export registry churn' \
        'i=0; while [ $i -lt 200 ]; do eval "export V$i=$i"; eval "unset V$i"; i=$((i+1)); done; export X=still; /bin/sh -c '\''echo "$X"'\'''

# eval and . run a line from inside a line that is already running, over the
# same arrays the outer one is standing in. What the inner line claims has to
# be given back to where it claimed from -- giving it back to where the outer
# line began threw the outer line's own words away, and giving nothing back
# ran the tree out after eighty of them.

group nesting
answer 'eval in a loop'  'for i in a b c d; do eval echo x > /dev/null; printf "[%s]" "$i"; done; echo'
answer 'eval keeps the list' 'for i in a b c d; do eval "echo $i" > /dev/null; printf "[%s]" "$i"; done; echo'
answer 'eval many times'  'i=0; while [ $i -lt 200 ]; do eval "x=$i"; i=$((i+1)); done; echo $x'
answer 'eval nested sixty four twice' 'f() { n=$((n-1)); if [ "$n" -eq 0 ]; then echo 64; else eval f; fi; }; n=64; f; n=64; f'
answer 'redefined in a loop' 'i=0; while [ $i -lt 200 ]; do eval "f() { echo body $i; }"; i=$((i+1)); done; f'
answer 'eval under a redirect' 'f() { eval "echo a"; echo b; } > /tmp/gn1.$$; f; echo visible; cat /tmp/gn1.$$'
answer 'dot in a loop'   'echo "echo sourced" > /tmp/gn2.$$; for i in a b c; do . /tmp/gn2.$$ > /dev/null; printf "[%s]" "$i"; done; echo'
answer 'dot nested thirty two twice' 'p=/tmp/nested-dot.$$; printf '\''n=$((n-1)); if [ "$n" -eq 0 ]; then echo 32; else . "$p"; fi\n'\'' > "$p"; n=32; . "$p"; n=32; . "$p"; rm -f "$p"'
eval_sixty_four=$(awk 'BEGIN { for (i = 0; i < 65536; i++) printf "a" }')
answer 'eval sixty four kilobytes' "eval \"x=$eval_sixty_four\"; echo \${#x}"
unset eval_sixty_four
eval_two_megabytes=$(awk 'BEGIN { for (i = 0; i < 2097152; i++) printf "a" }')
answer 'eval two megabytes' "eval \"x=$eval_two_megabytes\"; echo \${#x}"
unset eval_two_megabytes
answer 'dot sixty four kilobytes' 'p=/tmp/large-dot.$$; awk '\''BEGIN { printf "#"; for (i = 0; i < 65536; i++) printf "a"; print ""; print "echo 65536" }'\'' > "$p"; . "$p"; rm -f "$p"'
answer 'dot two megabytes' 'p=/tmp/large-dot.$$; awk '\''BEGIN { printf "#"; for (i = 0; i < 2097152; i++) printf "a"; print ""; print "echo 2097152" }'\'' > "$p"; . "$p"; rm -f "$p"'
answer 'long path command and dot' 'base=/tmp/long-path.$$; d=$base; piece=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv; i=0; while [ "$i" -lt 9 ]; do d=$d/$piece; i=$((i+1)); done; /bin/mkdir -p "$d"; printf '\''#!/bin/sh\necho command\n'\'' > "$d/longcommand"; printf '\''echo sourced\n'\'' > "$d/longsource"; /bin/chmod +x "$d/longcommand"; v=$(PATH=$d command -v longcommand); echo "${v##*/}"; PATH=$d longcommand; PATH=$d; . longsource; /bin/rm -f "$d/longcommand" "$d/longsource"; /bin/rm -rf "$base"'
# A definition written over another gives its space back when it is the last
# one taken, which is the shape a loop makes. Two names taking turns is the
# shape it does not: those run the arena out, and what has to hold then is
# that every definition either stands or is refused -- never a body that was
# copied halfway and recorded as good.
answer 'alternating definitions' 'i=0; while [ $i -lt 300 ]; do eval "f() { echo f; }"; eval "g() { for w in one two; do echo g$w; done; }"; i=$((i+1)); done; f; g'
answer 'a body with a loop' 'i=0; while [ $i -lt 300 ]; do eval "h() { for w in a b c; do echo h$w; done; }"; i=$((i+1)); done; h'
answer 'eval sees the case' 'for i in a b; do case $i in a) eval echo one > /dev/null;; b) eval echo two > /dev/null;; esac; printf "[%s]" "$i"; done; echo'

group builtins
answer 'printf kinds'    'printf "%d %s %c\n" 42 str x'
answer 'printf bases'    'printf "%x %o\n" 255 8'
answer 'printf short'    'printf "%s-%s\n" a'
answer 'printf repeats'  'printf "%s\n" a b c'
answer 'printf nothing'  'printf "%s" abc; echo'
answer 'printf percent'  'printf "100%%\n"'
answer 'echo minus n'    'echo -n hi; echo'
answer 'echo alone'      'echo; echo done'
answer 'test empty'      '[ -z "" ] && echo yes'
answer 'test negated'    '[ ! -z x ] && echo yes'
answer 'test and'        '[ 1 = 1 -a 2 = 2 ] && echo yes'
answer 'test or'         '[ 1 = 2 -o 2 = 2 ] && echo yes'
answer 'test parens'     '[ \( 1 = 1 \) ] && echo yes'
answer 'test file kinds' '[ -f /etc/hostname ] && [ -r / ] && [ -x / ] && echo yes'
answer 'test numbers'    '[ 1 -ne 2 ] && [ 1 -le 1 ] && [ 2 -ge 1 ] && echo yes'
answer 'pwd after cd'    'cd /tmp; pwd'
answer 'cd back'         'cd /tmp; cd /; cd - > /dev/null; pwd'
answer 'cd sets oldpwd'  'cd /tmp; cd /; echo $OLDPWD'
answer 'hash says so'    'hash; echo $?'
answer 'ulimit open files' 'ulimit -n; echo $?'
answer 'export to child' 'export X=1; sh -c "echo \$X"'
answer 'unset unknown'   'unset nosuch; echo $?'
answer 'alias runs'      'alias e=echo; e hi'

#       Aliases, which were one case until the table they live in was rewritten.

answer 'alias listed'    'alias e=echo; alias'
answer 'alias one listed' 'alias e=echo; alias f=printf; alias e'
answer 'alias unknown'   'alias nosuch; echo $?'
answer 'alias redefined' 'alias e=echo; alias e=printf; alias e'
answer 'alias runs redefined' 'alias e=printf; alias e=echo; e hi'
answer 'alias with words' 'alias g="echo one two"; g three'
answer 'alias taken away' 'alias e=echo; unalias e; alias e; echo $?'
answer 'alias middle taken away' 'alias a=echo b=echo c=echo; unalias b; a A; c C; alias b 2>/dev/null; echo $?'
answer 'alias all away'  'alias a=echo b=echo; unalias -a; alias; echo $?'
answer 'alias unknown away' 'unalias nosuch; echo $?'
answer 'alias to a builtin' 'alias c=cd; c /tmp; pwd'
answer 'alias not a word' 'alias e=echo; echo e'
answer 'alias trailing space' 'alias s="echo "; alias e=hi; s e'
answer 'alias in a function' 'alias e=echo; f() { e inside; }; f'
answer 'many aliases'    'i=0; while [ $i -lt 20 ]; do eval "alias a$i=echo"; i=$((i + 1)); done; a7 seven; a19 nineteen'
answer 'one hundred aliases' 'i=0; while [ $i -lt 100 ]; do eval "alias a$i=echo"; i=$((i + 1)); done; alias | wc -l'
answer 'alias redefinition reclaims' 'i=0; while [ $i -lt 300 ]; do eval "alias e=echo-value-$i-padding"; i=$((i + 1)); done; alias e'
answer 'exec replaces'   'exec echo replaced; echo not reached'
answer 'trap listed'     'trap "echo x" EXIT; trap'
answer 'trap rejects a name' 'trap : NOSUCH 2>/dev/null; echo $?'
answer 'trap rejects a large number' 'trap : 999 2>/dev/null; echo $?'
answer 'trap replacement reclaims' 'i=0; while [ $i -lt 100 ]; do trap "echo padding-padding-padding-padding-$i >/dev/null" EXIT; i=$((i + 1)); done; trap "echo final" EXIT'
answer 'wait alone'      'wait; echo $?'
answer 'background wait' 'sleep 0 & wait $!; echo $?'

# set -x, whose output is on standard error and so has to be caught in a file
# to be compared at all. What is traced is what runs: the words after they are
# expanded, in front of PS4, and before the command's own redirections take
# effect so that a command sending its errors away does not send the trace
# with them. A compound command is not traced; the commands inside it are.

group tracing
answer 'trace a command'  '{ set -x; echo hi; } 2>/tmp/gx1.$$; cat /tmp/gx1.$$'
answer 'trace expanded'   'x=ab; { set -x; echo $x$x; } 2>/tmp/gx2.$$; cat /tmp/gx2.$$'
answer 'trace a glob'     'cd /; { set -x; echo /et*; } 2>/tmp/gx3.$$; cat /tmp/gx3.$$'
answer 'trace assignment' '{ set -x; x=1 y=2; } 2>/tmp/gx4.$$; cat /tmp/gx4.$$'
answer 'trace a prefix'   '{ set -x; x=1 true; } 2>/tmp/gx5.$$; cat /tmp/gx5.$$'
answer 'trace a loop'     '{ set -x; for i in a b; do :; done; } 2>/tmp/gx6.$$; cat /tmp/gx6.$$'
answer 'trace a condition' '{ set -x; if true; then :; fi; } 2>/tmp/gx7.$$; cat /tmp/gx7.$$'
answer 'trace a function' 'f() { echo in; }; { set -x; f; } 2>/tmp/gx8.$$; cat /tmp/gx8.$$'
answer 'trace a case'     '{ set -x; case x in x) echo m;; esac; } 2>/tmp/gx9.$$; cat /tmp/gx9.$$'
answer 'trace uses ps4'   '{ set -x; PS4="> "; echo hi; } 2>/tmp/gxa.$$; cat /tmp/gxa.$$'
answer 'trace turned off' '{ set -x; set +x; echo hi; } 2>/tmp/gxb.$$; cat /tmp/gxb.$$'
answer 'trace not the redirect' '{ set -x; echo hi 2>/dev/null; } 2>/tmp/gxc.$$; cat /tmp/gxc.$$'
answer 'trace a subshell' '{ set -x; (echo s); } 2>/tmp/gxd.$$; cat /tmp/gxd.$$'

group globbing
answer 'question mark'   'cd /; echo /et?'
answer 'one of a class'  'cd /; echo /[e]tc'
answer 'no match kept'   'cd /tmp; echo nosuchthing*'
answer 'glob in value'   'cd /; x="/et*"; echo $x'
answer 'glob quoted'     'cd /; echo "/et*"'
answer 'globbing off'    'cd /; set -f; echo /et* ; set +f'
answer 'globbing back on' 'cd /; set -f; set +f; echo /et*'

group substitution
answer 'trailing gone'   'x=$(printf "a\n\n\n"); echo "[$x]"'
answer 'keeps the middle' 'x=$(printf "a\nb\n"); echo "$x"'
answer 'inside a string' 'echo "pre $(echo mid) post"'
answer 'quotes inside'   'echo "$(echo "inner")"'
answer 'quoted sub spaces' 'x="$(echo "one two")"; echo "$x" | wc -w'
answer 'quoted sub tick' 'echo "$(printf %s "a \" b")"'
answer 'status after'    'echo $(false); echo $?'
answer 'backtick nested' 'echo `echo \`echo deep\``'
answer 'two of them'     'x=$(echo a)$(echo b); echo $x'
answer 'inside arith'    'echo $(( $(echo 2) + 3 ))'

#
#       Every byte, including the ones a terminal will not show.
#
#       str() once included the string terminator, so every literal the
#       shell wrote carried a stray NUL after it. A terminal draws nothing
#       for that and no comparison here was looking at the bytes, so it
#       shipped for years. These read the output back through od.
#

group bytes
answer 'echo is three'   'echo hi | wc -c'
answer 'printf is one'   'printf x | wc -c'
answer 'no nul in echo'  'echo hi | tr -d "\0" | wc -c'
answer 'no nul in printf' 'printf "a\nb\n" | tr -d "\0" | wc -c'
answer 'no nul in pwd'   'cd /; pwd | tr -d "\0" | wc -c'
answer 'no nul in a sub' 'x=$(echo hi); printf "%s" "$x" | tr -d "\0" | wc -c'
answer 'no nul in a loop' 'for i in 1 2 3; do echo $i; done | tr -d "\0" | wc -c'
answer 'no nul in type'  'type echo | tr -d "\0" | wc -c'

#       kill, whose reference here is dash's own builtin. Ours is a utility
#       rather than a builtin, so what is being compared is a fork of this
#       shell against a builtin of that one, and they have to agree anyway.

group signals
answer 'signal by number' 'kill -l 9'
answer 'signal fifteen'  'kill -l 15'
answer 'signal from status' 'kill -l 143'
answer 'the whole list'  'kill -l | wc -l'
answer 'the list agrees' 'kill -l | tr "\n" " "'
answer 'nothing to a self' 'kill -0 $$; echo $?'
answer 'no such process' 'kill -0 999999 2>/dev/null; echo $?'
answer 'named signal'    'kill -s TERM 999999 2>/dev/null; echo $?'
answer 'short signal'    'kill -TERM 999999 2>/dev/null; echo $?'
answer 'numbered signal' 'kill -9 999999 2>/dev/null; echo $?'
answer 'no operands'     'kill 2>/dev/null; echo $?'
answer 'signal too high' 'kill -l 65 2>/dev/null; echo $?'
answer 'a name is not a status' 'kill -l TERM 2>/dev/null; echo $?'
answer 'group not signal' 'kill -0 -999999 2>/dev/null; echo $?'
answer 'unknown signal'  'kill -s NOPE 1 2>/dev/null; echo $?'
expected 'SIG prefix option' '1|' 0 'kill -SIGTERM 999999 2>/dev/null; echo $?'
expected 'SIG prefix with s' '1|' 0 'kill -s SIGTERM 999999 2>/dev/null; echo $?'
expected 'SIG prefix invalid' '2|' 0 'kill -SIGNOPE 999999 2>/dev/null; echo $?'
expected 'SIG option resets' '1 1|' 0 \
        'kill -SIGTERM 999999 2>/dev/null; first=$?; kill -0 999999 2>/dev/null; echo "$first $?"'

#       local, whose reference is dash again -- it is not POSIX, and dash is
#       what every script that uses it was written against.
#
#       The last case is the one that matters. The variables live in one
#       block that never gave anything back, so a save and a restore per call
#       filled it and the wrong value came out silently. Six hundred calls is
#       past where that happened.

group local
answer 'saved and put back' 'v=outer; f() { local v=inner; echo $v; }; f; echo $v'
answer 'kept without value' 'v=outer; f() { local v; echo $v; }; f; echo $v'
answer 'made and taken away' 'f() { local v=made; echo $v; }; f; echo "[${v-gone}]"'
answer 'two on one line'  'f() { local v=1 w=2; echo $v$w; }; f; echo "[${v-gone}][${w-gone}]"'
answer 'seen further in'  'v=outer; g() { echo $v; }; f() { local v=inner; g; }; f; echo $v'
answer 'put back on return' 'v=outer; f() { local v=inner; return 3; }; f; echo "$? $v"'
answer 'twice in one call' 'v=outer; f() { local v=1; local v=2; echo $v; }; f; echo $v'
answer 'unset inside'     'v=outer; f() { local v=1; unset v; echo "[${v-gone}]"; }; f; echo $v'
answer 'assigned after'   'v=outer; f() { local v; v=inner; echo $v; }; f; echo $v'
answer 'through recursion' 'f() { local d=$1; [ "$1" -le 0 ] && { echo "at $d"; return; }; f $(($1 - 1)); echo "back $d"; }; f 3'
answer 'a value with a space' 'f() { local v="a b"; echo "[$v]"; }; f'
answer 'six hundred calls' 'f() { local v=$1; local w=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx$1; [ "$v$w" = "${1}xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx$1" ] || echo "broken $1"; }; i=0; while [ $i -lt 600 ]; do f $i; i=$((i + 1)); done; echo "done [${v-unset}]"'

#       Traps for signals, which for a long time were recorded and never run.
#
#       The action goes through the parser, so it cannot be run from the
#       handler -- the handler marks the signal and the action runs where the
#       command it interrupted ends. Every case here is a way of asking
#       whether that boundary is the one dash uses.

group traps
answer 'caught'          'trap "echo caught" INT; kill -INT $$; echo after'
answer 'caught twice'    'trap "echo caught" USR1; kill -USR1 $$; kill -USR1 $$; echo after'
answer 'two commands'    'trap "echo one; echo two" USR1; kill -USR1 $$; echo after'
answer 'ignored'         'trap "" INT; kill -INT $$; echo alive'
answer 'given back'      'trap "echo x" WINCH; trap - WINCH; kill -WINCH $$; echo alive'
answer 'middle trap given back' 'trap "echo one" USR1; trap "echo two" USR2; trap "echo three" TERM; trap - USR2; kill -USR1 $$; kill -TERM $$; echo end'
answer 'status survives' 'trap "true" USR1; false; kill -USR1 $$; echo $?'
answer 'inside a function' 'f() { trap "echo in" USR2; kill -USR2 $$; echo done; }; f; echo after'
answer 'exit from one'   'trap "exit 7" USR1; kill -USR1 $$; echo "not reached"'
answer 'in a loop'       'trap "echo hit" USR1; i=0; while [ $i -lt 3 ]; do kill -USR1 $$; i=$((i + 1)); done; echo done'
answer 'while waiting'   'me=$$; trap "echo got" TERM; ( sleep 1; kill -TERM $me ) & sleep 2; echo after'
answer 'a subshell has none' 'trap "echo parent-hit" USR1; ( trap "echo sub" USR1; kill -USR1 $$; echo subdone ); echo parent'
answer 'listed'          'trap "echo x" USR1; trap'
answer 'listed after ignore' 'trap "" USR1; trap'

#       Signals the shell was handed already ignored.
#
#       POSIX: a non-interactive shell that inherits SIG_IGN keeps ignoring,
#       and a trap on that signal has no effect -- neither setting one nor
#       giving the signal back. It is what makes a command started in the
#       background, or under nohup, stay uninterruptible however it argues
#       with itself about it.

entry_ignored=1
answer 'entry ignored'   'trap "echo caught" INT; kill -INT $$; echo after'
answer 'entry given back' 'trap - INT; kill -INT $$; echo after'
answer 'entry ignored twice' 'trap "" INT; kill -INT $$; echo after'
answer 'entry quit'      'trap "echo q" QUIT; kill -QUIT $$; echo after'
answer 'entry not listed' 'trap "echo x" INT; trap'
answer 'entry no trap'   'kill -INT $$; echo alive'
answer 'entry others run' 'trap "echo hit" USR1; kill -USR1 $$; echo after'
answer 'entry in a function' 'f() { trap "echo in" INT; kill -INT $$; }; f; echo after'
answer 'entry through a child' 'trap "" INT; ( kill -INT $$; echo sub ); echo after'
entry_ignored=""
answer 'exit trap as well' 'trap "echo bye" EXIT; trap "echo hit" USR1; kill -USR1 $$; echo after'
answer 'one pid in a fork' 'a=$$; b=$( echo $$ ); c=$( ( echo $$ ) ); [ "$a" = "$b" ] && [ "$a" = "$c" ] && echo same || echo differs'

#
#       One binary, forty six names.
#
#       Every utility is a function inside the shell reached by the name the
#       binary was called as, so a link is the whole of what /bin/grep is.
#       Nothing tested that the name is what chooses, which is the only thing
#       holding the arrangement up.
#

section named

group dispatch

if [ -n "$names" ] && [ -d "$names" ]; then

emits()
{
        name=$1
        want=$2
        shift 2

        if /bin/sh -c "$*" > "$work/got" 2>/dev/null; then
                emitted_status=0
        else
                emitted_status=$?
        fi

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$want" ]; then
                won
                return 0
        fi

        lost "$name" "expected $want, got ${got_ours}[$emitted_status]"
}

emits 'called grep'      'alpha|'  "printf 'alpha\nbeta\n' | '$names/grep' alpha"
emits 'called wc'        '2|'      "printf 'a\nb\n' | '$names/wc' -l | tr -d ' '"
emits 'called rev'       'cba|'    "printf 'abc\n' | '$names/rev'"
emits 'called basename'  'c|'      "'$names/basename' /a/b/c"
emits 'called seq'       '1|2|3|'  "'$names/seq' 3"
emits 'called uname'     'Linux|'  "'$names/uname'"
emits 'called expr'      '2|'      "'$names/expr' 1 + 1"
emits 'called cmp'       '0|'      "'$names/cmp' -s /etc/hostname /etc/hostname; echo \$?"
emits 'called mktemp'    '0|'      "d=\$('$names/mktemp' -d) && test -d \"\$d\" && rmdir \"\$d\"; echo \$?"
emits 'called kill'      '0|'      "'$names/kill' -0 \$\$; echo \$?"
emits 'called date'      '2001-09-09|' "TZ=UTC0 '$names/date' -d @1000000000 +%F"
emits 'called xargs'     'a b|'    "printf 'a\\nb\\n' | '$names/xargs' echo"
emits 'kill ends it'     'gone|'   "sleep 30 & p=\$!; '$names/kill' \$p; wait \$p 2>/dev/null; echo gone"
emits 'through a dot'    'cba|'    "printf 'abc\n' | '$names/./rev'"
emits 'link elsewhere'   'cba|'    "mkdir -p /tmp/sn && ln -sf '$names/rev' /tmp/sn/rev && printf 'abc\n' | /tmp/sn/rev"
emits 'another name'     'hi|'     "ln -sf '$names/rev' /tmp/notatool && printf 'echo hi\n' | /tmp/notatool"
emits 'the shell itself' 'hi|'     "printf 'echo hi\n' | '$subject'"
emits 'bare with no path' 'alpha|' "printf 'PATH=\necho alpha | grep alpha\n' | '$subject'"
emits 'expr with no path' '2|'    "printf 'PATH=\nexpr 1 + 1\n' | '$subject'"
emits 'type says utility' '0|'     "printf 'type grep > /dev/null; echo \$?\n' | '$subject'"
emits 'command v finds'  '0|'      "printf 'PATH=\ncommand -v grep > /dev/null; echo \$?\n' | '$subject'"
emits 'which finds'      '0|'      "printf 'PATH=\nwhich grep > /dev/null; echo \$?\n' | '$subject'"
emits 'help lists them'  '0|'      "printf 'help > /dev/null; echo \$?\n' | '$subject'"

fi

#
#       Where ours and dash part company.
#
#       Each of these is a thing the shell does not do yet, recorded as what
#       it does instead. A case here failing means the answer moved: either
#       the gap closed, in which case it belongs above, or something else
#       changed and nobody meant it to.
#

section differs

group status
answer 'negative exit rejected' 'exit -1'
answer 'nonnumeric exit rejected' 'exit nope'
answer 'large exit wraps' 'exit 999'

group arithmetic
answer 'empty arithmetic rejected' 'echo $(( ))'
answer 'comma rejected' 'echo $((1,2))'
answer 'post increment rejected' 'x=1; echo $((x++)) $x'
answer 'post decrement rejected' 'x=1; echo $((x--)) $x'
answer 'bad octal rejected' 'echo $((08))'
answer 'bad hex rejected' 'echo $((0x))'

group language
answer 'echo backspace escape' 'echo "a\b" | od -An -tu1'
answer 'echo newline escape' 'echo "a\nb"'
answer 'echo unknown escape' 'echo "a\q"'
answer 'echo cut escape' 'echo "a\cdiscard"; echo kept'
answer 'local outside function is fatal' 'local v=1 2>/dev/null; echo $?; echo after'
differs 'expr is sixty four' '-9223372036854775808|' 0 'expr 9223372036854775807 + 1'

# The expanded form of a here-document body is built in the storage a command
# line shares, which holds eight kilobytes. A body that outgrows it is refused
# rather than handed over with its end missing, which is what used to happen.
# A here-document is as long as it is. This used to stop at the eight kilobyte
# mark and hand the truncated body on without saying so.
answer 'no here-document ceiling' '{ echo "cat <<END"; i=0; while [ $i -lt 400 ]; do echo "line $i padding padding padding"; i=$((i+1)); done; echo END; } > /tmp/gh5.$$
. /tmp/gh5.$$
rm -f /tmp/gh5.$$'

# Reader storage is movable and grows with the source. These cross each old
# parser boundary independently: document count, body bytes, delimiter bytes,
# and the pending token assembled from multiple physical lines.
many_heredocs=$(awk 'BEGIN {
        for (i = 0; i < 40; i++) printf "%scat <<E%d", i ? "; " : "", i
        printf "\n"
        for (i = 0; i < 40; i++) printf "%d\nE%d\n", i, i
}')
answer 'forty here-documents' "$many_heredocs"

large_heredoc=$(awk 'BEGIN {
        print "wc -c <<END"
        for (i = 0; i < 4000; i++) print "0123456789 padding line"
        print "END"
}')
answer 'here-document over sixty four KiB' "$large_heredoc"

long_delimiter=$(awk 'BEGIN { for (i = 0; i < 5000; i++) printf "D" }')
answer 'five thousand byte delimiter' "wc -c <<$long_delimiter
payload
$long_delimiter"

continued_quote=$(awk 'BEGIN {
        printf "printf \047%%s\047 \042"
        for (i = 0; i < 160; i++) {
                for (j = 0; j < 80; j++) printf "q"
                printf "\n"
        }
        print "\042 | wc -c"
}')
answer 'continued quote over eight KiB' "$continued_quote"

continued_backslash=$(awk 'BEGIN {
        printf "printf \047%%s\047 "
        for (i = 0; i < 160; i++) {
                for (j = 0; j < 80; j++) printf "b"
                printf "\\\n"
        }
        print "z | wc -c"
}')
answer 'backslash continuation over eight KiB' "$continued_backslash"

# Empty physical lines must still reach the parser: inside a here-document,
# they are data rather than ignorable command lines.
answer 'heredoc blank' 'cat <<EOF
a

b
EOF'

# A case pattern is expanded by shell_expand_word, which hands back bytes and
# not the marks that say which of them were quoted -- so the star that came
# out of "$p" is a star to the matcher as much as to the eye.
answer 'quoted pattern is literal' "p='a*'; case aXX in \"\$p\") echo yes;; *) echo no;; esac"
answer 'escaped pattern is literal' 'case aXb in a\*b) echo yes;; *) echo no;; esac'
answer 'unquoted variable pattern' "p='a*'; case aXX in \$p) echo yes;; *) echo no;; esac"
answer 'quoted question is literal' 'case aXb in "a?b") echo yes;; *) echo no;; esac'
answer 'escaped bracket is literal' 'case "a[b" in a\[b) echo yes;; *) echo no;; esac'

# The structural ceilings that remain are explicit. A function goes as deep
# as the table its locals sit in, which is a hundred and twenty eight calls.
# Empty quoted arguments must remain distinct words through builtin dispatch.
answer 'echo an empty word' 'echo "" x'

# A line the language is not finished with waits for the rest of it. At EOF,
# missing grammar is a syntax error rather than a successful empty parse.
answer 'case never closed' 'case x in x) echo m esac'
answer 'twice negated' '! ! true; echo $?'


# MAX_TOKENS in shell.c is what a command line holds, so a glob that matches
# more than that many names is cut off rather than refused.
# A glob is as long as the directory is. This used to stop at sixty three
# words, silently, which is the worst way for it to be wrong: the script goes
# on believing it saw everything. Two hundred is enough to have caught it and
# small enough not to slow the lane down.
answer 'no word ceiling' 'cd "$(mktemp -d)"; i=0; while [ $i -lt 200 ]; do : > f$i; i=$((i+1)); done; echo * | wc -w'
answer 'no field ceiling' 'cd "$(mktemp -d)"; i=0; while [ $i -lt 200 ]; do : > f$i; i=$((i+1)); done; set -- *; echo $#'
answer 'no parameter ceiling' 'cd "$(mktemp -d)"; i=0; while [ $i -lt 200 ]; do : > f$i; i=$((i+1)); done; set -- *; n=0; for f in "$@"; do n=$((n+1)); done; echo $n'

group language-errors
answer 'set rejects unknown option' 'set -Z; echo after'
answer 'nounset stops the shell' 'set -u; echo $nosuch; echo after'
answer 'readonly reassignment stops the shell' 'readonly r=1; r=2; echo $?'
answer 'readonly unset stops the shell' 'readonly r=1; unset r; echo after'
answer 'readonly export assignment stops' 'readonly r=1; export r=2; echo after'
answer 'export rejects digit name' 'export 1bad=value; echo after'
answer 'export rejects punctuated name' 'export bad-name=value; echo after'
answer 'readonly rejects digit name' 'readonly 1bad=value; echo after'
answer 'readonly rejects punctuated name' 'readonly bad-name=value; echo after'
answer 'unset rejects digit name' 'unset 1bad; echo after'
answer 'unset rejects punctuated name' 'unset bad-name; echo after'
answer 'export option end' 'export -- named=value; echo "$named"'
answer 'readonly option end' 'readonly -- named=value; echo "$named"'
answer 'unset option end' 'named=value; unset -- named; echo "${named-unset}"'
answer 'unset function option' 'f() { echo a; }; unset -f f; f 2>/dev/null; echo $?'
answer 'shift past end is fatal' 'set -- a; shift 2; echo $?'
answer 'shift rejects a word' 'set -- a; shift nope; echo after'
answer 'shift rejects negative' 'set -- a; shift -1; echo after'
answer 'return rejects a word' 'f() { return nope; echo BAD; }; f; echo after'
answer 'return rejects negative' 'f() { return -1; echo BAD; }; f; echo after'
answer 'return rejects overflow' 'f() { return 2147483648; echo BAD; }; f; echo after'
answer 'return rejects huge number' 'f() { return 999999999999999999999999999999999999999999999999999999999999; echo BAD; }; f; echo after'
answer 'break rejects a word' 'while true; do break nope; echo BAD; done; echo after'
answer 'break rejects zero' 'while true; do break 0; echo BAD; done; echo after'
answer 'break rejects huge number' 'while true; do break 999999999999999999999999999999999999999999999999999999999999; echo BAD; done; echo after'
answer 'continue rejects a word' 'while true; do continue nope; echo BAD; done; echo after'
answer 'continue rejects zero' 'while true; do continue 0; echo BAD; done; echo after'
answer 'continue rejects huge number' 'while true; do continue 999999999999999999999999999999999999999999999999999999999999; echo BAD; done; echo after'
answer 'missing input status' 'cat < /nonexistent12345; echo $?'
answer 'closed output status' 'echo x >&- 2>/dev/null; echo $?'

many_readonly=$(awk 'BEGIN { for (i = 0; i < 40; i++) printf "readonly R%02d=%d;", i, i }')
answer 'forty readonly names' "$many_readonly readonly -p | grep '^readonly R' | wc -l"

# PID 1 may receive an empty environment, but an ordinary shell must keep the
# one its caller supplied. HOME is a representative variable that make,
# system(), and login shells routinely depend on.
answer 'inherits environment' '[ -n "$HOME" ] && echo set || echo unset'

# A line ending in the middle of a quote waits for more physical input. EOF
# finalizes the reader and turns any still-open construct into a syntax error.
answer 'quote never closed' 'echo "open'
answer 'a bare semicolon' ';'
answer 'backslash at the end' 'echo one\'

# Nested readers must finalize independently: unfinished eval or sourced input
# is a fatal syntax error and cannot leak into the outer parser.
answer 'eval unfinished' "eval 'echo \"unclosed'
echo second"
answer 'dot unfinished' 'f=/tmp/syntax-dot.$$
printf '\''echo "unclosed\n'\'' > "$f"
. "$f"
echo second'

#
#       Bash parity ledger.
#
#       Every named family is either compared directly with Bash as supported
#       or pinned to its exact current unsupported result. This is deliberately
#       redundant with deeper cases above: it is the machine-readable summary
#       that prevents a command-not-found, syntax error, or plausible unchanged
#       value from being mistaken for broad Bash compatibility.
#

section bash-ledger

group supported
bash_answer 'ledger parameter replace' 'x=aba; echo "${x//a/X}"'
bash_answer 'ledger substring' 'x=abcdef; echo "${x:1:3}"'
bash_answer 'ledger case conversion' 'x=aBc; echo "${x^^}"'
bash_answer 'case conversion below bulk boundary' \
        'x=abcdefghijklmnopqrstuvwxyzABCDE; printf "<%s><%s>\n" "${x^^}" "${x,,}"'
bash_answer 'case conversion at bulk boundary' \
        'x=abcdefghijklmnopqrstuvwxyzABCDEF; printf "<%s><%s>\n" "${x^^}" "${x,,}"'
bash_answer 'case conversion above bulk boundary' \
        'x=abcdefghijklmnopqrstuvwxyzABCDEFG; printf "<%s><%s>\n" "${x^^}" "${x,,}"'
bash_answer 'case conversion explicit pattern stays selective' \
        'x=abcXYZabcXYZabcXYZabcXYZabcXYZabcXYZ; printf "<%s>\n" "${x^^[a-c]}"'
bash_answer 'ledger brace expansion' 'printf "[%s]" {a,b}{1,2}; echo'
bash_answer 'ledger here string' 'cat <<< "a b"'
bash_answer 'ledger function keyword' 'function f { echo yes; }; f'
bash_answer 'ledger append assignment' 'x=a; x+=b; echo "$x"'
bash_answer 'ledger pipefail' 'set -o pipefail; false | true; echo $?'
bash_answer 'ledger both append' 'p=/tmp/bash-ledger.$$; echo a > "$p"; { echo b; echo c >&2; } &>> "$p"; cat "$p"; rm "$p"'
bash_answer 'ledger overflowing io number is a word' 'p=/tmp/bash-io-over.$$; echo marker 999999999999999999999>"$p"; cat "$p"; rm "$p"'
bash_answer 'ledger source alias' 'p=/tmp/bash-source.$$; printf "echo sourced\n" > "$p"; source "$p"; rm "$p"'
bash_answer 'ledger arithmetic command' 'x=0; ((x+=2)); echo "$x"'
bash_answer 'ledger c style for' 'for ((i=0;i<2;i++)); do echo "$i"; done'
bash_answer 'ledger double brackets' '[[ x == x ]]'
bash_answer 'ledger regex match' '[[ abc =~ ^a ]]'
bash_answer 'ledger let' 'x=0; let x+=2 x*=3; a=$?; let x-=6; printf "%s:%s:%s\n" "$x" "$a" "$?"'
bash_answer 'let quoted expression' 'let "x = 2 + 3" "x == 5"; printf "%s:%s\n" "$x" "$?"'
bash_answer 'let empty and invalid' 'let 2>/dev/null; a=$?; let "1 +" 2>/dev/null; printf "%s:%s\n" "$a" "$?"'

group remaining
bash_remaining 'ledger regex captures' '' 2 '[[ abc =~ ^(a)(b) ]]; echo "${BASH_REMATCH[0]}:${BASH_REMATCH[1]}:${BASH_REMATCH[2]}"'
bash_remaining 'ledger indexed arrays' '' 2 'a=(one two); echo "${a[1]}"'
bash_remaining 'ledger associative arrays' '' 2 'declare -A a; a[k]=v; echo "${a[k]}"'
bash_remaining 'ledger process substitution' '<>|' 0 'x=$(cat <(printf x)); printf "<%s>\n" "$x"'
bash_remaining 'ledger extglob' '' 2 'shopt -s extglob; eval '\''case aa in +(a)) echo yes;; esac'\'''
bash_remaining 'ledger globstar' '' 127 'shopt -s globstar'
bash_remaining 'ledger declare' '' 127 'declare x=1'
bash_remaining 'ledger typeset' '' 127 'typeset x=1'
bash_remaining 'ledger mapfile' '' 127 'printf "a\n" | mapfile a'
bash_remaining 'ledger readarray' '' 127 'printf "a\n" | readarray a'
bash_remaining 'ledger shopt' '' 127 'shopt -s nullglob'
bash_remaining 'ledger select' '' 2 'select x in a; do echo "$x"; break; done </dev/null'
bash_remaining 'ledger coproc' '' 2 'coproc C { echo x; }'
bash_remaining 'ledger noclobber status' '2|' 0 'p=/tmp/bash-noclobber.$$; echo a > "$p"; set -C; echo b > "$p"; s=$?; rm -f "$p"; echo "$s"'
bash_remaining 'ledger indirection' '' 2 'x=y; y=value; echo "${!x}"'

#
#       What the shell has no answer for at all.
#
#       PATH is emptied first, so these ask whether the shell carries the
#       thing rather than whether this machine has one lying about.
#

section absent

group missing
absent 'tar'      '127|' 'tar --help; echo $?'
absent 'gzip'     '127|' 'gzip --help; echo $?'

section ""

total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
