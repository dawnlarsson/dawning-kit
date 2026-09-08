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

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"
. "$test_dir/shell_compare.sh"

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
answer 'binary exact'    'test 1 -ltx 2; echo $?; test 1 -l 2; echo $?'
answer 'a terminal'      'test -t 0; echo $?; test -t 9; echo $?'
answer 'a link'          'test -h soft; echo $?; test -L soft; echo $?; test -f soft; echo $?'
answer 'readable'        'test -r newer; echo $?; test -r none; echo $?'
answer 'numeric on text' 'test 1 -eq a; echo $?'

#
#       The two most subtle: -nt against a file written in the same second,
#       which needs the nanoseconds, and -ef across two devices, where the
#       inode number alone is not an answer.
#
#       Fixed timestamps are essential here.  Running two `touch` processes
#       for each interpreter made the expected and actual pairs independently
#       land on one filesystem tick: under QEMU either side could then say the
#       files were equal while the other saw an order.  One nanosecond within
#       one known second is the exact distinction this test means to exercise.
#
group precision

BUILTIN_WORK=$work python3 - <<'PYTHON'
import os

root = os.environ['BUILTIN_WORK']
base = 1_000_000_000_100_000_000

for name, stamp in [('a1', base), ('a2', base + 1),
                    ('b1', base), ('b2', base + 1)]:
    path = os.path.join(root, name)
    open(path, 'ab').close()
    os.utime(path, ns=(stamp, stamp))
PYTHON

answer 'same second'     'test a2 -nt a1; echo $?'
answer 'same second back' 'test b1 -nt b2; echo $?'
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

#       The floating conversions, through the same field writer awk prints
#       its numbers with. The reference reads them with strtod, so a leading
#       0x is sixteen and "inf" is itself, and the exact digits and the
#       half-to-even rounding are the formatter's rather than a second
#       spelling of them here. Hexadecimal output also reuses that writer,
#       including precision rounding and signed-zero handling.
group floating
answer 'fixed'           "printf '%f|' 1.5; echo"
answer 'fixed precision' "printf '%.2f|%.0f|' 3.14159 2.5; echo"
answer 'fixed rounding'  "printf '%.0f %.0f %.1f\\n' 2.5 3.5 0.05"
answer 'fixed width'     "printf '[%10.3f][%-10.2f][%010.2f]' 2.5 1.5 1.5; echo"
answer 'signs'           "printf '[%+f][% f][%+.1f]' 1.5 1.5 -1.5; echo"
answer 'scientific'      "printf '%e %E\\n' 1234.5 0.00025"
answer 'general'         "printf '%g %G %g\\n' 0.0001 1e20 100000"
answer 'alternate keeps' "printf '[%#.0f][%#g]' 2 1.0; echo"
answer 'not finite'      "printf '%f %f %f\\n' inf -inf nan"
answer 'hexadecimal in'  "printf '%f\\n' 0x10"
answer 'exponent in'     "printf '%f %f\\n' 1e3 1.5e-3"
answer 'empty is zero'   "printf '%f|' ''; echo"
answer 'missing is zero' "printf '%f %f|' 1.5; echo"
answer 'quote is a byte' "printf '%f\\n' \"'a\""
answer 'blanks in front' "printf '%f\\n' ' 1.5'; echo \$?"
answer 'not a number'    "printf '%f\\n' nope 2>/dev/null; echo \$?"
answer 'not completely'  "printf '%f\\n' 1.5x 2>/dev/null; echo \$?"
answer 'wide precision'  "printf '%.40f\\n' 0.1"
answer 'zero and sign'   "printf '%f %f\\n' 0 -0.0"
answer 'hexadecimal out' "printf '%a %A\\n' 8 1.5"
answer 'hexadecimal rounding' "printf '%.0a %.1a %.0a\\n' 1.5 1.96875 2.5"
answer 'hexadecimal padding' "printf '[%+016a][%-16A][%#.0a]\\n' 8 1.5 -0"
answer 'hexadecimal tiny' "printf '%a\\n' 0x1p-1074"
answer 'hexadecimal special' "printf '%a %A %a\\n' inf -inf nan"

#       The grammar an integer conversion reads: strtoimax's, with a quote
#       meaning the byte after it. Nothing is zero and no complaint; a tail
#       the digits did not use is a complaint after the number, and no digits
#       at all is zero with the other complaint.
group grammar
answer 'empty is zero'   "printf '%d|' ''; echo"
answer 'missing is zero' "printf '%d %d|' 1; echo \$?"
answer 'quote is a byte' "printf '%d %d\\n' \"'a\" '\"b'"
answer 'hexadecimal'     "printf '%d %d\\n' 0x10 -0X1f"
answer 'octal'           "printf '%d %u\\n' 010 0"
answer 'blanks in front' "printf '%d\\n' ' 12'; echo \$?"
answer 'not completely'  "printf '%d\\n' 12abc; echo \$?"
answer 'unsigned forms'  "printf '%u %o %x %X\\n' 42 8 255 255"
answer 'alternate forms' "printf '%#x %#X %#o %#x %#o\\n' 255 255 8 0 0"
answer 'alternate width' "printf '[%#6x][%-#6x][%#06x]\\n' 255 255 255"
answer 'star grammar'    "printf '[%*d]' 0x3 7; echo"

#       A \\c ends the output, and not before what stood in front of it.
group cut
answer 'cut in a field'  "printf '[%5b]' 'ab\\c'; echo"
answer 'cut then more'   "printf '[%3b][%s]' 'a\\cz' next; echo done"

#       \\0 opens an octal escape only in an argument; in the format it is
#       the first of the digits, as the reference shell reads it.
group format-octal
answer 'format zero'     "printf 'x\\0101y' | od -An -c | tr -s ' '"
answer 'argument zero'   "printf '%b' 'x\\0101y'; echo"

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
written 'readonly assignment order' '2 [one][old][three][four]|' 0 'printf "one two\nthree four\n" | { readonly b=old; read a b 2>/dev/null; s=$?; read c d; printf "%s [%s][%s][%s][%s]\n" "$s" "$a" "$b" "$c" "$d"; }'
written 'read error is not EOF' '2 []|' 0 'read v <&- 2>/dev/null; printf "%s [%s]\n" "$?" "$v"'

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
written 'no argument unsets OPTARG' 'a [unset]|' 0 'OPTARG=old; set -- -a; getopts a o; echo "$o [${OPTARG-unset}]"'
written 'end unsets OPTARG' '1 [unset]|' 0 'OPTARG=old; set --; getopts a o; echo "$? [${OPTARG-unset}]"'

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
written 'readonly name fails' '2 [old]|' 0 'readonly o=old; set -- -a; getopts a o 2>/dev/null; echo "$? [$o]"'
written 'readonly OPTIND fails' '2|' 0 'readonly OPTIND; set -- -a; getopts a o 2>/dev/null; echo $?'
written 'readonly OPTARG fails' '2 [old]|' 0 'OPTARG=old; readonly OPTARG; set -- -a new; getopts a: o 2>/dev/null; echo "$? [$OPTARG]"'
written 'invalid name rejected' '2|' 0 'set -- -a; getopts a 1bad 2>/dev/null; echo $?'

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

#       An empty entry is the directory the shell is in and is not said; a
#       relative one is under that directory, so cd .. afterwards still
#       goes where it should. What cd says is read back from where it was
#       written, since the shell has moved on by then.
answer 'empty entry first' 'mkdir -p one/two two; CDPATH=:$PWD/one; cd two; echo "${PWD#"$OLDPWD"}"'
answer 'empty entry last' 'mkdir -p two; CDPATH=$PWD/nowhere:; cd two; echo "${PWD#"$OLDPWD"}"'
answer 'double colon'    'mkdir -p one/two two; CDPATH=$PWD/nowhere::$PWD/one; cd two; echo "${PWD#"$OLDPWD"}"'
answer 'relative entry'  'mkdir -p one/two; CDPATH=one; cd two > said; echo "${PWD#"$OLDPWD"}"; read said < "$OLDPWD/said"; echo "${said#"$OLDPWD"}"'
answer 'relative keeps dots' 'mkdir -p one/two; CDPATH=one; cd two > /dev/null; cd ..; pwd | sed "s|.*/||"'
answer 'trailing slash entry' 'mkdir -p one/two; CDPATH=$PWD/one/; cd two > said; echo "${PWD#"$OLDPWD"}"; read said < "$OLDPWD/said"; echo "${said#"$OLDPWD"}"'
answer 'later entry only' 'mkdir -p one/two; CDPATH=$PWD/nowhere:$PWD/one; cd two > said; echo "${PWD#"$OLDPWD"}"; read said < "$OLDPWD/said"; echo "${said#"$OLDPWD"}"'

group issue8
written 'empty operand rejected' 'rejected same|' 0 \
        'before=$PWD; if cd "" 2>/dev/null; then echo BAD; else printf "rejected "; fi; [ "$PWD" = "$before" ] && echo same'
written 'combined e and P options' '0 /|' 0 \
        'cd -Pe /; printf "%s %s\n" "$?" "$PWD"'
written 'e reports unnamed physical directory' 'reported|' 0 \
        'base=$(mktemp -d); mkdir "$base/gone"; (cd "$base/gone" && rmdir "$base/gone" && { cd -Pe . 2>/dev/null; [ "$?" -eq 1 ]; }) && echo reported; rmdir "$base"'
written 'P alone permits unnamed directory' 'accepted|' 0 \
        'base=$(mktemp -d); mkdir "$base/gone"; (cd "$base/gone" && rmdir "$base/gone" && cd -P . 2>/dev/null) && echo accepted; rmdir "$base"'
written 'too many operands rejected' 'rejected same|' 0 \
        'before=$PWD; if cd / /tmp 2>/dev/null; then echo BAD; else printf "rejected "; fi; [ "$PWD" = "$before" ] && echo same'
answer 'readonly PWD reports update failure' \
        'cd /tmp; readonly PWD; cd / 2>/dev/null; s=$?; printf "%s:%s:" "$s" "$PWD"; /bin/pwd'
answer 'readonly OLDPWD stops PWD update' \
        'cd /tmp; readonly OLDPWD; cd / 2>/dev/null; s=$?; printf "%s:%s:" "$s" "$PWD"; /bin/pwd'

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

#       Where along PATH a bare name is: an empty field is the directory the
#       shell is in, a relative field is under it, and the executor walks on
#       past a file of that name it could not run to one it can.
group search
answer 'leading colon'   'mkdir -p bin; : > bin/zz2; chmod +x bin/zz2; cd bin; PATH=:/usr/bin; command -v zz2'
answer 'trailing colon'  'mkdir -p bin; : > bin/zz3; chmod +x bin/zz3; cd bin; PATH=/usr/bin:; command -v zz3'
answer 'double colon'    'mkdir -p bin; : > bin/zz4; chmod +x bin/zz4; cd bin; PATH=/usr/bin::/bin; command -v zz4'
answer 'trailing slash'  'mkdir -p bin; printf "#!/bin/sh\necho ran\n" > bin/zz5; chmod +x bin/zz5; PATH=$PWD/bin/:/usr/bin; zz5; type zz5 > /dev/null; echo $?'
answer 'relative field'  'mkdir -p bin; printf "#!/bin/sh\necho ran\n" > bin/zz6; chmod +x bin/zz6; PATH=bin:/usr/bin; command -v zz6; zz6'
answer 'later field only' 'mkdir -p first second; printf "#!/bin/sh\necho ran\n" > second/zz7; chmod +x second/zz7; PATH=$PWD/first:$PWD/second; command -v zz7 | /bin/sed "s|.*/second/|second/|"; zz7'
answer 'denied then found' 'mkdir -p first second; echo bad > first/zz8; chmod 600 first/zz8; printf "#!/bin/sh\necho ran\n" > second/zz8; chmod +x second/zz8; PATH=$PWD/first:$PWD/second; zz8; command -v zz8 > /dev/null; echo $?'

group type
answer 'several names'   'type echo true'

section set

group options
answer 'listed'          'set -o'
answer 'listed as set'   'set +o'
answer 'a long name'     'set -o nounset; set +o | grep nounset'
answer 'turned off'      'set -o nounset; set +o nounset; set +o | grep nounset'
answer 'letter and name' 'set -e; set +o | grep errexit'

group allexport
answer 'read assignment' 'unset READV; set -a; printf "value\n" | { read READV; /bin/sh -c '\''echo "$READV"'\''; }'
answer 'getopts assignments' 'unset OPTION OPTARG; OPTIND=1; set -a; set -- -x value; getopts x: OPTION; /bin/sh -c '\''echo "$OPTION:$OPTARG:$OPTIND"'\'''
answer 'cd assignments' 'mkdir target; unset PWD OLDPWD; set -a; cd target; /bin/sh -c '\''echo "${PWD##*/}:${OLDPWD##*/}"'\'''
answer 'readonly assignment' 'unset FIXED; set -a; readonly FIXED=value; /bin/sh -c '\''echo "$FIXED"'\'''

section declarations

group elements
bash_answer 'scalar assignments mark indexed element zero as assigned' \
        'declare -a first=target; declare -a second; second=target; f(){ local -a third=target; declare -p third; }; declare -p first second; f'
bash_answer 'adding indexed type retains a preexisting scalar element zero' \
        'first=target; declare -a first; declare -i second=7; declare -a second; declare -p first second'
bash_answer 'associative conversion moves scalar zero without duplicate storage' \
        'first=old; declare -A first; declare -p first; declare first=next; declare -p first; second=old; declare -A second=new; printf "%s:%s\n" "${second[0]}" "${#second[@]}"; declare -p second'
bash_answer 'declare indexed elements retain sparse values and append attributes' \
        'declare -ai a[2]=7; declare -ai a[2]+=3 a[0]=4; printf "%s:%s:%s\n" "${a[0]}" "${a[2]}" "${a@a}"'
bash_answer 'declare associative elements preserve key bytes and fold values' \
        'declare -Al a; declare "a[a=b]=Value" "a[a b]=Other"; declare "a[a=b]+=MORE"; printf "%s:%s:%s\n" "${a[a=b]}" "${a[a b]}" "${a@a}"'
bash_answer 'declaration subscript arithmetic runs exactly once' \
        'i=0; declare "a[i++]=first" "a[i=2]=second"; printf "%s:%s:%s:%s\n" "$i" "${a[0]}" "${a[2]}" "${#a[@]}"'
bash_answer 'bare element declaration creates type without evaluating index' \
        'i=0; declare -i "a[i++]"; printf "%s:%s:%s\n" "$i" "${#a[@]}" "${a@a}"'
bash_answer 'bare declaration allows equals inside an unevaluated subscript' \
        'i=0; declare "a[i=1]"; printf "%s:%s:%s\n" "$i" "${#a[@]}" "${a@a}"'
bash_answer 'element locals restore the whole containing array' \
        'declare -A a=([outside]=old); f(){ local -Al a[key]=Value; local a[key]+=MORE; printf "%s:%s\n" "${a[key]}" "${a@a}"; }; f; printf "%s:%s:%s\n" "${a[outside]}" "${a[key]-unset}" "${a@a}"'
bash_answer 'fresh declare locals choose their own array kind' \
        'declare -a a=([2]=outer); f(){ declare -A a[key]=inner; printf "%s:%s\n" "${a[key]}" "${a@a}"; }; f; printf "%s:%s\n" "${a[2]}" "${a@a}"'
bash_answer 'declare element converts a nameref record without writing its target' \
        'target=old; declare -n ref=target; declare -u ref[2]=Value; printf "%s:%s:%s\n" "$target" "${ref[2]}" "${ref@a}"'
bash_answer 'local element hides and restores an outer nameref' \
        'target=old; declare -n ref=target; f(){ local ref[2]=inner; printf "%s:%s\n" "$target" "${ref[2]}"; }; f; printf "%s:%s\n" "$ref" "${ref@a}"'
bash_answer 'explicit nameref element declarations are rejected' \
        'declare -n "ref[2]=target"; printf "%s:%s\n" "$?" "${ref-unset}"'
bash_answer 'readonly element declarations preserve existing contents' \
        'declare -ar a=([2]=old); declare a[2]=new; printf "%s:%s\n" "$?" "${a[2]}"'
bash_answer 'new readonly export attributes precede element storage' \
        'declare -a a; declare -rx a[2]=new; printf "%s:%s:%s\n" "$?" "${a[2]-unset}" "${a@a}"'
bash_answer 'exported element declaration marks its containing array' \
        'declare -x a[2]=new; printf "%s:%s\n" "${a[2]}" "${a@a}"'
bash_answer 'global element spelling still assigns the visible local element' \
        'declare -a a=([2]=outer); f(){ local -a a=([2]=inner); declare -g a[2]=changed; printf "%s\n" "${a[2]}"; }; f; printf "%s\n" "${a[2]}"'
bash_answer 'global element attributes mark the saved global declaration' \
        'declare -a a=([2]=outer); f(){ local -a a=([2]=inner); declare -giru a[2]=Value; printf "%s:%s\n" "${a[2]}" "${a@a}"; }; f; printf "%s:%s\n" "${a[2]}" "${a@a}"'
bash_answer 'global element kind validation uses the saved global array' \
        'declare -A a=([key]=outer); f(){ local -a a=([2]=inner); declare -gA a[key]=Value; printf "%s:%s:%s\n" "${a[0]}" "${a[2]}" "${a@a}"; }; f; printf "%s:%s\n" "${a[key]}" "${a@a}"'
bash_answer 'global element declarations retain a saved scalar as element zero' \
        'a=outer; f(){ local a=inner; declare -g a[2]=changed; printf "%s:%s\n" "${a[0]}" "${a[2]}"; }; f; declare -p a'
bash_answer 'quoted parentheses stay scalar through declare round trips' \
        'for value in "(" "(x)" "(a b)"; do declaration=$(declare -p value); unset value; eval "$declaration"; printf "<%s>\n" "$value"; done'
bash_answer 'retained function compounds keep lexical assignment identity' \
        'f(){ local a=("(x)" "("); local b="(y)"; printf "%s:%s:%s\n" "${a[0]}" "${a[1]}" "$b"; }; f; f'

group globals
bash_answer 'hidden nameref subscript writes the visible prefix without recursive publication' \
        'i=0; declare -a ar=(zero one two); declare -n y=x; f(){ declare -gn x="ar[x=1]"; declare -g x=Z; printf "status:%s inner:%s i:%s\n" "$?" "$x" "$i"; }; x=0 f; declare -p ar x y i; :'
bash_answer 'hidden scalar and element namerefs append through their target' \
        'y=old; a=(first second); f(){ declare -gn x=y; declare -g x+=Z; declare -gn x="a[1]"; declare -g x+=Q; printf "%s:%s:%s\n" "$x" "$y" "${a[1]}"; }; x=temp f; printf "%s:%s\n" "$y" "${a[1]}"'
bash_answer 'hidden nameref writes retain target readonly status' \
        'ro=old; readonly ro; f(){ declare -gn x=ro; declare -g x=Z; printf "%s:%s\n" "$?" "$x"; }; x=temp f; printf "%s\n" "$ro"'
bash_answer 'hidden absent RANDOM declarations retain dynamic state' \
        'f(){ declare -g RANDOM=7; }; RANDOM=3 f; first=$RANDOM; second=$RANDOM; test "$first" != "$second"; printf "%s\n" "$?"'
bash_answer 'global declarations update the variable below a function prefix' \
        'x=old; y=target; f(){ declare -gn x=y; printf "%s:%s\n" "$?" "$x"; declare -p x; }; x=raw f; declare -p x y'
bash_answer 'global integer declarations evaluate against the visible prefix' \
        'x=7; f(){ declare -gi x=x+1; printf "%s\n" "$x"; }; x=10 f; declare -p x; g(){ declare -gi x+="(x=4,x+1)"; printf "%s\n" "$x"; }; x=10 g; declare -p x'
bash_answer 'global prefix array attributes and scalar element writes stay separate' \
        'x=old; f(){ declare -gai x=Value; declare -p x; declare -g x[2]=Other; declare -p x; }; x=raw f; declare -p x'
bash_answer 'explicit exported global elements adopt repeated temporary prefixes' \
        'x=old; f(){ declare -gx x[2]=new; declare -p x; }; x=one x=two f; declare -p x'
bash_answer 'readonly global elements retain their adopted temporary array' \
        'x=old; f(){ declare -gr x[2]=new 2>diagnostic; printf "%s:" "$?"; test -s diagnostic; printf "%s\n" "$?"; declare -p x; }; x=temp f; declare -p x'
bash_answer 'promoted arrays keep the visible kind after global attribute changes' \
        'set -o posix; declare -A x=([key]=old); x=temp eval '\''declare -gA x[2]=new; declare -p x'\''; declare -p x'
bash_answer 'global declarations follow an ordinary nameref below a prefix' \
        'x=old; declare -n ref=x; f(){ declare -g ref=new; printf "%s:%s\n" "$ref" "$x"; }; ref=temp f; printf "%s:%s\n" "$ref" "$x"'
bash_answer 'hidden global values survive later command arena reuse' \
        'x=old; wanted=$(printf "%0192d" 1); f(){ declare -g x="$wanted"; for i in 1 2 3 4 5 6 7 8; do scratch=$(printf "%04096d" 1) :; done; }; x=temp f; test "$x" = "$wanted"; printf "%s\n" "$?"'
bash_answer 'failed global nameref declarations restore the visible prefix' \
        'x=old; f(){ declare -gn x=bad-name; printf "%s:%s\n" "$?" "$x"; }; x=raw f; declare -p x'

group namerefs
bash_answer 'explicit nameref binding clears an inherited integer attribute' \
        'target=Value; declare -i ref=7; declare -n ref=target; printf "%s:%s\n" "$?" "$ref"; declare -p ref'
bash_answer 'implicit nameref conversion validates old bytes before attributes change' \
        'declare -i ref=7; declare -n ref; printf "%s\n" "$?"; declare -p ref'
bash_answer 'integer nameref declaration without a value does not evaluate' \
        'ref=old; declare -in ref; printf "%s\n" "$?"; declare -p ref; declare -in absent; declare -p absent'
bash_answer 'invalid integer nameref binding restores prior reference presence' \
        'value=7; declare -in absent=value; printf "%s\n" "$?"; declare -p absent; ref=old; declare -in ref=value; printf "%s\n" "$?"; declare -p ref; declare -n bound=value; declare -in bound=value; printf "%s\n" "$?"; declare -p bound'
bash_answer 'array records reject nameref conversion before arithmetic evaluation' \
        'side=0; value="(side=3,7)"; declare -a ref=([2]=old); declare -in ref=value; printf "%s:%s:%s\n" "$?" "$side" "${ref[2]}"; declare -p ref'
bash_answer 'fresh declare locals may hide an outer array with a nameref' \
        'target=Value; declare -a ref=([2]=outer); f(){ declare -n ref=target; printf "%s:%s\n" "$?" "$ref"; }; f; printf "%s:%s\n" "${ref[2]}" "${ref@a}"'
bash_answer 'malformed nameref targets do not change existing attributes' \
        'ref=old; declare -iln ref=bad-name; printf "%s\n" "$?"; declare -p ref; declare -i number=7; declare -n number=bad-name; printf "%s\n" "$?"; declare -p number'
bash_answer 'integer nameref validation preserves original RHS assignment targets' \
        'ref=old; value="(ref[2]=3,7)"; declare -in ref=value; printf "%s:%s\n" "$?" "${old-unset}"; declare -p ref; scalar=old; value="(scalar=3,7)"; declare -in scalar=value; printf "%s:%s\n" "$?" "${old-unset}"; declare -p scalar'
bash_answer 'integer nameref append evaluates before rejecting the binding' \
        'ref=old; value="(side[2]=3,7)"; declare -in ref+=value; printf "%s:%s\n" "$?" "${side[2]}"; declare -p ref; other=old; declare -in other+="(changed=3,7)"; printf "%s:%s\n" "$?" "$changed"; declare -p other'
bash_answer 'integer nameref append diagnoses the original operand after evaluation' \
        'ref=old; value="(side=3,7)"; declare -in ref+=value 2>diagnostic; printf "%s:%s:" "$?" "$side"; test -s diagnostic; printf "%s\n" "$?"; other=old; declare -in other+="(changed=3,7)" 2>diagnostic; printf "%s:%s:" "$?" "$changed"; test -s diagnostic; printf "%s\n" "$?"'
bash_answer 'ordinary declaration writes through readonly namerefs report refusal' \
        'readonly target=old; declare -n ref=target; declare ref=new; printf "%s:%s\n" "$?" "$target"; declare ref+=new; printf "%s:%s\n" "$?" "$target"'

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
answer 'several actions' 'umask 0777; umask u+w+r; umask'
answer 'last equals wins' 'umask 0777; umask u=r=w; umask'
answer 'copies a class'  'umask 0077; umask g=u,o+g; umask'
answer 'capital x'       'umask 0077; umask g+X; umask'
answer 'capital x none'  'umask 0177; umask a+X; umask'
answer 'special bit'     'umask 0077; umask g+s; umask'
answer 'no sticky'       'umask 022; umask g+t 2>/dev/null; echo $?; umask'
answer 'trailing comma'  'umask 0777; umask u+r,; umask'
answer 'comma alone'     'umask 022; umask , 2>/dev/null; echo $?; umask'

section hash

#       dash keeps its table in an order of its own and prints it in that
#       order, so anything with more than one name in it is comparing hash
#       functions. One name at a time is the same in both.
#       trap takes a signal by any spelling kill does: either case, with or
#       without SIG, the three Linux aliases, and numbers no further than 64.
group signals
answer 'lower case name' 'trap : int; echo $?; trap : Int; echo $?; trap : hUp; echo $?'
answer 'linux io name'   'trap "echo io" IO; kill -IO $$; echo $?'
answer 'past the last'   'trap : 64; echo $?; trap : 65 2>/dev/null; echo $?'
answer 'real-time number' 'trap "echo rt" 40; kill -40 $$; echo $?'

#       set with no words is every variable as a line the shell could be
#       fed: sorted and quoted; export -p and readonly -p sort as well.
group listing
answer 'set sorted quoted' "b='x y'; a=\"it's\"; set | grep '^[ab]='"
answer 'export sorted'   'export zz=1 aa=2; export -p | grep " [az][az]="'
answer 'readonly sorted' 'readonly zz=1 aa=2; readonly -p | grep " [az][az]="'
bash_answer 'declaration and transformation attribute order' \
        'declare -airx indexed=(7); declare -Alx assoc=([key]=Value); declare -ux upper=abc; declare -lr lower=ABC; declare -n ref=upper; declare -p indexed assoc upper lower ref; printf "<%s>\n" "${indexed@a}" "${assoc@a}" "${upper@a}" "${lower@a}" "${ref@a}"'
bash_answer 'attribute queries evaluate subscript side effects once' \
        'declare -ai indexed=([2]=7); i=2; printf "<%s>:%s\n" "${indexed[i++]@a}" "$i"; declare -n ref="indexed[i++]"; printf "<%s>:%s\n" "${ref@a}" "$i"'
bash_answer 'integer appends add expressions across storage forms' \
        'declare -i scalar=7; declare -ai indexed=([0]=7 [2]=7); declare -Ai assoc=([key]=7); scalar+=2*3; indexed[0]+=2*3; indexed[2]+=2*3; assoc[key]+=2*3; declare -p scalar indexed assoc'
bash_answer 'associative scalar append reads the zero key' \
        'declare -Ai assoc=([0]=7); assoc+=3; declare -p assoc; declare -Ai assoc+=4; declare -p assoc'
bash_answer 'blank integer append operands are zero' \
        'declare -i scalar=7; declare -ai indexed=([2]=7); declare -Ai assoc=([key]=7); scalar+=" "; indexed[2]+=" "; assoc[key]+=" "; declare -p scalar indexed assoc'
bash_answer 'case attributes shape the complete appended value' \
        'scalar=ABC; indexed=([0]=ABC [2]=ABC); declare -A assoc=([key]=abc); declare -l scalar; declare -al indexed; declare -Au assoc; scalar+=DEF; indexed[0]+=DEF; indexed[2]+=DEF; assoc[key]+=def; declare -p scalar indexed assoc'
bash_answer 'integer writes retain the outer arithmetic cursor' \
        'declare -i scalar=7; declare -ai indexed=([2]=7); declare -Ai assoc=([key]=7); printf "%s:%s:%s\n" "$((scalar=4,scalar+2))" "$((indexed[2]=4,indexed[2]+2))" "$((assoc[key]=4,assoc[key]+2))"; declare -p scalar indexed assoc'
bash_answer 'integer append snapshots operands before recursive writes' \
        'declare -i scalar=7; declare -ai indexed=([2]=7); declare -Ai assoc=([key]=7); scalar+="scalar=3"; indexed[2]+="indexed[4]=5"; assoc[key]+="assoc[other]=5"; declare -p scalar indexed assoc'
bash_answer 'invalid integer append does not overwrite before exit' \
        'declare -i scalar=7; trap "declare -p scalar" EXIT; scalar+="1/0"; echo after'
bash_answer 'invalid array integer write does not overwrite before exit' \
        'declare -Ai assoc=([key]=7); trap "declare -p assoc" EXIT; assoc[key]="1+"; echo after'
bash_answer 'readonly nested integer write propagates failure' \
        'declare -ir fixed=3; declare -i scalar=7; trap "declare -p fixed scalar" EXIT; scalar="fixed=4"; echo after'
bash_answer 'nameref and element writes share integer append semantics' \
        'declare -i scalar=7; declare -ai indexed=([2]=7); declare -Ai assoc=([key]=7); declare -n s=scalar i=indexed[2] a=assoc[key]; s+=3; i+=3; a+=3; declare -p scalar indexed assoc'
bash_answer 'unset prefix scope basic' \
        'x=old; f(){ unset x; printf "body:%s\n" "${x-unset}"; }; x=temp f; printf "end:%s\n" "${x-unset}"'
bash_answer 'unset prefix scope writeafter' \
        'x=old; f(){ unset x; x=new; printf "body:%s\n" "$x"; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope globalbefore' \
        'x=old; f(){ declare -g x=new; unset x; printf "body:%s\n" "$x"; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope repeated' \
        'x=old; f(){ unset x; printf "body:%s\n" "${x-unset}"; }; x=first x=second f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope doubleunset' \
        'x=old; f(){ unset x x; printf "body:%s\n" "${x-unset}"; }; x=temp f; printf "end:%s\n" "${x-unset}"'
bash_answer 'unset prefix scope localshadow' \
        'x=old; f(){ local x=local; unset x; printf "body:%s\n" "${x-unset}"; x=localnew; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope nestedfunc' \
        'x=old; g(){ unset x; printf "g:%s\n" "${x-unset}"; x=new; }; f(){ g; printf "f:%s\n" "$x"; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope nestedprefix' \
        'x=old; g(){ unset x; printf "g:%s\n" "${x-unset}"; x=new; }; f(){ x=inner g; printf "f:%s\n" "$x"; }; x=outer f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope nestedlocal' \
        'x=old; g(){ unset x; printf "g:%s\n" "${x-unset}"; x=new; }; f(){ local x=local; g; printf "f:%s\n" "$x"; }; x=outer f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope evalplain' \
        'x=old; x=temp eval '"'"'unset x; printf "body:%s\n" "${x-unset}"; x=new'"'"'; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope evalposix' \
        'set -o posix; x=old; x=temp eval '"'"'unset x; printf "body:%s\n" "${x-unset}"; x=new'"'"'; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope scalarnameref' \
        'x=old; declare -n n=x; f(){ unset n; printf "body:%s:%s\n" "$x" "$n"; n=new; }; n=temp f; printf "end:%s:%s\n" "$x" "$n"'
bash_answer 'unset prefix scope elementnameref' \
        'a=(zero one); declare -n n="a[1]"; f(){ unset n; printf "body:%s:%s\n" "${a[1]}" "$n"; n=new; }; n=temp f; printf "end:%s:%s\n" "${a[1]}" "$n"'
bash_answer 'unset prefix scope elementzero' \
        'x=old; f(){ unset "x[0]"; printf "body:%s\n" "${x-unset}"; x=new; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope arrayoriginal' \
        'a=(zero one); f(){ unset a; printf "body:%s:%s\n" "$a" "${a[1]}"; a=new; }; a=temp f; printf "end:%s:%s\n" "$a" "${a[1]}"'
bash_answer 'unset prefix scope localprefix' \
        'x=old; f(){ local x=local; g(){ unset x; printf "g:%s\n" "$x"; x=new; }; x=temp g; printf "f:%s\n" "$x"; }; f; printf "end:%s\n" "$x"'
bash_answer 'unset prefix scope posixfunc' \
        'set -o posix; x=old; f(){ unset x; printf "body:%s\n" "${x-unset}"; x=new; }; x=temp f; printf "end:%s\n" "$x"'
bash_answer 'all nonzero bytes survive reusable quoting' \
        'LC_ALL=C; for ((i=1; i<256; i++)); do printf -v oct "%03o" "$i"; printf -v value "%b" "\\$oct"; eval "round=${value@Q}"; [[ $round == "$value" ]] || exit 1; printf -v quoted "%q" "$value"; eval "round=$quoted"; [[ $round == "$value" ]] || exit 2; declaration=$(declare -p value); unset value; eval "$declaration"; [[ $round == "$value" ]] || exit 3; done; echo "$i"'
bash_answer 'sorted function names bodies and attributes' \
        'zz() { echo zz; }; aa() { echo aa; }; bodies=$(declare -f); (unset -f aa zz; eval "$bodies"; aa; zz); readonly -f zz; export -f aa; declare -F; readonly -fp | tr -d " \t\n"; echo; export -fp | tr -d " \t\n"; echo; compgen -A function'

#       A special builtin that fails ends a script: set with a bad option
#       name, and . with a file it cannot open.
group fatal
answer 'set option unknown' 'set -o nosuch 2>/dev/null; echo after'
answer 'dot missing status' 'sh -c ". /nonexistent 2>/dev/null; echo after"; echo $?'

#       echo takes one -n, and the reference shell prints the second.
group echo
answer 'echo -n twice'   'echo -n -n x; echo'

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
