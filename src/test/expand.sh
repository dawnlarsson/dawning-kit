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
#                   list somebody thought of. Every field is printed inside
#                   brackets, because the failure a written list misses is a
#                   word that came out with the right bytes in the wrong
#                   number of fields.
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
check 'holds a dollar'  "x=v; printf '[%s]' '\$x'; echo"
check 'holds a star'    "cd $tree; printf '[%s]' '*'; echo"
check 'holds a blank'   "printf '[%s]' 'a b'; echo"
check 'holds a brace'   "printf '[%s]' '\${x}'; echo"
check 'holds a tick'    "printf '[%s]' '\`echo x\`'; echo"

group double
check 'backslash dollar' 'printf "[%s]" "a\$b"; echo'
check 'backslash quote'  'printf "[%s]" "a\"b"; echo'
check 'backslash tick'   'printf "[%s]" "a\`b"; echo'
check 'backslash twice'  'printf "[%s]" "a\\\\b"; echo'
# A backslash in double quotes is special only in front of five bytes, and
# stays a backslash in front of everything else.
check 'backslash plain'  'printf "[%s]" "a\qb"; echo'
check 'backslash digit'  'printf "[%s]" "a\1b"; echo'
check 'single inside'    "printf '[%s]' \"a'b\"; echo"
check 'star inside'      "cd $tree; printf '[%s]' \"*\"; echo"
check 'adjacent'         'printf "[%s]" a"b"c; echo'
check 'empty word'       'printf "[%s]" ""; echo'
check 'empty single'     "printf '[%s]' ''; echo"

group escape
check 'bare backslash'  'printf "[%s]" a\ b; echo'
check 'escaped star'    "cd $tree; printf '[%s]' \\*; echo"
check 'escaped dollar'  'x=v; printf "[%s]" \$x; echo'

#
#       Parameters, in the eleven forms and in the places a name can stand.
#

section parameter

group simple
answer 'plain'          'x=v; printf "[%s]" $x; echo'
answer 'braced'         'x=v; printf "[%s]" ${x}; echo'
answer 'unset alone'    'printf "[%s]" $nosuch; echo'
answer 'unset in word'  'printf "[%s]" a${nosuch}b; echo'
answer 'length'         'x=hello; printf "[%s]" ${#x}; echo'
answer 'length unset'   'printf "[%s]" ${#nosuch}; echo'
answer 'digits'         'set -- a b c; printf "[%s]" $1 $2 $3; echo'
answer 'braced digits'  'set -- a b c; printf "[%s]" ${1}${2}; echo'
answer 'count'          'set -- a b c; printf "[%s]" $#; echo'
answer 'name in name'   'x=v; y=x; printf "[%s]" ${y}; echo'

group default
answer 'dash set'       'x=v; printf "[%s]" ${x-d}; echo'
answer 'dash empty'     'x=; printf "[%s]" ${x-d}; echo'
answer 'dash unset'     'printf "[%s]" ${nosuch-d}; echo'
answer 'colon dash'     'x=; printf "[%s]" ${x:-d}; echo'
answer 'plus set'       'x=v; printf "[%s]" ${x+p}; echo'
answer 'plus empty'     'x=; printf "[%s]" ${x+p} ${x:+p}; echo'
answer 'assign'         'printf "[%s]" ${x=d} "$x"; echo'
answer 'colon assign'   'x=; printf "[%s]" ${x:=d} "$x"; echo'
answer 'complain'       'printf "[%s]" ${nosuch?} ; echo after'
answer 'complain colon' 'x=; printf "[%s]" ${x:?gone} ; echo after'
#       A colon says one of the four is coming and nothing else. ${x:1:1} is
#       a substring in three shells and in no part of POSIX, and used to hand
#       back the whole value here -- wrong against either reading.
answer 'no substring'   'x=abc; echo "${x:1:1}"; echo after'
answer 'no offset'      'x=abc; echo "${x:1}"; echo after'
answer 'no bare colon'  'x=abc; echo "${x:}"; echo after'
answer 'no colon trim'  'x=abc; echo "${x:%b}"; echo after'
answer 'no at offset'   'set -- a b; echo "${@:1}"; echo after'
answer 'word expands'   'y=w; printf "[%s]" ${nosuch-$y}; echo'
answer 'word is a word' 'printf "[%s]" ${nosuch-"a  b"}; echo'
#       The tail of an unquoted one is the result of an expansion like any
#       other, so it splits: ${x-D E} is two fields and "${x-D E}" is one.
answer 'the word splits' 'printf "[%s]" ${nosuch-D E}; echo'
answer 'the word holds'  'printf "[%s]" "${nosuch-D E}"; echo'
answer 'plus splits'     'x=v; printf "[%s]" ${x+D E}; echo'
answer 'splits on ifs'   'IFS=:; printf "[%s]" ${nosuch-D:E}; echo'
answer 'assign splits'   'printf "[%s]" ${x=D E} "$x"; echo'
answer 'nested inside'  'printf "[%s]" ${nosuch-${also-deep}}; echo'

group trim
answer 'shortest head'  'x=a.b.c; printf "[%s]" ${x#*.}; echo'
answer 'longest head'   'x=a.b.c; printf "[%s]" ${x##*.}; echo'
answer 'shortest tail'  'x=a.b.c; printf "[%s]" ${x%.*}; echo'
answer 'longest tail'   'x=a.b.c; printf "[%s]" ${x%%.*}; echo'
answer 'no match'       'x=abc; printf "[%s]" ${x#z}; echo'
answer 'whole value'    'x=abc; printf "[%s]" ${x#abc}; echo'
answer 'a question'     'x=abc; printf "[%s]" ${x#?}; echo'
answer 'a set'          'x=abc; printf "[%s]" ${x%[bc]}; echo'
#       The pattern is not inside the quotes around the substitution, which
#       is where "${x##*/}" used to hand back the whole path: every byte of
#       the pattern was marked quoted, so the star matched only a star.
answer 'quoted head'    'x=/a/b/c; printf "[%s]" "${x##*/}"; echo'
answer 'quoted tail'    'x=a.b.c; printf "[%s]" "${x%%.*}"; echo'
answer 'quoted middle'  'x=abc; printf "[%s]" "${x#*b}"; echo'
answer 'quotes in it'   "x=abc; printf '[%s]' \"\${x#'a'}\"; echo"
#       And a star that was written quoted is still a star to the eye and a
#       byte to the matcher, inside the enclosing quotes as much as outside.
answer 'escaped star'   'x="a*c"; printf "[%s]" "${x#a\*}" ${x#a\*}; echo'
answer 'pattern by name' 'x=/a/b/c; y="*/"; printf "[%s]" "${x#$y}" "${x#"$y"}"; echo'

group special
answer 'status'         'false; printf "[%s]" $?; true; printf "[%s]" $?; echo'
answer 'status quoted'  'false; printf "[%s]" "$?"; echo'
answer 'status braced'  'false; printf "[%s]" ${?}; echo'
answer 'status in word' 'false; printf "[%s]" a$?b; echo'
answer 'count in word'  'set -- a b; printf "[%s]" x${#}y; echo'
answer 'pid is a number' 'case $$ in [0-9]*) echo yes;; *) echo no;; esac'
answer 'pid twice'      'a=$$; b=$$; [ "$a" = "$b" ] && echo same'
answer 'no last job'    'printf "[%s]" "$!"; echo'

group at and star
answer 'at bare'        'set -- a b c; printf "[%s]" $@; echo'
answer 'at quoted'      'set -- "a b" c; printf "[%s]" "$@"; echo'
answer 'at empty'       'set --; printf "[%s]" "$@"; echo'
answer 'at one'         'set -- "a b"; printf "[%s]" "$@"; echo'
answer 'at joined'      'set -- a b; printf "[%s]" "x$@y"; echo'
answer 'star quoted'    'set -- "a b" c; printf "[%s]" "$*"; echo'
answer 'star bare'      'set -- "a b" c; printf "[%s]" $*; echo'
answer 'star separator' 'set -- a b c; IFS=-; printf "[%s]" "$*"; echo'
answer 'star no ifs'    'set -- a b c; IFS=; printf "[%s]" "$*"; echo'
#       Unquoted, $* makes one field per parameter exactly as $@ does. It
#       joins on the first byte of IFS and the join is split back apart,
#       which is the same thing until IFS is empty.
answer 'bare star no ifs' 'set -- a b c; IFS=; printf "[%s]" $*; echo'
answer 'bare star splits' 'set -- "a b" c; IFS=; printf "[%s]" $*; echo'
answer 'at no ifs'      'set -- a b c; IFS=; printf "[%s]" "$@"; echo'
answer 'braced at'      'set -- "a b" c; printf "[%s]" "${@}"; echo'
answer 'braced star'    'set -- "a b" c; printf "[%s]" "${*}"; echo'
#       $@ and $* are set with nothing in them, not unset: the default is
#       not what an empty parameter list falls back to.
answer 'at unset'       'set --; printf "[%s]" "${@-none}"; echo'
answer 'at unset bare'  'set --; printf "[%s]" ${@-none}; echo'
answer 'star unset'     'set --; printf "[%s]" "${*-none}"; echo'
answer 'at unset plus'  'set --; printf "[%s]" "${@+set}"; echo'
answer 'at unset colon' 'set --; printf "[%s]" "${@:-none}"; echo'
answer 'at after shift' 'set -- a b c; shift; printf "[%s]" "$@" $#; echo'

#
#       Command substitution, in both spellings and nested.
#

section command

group substitution
answer 'plain'          'printf "[%s]" $(echo x); echo'
answer 'quoted'         'printf "[%s]" "$(echo a b)"; echo'
answer 'unquoted splits' 'printf "[%s]" $(echo a b); echo'
answer 'backtick'       'printf "[%s]" `echo x`; echo'
answer 'quotes inside'  'printf "[%s]" "$(echo "a b")"; echo'
answer 'nested'         'printf "[%s]" "$(echo "$(echo deep)")"; echo'
answer 'nested backtick' 'printf "[%s]" "`echo \`echo deep\``"; echo'
answer 'mixed nesting'  'printf "[%s]" $(echo `echo x`); echo'
answer 'newlines go'    'printf "[%s]" "$(printf "a\nb\n\n\n")"; echo'
answer 'empty output'   'printf "[%s]" "$(true)"; echo'
answer 'in the middle'  'printf "[%s]" a$(echo b)c; echo'
answer 'a dollar out'   'printf "[%s]" "$(echo "\$x")"; echo'

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
answer 'a big one'      'echo $((0x7fffffffffffffff))'

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

#
#       Field splitting.
#

section splitting

group ifs
answer 'default blanks' 'x="a b	c"; printf "[%s]" $x; echo'
answer 'runs are one'   'x="a   b"; printf "[%s]" $x; echo'
answer 'leading blanks' 'x="   a"; printf "[%s]" $x; echo'
answer 'trailing blanks' 'x="a   "; printf "[%s]" $x; echo'
answer 'all blanks'     'x="   "; printf "[%s]" $x; echo'
answer 'a colon'        'IFS=:; x="a:b:c"; printf "[%s]" $x; echo'
answer 'colon empty'    'IFS=:; x="a::b"; printf "[%s]" $x; echo'
answer 'colon leading'  'IFS=:; x=":a"; printf "[%s]" $x; echo'
answer 'colon trailing' 'IFS=:; x="a:"; printf "[%s]" $x; echo'
answer 'colon alone'    'IFS=:; x=":"; printf "[%s]" $x; echo'
answer 'blank and colon' 'IFS=" :"; x="a : b"; printf "[%s]" $x; echo'
answer 'blank then colon' 'IFS=" :"; x="a  :  b"; printf "[%s]" $x; echo'
answer 'colon then blank' 'IFS=" :"; x="a::b"; printf "[%s]" $x; echo'
answer 'ifs empty'      'IFS=; x="a b"; printf "[%s]" $x; echo'
answer 'ifs restored'   'IFS=:; IFS=" "; x="a:b c"; printf "[%s]" $x; echo'
answer 'quoted holds'   'IFS=:; x="a:b"; printf "[%s]" "$x"; echo'
answer 'literal blank'  'printf "[%s]" a" "b; echo'
answer 'only expansions split' 'x="a b"; printf "[%s]" "$x"c; echo'
answer 'substitution splits' 'IFS=:; printf "[%s]" $(echo a:b); echo'

#
#       Pathname expansion.
#

section glob

group patterns
answer 'a star'         "cd $tree; printf '[%s]' *; echo"
answer 'a question'     "cd $tree; printf '[%s]' ?one; echo"
answer 'a suffix'       "cd $tree; printf '[%s]' *.txt; echo"
answer 'a prefix'       "cd $tree; printf '[%s]' a*; echo"
answer 'in the middle'  "cd $tree; printf '[%s]' a*t; echo"
answer 'a directory'    "cd $tree; printf '[%s]' */; echo"
answer 'a path'         "printf '[%s]' $tree/a*; echo"
answer 'no match'       "cd $tree; printf '[%s]' nosuch*; echo"
answer 'no match path'  "cd $tree; printf '[%s]' nosuch*/x; echo"

group sets
answer 'a range'        "cd $tree; printf '[%s]' [a-b]*; echo"
answer 'a list'         "cd $tree; printf '[%s]' [ab].txt; echo"
answer 'negated'        "cd $tree; printf '[%s]' [!a]*.txt; echo"
answer 'negated caret'  "cd $tree; printf '[%s]' [^a]*.txt; echo"
answer 'a dash inside'  "cd $tree; printf '[%s]' a[-.]*; echo"
answer 'unclosed is a byte' "cd $tree; printf '[%s]' '[ab'; echo"
#       [[:alpha:]] used to be a set holding a bracket, a colon and five
#       letters, because the ] that closes the class was read as the ] that
#       closes the set.
answer 'class alpha'    "cd $tree; printf '[%s]' [[:alpha:]]*; echo"
answer 'class digit'    "cd $tree; printf '[%s]' [[:digit:]]*; echo"
answer 'class upper'    "cd $tree; printf '[%s]' [[:upper:]]*; echo"
answer 'class lower'    "cd $tree; printf '[%s]' [[:lower:]]*; echo"
answer 'class alnum'    "cd $tree; printf '[%s]' [[:alnum:]]*; echo"
answer 'class punct'    "cd $tree; printf '[%s]' *[[:punct:]]*; echo"
answer 'class xdigit'   "cd $tree; printf '[%s]' [[:xdigit:]]*; echo"
answer 'two classes'    "cd $tree; printf '[%s]' [[:upper:][:digit:]]*; echo"
answer 'class negated'  "cd $tree; printf '[%s]' [![:digit:]]*.txt; echo"
answer 'class and byte' "cd $tree; printf '[%s]' [[:digit:]b]*.txt; echo"
answer 'no such class'  "cd $tree; printf '[%s]' [[:nosuch:]]*; echo"
answer 'class trims'    'x=abc9; printf "[%s]" ${x%[[:digit:]]}; echo'

group hidden
#       A leading dot is not what a star matches, which is the one rule that
#       keeps rm * out of the dot files.
answer 'star skips dots' "cd $tree; printf '[%s]' *; echo"
answer 'dot star'       "cd $tree; printf '[%s]' .*; echo"
answer 'dot named'      "cd $tree; printf '[%s]' .h*; echo"
answer 'a set is not a dot' "cd $tree; printf '[%s]' [.]*; echo"

group quoted
answer 'quoted star'    "cd $tree; printf '[%s]' '*'; echo"
answer 'star by name'   "cd $tree; x='*'; printf '[%s]' \"\$x\"; echo"
answer 'star from name' "cd $tree; x='*'; printf '[%s]' \$x; echo"
answer 'half quoted'    "cd $tree; printf '[%s]' a'*'; echo"
answer 'no glob flag'   "cd $tree; set -f; printf '[%s]' *; echo"

#
#       Tilde.
#

section tilde
group tilde
answer 'only in front'  'printf "[%s]" a~ "~" "a~b"; echo'
answer 'not after slash' 'printf "[%s]" /~; echo'

#
#       What ours says where dash says something else.
#

section diverges

group arithmetic
#       Nothing between the brackets is nothing to get wrong, and zero is
#       what an empty value reads as everywhere else here. dash calls it a
#       syntax error instead.
differs 'empty is zero' '0|' 0 'echo $(( ))'
#       There is no comma operator, so the cursor stops after the first
#       expression and what follows it is never read.
differs 'comma is not'  '1|' 0 'echo $((1,2))'
#       A leading zero says octal, and an eight is not an octal digit -- so
#       this is read as decimal rather than refused.
differs 'a bad number'  '8|' 0 'echo $((08))'
#       ++ and -- are here, which is one more than POSIX asks for.
differs 'has increment' '1 2|' 0 'x=1; echo $((x++)) $x'

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

forms='printf "[%s]" $x
printf "[%s]" "$x"
printf "[%s]" a${x}b
printf "[%s]" "a${x}b"
printf "[%s]" ${x-D}
printf "[%s]" "${x-D}"
printf "[%s]" ${x:-D E}
printf "[%s]" "${x:+Y Z}"
printf "[%s]" ${x#a}
printf "[%s]" "${x#a}"
printf "[%s]" ${x##*b}
printf "[%s]" "${x%c}"
printf "[%s]" "${x%%*}"
printf "[%s]" ${#x}
set -- $x; printf "[%s]" "$@"; printf "(%s)" $#
set -- $x; printf "[%s]" "$*"
set -- $x; printf "[%s]" $*
set -- "$x"; printf "[%s]" "$@"'

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
