#!/bin/sh
#
#       Cases for the expand lane.
#
#           sh src/test/expand.sh [directory of the names it answers to]
#                                 [shell] [reference]
#
#       Every case runs the same input through the reference the case names
#       and through ours, and compares. Agreeing with the reference is what
#       passing means; there is no separate idea here of the right answer.
#
#       What is asked here is the order POSIX fixes in XCU 2.6 and nothing
#       else: tilde, parameter, command substitution, arithmetic, field
#       splitting, pathname expansion, quote removal. The grammar around it is
#       shell.sh's; a case belongs here when moving it to a different command
#       would not change the answer.
#
#       Four kinds of case, the same four shell.sh has:
#
#         check     the two shells write the same bytes to standard output.
#         answer    the same bytes and the same exit status.
#         differs   ours and dash disagree, and this is exactly what ours says
#                   today. Written down rather than left out, so that closing
#                   the gap fails here and says so.
#         generated a fixed cross product of form, value and IFS rather than a
#                   list somebody thought of.
#
#       Every field is printed inside brackets, because the failure a written
#       list misses is a word that came out with the right bytes in the wrong
#       number of fields. The END after it is not decoration: printf with no
#       arguments and printf with one empty argument both write [], so without
#       a sentinel the one distinction that "$@" turns on -- no field against
#       one empty field -- is the one thing the brackets cannot show.
#
set -u

farm=${1:-/tmp/mwfarm}
subject=${2:-}
reference=${3:-/bin/dash}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

#       The farm is one binary under forty six names, and which one it is
#       being is read out of the name it was called by. A link called sh is
#       therefore the shell itself, and the lane is handed the farm rather
#       than the binary.
if [ -z "$subject" ]; then
        for name in cat echo ls; do
                [ -e "$farm/$name" ] || continue

                ln -sf "$farm/$name" "$work/sh" && subject="$work/sh"
                break
        done
fi

[ -n "$subject" ] && [ -x "$subject" ] ||
        { echo "  expand       no shell under $farm" >&2; exit 1; }

# Agreeing with dash is the whole of what passing means here, so without one
# there is nothing to compare against and saying so is better than inventing
# an answer.
[ -x "$reference" ] || { echo "  expand       no $reference, skipped"; exit 0; }

pass=0
fail=0
current=""
section_name=""
section_pass=0
section_total=0

section()
{
        [ -z "$section_name" ] ||
                printf '  %-12s %s of %s\n' \
                        "$section_name" "$section_pass" "$section_total"

        [ -z "$section_name" ] || [ -z "${TEST_TALLY:-}" ] ||
                printf '%s %s %s\n' \
                        "$section_name" "$section_pass" "$section_total" \
                        >> "$TEST_TALLY"

        section_name=$1
        section_pass=0
        section_total=0
}

group() { current=$1; }

won()
{
        pass=$((pass + 1))
        section_pass=$((section_pass + 1))
        section_total=$((section_total + 1))
}

lost()
{
        fail=$((fail + 1))
        section_total=$((section_total + 1))
        printf '  %-14s %-26s %s\n' "$current" "$1" "$2"
}

#       One fixed directory for every case that asks the filesystem, so that a
#       glob answers the same thing on a laptop as it does in a container.
#       The names are chosen to separate the questions: case, digits, a dot, a
#       dash, and a directory.
tree="$work/tree"
mkdir -p "$tree/dir"
: > "$tree/alpha"
: > "$tree/beta"
: > "$tree/Gamma"
: > "$tree/1one"
: > "$tree/2two"
: > "$tree/.hidden"
: > "$tree/a.txt"
: > "$tree/b.txt"
: > "$tree/a-b"

#       The old expansion scratch rooms were all 1024 bytes. Keep each stress
#       shape independently beyond that line: operator words, arithmetic text,
#       names, split output, a pathname, and the number of glob answers.
long_a=
long_sum=1
long_name=v
long_fields=a
deep_piece=
i=0
while [ "$i" -lt 1400 ]; do
        long_a=${long_a}a
        long_sum="$long_sum + 0"
        long_fields="$long_fields:a"
        [ "$i" -lt 180 ] && deep_piece=${deep_piece}d
        [ "$i" -lt 180 ] && long_name=${long_name}n
        i=$((i + 1))
done

deep=$tree
i=0
while [ "$i" -lt 7 ]; do
        deep="$deep/${deep_piece}$i"
        mkdir "$deep"
        i=$((i + 1))
done
: > "$deep/target"

many="$tree/many"
mkdir "$many"
i=0
while [ "$i" -lt 1200 ]; do
        name=$(printf 'item%04d' "$i")
        : > "$many/$name"
        i=$((i + 1))
done

run_both()
{
        printf '%s\n' "$*" > "$work/case.sh"

        # Bounded, because a shell under test is exactly the kind of thing
        # that loops forever, and a suite that hangs tells you nothing about
        # which case did it.
        if timeout 5 "$reference" < "$work/case.sh" > "$work/want" 2>/dev/null
        then
                want_status=0
        else
                want_status=$?
        fi

        if timeout 5 "$subject" < "$work/case.sh" > "$work/got" 2>/dev/null
        then
                got_status=0
        else
                got_status=$?
        fi
}

shown() { head -c 44 "$1" | tr '\n' '|'; }

check()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got"; then
                won
                return 0
        fi

        lost "$name" "want $(shown "$work/want")   got $(shown "$work/got")"
}

answer()
{
        name=$1
        shift
        run_both "$@"

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

# Bash extensions are compared to Bash directly and kept out of the POSIX
# reference stream. They remain ordinary pass/fail cases in the same lane.
bash_answer()
{
        name=$1
        shift

        [ -x /bin/bash ] || {
                lost "$name" "/bin/bash is required for a Bash extension case"
                return 0
        }

        held_reference=$reference
        reference=/bin/bash
        run_both "$@"
        reference=$held_reference

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$name" \
                "bash $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

bash_check()
{
        name=$1
        shift

        [ -x /bin/bash ] || {
                lost "$name" "/bin/bash is required for a Bash extension case"
                return 0
        }

        held_reference=$reference
        reference=/bin/bash
        run_both "$@"
        reference=$held_reference

        if cmp -s "$work/want" "$work/got"; then
                won
                return 0
        fi

        lost "$name" "bash $(shown "$work/want")   got $(shown "$work/got")"
}

# What ours says where dash says something else. Both halves are checked: if
# ours starts agreeing with dash the case fails and moves up into answer, and
# if ours changes to a third thing it fails too.
differs()
{
        name=$1
        recorded=$2
        recorded_status=$3
        shift 3
        run_both "$@"

        got_ours=$(shown "$work/got")

        if [ "$got_ours" != "$recorded" ] || [ "$got_status" != "$recorded_status" ]; then
                lost "$name" "recorded ${recorded}[$recorded_status]   now ${got_ours}[$got_status]"
                return 0
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                lost "$name" "agrees with dash now -- move it into answer"
                return 0
        fi

        won
}

#
#       Quoting, and what survives it.
#

section quoting

group single
check 'holds a dollar'  "x=v; printf '[%s]' '\$x' END; echo"
check 'holds a star'    "cd $tree; printf '[%s]' '*' END; echo"
check 'holds a blank'   "printf '[%s]' 'a b' END; echo"
check 'holds a brace'   "printf '[%s]' '\${x}' END; echo"
check 'holds a tick'    "printf '[%s]' '\`echo x\`' END; echo"

group double
check 'backslash dollar' 'printf "[%s]" "a\$b" END; echo'
check 'backslash quote'  'printf "[%s]" "a\"b" END; echo'
check 'backslash tick'   'printf "[%s]" "a\`b" END; echo'
check 'backslash twice'  'printf "[%s]" "a\\\\b" END; echo'
# A backslash in double quotes is special only in front of five bytes, and
# stays a backslash in front of everything else.
check 'backslash plain'  'printf "[%s]" "a\qb" END; echo'
check 'backslash digit'  'printf "[%s]" "a\1b" END; echo'
check 'single inside'    "printf '[%s]' \"a'b\" END; echo"
check 'star inside'      "cd $tree; printf '[%s]' \"*\" END; echo"
check 'adjacent'         'printf "[%s]" a"b"c END; echo'
check 'empty word'       'printf "[%s]" "" END; echo'
check 'empty single'     "printf '[%s]' '' END; echo"

group escape
check 'bare backslash'  'printf "[%s]" a\ b END; echo'
check 'escaped star'    "cd $tree; printf '[%s]' \\* END; echo"
check 'escaped dollar'  'x=v; printf "[%s]" \$x END; echo'

#
#       Parameters, in the eleven forms and in the places a name can stand.
#

section parameter

group simple
answer 'plain'          'x=v; printf "[%s]" $x END; echo'
answer 'braced'         'x=v; printf "[%s]" ${x} END; echo'
answer 'unset alone'    'printf "[%s]" $nosuch END; echo'
answer 'unset in word'  'printf "[%s]" a${nosuch}b END; echo'
answer 'length'         'x=hello; printf "[%s]" ${#x} END; echo'
answer 'length unset'   'printf "[%s]" ${#nosuch} END; echo'
answer 'digits'         'set -- a b c; printf "[%s]" $1 $2 $3 END; echo'
answer 'braced digits'  'set -- a b c; printf "[%s]" ${1}${2} END; echo'
answer 'count'          'set -- a b c; printf "[%s]" $# END; echo'
answer 'name in name'   'x=v; y=x; printf "[%s]" ${y} END; echo'

group default
answer 'dash set'       'x=v; printf "[%s]" ${x-d} END; echo'
answer 'dash empty'     'x=; printf "[%s]" ${x-d} END; echo'
answer 'dash unset'     'printf "[%s]" ${nosuch-d} END; echo'
answer 'colon dash'     'x=; printf "[%s]" ${x:-d} END; echo'
answer 'plus set'       'x=v; printf "[%s]" ${x+p} END; echo'
answer 'plus empty'     'x=; printf "[%s]" ${x+p} ${x:+p} END; echo'
answer 'assign'         'printf "[%s]" ${x=d} "$x" END; echo'
answer 'colon assign'   'x=; printf "[%s]" ${x:=d} "$x" END; echo'
answer 'complain'       'printf "[%s]" ${nosuch?} ; echo after'
answer 'complain colon' 'x=; printf "[%s]" ${x:?gone} ; echo after'
#       A bare colon remains invalid and positional-parameter slicing remains
#       rejected until the shell has Bash's parameter-array representation.
answer 'no bare colon'  'x=abc; echo "${x:}"; echo after'
answer 'no colon trim'  'x=abc; echo "${x:%b}"; echo after'
answer 'no at offset'   'set -- a b; echo "${@:1}"; echo after'
# Bash-only transformations must be implemented or refused. Handing the
# original value back says they succeeded and lets wrong data travel onward.
answer 'no length default' 'x=abc; echo "${#x:-}"; echo after'
answer 'no length trim' 'x=abc; echo "${#x%}"; echo after'
answer 'word expands'   'y=w; printf "[%s]" ${nosuch-$y} END; echo'
answer 'word is a word' 'printf "[%s]" ${nosuch-"a  b"} END; echo'
#       The tail of an unquoted one is the result of an expansion like any
#       other, so it splits: ${x-D E} is two fields and "${x-D E}" is one.
answer 'the word splits' 'printf "[%s]" ${nosuch-D E} END; echo'
answer 'the word holds'  'printf "[%s]" "${nosuch-D E}" END; echo'
answer 'plus splits'     'x=v; printf "[%s]" ${x+D E} END; echo'
answer 'splits on ifs'   'IFS=:; printf "[%s]" ${nosuch-D:E} END; echo'
answer 'assign splits'   'printf "[%s]" ${x=D E} "$x" END; echo'
answer 'nested inside'  'printf "[%s]" ${nosuch-${also-deep}} END; echo'

group bash replace
bash_answer 'replace first' 'x=abcabc; printf "[%s]" "${x/b/X}" END; echo'
bash_answer 'replace all' 'x=abcabc; printf "[%s]" "${x//b/X}" END; echo'
bash_answer 'replace no match' 'x=abc; printf "[%s]" "${x/z/X}" END; echo'
bash_answer 'replace longest' 'x=abcabc; printf "[%s]" "${x/a*c/X}" END; echo'
bash_answer 'replace earliest' 'x=abcabc; printf "[%s]" "${x/b?/X}" END; echo'
bash_answer 'replace prefix' 'x=abcabc; printf "[%s]" "${x/#a/X}" END; echo'
bash_answer 'prefix no match' 'x=abcabc; printf "[%s]" "${x/#b/X}" END; echo'
bash_answer 'replace suffix' 'x=abcabc; printf "[%s]" "${x/%bc/X}" END; echo'
bash_answer 'empty replacement' 'x=abcabc; printf "[%s]" "${x//b}" END; echo'
bash_answer 'expanded pieces' 'x=abcabc; p=b; r=X; printf "[%s]" "${x//$p/$r}" END; echo'
bash_answer 'slash in pattern' 'x="a/b/c"; printf "[%s]" "${x/\//X}" END; echo'
bash_answer 'nested replacement' 'x=abc; y=bb; printf "[%s]" "${x/b/${y/b/X}}" END; echo'
bash_answer 'matched ampersand' 'x=abc; printf "[%s]" "${x/b/[&]}" END; echo'
bash_answer 'quoted ampersand' 'x=abc; printf "[%s]" "${x/b/[\&]}" END; echo'
bash_answer 'replacement fields' 'x=a-b; printf "[%s]" ${x/-/" X Y "} END; echo'
bash_answer 'unset is empty' 'unset x; printf "[%s]" "${x/a/b}" END; echo'
bash_check 'nounset stays fatal' 'set -u; unset x; printf before; printf "%s" "${x/a/b}"; echo after'
bash_answer 'long replace all' "x=$long_a; y=\${x//a/bb}; echo \${#y}"

group bash substring
bash_answer 'substring offset' 'x=abcdef; printf "[%s]" "${x:2}" END; echo'
bash_answer 'substring length' 'x=abcdef; printf "[%s]" "${x:1:3}" END; echo'
bash_answer 'negative offset' 'x=abcdef; printf "[%s]" "${x: -2}" END; echo'
bash_answer 'negative length' 'x=abcdef; printf "[%s]" "${x:1:-1}" END; echo'
bash_answer 'empty offset' 'x=abcdef; printf "[%s]" "${x::2}" END; echo'
bash_answer 'past end' 'x=abcdef; printf "[%s]" "${x:20:3}" END; echo'
bash_answer 'before start' 'x=abcdef; printf "[%s]" "${x: -20:3}" END; echo'
bash_answer 'arithmetic offset' 'x=abcdef; n=2; printf "[%s]" "${x:n+1:2}" END; echo'
bash_answer 'ternary offset' 'x=abcdef; n=0; printf "[%s]" "${x:n?1:2:3}" END; echo'
bash_answer 'substring assigns' 'x=abcdef; n=1; printf "[%s][%s]" "${x:n+=2:2}" "$n" END; echo'
bash_answer 'substring fields' 'x="a b c"; printf "[%s]" ${x:2:3} END; echo'
bash_answer 'long substring' "x=$long_a; y=\${x:100:1200}; echo \${#y}"

group bash case
bash_answer 'uppercase first' 'x=abCDef; printf "[%s]" "${x^}" END; echo'
bash_answer 'uppercase all' 'x=abCDef; printf "[%s]" "${x^^}" END; echo'
bash_answer 'lowercase first' 'x=ABcdEF; printf "[%s]" "${x,}" END; echo'
bash_answer 'lowercase all' 'x=ABcdEF; printf "[%s]" "${x,,}" END; echo'
bash_answer 'uppercase pattern' 'x=abCDef; printf "[%s]" "${x^^[bd]}" END; echo'
bash_answer 'lowercase pattern' 'x=ABcdEF; printf "[%s]" "${x,,[AEF]}" END; echo'
bash_answer 'first must match' 'x=abcbd; printf "[%s]" "${x^[bd]}" END; echo'
bash_answer 'expanded pattern case' 'x=abCDef; p="[bd]"; printf "[%s]" "${x^^$p}" END; echo'
bash_answer 'long uppercase' "x=$long_a; y=\${x^^}; echo \${#y}:\${y:0:1}"

group bash braces
bash_answer 'brace list' 'printf "[%s]" pre{a,b}post END; echo'
bash_answer 'brace nested' 'printf "[%s]" x{a,{b,c}}y END; echo'
bash_answer 'quoted comma element' 'printf "[%s]" {a,"b,c"} END; echo'
bash_answer 'expanded comma element' 'x="b,c"; printf "[%s]" {a,"$x"} END; echo'
bash_answer 'brace product' 'printf "[%s]" {a,b}{1,2} END; echo'
bash_answer 'brace empties' 'printf "[%s]" {a,} {,b} END; echo'
bash_answer 'numeric range' 'printf "[%s]" {2..5} END; echo'
bash_answer 'numeric descending' 'printf "[%s]" {3..-1} END; echo'
bash_answer 'numeric step' 'printf "[%s]" {1..8..3} END; echo'
bash_answer 'negative step magnitude' 'printf "[%s]" {8..1..-3} END; echo'
bash_answer 'padded range' 'printf "[%s]" {-02..2} END; echo'
bash_answer 'character range' 'printf "[%s]" {d..a} END; echo'
bash_answer 'character step' 'printf "[%s]" {a..g..2} {g..a..3} END; echo'
bash_answer 'zero range step' 'printf "[%s]" {1..3..0} {c..a..0} END; echo'
bash_answer 'quoted brace literal' 'printf "[%s]" "{a,b}" END; echo'
bash_answer 'escaped brace literal' 'printf "[%s]" \{a,b\} END; echo'
bash_answer 'parameter braces skipped' 'x="a,b"; printf "[%s]" "${x}" END; echo'
bash_answer 'variable braces stay literal' 'x="{a,b}"; printf "[%s]" $x END; echo'
bash_answer 'assignment stays one' 'x={a,b}; printf "[%s]" "$x" END; echo'
bash_answer 'brace before parameter' 'x=Q; printf "[%s]" {$x,a} END; echo'
bash_answer 'large brace range' 'set -- {1..1200}; printf "%s:%s:%s\n" "$#" "$1" "${1200}"'

group trim
answer 'shortest head'  'x=a.b.c; printf "[%s]" ${x#*.} END; echo'
answer 'longest head'   'x=a.b.c; printf "[%s]" ${x##*.} END; echo'
answer 'shortest tail'  'x=a.b.c; printf "[%s]" ${x%.*} END; echo'
answer 'longest tail'   'x=a.b.c; printf "[%s]" ${x%%.*} END; echo'
answer 'no match'       'x=abc; printf "[%s]" ${x#z} END; echo'
answer 'whole value'    'x=abc; printf "[%s]" ${x#abc} END; echo'
answer 'a question'     'x=abc; printf "[%s]" ${x#?} END; echo'
answer 'question ends'  'x=abc; printf "[%s]" "${x#?}" "${x%?}" END; echo'
answer 'question empty' 'x=; printf "[%s]" "${x#?}" "${x%?}" END; echo'
answer 'question unset' 'unset x; printf "[%s]" "${x#?}" "${x%?}" END; echo'
answer 'question head nounset' 'set -u; unset x; echo before; printf "%s" "${x#?}"; echo after'
answer 'question tail nounset' 'set -u; unset x; echo before; printf "%s" "${x%?}"; echo after'
answer 'a set'          'x=abc; printf "[%s]" ${x%[bc]} END; echo'
#       The pattern is not inside the quotes around the substitution, which
#       is where "${x##*/}" used to hand back the whole path: every byte of
#       the pattern was marked quoted, so the star matched only a star.
answer 'quoted head'    'x=/a/b/c; printf "[%s]" "${x##*/}" END; echo'
answer 'quoted tail'    'x=a.b.c; printf "[%s]" "${x%%.*}" END; echo'
answer 'quoted middle'  'x=abc; printf "[%s]" "${x#*b}" END; echo'
answer 'quotes in it'   "x=abc; printf '[%s]' \"\${x#'a'}\" END; echo"
#       And a star that was written quoted is still a star to the eye and a
#       byte to the matcher, inside the enclosing quotes as much as outside.
answer 'escaped star'   'x="a*c"; printf "[%s]" "${x#a\*}" ${x#a\*} END; echo'
answer 'pattern by name' 'x=/a/b/c; y="*/"; printf "[%s]" "${x#$y}" "${x#"$y"}" END; echo'

group special
answer 'status'         'false; printf "[%s]" $?; true; printf "[%s]" $? END; echo'
answer 'status quoted'  'false; printf "[%s]" "$?" END; echo'
answer 'status braced'  'false; printf "[%s]" ${?} END; echo'
answer 'status in word' 'false; printf "[%s]" a$?b END; echo'
answer 'count in word'  'set -- a b; printf "[%s]" x${#}y END; echo'
answer 'pid is a number' 'case $$ in [0-9]*) echo yes;; *) echo no;; esac'
answer 'pid twice'      'a=$$; b=$$; [ "$a" = "$b" ] && echo same'
answer 'no last job'    'printf "[%s]" "$!" END; echo'

group at and star
answer 'at bare'        'set -- a b c; printf "[%s]" $@ END; echo'
answer 'at quoted'      'set -- "a b" c; printf "[%s]" "$@" END; echo'
#       A word that is only "$@" with no parameters is no word: f "$@" hands
#       a function nothing, where it used to hand it one empty argument.
#       Everywhere else in a word the quotes still leave an empty field.
answer 'at empty'       'set --; printf "[%s]" "$@" END; echo'
answer 'at empty braced' 'set --; printf "[%s]" "${@}" END; echo'
answer 'at empty twice' 'set --; printf "[%s]" "$@" "$@" END; echo'
answer 'at empty counted' 'set --; f() { echo $#; }; f "$@"'
answer 'at empty set'   'set --; set -- "$@"; echo $#'
answer 'at empty joined' 'set --; printf "[%s]" x"$@" END; echo'
answer 'at empty beside' 'set --; printf "[%s]" "$nosuch$@" END; echo'
answer 'at empty doubled' 'set --; printf "[%s]" "$@$@" END; echo'
answer 'star empty'     'set --; printf "[%s]" "$*" END; echo'
answer 'empty word is one' 'printf "[%s]" "" END; echo'
answer 'unset is none'  'printf "[%s]" $nosuch END; echo'
answer 'unset quoted is one' 'printf "[%s]" "$nosuch" END; echo'
answer 'at one'         'set -- "a b"; printf "[%s]" "$@" END; echo'
answer 'at joined'      'set -- a b; printf "[%s]" "x$@y" END; echo'
answer 'star quoted'    'set -- "a b" c; printf "[%s]" "$*" END; echo'
answer 'star bare'      'set -- "a b" c; printf "[%s]" $* END; echo'
answer 'star separator' 'set -- a b c; IFS=-; printf "[%s]" "$*" END; echo'
answer 'star no ifs'    'set -- a b c; IFS=; printf "[%s]" "$*" END; echo'
#       Unquoted, $* makes one field per parameter exactly as $@ does. It
#       joins on the first byte of IFS and the join is split back apart,
#       which is the same thing until IFS is empty.
answer 'bare star no ifs' 'set -- a b c; IFS=; printf "[%s]" $* END; echo'
answer 'bare star splits' 'set -- "a b" c; IFS=; printf "[%s]" $* END; echo'
#       Unquoted, an empty parameter joins to nothing and splits to nothing,
#       so it is no field. Quoted it is a field, and with IFS empty there is
#       no join and no splitting and it is a field again.
answer 'bare empty goes'  'set -- "" a; printf "[%s]" $@ END; echo'
answer 'bare empty star'  'set -- "" a; printf "[%s]" $* END; echo'
answer 'quoted empty stays' 'set -- "" a; printf "[%s]" "$@" END; echo'
answer 'all empty'        'set -- "" ""; printf "[%s]" $@ END; echo'
answer 'empty at the end' 'set -- "a b" ""; printf "[%s]" $@ END; echo'
answer 'a blank goes'     'set -- " " a; printf "[%s]" $@ END; echo'
answer 'empty no ifs'     'set -- "" a; IFS=; printf "[%s]" $@ END; echo'
answer 'empty colon ifs'  'set -- a "" b; IFS=:; printf "[%s]" $@ END; echo'
answer 'at no ifs'      'set -- a b c; IFS=; printf "[%s]" "$@" END; echo'
answer 'braced at'      'set -- "a b" c; printf "[%s]" "${@}" END; echo'
answer 'braced star'    'set -- "a b" c; printf "[%s]" "${*}" END; echo'
#       $@ and $* are set with nothing in them, not unset: the default is
#       not what an empty parameter list falls back to.
answer 'at unset'       'set --; printf "[%s]" "${@-none}" END; echo'
answer 'at unset bare'  'set --; printf "[%s]" ${@-none} END; echo'
answer 'star unset'     'set --; printf "[%s]" "${*-none}" END; echo'
answer 'at unset plus'  'set --; printf "[%s]" "${@+set}" END; echo'
answer 'at unset colon' 'set --; printf "[%s]" "${@:-none}" END; echo'
answer 'at after shift' 'set -- a b c; shift; printf "[%s]" "$@" $# END; echo'
answer 'at after block shift' 'set -- a b c d e f; shift 3; printf "[%s]" "$@" $# END; echo'
answer 'trim all'       'set -- aX aY; printf "[%s]" ${@#a} END; echo'
answer 'trim all quoted' 'set -- aX aY; printf "[%s]" "${@#a}" END; echo'
answer 'trim all star'  'set -- aX aY; printf "[%s]" ${*%Y} END; echo'
answer 'trim the join'  'set -- ab cb db eb; printf "[%s]" ${*%b} END; echo'
answer 'trim the head'  'set -- xa xb xc; printf "[%s]" ${@#x} END; echo'
answer 'trim all no ifs' 'set -- aX aY; IFS=; printf "[%s]" ${@#a} END; echo'
answer 'trim all colon' 'set -- aX aY; IFS=:; printf "[%s]" ${@#a} END; echo'
answer 'trim all blanks' 'set -- "a b" c; printf "[%s]" "${@#a}" END; echo'

group long
answer 'long simple name' "$long_name=ok; echo \$$long_name"
answer 'long braced name' "$long_name=ok; echo \${$long_name}"
answer 'long default word' "unset x; x=\${x-$long_a}; echo \${#x}"
answer 'long trim pattern' "x=$long_a; printf '[%s]' \"\${x%$long_a}\" END; echo"

#
#       Command substitution, in both spellings and nested.
#

section command

group substitution
answer 'plain'          'printf "[%s]" $(echo x) END; echo'
answer 'quoted'         'printf "[%s]" "$(echo a b)" END; echo'
answer 'unquoted splits' 'printf "[%s]" $(echo a b) END; echo'
answer 'backtick'       'printf "[%s]" `echo x` END; echo'
answer 'quotes inside'  'printf "[%s]" "$(echo "a b")" END; echo'
answer 'nested'         'printf "[%s]" "$(echo "$(echo deep)")" END; echo'
answer 'nested backtick' 'printf "[%s]" "`echo \`echo deep\``" END; echo'
answer 'mixed nesting'  'printf "[%s]" $(echo `echo x`) END; echo'
answer 'newlines go'    'printf "[%s]" "$(printf "a\nb\n\n\n")" END; echo'
answer 'empty output'   'printf "[%s]" "$(true)" END; echo'
answer 'in the middle'  'printf "[%s]" a$(echo b)c END; echo'
answer 'a dollar out'   'printf "[%s]" "$(echo "\$x")" END; echo'

#
#       Arithmetic.
#

section arithmetic

group precedence
answer 'the four'       'echo $((1 + 2 * 3 - 4 / 2))'
answer 'brackets'       'echo $(( (1 + 2) * 3 ))'
answer 'unary'          'echo $((-3 + +4)) $((!0)) $((!7)) $((~5))'
answer 'shifts'         'echo $((1 << 4)) $((256 >> 4)) $((-8 >> 1))'
answer 'the three bits' 'echo $((6 & 3)) $((6 | 3)) $((6 ^ 3))'
answer 'compare'        'echo $((1 < 2)) $((2 <= 2)) $((3 > 4)) $((3 >= 4))'
answer 'equality'       'echo $((2 == 2)) $((2 != 2))'
answer 'logical'        'echo $((1 && 0)) $((1 || 0)) $((0 && 1))'
answer 'ternary'        'echo $((1 ? 2 : 3)) $((0 ? 2 : 3))'
answer 'ternary nested' 'echo $((1 ? 0 ? 4 : 5 : 6))'
answer 'modulo'         'echo $((7 % 3)) $((-7 / 2)) $((-7 % 3))'

group bases
answer 'hex'            'echo $((0x10)) $((0xff)) $((0XFF))'
answer 'octal'          'echo $((010)) $((0777))'
answer 'hex cursor'     'echo $((0x1+2)) $((0Xf*2))'
answer 'octal cursor'   'echo $((07+1)) $((077*2))'
answer 'a big one'      'echo $((0x7fffffffffffffff))'
answer 'hex saturates'  'echo $((0xffffffffffffffff))'
answer 'hex far over'   'echo $((0xffffffffffffffffffffffffffffffff))'
answer 'decimal saturates' 'echo $((18446744073709551615))'
answer 'octal saturates' 'echo $((0777777777777777777777))'
answer 'long expression' "echo \$(( $long_sum ))"

group names
answer 'a name reads'   'x=5; echo $((x + 1))'
answer 'unset is zero'  'echo $((nosuch + 1))'
answer 'empty is zero'  'x=; echo $((x + 1))'
answer 'a name writes'  'x=5; echo $((x = x * 2)) $x'
answer 'compound'       'x=5; echo $((x += 3)) $((x -= 1)) $((x *= 2)) $x'
answer 'compound bits'  'x=6; echo $((x &= 3)) $((x |= 8)) $((x ^= 1)) $x'
answer 'compound shift' 'x=1; echo $((x <<= 4)) $((x >>= 2)) $x'
answer 'a name is read' 'x=010; echo $((x + 0))'
answer 'spaces inside'  'x=" 5 "; echo $((x + 1))'
answer 'dollar inside'  'x=5; echo $(($x + 1))'
answer 'name plus literal' 'x=7; echo $((x+1)) $(( x + 010 ))'
answer 'name minus literal' 'x=7; echo $((x-2)) $(( x - 0x3 ))'
answer 'name add falls through' 'x=2; echo $((x + 1 < 4)) $((x + 1 * 2))'

group refused
#       Every one of these answered with a number the script never computed.
#       A divide by zero was zero, a missing operand was whatever had been
#       read so far, and a name holding a word was a name holding nothing.
answer 'divide by zero' 'echo $((5 / 0)); echo after'
answer 'modulo by zero' 'echo $((5 % 0)); echo after'
answer 'assigning zero' 'x=1; echo $((x /= 0)); echo after'
answer 'missing right'  'echo $((1 + )); echo after'
answer 'missing left'   'echo $(( * 3 )); echo after'
answer 'no such power'  'echo $((2 ** 3)); echo after'
answer 'a word is not'  'x=bar; echo $((x + 1)); echo after'
answer 'half a word'    'x=12ab; echo $((x)); echo after'
answer 'empty expression' 'echo $(( ))'
answer 'comma operator' 'echo $((1,2))'
answer 'bad octal'      'echo $((08))'
answer 'post increment' 'x=1; echo $((x++)) $x'
#       The smallest number there is has no positive opposite, and the
#       machine faults rather than saying so: this used to kill the shell.
answer 'the least over minus one' \
        'x=-9223372036854775807; x=$((x - 1)); echo $((x / -1)); echo after'
answer 'the same modulo' \
        'x=-9223372036854775807; x=$((x - 1)); echo $((x % -1)); echo after'

group width
answer 'the largest'    'echo $((9223372036854775807))'
answer 'over the top'   'echo $((9223372036854775807 + 1))'
answer 'under the floor' 'echo $((-9223372036854775807 - 1))'
answer 'product wraps'  'echo $((9223372036854775807 * 9223372036854775807))'
answer 'shift wraps'    'echo $((1 << 64)) $((1 << -1)) $((-1 >> 64))'
answer 'negate floor'   'echo $((-(-9223372036854775807 - 1)))'

#
#       Field splitting.
#

section splitting

group ifs
answer 'default blanks' 'x="a b	c"; printf "[%s]" $x END; echo'
answer 'runs are one'   'x="a   b"; printf "[%s]" $x END; echo'
answer 'leading blanks' 'x="   a"; printf "[%s]" $x END; echo'
answer 'trailing blanks' 'x="a   "; printf "[%s]" $x END; echo'
answer 'all blanks'     'x="   "; printf "[%s]" $x END; echo'
answer 'a colon'        'IFS=:; x="a:b:c"; printf "[%s]" $x END; echo'
answer 'colon empty'    'IFS=:; x="a::b"; printf "[%s]" $x END; echo'
answer 'colon leading'  'IFS=:; x=":a"; printf "[%s]" $x END; echo'
answer 'colon trailing' 'IFS=:; x="a:"; printf "[%s]" $x END; echo'
answer 'colon alone'    'IFS=:; x=":"; printf "[%s]" $x END; echo'
answer 'blank and colon' 'IFS=" :"; x="a : b"; printf "[%s]" $x END; echo'
answer 'blank then colon' 'IFS=" :"; x="a  :  b"; printf "[%s]" $x END; echo'
answer 'colon then blank' 'IFS=" :"; x="a::b"; printf "[%s]" $x END; echo'
answer 'ifs empty'      'IFS=; x="a b"; printf "[%s]" $x END; echo'
answer 'ifs restored'   'IFS=:; IFS=" "; x="a:b c"; printf "[%s]" $x END; echo'
answer 'quoted holds'   'IFS=:; x="a:b"; printf "[%s]" "$x" END; echo'
answer 'literal blank'  'printf "[%s]" a" "b END; echo'
answer 'only expansions split' 'x="a b"; printf "[%s]" "$x"c END; echo'
answer 'substitution splits' 'IFS=:; printf "[%s]" $(echo a:b) END; echo'
answer 'long split output' "IFS=:; x=$long_fields; set -- \$x; echo \$#"
answer 'long captured value' \
        'unset x; : "${x:=$(i=0; while [ "$i" -lt 1400 ]; do printf a; i=$((i + 1)); done)}"; echo ${#x}'

#
#       Pathname expansion.
#

section glob

group patterns
answer 'a star'         "cd $tree; printf '[%s]' * END; echo"
answer 'a question'     "cd $tree; printf '[%s]' ?one END; echo"
answer 'a suffix'       "cd $tree; printf '[%s]' *.txt END; echo"
answer 'a prefix'       "cd $tree; printf '[%s]' a* END; echo"
answer 'in the middle'  "cd $tree; printf '[%s]' a*t END; echo"
answer 'a directory'    "cd $tree; printf '[%s]' */ END; echo"
answer 'a path'         "printf '[%s]' $tree/a* END; echo"
answer 'no match'       "cd $tree; printf '[%s]' nosuch* END; echo"
answer 'no match path'  "cd $tree; printf '[%s]' nosuch*/x END; echo"
answer 'long path'      "printf '[%s]' $deep/t* END; echo"
answer 'many answers'   "cd $many; set -- item*; echo \"\$#:\$1\""

group sets
answer 'a range'        "cd $tree; printf '[%s]' [a-b]* END; echo"
answer 'a list'         "cd $tree; printf '[%s]' [ab].txt END; echo"
answer 'negated'        "cd $tree; printf '[%s]' [!a]*.txt END; echo"
answer 'negated caret'  "cd $tree; printf '[%s]' [^a]*.txt END; echo"
answer 'a dash inside'  "cd $tree; printf '[%s]' a[-.]* END; echo"
answer 'lone opener literal' "cd $tree; printf '[%s]' [ END; echo"
answer 'unclosed is a byte' "cd $tree; printf '[%s]' '[ab' END; echo"
#       [[:alpha:]] used to be a set holding a bracket, a colon and five
#       letters, because the ] that closes the class was read as the ] that
#       closes the set.
answer 'class alpha'    "cd $tree; printf '[%s]' [[:alpha:]]* END; echo"
answer 'class digit'    "cd $tree; printf '[%s]' [[:digit:]]* END; echo"
answer 'class upper'    "cd $tree; printf '[%s]' [[:upper:]]* END; echo"
answer 'class lower'    "cd $tree; printf '[%s]' [[:lower:]]* END; echo"
answer 'class alnum'    "cd $tree; printf '[%s]' [[:alnum:]]* END; echo"
answer 'class punct'    "cd $tree; printf '[%s]' *[[:punct:]]* END; echo"
answer 'class xdigit'   "cd $tree; printf '[%s]' [[:xdigit:]]* END; echo"
answer 'two classes'    "cd $tree; printf '[%s]' [[:upper:][:digit:]]* END; echo"
answer 'class negated'  "cd $tree; printf '[%s]' [![:digit:]]*.txt END; echo"
answer 'class and byte' "cd $tree; printf '[%s]' [[:digit:]b]*.txt END; echo"
answer 'no such class'  "cd $tree; printf '[%s]' [[:nosuch:]]* END; echo"
answer 'class trims'    'x=abc9; printf "[%s]" ${x%[[:digit:]]} END; echo'

group hidden
#       A leading dot is not what a star matches, which is the one rule that
#       keeps rm * out of the dot files.
answer 'star skips dots' "cd $tree; printf '[%s]' * END; echo"
answer 'dot star'       "cd $tree; printf '[%s]' .* END; echo"
answer 'dot named'      "cd $tree; printf '[%s]' .h* END; echo"
answer 'a set is not a dot' "cd $tree; printf '[%s]' [.]* END; echo"

group quoted
answer 'quoted star'    "cd $tree; printf '[%s]' '*' END; echo"
answer 'star by name'   "cd $tree; x='*'; printf '[%s]' \"\$x\" END; echo"
answer 'star from name' "cd $tree; x='*'; printf '[%s]' \$x END; echo"
answer 'half quoted'    "cd $tree; printf '[%s]' a'*' END; echo"
answer 'no glob flag'   "cd $tree; set -f; printf '[%s]' * END; echo"

#
#       Tilde.
#

section tilde
group tilde
answer 'only in front'  'printf "[%s]" a~ "~" "a~b" END; echo'
answer 'not after slash' 'printf "[%s]" /~ END; echo'

#
#       What ours says where dash says something else.
#

section diverges

group trim
#       ${*%pat} and ${@%pat} take the parameters joined and cut the join, so
#       set -- ab cb db eb loses one b and not four -- which is what POSIX
#       says and what dash does. POSIX explicitly leaves these two parameter
#       forms unspecified, though: bash and ksh trim each parameter on its own,
#       dash cuts from an interior match through the join, and ours applies the
#       ordinary whole-value suffix rule. There is no portable answer to chase.
differs 'the join is cut' '[ab][cd][END]|' 0 \
        'set -- ab cd; printf "[%s]" ${*%b} END; echo'

group tilde
#       ~name wants a password file and the image this ships in has none, so
#       the name is left standing rather than answered with a guess.
differs 'no home'       '~root|' 0 'echo ~root'

#
#       Generated.
#
#       A fixed cross product rather than a list, and rather than a random
#       one: the same questions every run, so a failure is a change in the
#       shell and never a change in the seed.
#
#       Every field is printed inside brackets. A word that came out with the
#       right bytes in the wrong number of fields is the failure a written
#       list misses, and it is invisible without them.
#

section generated

forms='printf "[%s]" $x END
printf "[%s]" "$x" END
printf "[%s]" a${x}b END
printf "[%s]" "a${x}b" END
printf "[%s]" ${x-D} END
printf "[%s]" "${x-D}" END
printf "[%s]" ${x:-D E} END
printf "[%s]" "${x:+Y Z}" END
printf "[%s]" ${x#a} END
printf "[%s]" "${x#a}" END
printf "[%s]" ${x##*b} END
printf "[%s]" "${x%c}" END
printf "[%s]" "${x%%*}" END
printf "[%s]" ${#x} END
set -- $x; printf "[%s]" "$@" END; printf "(%s)" $#
set -- $x; printf "[%s]" "$*" END
set -- $x; printf "[%s]" $* END
set -- "$x"; printf "[%s]" "$@" END
set -- $x; printf "[%s]" ${@#a} END
set -- $x; printf "[%s]" "${@#a}" END'

made=0

emit()
{
        printf 'cd %s\n%s\n%s\n%s\necho\n' "$tree" "$1" "$2" "$3" > "$work/gen.sh"

        if timeout 5 "$reference" < "$work/gen.sh" > "$work/want" 2>/dev/null
        then
                want_status=0
        else
                want_status=$?
        fi

        if timeout 5 "$subject" < "$work/gen.sh" > "$work/got" 2>/dev/null
        then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
                return 0
        fi

        lost "$3" \
                "$2 $1   want $(shown "$work/want")[$want_status]   got $(shown "$work/got")[$got_status]"
}

group generated

old_ifs=$IFS

for setting in 'unset IFS' 'IFS=:' 'IFS=" :"' 'IFS=' 'IFS=" "'; do
        for value in 'unset x' 'x=' 'x=a' 'x="a b"' 'x="a  b"' 'x="*"' \
                'x="a*c"' 'x="a:b"' 'x=" a:b "' 'x=abc'; do
                IFS='
'
                for form in $forms; do
                        IFS=$old_ifs
                        emit "$setting" "$value" "$form"
                        made=$((made + 1))
                done

                IFS=$old_ifs
        done
done

IFS=$old_ifs

section ""

echo
printf '  %s of %s\n' "$pass" "$((pass + fail))"

[ "$fail" = 0 ]
