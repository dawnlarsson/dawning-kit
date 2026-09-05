#!/bin/sh
#
#       Comment policy and lexer/parser boundary cases.
#
#           sh src/test/shell_lex.sh [path-to-moonwater-shell]
#
#       Bash's interactive_comments option is grammar, not expansion: when it
#       is off in an interactive Bash, a fresh # is an ordinary word byte.
#       Non-interactive Bash and the sh/dash personalities continue to treat
#       the same byte as a comment.  The cases below also keep the unfinished-
#       line scan and the token scan level around quotes, continuations,
#       substitutions, aliases and here-documents.
#
set -u

subject=${1:-/tmp/mwsh}
[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x /bin/bash ] || { echo "no /bin/bash" >&2; exit 1; }
[ -x /bin/dash ] || { echo "no /bin/dash" >&2; exit 1; }

case $subject in
/*) ;;
*) subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/names" || exit 1
ln -s "$subject" "$work/names/bash" || exit 1
ln -s "$subject" "$work/names/sh" || exit 1

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

shown()
{
        head -c 100 "$1" | tr '\n' '|'
}

capture_command()
{
        capture_shell=$1
        capture_tag=$2
        capture_script=$3
        shift 3

        if timeout 6 "$capture_shell" "$@" -c "$capture_script" \
                > "$work/$capture_tag.out" 2> "$work/$capture_tag.err"; then
                capture_status=0
        else
                capture_status=$?
        fi

        case $capture_tag in
        want) want_status=$capture_status ;;
        got) got_status=$capture_status ;;
        esac
}

capture_input()
{
        capture_shell=$1
        capture_tag=$2

        if timeout 6 "$capture_shell" < "$work/input" \
                > "$work/$capture_tag.out" 2> "$work/$capture_tag.err"; then
                capture_status=0
        else
                capture_status=$?
        fi

        case $capture_tag in
        want) want_status=$capture_status ;;
        got) got_status=$capture_status ;;
        esac
}

same_result()
{
        case_name=$1
        diagnostic=${2:-exact}

        if ! cmp -s "$work/want.out" "$work/got.out" ||
                [ "$want_status" != "$got_status" ]; then
                lost "$case_name" \
                        "want $(shown "$work/want.out")[$want_status], got $(shown "$work/got.out")[$got_status]"
                return
        fi

        if [ "$diagnostic" = exact ] &&
                ! cmp -s "$work/want.err" "$work/got.err"; then
                lost "$case_name" \
                        "stderr want $(shown "$work/want.err"), got $(shown "$work/got.err")"
                return
        fi

        won
}

bash_interactive_case()
{
        case_name=$1
        case_script=$(command cat)

        # Forced interactive Bash diagnoses the absent controlling terminal;
        # stdout and status carry the grammar result under test.
        capture_command /bin/bash want "$case_script" --noprofile --norc -i
        capture_command "$work/names/bash" got "$case_script" -i
        same_result "$case_name" ignore
}

bash_script_case()
{
        case_name=$1
        command cat > "$work/input"
        capture_input /bin/bash want
        capture_input "$work/names/bash" got
        same_result "$case_name" exact
}

bash_script_diagnostic_case()
{
        case_name=$1
        command cat > "$work/input"
        capture_input /bin/bash want
        capture_input "$work/names/bash" got
        # Syntax diagnostics necessarily contain the invoked pathname. The
        # bytes written before it and the terminating status remain exact.
        same_result "$case_name" ignore
}

dash_case()
{
        case_name=$1
        command cat > "$work/input"
        capture_input /bin/dash want
        capture_input "$work/names/sh" got
        same_result "$case_name" exact
}

subject_expected()
{
        case_name=$1
        expected=$2
        case_script=$(command cat)

        capture_command "$work/names/sh" got "$case_script" -i
        printf '%s' "$expected" > "$work/want.out"
        want_status=0
        : > "$work/want.err"
        same_result "$case_name" ignore
}

section "comments"
group "bash interactive"

bash_interactive_case "disabled fresh hash" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one # two
CASE

bash_interactive_case "reenabled fresh hash" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one # two
shopt -s interactive_comments
printf '<%s>\n' three # four
CASE

bash_interactive_case "same line parsed first" <<'CASE'
shopt -u interactive_comments; printf '<%s>\n' one # two
CASE

bash_interactive_case "operator starts word" <<'CASE'
shopt -u interactive_comments
printf 'A\n'; printf '<%s>\n' # after operator
CASE

bash_interactive_case "embedded quoted escaped" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' a#b \# '#' "#"
CASE

bash_interactive_case "disabled open quote" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' # "held
across line"
CASE

bash_interactive_case "disabled continuation" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one \
# two
CASE

bash_interactive_case "enabled continuation" <<'CASE'
shopt -s interactive_comments
printf '<%s>\n' one \
# two
CASE

bash_interactive_case "nested substitution" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' "$(printf x # ignored close )
printf y)"
CASE

bash_interactive_case "nested word hash" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' "$(printf x$(printf y)#z)"
CASE

bash_interactive_case "backtick commented close" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' "`printf x # ignored close`"
CASE

bash_interactive_case "alias replacement" <<'CASE'
shopt -u interactive_comments
alias say='printf "<%s>\n" alias # tail'
say
CASE

bash_interactive_case "heredoc header" <<'CASE'
shopt -u interactive_comments
say() { printf '<%s>\n' "$@"; }
say <<EOF # header words
body
EOF
CASE

bash_interactive_case "heredoc body literal" <<'CASE'
shopt -u interactive_comments
cat <<'EOF'
# "literal body \
EOF
CASE


bash_interactive_case "heredoc line joining" <<'CASE'
shopt -u interactive_comments
cat <<EOF
one \
EOF
EOF
CASE

group "noninteractive"

bash_script_case "bash ignores disabled shopt" <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one # still comment
CASE

bash_script_case "arithmetic hash is not comment" <<'CASE'
printf '<%s>\n' "$((16#10))"
CASE

bash_script_case "substitution continued comment" <<'CASE'
printf '<%s>\n' "$(printf x; \
# ignored close )
printf y)"
CASE

bash_script_case "substitution heredoc paren" <<'CASE'
value=$(cat <<EOF
)
EOF
)
printf '<%s>\n' "$value"
CASE

bash_script_case "quoted paren delimiter" <<'CASE'
value=$(cat <<'END)'
body)
END)
)
printf '<%s>\n' "$value"
CASE

bash_script_case "multiple substitution heredocs" <<'CASE'
value=$(cat <<FIRST <<SECOND
first)
FIRST
second)
SECOND
)
printf '<%s>\n' "$value"
CASE

bash_script_case "split substitution heredoc operator" <<'CASE'
value=$(cat <\
<EOF
)
EOF
)
printf '<%s>\n' "$value"
CASE

bash_script_case "split substitution heredoc delimiter" <<'CASE'
value=$(cat <<E\
OF
)
EOF
)
printf '<%s>\n' "$value"
CASE

bash_script_case "multiple split substitution heredocs" <<'CASE'
value=$(cat <\
<FIRST <<SEC\
OND
first)
FIRST
second)
SECOND
)
printf '<%s>\n' "$value"
CASE

bash_script_case "process substitution heredoc paren" <<'CASE'
cat <(cat <<EOF
)
EOF
)
CASE

subject_expected "sh policy unaffected" '<one>
' <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one # still comment
CASE

group "bash aliases"

bash_script_case "noninteractive alias default off" <<'CASE'
shopt -u expand_aliases
alias moon_alias='printf "<EXPANDED>\n"'
moon_alias 2>/dev/null
printf '<STATUS:%s>\n' "$?"
CASE

bash_script_case "expand_aliases enables parser" <<'CASE'
shopt -s expand_aliases
alias moon_alias='printf "<EXPANDED>\n"'
moon_alias
CASE

bash_script_case "posix enables aliases" <<'CASE'
set -o posix
alias moon_alias='printf "<EXPANDED>\n"'
moon_alias
CASE

bash_script_case "default aliases reserved word" <<'CASE'
shopt -s expand_aliases
alias time='printf "<ALIASED-TIME>\n"'
time :
CASE

bash_script_case "posix keeps reserved word" <<'CASE'
set -o posix
alias time='printf "<ALIASED-TIME>\n"'
{ time :; } 2>/dev/null
printf '<DONE>\n'
CASE

group "posix parameter quote"

bash_script_case "default single quote holds brace" <<'CASE'
unset x
printf '<%s>\n' "${x:-'a}b'}"
CASE

bash_script_case "posix single quote releases brace" <<'CASE'
set -o posix
unset x
printf '<%s>\n' "${x:-'a}b'}"
CASE

bash_script_case "pattern removal quote exception" <<'CASE'
set -o posix
x=abc
printf '<%s>\n' "${x#'a}b'}"
CASE

bash_script_diagnostic_case "unmatched pattern quote rejected" <<'CASE'
set -o posix
x=abc
printf '<%s>\n' "${x#'a}"
CASE

group "time policy"

bash_script_case "default time remains reserved" <<'CASE'
d=$(mktemp -d) || exit
printf '#!/bin/sh\nprintf "<EXTERNAL:%%s>\\n" "$*"\n' > "$d/time"
chmod +x "$d/time"
PATH="$d:$PATH"
{ time -p marker; } 2>/dev/null
printf '<DONE>\n'
rm -rf "$d"
CASE

bash_script_case "posix time option is command word" <<'CASE'
d=$(mktemp -d) || exit
printf '#!/bin/sh\nprintf "<EXTERNAL:%%s>\\n" "$*"\n' > "$d/time"
chmod +x "$d/time"
PATH="$d:$PATH"
set -o posix
time -p marker
rm -rf "$d"
CASE

group "bash redirects"

bash_script_case "default unique redirect glob" <<'CASE'
d=$(mktemp -d) || exit
: > "$d/one.target"
printf '<UNIQUE>\n' > "$d"/*.target
cat "$d/one.target"
rm -rf "$d"
CASE

bash_script_case "default ambiguous redirect glob" <<'CASE'
d=$(mktemp -d) || exit
: > "$d/one.target"
: > "$d/two.target"
{ printf bad > "$d"/*.target; } 2>/dev/null
printf '<STATUS:%s>\n' "$?"
rm -rf "$d"
CASE

bash_script_case "posix redirect keeps pattern" <<'CASE'
d=$(mktemp -d) || exit
: > "$d/one.target"
set -o posix
printf '<LITERAL>\n' > "$d"/*.target
cat "$d/*.target"
rm -rf "$d"
CASE

bash_script_case "default redirect splits" <<'CASE'
d=$(mktemp -d) || exit
target="$d/one $d/two"
{ printf bad > $target; } 2>/dev/null
printf '<STATUS:%s>\n' "$?"
rm -rf "$d"
CASE

bash_script_case "posix redirect stays whole" <<'CASE'
d=$(mktemp -d) || exit
cd "$d" || exit
target='one two'
set -o posix
printf '<WHOLE>\n' > $target
cat 'one two'
cd /
rm -rf "$d"
CASE

section "boundaries"
group "dash grammar"

dash_case "word and escapes" <<'CASE'
printf '<%s>\n' a#b \# '#' "#" # tail
CASE

dash_case "operator comment" <<'CASE'
printf 'one\n';# comment after separator
printf 'two\n'
CASE

dash_case "continued into comment" <<'CASE'
printf '<%s>\n' one \
# comment after joined blank
CASE

dash_case "nested comment" <<'CASE'
printf '%s\n' "$(printf 'inner\n' # nested comment
printf 'tail')"
CASE

dash_case "hash in nested languages" <<'CASE'
v=xword
printf '<%s>\n' "${v#x}"
CASE

dash_case "heredoc keeps hashes" <<'CASE'
cat <<EOF
# quoted " and slash \
plain#word
EOF
CASE


dash_case "heredoc delimiter joined" <<'CASE'
cat <<EOF
one \
EOF
EOF
CASE

dash_case "substitution heredoc paren" <<'CASE'
value=$(cat <<EOF
)
EOF
)
printf '<%s>\n' "$value"
CASE

dash_case "stripped heredoc paren" <<'CASE'
value=$(cat <<-EOF
	)
	EOF
)
printf '<%s>\n' "$value"
CASE

dash_case "joined body paren" <<'CASE'
value=$(cat <<EOF
one \
)
EOF
)
printf '<%s>\n' "$value"
CASE

dash_case "split substitution heredoc operator" <<'CASE'
value=$(cat <\
<EOF
)
EOF
)
printf '<%s>\n' "$value"
CASE

dash_case "split substitution heredoc delimiter" <<'CASE'
value=$(cat <<E\
OF
)
EOF
)
printf '<%s>\n' "$value"
CASE

section "bounds"
group "nesting"

deep='printf x'
depth=0
while [ "$depth" -lt 70 ]; do
        deep='$('"$deep"')'
        depth=$((depth + 1))
done
capture_command "$work/names/bash" got ": $deep"
if [ "$got_status" -eq 2 ]; then
        won
else
        lost "recursive substitution cap" \
                "wanted bounded status 2, got $got_status: $(shown "$work/got.err")"
fi

section
printf '  shell lex     %s of %s checks\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
