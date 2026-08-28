#!/bin/sh
#
#       dd, diff and ps against the ones already on the machine.
#
#           sh src/test/tools.sh [directory of the names our shell answers to]
#
#       Every case runs the same input through the system's dd, diff or ps
#       and through ours, and compares. Agreeing with the reference is what
#       passing means; there is no separate idea here of the right answer.
#
#       What each of the three compares, and why it is not the same thing:
#
#       dd     writes its summary to the error stream, not to standard
#              output, so the comparison has to capture both. The default
#              summary ends in a duration and a rate that are what this
#              machine did in that second and cannot agree between two runs,
#              so there are two shapes of case: status=noxfer, which is byte
#              for byte including both records lines, and the default, with
#              everything from " copied," onwards cut off so that the byte
#              count and its human readable forms are still compared.
#
#       diff   is compared whole, standard output and exit status, because
#              all of it is deterministic -- including the timestamps in a
#              -u header, since both tools read the mtime of the same file
#              and TZ is fixed below. The generated section is the one that
#              matters: a minimal edit script is not unique, and which of
#              several identical lines gets called the changed one is a
#              choice GNU makes in three places -- the identical head and
#              tail it trims first, the lines it discards before the matcher
#              runs, and the pass that slides a run of changes as far forward
#              as it will go. Hand written cases do not reach any of them.
#              Random pairs over a five letter alphabet reach all three,
#              because that is what makes duplicate lines everywhere.
#
#       ps     reads a machine that is not the same machine a second later,
#              so nothing about its content can be compared. What is compared
#              is the shape: the header line, which is fixed; the number of
#              fields a row has; and that a process known to exist is in the
#              listing. A case that says only "it printed something" would
#              pass for a tool that printed anything at all, so each one
#              below says what it is looking at.
#
#       LC_ALL=C for the same reason every other lane sets it, and TZ so that
#       the reference and ours agree about what +0000 means.
#
#       What is not here, written down so that the absence is a decision and
#       not an oversight:
#
#         * TZ. Our diff prints a -u timestamp in UTC, always: the tree has
#           no reader for /usr/share/zoneinfo and nothing else in it wants
#           one. Pinning TZ=UTC0 above makes the reference agree, which also
#           means no case here covers a machine whose clock is not UTC.
#
#         * The formats we do not have. Context (-c), -U with a count of its
#           own, -I, -Z, --from-file, and the obsolete -NUM spelling of the
#           context count are not implemented and are not compared. diff -Z9
#           is a real GNU invocation and ours refuses it.
#
#         * Names with a space or a quote in them. GNU quotes a name in a
#           header and in an "Only in" line; ours prints the bytes. Nothing
#           below uses such a name.
#
#         * Where the binary check looks. GNU decides a file is binary from
#           the first buffer it read; ours looks at the whole file, so a
#           large file whose first zero byte is a long way in is binary to us
#           and text to GNU.
#
#         * A read that comes back short from a pipe that is still being
#           written. The one case below that reaches it uses a fifo with a
#           pause in it, and only to check that a signal does not shorten a
#           read. Two runs of a racing writer do not agree with each other,
#           so there is no comparison against the reference to be had.
#
#         * ps content. RSS here is the page count out of /proc/PID/stat
#           rather than the field procps reads, STIME is UTC, and C is a
#           simpler ratio than the reference computes. The ps cases compare
#           shape for that reason as well as the obvious one.

LC_ALL=C
TZ=UTC0
export LC_ALL TZ

bin=${1:-/tmp/mwfarm}
rounds=${2:-300}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0
group=""

case_start()
{
        group=$1
}

report()
{
        fail=$((fail + 1))
        printf '  %-8s %-34s %s\n' "$group" "$1" "$2"
}

show()
{
        head -c 40 "$1" | tr '\n\t' '|>'
}

#       Standard output and exit status, for the tools whose answer is on
#       standard output.
compare()
{
        name=$1
        tool=$2
        shift 2

        "$tool" "$@" > "$work/want" 2> /dev/null
        want_status=$?
        "$bin/$tool" "$@" > "$work/got" 2> /dev/null
        got_status=$?

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name" "want $(show "$work/want")[$want_status] got $(show "$work/got")[$got_status]"
}

#       Same, with standard input coming from a named file.
compare_in()
{
        name=$1
        tool=$2
        feed=$3
        shift 3

        "$tool" "$@" < "$feed" > "$work/want" 2> /dev/null
        want_status=$?
        "$bin/$tool" "$@" < "$feed" > "$work/got" 2> /dev/null
        got_status=$?

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name" "want $(show "$work/want")[$want_status] got $(show "$work/got")[$got_status]"
}

#       dd: standard input from a file, standard output to a file, and the
#       summary from the error stream, all three compared. The filter is what
#       the case asks to have cut out of the summary before comparing.
compare_dd()
{
        name=$1
        feed=$2
        filter=$3
        shift 3

        rm -f "$work/dd_want" "$work/dd_got"

        dd "$@" < "$feed" > "$work/dd_want" 2> "$work/want_err"
        want_status=$?
        "$bin/dd" "$@" < "$feed" > "$work/dd_got" 2> "$work/got_err"
        got_status=$?

        sed "$filter" < "$work/want_err" > "$work/want"
        sed "$filter" < "$work/got_err" > "$work/got"

        if cmp -s "$work/want" "$work/got" &&
                cmp -s "$work/dd_want" "$work/dd_got" &&
                [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name" "want $(show "$work/want")[$want_status] got $(show "$work/got")[$got_status]"
}

#       diff: the whole of standard output and the exit status, with the two
#       operands named last.
compare_diff()
{
        name=$1
        shift

        diff "$@" > "$work/want" 2> /dev/null
        want_status=$?
        "$bin/diff" "$@" > "$work/got" 2> /dev/null
        got_status=$?

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name" "want $(show "$work/want")[$want_status] got $(show "$work/got")[$got_status]"
}

check()
{
        if [ "$2" = "$3" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$1" "want [$2] got [$3]"
}

#
#       dd
#

case_start dd

dd if=/dev/urandom of="$work/blob" bs=1024 count=17 2> /dev/null
printf 'abcdefghij' > "$work/ten"
printf '' > "$work/none"
head -c 4096 /dev/zero > "$work/zeros"

#       The two records lines, byte for byte. A short read is not an error
#       and not the end of the input: with bs=4 over ten bytes the answer is
#       two whole records and one partial, and an implementation that treats
#       the short read as either of the other two gets a different number
#       here and nowhere else.
compare_dd 'records exact'    "$work/ten"  's/x/x/' bs=4 status=noxfer
compare_dd 'records short'    "$work/ten"  's/x/x/' bs=16 status=noxfer
compare_dd 'records empty'    "$work/none" 's/x/x/' bs=8 status=noxfer
compare_dd 'records aligned'  "$work/zeros" 's/x/x/' bs=512 status=noxfer
compare_dd 'count stops'      "$work/blob" 's/x/x/' bs=1024 count=3 status=noxfer
compare_dd 'count past end'   "$work/ten"  's/x/x/' bs=4 count=99 status=noxfer
compare_dd 'count zero'       "$work/blob" 's/x/x/' bs=1024 count=0 status=noxfer
compare_dd 'skip blocks'      "$work/blob" 's/x/x/' bs=1024 skip=4 status=noxfer
compare_dd 'skip past end'    "$work/ten"  's/x/x/' bs=4 skip=9 status=noxfer

#       ibs and obs apart is the other counting path: the input is read in
#       one size and the output written in another, so the records in and
#       the records out are different numbers on purpose.
compare_dd 'ibs obs apart'    "$work/blob" 's/x/x/' ibs=1000 obs=512 status=noxfer
compare_dd 'ibs obs ragged'   "$work/ten"  's/x/x/' ibs=3 obs=7 status=noxfer
compare_dd 'obs larger'       "$work/ten"  's/x/x/' ibs=2 obs=64 status=noxfer

#       conv=sync pads the short block out to the input size, which turns a
#       partial record in into a whole record out.
compare_dd 'conv sync'        "$work/ten"  's/x/x/' bs=16 conv=sync status=noxfer
compare_dd 'conv sync ragged' "$work/ten"  's/x/x/' bs=4 conv=sync status=noxfer
compare_dd 'conv fsync'       "$work/ten"  's/x/x/' bs=4 conv=fsync status=noxfer
compare_dd 'conv noerror'     "$work/blob" 's/x/x/' bs=512 conv=noerror status=noxfer

#       status=none prints nothing at all, which is a summary too.
compare_dd 'status none'      "$work/blob" 's/x/x/' bs=512 status=none

#       The default summary, with the duration and the rate cut off. What is
#       left is the byte count and the two human readable forms of it, and
#       which of those two appear depends on the count: under a thousand
#       neither, a round thousand only the decimal one, and past a kibibyte
#       both.
compare_dd 'summary small'    "$work/ten"  's/ copied,.*//' bs=4
compare_dd 'summary kilo'     "$work/zeros" 's/ copied,.*//' bs=1000 count=1
compare_dd 'summary kibi'     "$work/zeros" 's/ copied,.*//' bs=1024 count=1
compare_dd 'summary blob'     "$work/blob" 's/ copied,.*//' bs=1024
compare_dd 'summary one byte' "$work/ten"  's/ copied,.*//' bs=1 count=1
compare_dd 'summary none'     "$work/none" 's/ copied,.*//' bs=512

#       The size suffixes, where KB and KiB are two different numbers and a
#       bare K is the binary one.
for suffix in c b K KB KiB M MB; do
        compare_dd "size $suffix" "$work/blob" 's/x/x/' bs=1$suffix count=2 status=noxfer
done

compare_dd 'size product'     "$work/blob" 's/x/x/' bs=2x512 count=2 status=noxfer

#       of= and seek= write into a file rather than to standard output, so
#       what is compared is the file each one left behind.
for one in 'seek=2' 'seek=2 conv=notrunc' 'seek=0'; do
        rm -f "$work/out_want" "$work/out_got"
        head -c 3000 /dev/zero > "$work/out_want"
        head -c 3000 /dev/zero > "$work/out_got"

        # shellcheck disable=SC2086
        dd if="$work/ten" of="$work/out_want" bs=4 $one status=noxfer 2> "$work/want"
        want_status=$?
        # shellcheck disable=SC2086
        "$bin/dd" if="$work/ten" of="$work/out_got" bs=4 $one status=noxfer 2> "$work/got"
        got_status=$?

        if cmp -s "$work/want" "$work/got" &&
                cmp -s "$work/out_want" "$work/out_got" &&
                [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
        else
                report "of $one" "want $(show "$work/want")[$want_status] got $(show "$work/got")[$got_status]"
        fi
done

#       if= that is not there: the exit status and the fact that nothing was
#       written, not the wording, which quotes the name differently between
#       coreutils releases.
dd if="$work/nosuch" of=/dev/null 2> /dev/null
want_status=$?
"$bin/dd" if="$work/nosuch" of=/dev/null 2> /dev/null
got_status=$?
check 'missing input status' "$want_status" "$got_status"

#       A running dd told to say where it is prints the summary and keeps
#       going. There is no way to compare that against the reference without
#       racing it, so the input is a pipe with a pause in the middle of it:
#       dd is certainly inside a read when the signal lands, and what is
#       checked is that the read went back to waiting rather than coming back
#       short, that a summary arrived before the final one, and that all
#       eight bytes still came out.
rm -f "$work/pipe"
mkfifo "$work/pipe" 2> /dev/null

if [ -p "$work/pipe" ]; then
        (printf 'aaaa'; sleep 1; printf 'bbbb') > "$work/pipe" &
        writer=$!

        "$bin/dd" if="$work/pipe" of="$work/info_out" bs=4 status=noxfer \
                2> "$work/info_err" &
        child=$!

        sleep 0.3
        kill -USR1 "$child" 2> /dev/null
        wait "$child" 2> /dev/null
        wait "$writer" 2> /dev/null

        if [ "$(cat "$work/info_out" 2> /dev/null)" = "aaaabbbb" ]; then
                pass=$((pass + 1))
        else
                report 'usr1 keeps copying' "got [$(cat "$work/info_out" 2> /dev/null)]"
        fi

        said=$(grep -c 'records in' "$work/info_err" 2> /dev/null)

        if [ "${said:-0}" -ge 2 ]; then
                pass=$((pass + 1))
        else
                report 'usr1 says where it is' "${said:-0} summaries, wanted the final one and one more"
        fi

        #       Two whole records, not four partial ones: a read that a
        #       signal interrupted goes back to waiting.
        if grep -q '^2+0 records in$' "$work/info_err" 2> /dev/null; then
                pass=$((pass + 1))
        else
                report 'usr1 does not shorten a read' "$(tail -2 "$work/info_err" | tr '\n' '|')"
        fi
else
        echo "  dd       usr1: no fifo here, the three signal cases did not run"
fi

#
#       diff
#

case_start diff

printf 'alpha\nbeta\ngamma\ndelta\n'            > "$work/a"
printf 'alpha\nbeta\nBETA\ngamma\ndelta\n'      > "$work/b"
printf 'alpha\nbeta\ngamma\ndelta\n'            > "$work/a2"
printf 'ALPHA\nBeta\nGamma\nDelta\n'            > "$work/case"
printf 'alpha \n  beta\ngam ma\ndelta\n'        > "$work/spaced"
printf 'alpha\t\nbeta\ngam\t\tma\ndelta\n'      > "$work/tabbed"
printf 'alpha\n\n\nbeta\n'                      > "$work/blanks"
printf 'alpha\nbeta\n'                          > "$work/tight"
printf ''                                       > "$work/empty"
printf 'one\ntwo\nthree'                        > "$work/nonl"
printf 'one\ntwo\nthree\n'                      > "$work/withnl"
printf 'one\ntwo\nthre'                         > "$work/nonl2"
printf 'a\0b\0c\n'                              > "$work/bin1"
printf 'a\0b\0d\n'                              > "$work/bin2"

seq 1 40 > "$work/long1"
seq 1 40 | sed '7s/.*/SEVEN/; 23s/.*/TWENTYTHREE/' > "$work/long2"
seq 1 40 | sed '5d; 6d; 30i\
inserted' > "$work/long3"

compare_diff 'same'            "$work/a" "$work/a2"
compare_diff 'insert'          "$work/a" "$work/b"
compare_diff 'delete'          "$work/b" "$work/a"
compare_diff 'empty left'      "$work/empty" "$work/a"
compare_diff 'empty right'     "$work/a" "$work/empty"
compare_diff 'both empty'      "$work/empty" "$work/empty"
compare_diff 'shorter'         "$work/a" "$work/tight"
compare_diff 'long change'     "$work/long1" "$work/long2"
compare_diff 'long mixed'      "$work/long1" "$work/long3"

compare_diff 'unified'         -u "$work/a" "$work/b"
compare_diff 'unified long'    -u "$work/long1" "$work/long2"
compare_diff 'unified mixed'   -u "$work/long1" "$work/long3"
compare_diff 'unified same'    -u "$work/a" "$work/a2"
compare_diff 'unified empty'   -u "$work/empty" "$work/a"
compare_diff 'unified labels'  -u -L left -L right "$work/a" "$work/b"

compare_diff 'brief differ'    -q "$work/a" "$work/b"
compare_diff 'brief same'      -q "$work/a" "$work/a2"

compare_diff 'ignore case'     -i "$work/a" "$work/case"
compare_diff 'ignore case off' "$work/a" "$work/case"
compare_diff 'ignore space'    -w "$work/a" "$work/spaced"
compare_diff 'ignore change'   -b "$work/a" "$work/spaced"
compare_diff 'ignore tabs'     -b "$work/spaced" "$work/tabbed"
compare_diff 'ignore blanks'   -B "$work/tight" "$work/blanks"
compare_diff 'ignore blanks u' -u -B "$work/tight" "$work/blanks"

#       The last line of a file that has no newline of its own is not the
#       same line as a complete one however the bytes read, and the marker
#       that says so is part of the output.
compare_diff 'no newline right' "$work/withnl" "$work/nonl"
compare_diff 'no newline left'  "$work/nonl" "$work/withnl"
compare_diff 'no newline both'  "$work/nonl" "$work/nonl2"
compare_diff 'no newline u'     -u "$work/withnl" "$work/nonl"
compare_diff 'no newline u two' -u "$work/nonl" "$work/nonl2"

compare_diff 'binary differ'   "$work/bin1" "$work/bin2"
compare_diff 'binary same'     "$work/bin1" "$work/bin1"
compare_diff 'binary as text'  -a "$work/bin1" "$work/bin2"
compare_diff 'binary brief'    -q "$work/bin1" "$work/bin2"

compare_diff 'missing operand' "$work/a" "$work/nosuchfile"

mkdir -p "$work/d1/sub" "$work/d2/sub"
printf 'one\ntwo\n'   > "$work/d1/same"
printf 'one\ntwo\n'   > "$work/d2/same"
printf 'one\ntwo\n'   > "$work/d1/differs"
printf 'one\nTWO\n'   > "$work/d2/differs"
printf 'only\n'       > "$work/d1/onlyleft"
printf 'only\n'       > "$work/d2/onlyright"
printf 'deep\n'       > "$work/d1/sub/inner"
printf 'DEEP\n'       > "$work/d2/sub/inner"

compare_diff 'directories'     "$work/d1" "$work/d2"
compare_diff 'directories r'   -r "$work/d1" "$work/d2"
compare_diff 'directories r u' -ru "$work/d1" "$work/d2"
compare_diff 'directories q'   -rq "$work/d1" "$work/d2"
compare_diff 'directories N'   -rN "$work/d1" "$work/d2"
compare_diff 'directory file'  "$work/d1" "$work/d2/same"

#
#       ps
#

case_start ps

#       The header is fixed text and is the one part of a ps listing that can
#       be compared with the reference outright. It is also what says the
#       columns are the right columns in the right order at the right width.
for one in '' '-e' '-f' '-ef' '-A'; do
        # shellcheck disable=SC2086
        ps $one 2> /dev/null | head -1 > "$work/want"
        # shellcheck disable=SC2086
        "$bin/ps" $one 2> /dev/null | head -1 > "$work/got"

        if cmp -s "$work/want" "$work/got"; then
                pass=$((pass + 1))
        else
                report "header ps $one" "want $(show "$work/want") got $(show "$work/got")"
        fi
done

for one in pid pid,ppid pid,comm user,pid,stat pid,ppid,user,comm,args,stat,time,etime,rss,vsz,tty; do
        ps -o "$one" 2> /dev/null | head -1 > "$work/want"
        "$bin/ps" -o "$one" 2> /dev/null | head -1 > "$work/got"

        if cmp -s "$work/want" "$work/got"; then
                pass=$((pass + 1))
        else
                report "header -o $one" "want $(show "$work/want") got $(show "$work/got")"
        fi
done

#       A process that is certainly running is in the listing: the shell that
#       is running this script. Its number cannot be compared against
#       anything, so what is asked is whether it is there at all.
mine=$$
if "$bin/ps" -e 2> /dev/null | awk -v p="$mine" '$1 == p { found = 1 } END { exit !found }'; then
        pass=$((pass + 1))
else
        report 'own pid listed' "pid $mine not in ps -e"
fi

#       -o selects what was asked and nothing else, which is a count of
#       fields rather than their values.
for spec in 'pid 1' 'pid,ppid 2' 'pid,ppid,user 3' 'pid,ppid,user,stat 4'; do
        want=${spec#* }
        columns=${spec% *}
        got=$("$bin/ps" -e -o "$columns" 2> /dev/null | sed -n '2p' | awk '{ print NF }')

        check "-o $columns field count" "$want" "${got:-none}"
done

#       Every listing has at least a header and, on any machine that is
#       running this, one process.
lines=$("$bin/ps" -e 2> /dev/null | wc -l)
if [ "$lines" -gt 1 ]; then
        pass=$((pass + 1))
else
        report 'ps -e has rows' "$lines lines"
fi

#       The two number columns of a row are numbers, which a listing that had
#       lost its alignment would fail.
if "$bin/ps" -e -o pid,ppid 2> /dev/null | sed -n '2,20p' |
        awk '{ if ($1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/) bad = 1 } END { exit bad }'; then
        pass=$((pass + 1))
else
        report 'pid and ppid numeric' "a row was not two numbers"
fi

#       A column nobody has heard of is refused, as it is by the reference.
ps -o nosuchcolumn > /dev/null 2>&1
want_status=$?
"$bin/ps" -o nosuchcolumn > /dev/null 2>&1
got_status=$?
check 'unknown column refused' "$((want_status != 0))" "$((got_status != 0))"

printf '  %-12s %s of %s\n' listed "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'tools-listed %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

listed_fail=$fail

#
#       The generated ones.
#
#       Random pairs of files over a small alphabet diffed both ways, and dd
#       given operands drawn out of a hat. The
#       alphabet is small on purpose: with every line distinct the longest
#       common subsequence is unique and any correct implementation agrees,
#       so a generator over distinct lines reports green and proves nothing.
#       Repeats everywhere are what make the choice of which identical line
#       to call the changed one observable, and that choice is the whole of
#       what is hard here.
#
#       The files stay small for a second reason: past a few hundred lines
#       GNU stops looking for a minimal edit script and returns a good enough
#       one from a heuristic. Agreement above that ceiling is not something
#       this claims.

if ! command -v python3 > /dev/null 2>&1; then
        echo "  generated    python3 missing, generated cases not run"
        exit 1
fi

python3 - "$bin" "$rounds" "$work" << 'PYTHON' > "$work/generated" 2>&1
import os
import random
import subprocess
import sys

binary, rounds, work = sys.argv[1], int(sys.argv[2]), sys.argv[3]

environment = dict(os.environ)
environment["LC_ALL"] = "C"
environment["TZ"] = "UTC0"

total = 0
bad = 0

left = os.path.join(work, "gen_a")
right = os.path.join(work, "gen_b")


def both(flags, a, b):
    global total, bad

    open(left, "w").write(a)
    open(right, "w").write(b)

    want = subprocess.run(["diff"] + flags + [left, right],
                          capture_output=True, env=environment)
    got = subprocess.run([binary + "/diff"] + flags + [left, right],
                         capture_output=True, env=environment)

    total += 1

    if want.stdout == got.stdout and want.returncode == got.returncode:
        return

    bad += 1

    if bad <= 6:
        print("  diff %-14s want %r[%d] got %r[%d]"
              % (" ".join(flags) or "(plain)",
                 want.stdout[:70], want.returncode,
                 got.stdout[:70], got.returncode))
        print("      a=%r" % a)
        print("      b=%r" % b)


def lines(alphabet, count):
    return "".join(random.choice(alphabet) + "\n" for _ in range(count))


random.seed(20260828)

for _ in range(rounds):
    for alphabet in ("ab", "abcde", "abcdefghij"):
        a = lines(alphabet, random.randint(0, 24))
        b = lines(alphabet, random.randint(0, 24))

        both([], a, b)
        both(["-u"], a, b)

for _ in range(rounds // 3):
    a = lines("abcde", random.randint(0, 60))
    b = lines("abcde", random.randint(0, 60))

    both([], a, b)
    both(["-u"], a, b)
    both(["-q"], a, b)

#       Longer than sixty four lines, which is where GNU stops taking every
#       line that matches nothing on the other side and starts scaling the
#       threshold for how many matches make a line confusing. A generator
#       that stayed under that ceiling would never reach the branch, and the
#       one bug that lived there -- a threshold computed from the wrong
#       variable, so every repeated line was thrown away -- passed several
#       thousand shorter cases without a murmur.
for _ in range(rounds // 4):
    for alphabet in ("abcde", "abcdefghij"):
        a = lines(alphabet, random.randint(60, 400))
        b = lines(alphabet, random.randint(0, 400))

        both([], a, b)
        both(["-u"], a, b)

#       The same length, with one file a lightly edited copy of the other,
#       which is the shape a diff is actually asked for.
for _ in range(rounds // 4):
    base = ["line %d of it" % i for i in range(random.randint(20, 300))]
    other = list(base)

    for _ in range(random.randint(0, 14)):
        if not other:
            break

        where = random.randrange(len(other))
        what = random.randrange(4)

        if what == 0:
            other[where] = "changed %d" % random.randrange(40)
        elif what == 1:
            del other[where]
        elif what == 2:
            other.insert(where, "inserted %d" % random.randrange(40))
        else:
            other.insert(where, other[where])

    a = "".join(line + "\n" for line in base)
    b = "".join(line + "\n" for line in other)

    both([], a, b)
    both(["-u"], a, b)
    both(["-q"], a, b)

#       Words rather than single letters, so a line can differ from another
#       by its case or by its spaces and the ignore flags have something to
#       ignore. The identical head and tail are trimmed by raw bytes while
#       the matcher compares folded ones, so these two stages disagree on
#       purpose and this is where that shows.
words = ["alpha", "ALPHA", "Alpha", "beta", "BETA", " beta", "beta ",
         "gam ma", "gam  ma", "gamma", "", "  ", "delta\t"]

for _ in range(rounds):
    a = "".join(random.choice(words) + "\n" for _ in range(random.randint(0, 14)))
    b = "".join(random.choice(words) + "\n" for _ in range(random.randint(0, 14)))

    for flags in ([], ["-i"], ["-w"], ["-b"], ["-B"], ["-u"], ["-u", "-b"],
                  ["-u", "-B"], ["-i", "-w"]):
        both(flags, a, b)

#       A file whose last line has no newline takes a different path through
#       the trimming: the identical tail is not taken off at all unless both
#       files are missing one, so every pairing of the four is worth having.
for _ in range(rounds // 2):
    a = lines("abc", random.randint(1, 12))
    b = lines("abc", random.randint(1, 12))

    for cut_a in (False, True):
        for cut_b in (False, True):
            both([], a[:-1] if cut_a else a, b[:-1] if cut_b else b)
            both(["-u"], a[:-1] if cut_a else a, b[:-1] if cut_b else b)

#       Directory trees, which are the one place a name from one level can be
#       compared against a name from another: the walk recurses, and a level
#       that kept its names in a buffer shared with the level below went on
#       comparing whatever the level below left there. What is compared is
#       the whole listing -- the "Only in" lines, their order, the headers
#       naming each pair, and the diffs themselves.
import shutil


def tree(root, seed):
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)

    own = random.Random(seed)

    for _ in range(own.randint(0, 8)):
        name = os.path.join(root, "f%d" % own.randint(0, 10))
        open(name, "w").write("".join(own.choice("abcde") + "\n"
                                     for _ in range(own.randint(0, 12))))

    for _ in range(own.randint(0, 3)):
        under = os.path.join(root, "d%d" % own.randint(0, 4))
        os.makedirs(under, exist_ok=True)

        for _ in range(own.randint(0, 4)):
            name = os.path.join(under, "g%d" % own.randint(0, 6))
            open(name, "w").write("".join(own.choice("xyz") + "\n"
                                          for _ in range(own.randint(0, 8))))

        deeper = os.path.join(under, "deeper")
        os.makedirs(deeper, exist_ok=True)
        open(os.path.join(deeper, "h"), "w").write(
            "".join(own.choice("pq") + "\n" for _ in range(own.randint(0, 5))))


one = os.path.join(work, "tree_a")
two = os.path.join(work, "tree_b")

for round_number in range(rounds // 6):
    tree(one, round_number * 2)
    tree(two, round_number * 2 + random.choice([0, 1, 7]))

    for flags in (["-r"], ["-r", "-u"], ["-r", "-q"], ["-r", "-N"],
                  ["-r", "-N", "-u"], []):
        want = subprocess.run(["diff"] + flags + [one, two],
                              capture_output=True, env=environment)
        got = subprocess.run([binary + "/diff"] + flags + [one, two],
                             capture_output=True, env=environment)

        total += 1

        if want.stdout == got.stdout and want.returncode == got.returncode:
            continue

        bad += 1

        if bad <= 6:
            print("  diff %-14s want %r[%d] got %r[%d]"
                  % (" ".join(flags) or "(plain)",
                     want.stdout[:70], want.returncode,
                     got.stdout[:70], got.returncode))

shutil.rmtree(one, ignore_errors=True)
shutil.rmtree(two, ignore_errors=True)

#       dd, with operands drawn at random rather than listed. What is
#       compared is standard output, the exit status, and the summary on the
#       error stream with the duration and rate cut off. Sizes on either side
#       of a block boundary are what make the records lines interesting: the
#       counting is per block, and a naive one gets the last block wrong.
sizes = [0, 1, 7, 511, 512, 513, 1000, 4096, 10000]
feeds = []

for which, size in enumerate(sizes):
    name = os.path.join(work, "dd_feed_%d" % which)
    open(name, "wb").write(bytes(random.randrange(256) for _ in range(size)))
    feeds.append(name)

suffixes = ["", "c", "b", "K", "KB", "KiB", "M"]


def dd_both(args, feed):
    global total, bad

    want = subprocess.run(["dd"] + args, stdin=open(feed, "rb"),
                          capture_output=True, env=environment)
    got = subprocess.run([binary + "/dd"] + args, stdin=open(feed, "rb"),
                         capture_output=True, env=environment)

    want_error = want.stderr.split(b" copied,")[0]
    got_error = got.stderr.split(b" copied,")[0]

    total += 1

    if (want.stdout == got.stdout and want_error == got_error
            and want.returncode == got.returncode):
        return

    bad += 1

    if bad <= 6:
        print("  dd %-20s want %r[%d] got %r[%d]"
              % (" ".join(args),
                 want_error[:70], want.returncode,
                 got_error[:70], got.returncode))


for _ in range(rounds):
    args = []

    if random.random() < 0.6:
        args.append("bs=%d%s" % (random.randint(1, 17), random.choice(suffixes)))
    else:
        args.append("ibs=%d" % random.randint(1, 300))
        args.append("obs=%d" % random.randint(1, 300))

    if random.random() < 0.5:
        args.append("count=%d" % random.randint(0, 40))

    if random.random() < 0.4:
        args.append("skip=%d" % random.randint(0, 40))

    conversions = [name for name in ("sync", "noerror", "fsync", "fdatasync")
                   if random.random() < 0.25]

    if conversions:
        args.append("conv=" + ",".join(conversions))

    level = random.choice(["status=noxfer", "status=noxfer", "status=none", ""])

    if level:
        args.append(level)

    dd_both(args, random.choice(feeds))

print("\n  %s of %s" % (total - bad, total))
PYTHON

sed -n '/want/p;/^      /p' "$work/generated"

line=$(sed -n 's/^  \([0-9]*\) of \([0-9]*\)$/\1 \2/p' "$work/generated")

#       An empty parse would leave both counts empty and compare equal, which
#       is a suite reporting that nothing failed because nothing ran.
if [ -z "$line" ]; then
        echo "  generated    printed no verdict"
        sed 's/^/    /' "$work/generated" | tail -5
        exit 1
fi

made=${line% *}
made_total=${line#* }

printf '  %-12s %s of %s\n' generated "$made" "$made_total"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'tools-generated %s %s\n' "$made" "$made_total" >> "$TEST_TALLY"

[ "$listed_fail" = 0 ] && [ "$made" = "$made_total" ]
