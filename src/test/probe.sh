#!/bin/sh
#
#       Running the probes and reading what came back.
#
#           PROBE=/path/to/probe sh src/test/probe.sh
#
#       src/test/probe.c is the program; this is what asks it questions. The
#       probes existed for a long time and nothing ran them, so a stack of
#       arguments that arrived wrong, a .bss that arrived dirty or an exit
#       status that never came back would all have been silent.
#
set -u

probe=${PROBE:-/tmp/probe}

[ -x "$probe" ] || { echo "no probe at $probe" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

won() { pass=$((pass + 1)); }

lost()
{
        fail=$((fail + 1))
        printf '  %-24s %s\n' "$1" "$2"
}

# Output and exit status both, because half of what these prove is the status.
expect()
{
        name=$1
        want=$2
        want_status=$3
        shift 3

        if "$@" > "$work/got" 2>&1; then
                got_status=0
        else
                got_status=$?
        fi

        printf '%s' "$want" > "$work/want"

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" "want [$(tr '\n' '|' < "$work/want")]($want_status) got [$(tr '\n' '|' < "$work/got")]($got_status)"
}

# The same, when only one line of the answer is the point.
expect_line()
{
        name=$1
        want=$2
        line=$3
        shift 3

        "$@" 2>&1 | sed -n "${line}p" > "$work/got" || true
        printf '%s\n' "$want" > "$work/want"

        if cmp -s "$work/want" "$work/got"; then
                won
                return 0
        fi

        lost "$name" "line $line want [$want] got [$(cat "$work/got")]"
}

#
#       A stack of arguments.
#

expect_line 'counted'        '5 argument(s)'   1 "$probe" arguments a b c
expect_line 'called as'      "  0: $probe"     2 "$probe" arguments a b c
expect_line 'first is mode'  '  1: arguments'  3 "$probe" arguments a b c
expect_line 'third'          '  2: a'          4 "$probe" arguments a b c
expect_line 'last'           '  4: c'          6 "$probe" arguments a b c
expect_line 'no mode at all' 'probe: arguments regions environment spawn quack status' 1 "$probe"
expect_line 'mode only'      '2 argument(s)'   1 "$probe" arguments
expect_line 'an empty one'   '  2: '           4 "$probe" arguments '' x
expect_line 'one with space' '  2: a b'        4 "$probe" arguments 'a b'
expect_line 'one with a tab' '  2: a	b'        4 "$probe" arguments 'a	b'
expect_line 'a long one'     "  2: $(printf 'x%.0s' $(seq 200))" 4 \
        "$probe" arguments "$(printf 'x%.0s' $(seq 200))"

many=$(seq 100 | tr '\n' ' ')
# shellcheck disable=SC2086 # a hundred separate arguments is the point
expect_line 'a hundred'      '102 argument(s)' 1 "$probe" arguments $many

#
#       An exit status, which a spawner has to be able to read back.
#

for code in 0 1 7 42 255; do
        expect "status $code" '' "$code" "$probe" status "$code"
done

expect 'unknown mode' 'probe: arguments regions environment spawn quack status
' 2 "$probe" nosuchmode

#
#       The three regions the loader maps.
#

expect 'regions' 'bss zeroed:   yes
data initial: yes
data written: yes
bss written:  yes
' 0 "$probe" regions

expect 'quack' 'quack
' 0 "$probe" quack

#
#       The environment a spawner handed over.
#

if command -v env > /dev/null 2>&1; then
        expect 'environment' '  env: PROBE_SAYS=hello
' 0 env -i PROBE_SAYS=hello "$probe" environment

        expect 'no environment' 'environment is empty
' 1 env -i "$probe" environment
fi

#
#       /dev/spark, which only exists inside the image.
#

if [ -c /dev/spark ]; then
        expect_line 'spark device' 'device: 3' 1 "$probe" spawn
else
        printf '  %-24s %s\n' 'spark device' 'not here, skipped'
fi

printf '  %-12s %s of %s\n' probe "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'probe %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
