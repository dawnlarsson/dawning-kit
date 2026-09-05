#!/bin/sh
# Startup files and one-command input share the ordinary parser, not an eval.
set -u
subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || exit 1
[ -x /bin/bash ] && [ -x /bin/dash ] || exit 2
case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" "$work/search" "$work/home"
for name in bash sh dash moonwater; do ln -s "$subject" "$work/names/$name"; done
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

printf '%s\n' 'printf "startup:%s:%s:%s\n" "$0" "$#" "${1-none}"' 'value=loaded' > "$work/start"
printf '%s\n' 'printf "literal-name\n"' > "$work/start file"
cp "$work/start file" "$work/start*"
cp "$work/start file" "$work/\"quoted\""
cp "$work/start file" "$work/\x"
cp "$work/start file" "$work/home/start"
cp "$work/start file" "$work/search/only-in-path"
printf '%s\n' 'printf "start\n"' 'return 7' 'echo forbidden' > "$work/return"
printf '%s\n' 'false() { printf "override\n"; }; trap '\''printf "exit:%s\n" "$?"'\'' EXIT' > "$work/functions"
printf '%s\n' 'echo startup' 'exit 7' > "$work/exit"
printf '%s\n' 'set -e' 'false' 'echo forbidden' > "$work/errexit"
printf '%s\n' 'printf "body:%s:%s\n" "${value-unset}" "$?"' > "$work/body"
printf '%s\n' 'set -- replaced args' 'value=changed' > "$work/parameters"
: > "$work/input"

capture()
{
        executable=$1 tag=$2 startup=$3
        shift 3
        if HOME="$work/home" ROOT="$work" BASH_ENV="$startup" ENV= \
                PATH="$work/search:/usr/bin:/bin" LC_ALL=C \
                timeout 5 "$executable" "$@" < "$work/input" \
                > "$work/$tag.out" 2> "$work/$tag.err"; then result=0; else result=$?; fi
        printf '%s\n' "$result" > "$work/$tag.status"
}

compare()
{
        name=$1 mode=$2 startup=$3
        shift 3
        case $mode in bash) reference=/bin/bash ;; *) reference=/bin/dash ;; esac
        capture "$reference" want "$startup" "$@"
        capture "$work/names/$mode" got "$startup" "$@"
        if cmp -s "$work/want.out" "$work/got.out" &&
                cmp -s "$work/want.status" "$work/got.status" &&
                { [ "${diagnostic:-exact}" = ignore ] || cmp -s "$work/want.err" "$work/got.err"; }; then
                won
        else
                lost "$name [$mode]" "want $(tr '\n' '|' < "$work/want.out")[$(cat "$work/want.status")], got $(tr '\n' '|' < "$work/got.out")[$(cat "$work/got.status")]; stderr want $(head -c 120 "$work/want.err") got $(head -c 120 "$work/got.err")"
        fi
}

section startup_files
group bash
compare 'source before command' bash "$work/start" -c 'printf "body:%s\n" "$value"' named arg
compare 'source before script' bash "$work/start" "$work/body" arg
compare 'source with spaces' bash "$work/start file" -c ':'
compare 'no pathname expansion' bash "$work/start*" -c ':'
compare 'literal quote bytes' bash "$work/\"quoted\"" -c ':'
compare 'literal backslash bytes' bash "$work/\x" -c ':'
compare 'parameter path' bash '$ROOT/start file' -c ':'
compare 'command path' bash '$(printf "%s/start file" "$ROOT")' -c ':'
compare 'tilde path' bash '~/start' -c ':'
compare 'no PATH lookup' bash only-in-path -c 'echo body'
compare 'missing path ignored' bash "$work/missing" -c 'echo body'
compare 'empty path ignored' bash '' -c 'echo body'
compare 'return boundary' bash "$work/return" -c 'printf "body:%s\n" "$?"'
compare 'literal fast path observes functions' bash "$work/functions" -c false
compare 'literal fast path observes EXIT trap' bash "$work/functions" -c ':'
compare 'startup exit' bash "$work/exit" -c ':'
compare 'startup errexit' bash "$work/errexit" -c ':'
compare 'source changes parameters' bash "$work/parameters" -c 'printf "%s:%s:%s\n" "$value" "$#" "$*"' named original
compare 'noprofile does not disable BASH_ENV' bash "$work/start file" --noprofile -c ':'
compare 'norc does not disable BASH_ENV' bash "$work/start file" --norc -c ':'
compare 'noexec skips startup execution' bash "$work/start file" -n -c ':'
diagnostic=ignore compare 'interactive ignores BASH_ENV' bash "$work/start file" --noprofile --norc -ic ':'
for mode in sh dash moonwater; do compare 'non-Bash ignores BASH_ENV' "$mode" "$work/start file" -c ':'; done

section onecmd
group input
for input in 'echo one; echo two\necho three\n' '# first line\necho forbidden\n' '\necho forbidden\n' \
        'if true; then\necho one\nfi\necho forbidden\n' 'cat <<EOF\none\nEOF\necho forbidden\n' \
        'false\necho forbidden\n' 'set +t; echo one\necho two\n' 'set -t; echo one\necho forbidden\n'; do
        printf '%b' "$input" > "$work/input"
        compare 'stdin first complete program' bash '' -t
        compare 'script first complete program' bash '' -t "$work/input"
done
compare 'command string ignores onecmd' bash '' -tc 'echo one
echo two'
compare 'named onecmd' bash '' -o onecmd -c 'case $- in *t*) echo yes;; esac'
compare 'runtime onecmd command string' bash '' -c 'set -t; echo one
echo two'
compare 'onecmd flag state' bash '' -tc 'case $- in *t*) echo yes;; esac; set +t; case $- in *t*) echo bad;; *) echo off;; esac'

section ""
printf '  total        %s of %s\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
