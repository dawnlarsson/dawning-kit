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

case_start wc
compare 'default'        wc a
compare 'lines'          wc a  -l
compare 'words'          wc a  -w
compare 'bytes'          wc a  -c
compare 'characters'     wc a  -m
compare 'named'          wc -  "$work/a"
compare 'two files'      wc -  "$work/a" "$work/b"
compare 'lines named'    wc -  -l "$work/a"
compare 'lines two'      wc -  -l "$work/a" "$work/b"
compare 'no newline'     wc h
compare 'empty'          wc k  -l

case_start rev
compare 'plain'          rev a
compare 'blank lines'    rev k
compare 'no newline'     rev h

case_start nl
compare 'default'        nl a
compare 'all lines'      nl a  -ba
compare 'width'          nl a  -w3
compare 'separator'      nl a  -s:
compare 'start'          nl a  -v5
compare 'step'           nl a  -i2
compare 'blank heavy'    nl k

case_start fold
compare 'default'        fold a
compare 'width'          fold a  -w 8
compare 'joined width'   fold a  -w8
compare 'spaces'         fold a  -w 8 -s
compare 'narrow'         fold a  -w 3
compare 'bytes'          fold g  -w 4 -b
compare 'no newline'     fold h  -w 5

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
compare 'multiple files' sed -  -n '$p' "$work/s" "$work/m"
compare 'char class rep' sed n  's/[[:blank:]]\+/ /g'
compare 'leading blanks' sed n  's/^[ \t]*//'

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
compare 'repeat set'     tr s  'one' '[X*]'
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

case_start edges
compare 'cut mixed sep'  cut w   '-d ' -f2
compare 'cut tab sep'    cut w   -f2
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
