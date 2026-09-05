#!/bin/sh
#
#       Dynamic declaration attributes and Bash-mode ulimit compatibility.
#
#       Usage: sh src/test/shell_variables.sh [shell]
#
# The subject is reached through both `bash` and `sh` links: basename is part
# of the contract being tested. Bash cases compare with the pinned host Bash;
# the final policy cases compare the ordinary identity with dash.

set -e

subject=${1:-/tmp/mwsh}
bash_reference=${BASH_REFERENCE:-/bin/bash}
dash_reference=${DASH_REFERENCE:-/bin/dash}

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x "$bash_reference" ] || { echo "no Bash at $bash_reference" >&2; exit 1; }
[ -x "$dash_reference" ] || { echo "no dash at $dash_reference" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

case $subject in
/*) target=$subject ;;
*) target=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

ln -s "$target" "$work/bash"
ln -s "$target" "$work/sh"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

shown()
{
        head -c 80 "$1" | tr '\n' '|'
}

compare()
{
        mode=$1
        name=$2
        shift 2

        printf '%s\n' "$*" > "$work/case.sh"

        if [ "$mode" = bash ]; then
                reference=$bash_reference
                ours=$work/bash
        else
                reference=$dash_reference
                ours=$work/sh
        fi

        if timeout 5 "$reference" "$work/case.sh" > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi

        if timeout 5 "$ours" "$work/case.sh" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
        else
                lost "$name" \
                        "want $(shown "$work/want")[$want_status] got $(shown "$work/got")[$got_status]"
        fi
}

section variables
group readonly

compare bash 'declare -r scoped value' \
        'x=global; f() { declare -r x=local; declare -p x; }; f; x=after; printf "%s\n" "$x"'
compare bash 'local -r scoped value' \
        'x=global; f() { local -r x=local; declare -p x; }; f; x=after; printf "%s\n" "$x"'
compare bash 'nested readonly visibility' \
        'x=global; inner() { printf "<%s>\n" "$x"; }; outer() { local -r x=outer; inner; declare -p x; }; outer; x=after; printf "<%s>\n" "$x"'
compare bash 'nested attribute unwind' \
        'x=global; outer() { local x=mutable; inner; printf "<%s>\n" "$x"; }; inner() { local -r x=inner; declare -p x; }; outer; printf "<%s>\n" "$x"'
compare bash 'readonly marks a local' \
        'x=global; f() { local x=local; readonly x; declare -p x; }; f; x=after; printf "<%s>\n" "$x"'
compare bash 'unset declaration unwinds' \
        'f() { declare -r absent; declare -p absent; }; f; declare -p absent 2>/dev/null; printf "%s\n" "$?"'
compare bash 'readonly assignment status' \
        'x=outer; (f() { local x=inner; readonly x; x=bad; echo no; }; f) 2>/dev/null; printf "%s:%s\n" "$?" "$x"'
compare bash 'readonly cannot be hidden' \
        'readonly x=global; f() { local x=local; printf "%s:<%s>\n" "$?" "$x"; }; f 2>/dev/null'
compare bash 'nested declare readonly' \
        'x=global; outer() { local x=outer; inner; x=changed; printf "<%s>\n" "$x"; }; inner() { declare -r x=inner; declare -p x; }; outer; printf "<%s>\n" "$x"'
compare bash 'readonly read status' \
        'r=old; f() { local -r r=keep; printf new | read r; printf "%s:%s\n" "$?" "$r"; }; f'

section identity
group basename

compare bash 'Bash identity variables' \
        'printf "%s|%s\n" "$BASH_VERSION" "${BASH_VERSINFO[0]}"'
compare dash 'sh omits Bash identity' \
        'unset BASH_VERSION BASH_VERSINFO 2>/dev/null; printf "<%s>|<%s>\n" "$BASH_VERSION" "${BASH_VERSINFO:-}"'
compare bash 'Bash nounset status' \
        'set -u; printf "%s\n" "$missing"; printf after'
compare dash 'dash nounset status' \
        'set -u; printf "%s\n" "$missing"; printf after'
compare bash 'brace expansion enabled' \
        'printf "<%s>\n" pre{a,b}post'
compare bash 'brace expansion disabled' \
        'set +B; printf "<%s>\n" pre{a,b}post'

section ulimit
group bash

compare bash 'complete soft listing' 'ulimit -a'
compare bash 'complete hard listing' 'ulimit -Ha'
compare bash 'resource query scales' \
        'for option in c d e f i l m n p q r s t u v x R; do ulimit -$option; done'
compare bash 'file and core scaling' \
        '(ulimit -c 7; grep "Max core file size" /proc/self/limits); (ulimit -f 9; grep "Max file size" /proc/self/limits)'
compare bash 'soft and hard keywords' \
        '(ulimit -S -n hard; ulimit -S -n); (ulimit -H -n soft; ulimit -H -n)'
compare bash 'error statuses' \
        'ulimit -b >/dev/null 2>&1; printf "%s|" "$?"; ulimit -n nope >/dev/null 2>&1; printf "%s|" "$?"; ulimit -p 7 >/dev/null 2>&1; printf "%s\n" "$?"'

group dash
compare dash 'default listing unchanged' 'ulimit -a'
compare dash 'default scales unchanged' 'ulimit -c; ulimit -f; ulimit -p; ulimit -n'

section ""
total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
