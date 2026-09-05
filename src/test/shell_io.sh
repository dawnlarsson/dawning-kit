#!/bin/sh
#
# Focused read/printf builtin compatibility at option, numeric and I/O error
# boundaries.
#
#     sh src/test/shell_io.sh [moonwater-shell]
#
set -u

subject=${1:-/tmp/mwsh}

[ -x "$subject" ] || {
        echo "no shell at $subject" >&2
        exit 1
}

[ -x /bin/bash ] || {
        echo "  shell_io NOT RUN -- no /bin/bash" >&2
        exit 2
}

[ -x /bin/dash ] || {
        echo "  shell_io NOT RUN -- no /bin/dash" >&2
        exit 2
}

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" || exit 1
ln -s "$subject" "$work/names/bash" || exit 1
ln -s "$subject" "$work/names/moonwater" || exit 1

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

section shell_io
group read_printf

run_case()
{
        shell=$1
        script=$2
        output=$3
        status_file=$4

        if timeout 4 "$shell" -c "$script" > "$output" 2>/dev/null; then
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

        run_case "$reference" "$script" "$work/want" "$work/want.status"
        run_case "$candidate" "$script" "$work/got" "$work/got.status"

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
M=$work/names/moonwater
MB=$work/names/bash

case_compare 'printf option terminator Bash' "$B" "$MB" \
        'printf -- "%s\n" x; printf "s=%s\n" "$?"'
case_compare 'printf option terminator dash' "$D" "$M" \
        'printf -- "%s\n" x; printf "s=%s\n" "$?"'
case_compare 'printf attached variable' "$B" "$MB" \
        'unset value; printf -vvalue "%03d:%s" 7 x; printf "%s:%s\n" "$?" "$value"'
case_compare 'printf rejects bad option Bash' "$B" "$MB" \
        'printf -x value; printf "s=%s\n" "$?"'
case_compare 'printf rejects bad option dash' "$D" "$M" \
        'printf -x value; printf "s=%s\n" "$?"'
case_compare 'printf missing conversion Bash' "$B" "$MB" \
        'printf "abc%"; printf "|s=%s\n" "$?"'
case_compare 'printf missing conversion dash' "$D" "$M" \
        'printf "abc%"; printf "|s=%s\n" "$?"'
case_compare 'printf invalid conversion Bash' "$B" "$MB" \
        'printf "%y" value; printf "s=%s\n" "$?"'
case_compare 'printf invalid conversion dash' "$D" "$M" \
        'printf "%y" value; printf "s=%s\n" "$?"'
case_compare 'printf signed bounds' "$B" "$MB" \
        'printf "%d:%d:%d\n" 9223372036854775807 9223372036854775808 -9223372036854775809; printf "s=%s\n" "$?"'
case_compare 'printf signed bounds dash' "$D" "$M" \
        'printf "%d:%d:%d\n" 9223372036854775807 9223372036854775808 -9223372036854775809; printf "s=%s\n" "$?"'
case_compare 'printf unsigned bounds' "$B" "$MB" \
        'printf "%u:%u:%u\n" 9223372036854775808 18446744073709551615 18446744073709551616; printf "s=%s\n" "$?"'
case_compare 'printf unsigned bounds dash' "$D" "$M" \
        'printf "%u:%u:%u\n" 9223372036854775808 18446744073709551615 18446744073709551616; printf "s=%s\n" "$?"'

case_compare 'read fractional timeout EOF' "$B" "$MB" \
        'value=old; read -t .05 value </dev/null; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read signed zero timeout' "$B" "$MB" \
        'value=old; read -t +. value </dev/null; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read submicrosecond timeout' "$B" "$MB" \
        'value=old; sleep .05 | { read -t .0000001 value; printf "%s:<%s>\n" "$?" "$value"; }'
case_compare 'read rejects exponent' "$B" "$MB" \
        'value=old; read -t 1e-1 value </dev/null; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read rejects wide descriptor' "$B" "$MB" \
        'value=old; read -u 4294967296 value </dev/null; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read rejects overflowing descriptor' "$B" "$MB" \
        'value=old; read -u 999999999999999999999999 value </dev/null; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read descriptor error preserves' "$B" "$MB" \
        'value=old; read value <&-; printf "%s:<%s>\n" "$?" "$value"'
case_compare 'read timeout publishes empty' "$B" "$MB" \
        'value=old; sleep .15 | { read -t .02 value; printf "%s:<%s>\n" "$?" "$value"; }'
case_compare 'read timeout publishes partial' "$B" "$MB" \
        'value=old; { printf a; sleep .15; } | { read -t .02 value; printf "%s:<%s>\n" "$?" "$value"; }'
case_compare 'read escaped byte shares deadline' "$B" "$MB" \
        'value=old; { printf "%s" "\\"; sleep .15; printf x; } | { read -t .02 value; printf "%s\n" "$?"; }'

section ""
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
