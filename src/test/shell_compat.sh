#!/bin/sh
#
#       Invocation-mode and state compatibility for the one shell binary.
#
#           sh src/test/shell_compat.sh [path-to-moonwater-shell]
#
#       A bash-named link is compared with Bash. The default, sh and dash
#       names are each compared with dash. This is intentionally separate
#       from shell.sh: most cases here ask whether startup argv or argv[0]
#       selected the right policy before the shared parser begins.
#
set -u

subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x /bin/bash ] || { echo "no /bin/bash" >&2; exit 1; }
[ -x /bin/dash ] || { echo "no /bin/dash" >&2; exit 1; }

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" || exit 1
ln -s "$subject" "$work/names/bash" || exit 1
ln -s "$subject" "$work/names/sh" || exit 1
ln -s "$subject" "$work/names/dash" || exit 1
ln -s "$subject" "$work/names/moonwater" || exit 1

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

shown()
{
        head -c 80 "$1" | tr '\n' '|'
}

capture()
{
        capture_shell=$1
        capture_tag=$2
        shift 2

        if timeout 5 "$capture_shell" "$@" < "$work/input" \
                > "$work/$capture_tag.out" 2> "$work/$capture_tag.err"; then
                capture_status=0
        else
                capture_status=$?
        fi

        case $capture_tag in
        want) want_status=$capture_status ;;
        got) got_status=$capture_status ;;
        esac
}

same_result()
{
        case_name=$1
        diagnostic=${2:-exact}

        if ! cmp -s "$work/want.out" "$work/got.out" ||
                [ "$want_status" != "$got_status" ]; then
                lost "$case_name" \
                        "want $(shown "$work/want.out")[$want_status], got $(shown "$work/got.out")[$got_status]"
                return
        fi

        case $diagnostic in
        exact)
                if ! cmp -s "$work/want.err" "$work/got.err"; then
                        lost "$case_name" \
                                "stderr want $(shown "$work/want.err"), got $(shown "$work/got.err")"
                        return
                fi
                ;;
        mentions:*)
                diagnostic_word=${diagnostic#mentions:}
                if ! grep -F -- "$diagnostic_word" "$work/want.err" >/dev/null ||
                        ! grep -F -- "$diagnostic_word" "$work/got.err" >/dev/null; then
                        lost "$case_name" \
                                "diagnostic must mention $diagnostic_word; want $(shown "$work/want.err"), got $(shown "$work/got.err")"
                        return
                fi
                ;;
        ignore)
                # Forced interactive shells write terminal/job-control
                # decoration that depends on whether the harness owns a tty.
                # The caller must use this only when stdout and status carry
                # the option behavior under test.
                ;;
        *)
                lost "$case_name" "bad diagnostic policy $diagnostic"
                return
                ;;
        esac

        won
}

language_compare()
{
        language_reference=$1
        language_subject=$2
        language_name=$3
        language_script=$4

        printf '%s\n' "$language_script" > "$work/input"
        capture "$language_reference" want
        capture "$language_subject" got
        same_result "$language_name" exact
}

bash_case()
{
        language_compare /bin/bash "$work/names/bash" "$1" "$2"
}

posix_case()
{
        posix_name=$1
        posix_script=$2

        language_compare /bin/dash "$work/names/moonwater" \
                "$posix_name [moonwater]" "$posix_script"
        language_compare /bin/dash "$work/names/sh" \
                "$posix_name [sh]" "$posix_script"
        language_compare /bin/dash "$work/names/dash" \
                "$posix_name [dash]" "$posix_script"
}

startup_compare()
{
        startup_reference=$1
        startup_subject=$2
        startup_name=$3
        startup_input=$4
        startup_diagnostic=$5
        shift 5

        printf '%s' "$startup_input" > "$work/input"
        capture "$startup_reference" want "$@"
        capture "$startup_subject" got "$@"
        same_result "$startup_name" "$startup_diagnostic"
}

bash_startup()
{
        name=$1
        input=$2
        diagnostic=$3
        shift 3
        startup_compare /bin/bash "$work/names/bash" "$name" "$input" \
                "$diagnostic" "$@"
}

posix_startup()
{
        name=$1
        input=$2
        diagnostic=$3
        shift 3

        startup_compare /bin/dash "$work/names/moonwater" \
                "$name [moonwater]" "$input" "$diagnostic" "$@"
        startup_compare /bin/dash "$work/names/sh" \
                "$name [sh]" "$input" "$diagnostic" "$@"
        startup_compare /bin/dash "$work/names/dash" \
                "$name [dash]" "$input" "$diagnostic" "$@"
}

monitor_pty_case()
{
        if command -v python3 >/dev/null 2>&1 &&
                timeout 12 python3 - /bin/bash "$work/names/bash" <<'PY'
import fcntl
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time

script = (
    "/usr/bin/python3 -c 'import os; "
    "print(\"FG\" if os.tcgetpgrp(0)==os.getpgrp() else \"BG\")'; :"
)


def run(shell):
    master, slave = pty.openpty()

    def session():
        os.setsid()
        fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    process = subprocess.Popen(
        [shell, "-i", "-m", "-c", script],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
        preexec_fn=session,
    )
    os.close(slave)
    output = bytearray()
    deadline = time.monotonic() + 5

    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.1)
        if ready:
            try:
                output.extend(os.read(master, 4096))
            except OSError:
                break
        if process.poll() is not None:
            break

    if process.poll() is None:
        os.killpg(process.pid, signal.SIGKILL)
    status = process.wait()
    os.close(master)
    return status, bytes(output).replace(b"\r", b"")


want = run(sys.argv[1])
got = run(sys.argv[2])
if want != (0, b"FG\n") or got != want:
    print("PTY monitor: want %r, got %r" % (want, got), file=sys.stderr)
    raise SystemExit(1)
PY
        then
                won
        else
                lost 'explicit -i -m owns PTY' \
                        'foreground child did not own the controlling terminal'
        fi
}

group startup
section startup

bash_startup 'combined -ec' '' exact -ec \
        'printf "before\n"; false; printf "after\n"'
bash_startup 'combined -ce' '' exact -ce \
        'printf "before\n"; false; printf "after\n"'
bash_startup '-ce trailing operands' '' exact -ce \
        'printf "%s:%s\n" "$0" "$1"; false' -x argument
bash_startup '-ce missing command status' '' mentions:-c -ce -x
bash_startup 'combined -eu -c' '' mentions:missing -eu -c \
        'printf "%s\n" "$missing"; printf "after\n"'
bash_startup 'startup pipefail' '' exact -o pipefail -c \
        'false | true; echo $?'
bash_startup 'startup noexec' '' exact -n -c \
        'printf "must-not-run\n"'
bash_startup 'literal -nc false' '' exact -nc false
bash_startup '-c option terminator' '' exact -c -- \
        'printf "command-after-terminator\n"'
bash_startup 'missing -c operand' '' mentions:-c -c
bash_startup 'unknown startup flag' '' mentions:-Z -Z
bash_startup 'plus option disables' '' exact -e +e -c \
        'false; printf "after\n"'
bash_startup 'stdin -s operands' 'printf "%s:%s\n" "$1" "$2"
' exact -s -- one two
bash_startup 'plus-s still selects stdin' 'printf "<%s>\n" "$1"
' exact +s operand
bash_startup '-c name operands' '' exact -c \
        'printf "%s:%s\n" "$0" "$1"' named one

printf 'printf "end-options\n"\n' > "$work/-script"
chmod +x "$work/-script"
bash_startup 'option terminator' '' exact -- "$work/-script"

posix_startup 'combined -ec' '' exact -ec \
        'printf "before\n"; false; printf "after\n"'
posix_startup 'combined -ce' '' exact -ce \
        'printf "before\n"; false; printf "after\n"'
posix_startup '-ce trailing operands' '' exact -ce \
        'printf "%s:%s\n" "$0" "$1"; false' -x argument
posix_startup 'combined -eu -c' '' mentions:missing -eu -c \
        'printf "%s\n" "$missing"; printf "after\n"'
posix_startup 'startup noexec' '' exact -n -c \
        'printf "must-not-run\n"'
posix_startup 'literal -nc false' '' exact -nc false
posix_startup '-c option terminator' '' exact -c -- \
        'printf "command-after-terminator\n"'
posix_startup 'missing -c operand' '' mentions:-c -c
posix_startup 'unknown startup flag' '' mentions:-Z -Z
posix_startup 'reject Bash extra letter' '' mentions:-B -B -c ':'
posix_startup 'plus option disables' '' exact -e +e -c \
        'false; printf "after\n"'
posix_startup 'stdin -s operands' 'printf "%s:%s\n" "$1" "$2"
' exact -s -- one two
posix_startup '-c name operands' '' exact -c \
        'printf "%s:%s\n" "$0" "$1"' named one
posix_startup 'option terminator' '' exact -- "$work/-script"

bash_startup 'interactive +m precedence' '' ignore -i +m -c \
        'case $- in *m*) echo m-on;; *) echo m-off;; esac'
posix_startup 'interactive +m precedence' '' ignore -i +m -c \
        'case $- in *m*) echo m-on;; *) echo m-off;; esac'
bash_startup 'interactive without tty has no monitor' '' ignore -i -c \
        'case $- in *m*) echo m-on;; *) echo m-off;; esac'
posix_startup 'interactive without tty has no monitor' '' ignore -i -c \
        'case $- in *m*) echo m-on;; *) echo m-off;; esac'
monitor_pty_case

group mode
section mode

bash_case 'bash identity' \
        '[ -n "${BASH_VERSION+x}" ] && shopt -q cmdhist; echo $?'
bash_case 'set -n same parsed line' \
        'set -n; printf "must-not-run\n"; set +n; printf "still-must-not-run\n"'
bash_case 'braceexpand B toggle' \
        'set +B; echo {a,b}; set -B; echo {a,b}; case $- in *B*) echo B-on;; *) echo B-off;; esac'

mkdir "$work/hash-a" "$work/hash-b" || exit 1
printf '#!/bin/sh\necho one\n' > "$work/hash-a/pick"
printf '#!/bin/sh\necho two\n' > "$work/hash-b/pick"
chmod +x "$work/hash-a/pick" "$work/hash-b/pick"
MW_COMPAT_HASH_A=$work/hash-a
MW_COMPAT_HASH_B=$work/hash-b
export MW_COMPAT_HASH_A MW_COMPAT_HASH_B
bash_case 'hashall h toggle' \
        'PATH="$MW_COMPAT_HASH_A:/usr/bin"; hash -p "$MW_COMPAT_HASH_B/pick" pick; set +h; pick; set -h; hash -p "$MW_COMPAT_HASH_B/pick" pick; pick; case $- in *h*) echo h-on;; *) echo h-off;; esac'
bash_case 'flag cache follows mutations' \
        'printf "<%s>\n" "$-"; set -eu; printf "<%s>\n" "$-"; set +e; set +B; printf "<%s>\n" "$-"; set -B; printf "<%s>\n" "$-"'
posix_case 'non-bash identity' \
        '[ -z "${BASH_VERSION+x}" ]; echo $?'
posix_case 'set -n same parsed line' \
        'set -n; printf "must-not-run\n"; set +n; printf "still-must-not-run\n"'
posix_case 'reject Bash set letter' \
        '(set -B) 2>/dev/null; echo $?'
posix_case 'flag cache follows mutations' \
        'printf "<%s>\n" "$-"; set -eu; printf "<%s>\n" "$-"; set +e; printf "<%s>\n" "$-"'

group pipeline
section pipeline

bash_case 'default pipeline isolates state' \
        'v=old; printf "new\n" | read v; printf "%s\n" "$v"'
bash_case 'lastpipe keeps final state' \
        'shopt -s lastpipe; set +m; v=old; printf "new\n" | read v; s=$?; printf "%s:%s\n" "$v" "$s"'
bash_case 'PIPESTATUS immediate' \
        '(exit 3) | (exit 7) | true; printf "%s:%s:%s\n" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}" "${PIPESTATUS[2]}"'
bash_case 'PIPESTATUS reset by assignment' \
        '(exit 3) | (exit 7) | true; s=$?; printf "%s:%s:%s:%s\n" "$s" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}" "${PIPESTATUS[2]}"'
bash_case 'pipefail rightmost failure' \
        'set -o pipefail; (exit 3) | (exit 7) | true; echo $?'
bash_case 'negated pipefail status' \
        'set -o pipefail; ! false | true; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'
bash_case 'pipefail redirect status' \
        'set -o pipefail; { true | cat < /no/moonwater-compat-file; } 2>/dev/null; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'
bash_case 'lastpipe readonly status' \
        'shopt -s lastpipe; set +m; readonly v=old; printf "new\n" | read v 2>/dev/null; printf "%s:%s\n" "$?" "$v"'
posix_case 'pipeline isolates state' \
        'v=old; printf "new\n" | read v; printf "%s\n" "$v"'

group readonly
section readonly

bash_case 'local readonly lifetime' \
        'x=outer; f() { local -r x=inner; printf "in:%s\n" "$x"; }; f; x=changed; printf "out:%s:%s\n" "$x" "$?"'
bash_case 'readonly local failure status' \
        'x=outer; (f() { local x=inner; readonly x; x=bad; echo no; }; f) 2>/dev/null; printf "%s:%s\n" "$?" "$x"'
bash_case 'readonly local does not escape' \
        'f() { local item=inside; readonly item; }; f; item=outside; printf "%s:%s\n" "$item" "$?"'
posix_case 'local readonly lifetime' \
        'x=outer; f() { local x=inner; readonly x; printf "in:%s\n" "$x"; }; f; x=changed; printf "out:%s:%s\n" "$x" "$?"'
posix_case 'readonly refusal status' \
        'x=outer; (readonly x; x=bad; echo no) 2>/dev/null; printf "%s:%s\n" "$?" "$x"'

group expansion
section expansion

bash_case 'IFS nonblank edge fields' \
        'IFS=:; v=":a::b:"; set -- $v; printf "%s" "$#"; printf "<%s>" "$@"; echo'
bash_case 'IFS mixed blank fields' \
        'IFS=" :"; v="  a:: b : "; set -- $v; printf "%s" "$#"; printf "<%s>" "$@"; echo'
bash_case 'quoted at splices endpoints' \
        'set --; for x in pre"$@"post; do printf "<%s>" "$x"; done; echo; set -- "" x; for x in pre"$@"post; do printf "<%s>" "$x"; done; echo'
bash_case 'star empty and unset IFS' \
        'set -- a b; IFS=; printf "<%s>\n" "$*"; unset IFS; printf "<%s>\n" "$*"'
bash_case 'assignment suppresses split and glob' \
        'v="x y *"; a=$v; printf "<%s>\n" "$a"'
bash_case 'command substitution trims newlines' \
        'v=$(printf "a\n\n\n"); printf "<%s>:%s\n" "$v" "${#v}"'
bash_case 'associative whitespace subscript' \
        'declare -A m; m[a b]=one; m[a[b]]=two; printf "%s:%s\n" "${m[a b]}" "${m[a[b]]}"'

posix_case 'IFS nonblank edge fields' \
        'IFS=:; v=":a::b:"; set -- $v; printf "%s" "$#"; printf "<%s>" "$@"; echo'
posix_case 'IFS mixed blank fields' \
        'IFS=" :"; v="  a:: b : "; set -- $v; printf "%s" "$#"; printf "<%s>" "$@"; echo'
posix_case 'quoted at splices endpoints' \
        'set --; for x in pre"$@"post; do printf "<%s>" "$x"; done; echo; set -- "" x; for x in pre"$@"post; do printf "<%s>" "$x"; done; echo'
posix_case 'star empty and unset IFS' \
        'set -- a b; IFS=; printf "<%s>\n" "$*"; unset IFS; printf "<%s>\n" "$*"'
posix_case 'assignment suppresses split and glob' \
        'v="x y *"; a=$v; printf "<%s>\n" "$a"'
posix_case 'command substitution trims newlines' \
        'v=$(printf "a\n\n\n"); printf "<%s>:%s\n" "$v" "${#v}"'

group parse
section parse

bash_case 'nested heredoc in substitution' 'v=$(cat <<-EOF
	one $((1+1))
	EOF
); printf "<%s>\n" "$v"'
bash_case 'continued and-or and pipeline' 'printf "a\n" &&
printf "b\n" |
cat'
bash_case 'escaped case pattern' \
        'x="a b"; case $x in a\ b) echo yes;; *) echo no;; esac'

posix_case 'nested heredoc in substitution' 'v=$(cat <<-EOF
	one $((1+1))
	EOF
); printf "<%s>\n" "$v"'
posix_case 'continued and-or and pipeline' 'printf "a\n" &&
printf "b\n" |
cat'
posix_case 'escaped case pattern' \
        'x="a b"; case $x in a\ b) echo yes;; *) echo no;; esac'
posix_case 'errexit tested contexts' \
        'set -e; false && echo bad; if false; then echo bad; fi; ! true; echo after'
posix_case 'assignment substitution status' \
        'value=$(exit 7); echo $?'

section
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
