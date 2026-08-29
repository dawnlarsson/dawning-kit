#!/bin/sh
#
#       The builtins: the commands the shell answers itself.
#
#           sh src/test/builtin.sh [directory of the names it answers to] [shell]
#
#       Every case runs the same input through the reference the case names
#       and through ours, and compares. Agreeing with the reference is what
#       passing means; there is no separate idea here of the right answer.
#
#       Two references, and each case says which:
#
#         answer    dash. Same bytes on standard output and the same exit
#                   status. dash is taken as the reference because it is the
#                   smallest thing that is actually correct.
#         written   POSIX, or this shell's own documented answer, for the
#                   places dash has nothing to say: read -n, -d and -p are not
#                   options it has, and putting OPTIND back to one starts
#                   getopts again in POSIX and does not in dash.
#
set -e

farm=${1:-/tmp/mwfarm}
subject=${2:-}
reference=${3:-/bin/dash}

#       The runner hands this lane the farm and nothing else. Every name in it
#       is a link to the one binary, so the shell is whatever they point at.
if [ -z "$subject" ]; then
        subject=$(readlink "$farm/cat" 2>/dev/null) || subject=""

        [ -x "$subject" ] || subject="$farm/../shell"
fi

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }

[ -x "$reference" ] || { echo "  builtin      no $reference, skipped"; exit 0; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

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

#       A case is a name and a fragment. The fragment is fed to both shells on
#       standard input, which is how a script arrives, and from inside the work
#       directory, which is where the files the cases talk about are.
run_both()
{
        printf '%s\n' "$*" > "$work/case.sh"

        # Bounded, because a shell under test is exactly the kind of thing
        # that loops forever, and a suite that hangs tells you nothing about
        # which case did it.
        if (cd "$work" && timeout 5 "$reference" < "$work/case.sh") \
                > "$work/want" 2>/dev/null
        then
                want_status=0
        else
                want_status=$?
        fi

        if (cd "$work" && timeout 5 "$subject" < "$work/case.sh") \
                > "$work/got" 2>/dev/null
        then
                got_status=0
        else
                got_status=$?
        fi
}

shown() { head -c 60 "$1" | tr '\n' '|'; }

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

#       What POSIX asks for, where dash is not the thing to ask. The expected
#       output has its newlines written as | so a case stays one line.
written()
{
        name=$1
        want=$2
        want_code=$3
        shift 3

        printf '%s\n' "$*" > "$work/case.sh"

        if (cd "$work" && timeout 5 "$subject" < "$work/case.sh") \
                > "$work/got" 2>/dev/null
        then
                got_status=0
        else
                got_status=$?
        fi

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$want" ] && [ "$got_status" = "$want_code" ]; then
                won
                return 0
        fi

        lost "$name" "expected $want[$want_code], got $got_ours[$got_status]"
}

#
#       The files the cases about test talk about. Made rather than found,
#       because a machine that happens to have a setuid binary lying where the
#       last one did is not something to build a suite on.
#
: > "$work/older"
sleep 1
echo abc > "$work/newer"
echo abc > "$work/same"
ln "$work/newer" "$work/link" 2>/dev/null || cp "$work/newer" "$work/link"
ln -s newer "$work/soft"
mkdir -p "$work/dir"
chmod 1755 "$work/dir"
: > "$work/setuid" && chmod 4644 "$work/setuid"
: > "$work/setgid" && chmod 2644 "$work/setgid"
: > "$work/none" && chmod 0 "$work/none"
mkfifo "$work/fifo" 2>/dev/null || true

section test

#
#       POSIX resolves one, two, three and four words by counting them before
#       it looks at them, and the two orders disagree: "! = x" is three words
#       with a binary operator in the middle, so it compares "!" against "x"
#       rather than negating anything.
#
group forms
answer 'one word'        'test x; echo $?'
answer 'one empty'       'test ""; echo $?'
answer 'two negated'     'test ! ""; echo $?; test ! x; echo $?'
answer 'two unary'       'test -f newer; echo $?'
answer 'three binary'    'test = = =; echo $?'
answer 'three bang'      'test ! = x; echo $?'
answer 'three bang unary' 'test ! -f nosuchfile; echo $?'
answer 'three parens'    'test "(" "" ")"; echo $?; test "(" x ")"; echo $?'
answer 'four bang'       'test ! x = x; echo $?'
answer 'four parens'     'test "(" ! "" ")"; echo $?'
answer 'and beyond four' 'test x -a y; echo $?; test "" -o y; echo $?'
answer 'bracket closes'  '[ x = x ]; echo $?'

group operators
answer 'newer than'      'test newer -nt older; echo $?'
answer 'older than'      'test older -ot newer; echo $?'
answer 'newer missing'   'test newer -nt nosuchfile; echo $?'
answer 'same file'       'test newer -ef link; echo $?'
answer 'not same file'   'test newer -ef same; echo $?'
answer 'same across dev' 'test /dev/null -ef /dev/null; echo $?'
answer 'string before'   'test a "<" b; echo $?; test b "<" a; echo $?'
answer 'string after'    'test b ">" a; echo $?; test a ">" b; echo $?'
answer 'owned by me'     'test -O newer; echo $?; test -O /etc/hostname; echo $?'
answer 'group of mine'   'test -G newer; echo $?'
answer 'set user id'     'test -u setuid; echo $?; test -u newer; echo $?'
answer 'set group id'    'test -g setgid; echo $?; test -g newer; echo $?'
answer 'sticky'          'test -k dir; echo $?; test -k newer; echo $?'
answer 'character'       'test -c /dev/null; echo $?; test -b /dev/null; echo $?'
answer 'a pipe'          'test -p fifo; echo $?; test -p newer; echo $?'
answer 'a socket'        'test -S newer; echo $?'
answer 'a terminal'      'test -t 0; echo $?; test -t 9; echo $?'
answer 'a link'          'test -h soft; echo $?; test -L soft; echo $?; test -f soft; echo $?'
answer 'readable'        'test -r newer; echo $?; test -r none; echo $?'
answer 'numeric on text' 'test 1 -eq a; echo $?'

#
#       The two most subtle: -nt against a file written in the same second,
#       which needs the nanoseconds, and -ef across two devices, where the
#       inode number alone is not an answer.
#
group precision
answer 'same second'     'touch a1; touch a2; test a2 -nt a1; echo $?'
answer 'same second back' 'touch b1; touch b2; test b1 -nt b2; echo $?'
answer 'device counts'   'test /dev/null -ef /proc/self/environ; echo $?'

section printf

group escapes
answer 'in an argument'  "printf '%b|' 'a\\tb'; echo"
answer 'stops the lot'   "printf '%b|second|' 'a\\cb'; echo done"
answer 'not in a format' "printf 'a\\cb'; echo done"
answer 'octal with zero' "printf '%b\\n' 'x\\0101y'"
answer 'octal without'   "printf '%b\\n' 'x\\101y'"
answer 'octal cap zero'  "printf '%b\\n' 'x\\01012y'"
answer 'octal cap bare'  "printf '%b\\n' 'x\\1012y'"
answer 'octal breaker'   "printf '%b\\n' 'x\\1qy'"

group width
answer 'star'            "printf '[%*d]' 5 42; echo"
answer 'star negative'   "printf '[%*d]' -5 42; echo"
answer 'star precision'  "printf '[%.*s]' 2 abcdef; echo"
answer 'around escapes'  "printf '[%5b]' ab; echo"
answer 'precision cuts'  "printf '[%.2b]' abcd; echo"
answer 'precision zero string' "printf '[%.0s]' x; echo"
answer 'precision zero escapes' "printf '[%5.0b]' 'a\\tb'; echo"
answer 'precision zero held' "printf '[%.4b][%.0b]' WXYZ q; echo"
answer 'long measured escapes' 'v=$(awk '\''BEGIN { for (i = 0; i < 5000; i++) printf "x" }'\''); printf "%.6000b" "$v" | wc -c'

group status
answer 'not a number'    "printf '%d\\n' abc; echo \$?"
answer 'bad star width'  "printf '[%*s]\\n' nope x; echo \$?"
answer 'bad star precision' "printf '[%.*s]\\n' nope abc; echo \$?"
answer 'no such letter'  "printf '%y\\n' x; echo \$?"
answer 'reuses format'   "printf '%s-' a b c; echo; echo \$?"
answer 'runs out'        "printf '%s-%s|' a; echo; echo \$?"

section read

group splitting
answer 'on IFS'          'IFS=: ; printf "a:b:c\n" | { read x y; echo "$x-$y"; }'
answer 'one name'        'IFS=: ; printf "a:b\n" | { read x; echo "[$x]"; }'
answer 'empty fields'    'IFS=: ; printf ":a::b:\n" | { read x y z; echo "[$x][$y][$z]"; }'
answer 'blank and not'   'IFS=": " ; printf "a : b  c\n" | { read x y; echo "[$x][$y]"; }'
answer 'blanks at ends'  'printf "   a b   \n" | { read x; echo "[$x]"; }'
answer 'last takes rest' 'printf "a b c\n" | { read x y; echo "[$x][$y]"; }'
answer 'nothing to split' 'IFS= ; printf "  a b  \n" | { read x; echo "[$x]"; }'

group backslash
answer 'hides a blank'   'printf "a\\\\ b c\n" | { read x y; echo "[$x][$y]"; }'
answer 'hides a colon'   'IFS=: ; printf "a\\\\:b:c\n" | { read x y; echo "[$x][$y]"; }'
answer 'joins two lines' 'printf "a\\\\\nb\n" | { read x; echo "[$x]"; }'
answer 'kept by minus r' 'printf "a\\\\:b\n" | { read -r x; echo "[$x]"; } | od -An -c | head -1'

group ending
answer 'no newline'      'printf "abc" | { read x; echo "$?/[$x]"; }'
answer 'nothing at all'  ': | { read x; echo "$?/[$x]"; }'
answer 'a whole line'    'printf "abc\n" | { read x; echo "$?/[$x]"; }'

#       The command reader has no line ceiling, and neither may read itself.
#       The old 4096-byte array returned success at its edge and left the rest
#       of the same physical line for the next call, making one record into
#       three plausible-looking records. IFS had an independent 128-byte copy
#       ceiling which could silently discard a delimiter near its end.
group growth
answer 'ten kilobyte line once' 'awk '\''BEGIN { for (i = 0; i < 10000; i++) printf "x"; print "" }'\'' | { IFS= read -r x; a=$?; IFS= read -r y; b=$?; echo "$a ${#x} $b ${#y}"; }'
answer 'long IFS keeps its end' 'IFS=$(awk '\''BEGIN { for (i = 0; i < 200; i++) printf "x"; print ":" }'\''); printf "a:b\n" | { read x y; echo "[$x][$y]"; }'

group errors
answer 'unknown option'  'printf "x\n" | { read -Z v 2>/dev/null; echo "[$v] $?"; }'
answer 'invalid name'    'printf "x\n" | { read 1bad 2>/dev/null; echo $?; }'
answer 'readonly name'   'readonly v=old; printf "new\n" | { read v 2>/dev/null; echo "[$v] $?"; }'

#       -n, -d, -p and -t are not options dash has, so what they are measured
#       against is what they are for. The wait is per byte rather than per
#       line: a writer that stops halfway through one leaves a short field
#       behind, which is the same thing every shell with a -t does.
group ours
written 'a count'        '[abc]|' 0 'printf "abcdef\n" | { read -n 3 x; echo "[$x]"; }'
written 'a delimiter'    '[a]|' 0 'printf "a:b\n" | { read -d : x; echo "[$x]"; }'
written 'a prompt'       '[v]|' 0 'printf "v\n" | { read -p "say: " x; echo "[$x]"; }'
written 'a wait that ends' '1|' 0 '{ sleep 2; echo x; } | { read -t 1 y; echo $?; }'
written 'a wait in time'  '0|[x]|' 0 'echo x | { read -t 5 y; echo $?; echo "[$y]"; }'
written 'bad count rejected' '[] 1|' 0 'printf "x\n" | { read -n nope v 2>/dev/null; echo "[$v] $?"; }'
written 'missing count rejected' '[] 2|' 0 'printf "x\n" | { read -n 2>/dev/null; echo "[${REPLY-}] $?"; }'

section getopts

group walking
answer 'one at a time'   'set -- -a -b; while getopts ab o; do echo "$o"; done; echo $OPTIND'
answer 'bundled'         'set -- -ab c; getopts ab o; echo "$o $OPTIND"; getopts ab o; echo "$o $OPTIND"; getopts ab o; echo "$? $o $OPTIND"'
answer 'argument joined' 'set -- -aval b; getopts a: o; echo "$o $OPTARG $OPTIND"'
answer 'argument apart'  'set -- -a val b; getopts a: o; echo "$o $OPTARG $OPTIND"'
answer 'two dashes end'  'set -- -- -a; getopts a o; echo "$? $o $OPTIND"'
answer 'nothing left'    'set -- x; getopts ab o; echo "$? [$o] [${OPTARG-unset}] $OPTIND"'
answer 'its own words'   'getopts ab o -a -b; echo "$o $OPTIND"'
answer 'many own words'  'getopts ab o -a one two three four five; echo "$o $OPTIND"'
answer 'many parameters' 'set -- -a one two three four five; getopts ab o; echo "$o $OPTIND"'
answer 'no argument set' 'set -- -a; getopts a o; echo "[$o] [${OPTARG-unset}]"'

group complaining
answer 'unknown loud'    'set -- -z; getopts ab o 2>/dev/null; echo "[$o] [${OPTARG-unset}]"'
answer 'unknown quiet'   'set -- -z; getopts :ab o; echo "[$o] [$OPTARG]"'
answer 'missing loud'    'set -- -a; getopts a: o 2>/dev/null; echo "[$o] [${OPTARG-unset}]"'
answer 'missing quiet'   'set -- -a; getopts :a: o; echo "[$o] [$OPTARG] $OPTIND"'
answer 'a colon typed'   'set -- -:; getopts ":a" o; echo "[$o] [$OPTARG]"'
answer 'unknown in bundle' 'set -- -az; getopts a o 2>/dev/null; echo "$o $OPTIND"; getopts a o 2>/dev/null; echo "$o $OPTIND"'

#       POSIX says putting OPTIND back to one starts a new set of arguments.
#       dash keeps its own count and does not, so the reference here is the
#       standard.
group ours
written 'OPTIND put back' 'a|' 0 'set -- -a -b; getopts ab o; OPTIND=1; getopts ab o; echo "$o"'
written 'OPTIND starts at one' '1|' 0 'echo $OPTIND'

section cd

group where
answer 'absolute'        'cd /tmp; pwd'
answer 'relative'        'mkdir -p sub; cd sub; pwd | sed "s|.*/||"'
answer 'PWD follows'     'cd /tmp; echo $PWD'
answer 'OLDPWD follows'  'cd /tmp; cd /; echo $OLDPWD'
answer 'back again'      'cd /tmp; cd / > /dev/null; cd - > /dev/null; pwd'
answer 'back is said'    'cd /tmp; cd /usr > /dev/null; cd -'
answer 'no such place'   'cd /nosuchdir12345 2>/dev/null; echo $?; pwd'
answer 'no arguments'    'HOME=/tmp; cd; pwd'

group links
answer 'logical keeps it' 'mkdir -p real; ln -s real soft2; cd -L soft2; pwd | sed "s|.*/||"'
answer 'physical drops it' 'mkdir -p real; ln -s real soft3; cd -P soft3; pwd | sed "s|.*/||"'
answer 'dots go back'    'mkdir -p real; ln -s real soft4; cd soft4; cd ..; pwd | sed "s|.*/||"'
answer 'pwd physical'    'mkdir -p real; ln -s real soft5; cd soft5; pwd -P | sed "s|.*/||"'

group cdpath
answer 'found along it'  'mkdir -p one/two; CDPATH=$PWD/one; cd two > /dev/null; pwd | sed "s|.*/||"'
answer 'said out loud'   'mkdir -p one/two; CDPATH=$PWD/one; cd two | sed "s|.*/||"'
answer 'not for dots'    'mkdir -p one/two two; CDPATH=$PWD/one; cd ./two > /dev/null; pwd | sed "s|.*/one/two$|WRONG|"'

section names

group export
answer 'printed back'    'export FOO=1; export -p | grep "^export FOO="'
answer 'quotes the value' "export FOO='a b'; export -p | grep '^export FOO='"
answer 'a name alone'    'FOO=1; export FOO; echo $?; export -p | grep "^export FOO="'
answer 'no arguments'    'export FOO=1; export | grep "^export FOO="'

group readonly
answer 'printed back'    'readonly R=1; readonly -p | grep "^readonly R="'
answer 'quoted too'      "readonly R='a b'; readonly -p | grep '^readonly R='"

group command
answer 'v is the path'   'command -v echo'
answer 'V is a sentence' 'command -V echo'
answer 'V finds a file'  'PATH=/usr/bin; command -V sh'
answer 'V says nothing found' 'command -V nosuch12345; echo $?'
answer 'options compacted' 'command -p -- printf "[%s]\\n" compact'

group type
answer 'several names'   'type echo true'

section set

group options
answer 'listed'          'set -o'
answer 'listed as set'   'set +o'
answer 'a long name'     'set -o nounset; set +o | grep nounset'
answer 'turned off'      'set -o nounset; set +o nounset; set +o | grep nounset'
answer 'letter and name' 'set -e; set +o | grep errexit'

section times

#       Both shells run in no time at all, so what is compared is the shape:
#       minutes, a point and six places, which is what a %f with nothing said
#       about it writes.
group format
answer 'four of them'    'times | sed "s/[0-9]/N/g"'

section umask

group reading
answer 'four digits'     'umask 022; umask'
answer 'spoken'          'umask 022; umask -S'
answer 'spoken none'     'umask 0; umask -S'
answer 'spoken all'      'umask 0777; umask -S'

group setting
answer 'equals'          'umask 0; umask u=rwx,g=rx,o=; umask'
answer 'minus'           'umask 0; umask a-w; umask'
answer 'plus and equals' 'umask 0; umask u+r,go=; umask'
answer 'no who'          'umask 0; umask =rx; umask'
answer 'from spoken'     'umask -S u=rwx,g=rx,o=rx; umask'
answer 'not a mode'      'umask zzz 2>/dev/null; echo $?; umask'
answer 'bad octal tail'  'umask 022; umask 078 2>/dev/null; echo $?; umask'

section hash

#       dash keeps its table in an order of its own and prints it in that
#       order, so anything with more than one name in it is comparing hash
#       functions. One name at a time is the same in both.
group table
answer 'nothing yet'     'hash; echo $?'
answer 'a name asked'    'PATH=/usr/bin; hash ls; echo $?; hash'
answer 'forgotten'       'PATH=/usr/bin; hash ls; hash -r; hash; echo $?'
answer 'no such name'    'hash nosuchcommand12345 2>/dev/null; echo $?'
answer 'PATH clears it'  'PATH=/usr/bin; hash ls; PATH=/bin:/usr/bin; hash; echo $?'

#       That the table is an answer and not a hint: the program it names is
#       taken away, and the name still resolves to where it was found.
answer 'used again'      'mkdir -p bin; : > bin/zz1; chmod +x bin/zz1; PATH=$PWD/bin:/usr/bin; hash zz1; /usr/bin/rm bin/zz1; command -v zz1; echo $?'

section ulimit

group reading
answer 'bare is the file size' 'ulimit'
answer 'open files'      'ulimit -n'
answer 'every one'       'ulimit -a'
answer 'hard and soft'   'ulimit -Hn; ulimit -Sn'
answer 'the others'      'ulimit -t; ulimit -s; ulimit -c; ulimit -d; ulimit -v; ulimit -w; ulimit -r; ulimit -p; ulimit -m; ulimit -l'

group setting
answer 'both at once'    'ulimit -n 100; ulimit -n; ulimit -Hn'
answer 'soft alone'      'ulimit -Sn 50; ulimit -Sn; ulimit -Hn'
answer 'file blocks'     'ulimit -f 100; ulimit -f; ulimit -Hf'
answer 'unlimited'       'ulimit -c unlimited; ulimit -c'
answer 'cannot go back'  'ulimit -n 100; ulimit -n 200 2>/dev/null; echo $?; ulimit -n'
answer 'no such letter'  'ulimit -Z 2>/dev/null; echo $?'

section ""

total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
