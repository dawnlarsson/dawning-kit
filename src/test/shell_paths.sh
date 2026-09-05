#!/bin/sh
#
# Focused logical/physical directory policy and its Bash-only set options.
#
#     sh src/test/shell_paths.sh [moonwater-shell]
#
set -u

subject=${1:-/tmp/mwsh}

[ -x "$subject" ] || {
        echo "no shell at $subject" >&2
        exit 1
}

[ -x /bin/bash ] || {
        echo "  shell_paths NOT RUN -- no /bin/bash" >&2
        exit 2
}

[ -x /bin/dash ] || {
        echo "  shell_paths NOT RUN -- no /bin/dash" >&2
        exit 2
}

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" "$work/tree" "$work/tree/real" \
        "$work/tree/real/child" "$work/tree/search" || exit 1
ln -s real/child "$work/tree/link" || exit 1
ln -s ../real/child "$work/tree/search/place" || exit 1
ln -s loop "$work/tree/loop" || exit 1
ln -s "$subject" "$work/names/bash" || exit 1
ln -s "$subject" "$work/names/dash" || exit 1

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

section paths
group paths

run()
{
        shell=$1
        script=$2
        output=$3
        status_file=$4

        if timeout 5 env ROOT="$work/tree" "$shell" -c "$script" \
                > "$output" 2>/dev/null; then
                status=0
        else
                status=$?
        fi

        printf '%s\n' "$status" > "$status_file"
}

case_compare()
{
        name=$1
        reference=$2
        candidate=$3
        script=$4

        run "$reference" "$script" "$work/want" "$work/want.status"
        run "$candidate" "$script" "$work/got" "$work/got.status"

        if cmp -s "$work/want" "$work/got" &&
                cmp -s "$work/want.status" "$work/got.status"; then
                won
                return
        fi

        lost "$name" \
                "want $(tr '\n' '|' < "$work/want")[$(cat "$work/want.status")], got $(tr '\n' '|' < "$work/got")[$(cat "$work/got.status")]"
}

B=/bin/bash
D=/bin/dash
MB=$work/names/bash
MD=$work/names/dash

case_compare 'physical letter state' "$B" "$MB" \
        'set -P; case $- in *P*) echo on;; *) echo off;; esac; set +P; case $- in *P*) echo bad;; *) echo off;; esac'
case_compare 'physical named state' "$B" "$MB" \
        'set -o physical; case $- in *P*) echo on;; esac; set +o physical; case $- in *P*) echo bad;; *) echo off;; esac'
case_compare 'onecmd option state' "$B" "$MB" \
        'set -o onecmd; case $- in *t*) echo on;; esac; set +t; case $- in *t*) echo bad;; *) echo off;; esac'
case_compare 'shopt physical state' "$B" "$MB" \
        'shopt -so physical; printf "%s:" "$-"; shopt -qo physical; echo $?; shopt -uo physical; shopt -qo physical; echo $?'
case_compare 'shopt physical command' "$B" "$MB" \
        'shopt -op physical; shopt -so physical; shopt -op physical'
case_compare 'dash rejects physical letter' "$D" "$MD" \
        'set -P; echo forbidden'
case_compare 'dash rejects physical name' "$D" "$MD" \
        'set -o physical; echo forbidden'
case_compare 'dash rejects onecmd letter' "$D" "$MD" \
        'set -t; echo forbidden'

case_compare 'logical symlink default' "$B" "$MB" \
        'cd "$ROOT"; cd link; printf "%s:%s:%s\n" "$PWD" "$(pwd)" "$(pwd -P)"'
case_compare 'logical symlink dotdot' "$B" "$MB" \
        'cd "$ROOT"; cd link; cd ..; printf "%s:%s\n" "$PWD" "$(pwd -P)"'
case_compare 'physical letter symlink' "$B" "$MB" \
        'cd "$ROOT"; set -P; cd link; printf "%s:%s:%s\n" "$PWD" "$(pwd)" "$(pwd -P)"'
case_compare 'physical named symlink' "$B" "$MB" \
        'cd "$ROOT"; set -o physical; cd link; printf "%s:%s\n" "$PWD" "$(pwd)"'
case_compare 'physical symlink dotdot' "$B" "$MB" \
        'cd "$ROOT"; set -P; cd link/..; printf "%s:%s\n" "$PWD" "$(pwd -P)"'
case_compare 'explicit physical override' "$B" "$MB" \
        'cd "$ROOT"; set +P; cd -P link; printf "%s:%s\n" "$PWD" "$(pwd -L)"'
case_compare 'explicit logical override' "$B" "$MB" \
        'cd "$ROOT"; set -P; cd -L link; printf "%s:%s:%s\n" "$PWD" "$(pwd -L)" "$(pwd -P)"'
case_compare 'pwd option order' "$B" "$MB" \
        'cd "$ROOT"; cd -L link; printf "%s:%s\n" "$(pwd -LP)" "$(pwd -PL)"'
case_compare 'pwd physical default' "$B" "$MB" \
        'cd "$ROOT"; cd -L link; set -P; printf "%s:%s\n" "$(pwd)" "$(pwd -L)"'

case_compare 'logical CDPATH' "$B" "$MB" \
        'cd "$ROOT"; CDPATH="$ROOT/search"; cd place; printf "PWD=%s\n" "$PWD"'
case_compare 'physical CDPATH' "$B" "$MB" \
        'cd "$ROOT"; set -P; CDPATH="$ROOT/search"; cd place; printf "PWD=%s\n" "$PWD"'
case_compare 'cd dash prints destination' "$B" "$MB" \
        'cd "$ROOT"; cd real; cd -'
case_compare 'readonly OLDPWD still updates PWD' "$B" "$MB" \
        'cd "$ROOT"; readonly OLDPWD; cd real 2>/dev/null; printf "%s:%s:%s\n" "$?" "$PWD" "$OLDPWD"'
case_compare 'dash readonly OLDPWD update' "$D" "$MD" \
        'cd "$ROOT"; readonly OLDPWD; cd real 2>/dev/null; printf "%s:%s:%s\n" "$?" "$PWD" "$OLDPWD"'

case_compare 'Bash missing directory status' "$B" "$MB" \
        'cd "$ROOT/missing" 2>/dev/null; echo $?'
case_compare 'dash missing directory status' "$D" "$MD" \
        'cd "$ROOT/missing" 2>/dev/null; echo $?'
case_compare 'Bash HOME absent status' "$B" "$MB" \
        'unset HOME; cd 2>/dev/null; echo $?'
case_compare 'dash HOME absent status' "$D" "$MD" \
        'unset HOME; cd 2>/dev/null; echo $?'
case_compare 'Bash OLDPWD absent status' "$B" "$MB" \
        'unset OLDPWD; cd - 2>/dev/null; echo $?'
case_compare 'dash OLDPWD absent status' "$D" "$MD" \
        'unset OLDPWD; cd - 2>/dev/null; echo $?'
case_compare 'Bash empty directory status' "$B" "$MB" \
        'cd "" 2>/dev/null; echo $?'
case_compare 'Bash extra directory status' "$B" "$MB" \
        'cd "$ROOT" "$ROOT/real" 2>/dev/null; echo $?'
case_compare 'pwd rejects unknown option' "$B" "$MB" \
        'pwd -x >/dev/null 2>&1; echo $?'
case_compare 'symlink loop status' "$B" "$MB" \
        'cd "$ROOT/loop" 2>/dev/null; echo $?'
case_compare 'overlong path is not truncated' "$B" "$MB" \
        'name=$(awk '\''BEGIN { for (i = 0; i < 5000; i++) printf "a" }'\''); cd "$name" 2>/dev/null; echo $?'

case_compare 'unnamed physical success' "$B" "$MB" \
        'd="$ROOT/gone"; mkdir "$d"; cd "$d"; rmdir "$d"; cd -P . 2>/dev/null; printf "%s:%s\n" "$?" "${PWD#$ROOT}"'
case_compare 'unnamed physical exact failure' "$B" "$MB" \
        'd="$ROOT/gone"; mkdir "$d"; cd "$d"; rmdir "$d"; cd -Pe . 2>/dev/null; printf "%s:%s\n" "$?" "${PWD#$ROOT}"'

section ""
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
