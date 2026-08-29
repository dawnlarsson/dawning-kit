#!/bin/sh
#
#       The text utilities against the ones already on the machine.
#
#           sh src/test/text.sh [directory of the names our shell answers to]
#
#       Every case runs the same input through the system's grep, sed, cut --
#       whatever the case names -- and through ours, and compares standard
#       output and the exit status. Agreeing is passing; there is no separate
#       idea here of what the right answer is.
#
#       Two sections. The listed ones are a list somebody wrote down, which
#       means they test what that somebody thought of. The generated ones
#       build patterns instead, and are what found the one that mattered: a
#       jump target that pointed at the instruction a quantifier was about to
#       be inserted in front of, which broke \(a\)*\(a\)* and nothing
#       simpler. They need python3 and say so when it is missing.
#
#       LC_ALL=C is not politeness. GNU sort collates by locale, and a
#       byte-wise sort disagrees with it on almost any mixed case input, so
#       without this every sort case fails and looks like a bug in the
#       comparison rather than in the question being asked.

LC_ALL=C
export LC_ALL

bin=${1:-/tmp/multi}
rounds=${2:-600}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir "$work/read_dir"

pass=0
fail=0
group=""

printf 'alpha beta gamma\ndelta epsilon\n\nzeta eta theta iota\nalpha beta gamma\n' > "$work/a"
printf 'one:two:three\nfour:five:six\nnodelim\nseven::nine\n' > "$work/b"
printf '10\n9\n100\n2\n-3\n2.5\n0\n' > "$work/c"
printf 'b 2 x\na 10 y\nc 1 z\na 3 w\nb 2 x\n' > "$work/d"
printf 'apple\napple\nbanana\ncherry\ncherry\ncherry\ndate\n' > "$work/e"
printf 'Hello World\nHELLO world\nhello WORLD\n' > "$work/f"
printf 'aaa\tbbb\tccc\nddd\teee\tfff\n' > "$work/g"
printf 'no newline at the end' > "$work/h"
printf '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n' > "$work/i"
printf 'foo123bar\nbaz456qux\nnothing here\nFOO789BAR\n' > "$work/j"
printf 'x\n\ny\n\n\nz\n' > "$work/k"
printf 'a\n\\:\\:\\:\nhdr\n\\:\\:\nbody1\nbody2\n\\:\nfoot\n' > "$work/sections"
printf 'x\n\n\n\ny\n' > "$work/blanks"
printf 'a\tb\nlonger line here\n\rwide\n' > "$work/wide"
printf '  ab  cd ef\nxy\tzw\nplain\n' > "$work/spaced"
printf '\376\377\n' > "$work/tr_high"
head -c 6000 /dev/zero | tr '\0' x > "$work/cut_wide"
printf '\n' >> "$work/cut_wide"
head -c 65535 /dev/zero | tr '\0' x > "$work/wc_boundary"
printf ' y\n' >> "$work/wc_boundary"

# A single physical block with a ten-digit logical size exercises the wc width
# chosen from stat(2) without making wc read a gigabyte.
sparse_width=false
printf 'x\n' > "$work/sparse_width"
if dd if=/dev/zero of="$work/sparse_width" bs=1 count=1 seek=999999999 conv=notrunc 2>/dev/null
then
        allocated=$(stat -c %b "$work/sparse_width" 2>/dev/null) || allocated=

        case $allocated in
        ''|*[!0-9]*) ;;
        *) [ "$allocated" -le 2048 ] && sparse_width=true ;;
        esac
fi

[ "$sparse_width" = true ] || rm -f "$work/sparse_width"

# The first numbered line leaves the output buffer exactly full. The next
# number therefore flushes while it is being written, which must not change
# the left-aligned field width.
head -c 65528 /dev/zero | tr '\0' x > "$work/nl_flush"
printf '\ny\n' >> "$work/nl_flush"

#       -z reads and writes lines that end in a NUL rather than a newline,
#       which is the one fixture here that cannot be read by eye.
printf 'xa\000yb\000zc\000' > "$work/zeros"
printf 'a:b\000c:d\000a:b\000' > "$work/zpairs"

case_start()
{
        group=$1
}

# Runs one command both ways. The tool is the first argument; everything
# after it is passed to both, and standard input comes from the named file.
compare()
{
        name=$1
        tool=$2
        feed=$3
        shift 3

        if [ "$feed" = "-" ]; then
                "$tool" "$@" > "$work/want" 2> /dev/null
                want_status=$?
                "$bin/$tool" "$@" > "$work/got" 2> /dev/null
                got_status=$?
        else
                "$tool" "$@" < "$work/$feed" > "$work/want" 2> /dev/null
                want_status=$?
                "$bin/$tool" "$@" < "$work/$feed" > "$work/got" 2> /dev/null
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" \
                "$(head -c 34 "$work/want" | tr '\n\t' '|>')[$want_status]" \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status]"
}

#       A tool that rewrites its input cannot be handed the same file twice:
#       the second run would read what the first one wrote. Each side gets its
#       own copy and the copies are compared afterwards, because what -i puts
#       on standard output is nothing.
compare_edit()
{
        name=$1
        tool=$2
        feed=$3
        shift 3

        cp "$work/$feed" "$work/edit_want"
        cp "$work/$feed" "$work/edit_got"

        "$tool" "$@" "$work/edit_want" > /dev/null 2>&1
        want_status=$?
        "$bin/$tool" "$@" "$work/edit_got" > /dev/null 2>&1
        got_status=$?

        if cmp -s "$work/edit_want" "$work/edit_got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" \
                "$(head -c 34 "$work/edit_want" | tr '\n\t' '|>')[$want_status]" \
                "$(head -c 34 "$work/edit_got" | tr '\n\t' '|>')[$got_status]"
}

#       A tool that writes somewhere other than standard output has a third
#       thing to compare. Each side names its own file, and what landed in it
#       is compared after the run along with what came back.
compare_side()
{
        name=$1
        tool=$2
        feed=$3
        script=$4
        shift 4

        rm -f "$work/side_want" "$work/side_got"

        "$tool" "$@" "$(printf '%s' "$script" | sed "s|SIDE|$work/side_want|")" \
                < "$work/$feed" > "$work/want" 2> /dev/null
        want_status=$?
        "$bin/$tool" "$@" "$(printf '%s' "$script" | sed "s|SIDE|$work/side_got|")" \
                < "$work/$feed" > "$work/got" 2> /dev/null
        got_status=$?

        cat "$work/side_want" >> "$work/want" 2> /dev/null
        cat "$work/side_got" >> "$work/got" 2> /dev/null

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" \
                "$(head -c 34 "$work/want" | tr '\n\t' '|>')[$want_status]" \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status]"
}

# A fixed-memory tool may refuse a record it cannot hold, but it must not
# print a convincing prefix and report success. The system tools have no such
# ceiling, so these are contract checks on ours rather than differential rows.
refuses_long_record()
{
        name=$1
        tool=$2
        shift 2

        "$bin/$tool" "$@" < "$work/overlong" > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q "^$tool: line too long$" "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" 'loud refusal' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

refuses_long_pipe()
{
        name=$1
        tool=$2
        shift 2

        cat "$work/overlong" | "$bin/$tool" "$@" > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q "^$tool: line too long$" "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" 'loud refusal' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

refuses_long_pattern()
{
        "$bin/grep" -f "$work/grep_long_pattern" "$work/grep_long_subject" \
                > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -eq 2 ] && [ ! -s "$work/got" ] &&
           grep -q '^grep: pattern too long$' "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" 'grep pattern ceiling' 'loud refusal [2]' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

refuses_grep_context()
{
        name=$1
        feed=$2
        shift 2

        "$bin/grep" "$@" "$work/$feed" > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -eq 2 ] && [ ! -s "$work/got" ] &&
           grep -q '^grep: context .* too large$' "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" 'loud refusal [2]' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

refuses_long_sed_script()
{
        "$bin/sed" -f "$work/sed_long_script" "$work/one" \
                > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q '^sed: unsupported or invalid script$' "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" 'sed script ceiling' 'loud refusal' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

refuses_many_grep_globs()
{
        set --
        i=0

        while [ "$i" -lt 33 ]; do
                set -- "$@" '--include=*'
                i=$((i + 1))
        done

        "$bin/grep" "$@" x "$work/one" > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -eq 2 ] && [ ! -s "$work/got" ] &&
           grep -q '^grep: too many include patterns$' "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" 'grep glob ceiling' 'loud refusal [2]' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

# These modes have no correct bounded implementation here yet. They must be
# rejected instead of silently becoming ordinary byte sorting.
refuses_sort_mode()
{
        name=$1
        shift

        "$bin/sort" "$@" < "$work/a" > "$work/got" 2> "$work/err"
        got_status=$?

        if [ "$got_status" -ne 0 ] && [ ! -s "$work/got" ] &&
           grep -q '^sort:' "$work/err"
        then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' \
                "$group" "$name" 'loud refusal' \
                "$(head -c 34 "$work/got" | tr '\n\t' '|>')[$got_status] $(head -1 "$work/err")"
}

case_start grep
compare 'literal'        grep a  alpha
compare 'anchor start'   grep a  '^delta'
compare 'anchor end'     grep a  'gamma$'
compare 'any'            grep a  'd.lta'
compare 'star'           grep a  'al*pha'
compare 'star greedy'    grep a  'a.*a'
compare 'class'          grep a  '[dz]'
compare 'class range'    grep j  '[0-9]'
compare 'class negated'  grep j  '[^0-9a-z]'
compare 'class named'    grep j  '[[:digit:]]'
compare 'group basic'    grep a  '\(al\)pha'
compare 'backref'        grep a  '\(a\)lph\1'
compare 'alternation'    grep a  'delta\|zeta'
compare 'interval'       grep j  '[0-9]\{3\}'
compare 'plus gnu'       grep j  '[0-9]\+'
compare 'question gnu'   grep a  'alphas\?'
compare 'word start'     grep a  '\<beta'
compare 'word boundary'  grep a  '\bbeta\b'
compare 'ignore case'    grep f  -i hello
compare 'invert'         grep a  -v alpha
compare 'numbered'       grep a  -n a
compare 'count'          grep a  -c a
compare 'quiet hit'      grep a  -q alpha
compare 'quiet miss'     grep a  -q nowhere
compare 'no match'       grep a  nowhere
compare 'combined'       grep a  -in ALPHA
compare 'extended alt'   grep -  -E 'delta|zeta' "$work/a"
compare 'extended plus'  grep -  -E '[0-9]+' "$work/j"
compare 'extended group' grep -  -E '(al)+pha' "$work/a"
compare 'extended count' grep -  -E '[0-9]{3}' "$work/j"
compare 'extended opt'   grep -  -E 'alphas?' "$work/a"
compare 'fixed'          grep a  -F 'alpha beta'
compare 'files listed'   grep -  -l a "$work/a" "$work/b"
compare 'two files'      grep -  a "$work/a" "$work/b"
compare 'two files count' grep - -c a "$work/a" "$work/b"
compare 'whole line'     grep a  -x 'delta epsilon'
compare 'whole word'     grep a  -w beta
compare 'empty pattern'  grep a  ''
compare 'dot star'       grep a  '.*'
compare 'no trailing nl' grep h  newline
compare 'expression flag' grep a -e alpha
compare 'only matching'  grep a  -o 'a[a-z]*'
compare 'only numbered'  grep a  -on 'a[a-z]*'
compare 'only offsets'   grep a  -ob 'a[a-z]*'
compare 'byte offset'    grep a  -b a
compare 'max count'      grep a  -m 2 a
compare 'max count none' grep a  -m 0 a
compare 'after context'  grep a  -A1 alpha
compare 'before context' grep a  -B1 zeta
compare 'context both'   grep k  -C1 y
compare 'context groups' grep i  -C1 '^[13]$'
compare 'context count'  grep a  -c -A1 alpha
compare 'files without'  grep a  -L nowhere
compare 'files without hit' grep a -L alpha
compare 'named stdin'    grep a  -H a
compare 'named count'    grep a  -cH a
compare 'named label'    grep a  --label=X -H a
compare 'null names'     grep -  -Z -l a "$work/a" "$work/b"
compare 'initial tab'    grep a  -T -n a
compare 'initial tab off' grep a -T a
compare 'empty pattern file' grep a -f /dev/null
compare 'empty file invert' grep a -v -f /dev/null
compare 'unknown letter' grep a  -N a
compare 'long ignore case' grep f --ignore-case hello
compare 'long invert'    grep a  --invert-match alpha
compare 'long numbered'  grep a  --line-number a
compare 'long count'     grep a  --count a
compare 'long only'      grep a  --only-matching 'a[a-z]*'
compare 'long max count' grep a  --max-count 2 a
compare 'long context'   grep a  --context 1 alpha
compare 'long word'      grep a  --word-regexp beta
compare 'long fixed'     grep a  --fixed-strings 'alpha beta'
compare 'long extended'  grep -  --extended-regexp 'delta|zeta' "$work/a"
compare 'long quiet'     grep a  --quiet alpha
compare 'long joined'    grep a  --regexp=alpha
compare 'long unknown'   grep a  --nosuchflag a
compare 'long after --'  grep a  -- --nosuchflag
compare 'null data'      grep zeros -z a
compare 'null data long' grep zeros --null-data a
compare 'null data count' grep zeros -z -c a
compare 'null data list' grep zeros -z -l a
compare 'null data numbered' grep zeros -z -nb a
compare 'null data only'  grep zeros -z -o a
compare 'null data invert' grep zeros -z -v a
compare 'null data names' grep zeros -z -Z -H a
compare 'null data context' grep zeros -z -C1 b
compare 'null data patterns' grep zeros -z -e a -e b

case_start sed
compare 'substitute'     sed a  's/alpha/ALPHA/'
compare 'global'         sed a  's/a/A/g'
compare 'nth'            sed a  's/a/A/2'
compare 'nth global'     sed a  's/a/A/2g'
compare 'print flag'     sed a  -n 's/alpha/X/p'
compare 'ampersand'      sed a  's/alpha/[&]/'
compare 'group ref'      sed a  's/\(al\)pha/\1/'
compare 'two groups'     sed b  's/\(one\):\(two\)/\2:\1/'
compare 'empty match'    sed a  's/x*/-/g'
compare 'delete'         sed a  '/alpha/d'
compare 'delete range'   sed i  '2,4d'
compare 'quiet print'    sed a  -n '2p'
compare 'last line'      sed a  -n '$p'
compare 'line range'     sed i  -n '3,7p'
compare 'regex range'    sed a  -n '/delta/,/zeta/p'
compare 'negated'        sed a  -n '/alpha/!p'
compare 'quit'           sed i  '3q'
compare 'line number'    sed a  -n '='
compare 'transliterate'  sed a  'y/abc/xyz/'
compare 'other delim'    sed b  's,one,ONE,'
compare 'escaped delim'  sed b  's/one:two/X/'
compare 'block'          sed i  -n '2,4{p}'
compare 'two scripts'    sed a  -e 's/a/1/' -e 's/b/2/'
compare 'semicolons'     sed a  's/a/1/;s/b/2/'
compare 'anchor'         sed a  's/^alpha/X/'
compare 'anchor end'     sed a  's/gamma$/X/'
compare 'class'          sed j  's/[0-9]\+/N/'
compare 'case flag'      sed f  's/hello/X/I'
compare 'append'         sed k  '2a added'
compare 'insert'         sed k  '2i added'
compare 'change'         sed k  '2c changed'
compare 'hold'           sed a  -n '1h;$ {x;p}'
compare 'no trailing nl' sed h  's/newline/NEWLINE/'
compare 'delete blank'   sed k  '/^$/d'
compare 'sub blank'      sed k  's/^$/EMPTY/'
compare 'multiple files' sed -  's/a/A/' "$work/a" "$work/b"
compare 'long quiet'     sed a  --quiet '2p'
compare 'long silent'    sed a  --silent '2p'
compare 'long expression' sed a --expression='s/a/A/'
compare 'long file'      sed a  --file /dev/null
compare 'empty script file' sed a -f /dev/null
compare 'extended alt'   sed a  -E 's/alpha|zeta/X/'
compare 'extended plus'  sed a  -E 's/(al)+pha/X/'
compare 'extended group' sed a  -E 's/(a)(l)/\2\1/'
compare 'extended opt'   sed a  -E 's/alphas?/X/'
compare 'extended count' sed a  -E 's/a{2}/X/'
compare 'extended r flag' sed a -r 's/alpha|zeta/X/'
compare 'long extended'  sed a  --regexp-extended 's/alpha|zeta/X/'
compare 'basic still basic' sed a 's/\(al\)pha/X/'
compare 'separate last'  sed -  -s -n '$p' "$work/a" "$work/b"
compare 'joined last'    sed -  -n '$p' "$work/a" "$work/b"
compare 'separate number' sed - -s -n '=' "$work/a" "$work/b"
compare 'long separate'  sed -  --separate -n '$p' "$work/a" "$work/b"
compare 'unbuffered'     sed a  -u 's/a/A/'
compare 'long unbuffered' sed a --unbuffered 's/a/A/'
compare 'line length'    sed a  -l 2 's/a/A/'
compare 'long line length' sed a --line-length 2 's/a/A/'
compare 'posix'          sed a  --posix 's/a/A/'
compare 'sandbox'        sed a  --sandbox 's/a/A/'
compare 'in place no files' sed a -i 's/a/A/'
compare 'unknown letter' sed a  -Q 's/a/A/'
compare 'long unknown'   sed a  --nosuchflag 's/a/A/'
compare 'missing file'   sed -  's/a/A/' "$work/nosuch"
compare_edit 'in place'  sed a  -i 's/a/A/'
compare_edit 'in place backup' sed a -i.bak 's/a/A/'
compare_edit 'in place long' sed a --in-place 's/a/A/'
compare_edit 'in place long backup' sed a --in-place=.bak 's/a/A/'
compare_edit 'in place quiet' sed a -n -i '2p'
compare 'null data'      sed zeros -z 's/a/A/'
compare 'null data long' sed zeros --null-data 's/a/A/'
compare 'null data number' sed zeros -z -n '='
compare 'null data last' sed zeros -z '$d'

case_start cut
compare 'field one'      cut b  -d: -f1
compare 'field two'      cut b  -d: -f2
compare 'field list'     cut b  -d: -f1,3
compare 'field range'    cut b  -d: -f1-2
compare 'field open'     cut b  -d: -f2-
compare 'suppress'       cut b  -d: -f2 -s
compare 'tab default'    cut g  -f2
compare 'characters'     cut a  -c1-5
compare 'character list' cut a  -c1,3,5
compare 'character open' cut a  -c6-
compare 'bytes'          cut a  -b1-3
compare 'field high'     cut b  -d: -f9
compare 'no newline'     cut h  -c1-4
compare 'complement fields' cut b --complement -d: -f2
compare 'complement chars' cut a --complement -c1-3
compare 'complement list' cut a  --complement -c1,3
compare 'output delimiter' cut b -d: -f1,3 --output-delimiter=X
compare 'output chars'   cut a  -c1,3 --output-delimiter=X
compare 'output ranges'  cut a  -c1-2,4 --output-delimiter=X
compare 'output joined'  cut a  -c1,2 --output-delimiter=X
compare 'output short'   cut a  -OX -c1,3
compare 'whitespace one' cut spaced -w -f1
compare 'whitespace two' cut spaced -w -f2
compare 'whitespace rest' cut spaced -w -f2-
compare 'whitespace list' cut spaced -w -f1,2
compare 'long delimiter' cut b  --delimiter=: --fields=1
compare 'long characters' cut a --characters=1-3
compare 'long bytes'     cut a  --bytes=2-
compare 'long suppress'  cut b  --only-delimited -d: -f2
compare 'two lists'      cut a  -b 2 -c1-3
compare 'delimiter no fields' cut a -d, -c1
compare 'suppress no fields' cut a -s -c1
compare 'whitespace no fields' cut a -w -c1
compare 'whitespace and delimiter' cut spaced -w -d: -f1
compare 'no partial'     cut a   --no-partial -c1-3
compare 'unknown letter' cut a  -Z -c1
compare 'long unknown'   cut a  --nosuchflag -c1
compare 'null data'      cut zpairs -z -d: -f2
compare 'null data chars' cut zpairs -z -c1
compare 'null data long' cut zpairs --zero-terminated -d: -f1

case_start tr
compare 'translate'      tr a  a-z A-Z
compare 'single'         tr a  a X
compare 'short set'      tr a  abc X
compare 'delete'         tr a  -d aeiou
compare 'squeeze'        tr a  -s ' '
compare 'squeeze set'    tr e  -s a
compare 'complement'     tr j  -d -c '0-9\n'
compare 'class upper'    tr a  '[:lower:]' '[:upper:]'
compare 'class digit'    tr j  -d '[:digit:]'
compare 'escape'         tr a  ' ' '\n'
compare 'range map'      tr a  'a-e' '1-5'
compare 'delete squeeze' tr a  -ds aeiou ' '
compare 'truncate'       tr a  -t abc xy
compare 'truncate range' tr a  -t a-z A-M
compare 'long complement' tr a --complement a-y X
compare 'long delete'    tr a  --delete ab
compare 'long squeeze'   tr a  --squeeze-repeats an
compare 'long truncate'  tr a  --truncate-set1 abc xy
compare 'extra operand'  tr a  -d a A
compare 'missing second' tr a  a
compare 'three operands' tr a  -s a b c
compare 'long unknown'   tr a  --nosuchflag a b

case_start sort
compare 'plain'          sort a
compare 'reverse'        sort a  -r
compare 'numeric'        sort c  -n
compare 'numeric rev'    sort c  -nr
compare 'unique'         sort e  -u
compare 'unique plain'   sort a  -u
compare 'fold'           sort f  -f
compare 'key two'        sort d  -k2
compare 'key two numeric' sort d -k2n
compare 'key range'      sort d  -k1,1
compare 'key two only'   sort d  -k2,2
compare 'separator'      sort b  -t: -k2
compare 'separator num'  sort b  -t: -k1
compare 'blanks'         sort d  -b -k2
compare 'key reverse'    sort d  -k2r
compare 'two keys'       sort d  -k1,1 -k2n
compare 'numeric unique' sort c  -nu
compare 'no newline'     sort h

case_start uniq
compare 'plain'          uniq e
compare 'count'          uniq e  -c
compare 'repeated'       uniq e  -d
compare 'unique only'    uniq e  -u
compare 'ignore case'    uniq f  -i
compare 'skip fields'    uniq d  -f1
compare 'skip chars'     uniq e  -s1
compare 'width'          uniq e  -w2
compare 'count repeated' uniq e  -cd
compare 'all repeated'   uniq e  -D
compare 'all repeated none' uniq e --all-repeated=none
compare 'all repeated prepend' uniq e --all-repeated=prepend
compare 'all repeated separate' uniq e --all-repeated=separate
compare 'group'          uniq e  --group
compare 'group separate' uniq e  --group=separate
compare 'group prepend'  uniq e  --group=prepend
compare 'group append'   uniq e  --group=append
compare 'group both'     uniq e  --group=both
compare 'long count'     uniq e  --count
compare 'long repeated'  uniq e  --repeated
compare 'long unique'    uniq e  --unique
compare 'long ignore case' uniq f --ignore-case
compare 'long skip fields' uniq d --skip-fields 1
compare 'long skip chars' uniq e --skip-chars 1
compare 'long check chars' uniq e --check-chars 2
compare 'all repeated counted' uniq e -c -D
compare 'group with count' uniq e --group -c
compare 'bad repeat word' uniq e --all-repeated=nosuch
compare 'long unknown'   uniq e  --nosuchflag
compare 'null data'      uniq zpairs -z
compare 'null data count' uniq zpairs -z -c
compare 'null data long' uniq zpairs --zero-terminated
compare 'no newline'     uniq h
compare 'no newline count' uniq h -c
compare 'null no newline' uniq -  -z "$work/h"

case_start head
compare 'default'        head i
compare 'count'          head i  -n 3
compare 'joined'         head i  -n3
compare 'short'          head i  -3
compare 'zero'           head i  -n 0
compare 'bytes'          head a  -c 10
compare 'more than have' head a  -n 100
compare 'two files'      head -  -n 2 "$work/a" "$work/b"
compare 'no newline'     head h  -n 1
compare 'long lines'     head i  --lines 3
compare 'long bytes'     head a  --bytes 10
compare 'long quiet'     head -  --quiet "$work/a" "$work/b"
compare 'long silent'    head -  --silent "$work/a" "$work/b"
compare 'long verbose'   head a  --verbose
compare 'long unknown'   head a  --nosuchflag
compare 'null data'      head zeros -z -n 2
compare 'null data long' head zeros --zero-terminated -n 2

#       A count with a minus in front of it is what to leave off the end,
#       which is found by seeking when the input is a file and by holding
#       every line when it is a pipe.
compare 'all but last'   head i  -n -1
compare 'all but three'  head i  -n -3
compare 'all but none'   head i  -n -0
compare 'all but too many' head i -n -99
compare 'bytes but last' head a  -c -3
compare 'bytes but none' head a  -c -0
compare 'bytes but all'  head a  -c -999
compare 'short unended'  head h  -n -1
compare 'short unended byte' head h -c -1
compare 'short zero ends' head zeros -z -n -1
compare 'short long form' head i  --lines=-2
compare 'short two files' head -  -n -1 "$work/a" "$work/e"
compare 'short by name'   head -  -c -4 "$work/a"

case_start tail
compare 'default'        tail i
compare 'count'          tail i  -n 3
compare 'joined'         tail i  -n3
compare 'short'          tail i  -3
compare 'from line'      tail i  -n +12
compare 'bytes'          tail a  -c 10
compare 'more than have' tail a  -n 100
compare 'two files'      tail -  -n 2 "$work/a" "$work/b"
compare 'no newline'     tail h  -n 1
compare 'long lines'     tail i  --lines 3
compare 'long bytes'     tail a  --bytes 10
compare 'long quiet'     tail -  --quiet "$work/a" "$work/b"
compare 'long verbose'   tail a  --verbose
compare 'retry'          tail a  --retry
compare 'pid ignored'    tail a  --pid 2
compare 'sleep ignored'  tail a  --sleep-interval 2
compare 'unchanged stats' tail a --max-unchanged-stats 2
compare 'sleep short'    tail a  -s 2
compare 'debug'          tail a  --debug
compare 'long unknown'   tail a  --nosuchflag
compare 'null data'      tail zeros -z -n 2
compare 'null data long' tail zeros --zero-terminated -n 2
compare 'null data file' tail -  -z -n 2 "$work/zeros"

case_start wc
compare 'default'        wc a
compare 'lines'          wc a  -l
compare 'words'          wc a  -w
compare 'bytes'          wc a  -c
compare 'characters'     wc a  -m
compare 'lines characters' wc a -lm
compare 'word at read boundary' wc wc_boundary -w
compare 'default at read boundary' wc wc_boundary
compare 'lines words at boundary' wc wc_boundary -lw
compare 'words bytes at boundary' wc wc_boundary -wc
compare 'words chars at boundary' wc wc_boundary -wm
compare 'named'          wc -  "$work/a"
compare 'two files'      wc -  "$work/a" "$work/b"
compare 'lines named'    wc -  -l "$work/a"
compare 'lines two'      wc -  -l "$work/a" "$work/b"
[ "$sparse_width" = false ] || \
        compare 'width ten digits' wc - -c "$work/sparse_width" "$work/a"
compare 'no newline'     wc h
compare 'empty'          wc k  -l
compare 'longest'        wc a  -L
compare 'longest tabs'   wc wide -L
compare 'longest bytes'  wc wide -Lc
compare 'longest all'    wc wide -lwcmL
compare 'longest two'    wc -    -L "$work/a" "$work/wide"
compare 'long lines'     wc a    --lines
compare 'long words'     wc a    --words
compare 'long bytes'     wc a    --bytes
compare 'long chars'     wc a    --chars
compare 'long longest'   wc wide --max-line-length
compare 'long unknown'   wc a    --nosuchflag
compare 'debug'          wc a    --debug

case_start rev
compare 'plain'          rev a
compare 'blank lines'    rev k
compare 'no newline'     rev h
compare 'null data'      rev zeros -0
compare 'null data long' rev zeros --zero

case_start nl
compare 'default'        nl a
compare 'all lines'      nl a  -ba
compare 'width'          nl a  -w3
compare 'separator'      nl a  -s:
compare 'start'          nl a  -v5
compare 'step'           nl a  -i2
compare 'blank heavy'    nl k
compare 'no numbering'   nl a   -bn
compare 'pattern'        nl a   -b p^alpha
compare 'pattern miss'   nl a   -b pnowhere
compare 'sections'       nl sections
compare 'sections all'   nl sections -ha -fa
compare 'sections keep'  nl sections -p -ha -fa
compare 'join blanks'    nl blanks -ba -l3
compare 'join blanks two' nl blanks -ba -l2
compare 'delimiter'      nl sections -d '\:'
compare 'format left'    nl a   -nln
compare 'format left flush' nl nl_flush -nln -w6
compare 'format zeros'   nl a   -nrz
compare 'format right'   nl a   -nrn
compare 'long body'      nl a   --body-numbering=a
compare 'long width'     nl a   --number-width 3
compare 'long separator' nl a   --number-separator=:
compare 'long start'     nl a   --starting-line-number 5
compare 'long increment' nl a   --line-increment 2
compare 'long format'    nl a   --number-format=ln
compare 'long header'    nl sections --header-numbering=a --footer-numbering=a
compare 'long renumber'  nl sections --no-renumber -ha -fa
compare 'long join'      nl blanks --join-blank-lines 3 -ba
compare 'long delimiter' nl sections --section-delimiter '\:'
compare 'unknown letter' nl a   -Z
compare 'long unknown'   nl a   --nosuchflag

case_start fold
compare 'default'        fold a
compare 'width'          fold a  -w 8
compare 'joined width'   fold a  -w8
compare 'spaces'         fold a  -w 8 -s
compare 'narrow'         fold a  -w 3
compare 'bytes'          fold g  -w 4 -b
compare 'no newline'     fold h  -w 5
compare 'long width'     fold a  --width 8
compare 'long bytes'     fold g  --bytes --width 4
compare 'long spaces'    fold a  --spaces --width 8
compare 'long unknown'   fold a  --nosuchflag
compare 'characters'     fold a  -c -w 8
compare 'long characters' fold a --characters --width 8

case_start tee
compare 'passthrough'    tee a  "$work/tee1"
compare 'append'         tee a  -a "$work/tee2"


#       Harder cases. Everything above was written alongside the code and
#       agrees for that reason; these were written to disagree.

printf 'aaa\nab\nabab\na\n\nbbb\n' > "$work/m"
printf '  leading blanks\n\ttab first\nx  y   z\n' > "$work/n"
printf '0005\n5\n5.10\n5.9\n-0\n+7\n007\nnotanumber\n' > "$work/o"
printf 'a,b,,c\n,x,y\nsingle\n' > "$work/p"
printf 'AAA\naaa\nBBB\nbbb\nAAA\n' > "$work/q"
printf 'field1 field2 field3\nz1 a2 m3\nz1 b2 a3\n' > "$work/r"
printf 'one\ntwo\nthree\n' > "$work/s"
printf 'x\ty\tz\n' > "$work/t"
printf 'The quick brown fox jumps over the lazy dog again and again today\n' > "$work/u"
printf '3\n1K\n2M\n500\n-1G\n2\n900K\n' > "$work/v"
printf 'a\nc\ne\n' > "$work/w"
printf '1.10\n1.9\n1.2.3\nfoo-1.0.tar.gz\nfoo-1.0~rc1\nfoo-2.tar.gz\n.hidden\n' > "$work/y"
printf 'Mar\nJAN\nfeb\nnotamonth\nDec 3\n' > "$work/z"
printf 'b\nd\nf\n' > "$work/x"

#       A directory to walk. Both tools read it with the same getdents on the
#       same filesystem, so the order they print is the same order; nothing
#       here sorts and neither does GNU.

mkdir -p "$work/tree/inner" "$work/tree/other" "$work/bare"
printf 'alpha here\nbeta here\n' > "$work/tree/one.txt"
printf 'alpha again\n' > "$work/tree/two.log"
printf 'gamma only\n' > "$work/tree/three.txt"
printf 'alpha inner\n' > "$work/tree/inner/deep.txt"
printf 'alpha other\n' > "$work/tree/other/far.log"
ln -s one.txt "$work/tree/link.txt"

case_start grepr
compare 'recursive'      grep -  -r alpha "$work/tree"
compare 'recursive long' grep -  --recursive alpha "$work/tree"
compare 'recursive links' grep - -R alpha "$work/tree"
compare 'recursive names' grep - -rl alpha "$work/tree"
compare 'recursive without' grep - -rL alpha "$work/tree"
compare 'recursive count' grep - -rc alpha "$work/tree"
compare 'recursive numbered' grep - -rn alpha "$work/tree"
compare 'recursive no names' grep - -rh alpha "$work/tree"
compare 'recursive one file' grep - -r alpha "$work/tree/one.txt"
compare 'recursive empty'  grep - -r alpha "$work/bare"
compare 'recursive missing' grep - -r alpha "$work/nosuchdir"
compare 'recursive quiet miss' grep - -rs alpha "$work/nosuchdir"
compare 'recursive two'    grep - -rl alpha "$work/tree" "$work/bare"
compare 'directories recurse' grep - -d recurse alpha "$work/tree"
compare 'directories skip' grep -  -d skip alpha "$work/tree"
compare 'plain directory' grep -   alpha "$work/tree"
compare 'include suffix'  grep -  -rl --include='*.txt' alpha "$work/tree"
compare 'include two'     grep -  -rl --include='*.txt' --include='*.log' alpha "$work/tree"
compare 'include one letter' grep - -rl --include='?.txt' alpha "$work/tree"
compare 'include set'     grep -  -rl --include='[ot]*' alpha "$work/tree"
compare 'include nothing' grep -  -rl --include='*.zzz' alpha "$work/tree"
compare 'exclude suffix'  grep -  -rl --exclude='*.log' alpha "$work/tree"
compare 'exclude all'     grep -  -r --exclude='*' alpha "$work/tree"
compare 'exclude names'   grep -  -rl --exclude-dir=inner alpha "$work/tree"
compare 'exclude names slash' grep - -rl --exclude-dir=inner/ alpha "$work/tree"
compare 'exclude two dirs' grep - -rl --exclude-dir=inner --exclude-dir=other alpha "$work/tree"
compare 'include and exclude' grep - -rl --include='*.txt' --exclude='three*' alpha "$work/tree"
compare 'include on named'  grep - -l --include='*.log' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'include names one' grep - --include='*.log' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'exclude names one' grep - --exclude='*.txt' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'include counts one' grep - -c --include='*.log' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'include names none' grep - --include='*.zzz' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'exclude on named'  grep - -l --exclude='*.txt' alpha "$work/tree/one.txt" "$work/tree/two.log"
compare 'colour never'    grep -  --color=never alpha "$work/tree/one.txt"
compare 'colour auto'     grep -  --color=auto alpha "$work/tree/one.txt"
compare 'colour bare'     grep -  --color alpha "$work/tree/one.txt"
compare 'colour british'  grep -  --colour=never alpha "$work/tree/one.txt"

case_start grep2
compare 'nested star'    grep m  '\(ab\)*'
compare 'group star'     grep m  '^\(ab\)*$'
compare 'empty star'     grep m  '^a*$'
compare 'dot anchor'     grep m  '^.$'
compare 'star of dot'    grep m  '^.*b$'
compare 'escaped dot'    grep p  '\.'
compare 'literal dot'    grep b  'one\.two'
compare 'bracket rbrack' grep p  '[],]'
compare 'bracket dash'   grep p  '[a-]'
compare 'bracket caret'  grep p  '[^abc]'
compare 'interval exact' grep m  '^a\{3\}$'
compare 'interval range' grep m  '^a\{1,2\}$'
compare 'interval open'  grep m  '^a\{2,\}$'
compare 'ere interval'   grep -  -E '^(ab){2}$' "$work/m"
compare 'ere group opt'  grep -  -E '^(ab)?a$' "$work/m"
compare 'ere nested'     grep -  -E '^((a|b)+)$' "$work/m"
compare 'ere alt anchor' grep -  -E '^(aaa|bbb)$' "$work/m"
compare 'ere dollar alt' grep -  -E 'a$|b$' "$work/m"
compare 'backref twice'  grep m  '\(a\)\1'
compare 'case class'     grep q  -i '[a-b]\{3\}'
compare 'count invert'   grep m  -cv a
compare 'line num inv'   grep m  -nv a
compare 'multiple e'     grep m  -e aaa -e bbb
compare 'pattern star1'  grep m  '*a'
compare 'caret middle'   grep m  'a^b'
compare 'dollar middle'  grep m  'a$b'
compare 'blank line'     grep m  '^$'
compare 'not blank'      grep m  -v '^$'
compare 'word only'      grep u  -w the
compare 'word case'      grep u  -iw the

case_start sed2
compare 'star empty g'   sed m  's/a*/X/g'
compare 'anchor empty g' sed m  's/^/> /'
compare 'end append'     sed m  's/$/ </'
compare 'group swap'     sed r  's/\([a-z]*\)\([0-9]\)/\2\1/'
compare 'nested group'   sed r  's/\(\([a-z]\)[0-9]\)/[\1|\2]/'
compare 'saved loop programs' sed m -n '/^a*a*a*$/p;/^\(ab\)*$/p'
compare 'loop overflow state' sed m -n '/^a*a*a*a*a*a*a*a*a*$/p'
compare 'amp escape'     sed s  's/one/\&/'
compare 'newline in rep' sed s  's/one/a\nb/'
compare 'tab in rep'     sed s  's/one/a\tb/'
compare 'backslash rep'  sed s  's/one/a\\b/'
compare 'range to end'   sed i  -n '10,$p'
compare 'range one line' sed i  -n '5,3p'
compare 'regex to num'   sed i  -n '/3/,5p'
compare 'step of range'  sed i  '2,4s/^/> /'
compare 'negate range'   sed i  -n '2,4!p'
compare 'block many'     sed i  -n '3,5{s/^/> /;p}'
compare 'quit after sub' sed i  '3{s/^/> /;q}'
compare 'last only'      sed i  -n '$='
compare 'delete last'    sed i  '$d'
compare 'y with escape'  sed t  'y/\t/ /'
compare 'sub then sub'   sed s  's/one/two/;s/two/three/'
compare 'global anchor'  sed u  's/a/A/g'
compare 'no match keep'  sed s  's/zzz/X/'
compare 'print doubles'  sed s  'p'
compare 'N join'         sed s  'N;s/\n/+/'
compare 'P first of space' sed a -n 'N;P'
compare 'P without joined line' sed h -n 'P'
compare 'D restarts space' sed a -n 'N;P;D'
compare 'D without joined line' sed h -n 'D'
compare 'multiple files' sed -  -n '$p' "$work/s" "$work/m"
compare 'char class rep' sed n  's/[[:blank:]]\+/ /g'
compare 'leading blanks' sed n  's/^[ \t]*//'

#       Address forms GNU added to the ones POSIX has: a step, a range that
#       ends a count of lines later or at the next line a number divides, and
#       line zero, which is not a line and is only an address at all as the
#       open end of a range a pattern closes.

case_start sedaddr
compare 'step from one'  sed i  -n '1~2p'
compare 'step from zero' sed i  -n '0~3p'
compare 'step of one'    sed i  -n '3~1p'
compare 'step of none'   sed i  -n '4~0p'
compare 'step past end'  sed i  -n '99~2p'
compare 'step of none from zero' sed i -n '0~0p'
compare 'step deletes'   sed i  '1~4d'
compare 'step negated'   sed i  -n '1~2!p'
compare 'ahead two'      sed i  -n '3,+2p'
compare 'ahead none'     sed i  -n '3,+0p'
compare 'ahead past end' sed i  -n '14,+9p'
compare 'ahead from match' sed a -n '/delta/,+1p'
compare 'multiple three' sed i  -n '2,~3p'
compare 'multiple on it' sed i  -n '3,~3p'
compare 'multiple of one' sed i -n '2,~1p'
compare 'multiple of none' sed i -n '2,~0p'
compare 'zero to match'  sed i  -n '0,/1/p'
compare 'zero to later'  sed i  -n '0,/3/p'
compare 'zero to nothing' sed i -n '0,/nope/p'
compare 'zero deletes'   sed a  '0,/alpha/d'
compare 'one deletes'    sed a  '1,/alpha/d'
compare 'zero negated'   sed i  -n '0,/2/!p'
compare 'zero alone'     sed i  -n '0p'
compare 'zero to line'   sed i  -n '0,5p'
compare 'zero two files' sed -  -n '0,/alpha/p' "$work/a" "$work/e"
compare 'zero separate'  sed -  -s -n '0,/a/p' "$work/a" "$work/e"
compare 'quit silently'  sed i  'Q'
compare 'quit at line'   sed i  '3Q'
compare 'quit with status' sed i '3Q5'
compare 'quit after print' sed i -n '2{p;Q}'
compare 'name of input'  sed -  -n 'F' "$work/a"
compare 'name of stdin'  sed a  -n 'F'
compare 'name once'      sed -  -n '1F' "$work/a" "$work/b"

#       Where sed writes other than to standard output, what it reads in the
#       middle of a cycle, and the jump table: a label is an index into the
#       same flat array of commands, which is what makes b and t nothing more
#       than an assignment to the counter.

case_start sedfiles
compare_side 'write every line' sed a  'w SIDE' -n
compare_side 'write and print'  sed a  'w SIDE'
compare_side 'write one line'   sed a  '2w SIDE' -n
compare_side 'write last'       sed a  '$w SIDE' -n
compare_side 'write matched'    sed a  '/alpha/w SIDE' -n
compare_side 'write twice'      sed a  '1w SIDE
2w SIDE' -n
compare_side 'substitute wrote' sed a  's/a/X/w SIDE' -n
compare_side 'substitute all'   sed a  's/[ae]/X/gw SIDE' -n
compare_side 'substitute none'  sed a  's/zzz/X/w SIDE' -n
compare_side 'write unended'    sed h  'w SIDE' -n
compare_side 'write zero ends'  sed zeros 'w SIDE' -nz
compare_side 'write two files'  sed a  's/a/X/w SIDE' -n -e 's/b/Y/'
compare 'write to stdout' sed a -n 'w /dev/stdout'
compare 'substitute to stdout' sed a -n 's/alpha/X/w /dev/stdout'
compare 'read after one'  sed -  '1r '"$work/b" "$work/a"
compare 'read after last' sed -  '$r '"$work/b" "$work/a"
compare 'read after match' sed - '/delta/r '"$work/b" "$work/a"
compare 'read every line' sed -  'r '"$work/b" "$work/a"
compare 'read quiet'      sed -  -n '1r '"$work/b" "$work/a"
compare 'read missing'    sed -  '1r '"$work/nosuchfile" "$work/a"
compare 'read and append' sed -  '1{r '"$work/b"'
a APPENDED
}' "$work/a"
compare 'two appends'     sed a  '1a one
1a two'
compare 'append then read' sed - '1{a one
r '"$work/b"'
a two
}' "$work/a"

case_start sedjump
compare 'join every line' sed a  ':a;N;$!ba;s/\n/,/g'
compare 'branch forward'  sed a  's/alpha/X/;t done;s/beta/Y/;:done'
compare 'branch not taken' sed a 's/zzz/X/;t done;s/beta/Y/;:done'
compare 'branch when not'  sed a 's/alpha/X/;T done;s/beta/Y/;:done'
compare 'branch to the end' sed a 'b'
compare 'branch at a line' sed a  '2b
s/./X/'
compare 'branch in a block' sed a -n '/alpha/{:l;p;b};p'
compare 'loop until last'  sed a  ':a
$!{N;ba
}
s/\n/-/g'
compare 'print each looping' sed a -n ':x;p;$!{n;bx}'
compare 'label with no name' sed a ':'
compare 'label not there'  sed a  'b nowhere'
compare 'branch back once'  sed a 's/a/X/;ta;b;:a;s/$/!/'

case_start tee2
compare 'append flag'    tee a  -a /dev/null
compare 'ignore signals' tee a  -i /dev/null
compare 'pipe errors'    tee a  -p /dev/null
compare 'long append'    tee a  --append /dev/null
compare 'long ignore'    tee a  --ignore-interrupts /dev/null
compare 'long errors'    tee a  --output-error /dev/null
compare 'long errors when' tee a --output-error=warn /dev/null
compare 'letters together' tee a -ai /dev/null
compare 'unknown letter' tee a  -Q /dev/null
compare 'end of options' tee a  -- /dev/null
compare 'no files'       tee a

case_start cut2
compare 'comma delim'    cut p  -d, -f2
compare 'empty fields'   cut p  -d, -f3
compare 'out of order'   cut p  -d, -f3,1
compare 'repeat field'   cut p  -d, -f2,2
compare 'char past end'  cut s  -c1-100
compare 'char single'    cut s  -c2
compare 'field all'      cut p  -d, -f1-
compare 'space delim'    cut u  '-d ' -f2,4

case_start tr2
compare 'octal'          tr s  '\157' O
compare 'octal cap'      tr s  '\1570' X
compare 'octal breaker'  tr s  '\157q' X
compare 'repeat set'     tr s  'one' '[X*]'
compare 'repeat clips safely' tr s 'one' '[X*2048][Y*]'
compare 'range high bytes' tr tr_high '\376-\377' XY
compare 'complement sub' tr s  -c 'o\n' X
compare 'squeeze all'    tr q  -s '[:upper:]'
compare 'delete class'   tr n  -d '[:blank:]'
compare 'upper to lower' tr q  '[:upper:]' '[:lower:]'
compare 'longer set two' tr s  ab abcdef
compare 'newline delete' tr s  -d '\n'

case_start sort2
compare 'numeric mixed'  sort o  -n
compare 'numeric rev'    sort o  -nr
compare 'numeric unique' sort o  -nu
compare 'fold unique'    sort q  -fu
compare 'stable keys'    sort r  -k1,1
compare 'key char off'   sort r  -k1.2
compare 'key two fields' sort r  -k2,3
compare 'key numeric b'  sort n  -k2b
compare 'blank lines'    sort k
compare 'reverse unique' sort q  -ru
compare 'tab separator'  sort t  -t'	' -k2
compare 'key past end'   sort r  -k9
compare 'whole then key' sort r  -k2 -k1
compare 'check sorted'   sort s  -c
compare 'check unsorted' sort a  -c
compare 'check quiet'    sort a  -C
compare 'check quiet ok' sort s  -C
compare 'check unique'   sort e  -cu
compare 'check named'    sort -  -c "$work/a"
compare 'merge one'      sort w  -m
compare 'merge two'      sort -  -m "$work/w" "$work/x"
compare 'merge unsorted' sort -  -m "$work/a" "$work/x"
compare 'merge unique'   sort -  -mu "$work/w" "$work/w"
compare 'output file'    sort a  -o /dev/stdout
compare 'ignore unprintable' sort n -i
compare 'dictionary'     sort n  -d
compare 'human'          sort v  -h
compare 'human reverse'  sort v  -hr
compare 'human key'      sort d  -k2h
compare 'buffer size'    sort a  -S 2
compare 'temp directory' sort a  -T /tmp
compare 'long numeric'   sort c  --numeric-sort
compare 'long reverse'   sort a  --reverse
compare 'long unique'    sort e  --unique
compare 'long fold'      sort f  --ignore-case
compare 'long blanks'    sort n  --ignore-leading-blanks
compare 'long unprint'   sort n  --ignore-nonprinting
compare 'long dict'      sort n  --dictionary-order
compare 'long stable'    sort d  --stable -k1,1
compare 'long check'     sort a  --check
compare 'long merge'     sort w  --merge
compare 'long key'       sort d  --key 2
compare 'long separator' sort b  --field-separator=: --key 2
compare 'long human'     sort v  --human-numeric-sort
compare 'long buffer'    sort a  --buffer-size 2
compare 'long batch'     sort a  --batch-size 2
compare 'sort word'      sort c  --sort=numeric
compare 'long unknown'   sort a  --nosuchflag
compare 'unknown letter' sort a  -Q
compare 'version'        sort y  -V
compare 'version rev'    sort y  -Vr
compare 'version unique' sort y  -Vu
compare 'long version'   sort y  --version-sort
compare 'month'          sort z  -M
compare 'long month'     sort z  --month-sort
compare 'sort word month' sort z --sort=month
compare 'sort word version' sort y --sort=version
compare 'key version'    sort d  -k1,1V
compare 'key month'      sort z  -k1,1M
compare 'check quiet arg' sort a --check=quiet
compare 'check first arg' sort a --check=diagnose-first
compare 'null data'      sort zeros -z
compare 'null data unique' sort zeros -z -u
compare 'null data long' sort zeros --zero-terminated

case_start uniq2
compare 'all same'       uniq q  -c
compare 'no repeats'     uniq s  -c
compare 'd on unique'    uniq s  -d
compare 'u on all dup'   uniq q  -u
compare 'skip two'       uniq r  -f2
compare 'width one'      uniq q  -w1
compare 'ignore case c'  uniq q  -ic

case_start format
compare 'wc pipe wide'   wc a
compare 'wc lines pipe'  wc i  -l
compare 'wc all flags'   wc a  -lwc
compare 'wc chars named' wc -  -m "$work/a"
compare 'nl no number'   nl k  -bn
compare 'nl left'        nl a  -nln
compare 'nl zeros'       nl a  -nrz
compare 'nl width one'   nl a  -w1
compare 'fold long'      fold u  -w 20
compare 'fold spaces'    fold u  -w 20 -s
compare 'fold tabs'      fold g  -w 6
compare 'head bytes big' head a  -c 1000
compare 'tail bytes big' tail a  -c 1000
compare 'tail plus one'  tail i  -n +1
compare 'rev tabs'       rev t


#       A third batch: empty inputs, missing files, long lines, and the
#       places where a backtracking machine is expected to be slow or wrong.

: > "$work/empty"
awk 'BEGIN { for (i = 0; i < 4000; i++) printf "a"; printf "\n" }' > "$work/long"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "%d line %d\n", (i * 7919) % 20000, i }' > "$work/big"
printf 'a b\tc  d\n' > "$work/w"
printf 'x\n' > "$work/one"

case_start empty
compare 'grep'           grep empty  a
compare 'sed'            sed empty   's/a/b/'
compare 'cut'            cut empty   -c1
compare 'tr'             tr empty    a b
compare 'sort'           sort empty
compare 'uniq'           uniq empty
compare 'head'           head empty
compare 'tail'           tail empty
compare 'wc'             wc empty
compare 'rev'            rev empty
compare 'nl'             nl empty
compare 'fold'           fold empty
compare 'sort unique'    sort empty  -u
compare 'uniq count'     uniq empty  -c
compare 'wc lines'       wc empty    -l

case_start missing
compare 'grep gone'      grep -  a "$work/nosuchfile"
compare 'wc gone'        wc -    "$work/nosuchfile"
compare 'head gone'      head -  "$work/nosuchfile"
compare 'grep one gone'  grep -  a "$work/a" "$work/nosuchfile"

case_start long
compare 'grep dot star'  grep long  '^a*$'
compare 'grep anchored'  grep long  'a\{4000\}'
compare 'grep tail'      grep long  'aaaa$'
compare 'sed whole'      sed long   's/a*/X/'
compare 'sed each'       sed long   's/a/b/g'
compare 'wc'             wc long
compare 'fold'           fold long  -w 100
compare 'rev'            rev long
compare 'cut'            cut long   -c3990-

# One byte beyond the shared line buffer. Before the explicit check these
# nine printed exactly one MiB, silently dropped the tail, and exited zero.
head -c 1048577 /dev/zero | tr '\0' x > "$work/overlong"
printf '\n' >> "$work/overlong"
head -c 17000 /dev/zero | tr '\0' x > "$work/grep_long_pattern"
printf '\n' >> "$work/grep_long_pattern"
head -c 16383 /dev/zero | tr '\0' x > "$work/grep_long_subject"
printf 'Y\n' >> "$work/grep_long_subject"
head -c 600000 /dev/zero | tr '\0' a > "$work/grep_large_context"
printf '\n' >> "$work/grep_large_context"
head -c 600000 /dev/zero | tr '\0' b >> "$work/grep_large_context"
printf '\nhit\n' >> "$work/grep_large_context"
awk 'BEGIN {
        for (line = 0; line < 256; line++) {
                printf "#";
                for (i = 0; i < 62; i++) printf "x";
                printf "\n";
        }
        print "s/x/y/";
}' > "$work/sed_long_script"
awk 'BEGIN { for (i = 1; i <= 40; i++) print "1a line" i }' \
        > "$work/sed_many_appends"

case_start ceiling
refuses_long_record 'grep record' grep x
refuses_long_record 'sed record'  sed 's/x/y/'
refuses_long_record 'cut record'  cut -c1-
refuses_long_record 'sort record' sort
refuses_long_record 'uniq record' uniq
refuses_long_record 'head record' head -n1
refuses_long_record 'rev record'  rev
refuses_long_record 'nl record'   nl
refuses_long_record 'fold record' fold -w 2000000
refuses_long_pipe   'tail pipe record' tail -n1
refuses_long_pattern
refuses_grep_context 'grep context byte ceiling' grep_large_context -B2 hit
refuses_grep_context 'grep context slot ceiling' one -B8193 x
refuses_long_sed_script
refuses_many_grep_globs
compare 'sed forty appends' sed - -f "$work/sed_many_appends" "$work/one"

case_start readerr
compare 'cat directory'  cat  - "$work/read_dir"
compare 'wc directory'   wc   - "$work/read_dir"
compare 'head directory' head - "$work/read_dir"
compare 'tail directory' tail - "$work/read_dir"
compare 'rev directory'  rev  - "$work/read_dir"
compare 'nl directory'   nl   - "$work/read_dir"
compare 'fold directory' fold - "$work/read_dir"
compare 'cut directory'  cut  - -c1 "$work/read_dir"
compare 'uniq directory' uniq - "$work/read_dir"
compare 'sort directory' sort - "$work/read_dir"

case_start big
compare 'sort'           sort big
compare 'sort numeric'   sort big   -n
compare 'sort key'       sort big   -k3n
compare 'sort unique'    sort big   -u
compare 'uniq'           uniq big
compare 'wc'             wc big
compare 'grep count'     grep big   -c '^1'
compare 'head'           head big   -n 5
compare 'tail'           tail big   -n 5
compare 'sed'            sed big    -n '19999p'


#       The literal path, and the block skip in front of it.
#
#       A pattern that is nothing but characters is answered out of
#       memory_search rather than the machine, and whole read blocks that
#       cannot hold it are stepped over without ever being cut into lines.
#       What that must not do is lose the second pattern of a -e list, fold
#       case it was not asked to fold, or walk past a needle lying across the
#       end of a block. All three are invisible on a file smaller than one
#       read, which is what every other grep case here is.

awk 'BEGIN { for (i = 0; i < 20000; i++) printf "%s %d\n", (i % 500 == 0 ? "needle" : "filler"), i }' > "$work/sparse"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "%s line %d\n", (i % 3 == 0 ? "FOX" : (i % 3 == 1 ? "Fox" : "fox")), i }' > "$work/mixed"
printf '%s' "$(cat "$work/sparse")" > "$work/nonl"
printf 'needle\nzzz\n' > "$work/plist"

#       Two options that answer the same question, in both orders.
#
#       GNU takes the last one: head -n 2 -c 5 is five bytes and head -c 5 -n 2
#       is two lines, from the same pair of flags. A parser that keeps one
#       value per letter has no way to say which arrived last, so this is where
#       that shows if it is going to.

case_start ordering
compare 'head lines then bytes' head i  -n 2 -c 5
compare 'head bytes then lines' head i  -c 5 -n 2
compare 'head bytes then bytes' head i  -c 9 -c 3
compare 'head lines then lines' head i  -n 9 -n 3
compare 'tail lines then bytes' tail i  -n 2 -c 3
compare 'tail bytes then lines' tail i  -c 3 -n 2
compare 'tail bytes then bytes' tail i  -c 9 -c 3
compare 'sort numeric then human' sort c -n -h
compare 'sort human then numeric' sort c -h -n
compare 'sort numeric then version' sort c -n -V
compare 'sort version then numeric' sort c -V -n
compare 'grep after then context' grep i -A1 -C2 5
compare 'grep context then after' grep i -C2 -A1 5
compare 'cut chars then fields' cut b  -c1 -f1
compare 'cut fields then chars' cut b  -f1 -c1
compare 'cut fields twice'      cut b  -d: -f1 -f3
compare 'uniq skip twice'       uniq d -f1 -f2
compare 'fold width twice'      fold long -w 20 -w 60
compare 'nl width twice'        nl i   -w 3 -w 6

case_start grepblock
compare 'count'          grep sparse  -c needle
compare 'count miss'     grep sparse  -c zzzz
compare 'numbered'       grep sparse  -n needle
compare 'byte offset'    grep sparse  -b needle
compare 'two patterns'   grep sparse  -c -e needle -e filler
compare 'second is rare' grep sparse  -c -e zzzz -e needle
compare 'pattern file'   grep sparse  -c -f "$work/plist"
compare 'word'           grep sparse  -c -w needle
compare 'whole line'     grep sparse  -c -x 'needle 0'
compare 'fixed'          grep sparse  -c -F needle
compare 'inverted'       grep sparse  -c -v needle
compare 'context'        grep sparse  -A2 -B1 needle
compare 'files listed'   grep -       -l needle "$work/sparse" "$work/mixed"
compare 'mixed folded'   grep mixed   -c -i FOX
compare 'mixed exact'    grep mixed   -c FOX
compare 'mixed word'     grep mixed   -c -iw fox
compare 'no last newline' grep nonl   -c needle
compare 'no last newline n' grep nonl -n needle
compare 'sed on blocks'  sed sparse   -n '/needle/p'
compare 'nl on blocks'   nl sparse    -b 'p^needle'

#       The needle moved a byte at a time across the end of the first read.
across=65530
while [ $across -le 65541 ]; do
        awk -v want=$across 'BEGIN {
                n = 0
                while (n + 9 <= want) { printf "abcdefgh\n"; n += 9 }
                while (n < want) { printf "z"; n++ }
                printf "needle\n"
        }' > "$work/across"

        compare "across $across" grep across -n needle
        across=$((across + 1))
done

case_start edges
compare 'cut mixed sep'  cut w   '-d ' -f2
compare 'cut tab sep'    cut w   -f2
compare 'cut byte above old ceiling' cut cut_wide -c5000
compare 'cut range above old ceiling' cut cut_wide -c4999-5001
compare 'cut separated high bytes' cut cut_wide -c4999,5001 -OX
compare 'sort one line'  sort one
compare 'uniq one line'  uniq one -c
compare 'nl one line'    nl one
compare 'tail one'       tail one -n 5
compare 'head zero c'    head one -c 0
compare 'grep both'      grep one -c x
compare 'tr no set two'  tr one  -d x
compare 'fold width 1'   fold one -w 1
compare 'sed s empty re' sed one 's///'
compare 'sed multiple !' sed i   -n '3!!p'
compare 'wc four flags'  wc a    -lwmc
compare 'sort t missing' sort b  -t: -k3
compare 'sort t empty'   sort b  -t '' -k1
compare 'sort t two'     sort b  -t '::' -k1
compare 'sort t tab word' sort g -t '\t' -k2
compare 'sort t null'    sort zpairs -t '\0' -k1
compare 'sort t backslash' sort b -t '\' -k1
compare 'sort key unknown modifier' sort a -k1Q
compare 'sort key missing end field' sort a -k1,
compare 'sort key missing first offset' sort a -k1.
compare 'sort key zero first offset' sort a -k1.0
compare 'sort key conflicting kinds' sort a -k1nV
compare 'sort conflicting global kinds' sort a -h --sort=numeric
refuses_sort_mode 'sort general numeric' -g
refuses_sort_mode 'sort long general numeric' --sort=general-numeric
refuses_sort_mode 'sort random' --sort=random
compare 'uniq f past'    uniq a  -f9
compare 'cut f zero pad' cut b   -d: -f1,1,1


#       Several files at once, which is where one arena shared between an
#       index, the lines and the key bounds either holds up or does not.

case_start manyfiles
compare 'tail three'     tail -  -n 2 "$work/a" "$work/b" "$work/e"
compare 'tail three all' tail -  -n 100 "$work/s" "$work/one" "$work/k"
compare 'tail bytes two' tail -  -c 5 "$work/a" "$work/b"
compare 'tail bytes q'   tail -  -q -c 5 "$work/a" "$work/b"
compare 'head three'     head -  -n 2 "$work/a" "$work/b" "$work/e"
compare 'head bytes two' head -  -c 5 "$work/a" "$work/b"
compare 'sort two'       sort -  "$work/a" "$work/e"
compare 'sort key two'   sort -  -k2 "$work/d" "$work/r"
compare 'sort numeric two' sort - -n "$work/c" "$work/o"
compare 'sort unique two' sort - -u "$work/e" "$work/q"
compare 'wc three'       wc -    "$work/a" "$work/b" "$work/e"
compare 'grep three'     grep -  -n a "$work/a" "$work/b" "$work/e"
compare 'nl two'         nl -    "$work/s" "$work/one"
compare 'rev two'        rev -   "$work/a" "$work/b"
compare 'cut two'        cut -   -d: -f2 "$work/b" "$work/p"
compare 'fold two'       fold -  -w 6 "$work/a" "$work/s"
compare 'tail empty mix' tail -  -n 2 "$work/empty" "$work/s"
compare 'sort empty mix' sort -  "$work/empty" "$work/s"

# cat, which for most of its life is a copy and for the rest of it is a walk.
compare 'cat'            cat a
compare 'cat number'     cat a   -n
compare 'cat number full' cat s  -b
compare 'cat ends'       cat a   -E
compare 'cat tabs'       cat d   -T
compare 'cat all'        cat d   -A
compare 'cat squeeze'    cat s   -s
compare 'cat number squeeze' cat s -ns
compare 'cat show ends'  cat a   -e
compare 'cat two'        cat -   "$work/a" "$work/b"
compare 'cat missing'    cat -   "$work/nosuch"
compare 'cat empty'      cat -   "$work/empty"
compare 'cat bad option' cat -   -Z "$work/a"
compare 'cat long number' cat a  --number
compare 'cat long nonblank' cat s --number-nonblank
compare 'cat long ends'  cat a   --show-ends
compare 'cat long tabs'  cat d   --show-tabs
compare 'cat long squeeze' cat s --squeeze-blank
compare 'cat long all'   cat d   --show-all
compare 'cat long nonprinting' cat d --show-nonprinting
compare 'cat long unknown' cat a --nosuchflag

#       xargs, which is standard input turned into arguments and a command
#       run with them -- so both halves of compare matter: what the command
#       printed, and the number xargs came back with when it went wrong.

printf 'a b c\n' > "$work/x1"
printf 'a\nb\nc\n' > "$work/x2"
printf 'a\0b\0' > "$work/x3"
printf "'a b' c\n" > "$work/x4"
printf 'a\\ b c\n' > "$work/x5"
printf '' > "$work/x6"
printf '   \n\n  \n' > "$work/x7"
printf '1 2 3 4 5 6 7 8 9 10 11 12\n' > "$work/x8"
printf 'a\nEND\nb\n' > "$work/x9"
printf '"a b" c\n' > "$work/x10"
printf 'a b' > "$work/x11"
printf '  a b  \n' > "$work/x12"
printf 'a\tb\n\tc\t\n' > "$work/x13"

case_start xargs
compare 'words'          xargs x1  echo
compare 'lines'          xargs x2  echo
compare 'tabs'           xargs x13 -n1 echo
compare 'no command'     xargs x1
compare 'one at a time'  xargs x1  -n1 echo
compare 'two at a time'  xargs x8  -n2 echo
compare 'five at a time' xargs x8  -n5 echo
compare 'joined number'  xargs x8  -n3 echo
compare 'with a prefix'  xargs x8  -n2 echo pre
compare 'zero ended'     xargs x3  -0 echo
compare 'zero one each'  xargs x3  -0 -n1 echo
compare 'single quotes'  xargs x4  -n1 echo
compare 'double quotes'  xargs x10 -n1 echo
compare 'a backslash'    xargs x5  -n1 echo
compare 'nothing at all' xargs x6  echo
compare 'blanks only'    xargs x7  echo
compare 'nothing refused' xargs x6 -r echo
compare 'blanks refused' xargs x7  -r echo
compare 'nothing to replace' xargs x6 -I{} echo x
compare 'nothing but a status' xargs x6 false
compare 'only the end word' xargs x9 -E a echo
compare 'no last newline' xargs x11 echo
compare 'replacing'      xargs x2  -I{} echo x{}y
compare 'replacing short' xargs x2 -i echo x{}y
compare 'replacing twice' xargs x2 -I{} echo {}-{}
compare 'replacing blanks' xargs x12 -I{} echo "[{}]"
compare 'replacing unused' xargs x2 -I{} echo plain
compare 'logical end'    xargs x9  -E END echo
compare 'end ignored'    xargs x9  -0 -E END echo
compare 'end of options' xargs x1  -- echo
compare 'an absolute path' xargs x1 /bin/echo
compare 'the command failed' xargs x1 false
compare 'each one failed' xargs x1 -n1 false
compare 'no such command' xargs x1 nosuchcommand12345
compare 'traced'         xargs x1  -t echo
compare 'an option it has not' xargs x1 -Q echo


#       xargs, which is standard input turned into arguments and a command
#       run with them -- so both halves of compare matter: what the command
#       printed, and the number xargs came back with when it went wrong.

printf 'a b c\n' > "$work/x1"
printf 'a\nb\nc\n' > "$work/x2"
printf 'a\0b\0' > "$work/x3"
printf "'a b' c\n" > "$work/x4"
printf 'a\\ b c\n' > "$work/x5"
printf '' > "$work/x6"
printf '   \n\n  \n' > "$work/x7"
printf '1 2 3 4 5 6 7 8 9 10 11 12\n' > "$work/x8"
printf 'a\nEND\nb\n' > "$work/x9"
printf '"a b" c\n' > "$work/x10"
printf 'a b' > "$work/x11"
printf '  a b  \n' > "$work/x12"
printf 'a\tb\n\tc\t\n' > "$work/x13"

case_start xargs
compare 'words'          xargs x1  echo
compare 'lines'          xargs x2  echo
compare 'tabs'           xargs x13 -n1 echo
compare 'no command'     xargs x1
compare 'one at a time'  xargs x1  -n1 echo
compare 'two at a time'  xargs x8  -n2 echo
compare 'five at a time' xargs x8  -n5 echo
compare 'joined number'  xargs x8  -n3 echo
compare 'with a prefix'  xargs x8  -n2 echo pre
compare 'zero ended'     xargs x3  -0 echo
compare 'zero one each'  xargs x3  -0 -n1 echo
compare 'single quotes'  xargs x4  -n1 echo
compare 'double quotes'  xargs x10 -n1 echo
compare 'a backslash'    xargs x5  -n1 echo
compare 'nothing at all' xargs x6  echo
compare 'blanks only'    xargs x7  echo
compare 'nothing refused' xargs x6 -r echo
compare 'blanks refused' xargs x7  -r echo
compare 'nothing to replace' xargs x6 -I{} echo x
compare 'nothing but a status' xargs x6 false
compare 'only the end word' xargs x9 -E a echo
compare 'no last newline' xargs x11 echo
compare 'replacing'      xargs x2  -I{} echo x{}y
compare 'replacing short' xargs x2 -i echo x{}y
compare 'replacing twice' xargs x2 -I{} echo {}-{}
compare 'replacing blanks' xargs x12 -I{} echo "[{}]"
compare 'replacing unused' xargs x2 -I{} echo plain
compare 'logical end'    xargs x9  -E END echo
compare 'end ignored'    xargs x9  -0 -E END echo
compare 'end of options' xargs x1  -- echo
compare 'an absolute path' xargs x1 /bin/echo
compare 'the command failed' xargs x1 false
compare 'each one failed' xargs x1 -n1 false
compare 'no such command' xargs x1 nosuchcommand12345
compare 'traced'         xargs x1  -t echo
compare 'an option it has not' xargs x1 -Q echo


#       cmp, whose answer is often nothing at all -- so the exit status is
#       most of what there is to compare, and compare looks at both.

i=0
while [ $i -lt 30 ]; do printf '0123456789'; i=$((i + 1)); done > "$work/cl1"
tr 5 x < "$work/cl1" > "$work/cl2"
head -c 250 "$work/cl1" > "$work/cl3"
cp "$work/a" "$work/a2"
sed 's/gamma/gammX/' "$work/a" > "$work/a3"
head -c 12 "$work/a" > "$work/a4"
i=0
while [ $i -lt 7000 ]; do printf '0123456789\n'; i=$((i + 1)); done > "$work/cmp_block1"
cp "$work/cmp_block1" "$work/cmp_block2"
sed '6000s/5/X/' "$work/cmp_block1" > "$work/cmp_block3"
head -c 65536 "$work/cmp_block1" > "$work/cmp_block4"

case_start cmp
compare 'same'           cmp -  "$work/a" "$work/a2"
compare 'same across blocks' cmp - "$work/cmp_block1" "$work/cmp_block2"
compare 'late block difference' cmp - "$work/cmp_block1" "$work/cmp_block3"
compare 'prefix at block edge' cmp - "$work/cmp_block1" "$work/cmp_block4"
compare 'limit inside equal block' cmp - -n 65000 "$work/cmp_block1" "$work/cmp_block3"
compare 'differ'         cmp -  "$work/a" "$work/a3"
compare 'differ other way' cmp - "$work/a3" "$work/a"
compare 'prefix'         cmp -  "$work/a" "$work/a4"
compare 'prefix first'   cmp -  "$work/a4" "$work/a"
compare 'empty against'  cmp -  "$work/a" "$work/empty"
compare 'both empty'     cmp -  "$work/empty" "$work/empty"
compare 'silent same'    cmp -  -s "$work/a" "$work/a2"
compare 'silent differ'  cmp -  -s "$work/a" "$work/a3"
compare 'silent prefix'  cmp -  -s "$work/a" "$work/a4"
compare 'listed'         cmp -  -l "$work/a" "$work/a3"
compare 'listed columns' cmp -  -l "$work/cl1" "$work/cl2"
compare 'listed prefix'  cmp -  -l "$work/cl1" "$work/cl3"
compare 'listed same'    cmp -  -l "$work/cl1" "$work/cl1"
compare 'no newline'     cmp -  "$work/h" "$work/a"
compare 'missing file'   cmp -  "$work/a" "$work/nosuch"
compare 'both missing'   cmp -  "$work/nosuch" "$work/nosuch2"
compare 'end of options' cmp -  -- "$work/a" "$work/a3"
compare 'standard input' cmp a  "$work/a"
compare 'named as dash'  cmp a  "$work/a" -
compare 'dash is first'  cmp a  - "$work/a3"
compare 'no operands'    cmp -
compare 'three operands' cmp -  "$work/a" "$work/a" "$work/a"

#       -b puts the differing bytes beside their octal, -i and the operands
#       after the names say where to start, and -n says how far to go.
compare 'print bytes'    cmp -  -b "$work/cl1" "$work/cl2"
compare 'print bytes listed' cmp - -bl "$work/cl1" "$work/cl2"
compare 'print bytes other order' cmp - -lb "$work/cl1" "$work/cl2"
compare 'print bytes odd'  cmp - -b "$work/wide" "$work/spaced"
compare 'print bytes listed odd' cmp - -bl "$work/wide" "$work/spaced"
compare 'print bytes prefix' cmp - -b "$work/cl1" "$work/cl3"
compare 'print bytes same' cmp - -b "$work/cl1" "$work/cl1"
compare 'skip both'      cmp -  -i 5 "$work/cl1" "$work/cl2"
compare 'skip each'      cmp -  -i 5:7 "$work/cl1" "$work/cl2"
compare 'skip nothing'   cmp -  -i 0:0 "$work/cl1" "$work/cl2"
compare 'skip past end'  cmp -  -i 9999 "$work/cl1" "$work/cl3"
compare 'skip listed'    cmp -  -l -i 5 "$work/cl1" "$work/cl2"
compare 'skip is not a number' cmp - -i zz "$work/cl1" "$work/cl2"
compare 'limit short'    cmp -  -n 3 "$work/cl1" "$work/cl2"
compare 'limit none'     cmp -  -n 0 "$work/cl1" "$work/cl2"
compare 'limit long'     cmp -  -n 9999 "$work/cl1" "$work/cl2"
compare 'limit listed'   cmp -  -l -n 60 "$work/cl1" "$work/cl2"
compare 'limit suffix'   cmp -  -n 1K "$work/cl1" "$work/cl2"
compare 'limit thousand' cmp -  -n 1kB "$work/cl1" "$work/cl2"
compare 'limit is not a number' cmp - -n zz "$work/cl1" "$work/cl2"
compare 'skip one operand' cmp - "$work/cl1" "$work/cl2" 5
compare 'skip two operands' cmp - "$work/cl1" "$work/cl2" 5 7
compare 'skip zero operands' cmp - "$work/cl1" "$work/cl2" 0 0
compare 'five operands'  cmp -  "$work/cl1" "$work/cl2" 1 2 3
compare 'long print bytes' cmp - --print-bytes "$work/cl1" "$work/cl2"
compare 'long ignore initial' cmp - --ignore-initial=5 "$work/cl1" "$work/cl2"
compare 'long ignore each' cmp - --ignore-initial=5:7 "$work/cl1" "$work/cl2"
compare 'long bytes'     cmp -  --bytes=3 "$work/cl1" "$work/cl2"
compare 'long quiet'     cmp -  --quiet "$work/cl1" "$work/cl2"
compare 'long silent'    cmp -  --silent "$work/cl1" "$work/cl2"
compare 'long verbose'   cmp -  --verbose "$work/cl1" "$work/cl2"
compare 'skip and silent' cmp - -s -i 5 "$work/cl1" "$work/cl2"
compare 'skip prefix'    cmp -  -i 3 "$work/h" "$work/a"

#       expr, whose answer is on standard output and whose verdict is the exit
#       status, so both halves of compare matter here.

case_start expr
compare 'add'            expr -  1 + 1
compare 'subtract'       expr -  5 - 9
compare 'multiply'       expr -  3 '*' 4
compare 'divide'         expr -  7 / 2
compare 'divide negative' expr - -3 / 2
compare 'remainder'      expr -  7 % 3
compare 'precedence'     expr -  2 + 3 '*' 4
compare 'left to right'  expr -  10 - 3 - 2
compare 'parentheses'    expr -  '(' 1 + 2 ')' '*' 3
compare 'zero is false'  expr -  0
compare 'padded zero'    expr -  00
compare 'empty is false' expr -  ''
compare 'string is true' expr -  abc
compare 'not a number'   expr -  0.0
compare 'equal'          expr -  1 = 1
compare 'unequal'        expr -  1 = 2
compare 'strings equal'  expr -  abc = abc
compare 'string order'   expr -  abc '<' abd
compare 'number order'   expr -  3 '<' 10
compare 'number not text' expr - 10 '>' 9
compare 'differs'        expr -  1 != 2
compare 'compare chain'  expr -  1 = 1 = 1
compare 'or takes first' expr -  abc '|' 0
compare 'or takes second' expr - '' '|' abc
compare 'or has neither' expr -  0 '|' 0
compare 'and takes first' expr - abc '&' def
compare 'and wants both' expr -  abc '&' ''
compare 'and wants first' expr - '' '&' abc
compare 'or is lazy'     expr -  1 '|' 1 / 0
compare 'and is lazy'    expr -  0 '&' 1 / 0
compare 'match counts'   expr -  abc : 'a.c'
compare 'match anchored' expr -  abc : 'b'
compare 'match start'    expr -  abc : '^a'
compare 'match star'     expr -  aab : 'a*'
compare 'match nothing'  expr -  bbb : 'a*'
compare 'match end'      expr -  abc : 'abc$'
compare 'match group'    expr -  abcdef : 'abc\(d\)e'
compare 'group missing'  expr -  abc : '\(b\)'
compare 'match keyword'  expr -  match abc 'a\(b\)'
compare 'match keyword none' expr - match abc x
compare 'length'         expr -  length abcde
compare 'length empty'   expr -  length ''
compare 'substr'         expr -  substr abcde 2 3
compare 'substr from nought' expr - substr abcde 0 2
compare 'substr past end' expr - substr abcde 2 100
compare 'substr beyond'  expr -  substr abcde 9 2
compare 'index'          expr -  index abcde cd
compare 'index missing'  expr -  index abcde xyz
compare 'divide by zero' expr -  5 / 0
compare 'modulo by zero' expr -  5 % 0
compare 'not an integer' expr -  foo + 1
compare 'blank is not'   expr -  ' 1' + 1
compare 'missing operand' expr - 1 +
compare 'unclosed'       expr -  '(' 1
compare 'trailing word'  expr -  1 1
compare 'no arguments'   expr -
compare 'end of options' expr -  -- 1

#       Sixty four bits where GNU has as many as it likes, written down here
#       so the wrap is a decision and not a surprise: what is asserted is that
#       it wraps the way two's complement does, not that it agrees.
overflow=$("$bin/expr" 9223372036854775807 + 1)
if [ "$overflow" = "-9223372036854775808" ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        printf '  %-8s %-30s want %-24s got %s\n' expr 'overflow wraps' \
                '-9223372036854775808' "$overflow"
fi

printf '  %-12s %s of %s\n' listed "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'text-listed %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

listed_fail=$fail

#
#       Generated cases, ours against the system's.
#
#       The seed is fixed so the same twelve thousand nine hundred questions
#       are asked every time and a failure can be reproduced by running it
#       again rather than by being lucky twice.
#

if ! command -v python3 > /dev/null 2>&1; then
        echo "  generated    skipped (needs python3)"
        [ "$listed_fail" = 0 ]
        exit
fi

BIN=$bin ROUNDS=$rounds python3 - <<'PYTHON' > "$work/generated" 2>&1
import os, random, subprocess

ours = os.environ["BIN"]
rounds = int(os.environ["ROUNDS"])
total = 0
bad = 0
shown = 0


def report(what, script, want, got, data):
    global shown
    shown += 1
    if shown <= 8:
        print("  %-6s %-28s want %r got %r" % (what, script, want, got))
        print("         on %r" % data)


def lines(count, longest, alphabet="abc"):
    return "\n".join(
        "".join(random.choice(alphabet) for _ in range(random.randint(0, longest)))
        for _ in range(count)
    ) + "\n"


def run(program, arguments, data):
    got = subprocess.run([program] + arguments, input=data,
                         capture_output=True, text=True)
    return got.stdout, got.returncode


def both(name, arguments, data):
    global total, bad
    total += 1
    want, want_status = run(name, arguments, data)
    mine, mine_status = run(ours + "/" + name, arguments, data)

    if want != mine or want_status != mine_status:
        bad += 1
        report(name, " ".join(arguments), want, mine, data)


basic = ["a", "b", "c", ".", "[ab]", "[^a]", "\\(a\\)", "\\(ab\\)", "[a-c]",
         "a*", "b*", ".*", "\\(a\\)*", "ab", "a\\|b", "\\(a\\|b\\)",
         "a\\{1,2\\}", "[abc]\\{2\\}", "a\\+", "b\\?", "\\(ab\\)*",
         "\\(a\\|bc\\)", "\\(ab\\|a\\)"]

extended = ["a", "b", "c", ".", "[ab]", "[^a]", "(a)", "(ab)", "[a-c]", "a*",
            "b*", ".*", "(a)*", "ab", "a|b", "(a|b)", "a{1,2}", "[abc]{2}",
            "a+", "b?", "(ab)+", "(a|b)*", "(a|bc)"]

random.seed(20260827)

for _ in range(rounds):
    pattern = "".join(random.choice(basic) for _ in range(random.randint(1, 3)))

    if random.random() < 0.25:
        pattern = "^" + pattern

    if random.random() < 0.25:
        pattern = pattern + "$"

    data = lines(8, 6)

    for flags in (["-c"], ["-n"], ["-ci"], ["-cv"]):
        both("grep", flags + [pattern], data)

for _ in range(rounds):
    pattern = "".join(random.choice(extended) for _ in range(random.randint(1, 3)))

    if random.random() < 0.25:
        pattern = "^" + pattern

    if random.random() < 0.25:
        pattern = pattern + "$"

    data = lines(8, 6)

    for flags in (["-cE"], ["-nE"], ["-cEi"]):
        both("grep", flags + [pattern], data)

for _ in range(rounds):
    pattern = "".join(random.choice(basic) for _ in range(random.randint(1, 3)))
    replacement = random.choice(["X", "[&]", "<\\1>", "", "Y&Y"])

    if "\\1" in replacement and "\\(" not in pattern:
        replacement = "X"

    data = lines(6, 6)

    for tail in ("", "g", "2", "gp"):
        both("sed", ["-n" if tail == "gp" else "-e",
                     "s/" + pattern + "/" + replacement + "/" + tail], data)

for _ in range(rounds // 2):
    data = lines(6, 6)

    for flags in (["-n"], ["-c"], ["-u"], ["-d"], ["-i"]):
        both("uniq", flags if flags != ["-n"] else [], data)

    for flags in (["-r"], ["-u"], ["-f"], ["-n"]):
        both("sort", flags, data)

    for width in ("1", "3", "5"):
        both("fold", ["-w", width], data)
        both("cut", ["-c" + width + "-"], data)

    both("rev", [], data)
    both("wc", [], data)
    both("nl", [], data)
    both("tr", ["a-c", "x-z"], data)
    both("tr", ["-d", "ab"], data)
    both("tr", ["-s", "abc"], data)

#       A count taken off the end, against one taken off the front of the
#       same input read backwards. The unterminated last line is the one
#       that decides where the boundary is, so half of these have none.

for _ in range(rounds // 2):
    count = random.randint(0, 9)
    data = lines(random.randint(0, 8), 5)

    if random.random() < 0.5:
        data = data[:-1]

    for flag in ("-n", "-c"):
        both("head", [flag, "-%d" % count], data)
        both("head", [flag, "%d" % count], data)
        both("tail", [flag, "-%d" % count], data)
        both("tail", [flag, "+%d" % count], data)

#       The address forms GNU added. A step, a range that ends N lines later
#       or at the next line a number divides, and line zero.

for _ in range(rounds // 2):
    data = lines(random.randint(1, 9), 4)
    first = random.randint(0, 5)
    step = random.randint(0, 4)
    other = random.randint(0, 5)

    for script in ("%d~%dp" % (first, step),
                   "%d~%d!p" % (first, step),
                   "%d,+%dp" % (max(first, 1), other),
                   "%d,~%dp" % (max(first, 1), other),
                   "/a/,+%dp" % other,
                   "/a/,~%dp" % max(other, 1),
                   "0,/a/p",
                   "0,/%s/p" % random.choice("abc"),
                   "0,/a/!p"):
        both("sed", ["-n", script], data)

    both("sed", ["0,/b/d"], data)
    both("sed", ["%d~%dd" % (first, step)], data)
    both("sed", ["%dQ" % max(first, 1)], data)
    both("sed", ["-n", "%dQ%d" % (max(first, 1), other)], data)

#       A substitution and a branch off the back of whether it took, which is
#       the whole of what t and T decide.

for _ in range(rounds // 4):
    data = lines(random.randint(1, 6), 4)
    one = random.choice("abc")
    two = random.choice("abc")

    for script in ("s/%s/X/;t e;s/%s/Y/;:e" % (one, two),
                   "s/%s/X/;T e;s/%s/Y/;:e" % (one, two),
                   "/%s/b e\ns/%s/Y/\n:e" % (one, two),
                   ":a;s/%s/X/;ta" % one,
                   ":a;N;$!ba;s/\\n/,/g",
                   "$!{N;s/\\n/+/}"):
        both("sed", [script], data)

#       Two files, byte for byte, with a skip and a limit either side of the
#       first difference. cmp reads names rather than standard input, so the
#       pair is written out and handed over.

import tempfile

folder = tempfile.mkdtemp()
left = folder + "/left"
right = folder + "/right"


def bytes_of(count):
    return "".join(random.choice("abc\n") for _ in range(count))


for _ in range(rounds // 2):
    one = bytes_of(random.randint(0, 24))
    two = one

    if random.random() < 0.8:
        at = random.randint(0, max(len(one) - 1, 0))
        two = one[:at] + random.choice("xyz\n") + one[at + 1:]

    if random.random() < 0.3:
        two = two[:random.randint(0, len(two))]

    open(left, "w").write(one)
    open(right, "w").write(two)

    skip = random.randint(0, 6)
    limit = random.randint(0, 12)

    for flags in ([], ["-b"], ["-l"], ["-bl"], ["-s"],
                  ["-i", str(skip)],
                  ["-i", "%d:%d" % (skip, random.randint(0, 6))],
                  ["-n", str(limit)],
                  ["-l", "-i", str(skip)],
                  ["-b", "-n", str(limit)]):
        both("cmp", flags + [left, right], "")

    both("cmp", [left, right, str(skip)], "")
    both("cmp", [left, right, str(skip), str(random.randint(0, 6))], "")

os.remove(left)
os.remove(right)
os.rmdir(folder)

#       The globs --include and the two beside it are matched against, asked
#       here of grep itself with one file named after the other so what the
#       glob decided is the whole of the answer.

names = ["ab.txt", "a.txt", "b.log", "abc", "a-b.txt", "A.txt", ".hidden",
         "x.tar.gz"]
folder = tempfile.mkdtemp()

for name in names:
    open(folder + "/" + name, "w").write("alpha\n")

pieces = ["*", "?", "a", "b", ".", "txt", "[ab]", "[^a]", "[a-c]", "*.txt",
          "-", "[!b]", "]"]

for _ in range(rounds // 2):
    glob = "".join(random.choice(pieces)
                   for _ in range(random.randint(1, 3)))

    both("grep", ["-rl", "--include=" + glob, "alpha", folder], "")
    both("grep", ["-rl", "--exclude=" + glob, "alpha", folder], "")

for name in names:
    os.remove(folder + "/" + name)

os.rmdir(folder)

print("\n  %s of %s" % (total - bad, total))
PYTHON

sed -n '/want/p' "$work/generated"

line=$(sed -n 's/^  \([0-9]*\) of \([0-9]*\)$/\1 \2/p' "$work/generated")

# An empty parse would leave both counts empty and compare equal, which is a
# suite reporting that nothing failed because nothing ran.
if [ -z "$line" ]; then
        echo "  generated    printed no verdict"
        sed 's/^/    /' "$work/generated" | tail -5
        exit 1
fi

made=${line% *}
made_total=${line#* }

printf '  %-12s %s of %s\n' generated "$made" "$made_total"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'text-generated %s %s\n' "$made" "$made_total" >> "$TEST_TALLY"

[ "$listed_fail" = 0 ] && [ "$made" = "$made_total" ]
