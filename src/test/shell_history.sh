#!/bin/sh
#
#       Bash history expansion over the shell's existing history/fc store.
#
#           sh src/test/shell_history.sh [path-to-moonwater-shell]
#
#       -i is deliberate even though input is a pipe: Bash history expansion
#       is an interactive reader operation, while a pipe keeps stdout exact
#       and prompts/expanded-line diagnostics isolated on stderr. HISTFILE is
#       an owned empty fixture, never the invoking user's login history.
#
set -u

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x /bin/bash ] || { echo "no /bin/bash" >&2; exit 1; }

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
ln -s "$subject" "$work/bash" || exit 1
: > "$work/history"

command cat > "$work/input" <<'CASE'
history -c
printf '<BASE:%s:%s:%s>\n' alpha beta gamma
printf '<ABS:%s>\n' !1:2
printf '<LAST:%s>\n' !1:$
printf '<RANGE:%s>\n' "!1:2-4"
printf '<SEARCH:%s>\n' !?BASE?:$
!1:s/BASE/SUB/
!1:p
printf '<LITERAL:%s>\n' '!!'
set +H
printf '<OFF:%s>\n' !!
set -H
printf '<ON:%s>\n' "!!:$"
printf '<GLOBAL:%s>\n' one-one
!!:gs/one/two/
^GLOBAL^QUICK^
!does-not-exist
printf '<STATUS:%s>\n' "$?"
exit
CASE

run()
{
        program=$1
        tag=$2

        if timeout 10 env HISTFILE="$work/history" HISTSIZE=100 \
                HISTFILESIZE=100 "$program" --noprofile --norc -i \
                < "$work/input" > "$work/$tag.out" 2> "$work/$tag.err"
        then
                eval "${tag}_status=0"
        else
                eval "${tag}_status=$?"
        fi

        : > "$work/history"
}

run /bin/bash want
run "$work/bash" got

# Moonwater's direct interactive prompt is on stdout; Bash's is on stderr.
# The controlled first result is the unambiguous boundary after that prompt.
sed '1s/^.*<BASE/<BASE/' "$work/got.out" > "$work/got.clean"

section expansion
group selectors

line_case()
{
        line_name=$1
        line_number=$2
        sed -n "${line_number}p" "$work/want.out" > "$work/want.line"
        sed -n "${line_number}p" "$work/got.clean" > "$work/got.line"
        if cmp -s "$work/want.line" "$work/got.line"; then
                won
        else
                lost "$line_name" \
                        "want $(cat "$work/want.line"), got $(cat "$work/got.line")"
        fi
}

line_case "base event" 1
line_case "absolute event word" 2
line_case "last event word" 3
line_case "event word range" 4
line_case "substring event" 5
line_case "substitution modifier" 6

group quoting
line_case "single quote suppresses" 7
line_case "disabled stays literal" 8
line_case "double quote expands" 9

group modifiers
line_case "global first event" 10
line_case "global expanded event" 11
line_case "quick substitution" 12

group errors
line_case "failure preserves status" 13
if cmp -s "$work/want.out" "$work/got.clean"; then
        won
else
        lost "complete transcript" "output has missing or extra records"
fi
if [ "$want_status" = "$got_status" ]; then
        won
else
        lost "final status" "want $want_status, got $got_status"
fi

# Valid Bash modifiers outside this deliberately bounded implementation must
# be rejected by the reader; they must never fall through as an altered
# command.  :q is useful as a fixed policy probe because Bash implements it.
command cat > "$work/unsupported.input" <<'CASE'
history -c
printf '<GUARD>\n'
!!:q
printf '<AFTER:%s>\n' "$?"
exit
CASE

: > "$work/history"
if timeout 10 env HISTFILE="$work/history" HISTSIZE=100 \
        HISTFILESIZE=100 "$work/bash" --noprofile --norc -i \
        < "$work/unsupported.input" > "$work/unsupported.out" \
        2> "$work/unsupported.err" &&
        sed '1s/^.*<GUARD>/<GUARD>/' "$work/unsupported.out" \
                > "$work/unsupported.clean" &&
        [ "$(grep -c '^<GUARD>$' "$work/unsupported.clean")" -eq 1 ] &&
        grep -q '^<AFTER:0>$' "$work/unsupported.clean" &&
        grep -q 'unsupported modifier' "$work/unsupported.err"; then
        won
else
        lost "unsupported modifier" \
                "was not rejected before execution: $(cat "$work/unsupported.clean" 2>/dev/null)"
fi

section ""
printf '  shell history %s of %s checks\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
