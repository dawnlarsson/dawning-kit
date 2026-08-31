#!/bin/sh
#
#       The file utilities against the system's own, diffed.
#
#           sh src/test/files.sh [directory holding our binaries]
#
#       Three kinds of case, because three kinds of thing are being checked
#       and pretending they are one kind is how a comparison gets rigged:
#
#         same    the two tools' output has to match byte for byte.
#         effect  the two tools are run on twin trees and what is left behind
#                 has to match -- names, permissions, link targets, contents.
#                 chmod and mv and rm print nothing worth diffing; what they
#                 did is the whole of what they are.
#         near    the output is compared after a named normalisation, and the
#                 normalisation is printed so it can be argued with. ls -l
#                 chooses its date format on a six month recency rule, stat's
#                 default block was a layout rather than an answer. All
#                 three turned out to be matchable in the end and are
#                 compared whole; what is left under near is find and du,
#                 whose output is in readdir order and is sorted first, and
#                 the two cases named where the tools genuinely disturb what
#                 they are measuring.
#
#       Everything of ours prints times in UTC, so the system tool is run
#       with TZ set to UTC as well; a comparison against Europe/Stockholm
#       would be measuring the timezone database and not the utility.
#
set -e

binaries=${1:-/tmp/multi}
export TZ=UTC0

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

started=$(date +%s)

pass=0
fail=0
current=""

group() { current=$1; }

report() {
        if [ "$1" = ok ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-10s %-30s %s\n' "$current" "$2" "$3"
}

# Byte for byte, both tools given the same words.
same() {
        name=$1
        tool=$2
        shift 2

        "$tool" "$@" > "$work/want" 2> "$work/want.err" || true
        "$binaries/$tool" "$@" > "$work/got" 2> "$work/got.err" || true

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 60 "$work/want" | tr '\n' '|')] got [$(head -c 60 "$work/got" | tr '\n' '|')]"
}

# Byte for byte, but the output of both is passed through a filter first.
near() {
        name=$1
        filter=$2
        tool=$3
        shift 3

        "$tool" "$@" 2>/dev/null | eval "$filter" > "$work/want" || true
        "$binaries/$tool" "$@" 2>/dev/null | eval "$filter" > "$work/got" || true

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 60 "$work/want" | tr '\n' '|')] got [$(head -c 60 "$work/got" | tr '\n' '|')]"
}

# Both channels and the exit status, for the cases where what a tool says
# about something it could not do is the whole of the answer.
spoken() {
        name=$1
        tool=$2
        shift 2

        if "$tool" "$@" > "$work/want" 2>&1; then
                want_status=0
        else
                want_status=$?
        fi

        if "$binaries/$tool" "$@" > "$work/got" 2>&1; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 50 "$work/want" | tr '\n' '|')][$want_status] got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status]"
}

# Standard output and status, with diagnostics deliberately out of the
# comparison. This is for option combinations whose answer matters but whose
# localized/system wording does not.
answered() {
        name=$1
        tool=$2
        shift 2

        if "$tool" "$@" > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi

        if "$binaries/$tool" "$@" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 50 "$work/want" | tr '\n' '|')][$want_status] got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status]"
}

generated_answer() {
        name=$1
        want_status=$2
        got_status=$3

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                report ok
                return 0
        fi

        report bad "$name" "want [$(head -c 50 "$work/want" | tr '\n' '|')][$want_status] got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status]"
}

env_stress() {
        set -- -i
        i=0
        while [ "$i" -lt 600 ]; do
                set -- "$@" "INHERITED_$i=$i"
                i=$((i + 1))
        done
        env "$@" /usr/bin/env -0 > "$work/want" 2>/dev/null; want_status=$?
        env "$@" "$binaries/env" -0 > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '600 inherited variables' "$want_status" "$got_status"

        set --
        i=0
        while [ "$i" -lt 600 ]; do
                set -- "$@" "ASSIGNED_$i=$i"
                i=$((i + 1))
        done
        env -i "$@" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/env" -i "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '600 assigned variables' "$want_status" "$got_status"

        set -- -i
        i=0
        while [ "$i" -lt 40 ]; do
                set -- "$@" -u "DROP_$i"
                i=$((i + 1))
        done
        i=0
        while [ "$i" -lt 40 ]; do
                set -- "$@" "DROP_$i=$i"
                i=$((i + 1))
        done
        env "$@" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/env" "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '40 unset variables' "$want_status" "$got_status"

        set --
        i=0
        while [ "$i" -lt 100 ]; do
                set -- "$@" "$i"
                i=$((i + 1))
        done
        env -i /bin/echo "$@" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/env" -i /bin/echo "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '100 command arguments' "$want_status" "$got_status"

        split=/bin/echo
        i=0
        while [ "$i" -lt 400 ]; do
                split="$split word$i"
                i=$((i + 1))
        done
        env -i -S "$split" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/env" -i -S "$split" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'large split string' "$want_status" "$got_status"
}

exec_path_stress() {
        local_command=$work/path-local
        denied_command=$work/path-denied
        system_find=$(command -v find)
        printf '#!/bin/sh\nprintf "local\\n"\n' > "$local_command"
        printf '#!/bin/sh\nprintf "denied\\n"\n' > "$denied_command"
        chmod 0755 "$local_command"
        chmod 0644 "$denied_command"

        (cd "$work" && env -i PATH= path-local) \
                > "$work/want" 2>/dev/null; want_status=$?
        (cd "$work" && "$binaries/env" -i PATH= path-local) \
                > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'empty PATH is current directory' "$want_status" "$got_status"

        (cd "$work" && env -i PATH=/nowhere: path-local) \
                > "$work/want" 2>/dev/null; want_status=$?
        (cd "$work" && "$binaries/env" -i PATH=/nowhere: path-local) \
                > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'trailing PATH component' "$want_status" "$got_status"

        if env -i PATH="$work" path-denied > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi
        if "$binaries/env" -i PATH="$work" path-denied \
                > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi
        generated_answer 'permission denied is 126' "$want_status" "$got_status"

        long_component=$(python3 - <<'PY'
print("x" * 20000)
PY
)
        env -i PATH="$long_component:/bin:/usr/bin" echo through \
                > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/env" -i PATH="$long_component:/bin:/usr/bin" echo through \
                > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'oversized PATH then valid' "$want_status" "$got_status"

        (cd "$work" && PATH= "$system_find" "$fixture" -maxdepth 0 \
                -exec path-local ';') \
                > "$work/want" 2>/dev/null; want_status=$?
        (cd "$work" && PATH= "$binaries/find" "$fixture" -maxdepth 0 \
                -exec path-local ';') > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'find exec empty PATH' "$want_status" "$got_status"
}

yes_stress() {
        python3 - "$binaries/yes" > "$work/yes-stress" <<'PY'
import subprocess
import sys

ours = sys.argv[1]
argument = "x" * 20000
expected = (argument + "\n").encode()

def first_line(program):
    child = subprocess.Popen([program, argument], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL)
    data = bytearray()
    while len(data) < len(expected):
        block = child.stdout.read(len(expected) - len(data))
        if not block:
            break
        data.extend(block)
    child.terminate()
    child.wait(timeout=10)
    return bytes(data)

print("ok" if first_line("yes") == first_line(ours) == expected else "bad")
PY

        if [ "$(cat "$work/yes-stress")" = ok ]; then
                report ok
        else
                report bad '20K argument' 'first output line differs'
        fi
}

large_copy_stress() {
        dd if=/dev/zero of="$work/cp-large-source" bs=1M count=32 \
                status=none 2>/dev/null
        printf 'tail-marker\n' | dd of="$work/cp-large-source" bs=1 seek=33554420 \
                conv=notrunc status=none 2>/dev/null

        cp "$work/cp-large-source" "$work/cp-large-want" 2>/dev/null
        want_status=$?
        "$binaries/cp" "$work/cp-large-source" "$work/cp-large-got" 2>/dev/null
        got_status=$?

        if [ "$want_status" = "$got_status" ] &&
           cmp -s "$work/cp-large-want" "$work/cp-large-got"; then
                report ok
        else
                report bad '32 MiB regular file' \
                        "status $want_status/$got_status or copied bytes differ"
        fi
}

# Fixed-memory ceilings are permitted to refuse work, but never to emit a
# plausible prefix and exit successfully.
refuses_ls_ceiling() {
        if "$binaries/ls" "$1" > "$work/got" 2> "$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q '^ls: directory has too many entries$' "$work/got.err"; then
                report ok
                return 0
        fi

        report bad 'entry ceiling' "wanted loud refusal, got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status] $(head -1 "$work/got.err")"
}

du_stress() {
        set --
        i=0

        while [ "$i" -lt 40 ]; do
                set -- "$@" "--exclude=never-match-$i"
                i=$((i + 1))
        done

        du "$@" "$fixture" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/du" "$@" "$fixture" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '40 exclude patterns' "$want_status" "$got_status"
}

find_stress() {
        set -- "$fixture"
        i=0

        while [ "$i" -lt 160 ]; do
                set -- "$@" -true
                i=$((i + 1))
        done

        find "$@" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/find" "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '160 expression nodes' "$want_status" "$got_status"

        find "$fixture" -exec /bin/true {} + -exec /bin/true {} + \
             -exec /bin/true {} + > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/find" "$fixture" -exec /bin/true {} + -exec /bin/true {} + \
             -exec /bin/true {} + > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'three exec batches' "$want_status" "$got_status"

        set -- "$fixture" -exec /bin/true
        i=0

        while [ "$i" -lt 300 ]; do
                set -- "$@" "word-$i"
                i=$((i + 1))
        done

        set -- "$@" {} +
        find "$@" > "$work/want" 2>/dev/null; want_status=$?
        "$binaries/find" "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer '300 exec arguments' "$want_status" "$got_status"

        command="find -L '$fixture' -name alpha -print; find '$fixture' -type l; find '$fixture' -maxdepth 0"
        /bin/sh -c "$command" > "$work/want" 2>/dev/null; want_status=$?
        "${binaries%/bin}/shell" -c "$command" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'three shell invocations' "$want_status" "$got_status"
}

ls_color_compare() {
        name=$1
        shift
        colors='di=34:ln=36:pi=33:ex=32:fi=0:*.txt=35:rs=0'

        LS_COLORS=$colors ls "$@" > "$work/want" 2>/dev/null; want_status=$?
        LS_COLORS=$colors "$binaries/ls" "$@" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer "$name" "$want_status" "$got_status"
}

ls_color_custom() {
        name=$1
        colors=$2
        shift 2

        LS_COLORS=$colors ls "$@" > "$work/want" 2>&1; want_status=$?
        LS_COLORS=$colors "$binaries/ls" "$@" > "$work/got" 2>&1; got_status=$?
        generated_answer "$name" "$want_status" "$got_status"
}

ls_color_unset() {
        env -u LS_COLORS ls --color=always -1 "$colored" > "$work/want" 2>/dev/null
        want_status=$?
        env -u LS_COLORS "$binaries/ls" --color=always -1 "$colored" > "$work/got" 2>/dev/null
        got_status=$?
        generated_answer 'color environment unset' "$want_status" "$got_status"
}

ls_pty_capture() {
        output=$1
        no_color=$2
        program=$3
        shift 3

        python3 - "$output" "$no_color" "$program" "$@" <<'PY'
import errno
import os
import pty
import sys

output, no_color, program, *arguments = sys.argv[1:]
pid, descriptor = pty.fork()
if pid == 0:
    os.environ["LS_COLORS"] = "di=34:ln=36:pi=33:ex=32:fi=0:rs=0"
    os.environ["TERM"] = "xterm"
    if no_color == "set":
        os.environ["NO_COLOR"] = "1"
    else:
        os.environ.pop("NO_COLOR", None)
    os.execvp(program, [program, *arguments])

data = bytearray()
while True:
    try:
        block = os.read(descriptor, 65536)
    except OSError as error:
        if error.errno == errno.EIO:
            break
        raise
    if not block:
        break
    data.extend(block)
_, status = os.waitpid(pid, 0)
with open(output, "wb") as stream:
    stream.write(data)
sys.exit(os.waitstatus_to_exitcode(status))
PY
}

ls_color_pty_tests() {
        ls_pty_capture "$work/want" unset ls --color=auto -1 "$colored"; want_status=$?
        ls_pty_capture "$work/got" unset "$binaries/ls" --color=auto -1 "$colored"; got_status=$?
        generated_answer 'color auto pty' "$want_status" "$got_status"

        ls_pty_capture "$work/want" unset ls --color=never -1 "$colored"; want_status=$?
        ls_pty_capture "$work/got" set "$binaries/ls" --color=auto -1 "$colored"; got_status=$?
        generated_answer 'color NO_COLOR auto' "$want_status" "$got_status"

        ls_pty_capture "$work/want" set ls --color=always -1 "$colored"; want_status=$?
        ls_pty_capture "$work/got" set "$binaries/ls" --color=always -1 "$colored"; got_status=$?
        generated_answer 'color NO_COLOR always' "$want_status" "$got_status"
}

ls_color_multicall() {
        command="LS_COLORS=di=34:fi=0 ls --color=always -1 '$colored'; ls -1 '$colored'"
        /bin/sh -c "$command" > "$work/want" 2>/dev/null; want_status=$?
        "${binaries%/bin}/shell" -c "$command" > "$work/got" 2>/dev/null; got_status=$?
        generated_answer 'color multicall reset' "$want_status" "$got_status"
}

refuses_du_depth_ceiling() {
        if "$binaries/du" "$1" > "$work/got" 2> "$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q '^du: tree is nested too deep$' "$work/got.err"; then
                report ok
                return 0
        fi

        report bad 'depth ceiling' "wanted loud refusal, got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status] $(head -1 "$work/got.err")"
}

refuses_realpath_relative_ceiling() {
        if "$binaries/realpath" -m --relative-to="$1" "$2" \
                > "$work/got" 2> "$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q 'File name too long' "$work/got.err"; then
                report ok
                return 0
        fi

        report bad 'relative result ceiling' \
                "wanted loud refusal, got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status] $(head -1 "$work/got.err")"
}

refuses_ln_relative_ceiling() {
        if "$binaries/ln" -sr "$1" "$2" > "$work/got" 2> "$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q 'File name too long' "$work/got.err"; then
                report ok
                return 0
        fi

        report bad 'relative link ceiling' \
                "wanted loud refusal, got [$(head -c 50 "$work/got" | tr '\n' '|')][$got_status] $(head -1 "$work/got.err")"
}

# When a file was last written, or the word now.
#
# The two trees are built one after the other and the two tools run one after
# the other, so anything either of them creates carries the second it happened
# to be created in. Comparing those fails whenever the pair falls either side
# of a tick -- a coin flip with nothing to do with the tools. A time from
# before the suite started is compared exactly, which is what touch -d and
# cp -p are for.
modified() {
        stamp=$(stat -c '%Y' "$1" 2>/dev/null)

        if [ -n "$stamp" ] && [ "$stamp" -ge "$started" ]; then
                echo now
        else
                echo "$stamp"
        fi
}

# A dump of a tree that captures everything the mutating tools are for.
dump() {
        (
                cd "$1" || exit 1
                find . | LC_ALL=C sort | while read -r entry; do
                        # %Y and not %X: find and cksum read the tree to
                        # dump it, so the access times differ between the two
                        # dumps by construction. The modify time is the one
                        # touch and cp -p are for.
                        # %h and not only %a %F %s: cp -l and ln make a name
                        # for a file rather than a copy of it, and the two are
                        # the same file by every other measure here.
                        printf '%s %s %s %s' "$entry" \
                                "$(stat -c '%a %F %s %h' "$entry" 2>/dev/null)" \
                                "$(modified "$entry")" \
                                "$(readlink "$entry" 2>/dev/null)"
                        if [ -f "$entry" ] && [ ! -L "$entry" ]; then
                                printf ' %s' "$(cksum < "$entry")"
                        fi
                        printf '\n'
                done
        )
}

# Twin trees, the system tool on one and ours on the other, and afterwards
# the two trees have to be the same tree.
effect() {
        name=$1
        tool=$2
        recipe=$3

        rm -rf "$work/a" "$work/b"
        mkdir -p "$work/a" "$work/b"

        seed "$work/a"
        seed "$work/b"

        if ( cd "$work/a" && eval "TOOL=$tool; $recipe" ) > "$work/want.out" 2>&1; then
                want_status=0
        else
                want_status=$?
        fi

        if ( cd "$work/b" && eval "TOOL=$binaries/$tool; $recipe" ) > "$work/got.out" 2>&1; then
                got_status=0
        else
                got_status=$?
        fi

        dump "$work/a" > "$work/want"
        dump "$work/b" > "$work/got"

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                report ok
                return 0
        fi

        report bad "$name" "status $want_status/$got_status $(diff "$work/want" "$work/got" | head -4 | tr '\n' '|')"
}

# What every effect case starts from: a small tree with a bit of everything
# in it that the mutating tools have to deal with.
seed() {
        root=$1
        mkdir -p "$root/tree/deep/deeper"
        printf 'hello\n' > "$root/tree/one"
        printf 'two two two\n' > "$root/tree/two"
        printf 'buried\n' > "$root/tree/deep/three"
        printf 'deepest\n' > "$root/tree/deep/deeper/four"
        printf 'top\n' > "$root/plain"
        ln -s tree/one "$root/link"
        chmod 0640 "$root/tree/two"
        chmod 0700 "$root/tree/deep"
        touch -d @1000000000 "$root/tree/one"
}

# How long a sleep takes, rounded to a tenth of a second. Nothing is printed
# by either tool, so the elapsed time is the whole of what there is to compare.
tenths() {
        started=$(date +%s%N)
        "$@" >/dev/null 2>&1 || true
        stopped=$(date +%s%N)
        echo $(( (stopped - started) / 100000000 ))
}

timing() {
        name=$1
        shift

        want=$(tenths sleep "$@")
        got=$(tenths "$binaries/sleep" "$@")

        if [ "$want" = "$got" ]; then
                report ok
                return 0
        fi

        report bad "$name" "want ${want} tenths, got ${got} tenths"
}

# ------------------------------------------------------------------

fixture=$work/fixture
mkdir -p "$fixture/sub/inner" "$fixture/empty"
printf 'a\n' > "$fixture/alpha"
printf 'bbbbbbbbbbbbbbbb\n' > "$fixture/beta.txt"
printf 'c\n' > "$fixture/sub/gamma.txt"
printf 'd\n' > "$fixture/sub/inner/delta"
ln -s ../alpha "$fixture/sub/back"
ln -s sub "$fixture/todir"
ln -s ../empty "$fixture/sub/away"
chmod 0751 "$fixture/sub"
touch -d @1500000000 "$fixture/alpha"
touch -d @1400000000 "$fixture/beta.txt"
printf 'old\n' > "$fixture/ancient"
touch -d @1000000000 "$fixture/ancient"
long_path=$(python3 - <<'PY'
print("x" * 20000)
PY
)

group basename
same 'path'             basename /usr/bin/ls
same 'trailing slash'   basename /usr/bin/
same 'no slash'         basename usr
same 'root'             basename /
same 'double root'      basename //
same 'dot'              basename .
same 'dotdot'           basename ..
same 'suffix'           basename file.txt .txt
same 'suffix absent'    basename file.txt .c
same 'suffix is all'    basename .txt .txt
same 'multiple'         basename -a /a/b /c/d
same 'suffix flag'      basename -s .txt one.txt two.txt
same 'multiple long'    basename --multiple /a/b /c/d
same 'suffix long'      basename --suffix=.txt one.txt two.txt
same 'suffix joined'    basename -s.txt one.txt
same 'zero'             basename -z /usr/bin/ls
same 'zero and many'    basename --zero -a /a/b /c/d
same 'not an option'    basename -x /a/b
answered 'extra operand' basename abc b extra
same '20K name'         basename "$long_path"
same '20K path tail'    basename "/short/$long_path"
same '20K suffix'       basename -s .tail "${long_path}.tail"

group dirname
same 'path'             dirname /usr/bin/ls
same 'trailing slash'   dirname /usr/bin/
same 'no slash'         dirname usr
same 'empty'            dirname ''
same 'root'             dirname /
same 'double root'      dirname //
same 'triple root'      dirname ///
same 'one level'        dirname /a
same 'repeated separators' dirname //a//b///
same 'many'             dirname /a/b /c ./d
same 'zero'             dirname -z /a/b /c
same 'zero long'        dirname --zero /a/b
same 'not an option'    dirname -x /a/b
same '20K directory'    dirname "${long_path}/tail"
same '20K absolute directory' dirname "/${long_path}/tail"
same '20K trailing slash' dirname "${long_path}/tail/"
same '20K directory zero' dirname -z "${long_path}/tail"

group seq
same 'last'             seq 5
same 'first last'       seq 3 9
same 'increment'        seq 2 3 20
same 'negative'         seq -4 4
same 'descending'       seq 10 -2 1
same 'empty'            seq 9 2
same 'padded'           seq -w 8 11
same 'padded negative'  seq -w -16 -18 -70
same 'separator'        seq -s , 1 5
same 'one'              seq 1 1
same 'padded long'      seq --equal-width 8 11
same 'separator long'   seq --separator=, 1 5
same 'separator joined' seq -s, 1 5
same 'not an option'    seq -x 3
answered 'malformed suffix' seq 1x
answered 'malformed word' seq x1
answered 'empty number' seq ''
answered 'sign only' seq +
answered 'dot only' seq .
same 'signed maximum singleton' seq 9223372036854775807 1 9223372036854775807
same 'signed minimum singleton' seq -9223372036854775808 -1 -9223372036854775808
same 'decimal tenths exact' seq 0 .1 .3
same 'decimal places retained' seq 1.00 .25 2.00
same 'decimal descending' seq 1 -.25 0
same 'decimal exponent' seq 1e0 2e-1 1.6e0
same 'decimal negative zero' seq -0.0 .1 .2
same 'decimal endpoint below step' seq 0 .1 .29999999999999999
same 'decimal endpoint above step' seq 0 .1 .30000000000000001
same 'decimal padded sign' seq -w -1 .5 1
same 'decimal padded negative range' seq -w -2.53 -.52 -15.19
same 'decimal padded crossing' seq -w 9.9 .1 10.05
same 'decimal separator' seq -s, 0 .1 .3
same 'decimal fixed format' seq -f %.2f 0 .1 .3
same 'decimal format zero pad' seq -f %06.2f 0 .1 .3
same 'decimal format left' seq -f %-6.2f 0 .5 1
same 'decimal format signed' seq -f %+06.2f 0 .5 1
same 'decimal format literal percent' seq -f '%% %.1f' 0 .5 1
same 'decimal format ties even' seq -f %.0f -1 .5 1
same 'decimal format alternate' seq -f '%#.0f' 1 2
same 'decimal format long double' seq -f %Lf 1 2
answered 'decimal zero increment' seq 1 0.0 2
answered 'decimal malformed exponent' seq 1e 2
answered 'decimal repeated point' seq 1.2.3 2
answered 'decimal format with equal width' seq -w -f %.1f 1 2
answered 'decimal multiple formats' seq -f %f-%f 1 2

group yes
yes_stress

group readlink
same 'link'             readlink "$fixture/sub/back"
same 'not a link'       readlink "$fixture/alpha"
same 'resolve'          readlink -f "$fixture/sub/back"
same 'resolve plain'    readlink -f "$fixture/alpha"
same 'resolve missing'  readlink -f "$fixture/nothing"
same 'exists missing'   readlink -e "$fixture/nothing"
same 'resolve long'     readlink --canonicalize "$fixture/sub/back"
same 'no newline'       readlink -n "$fixture/sub/back"
same 'zero'             readlink -z "$fixture/sub/back"
same 'zero long'        readlink --zero "$fixture/sub/back"
spoken 'quiet by default' readlink "$fixture/alpha"
spoken 'loud'           readlink -v "$fixture/alpha"
spoken 'loud long'      readlink --verbose "$fixture/alpha"
spoken 'quiet'          readlink -q "$fixture/alpha"
spoken 'loud and gone'  readlink -v "$fixture/nothing"
spoken 'loud resolving' readlink -vf "$fixture/nothing/at/all"
spoken 'loud existing'  readlink -ve "$fixture/nothing"
answered 'missing then existing' readlink -m -e "$fixture/nothing/at/all"
answered 'existing then missing' readlink -e -m "$fixture/nothing/at/all"
answered 'no newline many' readlink -n "$fixture/sub/back" "$fixture/todir"
answered 'zero beats no newline many' readlink -n -z "$fixture/sub/back" "$fixture/todir"

group realpath
same 'plain'            realpath "$fixture/alpha"
same 'through link'     realpath "$fixture/sub/back"
same 'dots'             realpath "$fixture/sub/../alpha"
same 'directory'        realpath "$fixture/sub/inner"
same 'root'             realpath /
same 'missing'          realpath "$fixture/nothing"
same 'missing allowed'  realpath -m "$fixture/nothing/at/all"
same 'missing long'     realpath --canonicalize-missing "$fixture/nothing/at/all"
same 'exists'           realpath -e "$fixture/nothing"
same 'no symlinks'      realpath -s "$fixture/sub/back"
same 'no symlinks dots' realpath --no-symlinks "$fixture/sub/../alpha"
same 'no symlinks gone' realpath -s "$fixture/nothing/at/all"
same 'zero'             realpath -z "$fixture/alpha"
same 'relative to'      realpath --relative-to="$fixture/sub" "$fixture/alpha"
same 'relative to down' realpath --relative-to="$fixture" "$fixture/sub/inner"
same 'relative to same' realpath --relative-to="$fixture" "$fixture"
same 'relative to ancestor' realpath --relative-to="$fixture/sub/inner" "$fixture"
same 'relative to parent' realpath --relative-to="$fixture/sub" "$work"
same 'relative base'    realpath --relative-base="$fixture" "$fixture/sub/inner"
same 'relative base out' realpath --relative-base=/usr "$fixture/alpha"
answered 'empty relative to' realpath --relative-to= "$fixture/alpha"
answered 'empty relative base' realpath --relative-base= "$fixture/alpha"
same 'not an option'    realpath -x "$fixture/alpha"
same 'logical'          realpath -L "$fixture/sub/away/.."
same 'physical of that' realpath -P "$fixture/sub/away/.."
same 'logical of a link' realpath -L "$fixture/todir/.."
same 'physical of it'   realpath -P "$fixture/todir/.."
same 'logical of a file' realpath -L "$fixture/sub/back/.."
same 'logical dots'     realpath -L "$fixture/sub/../alpha"
same 'logical long'     realpath --logical "$fixture/sub/inner/.."
same 'physical'         realpath -P "$fixture/sub/back"
same 'physical long'    realpath --physical "$fixture/sub/back"
same 'exists not'       realpath -E "$fixture/nothing"
same 'not a word'       realpath --canonical "$fixture/alpha"
answered 'missing then existing' realpath -m -e "$fixture/nothing/at/all"
answered 'existing then missing' realpath -e -m "$fixture/nothing/at/all"
answered 'logical then physical' realpath -L -P "$fixture/sub/away/.."
answered 'physical then logical' realpath -P -L "$fixture/sub/away/.."
answered 'overlong path' realpath "$long_path"
relative_from=$(python3 - <<'PY'
print("/" + "/".join(["a"] * 1300))
PY
)
relative_to=$(python3 - <<'PY'
print("/" + "/".join(["b"] * 1300))
PY
)
refuses_realpath_relative_ceiling "$relative_from" "$relative_to"
refuses_ln_relative_ceiling "$relative_to" "$relative_from/link"

group id
same 'default'          id
same 'user'             id -u
same 'group'            id -g
same 'user name'        id -un
same 'group name'       id -gn
same 'groups'           id -G
same 'user long'        id --user
same 'group long'       id --group
same 'groups long'      id --groups
same 'name long'        id --user --name
same 'zero user'        id -uz
same 'zero groups'      id -Gz
same 'name alone'       id -n
same 'real alone'       id -r
same 'zero alone'       id -z
same 'context'          id -Z
same 'a named user'     id root
same 'a named user id'  id -u root
same 'a named name'     id -un root
same 'a named groups'   id -G root
same 'no such user'     id nosuchuseranywhere

group uname
same 'system'           uname
same 'node'             uname -n
same 'release'          uname -r
same 'machine'          uname -m
near 'all'              'cat' uname -snrvm
same 'node long'        uname --nodename
same 'release long'     uname --kernel-release
same 'machine long'     uname --machine
same 'processor'        uname -p
same 'hardware'         uname -i
same 'not an option'    uname -x

group find
near 'plain'            'LC_ALL=C sort' find "$fixture"
near 'name'             'LC_ALL=C sort' find "$fixture" -name '*.txt'
near 'type file'        'LC_ALL=C sort' find "$fixture" -type f
near 'type directory'   'LC_ALL=C sort' find "$fixture" -type d
near 'type link'        'LC_ALL=C sort' find "$fixture" -type l
near 'maxdepth 1'       'LC_ALL=C sort' find "$fixture" -maxdepth 1
near 'maxdepth 2'       'LC_ALL=C sort' find "$fixture" -maxdepth 2
near 'mindepth'         'LC_ALL=C sort' find "$fixture" -mindepth 2
near 'size bytes'       'LC_ALL=C sort' find "$fixture" -type f -size -10c
near 'size blocks'      'LC_ALL=C sort' find "$fixture" -type f -size 1
near 'empty'            'LC_ALL=C sort' find "$fixture" -empty
near 'name and type'    'LC_ALL=C sort' find "$fixture" -type f -name '*a*'
near 'name question'    'LC_ALL=C sort' find "$fixture" -name 'sub?'
near 'name class'       'LC_ALL=C sort' find "$fixture" -name '[ab]*'
near 'name negated'     'LC_ALL=C sort' find "$fixture" -name '[!ab]*'
near 'name exact'       'LC_ALL=C sort' find "$fixture" -name alpha
near 'name prefix'      'LC_ALL=C sort' find "$fixture" -name 'a*'
near 'path'             'LC_ALL=C sort' find "$fixture" -path '*sub*'
near 'perm'             'LC_ALL=C sort' find "$fixture" -perm 751
near 'size plus'        'LC_ALL=C sort' find "$fixture" -type f -size +10c
near 'depth and name'   'LC_ALL=C sort' find "$fixture" -maxdepth 1 -type f
near 'two roots'        'LC_ALL=C sort' find "$fixture/sub" "$fixture/empty"
# The expression is a language: what follows is precedence, grouping and
# negation, none of which a list of tests that all had to hold could say.
near 'or'               'LC_ALL=C sort' find "$fixture" -name '*.txt' -o -name alpha
near 'or long'          'LC_ALL=C sort' find "$fixture" -name '*.txt' -or -name alpha
near 'and'              'LC_ALL=C sort' find "$fixture" -type f -a -name '*.txt'
near 'and long'         'LC_ALL=C sort' find "$fixture" -type f -and -name '*.txt'
near 'not'              'LC_ALL=C sort' find "$fixture" ! -type d
near 'not long'         'LC_ALL=C sort' find "$fixture" -not -type d
near 'not and'          'LC_ALL=C sort' find "$fixture" ! -type d -name '*a*'
near 'and binds first'  'LC_ALL=C sort' find "$fixture" -name alpha -o -type f -name '*.txt'
near 'grouped'          'LC_ALL=C sort' find "$fixture" '(' -name alpha -o -name '*.txt' ')' -type f
near 'grouped negated'  'LC_ALL=C sort' find "$fixture" ! '(' -type d -o -type l ')'
near 'nested groups'    'LC_ALL=C sort' find "$fixture" '(' '(' -name alpha ')' ')'
near 'true'             'LC_ALL=C sort' find "$fixture" -true
near 'false'            'LC_ALL=C sort' find "$fixture" -false
near 'true or'          'LC_ALL=C sort' find "$fixture" -false -o -name alpha
near 'prune'            'LC_ALL=C sort' find "$fixture" -name sub -prune -o -print
near 'prune printed'    'LC_ALL=C sort' find "$fixture" -name sub -prune
near 'prune a path'     'LC_ALL=C sort' find "$fixture" -path '*/inner' -prune -o -print
near 'depth first'      'cat' find "$fixture/sub" -depth
near 'depth and prune'  'LC_ALL=C sort' find "$fixture" -depth -name '*.txt'
near 'print explicit'   'LC_ALL=C sort' find "$fixture" -name alpha -print
near 'print zero'       "tr '\\0' '\\n' | LC_ALL=C sort" find "$fixture" -name '*.txt' -print0
near 'print and or'     'LC_ALL=C sort' find "$fixture" -name alpha -print -o -name '*.txt' -print
near 'links'            'LC_ALL=C sort' find "$fixture" -type f -links 1
near 'links plus'       'LC_ALL=C sort' find "$fixture" -links +1
near 'user'             'LC_ALL=C sort' find "$fixture" -user "$(id -un)"
near 'user numeric'     'LC_ALL=C sort' find "$fixture" -uid "$(id -u)"
near 'group'            'LC_ALL=C sort' find "$fixture" -group "$(id -gn)"
near 'group numeric'    'LC_ALL=C sort' find "$fixture" -gid "$(id -g)"
near 'no user'          'LC_ALL=C sort' find "$fixture" -nouser
near 'newer'            'LC_ALL=C sort' find "$fixture" -newer "$fixture/beta.txt"
near 'newer than a date' 'LC_ALL=C sort' find "$fixture" -newermt 2010-01-01
near 'older than a date' 'LC_ALL=C sort' find "$fixture" ! -newermt 2010-01-01
near 'modified days'    'LC_ALL=C sort' find "$fixture" -mtime +1
near 'modified minutes' 'LC_ALL=C sort' find "$fixture" -mmin +1
near 'changed days'     'LC_ALL=C sort' find "$fixture" -ctime -1
near 'perm any'         'LC_ALL=C sort' find "$fixture" -perm /111
near 'perm all'         'LC_ALL=C sort' find "$fixture" -perm -100
near 'perm symbolic'    'LC_ALL=C sort' find "$fixture" -perm -u+x
near 'case insensitive name' 'LC_ALL=C sort' find "$fixture" -iname 'ALPHA'
near 'case insensitive suffix' 'LC_ALL=C sort' find "$fixture" -iname '*.TXT'
near 'case insensitive path' 'LC_ALL=C sort' find "$fixture" -ipath '*SUB*'
near 'link target'      'LC_ALL=C sort' find "$fixture" -lname '*alpha'
near 'inode'            'LC_ALL=C sort' find "$fixture" -inum +0
near 'one file system'  'LC_ALL=C sort' find "$fixture" -xdev
near 'following links'  'LC_ALL=C sort' find -L "$fixture" -type f
near 'not following'    'LC_ALL=C sort' find -P "$fixture" -type l
near 'follow named'     'LC_ALL=C sort' find -H "$fixture/sub/back" -type f
near 'follow the word'  'LC_ALL=C sort' find "$fixture" -follow -type f
near 'exec once each'   'LC_ALL=C sort' find "$fixture" -name '*.txt' -exec echo saw {} ';'
near 'exec twice over'  'LC_ALL=C sort' find "$fixture" -name '*.txt' -exec echo one {} ';' -exec echo two {} ';'
near 'exec inside a word' 'LC_ALL=C sort' find "$fixture" -name alpha -exec echo 'x{}y' ';'
near 'exec status'      'LC_ALL=C sort' find "$fixture" -type f -exec test -s {} ';' -print
near 'exec together'    "tr ' ' '\\n' | LC_ALL=C sort" find "$fixture" -name '*.txt' -exec echo {} +
near 'exec together all' "tr ' ' '\\n' | LC_ALL=C sort" find "$fixture" -type f -exec echo {} +
near 'quit'             'cat' find "$fixture" -name alpha -print -quit
near 'no such predicate' 'cat' find "$fixture" -nonsense
near 'unclosed group'   'cat' find "$fixture" '(' -name alpha
near 'exec unterminated' 'cat' find "$fixture" -exec echo {}
near 'path after expression' 'cat' find "$fixture" -name alpha "$fixture/sub"

# -delete changes the tree, so it is judged by what it left behind.
effect 'delete a name'  find '$TOOL tree -name three -delete'
effect 'delete by type' find '$TOOL tree -type f -delete'
effect 'delete a tree'  find '$TOOL tree -delete'
effect 'delete nothing' find '$TOOL tree -name nowhere -delete'
effect 'delete deep'    find '$TOOL tree/deep -depth -delete'
effect 'exec removes'   find '$TOOL tree -name two -exec rm {} ";"'
effect 'exec removes many' find '$TOOL tree -type f -exec rm {} +'
find_stress

group stat
same 'name'             stat -c '%n' "$fixture/alpha"
same 'size'             stat -c '%s' "$fixture/beta.txt"
same 'octal mode'       stat -c '%a' "$fixture/sub"
same 'mode letters'     stat -c '%A' "$fixture/sub"
same 'kind'             stat -c '%F' "$fixture/sub"
same 'kind of link'     stat -c '%F' "$fixture/sub/back"
same 'links'            stat -c '%h' "$fixture/alpha"
same 'owner'            stat -c '%u %U' "$fixture/alpha"
same 'group'            stat -c '%g %G' "$fixture/alpha"
same 'blocks'           stat -c '%b %B' "$fixture/alpha"
same 'inode'            stat -c '%i' "$fixture/alpha"
same 'modify epoch'     stat -c '%Y' "$fixture/alpha"
same 'access epoch'     stat -c '%X' "$fixture/beta.txt"
same 'modify stamp'     stat -c '%y' "$fixture/alpha"
same 'raw mode'         stat -c '%f' "$fixture/alpha"
same 'many'             stat -c '%n %s %a %F' "$fixture/alpha" "$fixture/sub"
same 'escapes'          stat -c '%n:%s' "$fixture/alpha"
same 'follow'           stat -L -c '%F' "$fixture/sub/back"
same 'follow size'      stat -L -c '%s' "$fixture/sub/back"
same 'io block'         stat -c '%o' "$fixture/alpha"
same 'device'           stat -c '%d' "$fixture/alpha"
same 'change epoch'     stat -c '%Z' "$fixture/alpha"
same 'access stamp'     stat -c '%x' "$fixture/beta.txt"
same 'change stamp'     stat -c '%z' "$fixture/beta.txt"
same 'literal percent'  stat -c '100%%' "$fixture/alpha"
same 'tab escape'       stat -c '%n\t%s' "$fixture/alpha"
same 'directory octal'  stat -c '%a %A %F' "$fixture/empty"
same 'default block'    stat "$fixture/alpha"
same 'default directory' stat "$fixture/sub"
# The Access line is dropped for the link only: reading a symlink is what
# sets its access time, so whichever of the two tools runs second sees the
# time the first one caused. Every other line is compared.
near 'default link'     "grep -v '^Access: 2'" stat "$fixture/sub/back"
same 'quoted name'      stat -c '%N' "$fixture/alpha"
same 'quoted link'      stat -c '%N' "$fixture/sub/back"
near 'file system'      "sed 's/[0-9]/X/g'" stat -f "$fixture/alpha"
near 'file system long' "sed 's/[0-9]/X/g'" stat --file-system "$fixture/alpha"
same 'file system fields' stat -f -c '%n|%i|%l|%s|%S|%b|%c|%T|%t' "$fixture/alpha"
near 'file system changing fields' "sed 's/[0-9]/X/g'" stat -f -c '%f|%a|%d' "$fixture/alpha"
same 'file system many' stat -f -c '%n:%T' "$fixture/alpha" "$fixture/sub"
spoken 'file system missing' stat -f "$fixture/nothing"

#       One of everything a name can be, for the tools that say which it is.
kinds=$work/kinds
mkdir -p "$kinds/adir"
printf 'x\n' > "$kinds/plain"
printf 'x\n' > "$kinds/runnable"
chmod 0755 "$kinds/runnable"
ln -s plain "$kinds/pointer"
ln -s nowhere "$kinds/broken"
mkfifo "$kinds/pipe" 2>/dev/null || true

crowded=$work/crowded
mkdir "$crowded"
i=0
while [ "$i" -lt 8193 ]; do
        : > "$crowded/f$i"
        i=$((i + 1))
done
crowded_copy=$work/crowded-copy
cp -al "$crowded" "$crowded_copy"

deep_du=$work/deep-du
deep_step=$deep_du
mkdir "$deep_step"
i=0
while [ "$i" -lt 33 ]; do
        deep_step=$deep_step/d
        mkdir "$deep_step"
        i=$((i + 1))
done

colored=$work/colored
mkdir "$colored" "$colored/directory"
printf 'plain\n' > "$colored/plain"
printf 'text\n' > "$colored/note.txt"
printf 'run\n' > "$colored/runnable"
chmod 0755 "$colored/runnable"
ln -s plain "$colored/link"
ln -s directory "$colored/link-directory"
ln -s absent "$colored/broken"
mkfifo "$colored/pipe"
control_name=$(printf 'control\033byte')
printf 'control\n' > "$colored/$control_name"

group ls
ls_color_compare 'color always' --color=always -1 "$colored"
ls_color_compare 'color bare' --color -1 "$colored"
ls_color_compare 'color never' --color=never -1 "$colored"
ls_color_compare 'color auto redirected' --color=auto -1 "$colored"
ls_color_unset
ls_color_custom 'color environment empty' '' --color=always -1 "$colored"
ls_color_custom 'orphan color' 'ln=36:or=31:mi=35' --color=always -1 "$colored"
ls_color_custom 'link target color' 'ln=target:di=34:or=31' --color=always -1 "$colored"
ls_color_custom 'literal suffix pattern' '*.txt=33:*.?=32' --color=always -1 "$colored"
ls_color_custom 'malformed colors' 'di=34:broken' --color=always -1 "$colored"
answered 'bad color word' ls --color=sometimes -1 "$colored"
ls_color_pty_tests
ls_color_multicall
near 'plain'            'cat' ls "$fixture"
near 'all'              'cat' ls -a "$fixture"
near 'almost all'       'cat' ls -A "$fixture"
near 'one per line'     'cat' ls -1 "$fixture"
near 'time sorted'      'cat' ls -t "$fixture"
near 'size sorted'      'cat' ls -S "$fixture"
near 'reversed'         'cat' ls -r "$fixture"
near 'directory itself' 'cat' ls -d "$fixture"
near 'recursive'        'cat' ls -R "$fixture"
near 'inode'            "awk '{print \$NF}'" ls -i "$fixture"
same 'long'             ls -l "$fixture"
same 'long human'       ls -lh "$fixture"
same 'long links'       ls -l "$fixture/sub"
same 'long time sorted' ls -lt "$fixture"
same 'long size sorted' ls -lS "$fixture"
same 'long reversed'    ls -lr "$fixture"
same 'long all'         ls -la "$fixture"
same 'long numeric'     ls -n "$fixture"
same 'long inode'       ls -li "$fixture"
same 'long recursive'   ls -lR "$fixture"
same 'long empty dir'   ls -l "$fixture/empty"
near 'all time reverse' 'cat' ls -atr "$fixture"
near 'a file'           'cat' ls "$fixture/alpha"
near 'two operands'     'cat' ls "$fixture/sub" "$fixture/empty"
near 'file and dir'     'cat' ls "$fixture/alpha" "$fixture/empty"
near 'recursive deep'   'cat' ls -R "$fixture/sub"
near 'recursive all'    'cat' ls -aR "$fixture/empty"
near 'missing'          'cat' ls "$fixture/nothing"
near 'classified'       'cat' ls -F "$fixture"
near 'classified all'   'cat' ls -aF "$fixture"
near 'classified deep'  'cat' ls -F "$fixture/sub"
near 'slashed'          'cat' ls -p "$fixture"
same 'long classified'  ls -lF "$fixture"
same 'long classified link' ls -lF "$fixture/sub"
same 'long slashed'     ls -lp "$fixture"
near 'classified kinds' 'cat' ls -F "$kinds"
near 'classified kinds long' 'cat' ls -lF "$kinds"
near 'slashed kinds'    'cat' ls -p "$kinds"
answered 'all then almost all' ls -a -A "$fixture/empty"
answered 'almost all then all' ls -A -a "$fixture/empty"
answered 'time then size' ls -t -S "$fixture"
answered 'size then time' ls -S -t "$fixture"
refuses_ls_ceiling "$crowded"

#       A file with two names in the tree is one file. Nothing above has one,
#       so a tree with a pair of them is built for the tools that have to
#       count it once, and a second pair on a directory of its own for -S and
#       --max-depth to have something with a shape.
linked=$work/linked
mkdir -p "$linked/one/two"
printf 'aaaa\n' > "$linked/first"
ln "$linked/first" "$linked/second"
printf 'bbbb\n' > "$linked/one/buried"
ln "$linked/one/buried" "$linked/one/two/alias"
printf 'cc\n' > "$linked/one/two/plain"

group du
near 'summary'          "awk '{print \$1}'" du -s "$fixture"
near 'all'              "LC_ALL=C sort" du "$fixture"
near 'every file'       "LC_ALL=C sort" du -a "$fixture"
near 'apparent'         "awk '{print \$1}'" du -sb "$fixture"
near 'human summary'    "awk '{print \$1}'" du -sh "$fixture"
near 'with total'       "LC_ALL=C sort" du -c "$fixture/sub"
near 'summary of two'   "LC_ALL=C sort" du -s "$fixture/sub" "$fixture/empty"
near 'apparent all'     "LC_ALL=C sort" du -ab "$fixture"
# The second name of a file is not a second file: without -l it costs
# nothing, and with -a it is not even listed.
near 'hard links once'  "LC_ALL=C sort" du "$linked"
near 'hard links all'   "LC_ALL=C sort" du -a "$linked"
near 'hard links twice' "LC_ALL=C sort" du -l "$linked"
near 'hard links deep'  "LC_ALL=C sort" du -s "$linked"
near 'hard links apparent' "LC_ALL=C sort" du -sb "$linked"
near 'separate dirs'    "LC_ALL=C sort" du -S "$linked"
near 'separate summary' "LC_ALL=C sort" du -Sa "$linked"
near 'max depth'        "LC_ALL=C sort" du -d 1 "$linked"
near 'max depth zero'   "LC_ALL=C sort" du -d 0 "$linked"
near 'max depth long'   "LC_ALL=C sort" du --max-depth=1 "$linked"
near 'max depth all'    "LC_ALL=C sort" du -a -d 1 "$linked"
near 'one file system'  "LC_ALL=C sort" du -x "$linked"
near 'exclude a name'   "LC_ALL=C sort" du --exclude=two "$linked"
near 'exclude a path'   "LC_ALL=C sort" du --exclude="$linked/one" "$linked"
near 'exclude a glob'   "LC_ALL=C sort" du --exclude='*/two' "$linked"
near 'exclude twice'    "LC_ALL=C sort" du --exclude=two --exclude=buried "$linked"
near 'megabytes'        "LC_ALL=C sort" du -sm "$linked"
near 'count links long' "LC_ALL=C sort" du --count-links "$linked"
near 'separate long'    "LC_ALL=C sort" du --separate-dirs "$linked"
near 'one system long'  "LC_ALL=C sort" du --one-file-system "$linked"
near 'apparent long'    "LC_ALL=C sort" du -s --apparent-size "$linked"
same 'not an option'    du -W "$linked"
same 'not a word'       du --excluding=two "$linked"
answered 'bytes then megabytes' du -b -m "$fixture/alpha"
answered 'megabytes then bytes' du -m -b "$fixture/alpha"
answered 'kilobytes then megabytes' du -k -m "$fixture/alpha"
answered 'megabytes then kilobytes' du -m -k "$fixture/alpha"
answered 'summary with all' du -s -a "$fixture"
answered 'summary with depth' du -s -d1 "$fixture"
answered 'bad depth word' du -d nope "$fixture"
answered 'negative depth means zero' du -d -1 "$fixture"
answered 'positive signed depth' du -d +1 "$fixture"
du_stress
answered '8193 hard-link identities' du "$crowded"
refuses_du_depth_ceiling "$deep_du"

group df
# The digits are masked and their count is not: how full a filesystem is
# changes while the two tools are being run -- this suite writes into /tmp
# between them -- and the column layout is the part that is being compared.
near 'whole table'      "sed 's/[0-9]/X/g'" df
near 'human'            "sed 's/[0-9]/X/g'" df -h
near 'root'             "sed 's/[0-9]/X/g'" df /
near 'a path'           "sed 's/[0-9]/X/g'" df /tmp
near 'mount points'     "tail -n +2 | awk '{print \$NF}' | LC_ALL=C sort" df
near 'inodes'           "sed 's/[0-9]/X/g'" df -i
near 'inodes of a path' "sed 's/[0-9]/X/g'" df -i /tmp
near 'types'            "sed 's/[0-9]/X/g'" df -T
near 'types of a path'  "sed 's/[0-9]/X/g'" df -T /tmp
near 'portable'         "sed 's/[0-9]/X/g'" df -P
near 'portable human'   "sed 's/[0-9]/X/g'" df -Ph
near 'everything'       "sed 's/[0-9]/X/g' | LC_ALL=C sort" df -a
near 'everything typed' "sed 's/[0-9]/X/g' | LC_ALL=C sort" df -aT
near 'inodes and types' "sed 's/[0-9]/X/g'" df -iT /tmp
near 'inodes portable'  "sed 's/[0-9]/X/g'" df -Pi /tmp
near 'all long'         "sed 's/[0-9]/X/g' | LC_ALL=C sort" df --all
near 'inodes long'      "sed 's/[0-9]/X/g'" df --inodes /tmp
near 'types long'       "sed 's/[0-9]/X/g'" df --print-type /tmp
same 'not an option'    df -W

group env
# The shell sets _ to the path of the command it is about to run, so the two
# tools disagree on that one line by construction and it is dropped.
near 'listing'          "grep -v '^_='" env
same 'empty'            env -i
same 'set and run'      env NEW=here /bin/sh -c 'echo $NEW'
same 'empty and run'    env -i /bin/sh -c 'echo [$PATH]'
same 'given path'       env -i PATH=/bin:/usr/bin echo through
same 'empty path'       env -i PATH= echo not-through
same 'unset and run'    env -u HOME /bin/sh -c 'echo [$HOME]'
same 'plain command'    env /bin/echo through
same 'unset long'       env --unset=HOME -i A=1 B=2
same 'unset twice'      env -u A -u C -i A=1 B=2 C=3
same 'ignore long'      env --ignore-environment A=1 B=2
same 'null'             env -i -0 A=1 B=2
same 'null long'        env -i --null A=1 B=2
same 'a lone dash'      env - A=1
same 'change directory' env -C /usr /bin/pwd
same 'directory long'   env --chdir=/usr /bin/pwd
same 'no directory'     env -C /nowhere /bin/pwd
same 'zeroth argument'  env -a zero /bin/sh -c 'echo $0'
same 'zeroth long'      env --argv0=zero /bin/sh -c 'echo $0'
same 'split string'     env -S'/bin/echo one two'
same 'split long'       env --split-string='/bin/echo three four'
same 'nothing to run'   env -C /usr
same 'not an option'    env -x /bin/true
env_stress
exec_path_stress

group chown
effect 'user and group' chown '$TOOL '"$(id -un):$(id -gn)"' tree/one'
effect 'numeric'        chown '$TOOL '"$(id -u):$(id -g)"' tree/one'
effect 'user only'      chown '$TOOL '"$(id -un)"' tree/one'
effect 'group only'     chown '$TOOL :'"$(id -gn)"' tree/two'
effect 'recursive'      chown '$TOOL -R '"$(id -un):$(id -gn)"' tree'
effect 'unknown user'   chown '$TOOL nosuchuser tree/one'
effect 'through link'   chown '$TOOL '"$(id -un)"' link'
effect 'not the link'   chown '$TOOL -h '"$(id -un)"' link'
effect 'verbose'        chown '$TOOL -v '"$(id -un)"' tree/one > said'
effect 'verbose group'  chown '$TOOL -v '"$(id -un):$(id -gn)"' tree/one > said'
effect 'changes'        chown '$TOOL -c '"$(id -un)"' tree/one > said'
effect 'quiet'          chown '$TOOL -f '"$(id -un)"' nothing > said 2>&1'
effect 'reference'      chown '$TOOL --reference=tree/two tree/one'
effect 'reference gone' chown '$TOOL --reference=nothing tree/one'
effect 'not an option'  chown '$TOOL -W '"$(id -un)"' tree/one'
effect 'nofollow then follow' chown 'ln -s absent broken; $TOOL -h --dereference '"$(id -un)"' broken'
effect 'follow then nofollow' chown 'ln -s absent broken; $TOOL --dereference -h '"$(id -un)"' broken'

group chgrp
effect 'named group'     chgrp '$TOOL '"$(id -gn)"' tree/one'
effect 'numeric group'   chgrp '$TOOL '"$(id -g)"' tree/one'
effect 'recursive'       chgrp '$TOOL -R '"$(id -gn)"' tree'
effect 'unknown group'   chgrp '$TOOL nosuchgroup tree/one'
effect 'through link'    chgrp '$TOOL '"$(id -gn)"' link'
effect 'not the link'    chgrp '$TOOL -h '"$(id -gn)"' link'
effect 'verbose'         chgrp '$TOOL -v '"$(id -gn)"' tree/one > said'
effect 'changes'         chgrp '$TOOL -c '"$(id -gn)"' tree/one > said'
effect 'quiet'           chgrp '$TOOL -f '"$(id -gn)"' nothing > said 2>&1'
effect 'reference'       chgrp '$TOOL --reference=tree/two tree/one'
effect 'reference gone'  chgrp '$TOOL --reference=nothing tree/one'
effect 'not an option'   chgrp '$TOOL -W '"$(id -gn)"' tree/one'
effect 'nofollow then follow' chgrp 'ln -s absent broken; $TOOL -h --dereference '"$(id -gn)"' broken'
effect 'follow then nofollow' chgrp 'ln -s absent broken; $TOOL --dereference -h '"$(id -gn)"' broken'

group sleep
timing 'fraction'       0.3
timing 'seconds'        1
timing 'with suffix'    2s
timing 'and a half'     1.5
same 'bad interval'     sleep nonsense

group mkdir
effect 'plain'          mkdir '$TOOL made'
effect 'parents'        mkdir '$TOOL -p a/b/c/d'
effect 'parents exist'  mkdir '$TOOL -p tree/deep/deeper'
effect 'mode'           mkdir '$TOOL -m 0700 walled'
effect 'parents mode'   mkdir '$TOOL -p -m 0705 x/y'
effect 'parents on file' mkdir '$TOOL -p plain'
effect 'parent is file' mkdir '$TOOL -p plain/child'
answered 'overlong parents' mkdir -p "$long_path"

group rmdir
effect 'empty'          rmdir 'mkdir gone; $TOOL gone'
effect 'not empty'      rmdir '$TOOL tree'
effect 'parents'        rmdir 'mkdir -p a/b/c; $TOOL -p a/b/c'

group chmod
effect 'octal'          chmod '$TOOL 0600 tree/one'
effect 'octal leading'  chmod '$TOOL 0000600 tree/one'
effect 'octal bad tail' chmod '$TOOL 0788 tree/one 2>/dev/null || :'
effect 'octal directory' chmod '$TOOL 0711 tree/deep'
effect 'symbolic add'   chmod '$TOOL u+x tree/one'
effect 'symbolic remove' chmod '$TOOL go-rwx tree/two'
effect 'symbolic set'   chmod '$TOOL a=rw tree/one'
effect 'symbolic many'  chmod '$TOOL u+rw,g=r,o-rwx tree/two'
effect 'capital x'      chmod '$TOOL -R a+X tree'
effect 'recursive'      chmod '$TOOL -R 0755 tree'
effect 'setuid'         chmod '$TOOL 4755 tree/one'
effect 'sticky'         chmod '$TOOL 1777 tree/deep'
effect 'through link'   chmod '$TOOL 0600 link'
effect 'verbose'        chmod '$TOOL -v 0600 tree/one > said'
effect 'verbose retained' chmod '$TOOL -v 0640 tree/two > said'
effect 'changes'        chmod '$TOOL -c 0600 tree/one > said'
effect 'changes retained' chmod '$TOOL -c 0640 tree/two > said'
effect 'verbose recursive' chmod '$TOOL -Rv a+rX tree | LC_ALL=C sort > said'
effect 'verbose setuid' chmod '$TOOL -v 4755 tree/one > said'
effect 'verbose sticky' chmod '$TOOL -v 1777 tree/deep > said'
effect 'quiet'          chmod '$TOOL -f 0600 nothing > said 2>&1'
effect 'loud'           chmod '$TOOL 0600 nothing > said 2>&1'
effect 'reference'      chmod '$TOOL --reference=tree/two tree/one'
effect 'reference many' chmod '$TOOL --reference=tree/two tree/one plain'
effect 'reference gone' chmod '$TOOL --reference=nothing tree/one'
effect 'capital x on a file' chmod '$TOOL a+X plain'
effect 'capital x once set' chmod 'chmod 0700 tree/one; $TOOL -R go+X tree'
effect 'capital x equals' chmod '$TOOL -R a=rwX tree'
effect 'not an option'  chmod '$TOOL -W 0600 tree/one'
effect 'broken link'    chmod 'ln -s absent broken; $TOOL 0600 broken'
effect 'quiet broken link' chmod 'ln -s absent broken; $TOOL -f 0600 broken'

group ln
effect 'symbolic'       ln '$TOOL -s tree/one pointer'
effect 'symbolic force' ln '$TOOL -s tree/two link'
effect 'hard'           ln '$TOOL tree/one hard'
effect 'into directory' ln '$TOOL -s ../plain tree/'
effect 'forced'         ln '$TOOL -sf tree/two link'
effect 'relative'       ln '$TOOL -sr plain tree/deep/rel'
effect 'relative deep'  ln '$TOOL -sr tree/one tree/deep/deeper/rel'
effect 'relative up'    ln '$TOOL -sr tree/deep/three near'
effect 'relative long'  ln '$TOOL -s --relative plain tree/rel'
effect 'no dereference' ln 'ln -s tree here; $TOOL -sfn plain here'
effect 'through a link' ln 'ln -s tree here; $TOOL -sf plain here'
effect 'target dir'     ln '$TOOL -st tree plain'
effect 'no target dir'  ln '$TOOL -sT tree/one aimed'
effect 'no target over' ln '$TOOL -sT tree/one tree'
effect 'interactive no' ln '$TOOL -si tree/two link < /dev/null'
effect 'interactive yes' ln 'printf "y\n" | $TOOL -si tree/two link'
effect 'force then interactive' ln '$TOOL -f -i -s tree/two link < /dev/null'
effect 'interactive then force' ln '$TOOL -i -f -s tree/two link < /dev/null'
effect 'hard many'      ln '$TOOL tree/one tree/two tree/deep/'
effect 'verbose'        ln '$TOOL -sv tree/one pointer > said'
effect 'through the link' ln '$TOOL -L link followed'
effect 'the link itself' ln '$TOOL -P link kept'
effect 'not an option'  ln '$TOOL -W tree/one pointer'

group touch
effect 'create'         touch '$TOOL fresh'
effect 'no create'      touch '$TOOL -c absent'
effect 'reference'      touch '$TOOL -r tree/one tree/two'
effect 'modify only'    touch '$TOOL -m -r tree/one tree/two'
effect 'access only'    touch '$TOOL -a -r tree/one tree/two'
effect 'epoch date'     touch '$TOOL -d @1234567890 tree/two'
effect 'epoch on new'   touch '$TOOL -d @1000000000 born'
effect 'modify epoch'   touch '$TOOL -m -d @1234567890 tree/one'
effect 'bad date'       touch '$TOOL -d nonsense tree/one'
effect 'many'           touch '$TOOL -d @900000000 tree/one tree/two plain'
effect 'written day'    touch '$TOOL -d 2001-09-09 tree/one'
effect 'written moment' touch '$TOOL -d "2001-09-09 01:46:40" tree/one'
effect 'written back'   touch '$TOOL -d "2001-09-09 1 day ago" tree/one'
effect 'written month'  touch '$TOOL -d "2001-09-09 +1 month" tree/one'
effect 'old stamp'      touch '$TOOL -t 200109090146 tree/one'
effect 'old stamp century' touch '$TOOL -t 20010909014640 tree/one'
effect 'old stamp seconds' touch '$TOOL -t 200109090146.40 tree/one'
effect 'old stamp short' touch '$TOOL -t 09090146 tree/one'
effect 'old stamp two digit' touch '$TOOL -t 0109090146 tree/one'
effect 'old stamp last century' touch '$TOOL -t 6909090146 tree/one'
effect 'old stamp this one' touch '$TOOL -t 6809090146 tree/one'
effect 'old stamp bad'   touch '$TOOL -t 99 tree/one'
effect 'old stamp month' touch '$TOOL -t 202113011234 tree/one'
effect 'date long'      touch '$TOOL --date=2001-09-09 tree/one'
effect 'reference long' touch '$TOOL --reference=tree/one tree/two'
effect 'no create long' touch '$TOOL --no-create absent'
effect 'time access'    touch '$TOOL --time=access -d @5 tree/one'
effect 'time modify'    touch '$TOOL --time=modify -d @5 tree/one'
effect 'time of nothing' touch '$TOOL --time=furlongs -d @5 tree/one'
effect 'not the link'   touch '$TOOL -h -d @5 link'
effect 'through a link' touch '$TOOL -d @5 link'
effect 'through broken link' touch 'ln -s absent broken; $TOOL broken'
effect 'no create broken link' touch 'ln -s absent broken; $TOOL -c broken'
effect 'broken link itself' touch 'ln -s absent broken; $TOOL -h -d @5 broken'
effect 'not an option'  touch '$TOOL -W tree/one'

group cp
large_copy_stress
effect 'file'           cp '$TOOL tree/one copy'
effect 'over file'      cp '$TOOL tree/one plain'
effect 'into directory' cp '$TOOL plain tree/'
effect 'recursive'      cp '$TOOL -r tree copied'
effect 'preserving'     cp '$TOOL -rp tree kept'
effect 'many into dir'  cp '$TOOL tree/one tree/two tree/deep/'
# A link named on the command line is followed and a link found inside a tree
# is not, which is the one place cp reads its own operands two ways.
effect 'link followed'  cp '$TOOL link followed'
effect 'link kept'      cp '$TOOL -P link kept'
effect 'link kept by d' cp '$TOOL -d link kept'
effect 'link in a tree' cp 'mkdir box; ln -s ../plain box/inside; $TOOL -r box copied'
effect 'link followed in tree' cp 'mkdir box; ln -s ../plain box/inside; $TOOL -rL box copied'
effect 'archive'        cp '$TOOL -a tree arch'
effect 'archive a link' cp '$TOOL -a link arch'
effect 'target dir'     cp '$TOOL -t tree/deep tree/one plain'
effect 'target dir long' cp '$TOOL --target-directory=tree/deep plain'
effect 'no target dir'  cp '$TOOL -T tree/one aimed'
effect 'no target tree' cp '$TOOL -rT tree/deep aimed'
effect 'no clobber'     cp '$TOOL -n tree/one plain'
effect 'no clobber new' cp '$TOOL -n tree/one fresh'
effect 'update older'   cp '$TOOL -u tree/one plain'
effect 'update newer'   cp '$TOOL -u plain tree/one'
effect 'hard link'      cp '$TOOL -l tree/one hard'
effect 'hard link tree' cp '$TOOL -rl tree linked'
effect 'symbolic'       cp '$TOOL -s plain pointed'
effect 'symbolic away'  cp '$TOOL -s plain tree/pointed'
effect 'interactive no' cp '$TOOL -i tree/one plain < /dev/null'
effect 'interactive yes' cp 'printf "y\n" | $TOOL -i tree/one plain'
effect 'no clobber then interactive' cp '$TOOL -n -i tree/one plain < /dev/null'
effect 'interactive then no clobber' cp '$TOOL -i -n tree/one plain < /dev/null'
effect 'physical then command line' cp '$TOOL -P -H link kept'
effect 'command line then physical' cp '$TOOL -H -P link kept'
effect 'verbose'        cp '$TOOL -rv tree copied | LC_ALL=C sort > said'
effect 'verbose one'    cp '$TOOL -v tree/one copy > said'
effect 'not an option'  cp '$TOOL -W tree/one copy'
effect 'both targets'   cp '$TOOL -T -t tree tree/one'
effect 'same file'      cp '$TOOL tree/one tree/one'
effect 'same hard link' cp 'ln tree/one alias; $TOOL tree/one alias'

group mv
effect 'rename'         mv '$TOOL plain renamed'
effect 'into directory' mv '$TOOL plain tree/'
effect 'directory'      mv '$TOOL tree moved'
effect 'over file'      mv '$TOOL tree/one tree/two'
effect 'many'           mv '$TOOL tree/one tree/two tree/deep/'
effect 'no clobber'     mv '$TOOL -n tree/one plain'
effect 'no clobber new' mv '$TOOL -n tree/one fresh'
effect 'interactive no' mv '$TOOL -i tree/one plain < /dev/null'
effect 'interactive yes' mv 'printf "y\n" | $TOOL -i tree/one plain'
effect 'force then interactive' mv '$TOOL -f -i tree/one plain < /dev/null'
effect 'interactive then force' mv '$TOOL -i -f tree/one plain < /dev/null'
effect 'no clobber then force' mv '$TOOL -n -f tree/one plain'
effect 'force then no clobber' mv '$TOOL -f -n tree/one plain'
effect 'forced'         mv '$TOOL -f tree/one plain'
effect 'target dir'     mv '$TOOL -t tree/deep plain'
effect 'target dir long' mv '$TOOL --target-directory=tree/deep plain'
effect 'no target dir'  mv '$TOOL -T tree/deep elsewhere'
effect 'no target extra' mv '$TOOL -T tree/one tree/two tree/deep'
effect 'verbose'        mv '$TOOL -v plain renamed > said'
effect 'not an option'  mv '$TOOL -W plain renamed'
effect 'same file'      mv '$TOOL tree/one tree/one'
effect 'same hard link' mv 'ln tree/one alias; $TOOL tree/one alias'

group rm
effect 'file'           rm '$TOOL plain'
effect 'directory'      rm '$TOOL tree'
effect 'recursive'      rm '$TOOL -r tree'
effect 'forced missing' rm '$TOOL -f nothing'
effect 'recursive force' rm '$TOOL -rf tree plain nothing'
effect 'link'           rm '$TOOL link'
effect 'empty directory' rm 'mkdir hole; $TOOL -d hole'
effect 'directory in use' rm '$TOOL -d tree'
effect 'directory forced' rm '$TOOL -fd tree'
effect 'dir long'       rm 'mkdir hole; $TOOL --dir hole'
effect 'interactive no' rm '$TOOL -i plain < /dev/null'
effect 'interactive yes' rm 'printf "y\n" | $TOOL -i plain'
effect 'force then interactive' rm '$TOOL -f -i plain < /dev/null'
effect 'interactive then force' rm '$TOOL -i -f plain < /dev/null'
effect 'interactive tree' rm '$TOOL -ri tree < /dev/null'
effect 'verbose'        rm '$TOOL -rv tree | LC_ALL=C sort > said'
effect 'verbose one'    rm '$TOOL -v plain > said'
effect 'verbose a directory' rm 'mkdir hole; $TOOL -dv hole > said'
effect 'one file system' rm '$TOOL -r --one-file-system tree'
effect 'not an option'  rm '$TOOL -W plain'

#
#       Names that are awkward to hold.
#
#       A space, a leading dash, a newline: every one of these has been a
#       whole class of bug in a tool that built its output by pasting words
#       together, and nothing above uses a name harder than "beta.txt".
#

odd=$work/odd
mkdir -p "$odd/a dir"
printf 'x\n' > "$odd/with space"
printf 'x\n' > "$odd/-dash"
printf 'xx\n' > "$odd/two  spaces"
printf 'x\n' > "$odd/a dir/inner file"
printf 'x\n' > "$odd/dot.in.the.middle"
ln -s "with space" "$odd/link to space"
ln -s nowhere "$odd/broken"
ln -s loop "$odd/loop"

group awkward
same 'basename space'   basename "$odd/with space"
same 'basename dash'    basename "$odd/-dash"
same 'dirname space'    dirname "$odd/a dir/inner file"
same 'realpath space'   realpath "$odd/with space"
same 'readlink space'   readlink "$odd/link to space"
same 'readlink broken'  readlink "$odd/broken"
same 'stat space'       stat "$odd/with space"
near 'stat link'        "grep -v '^Access: 2'" stat "$odd/link to space"
near 'stat broken'      "grep -v '^Access: 2'" stat "$odd/broken"
near 'stat loop'        "grep -v '^Access: 2'" stat "$odd/loop"
same 'long listing'     ls -l "$odd"
same 'long inner'       ls -l "$odd/a dir"
near 'listed'           'cat' ls "$odd"
near 'found'            "LC_ALL=C sort" find "$odd"
answered 'find follows broken link' find -L "$odd/broken" -type l
near 'measured'         "LC_ALL=C sort" du -a "$odd"
same 'realpath of link' realpath "$odd/link to space"
same 'realpath broken'  realpath "$odd/broken"
same 'realpath loop'    realpath "$odd/loop"
effect 'copy a space'   cp '$TOOL tree/one "a name with spaces"'
effect 'move a space'   mv '$TOOL tree/one "a name with spaces"'
effect 'remove a space' rm '$TOOL "nothing here" 2>/dev/null'
effect 'make a space'   mkdir '$TOOL "a made dir"'
effect 'touch a space'  touch '$TOOL "a touched file"'
effect 'link a space'   ln '$TOOL -s "tree/one" "a linked name"'

#       mktemp, whose whole point is a name nobody can predict -- so what the
#       two tools print cannot be compared with each other directly. What is
#       compared is the contract both of them are held to: the name is the
#       template with every X replaced by something, in the directory the
#       options asked for, and what each tool left on disk is the same tree.

# The printed name, with the template's X positions blanked out. A name of a
# different length cannot be shaped and says so, which is the case where a
# tool replaced too few characters or none at all.
shaped() {
        given=$1
        pattern=$2
        made=""
        at=1

        if [ "${#given}" != "${#pattern}" ]; then
                printf 'wrong length'
                return 0
        fi

        while [ "$at" -le "${#pattern}" ]; do
                letter=$(printf '%s' "$pattern" | cut -c "$at")

                if [ "$letter" = X ]; then
                        made="$made#"
                else
                        made="$made$(printf '%s' "$given" | cut -c "$at")"
                fi

                at=$((at + 1))
        done

        printf '%s' "$made"
}

temporary() {
        name=$1
        pattern=$2
        shift 2

        rm -rf "$work/a" "$work/b"
        mkdir -p "$work/a" "$work/b"

        if want=$(cd "$work/a" && TMPDIR=$work/a mktemp "$@" 2>/dev/null); then
                want_status=0
        else
                want_status=$?
        fi

        if got=$(cd "$work/b" && TMPDIR=$work/b "$binaries/mktemp" "$@" 2>/dev/null); then
                got_status=0
        else
                got_status=$?
        fi

        if [ "$want_status" != "$got_status" ]; then
                report bad "$name" "want status $want_status, got status $got_status"
                return 0
        fi

        if [ "$want_status" != 0 ]; then
                want=nothing-was-made
                got=nothing-was-made
        else
                wanted=$(printf '%s' "$pattern" | tr X '#')
                want_shape=$(shaped "${want##*/}" "$pattern")
                got_shape=$(shaped "${got##*/}" "$pattern")
                want_where=.
                got_where=.

                case $want in */*) want_where=$(printf '%s' "${want%/*}" | sed "s|$work/a|DIR|") ;; esac
                case $got in */*) got_where=$(printf '%s' "${got%/*}" | sed "s|$work/b|DIR|") ;; esac

                if [ "$want_shape" != "$wanted" ] || [ "$got_shape" != "$wanted" ]; then
                        report bad "$name" "wanted $wanted, want [$want_shape] got [$got_shape]"
                        return 0
                fi

                # Blanking the X positions cannot tell an answer from the
                # question, so the question being given back is caught here.
                if [ "${want##*/}" = "$pattern" ] || [ "${got##*/}" = "$pattern" ]; then
                        report bad "$name" "the template came back unchanged"
                        return 0
                fi

                if [ "$want_where" != "$got_where" ]; then
                        report bad "$name" "want in $want_where, got in $got_where"
                        return 0
                fi
        fi

        # A run that made nothing has no name to mask out.
        dump "$work/a" | sed "s|${want##*/}|NAME|g" > "$work/want"
        dump "$work/b" | sed "s|${got##*/}|NAME|g" > "$work/got"

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "$(diff "$work/want" "$work/got" | head -4 | tr '\n' '|')"
}

group mktemp
temporary 'default'            tmp.XXXXXXXXXX
temporary 'default directory'  tmp.XXXXXXXXXX -d
temporary 'default unmade'     tmp.XXXXXXXXXX -u
temporary 'template'           run.XXXXXX     run.XXXXXX
temporary 'template directory' run.XXXXXX     -d run.XXXXXX
temporary 'template unmade'    run.XXXXXX     -u run.XXXXXX
temporary 'long field'         run.XXXXXXXXXXXX run.XXXXXXXXXXXX
temporary 'least field'        run.XXX        run.XXX
temporary 'suffix kept'        run.XXXXXX.log run.XXXXXX.log
temporary 'too few'            run.XX         run.XX
temporary 'no field'           run            run
temporary 'in tmpdir'          run.XXXXXX     -t run.XXXXXX
temporary 'quiet failure'      run.XX         -q run.XX
temporary 'unmade directory'   run.XXXXXX     -u -d run.XXXXXX
temporary 'missing directory'  run.XXXXXX     sub/run.XXXXXX
answered 'overlong tmpdir' mktemp --tmpdir="$long_path"

#       date, which is the epoch turned into a date and then into whatever
#       the format asked for. TZ is UTC0 at the top of this file for the same
#       reason it is set for ls and stat: ours has no timezone database and
#       the system's has to be told not to use one either.
#
#       The listed cases are the shapes; the swept ones are where the bugs in
#       a calendar live. Each sweep runs a hundred and fifty five epochs from
#       1900 to 2098 through one format, both ways, and compares the lot --
#       which is how a leap year or a month boundary gets found by something
#       other than luck.

epochs="0 1 -1 86399 86400 951782400 951868800 68169600 1709164800 1709251200
2147483647 -2208988800 4102444800 1000000000 1234567890 1709251199 946684800"

at=0
while [ "$at" -lt 138 ]; do
        epochs="$epochs $((-2208988800 + at * 45000000))"
        at=$((at + 1))
done

swept() {
        name=$1
        shape=$2

        for moment in $epochs; do date -d "@$moment" "$shape"; done > "$work/want" 2>&1
        for moment in $epochs; do "$binaries/date" -d "@$moment" "$shape"; done > "$work/got" 2>&1

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "$(diff "$work/want" "$work/got" | head -2 | tr '\n' '|')"
}

# A date said from now moves while it is being asked for: the two tools run
# one after the other and the answer is a second apart whenever the pair
# straddles a tick. What does not move is the distance from the moment each
# one was asked, so that is what is compared, and a second of slack is left
# for the tick that falls between reading the clock and reading the date.
adrift() {
        name=$1
        expression=$2

        base=$(date +%s)
        want=$(( $(date -d "$expression" +%s) - base ))
        base=$(date +%s)
        got=$(( $("$binaries/date" -d "$expression" +%s) - base ))
        apart=$((want - got))

        if [ "$apart" -le 1 ] && [ "$apart" -ge -1 ]; then
                report ok
                return 0
        fi

        report bad "$name" "want ${want}s from now, got ${got}s"
}

# A written date swept across two centuries. The last day of February said
# as the first of March less one is the leap year rule and the calendar
# arithmetic in the same expression, and it has to hold every year.
sweep_written() {
        name=$1
        shape=$2

        for year in $(seq 1900 2100); do
                date -d "$(eval echo "$shape")" +%F || true
        done > "$work/want" 2>&1

        for year in $(seq 1900 2100); do
                "$binaries/date" -d "$(eval echo "$shape")" +%F || true
        done > "$work/got" 2>&1

        if cmp -s "$work/want" "$work/got"; then
                report ok
                return 0
        fi

        report bad "$name" "$(diff "$work/want" "$work/got" | head -2 | tr '\n' '|')"
}

# Every written date read by both, printed to the second.
reads() {
        name=$1
        expression=$2

        same "$name" date -d "$expression" '+%Y-%m-%d %H:%M:%S'
}

group date
same 'epoch'             date -d @1000000000
same 'epoch utc'         date -u -d @1000000000
same 'long form'         date --date=@1000000000
same 'iso'               date -d @1000000000 +%F
same 'time'              date -d @1000000000 +%T
same 'both'              date -d @1000000000 '+%Y-%m-%d %H:%M:%S'
same 'names'             date -d @1000000000 '+%a %A %b %B'
same 'counts'            date -d @1000000000 '+%j %u %w %y %C %e'
same 'twelve hour'       date -d @1000000000 '+%I %p %l %P %r'
same 'seconds out'       date -d @1000000000 +%s
same 'zone'              date -d @1000000000 '+%Z %z'
same 'literals'          date -d @1000000000 '+a%%b%nc%td'
same 'no padding'        date -d @1000000000 '+%-d/%-m/%-H'
same 'space padding'     date -d @1000000000 '+%_d|%_m|%_H'
same 'zero padding'      date -d @1000000000 '+%0e|%0k|%0l'
same 'weeks'             date -d @1000000000 '+%U %W %V %G %g'
same 'quarter'           date -d @1000000000 '+%q %N'
same 'grouped'           date -d @1000000000 '+%c|%x|%X|%D|%R'
same 'rfc'               date -R -d @1000000000
same 'before the epoch'  date -d @-1
same 'the epoch itself'  date -d @0
same 'a leap day'        date -d @951782400 +%F
same 'the day after'     date -d @951868800 +%F
same 'from a file'       date -r "$fixture/alpha" +%F
same 'empty format'      date -d @1000000000 +
same 'unknown letter'    date -d @1000000000 +%q%%

#       Written dates. Every one of these is anchored on a day rather than on
#       now, so the answer is the same whenever the suite is run.
reads 'a day'            '2001-09-09'
reads 'a day and a time' '2001-09-09 01:46:40'
reads 'with a T'         '2001-09-09T01:46:40'
reads 'to the minute'    '2001-09-09 01:46'
reads 'one digit fields' '2001-9-9'
reads 'a two digit year' '01-09-09'
reads 'the other century' '69-09-09'
reads 'the year before'  '68-09-09'
reads 'a leap day'       '2000-02-29'
reads 'the last second'  '1999-12-31 23:59:59'
reads 'before the epoch' '1900-01-01'
reads 'far ahead'        '2100-06-15 12:00:00'
reads 'a time alone'     '2001-09-09 12:00'
reads 'saying utc'       '2001-09-09 UTC'
reads 'a time and utc'   '2001-09-09 12:00 UTC'
reads 'a day on'         '2001-09-09 +1 day'
reads 'a day back'       '2001-09-09 -1 day'
reads 'a day ago'        '2001-09-09 1 day ago'
reads 'yesterday from'   '2001-09-09 yesterday'
reads 'tomorrow from'    '2001-09-09 tomorrow'
reads 'a month on'       '2001-09-09 +1 month'
reads 'a year back'      '2001-09-09 -1 year'
reads 'over a year end'  '2001-09-09 +4 months'
reads 'the month rolls'  '2024-01-31 +1 month'
reads 'the month rolls back' '2024-03-31 -1 month'
reads 'a leap year on'   '2024-02-29 +1 year'
reads 'next week'        '2001-09-09 next week'
reads 'last month'       '2001-09-09 last month'
reads 'a bare unit'      '2001-09-09 day'
reads 'a fortnight'      '2001-09-09 fortnight'
reads 'two of them'      '2001-09-09 +2 fortnights'
reads 'hours'            '2001-09-09 3 hours'
reads 'minutes back'     '2001-09-09 -90 minutes'
reads 'seconds'          '2001-09-09 5 seconds'
reads 'a second back'    '2001-09-09 -1 sec'
reads 'a minute'         '2001-09-09 1 min'
reads 'three at once'    '2001-09-09 1 hour 1 min 1 sec'
reads 'two and ago'      '2001-09-09 2 days 3 hours ago'
reads 'ago the other way' '2001-09-09 3 hours 2 days ago'
reads 'three and ago'    '2001-09-09 1 hour 1 min 1 sec ago'
reads 'after a time'     '2001-09-09 01:46:40 1 day ago'
reads 'weeks'            '2001-09-09 2 weeks'
reads 'a fraction'       '@1000000000.5'
reads 'before the epoch by hand' '@-100'
reads 'in capitals'      '2001-09-09 YESTERDAY'

sweep_written 'every february'  '$year-03-01 -1 day'
sweep_written 'every new year'  '$year-12-31 +1 day'
sweep_written 'a year on'       '$year-02-29 +1 year'
sweep_written 'a month on'      '$year-01-31 +1 month'
sweep_written 'twelve months'   '$year-06-15 +12 months'

# Now-relative, compared as a distance rather than as a moment.
adrift 'now'             'now'
adrift 'today'           'today'
adrift 'yesterday'       'yesterday'
adrift 'tomorrow'        'tomorrow'
adrift 'a week on'       '+1 week'
adrift 'an hour back'    '-1 hour'
adrift 'two days ago'    '2 days ago'
adrift 'next month'      'next month'
adrift 'last year'       'last year'

# What it will not read is refused, and refused the same way.
answered 'a date it cannot read' date -d nonsense
answered 'an epoch with more' date -d '@1000000000 +1 day'
answered 'a month past twelve' date -d '2001-13-09'
answered 'a day past thirty one' date -d '2001-09-32'
answered 'a day the month has not' date -d '2001-09-31'
answered 'february in a plain year' date -d '1900-02-29'
reads 'february in a leap one' '2000-02-29'
answered 'an hour past twenty three' date -d '2001-09-09 24:00'
answered 'a date said twice' date -d '2001-09-09 2002-01-01'
answered 'a time said twice' date -d '2001-09-09 01:00 02:00'
answered 'an empty date'    date -d ''
same 'iso day'           date -d @1000000000 -I
same 'iso hours'         date -d @1000000000 -Ihours
same 'iso minutes'       date -d @1000000000 -Iminutes
same 'iso seconds'       date -d @1000000000 -Iseconds
same 'iso long'          date -d @1000000000 --iso-8601=seconds
same 'iso long alone'    date -d @1000000000 --iso-8601
answered 'iso of nothing' date -d @1000000000 -Ifurlongs
same 'reference long'    date --reference="$fixture/alpha" +%F
answered 'a file that is not there' date -r "$fixture/nothing"
answered 'setting the time' date 1000000000
answered 'an option it has not' date -x

swept 'swept iso'        '+%Y-%m-%d'
swept 'swept time'       '+%H:%M:%S'
swept 'swept names'      '+%a %A %b %B'
swept 'swept counts'     '+%j %u %w %y %C'
swept 'swept weeks'      '+%U %W %V %G %g'
swept 'swept padding'    '+%e|%-d|%_m|%0k|%l'
swept 'swept seconds'    '+%s %q'
swept 'swept default'    '+%a %b %e %H:%M:%S %Z %Y'

printf '  %-12s %s of %s\n' files "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'files %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
