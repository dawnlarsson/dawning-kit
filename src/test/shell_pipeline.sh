#!/bin/sh
#
# Focused Bash pipeline policy: lastpipe, PIPESTATUS lifetime, redirect
# failures and the non-interactive noexec boundary.
#
#     sh src/test/shell_pipeline.sh [moonwater-shell]
#
set -u

subject=${1:-/tmp/mwsh}

[ -x "$subject" ] || {
        echo "no shell at $subject" >&2
        exit 1
}

[ -x /bin/bash ] || {
        echo "  shell_pipeline NOT RUN -- no /bin/bash" >&2
        exit 2
}

[ -x /bin/dash ] || {
        echo "  shell_pipeline NOT RUN -- no /bin/dash" >&2
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

section pipeline
group pipeline

run()
{
        shell=$1
        script=$2
        output=$3
        status_file=$4

        if timeout 5 "$shell" -c "$script" > "$output" 2>/dev/null; then
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
M=$work/names/moonwater
MB=$work/names/bash

case_compare 'lastpipe final builtin state' "$B" "$M" \
        'set +m; shopt -s lastpipe; value=old; printf "new\n" | read value; printf "%s:%s:%s\n" "$value" "$?" "${PIPESTATUS[*]}"'
case_compare 'lastpipe final function and status' "$B" "$M" \
        'set +m; shopt -s lastpipe; f(){ read value; mark=function; return 7; }; printf z | f; printf "%s:%s:%s:%s\n" "$value" "$mark" "$?" "${PIPESTATUS[*]}"'
case_compare 'lastpipe final compound and pipefail' "$B" "$M" \
        'set +m; shopt -s lastpipe; set -o pipefail; false | { mark=group; true; }; printf "%s:%s:%s\n" "$mark" "$?" "${PIPESTATUS[*]}"'
case_compare 'lastpipe bang' "$B" "$M" \
        'set +m; shopt -s lastpipe; ! false | { mark=yes; false; }; printf "%s:%s:%s\n" "$mark" "$?" "${PIPESTATUS[*]}"'
case_compare 'lastpipe disabled by monitor' "$B" "$M" \
        'set -m; shopt -s lastpipe; value=old; printf "new\n" | read value; set +m; echo "$value"'
case_compare 'lastpipe disabled in background' "$B" "$M" \
        'set +m; shopt -s lastpipe; value=old; printf "new\n" | read value & wait; echo "$value"'
case_compare 'lastpipe descriptor restoration' "$B" "$M" \
        'set +m; shopt -s lastpipe; p=/tmp/mw-lastpipe.$$; printf x | { read x; echo "$x"; } >"$p"; read y <<EOF
stdin
EOF
cat "$p"; rm -f "$p"; echo "$y"'
case_compare 'lastpipe break reaches loop' "$B" "$M" \
        'set +m; shopt -s lastpipe; for i in 1 2; do printf x | { read x; break; }; echo bad; done; echo break-ok'
case_compare 'lastpipe errexit reaches final group' "$B" "$M" \
        'set +m; shopt -s lastpipe; set -e; true | { false; echo forbidden; }; echo after'
case_compare 'lastpipe tested context suppresses errexit' "$B" "$M" \
        'set +m; shopt -s lastpipe; set -e; true | { false; echo allowed; } || echo caught; echo after'
case_compare 'lastpipe ERR trap in final group' "$B" "$M" \
        'set +m; shopt -s lastpipe; trap '\''echo ERR:$?'\'' ERR; true | { false; echo after-false; }; echo done'
case_compare 'lastpipe errexit EXIT status' "$B" "$MB" \
        'set +m; shopt -s lastpipe; trap '\''printf "EXIT:%s:%s\n" "$?" "${PIPESTATUS[*]}"'\'' EXIT; set -e; true | false'
case_compare 'lastpipe restores closed stdin' "$B" "$M" \
        'set +m; shopt -s lastpipe; exec 0<&-; printf x | read value; read after 2>/dev/null; if [ "$?" -ne 0 ]; then echo "closed:$value"; fi'
case_compare 'ordinary pipeline statuses' "$B" "$MB" \
        'false | true; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'
case_compare 'lastpipe keeps direct external final' "$B" "$M" \
        'set +m; shopt -s lastpipe; /bin/false | /bin/true; printf "%s:%s\n" "$?" "${PIPESTATUS[*]}"'
case_compare 'initial PIPESTATUS scalar and length' "$B" "$MB" \
        'printf "%s:%s:%s\n" "$PIPESTATUS" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'simple failure updates PIPESTATUS' "$B" "$MB" \
        'false; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'PIPESTATUS reset by assignment' "$B" "$MB" \
        'set -o pipefail; (exit 3) | (exit 7) | true; s=$?; printf "%s:%s:%s:%s\n" "$s" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}" "${PIPESTATUS[2]}"'
case_compare 'PIPESTATUS vector collapses after simple' "$B" "$MB" \
        'false | true; :; printf "%s:%s:%s\n" "${!PIPESTATUS[*]}" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'PIPESTATUS returns after unset' "$B" "$MB" \
        'unset PIPESTATUS; printf "%s:%s\n" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'PIPESTATUS readonly absent stays absent' "$B" "$MB" \
        'readonly PIPESTATUS; false; printf "%s:%s\n" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'PIPESTATUS readonly existing refreshes' "$B" "$MB" \
        'false | true; readonly PIPESTATUS; false; printf "%s:%s:%s\n" "${!PIPESTATUS[*]}" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'PIPESTATUS readonly existing pipeline refreshes' "$B" "$MB" \
        'false; readonly PIPESTATUS; (exit 3) | (exit 7); printf "%s:%s\n" "${!PIPESTATUS[*]}" "${PIPESTATUS[*]}"'
case_compare 'PIPESTATUS readonly absent rejects pipeline' "$B" "$MB" \
        'readonly PIPESTATUS; (exit 3) | (exit 7); printf "%s:%s:%s\n" "${!PIPESTATUS[*]}" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
case_compare 'declare sees pending PIPESTATUS' "$B" "$MB" \
        'false | true; saved=$?; declare -p PIPESTATUS'
case_compare 'Bash redirect failure status' "$B" "$MB" \
        ': >/no/moonwater-pipeline/target 2>/dev/null; printf "%s\n" "$?"'
case_compare 'Bash noclobber failure status' "$B" "$MB" \
        'p=/tmp/mw-noclobber.$$; : >"$p"; set -C; : >"$p" 2>/dev/null; s=$?; rm -f "$p"; echo "$s"'
case_compare 'Bash final redirect pipeline status' "$B" "$MB" \
        'set -o pipefail; true | cat < /no/moonwater-pipeline/input 2>/dev/null; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'
case_compare 'Bash left redirect pipeline status' "$B" "$MB" \
        'set -o pipefail; cat < /no/moonwater-pipeline/input 2>/dev/null | true; printf "%s:%s:%s\n" "$?" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}"'
# A writer racing a failed reader may legitimately finish or receive SIGPIPE.
# Keep redirect-state checks deterministic, and make readonly's writer survive
# that signal so its deliberately ignored write error always has status zero.
case_compare 'lastpipe redirect failure state' "$B" "$MB" \
        'set +m; shopt -s lastpipe; value=old; true | read value < /no/moonwater-pipeline/input 2>/dev/null; printf "%s:%s:%s\n" "$?" "$value" "${PIPESTATUS[*]}"'
case_compare 'lastpipe readonly builtin status' "$B" "$MB" \
        'set +m; shopt -s lastpipe; readonly value=old; (trap "" PIPE; printf new 2>/dev/null; :) | read value 2>/dev/null; printf "%s:%s:%s\n" "$?" "$value" "${PIPESTATUS[*]}"'
case_compare 'dash redirect failure status' "$D" "$M" \
        ': >/no/moonwater-pipeline/target 2>/dev/null'
case_compare 'dynamic noexec skips successors' "$B" "$MB" \
        'echo before; set -n; echo forbidden; echo later'

section ""
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
