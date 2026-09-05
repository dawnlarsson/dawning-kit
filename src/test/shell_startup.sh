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
cp "$work/start file" "$work/file1"
cp "$work/start file" "$work/\$FILE"
cp "$work/start file" "$work/home/start"
cp "$work/start file" "$work/search/only-in-path"
printf '%s\n' 'printf "start\n"' 'return 7' 'echo forbidden' > "$work/return"
printf '%s\n' 'false() { printf "override\n"; }; trap '\''printf "exit:%s\n" "$?"'\'' EXIT' > "$work/functions"
printf '%s\n' 'echo startup' 'exit 7' > "$work/exit"
printf '%s\n' 'set -e' 'false' 'echo forbidden' > "$work/errexit"
printf '%s\n' 'printf "body:%s:%s\n" "${value-unset}" "$?"' > "$work/body"
printf '%s\n' 'set -- replaced args' 'value=changed' > "$work/parameters"
printf '%s\n' 'cd "$ROOT"' > "$work/cd"
printf '%s\n' 'printf "source-status:%s\n" "$?"' 'false' > "$work/status"
printf '%s\n' 'false; return 7; echo forbidden' > "$work/return-after-false"
: > "$work/input"

capture()
{
        executable=$1 tag=$2 startup=$3
        shift 3
        if HOME="$work/home" ROOT="$work" RESULT="$work/$tag.effect" BASH_ENV="$startup" ENV= \
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
        # The native interactive renderer has its own prompt/escape bytes.
        # A recovery case compares the command's file effect, not that UI.
        suffix=${output_kind:-out}
        if cmp -s "$work/want.$suffix" "$work/got.$suffix" &&
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
compare 'backtick path' bash '`printf "%s/start file" "$ROOT"`' -c ':'
compare 'arithmetic path' bash '$ROOT/file$((1))' -c ':'
compare 'escaped dollar path' bash '$ROOT/\$FILE' -c ':'
compare 'empty expansion carries substitution status' bash '$(exit 7)' -c ''
compare 'missing expanded file carries status' bash '$(exit 7)/missing' -c 'printf "body-status:%s\n" "$?"'
compare 'source restores expansion status' bash '$(printf "%s/status" "$ROOT"; exit 7)' -c 'printf "body-status:%s\n" "$?"'
compare 'tilde path' bash '~/start' -c ':'
compare 'no PATH lookup' bash only-in-path -c 'echo body'
compare 'missing path ignored' bash "$work/missing" -c 'echo body'
compare 'empty path ignored' bash '' -c 'echo body'
compare 'return boundary' bash "$work/return" -c 'printf "body:%s\n" "$?"'
compare 'startup return preserves prior simple status' bash "$work/return-after-false" -c 'printf "body:%s\n" "$?"'
compare 'literal fast path observes functions' bash "$work/functions" -c false
compare 'literal fast path observes EXIT trap' bash "$work/functions" -c ':'
compare 'startup exit' bash "$work/exit" -c ':'
compare 'startup errexit' bash "$work/errexit" -c ':'
compare 'source changes parameters' bash "$work/parameters" -c 'printf "%s:%s:%s\n" "$value" "$#" "$*"' named original
compare 'startup cd precedes script open' bash "$work/cd" ./body
diagnostic=ignore compare 'startup precedes missing script' bash "$work/start file" "$work/missing"
diagnostic=ignore compare 'startup precedes directory rejection' bash "$work/start file" "$work/home"
diagnostic=ignore compare 'missing script does not run EXIT trap' bash "$work/functions" "$work/missing"
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

section source_files
group policy
: > "$work/input"
for mode in bash sh dash moonwater; do
        diagnostic=ignore compare 'missing dot-file status and EXIT' "$mode" '' -c 'trap '\''printf "exit:%s\n" "$?"'\'' EXIT; . "$ROOT/missing"; printf "after:%s\n" "$?"'
        diagnostic=ignore compare 'missing dot-file under errexit' "$mode" '' -ec '. "$ROOT/missing"; echo forbidden'
        diagnostic=ignore compare 'dot without filename' "$mode" '' -c '.; printf "status:%s\n" "$?"'
        diagnostic=ignore compare 'dot option end without filename' "$mode" '' -c '. --; printf "status:%s\n" "$?"'
        compare 'dot option end' "$mode" '' -c '. -- "$ROOT/start file"; printf "status:%s\n" "$?"'
        diagnostic=ignore compare 'current directory source fallback' "$mode" '' -c 'cd "$ROOT"; PATH=/nonexistent; . "start file"; printf "status:%s\n" "$?"'
        compare 'source positional parameter policy' "$mode" '' -c 'set -- outer args; . "$ROOT/start" inner words; printf "after:%s:%s\n" "$?" "$*"' named
        compare 'explicit set in source persists' "$mode" '' -c 'set -- outer args; . "$ROOT/parameters" inner words; printf "after:%s:%s\n" "$?" "$*"'
done
compare 'disabled sourcepath opens local file' bash '' -c 'cd "$ROOT"; shopt -u sourcepath; . "start file"'
diagnostic=ignore compare 'disabled sourcepath skips PATH' bash '' -c 'shopt -u sourcepath; . only-in-path; printf "status:%s\n" "$?"'
compare 'enabled sourcepath visits PATH' bash '' -c '. only-in-path'
printf '%s\n' 'shift' > "$work/shift"
printf '%s\n' 'f() { set -- function; }; f' > "$work/function-parameters"
printf '%s\n' '. "$ROOT/parameters" nested words' > "$work/nested-parameters"
compare 'shift restores source arguments' bash '' -c 'set -- outer args; . "$ROOT/shift" inner words; printf "after:%s\n" "$*"'
compare 'function set does not replace source caller arguments' bash '' -c 'set -- outer args; . "$ROOT/function-parameters" inner words; printf "after:%s\n" "$*"'
compare 'nested source operands isolate replacement marker' bash '' -c 'set -- outer args; . "$ROOT/nested-parameters" inner words; printf "after:%s\n" "$*"'
printf '%s\n' '. "$ROOT/parameters"' > "$work/nested-no-parameters"
printf '%s\n' 'set -- explicit outer' '. "$ROOT/parameters" nested words' > "$work/nested-explicit-parameters"
compare 'nested source without operands shares replacement marker' bash '' -c 'set -- outer args; . "$ROOT/nested-no-parameters" inner words; printf "after:%s\n" "$*"'
compare 'nested source operands consume earlier replacement marker' bash '' -c 'set -- outer args; . "$ROOT/nested-explicit-parameters" inner words; printf "after:%s\n" "$*"'

section control_numbers
group return
for value in '-1' '+2' '2147483648' '9223372036854775807' '-9223372036854775808' \
        '9223372036854775808' '-9223372036854775809' '99999999999999999999999999999999999999999' \
        '00000000000000000000000000000000000000003' 'bad' '0x10' '-- 7' '--' '"  +3  "'; do
        for mode in bash dash; do
                diagnostic=ignore compare "return numeric policy $value" "$mode" '' -c "f() { return $value; echo forbidden; }; f; printf 'status:%s\\n' \"\$?\""
        done
done
diagnostic=ignore compare 'Bash top-level return continues' bash '' -c 'return 7; printf "after:%s\n" "$?"'
diagnostic=ignore compare 'Bash top-level invalid return continues' bash '' -c 'return bad; printf "after:%s\n" "$?"'
diagnostic=ignore compare 'Bash return too many arguments' bash '' -c 'trap '\''printf "exit:%s\n" "$?"'\'' EXIT; f() { return 1 2; echo forbidden; }; f || echo forbidden; echo forbidden'
diagnostic=ignore compare 'interactive command return too many arguments' bash '' --noprofile --norc -ic 'f() { return 1 2; echo forbidden; }; f; echo forbidden
echo forbidden'
diagnostic=ignore compare 'Bash invalid return unwinds only function' bash '' -c 'f() { return bad; echo forbidden; }; f || printf "caught:%s\n" "$?"; echo after'
for mode in bash sh dash moonwater; do
        compare 'signed whitespace loop count' "$mode" '' -c 'for a in first second; do for b in inner other; do printf "%s:%s\n" "$a" "$b"; break "  +2  "; done; done'
done
printf '%s\n' 'f() { return 1 2; echo forbidden >> "$RESULT"; }; f; echo forbidden >> "$RESULT"' 'echo recovered >> "$RESULT"' > "$work/input"
output_kind=effect diagnostic=ignore compare 'interactive return error recovers at next input' bash '' --noprofile --norc -i

section ""
printf '  total        %s of %s\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
