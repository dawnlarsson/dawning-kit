#!/bin/sh
# Focused POSIX execution policy: special builtins, prefix assignments,
# command's exception, and dot/source search. Diagnostics are deliberately
# excluded; Bash and dash use different text for the same required outcome.
set -u

subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || {
        echo "no shell at $subject" >&2
        exit 1
}
[ -x /bin/bash ] || {
        echo "  shell_posix_exec NOT RUN -- no /bin/bash" >&2
        exit 2
}
[ -x /bin/dash ] || {
        echo "  shell_posix_exec NOT RUN -- no /bin/dash" >&2
        exit 2
}

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" "$work/source"
ln -s "$subject" "$work/names/bash"
ln -s "$subject" "$work/names/moonwater"
printf '%s\n' 'sourced=yes' > "$work/source/local-only"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

run_case()
{
        mode=$1
        shell=$2
        script=$3
        output=$4
        status_file=$5

        if [ "$mode" = posix ]; then
                set -- "$shell" --posix -c "$script"
        else
                set -- "$shell" -c "$script"
        fi

        if timeout 5 env -i PATH=/bin:/usr/bin HOME="$work" LC_ALL=C \
                "$@" > "$output" 2>/dev/null; then
                status=0
        else
                status=$?
        fi
        printf '%s\n' "$status" > "$status_file"
}

compare()
{
        name=$1
        mode=$2
        reference=$3
        candidate=$4
        script=$5

        run_case "$mode" "$reference" "$script" "$work/want" "$work/want.status"
        run_case "$mode" "$candidate" "$script" "$work/got" "$work/got.status"

        if cmp -s "$work/want" "$work/got" &&
                cmp -s "$work/want.status" "$work/got.status"; then
                won
        else
                lost "$name" \
                        "want $(tr '\n' '|' < "$work/want")[$(cat "$work/want.status")], got $(tr '\n' '|' < "$work/got")[$(cat "$work/got.status")]"
        fi
}

B=/bin/bash
D=/bin/dash
MB=$work/names/bash
M=$work/names/moonwater

section posix_exec
group assignments
compare 'special prefix persists' posix "$B" "$MB" \
        'x=old; x=new :; printf "%s\n" "$x"'
compare 'special append persists' posix "$B" "$MB" \
        'x=a; x+=b :; printf "%s\n" "$x"'
compare 'command prefix temporary' posix "$B" "$MB" \
        'x=old; x=new command :; printf "%s\n" "$x"'
compare 'function prefix temporary' posix "$B" "$MB" \
        'x=old; f(){ printf "inside:%s\n" "$x"; x=body; }; x=prefix f; printf "after:%s\n" "$x"'
compare 'sh special prefix persists' plain "$D" "$M" \
        'x=old; x=new :; printf "%s\n" "$x"'
compare 'default Bash prefix temporary' plain "$B" "$MB" \
        'x=old; x=new :; printf "%s\n" "$x"'
compare 'prefix enables persistent POSIX special' plain "$B" "$MB" \
        'POSIXLY_CORRECT=y :; shopt -qo posix; printf "%s:%s\n" "${POSIXLY_CORRECT-unset}" "$?"'
compare 'prefix mode stays temporary for regular' plain "$B" "$MB" \
        'POSIXLY_CORRECT=y true; shopt -qo posix; printf "%s:%s\n" "${POSIXLY_CORRECT-unset}" "$?"'
compare 'POSIX readonly prefix is fatal' posix "$B" "$MB" \
        'readonly x=old; x=new true; echo AFTER'
compare 'POSIX readonly special exits 127' posix "$B" "$MB" \
        'readonly x=old; x=new :; echo AFTER'
compare 'POSIX command prefix exits one' posix "$B" "$MB" \
        'readonly x=old; x=new command :; echo AFTER'
compare 'POSIX prefix aborts only its physical line' posix "$B" "$MB" \
        'readonly x=old
f() {
x=new command :; echo INNER
}
f
printf "outer:%s:%s\n" "$x" "$?"'
compare 'POSIX same-line prefix skips successor' posix "$B" "$MB" \
        'readonly x=old; f(){ x=new command :; echo INNER; }; f; echo AFTER'
compare 'default Bash readonly prefix continues' plain "$B" "$MB" \
        'readonly x=old; x=new true; printf "after:%s\n" "$?"'
compare 'assignment-only error is fatal' plain "$B" "$MB" \
        'readonly x=old; x=new; echo AFTER'
compare 'default readonly for skips loop' plain "$B" "$MB" \
        'readonly i=old; for i in x y; do echo BODY; done; printf "loop:%s\n" "$?"; echo AFTER'
compare 'POSIX readonly for is fatal' posix "$B" "$MB" \
        'readonly i=old; for i in x y; do echo BODY; done; echo AFTER'
compare 'default assignment RHS sees substitution status' plain "$B" "$MB" \
        'false; a=$(true) b=$? c=$(false) d=$?; printf "%s:%s:%s\n" "$b" "$d" "$?"'
compare 'POSIX assignment RHS freezes entering status' posix "$B" "$MB" \
        'false; a=$(true) b=$? c=$(false) d=$?; printf "%s:%s:%s\n" "$b" "$d" "$?"'
compare 'default first failing substitution is visible' plain "$B" "$MB" \
        'true; a=$(false) b=$?; printf "%s:%s\n" "$b" "$?"'
compare 'POSIX first failing substitution is deferred' posix "$B" "$MB" \
        'true; a=$(false) b=$?; printf "%s:%s\n" "$b" "$?"'

group precedence
compare 'special precedes old function' plain "$B" "$MB" \
        'unset(){ echo FUNCTION; }; POSIXLY_CORRECT=1; unset no; echo AFTER'
compare 'default function precedes special' plain "$B" "$MB" \
        'unset(){ echo FUNCTION; }; unset no; echo AFTER'
compare 'default colon function precedes builtin' plain "$B" "$MB" \
        'function : { echo FUNCTION; }; :; echo AFTER'
compare 'reused function slot learns special name' plain "$B" "$MB" \
        'f(){ echo OLD; }; unset -f f; function : { echo FUNCTION; }; :; set -o posix; :; echo AFTER'
compare 'deleted special function restores builtin' plain "$B" "$MB" \
        'function export { echo FUNCTION; }; builtin unset -f export; export x=y; printf "%s\\n" "$x"'
compare 'type sees POSIX special first' plain "$B" "$MB" \
        'unset(){ :; }; POSIXLY_CORRECT=1; type -t unset; command -v unset'
compare 'disabled colon is not special' plain "$B" "$MB" \
        'enable -n :; x=old; x=new :; printf "x=%s s=%s\n" "$x" "$?"'
compare 'disabled control builtin can be a function' plain "$B" "$MB" \
        'enable -n return; function return { echo FUNCTION; }; return; echo AFTER'
compare 'disabled special permits function in POSIX mode' posix "$B" "$MB" \
        'enable -n :; function : { echo FUNCTION; }; :; echo AFTER'
compare 'disabled control is absent from command query' plain "$B" "$MB" \
        'enable -n return; command -v return; printf "query:%s\n" "$?"'
compare 'builtin cannot invoke disabled control' plain "$B" "$MB" \
        'enable -n return; builtin return 7; printf "builtin:%s\n" "$?"'

group fatality
compare 'bad export is fatal' posix "$B" "$MB" \
        'export 1bad=x; echo AFTER'
compare 'command exempts bad export' posix "$B" "$MB" \
        'command export 1bad=x; printf "after:%s\n" "$?"'
compare 'bad unset option is fatal' posix "$B" "$MB" \
        'unset -Z; echo AFTER'
compare 'command exempts bad unset' posix "$B" "$MB" \
        'command unset -Z; printf "after:%s\n" "$?"'
compare 'bad set option is fatal' posix "$B" "$MB" \
        'set -Z; echo AFTER'
compare 'command exempts bad set' posix "$B" "$MB" \
        'command set -Z; printf "after:%s\n" "$?"'
compare 'nonnumeric shift is fatal' posix "$B" "$MB" \
        'shift bad; echo AFTER'
compare 'command exempts bad shift' posix "$B" "$MB" \
        'command shift bad; printf "after:%s\n" "$?"'
compare 'range shift stays nonfatal' posix "$B" "$MB" \
        'shift 2; printf "after:%s\n" "$?"'
compare 'special redirect is fatal' posix "$B" "$MB" \
        ': > /no/moonwater-posix/target; echo AFTER'
compare 'command exempts redirect' posix "$B" "$MB" \
        'command : > /no/moonwater-posix/target; printf "after:%s\n" "$?"'
compare 'bad exec option is fatal' posix "$B" "$MB" \
        'exec -Z; echo AFTER'
compare 'command exempts exec option' posix "$B" "$MB" \
        'command exec -Z; printf "after:%s\n" "$?"'
compare 'trap operand stays nonfatal' posix "$B" "$MB" \
        'trap BAD BAD; printf "after:%s\n" "$?"'
compare 'eval status stays nonfatal' posix "$B" "$MB" \
        'eval false; printf "after:%s\n" "$?"'

group source
compare 'dot missing is fatal' posix "$B" "$MB" \
        '. /no/moonwater-posix-source; echo AFTER'
compare 'command exempts missing dot' posix "$B" "$MB" \
        'command . /no/moonwater-posix-source; printf "after:%s\n" "$?"'
compare 'POSIX source has no cwd fallback' posix "$B" "$MB" \
        'cd "$HOME/source"; PATH=/bin:/usr/bin; . local-only; echo AFTER'
compare 'default Bash source uses cwd' plain "$B" "$MB" \
        'cd "$HOME/source"; PATH=/bin:/usr/bin; source local-only; printf "%s\n" "$sourced"'

group child_exit
compare 'subshell does not run inherited EXIT' plain "$B" "$MB" \
        'trap "echo parent" EXIT; (echo sub)'
compare 'subshell runs its own EXIT' plain "$B" "$MB" \
        'trap "echo parent" EXIT; (trap "echo child" EXIT; echo sub)'
compare 'command substitution runs its own EXIT' plain "$B" "$MB" \
        'trap "echo parent" EXIT; x=$(trap "echo child" EXIT; echo cap); printf "<%s>\n" "$x"'
compare 'process substitution runs its own EXIT' plain "$B" "$MB" \
        'trap "echo parent" EXIT; cat <(trap "echo child" EXIT; echo proc)'
compare 'errexit runs child EXIT' plain "$B" "$MB" \
        'set -e; (trap '\''echo child:$?'\'' EXIT; false); echo AFTER'

section ""
printf '  %-12s %s of %s\n' total "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
