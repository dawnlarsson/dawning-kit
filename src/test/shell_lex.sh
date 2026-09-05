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

subject_expected "sh policy unaffected" '<one>
' <<'CASE'
shopt -u interactive_comments
printf '<%s>\n' one # still comment
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
