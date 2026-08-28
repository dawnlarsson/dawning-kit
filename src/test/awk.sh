#!/bin/sh
#
#       Cases for the awk lane.
#
#       Every case runs the same input through the reference the case names
#       and through ours, and compares. Agreeing with the reference is what
#       passing means; there is no separate idea here of the right answer.
#
#       LC_ALL=C for the reason text.sh gives: a comparison sorts and folds
#       by locale otherwise, and the disagreement looks like a bug in awk
#       rather than in the question.
#
#       Two sections. The listed ones are what somebody thought to ask. The
#       generated ones build programs and inputs instead and ask thousands,
#       which is where field splitting and the string-versus-number rules
#       actually break.

LC_ALL=C
export LC_ALL

farm=${1:-/tmp/awkfarm}
rounds=${2:-400}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

#       The one binary, under the name awk.
#
#       The runner builds a directory of links for the names it knows about
#       and awk is not one of them, so the link is made here out of whatever
#       the farm already points at rather than by building a second shell.
ours=$farm/awk

if [ ! -x "$ours" ]; then
        for name in cat grep sed; do
                [ -e "$farm/$name" ] || continue

                target=$(readlink -f "$farm/$name" 2> /dev/null) ||
                        target=$(readlink "$farm/$name" 2> /dev/null)

                [ -n "$target" ] || target=$farm/$name

                mkdir -p "$work/bin"
                ln -sf "$target" "$work/bin/awk"
                ours=$work/bin/awk
                break
        done
fi

pass=0
fail=0
group=""

printf 'alpha beta gamma\ndelta epsilon\n\nzeta eta theta iota\nalpha beta gamma\n' > "$work/words"
printf 'one:two:three\nfour:five:six\nnodelim\nseven::nine\n' > "$work/colons"
printf '10 x\n9 y\n100 z\n2 w\n-3 v\n2.5 u\n0 t\n' > "$work/numbers"
printf '  ab  cd ef  \n\tgh\tij\t\n\nplain\n' > "$work/spaced"
printf 'a\nb\nc\nd\ne\nf\ng\nh\n' > "$work/letters"
printf '1 2 3\n4 5 6\n7 8 9\n' > "$work/grid"
printf 'no newline at the end' > "$work/bare"
printf 'a\nb\n\n\nc\nd\n\ne\n' > "$work/paragraphs"
printf '10.0\n010\n1e2\n+3\n abc \n\n0x10\n3.\n' > "$work/looks"
printf 'x\n' > "$work/one"
: > "$work/empty"

case_start()
{
        group=$1
}

report()
{
        fail=$((fail + 1))
        printf '  %-9s %-26s want %-26s got %s\n' \
                "$group" "$1" \
                "$(head -c 40 "$work/want" | tr '\n\t' '|>')[$want_status]" \
                "$(head -c 40 "$work/got" | tr '\n\t' '|>')[$got_status]"
}

#       One program, both awks, same standard input. Everything after the
#       feed goes to both unchanged, so a case can name files and flags.
compare()
{
        name=$1
        feed=$2
        shift 2

        awk "$@" < "$feed" > "$work/want" 2> /dev/null
        want_status=$?
        "$ours" "$@" < "$feed" > "$work/got" 2> /dev/null
        got_status=$?

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name"
}

#       A program that writes files of its own gets a directory of its own,
#       and what it left there is compared as well as what it printed.
compare_effect()
{
        name=$1
        feed=$2
        shift 2

        rm -rf "$work/want.d" "$work/got.d"
        mkdir -p "$work/want.d" "$work/got.d"

        (cd "$work/want.d" && awk "$@" < "$feed") > "$work/want" 2> /dev/null
        want_status=$?
        (cd "$work/got.d" && "$ours" "$@" < "$feed") > "$work/got" 2> /dev/null
        got_status=$?

        if cmp -s "$work/want" "$work/got" && [ "$want_status" = "$got_status" ] &&
                diff -r "$work/want.d" "$work/got.d" > /dev/null 2>&1; then
                pass=$((pass + 1))
                return 0
        fi

        report "$name"
}

if ! command -v awk > /dev/null 2>&1; then
        echo "  no awk on this machine to compare against"
        exit 2
fi

if [ ! -x "$ours" ]; then
        echo "  $ours is not there"
        exit 2
fi

#
#       Program structure.
#

case_start structure
compare 'begin' /dev/null 'BEGIN { print "begin" }'
compare 'end' "$work/letters" 'END { print NR }'
compare 'begin and end' "$work/letters" 'BEGIN{print "b"} {print} END{print "e"}'
compare 'bare action' "$work/letters" '{ print $0 }'
compare 'bare pattern' "$work/numbers" '$1 > 5'
compare 'pattern action' "$work/numbers" '$1 > 5 { print $2 }'
compare 'two rules' "$work/letters" '/a/{print "A"} /b/{print "B"}'
compare 'semicolons' /dev/null 'BEGIN { ; print 1;; print 2 ; }'
compare 'newline separated' /dev/null 'BEGIN {
        print 1
        print 2
}'
compare 'comment' /dev/null 'BEGIN { # nothing here
        print 1 # nor here
}'
compare 'continuation' /dev/null 'BEGIN { x = 1 + \
        2; print x }'
compare 'empty program' "$work/letters" ''
compare 'only a comment' "$work/letters" '# nothing'
compare 'regex pattern' "$work/words" '/al/'
compare 'negated regex' "$work/words" '!/al/'
compare 'range' "$work/letters" '/b/,/d/'
compare 'range same line' "$work/letters" '/b/,/b/'
compare 'range never ends' "$work/letters" '/c/,/zzz/'
compare 'range by number' "$work/letters" 'NR==2,NR==4'
compare 'begin only reads nothing' "$work/letters" 'BEGIN{print "x"}'
compare 'end sees last record' "$work/grid" 'END{print $0, NF, NR}'

#
#       Fields.
#

case_start fields
compare 'dollar zero' "$work/grid" '{print $0}'
compare 'fields' "$work/grid" '{print $1, $3}'
compare 'nf' "$work/spaced" '{print NR, NF}'
compare 'field beyond nf' "$work/grid" '{print "[" $9 "]", NF}'
compare 'read past nf keeps nf' "$work/grid" '{x = $9; print NF}'
compare 'last field' "$work/grid" '{print $NF}'
compare 'computed field' "$work/grid" '{i=2; print $(i+1)}'
compare 'nested field' "$work/grid" '{print $$1}'
compare 'assign field' "$work/grid" '{$2 = "X"; print; print NF}'
compare 'assign past nf' "$work/grid" '{$5 = "X"; print; print NF}'
compare 'assign record' "$work/grid" '{$0 = "p q r"; print NF, $2}'
compare 'assign nf smaller' "$work/grid" '{NF = 2; print; print NF}'
compare 'assign nf larger' "$work/grid" '{NF = 5; print; print NF, length($0)}'
compare 'assign nf zero' "$work/grid" '{NF = 0; print "[" $0 "]", NF}'
compare 'ofs on rebuild' "$work/grid" 'BEGIN{OFS="-"} {$1=$1; print}'
compare 'ofs without touching' "$work/grid" 'BEGIN{OFS="-"} {print}'
compare 'ofs on print' "$work/grid" 'BEGIN{OFS="-"} {print $1, $2}'
compare 'ors' "$work/grid" 'BEGIN{ORS="|"} {print $1}'
compare 'empty ors' "$work/grid" 'BEGIN{ORS=""} {print $1}'
compare 'default splitting' "$work/spaced" '{print NF "|" $1 "|" $2}'
compare 'fs colon' "$work/colons" -F: '{print NF, $2}'
compare 'fs colon variable' "$work/colons" 'BEGIN{FS=":"} {print NF, $2}'
compare 'fs one dot' "$work/colons" -F. '{print NF}'
compare 'fs regex' "$work/numbers" -F'[0-9]+' '{print NF}'
compare 'fs tab escape' "$work/spaced" -F'\t' '{print NF}'
compare 'fs literal space' "$work/spaced" -F' ' '{print NF}'
compare 'fs changed midway' "$work/colons" '{FS=":"} {print $1}'
compare 'fs multi char' "$work/words" -F'ta' '{print NF}'
compare 'nr and fnr' "$work/letters" '{print NR, FNR}'
compare 'filename' "$work/one" '{print FILENAME}' "$work/one" "$work/one"
compare 'fnr per file' "$work/one" '{print FILENAME, FNR, NR}' "$work/one" "$work/one"
compare 'filename in begin' /dev/null 'BEGIN{print "[" FILENAME "]"}'
compare 'no trailing newline' "$work/bare" '{print NR, $0}'
compare 'empty input' "$work/empty" '{print "never"} END{print NR}'
compare 'blank lines' "$work/paragraphs" '{print NR, NF}'
compare 'blanks only' "$work/spaced" '$0 ~ /^[ \t]*$/ {print NR, NF}'
compare 'fs at both ends' "$work/colons" -F: '{print NF "[" $1 "][" $NF "]"}'
compare 'fs every byte' /dev/null 'BEGIN{n=split(":::",p,":"); print n; for(i=1;i<=n;i++) print i "[" p[i] "]"}'
compare 'fs absent from line' "$work/words" -F: '{print NF "[" $1 "]"}'

#
#       Records: RS.
#

case_start records
compare 'rs one char' "$work/colons" 'BEGIN{RS=":"} {print NR, $0}'
compare 'rs paragraph' "$work/paragraphs" 'BEGIN{RS=""} {print NR "[" $0 "]"}'
compare 'rs paragraph fields' "$work/paragraphs" 'BEGIN{RS=""} {print NR, NF, $2}'
compare 'rs paragraph with fs' "$work/colons" 'BEGIN{RS="";FS=":"} {print NF, $1, $NF}'
compare 'rs regex' "$work/numbers" 'BEGIN{RS="[0-9]+"} {print NR "[" $0 "]"}'
compare 'rs changed midway' "$work/letters" 'NR==1{RS="c"} {print NR "[" $0 "]"}'
compare 'rs and ors' "$work/colons" 'BEGIN{RS=":";ORS="-"} {print}'
compare 'rs absent from input' "$work/letters" 'BEGIN{RS="@"} {print NR "[" $0 "]"}'
compare 'rs paragraph to the end' "$work/bare" 'BEGIN{RS=""} {print NR "[" $0 "]"}'
compare 'rs paragraph one line' "$work/one" 'BEGIN{RS=""} {print NR, NF}'

#
#       Expressions.
#

case_start expression
compare 'arithmetic' /dev/null 'BEGIN{print 1+2, 7-3, 6*7, 9/2, 7%3, 2^10}'
compare 'negative modulo' /dev/null 'BEGIN{print -7%3, 7%-3, 7.5%2}'
compare 'power right' /dev/null 'BEGIN{print 2^3^2, -2^2, 2**3}'
compare 'power negative' /dev/null 'BEGIN{print 2^-1, (-2)^3, 10^-3}'
compare 'unary' /dev/null 'BEGIN{print -"3x", +"4y", !"", !"a", !0, !1}'
compare 'concatenation' /dev/null 'BEGIN{print 1 2, "a" "b", 1 " " 2}'
compare 'concat minus' /dev/null 'BEGIN{print 1 " " -1; print 1" "-1}'
compare 'concat binds looser' /dev/null 'BEGIN{print 1 -1, 1 - 1, "x" 1+1}'
compare 'string to number' /dev/null 'BEGIN{print "3.5e2"+0, " 12 "+0, ".5"+0, "1e"+0, "12abc"+0}'
compare 'increment' /dev/null 'BEGIN{x=5; print x++, x, ++x, x, x--, x, --x, x}'
compare 'increment string' /dev/null 'BEGIN{x="5"; x++; print x; y="a"; y++; print y}'
compare 'increment field' "$work/grid" '{i=1; print $i++, i, $1}'
compare 'assignment operators' /dev/null 'BEGIN{x=10; x-=1; x*=2; x/=3; x%=4; x^=2; print x}'
compare 'chained assignment' /dev/null 'BEGIN{x = y = 3; print x, y}'
compare 'ternary' /dev/null 'BEGIN{print 1?"a":"b", 0?"a":"b"}'
compare 'ternary nested' /dev/null 'BEGIN{x=2; print x==1?"one":x==2?"two":"many"}'
compare 'and or' /dev/null 'BEGIN{print 1&&1, 1&&0, 0||1, 0||0, !1||1}'
compare 'short circuit' /dev/null 'BEGIN{x=0; 0 && x++; print x; 1 || x++; print x}'
compare 'match operator' "$work/words" '{print ($0 ~ /al/), ($0 !~ /al/)}'
compare 'dynamic regex' /dev/null 'BEGIN{r="^a"; print ("abc" ~ r), ("bc" ~ r)}'
compare 'dynamic regex escape' /dev/null 'BEGIN{print ("a.b" ~ "a\\.b"), ("axb" ~ "a\\.b")}'
compare 'regex as value' "$work/words" '{x = /al/; print x}'
compare 'parenthesised' /dev/null 'BEGIN{print (1+2)*3, 1+2*3}'
compare 'uninitialised' /dev/null 'BEGIN{print "[" x "]", x+0, length(x)}'
compare 'in operator' /dev/null 'BEGIN{a["x"]=1; print ("x" in a), ("y" in a)}'
compare 'in precedence' /dev/null 'BEGIN{a["x"]=1; print "x" in a ? "y" : "n"}'

#
#       Comparison: the string and number rules, which is where awks differ.
#

case_start compare
compare 'numbers' /dev/null 'BEGIN{print (1<2), (2<1), (1==1.0), (10<9)}'
compare 'strings' /dev/null 'BEGIN{print ("abc"<"abd"), ("10"<"9"), ("a"=="a")}'
compare 'constant is a string' /dev/null 'BEGIN{x="10.0"; print (x==10), (x=="10"), (x=="10.0")}'
compare 'field is a strnum' "$work/looks" 'NR==1{print ($1==10), ($1=="10"), ($1=="10.0")}'
compare 'field leading zero' "$work/looks" 'NR==2{print ($1==10), ($1=="010"), ($1<9)}'
compare 'field exponent' "$work/looks" 'NR==3{print ($1==100), ($1=="1e2")}'
compare 'field signed' "$work/looks" 'NR==4{print ($1==3), ($1=="+3")}'
compare 'field not a number' "$work/looks" 'NR==5{print ($1==0), ($1=="abc")}'
compare 'field hex is not' "$work/looks" 'NR==7{print ($1==0), ($1==16), ($1=="0x10")}'
compare 'record with blanks' "$work/looks" 'NR==1{print ($0==10)}'
compare 'uninitialised both ways' /dev/null 'BEGIN{print (u==0), (u==""), (u<1), (u<"1")}'
compare 'empty field' "$work/looks" 'NR==6{print ($1==0), ($1==""), NF}'
compare 'strnum against strnum' "$work/looks" 'NR==1{a=$1} NR==2{print (a<$1), (a>$1)}'
compare 'number against string' /dev/null 'BEGIN{print (10=="10"), (10=="10.0"), (0=="")}'
compare 'getline var is a strnum' "$work/looks" 'NR==1{getline x; print (x==10), (x=="010")}'
compare 'split makes strnums' /dev/null 'BEGIN{split("10.0 abc",a); print (a[1]==10), (a[1]=="10.0")}'
compare 'sorted by string' "$work/numbers" '{ if ($1 < "5") print $1 }'

#
#       Control flow.
#

case_start control
compare 'if else' /dev/null 'BEGIN{if (1) print "a"; else print "b"}'
compare 'else on its own line' /dev/null 'BEGIN{if (0) print "a"
else print "b"}'
compare 'nested if' /dev/null 'BEGIN{if (1) if (0) print "a"; else print "b"}'
compare 'while' /dev/null 'BEGIN{i=0; while (i<3) {print i; i++}}'
compare 'while empty body' /dev/null 'BEGIN{i=0; while (i++ < 3) ; print i}'
compare 'do while' /dev/null 'BEGIN{i=0; do {print i; i++} while (i<3)}'
compare 'do while once' /dev/null 'BEGIN{i=9; do print i; while (0)}'
compare 'for' /dev/null 'BEGIN{for (i=0;i<3;i++) print i}'
compare 'for no parts' /dev/null 'BEGIN{i=0; for (;;) {if (i>2) break; print i; i++}}'
compare 'for in sorted' /dev/null 'BEGIN{a["b"]=1;a["a"]=2;a["c"]=3; n=0; for (k in a) n++; print n}'
compare 'break' /dev/null 'BEGIN{for (i=0;i<9;i++) {if (i==3) break; print i}}'
compare 'continue' /dev/null 'BEGIN{for (i=0;i<5;i++) {if (i%2) continue; print i}}'
compare 'break in while' /dev/null 'BEGIN{i=0; while (1) {i++; if (i>2) break} print i}'
compare 'next' "$work/letters" '{if (NR%2) next; print}'
compare 'next in a function of a rule' "$work/letters" 'NR==2{next} {print}'
compare 'exit code' /dev/null 'BEGIN{exit 3}'
compare 'exit runs end' /dev/null 'BEGIN{exit 3} END{print "end"}'
compare 'exit in a rule' "$work/letters" 'NR==2{exit 1} {print} END{print "e", NR}'
compare 'exit in end' /dev/null 'BEGIN{exit 1} END{exit}'
compare 'exit in end with code' /dev/null 'BEGIN{exit 1} END{exit 5}'
compare 'exit no code keeps zero' "$work/letters" '{exit} END{print NR}'
compare 'nested loops' /dev/null 'BEGIN{for(i=0;i<3;i++){for(j=0;j<3;j++){if(j==1)continue; if(j==2)break; print i,j}}}'

#
#       Arrays.
#

case_start array
compare 'assign and read' /dev/null 'BEGIN{a["k"]="v"; print a["k"]}'
compare 'number key' /dev/null 'BEGIN{a[1]="x"; print a["1"], a[1.0]}'
compare 'key format' /dev/null 'BEGIN{CONVFMT="%.2g"; a[12]=1; a[3.14159]=2; for(k in a) print k}'
compare 'in does not create' /dev/null 'BEGIN{if ("x" in a) print "y"; print length(a)}'
compare 'reading creates' /dev/null 'BEGIN{x = a["k"]; print length(a), ("k" in a)}'
compare 'delete one' /dev/null 'BEGIN{a[1];a[2]; delete a[1]; print length(a), (1 in a), (2 in a)}'
compare 'delete all' /dev/null 'BEGIN{a[1];a[2];a[3]; delete a; print length(a)}'
compare 'delete in a loop' /dev/null 'BEGIN{a[1];a[2];a[3]; for(k in a) delete a[k]; print length(a)}'
compare 'subsep' /dev/null 'BEGIN{a[1,2]="x"; for(k in a){split(k,p,SUBSEP); print p[1], p[2]}}'
compare 'in with parentheses' /dev/null 'BEGIN{a[1,2]=1; print ((1,2) in a), ((1,3) in a)}'
compare 'subsep by hand' /dev/null 'BEGIN{a[1,2]=1; print ((1 SUBSEP 2) in a)}'
compare 'counting' "$work/words" '{for(i=1;i<=NF;i++) n[$i]++} END{print length(n), n["alpha"]}'
compare 'array length' /dev/null 'BEGIN{for(i=1;i<=10;i++) a[i]=i; print length(a)}'
compare 'array grows intact' /dev/null 'BEGIN{for(i=1;i<=140;i++)a["k" i]=i; for(i=1;i<=140;i++)s+=a["k" i]; print length(a),s,a["k1"],a["k140"]}'
compare 'sum over keys' /dev/null 'BEGIN{a[1]=1;a[2]=2;a[3]=3; for(k in a) s+=a[k]; print s}'

#
#       Built-in functions.
#

case_start builtin
compare 'length of record' "$work/grid" '{print length()}'
compare 'length no parentheses' "$work/grid" '{print length}'
compare 'length of string' /dev/null 'BEGIN{print length("hello"), length(""), length(12345)}'
compare 'substr' /dev/null 'BEGIN{print substr("hello",2), substr("hello",2,2), substr("hello",1,0) "|"}'
compare 'substr past the end' /dev/null 'BEGIN{print "[" substr("hello",6) "]", "[" substr("hello",2,99) "]"}'
compare 'substr fractional' /dev/null 'BEGIN{print substr("hello",1.9,1.9) "|" substr("hello",2.7,2.2)}'
compare 'index' /dev/null 'BEGIN{print index("hello","ll"), index("hello","z"), index("hello","h")}'
compare 'index empty needle' /dev/null 'BEGIN{print index("abc",""), index("",""), index("","a")}'
compare 'index at the end' /dev/null 'BEGIN{print index("hello","o"), index("hello","lo"), index("hello","hello")}'
compare 'index overlapping' /dev/null 'BEGIN{print index("aaaa","aaa"), index("abababc","babc")}'
compare 'split default' /dev/null 'BEGIN{n=split("  a b  c ",a); print n, "[" a[1] "]", "[" a[3] "]"}'
compare 'split on a string' /dev/null 'BEGIN{n=split("a:b::c",a,":"); print n, "[" a[3] "]"}'
compare 'split on a regex' /dev/null 'BEGIN{n=split("a1b22c",a,/[0-9]+/); print n, a[2], a[3]}'
compare 'split empty string' /dev/null 'BEGIN{print split("",a), length(a)}'
compare 'split clears' /dev/null 'BEGIN{a[9]="old"; n=split("x y",a); print n, length(a), "[" a[9] "]"}'
compare 'sub' /dev/null 'BEGIN{s="aaa"; n=sub(/a/,"X",s); print n, s}'
compare 'sub no match' /dev/null 'BEGIN{s="aaa"; n=sub(/z/,"X",s); print n, s}'
compare 'gsub' /dev/null 'BEGIN{s="aaa"; n=gsub(/a/,"X",s); print n, s}'
compare 'gsub empty match' /dev/null 'BEGIN{s="abc"; n=gsub(/x*/,"-",s); print n, s}'
compare 'gsub empty after match' /dev/null 'BEGIN{s="baac"; n=gsub(/a*/,"-",s); print n, s}'
compare 'gsub ampersand' /dev/null 'BEGIN{s="ab"; gsub(/a/,"[&]",s); print s}'
compare 'gsub escaped ampersand' /dev/null 'BEGIN{s="ab"; gsub(/a/,"\\&",s); print s}'
compare 'gsub backslash' /dev/null 'BEGIN{s="ab"; gsub(/a/,"x\\\\y",s); print s}'
compare 'gsub anchored' /dev/null 'BEGIN{s="aaa"; n=gsub(/^a/,"X",s); print n, s}'
compare 'sub on the record' "$work/grid" '{sub(/1/,"X"); print}'
compare 'gsub on a field' "$work/colons" -F: '{gsub(/e/,"E",$2); print; print NF}'
compare 'gsub on an element' /dev/null 'BEGIN{a[1]="xyx"; gsub(/x/,"z",a[1]); print a[1]}'
compare 'match' /dev/null 'BEGIN{print match("hello",/l+/), RSTART, RLENGTH}'
compare 'match failing' /dev/null 'BEGIN{print match("hello",/z/), RSTART, RLENGTH}'
compare 'match longest' /dev/null 'BEGIN{print match("aaa",/a*/), RSTART, RLENGTH}'
compare 'sprintf' /dev/null 'BEGIN{print sprintf("%s-%d", "a", 3)}'
compare 'toupper tolower' /dev/null 'BEGIN{print toupper("aBc1_"), tolower("AbC1_")}'
compare 'int' /dev/null 'BEGIN{print int(3.9), int(-3.9), int("4.7x"), int(0), int("1e3")}'
compare 'sqrt' /dev/null 'BEGIN{print sqrt(2), sqrt(0), sqrt(9), sqrt(1e10)}'
compare 'exp and log' /dev/null 'BEGIN{print exp(1), log(2), exp(0), log(1), exp(-1)}'
compare 'sin and cos' /dev/null 'BEGIN{print sin(0), cos(0), sin(1), cos(1), sin(3.14159)}'
compare 'atan2' /dev/null 'BEGIN{print atan2(0,1), atan2(1,1), atan2(1,0), atan2(-1,-1)}'
compare 'trig further out' /dev/null 'BEGIN{print sin(10), cos(10), sin(-2.5), exp(10), log(1000)}'
compare 'rand is in range' /dev/null 'BEGIN{srand(1); ok=1; for(i=0;i<100;i++){x=rand(); if (x<0||x>=1) ok=0} print ok}'
compare 'srand returns the last' /dev/null 'BEGIN{srand(1); print srand(5); print srand(7)}'
compare 'rand repeats with a seed' /dev/null 'BEGIN{srand(3); x=rand(); srand(3); print (x==rand())}'
compare 'close what is not open' /dev/null 'BEGIN{print close("nothing")}'
compare 'system' /dev/null 'BEGIN{print "a"; r = system("echo b"); print "c" r}'
compare 'system status' /dev/null 'BEGIN{print system("exit 3")}'
compare 'system status 255' /dev/null 'BEGIN{print system("exit 255")}'
# gawk distinguishes an ordinary signal (256+signal) from one whose default
# action carries WCOREDUMP (512+signal). stderr is discarded by compare, so
# only the numeric system() contract is under test here.
compare 'system term status' /dev/null 'BEGIN{print system("kill -TERM $$")}'
compare 'system kill status' /dev/null 'BEGIN{print system("kill -KILL $$")}'
compare 'system segv status' /dev/null 'BEGIN{print system("ulimit -c 0; kill -SEGV $$")}'
compare 'system abort status' /dev/null 'BEGIN{print system("ulimit -c 0; kill -ABRT $$")}'

#
#       print and printf.
#

case_start printf
compare 'print list' /dev/null 'BEGIN{print 1, "a", 2.5}'
compare 'print no arguments' "$work/grid" '{print}'
compare 'print concatenated' /dev/null 'BEGIN{print "a" "b" "c"}'
compare 'printf strings' /dev/null 'BEGIN{printf "[%s][%10s][%-10s][%.2s]\n", "ab", "ab", "ab", "abcd"}'
compare 'printf integers' /dev/null 'BEGIN{printf "[%d][%5d][%-5d][%05d][%+d][% d]\n", 42, 42, 42, 42, 42, 42}'
compare 'printf integer rounding' /dev/null 'BEGIN{printf "[%d][%d][%d][%d]\n", 3.9, -3.9, 0.5, -0.5}'
compare 'printf integer big' /dev/null 'BEGIN{printf "[%d][%d][%d]\n", 1e18, 2^53, -1e15}'
compare 'printf bases' /dev/null 'BEGIN{printf "[%o][%x][%X][%u][%#o][%#x]\n", 8, 255, 255, 42, 8, 255}'
compare 'printf bases zero precision' /dev/null 'BEGIN{printf "[%.0o][%.0u][%.0x][%.0X]\n", 0, 0, 0, 0}'
compare 'printf alternate zero precision' /dev/null 'BEGIN{printf "[%#.0o][%#.0x][%#.0X]\n", 0, 0, 0}'
compare 'printf bases with precision' /dev/null 'BEGIN{printf "[%#.3o][%#.1o][%.5x][%#.5X]\n", 8, 8, 255, 255}'
compare 'printf floats' /dev/null 'BEGIN{printf "[%f][%.2f][%10.3f][%-10.1f][%.0f]\n", 3.14159, 3.14159, 3.14159, 3.14159, 0.5}'
compare 'printf scientific' /dev/null 'BEGIN{printf "[%e][%E][%.2e][%.0e]\n", 12345.678, 12345.678, 12345.678, 12345.678}'
compare 'printf general' /dev/null 'BEGIN{printf "[%g][%G][%.3g][%g][%g]\n", 0.0001234, 1e20, 12345.6, 100000, 1000000}'
compare 'printf general edges' /dev/null 'BEGIN{printf "[%g][%g][%g][%g]\n", 0, 0.0001, 0.00001, 123456789}'
compare 'printf percent' /dev/null 'BEGIN{printf "100%%\n"}'
compare 'printf character' /dev/null 'BEGIN{printf "[%c][%c][%c]\n", 65, "hello", 300}'
compare 'printf star' /dev/null 'BEGIN{printf "[%*d][%-*d][%.*f]\n", 5, 42, 5, 42, 2, 3.14159}'
compare 'printf negative star' /dev/null 'BEGIN{printf "[%*d]\n", -5, 42}'
compare 'printf precision on an integer' /dev/null 'BEGIN{printf "[%.5d][%.0d]\n", 42, 0}'
compare 'printf no newline' /dev/null 'BEGIN{printf "%s", "x"}'
compare 'printf zero and minus' /dev/null 'BEGIN{printf "[%05.1f][%-8.3e][%+.2f]\n", -3.14159, 0.000123, -0}'
compare 'printf builder grows' /dev/null 'BEGIN{printf "[%1500s][%01500d][%-1500s]\n", "x", 7, "y"}'
compare 'ofmt' /dev/null 'BEGIN{OFMT="%.2f"; print 3.14159; print 3.14159 ""}'
compare 'convfmt' /dev/null 'BEGIN{CONVFMT="%.2g"; x=3.14159; print x ""; print x}'
compare 'ofmt leaves integers' /dev/null 'BEGIN{OFMT="%.2f"; print 3, 3.0, 100000}'
compare 'big integers print whole' /dev/null 'BEGIN{print 2^53, 2^62, 2^64, 1e16, 1e17}'
compare 'small numbers' /dev/null 'BEGIN{print 1/3, 0.0000001, 1e-300, 2/7}'
compare 'number to string' /dev/null 'BEGIN{print 1/3 "x", 1000000 "x", 0.5 "x"}'

#
#       getline, in its forms.
#

case_start getline
compare 'plain' "$work/letters" 'NR==1{getline; print $0, NR, NF}'
compare 'into a variable' "$work/letters" 'NR==1{getline x; print x, $0, NR}'
compare 'into a field' "$work/grid" 'NR==1{getline $2; print $0, NF}'
compare 'at the end' "$work/one" '{print (getline), NR}'
compare 'from a file' /dev/null 'BEGIN{while ((getline l < "'"$work/letters"'") > 0) n++; print n}'
compare 'from a file into the record' /dev/null 'BEGIN{getline < "'"$work/grid"'"; print $0, NF, NR}'
compare 'from a file that is not there' /dev/null 'BEGIN{print (getline x < "/nonesuch/at/all")}'
compare 'reread after close' /dev/null 'BEGIN{f="'"$work/one"'"; getline a < f; close(f); getline b < f; print a, b}'
compare 'from a command' /dev/null 'BEGIN{"echo hi" | getline x; print x}'
compare 'from a command into the record' /dev/null 'BEGIN{"echo a b" | getline; print $0, NF}'
compare 'command in a loop' /dev/null 'BEGIN{while (("printf \"1\\n2\\n3\\n\"" | getline l) > 0) n++; print n}'
compare 'command status' /dev/null 'BEGIN{cmd="echo x"; cmd | getline; print close(cmd)}'
compare 'main input in a loop' "$work/letters" 'NR==1{while ((getline line) > 0) n++; print n, NR}'

#
#       Redirection.
#

case_start redirect
compare_effect 'print to a file' /dev/null 'BEGIN{print "a" > "out"; print "b" > "out"; close("out")}'
compare_effect 'append' /dev/null 'BEGIN{print "a" > "out"; close("out"); print "b" >> "out"; close("out")}'
compare_effect 'printf to a file' /dev/null 'BEGIN{printf "%s|", "z" > "out"}'
compare_effect 'two files' /dev/null 'BEGIN{print "a" > "one"; print "b" > "two"}'
compare_effect 'field to a file' "$work/grid" '{print $1 > "col"}'
compare 'print to a pipe' /dev/null 'BEGIN{print "b\na" | "sort"; close("sort"); print "done"}'
compare 'printf to a pipe' /dev/null 'BEGIN{printf "%s\n%s\n", "b", "a" | "sort"}'
compare 'greater than is a redirect' /dev/null 'BEGIN{print (1>2)}'
compare 'comparison in parentheses' /dev/null 'BEGIN{print (1<2), (2<1)}'
compare 'print list to a file' /dev/null 'BEGIN{print 1, 2 > "/dev/stdout"}'
compare 'stdout by name' /dev/null 'BEGIN{print "x" > "/dev/stdout"}'

#
#       Functions.
#

case_start function
compare 'call' /dev/null 'function f(x) {return x*2} BEGIN{print f(3)}'
compare 'called before defined' /dev/null 'BEGIN{print f(2)} function f(x){return x+1}'
compare 'locals' /dev/null 'function f(a,b,  c){c=a+b; return c} BEGIN{print f(1,2), "[" c "]"}'
compare 'no return value' /dev/null 'function f(){return} BEGIN{x=f(); print "[" x "]", x+0}'
compare 'no return at all' /dev/null 'function f(){x=1} BEGIN{print "[" f() "]"}'
compare 'recursion' /dev/null 'function f(n){return n<2?1:n*f(n-1)} BEGIN{print f(10)}'
compare 'deep recursion' /dev/null 'function f(n){return n==0?0:1+f(n-1)} BEGIN{print f(200)}'
compare 'array by reference' /dev/null 'function f(a){a["k"]=1} BEGIN{f(x); print x["k"], length(x)}'
compare 'array through two calls' /dev/null 'function h(b){b["k"]="v"} function g(a){h(a)} BEGIN{g(x); print x["k"]}'
compare 'scalar by value' /dev/null 'function f(x){x=9} BEGIN{y=1; f(y); print y}'
compare 'fills an array' /dev/null 'function g(a,i){for(i=1;i<=3;i++)a[i]=i} BEGIN{g(arr); print arr[1] arr[2] arr[3]}'
compare 'globals visible' /dev/null 'function f(){return g} BEGIN{g=7; print f()}'
compare 'fields in a function' "$work/grid" 'function f(){return $1} {print f()}'
compare 'next out of a function' "$work/letters" 'function f(){if (NR==2) return 1; return 0} {if (f()) next; print}'
compare 'exit out of a function' /dev/null 'function f(){exit 2} BEGIN{f(); print "no"} END{print "end"}'
compare 'locals are fresh' /dev/null 'function f(n,  a){a[n]=1; return length(a)} BEGIN{print f(1), f(2)}'

#
#       Flags and operands.
#

case_start flags
compare 'dash v' /dev/null -v 'x=5' 'BEGIN{print x, x+1}'
compare 'dash v escapes' /dev/null -v 'x=a\tb' 'BEGIN{print length(x)}'
compare 'dash v is a strnum' /dev/null -v 'x=010' 'BEGIN{print (x==10), (x=="010")}'
compare 'two dash v' /dev/null -v 'x=1' -v 'y=2' 'BEGIN{print x+y}'
compare 'dash v joined' /dev/null -vx=5 'BEGIN{print x}'
compare 'dash F joined' "$work/colons" -F: '{print $2}'
compare 'dash F separate' "$work/colons" -F : '{print $2}'
compare 'assignment operand' "$work/one" '{print v, FILENAME}' v=1 "$work/one" v=2 "$work/one"
compare 'assignment before any file' "$work/one" 'BEGIN{print "[" v "]"} {print v}' v=9 "$work/one"
compare 'argv and argc' /dev/null 'BEGIN{print ARGC, ARGV[0], ARGV[1], ARGV[2]}' one two
compare 'environ' /dev/null 'BEGIN{print (("PATH" in ENVIRON) ? "yes" : "no"), (("NOSUCHVAR" in ENVIRON) ? "yes" : "no")}'
compare 'dash dash' "$work/one" -- '{print $0}'
compare 'standard input by name' "$work/one" '{print FILENAME, $0}' -
compare 'two files' /dev/null '{print FILENAME, FNR, NR}' "$work/one" "$work/grid"

case_start program-file
printf 'BEGIN { x = 1 }\n' > "$work/p1.awk"
printf 'BEGIN { print x + 1 }\n' > "$work/p2.awk"
compare 'one file' /dev/null -f "$work/p1.awk" -f "$work/p2.awk"
printf 'function twice(n) { return n * 2 }\nBEGIN { print twice(21) }\n' > "$work/p3.awk"
compare 'a function in a file' /dev/null -f "$work/p3.awk"

case_start more
compare 'dash v sets fs' "$work/colons" -v 'FS=:' '{print $2}'
compare 'dash v sets ofs' "$work/grid" -v 'OFS=-' '{$1=$1; print}'
compare 'dash v sets rs' "$work/colons" -v 'RS=:' '{print NR, $0}'
compare 'operand sets fs' "$work/one" '{print $2}' 'FS=:' "$work/colons"
compare 'subsep changed' /dev/null 'BEGIN{SUBSEP="-"; a[1,2]=1; for(k in a) print k}'
compare 'convfmt in a key' /dev/null 'BEGIN{CONVFMT="%.1f"; a[1.25]=1; for(k in a) print k}'
compare 'dynamic format' /dev/null 'BEGIN{f="%s-%d\n"; printf f, "a", 2}'
compare 'format from a field' "$work/grid" '{printf "%d:%d\n", $1, $2}'
compare 'assign to nr' "$work/letters" '{NR = 10; print NR}'
compare 'assign to fnr' "$work/letters" 'NR==1{FNR=99} {print FNR}'
compare 'assign to filename' "$work/one" '{FILENAME="x"; print FILENAME}'
compare 'rstart and rlength survive' /dev/null 'BEGIN{match("abc",/b/); x=RSTART; match("abc",/z/); print x, RSTART, RLENGTH}'
compare 'field assignment in end' "$work/grid" 'END{$2="X"; print; print NF}'
compare 'record assignment in end' "$work/grid" 'END{$0="a b"; print NF}'
compare 'ofs changed between' "$work/grid" '{OFS="-"; $1=$1; OFS="+"; print}'
compare 'empty fs field by field' "$work/one" -F'' '{print NF}'
compare 'many fields' /dev/null 'BEGIN{for(i=1;i<=100;i++) s = s i " "; $0 = s; print NF, $50, $100}'
compare 'array passed empty' /dev/null 'function f(a){return length(a)} BEGIN{print f(x), length(x)}'
compare 'array grown in a call' /dev/null 'function f(a,n,i){for(i=1;i<=n;i++)a[i]=i} BEGIN{f(z,5); print length(z), z[3]}'
compare 'local array is fresh' /dev/null 'function f(k,  a){a[k]=1; return length(a)} BEGIN{print f(1) f(2) f(3)}'
compare 'string of a number in a key' /dev/null 'BEGIN{a[1]=1; a["1"]=2; print length(a), a[1]}'
compare 'negative zero' /dev/null 'BEGIN{print -0, 0*-1, -0 ""}'
compare 'very long string' /dev/null 'BEGIN{s=""; for(i=0;i<5000;i++) s=s "ab"; print length(s), substr(s,9999)}'
compare 'nested function calls' /dev/null 'function a(x){return b(x)+1} function b(x){return x*2} BEGIN{print a(5)}'
compare 'function changes a global' /dev/null 'function f(){g++} BEGIN{f();f();print g}'
compare 'concatenation in a condition' "$work/grid" '{if ($1 $2 == "12") print "yes"}'
compare 'increment an element' /dev/null 'BEGIN{a["k"]++; a["k"]+=2; print a["k"]}'
compare 'delete while walking' /dev/null 'BEGIN{for(i=1;i<=5;i++)a[i]=i; for(k in a) if (k%2) delete a[k]; print length(a)}'
compare 'uninitialised in printf' /dev/null 'BEGIN{printf "[%s][%d][%5.2f]\n", u, u, u}'
compare 'regex with a slash' "$work/one" '$0 ~ /x/ {print "yes"}'
compare 'regex escapes' /dev/null 'BEGIN{print ("a.b" ~ /a\.b/), ("axb" ~ /a\.b/), ("a/b" ~ /a\/b/)}'
compare 'regex classes' /dev/null 'BEGIN{print ("a1" ~ /[[:alpha:]][[:digit:]]/), ("11" ~ /[[:alpha:]]/)}'
compare 'anchors' /dev/null 'BEGIN{print ("abc" ~ /^abc$/), ("abc" ~ /^b/), ("" ~ /^$/)}'
compare 'string escapes' /dev/null 'BEGIN{print "a\tb", "c\\d", "e\"f", length("\061\x41")}'
compare 'escape digit caps' /dev/null 'BEGIN{print "\1012", "\x414", "\1q", "\x@z"}'
compare 'assignment escapes' /dev/null -v 'x=\1412' 'BEGIN{print x}'

case_start edges
compare 'nextfile' /dev/null 'FNR==1{nextfile} {print FILENAME, $0}' "$work/letters" "$work/grid"
compare 'nextfile is the last file' /dev/null '{nextfile} END{print NR}' "$work/letters" "$work/grid"
compare 'getline in end' "$work/letters" 'END{print (getline), NR}'
compare 'getline in begin' "$work/letters" 'BEGIN{print (getline), $0, NR}'
compare 'close a pipe status' /dev/null 'BEGIN{c="exit 3"; print "x" | c; print close(c)}'
compare 'close a read pipe status' /dev/null 'BEGIN{c="exit 4"; c | getline x; print close(c)}'
compare 'a pipe with a lot in it' /dev/null 'BEGIN{for(i=0;i<20000;i++) print i | "wc -l"; close("wc -l")}'
compare 'getline a lot' /dev/null 'BEGIN{while (("seq 1 20000" | getline l) > 0) n++; print n}'
compare 'a very long record' /dev/null 'BEGIN{for(i=0;i<100000;i++) s = s "x"; $0 = s; print length($0), NF}'
compare 'substr of a nan' /dev/null 'BEGIN{print "[" substr("hello", log(-1), 2) "]"}'
compare 'substr of an infinity' /dev/null 'BEGIN{x = 1e300*1e300; print "[" substr("hello", 1, x) "]", "[" substr("hello", x) "]"}'
compare 'split on an empty match' /dev/null 'BEGIN{n=split("abc",a,"x*"); print n, a[1], a[2]}'
compare 'record set in begin' /dev/null 'BEGIN{$0="a b c"; print NF, $2}'
compare 'record set empty' /dev/null 'BEGIN{$0=""; print NF, length($0)}'
compare 'deeply parenthesised' /dev/null 'BEGIN{print ((((((((((1+2))))))))))}'
compare 'many arguments' /dev/null 'BEGIN{printf "%s%s%s%s%s%s%s%s%s%s\n",1,2,3,4,5,6,7,8,9,10}'
compare 'a field far past the end' /dev/null 'BEGIN{$0="a"; print "[" $1000 "]", NF}'
compare 'ors is not printf' /dev/null 'BEGIN{ORS="X"; printf "a\n"}'
compare 'end only exit' /dev/null 'END{exit 7}'
compare 'a file that is not there' /dev/null '{print}' "$work/letters" /nonesuch/at/all "$work/grid"
#       Not printed, only compared: which way the sign bit of a value that
#       is not a number falls is the machine's answer and not awk's, and the
#       three machines here do not agree with each other about it.
compare 'a nan through arithmetic' /dev/null 'BEGIN{x = log(-1); print (x==x), (x!=x), (x<1), (x>1), (x<=1), (x>=1)}'
compare 'a nan against itself' /dev/null 'BEGIN{x = sqrt(-1); y = x + 1; print (y==y), (x==0), length(x "")}'
compare 'infinities' /dev/null 'BEGIN{x=1e300*1e300; print x, -x, 1/x, (x>1e308), (x==x)}'
compare 'thirty six patterns' /dev/null 'BEGIN{s="abcdefghij"; n=0
if (s~/a/) n++; if (s~/b/) n++; if (s~/c/) n++; if (s~/d/) n++; if (s~/e/) n++
if (s~/f/) n++; if (s~/g/) n++; if (s~/h/) n++; if (s~/i/) n++; if (s~/j/) n++
if (s~/ab/) n++; if (s~/bc/) n++; if (s~/cd/) n++; if (s~/de/) n++; if (s~/ef/) n++
if (s~/fg/) n++; if (s~/gh/) n++; if (s~/hi/) n++; if (s~/ij/) n++; if (s~/ja/) n++
if (s~/a.c/) n++; if (s~/b.d/) n++; if (s~/c.e/) n++; if (s~/d.f/) n++; if (s~/e.g/) n++
if (s~/x/) n++; if (s~/y/) n++; if (s~/z/) n++; if (s~/aa/) n++; if (s~/bb/) n++
if (s~/[a-c]/) n++; if (s~/[x-z]/) n++; if (s~/a+/) n++; if (s~/q*/) n++; if (s~/^a/) n++
if (s~/(ab|cd)/) n++; if (s~/z|a/) n++
print n}'
compare 'patterns built at run time' "$work/letters" '{r = "^" $0 "$"; if ($0 ~ r) n++} END{print n}'
compare 'static and dynamic together' "$work/letters" '{if (/a/) x++; r="^" $0; if ($0 ~ r) y++; if (/b/) z++} END{print x, y, z}'

#       A call with the wrong number of arguments is refused rather than
#       reached for. gawk answers 1 for most of these and 2 for two of them,
#       so what is compared is that the answer is an error and not a signal
#       and that nothing was printed.
case_start arity
for wrong in 'sin()' 'cos()' 'substr("x")' 'split("x")' 'index("x")' \
        'atan2(1)' 'match("x")' 'sub(/a/)' 'sprintf()' 'toupper()' 'tolower()' \
        'close()' 'system()' 'exp()' 'log()' 'sqrt()' 'int()' 'rand(1)' \
        'length(1,2)' 'substr("a",1,2,3)' 'split("a","b","c")' 'srand(1,2)' \
        'gsub(/a/)' 'match("a","b","c")'; do
        "$ours" "BEGIN{ $wrong }" > "$work/got" 2> /dev/null
        got_status=$?
        : > "$work/want"
        want_status=$got_status

        if [ "$got_status" -gt 0 ] && [ "$got_status" -lt 128 ] &&
                [ ! -s "$work/got" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                printf '  %-9s %-26s status %s\n' arity "$wrong" "$got_status"
        fi
done

#
#       What is not here.
#
#       Written down as cases so the gap is visible rather than surprising:
#       each of these is a place where this awk and the reference part, and
#       the case says which way. They are counted as passes only if they
#       still behave the way the comment says.
#

case_start known-gaps
#       gawk stops with a fatal error when a format wants more arguments
#       than it was given; this one uses the empty string and zero, which is
#       what POSIX says and what the other awks do.
printf '%s' "$("$ours" 'BEGIN{printf "[%s][%d]\n"}' 2> /dev/null)" > "$work/got"
if [ "$(cat "$work/got")" = "[][0]" ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps printf short of arguments: $(cat "$work/got")"
fi

#       The functions gawk adds and POSIX does not have are not here. Each
#       is an undefined function rather than something that quietly does the
#       wrong thing, which is the difference that matters to a script.
for absent in 'gensub(/a/,"b","g","aa")' 'systime()' 'strftime("%Y")' \
        'asort(a)' 'asorti(a)' 'mktime("2026 01 01 0 0 0")' 'toupper()'; do
        "$ours" "BEGIN{ x = $absent }" > "$work/got" 2> /dev/null
        got_status=$?

        if [ "$got_status" -gt 0 ] && [ "$got_status" -lt 128 ] &&
                [ ! -s "$work/got" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                echo "  known-gaps $absent answered $got_status"
        fi
done

#       IGNORECASE and RT are gawk's and mean nothing here, so they are
#       ordinary variables and a pattern is still matched case sensitively.
if [ "$("$ours" 'BEGIN{IGNORECASE=1; print ("A" ~ /a/), "[" RT "]"}' 2> /dev/null)" \
        = "0 []" ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps IGNORECASE and RT are not special"
fi

#       Recursion goes as deep as the stack the machine gave this process
#       and then stops with an error. gawk goes further; what is being
#       checked here is that the end of it is an answer and not a signal.
"$ours" 'function f(n){return n==0?0:1+f(n-1)} BEGIN{print f(1000)}' > "$work/got" 2>&1
shallow=$?
"$ours" 'function f(n){return n==0?0:1+f(n-1)} BEGIN{print f(1000000)}' > /dev/null 2>&1
deep=$?

if [ "$shallow" = 0 ] && [ "$(cat "$work/got")" = 1000 ] &&
        [ "$deep" -gt 0 ] && [ "$deep" -lt 128 ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps recursion: shallow $shallow deep $deep"
fi

#       gawk refuses next in an END action at parse time. This runs it and
#       treats it as the end of the action.
if "$ours" 'END{next}' < /dev/null > /dev/null 2>&1; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps next in END did not run"
fi

#       gawk refuses at run time to use a scalar as an array or an array as a
#       scalar. This does neither: an array read where a scalar goes is the
#       empty string, and a scalar subscripted starts an array beside it.
if [ "$("$ours" 'BEGIN{a[1]=1; print "[" a "]"}' 2> /dev/null)" = "[]" ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps an array read as a scalar"
fi

#       Neither uninitialised nor a strnum: a name followed by a space and a
#       parenthesis is a call here and an error in gawk.
if [ "$("$ours" 'function foo(x){return x} BEGIN{print foo (3)}' 2> /dev/null)" = "3" ]; then
        pass=$((pass + 1))
else
        fail=$((fail + 1))
        echo "  known-gaps a call with a space before the parenthesis"
fi

printf '\n'
printf '  %-12s %s of %s\n' listed "$pass" "$((pass + fail))"

listed_fail=$fail

#
#       Generated cases.
#
#       The seed is fixed, so the same questions are asked every run and a
#       failure can be reproduced by running it again rather than by being
#       lucky twice.
#

if ! command -v python3 > /dev/null 2>&1; then
        echo "  generated    skipped (needs python3)"
        [ "$listed_fail" = 0 ]
        exit
fi

OURS=$ours ROUNDS=$rounds python3 - <<'PYTHON' > "$work/generated" 2>&1
import os, random, subprocess

ours = os.environ["OURS"]
rounds = int(os.environ["ROUNDS"])
total = 0
bad = 0
shown = 0


def report(script, data, want, got):
    global shown
    shown += 1
    if shown <= 10:
        print("  %-58s want %r got %r" % (script[:58], want, got))
        print("         on %r" % data[:60])


#      Bytes rather than text: %c can write one that is not a character in
#      any encoding, and decoding it would fail before the comparison.
def run(program, arguments, data):
    got = subprocess.run([program] + arguments, input=data.encode(),
                         capture_output=True, timeout=20)
    return got.stdout, got.returncode


def both(arguments, data):
    global total, bad
    total += 1
    want, want_status = run("awk", arguments, data)
    mine, mine_status = run(ours, arguments, data)

    if want != mine or want_status != mine_status:
        bad += 1
        report(" ".join(arguments), data, (want, want_status), (mine, mine_status))


random.seed(20260828)

#       Numbers, printed and formatted. Every one of these is a place where a
#       conversion between a double and its digits can be off by one.
values = ["0", "1", "-1", "0.5", "-0.5", "1/3", "2/3", "1e6", "1e-6", "1e16",
          "1e17", "2^53", "2^53+1", "2^62", "1e300", "1e-300", "123456.789",
          "0.1+0.2", "100000", "1000000", "3.0", "-0.0", "1e100", "9.995",
          "0.0001", "0.00001", "12345678901234567890", "1.005", "255", "65536"]

formats = ["%d", "%i", "%5d", "%-5d", "%05d", "%+d", "% d", "%.3d",
           "%o", "%x", "%X", "%u", "%#o", "%#x",
           "%f", "%.0f", "%.1f", "%.10f", "%12.3f", "%-12.2f", "%+.2f",
           "%e", "%.0e", "%.3e", "%E", "%15.4e",
           "%g", "%G", "%.1g", "%.3g", "%.10g", "%#g", "%.0g",
           "%s", "%10s", "%-10s", "%.3s"]

for _ in range(rounds):
    value = random.choice(values)
    both(["BEGIN{ print %s }" % value], "")
    both(["BEGIN{ x = %s; print x \"\" }" % value], "")
    both(["BEGIN{ printf \"[%s]\\n\", %s }" % (random.choice(formats), value)], "")
    both(["BEGIN{ printf \"[%%c][%%c]\\n\", %s, \"%s\" }"
          % (value if len(value) < 4 else "65", random.choice(["A", "65", "", "z9"]))], "")

for _ in range(rounds // 2):
    a = random.choice(values)
    b = random.choice(values)
    operator = random.choice(["+", "-", "*", "%"])
    both(["BEGIN{ print %s %s %s }" % (a, operator, b)], "")
    both(["BEGIN{ print (%s < %s), (%s == %s) }" % (a, b, a, b)], "")

#       Fields, separators and records.
def lines(count, longest, alphabet):
    return "\n".join(
        "".join(random.choice(alphabet) for _ in range(random.randint(0, longest)))
        for _ in range(count)) + "\n"


separators = [" ", ":", ",", "\t", "ab", "[0-9]", "[,:]", "a+", " *", "|", ".",
              "]", "\\\\.", "x"]

programs = ["{print NF}", "{print $1}", "{print $2 \"|\" $NF}",
            "{print NR, NF, $0}", "{$1=$1; print}", "{NF=2; print; print NF}",
            "{print length($0), length($1)}", "{$3=\"X\"; print NF, $0}",
            "{for(i=1;i<=NF;i++) printf \"[%s]\", $i; print \"\"}"]

for _ in range(rounds):
    separator = random.choice(separators)
    data = lines(6, 8, "ab:, \tx1")
    both(["-F", separator, random.choice(programs)], data)

for _ in range(rounds // 2):
    data = lines(6, 8, "ab:, \tx1")
    both(["BEGIN{RS=\"%s\"}{print NR, \"[\" $0 \"]\"}" % random.choice([":", "x", "\\n", "[0-9]", "ab"])], data)
    both(["BEGIN{OFS=\"%s\"}{$1=$1; print}" % random.choice(["-", "", ":", "||"])], data)

#       The string and number rules, asked of values that look like numbers
#       and values that do not.
looks = ["1", "10", "10.0", "010", "1e2", "+3", "-0", " 7 ", "0", "", "abc",
         "1x", "3.", ".5", "0x10", "1e", "  ", "00", "1.0e1", "inf", "nan"]

for _ in range(rounds):
    left = random.choice(looks)
    right = random.choice(looks)
    data = "%s\n%s\n" % (left, right)
    both(["NR==1{a=$1} NR==2{print (a<$1), (a==$1), (a>$1)}"], data)
    both(["NR==1{print ($1==%s), ($1==\"%s\"), ($1<%s)}"
          % (random.choice(["0", "1", "10"]), left, random.choice(["0", "1", "10"]))], data)
    both(["{print ($0+0), length($0), ($0==\"\")}"], data)

#       Patterns, through the same engine grep and sed use.
patterns = ["a", "ab", "a*", "a+", "[ab]", "[^a]", ".", "^a", "b$", "a|b",
            "(ab)+", "a{2}", "a{1,2}", "[a-c]+", "(a|bc)*", "x?b", ".*a",
            "^$", "^ab$", "[[:digit:]]"]

for _ in range(rounds):
    pattern = random.choice(patterns)
    data = lines(6, 6, "abc1 ")
    both(["/%s/{print NR}" % pattern], data)
    both(["{print ($0 ~ /%s/)}" % pattern], data)
    both(["{n=gsub(/%s/,\"<&>\"); print n, $0}" % pattern], data)
    both(["{print match($0,/%s/), RSTART, RLENGTH}" % pattern], data)
    both(["{n=split($0,a,/%s/); print n, \"[\" a[1] \"]\", \"[\" a[n] \"]\"}" % pattern], data)

#       String functions over generated arguments.
for _ in range(rounds // 2):
    text = "".join(random.choice("abcab ") for _ in range(random.randint(0, 8)))
    start = random.choice(["-2", "0", "1", "2", "3", "9", "1.5", "2.7"])
    count = random.choice(["-1", "0", "1", "2", "5", "99", "2.5"])
    both(["BEGIN{print \"[\" substr(\"%s\",%s) \"]\", \"[\" substr(\"%s\",%s,%s) \"]\"}"
          % (text, start, text, start, count)], "")
    both(["BEGIN{print index(\"%s\",\"%s\"), length(\"%s\")}"
          % (text, text[1:3], text)], "")
    both(["BEGIN{s=\"%s\"; n=sub(/a/,\"[&]\",s); print n, s}" % text], "")
    both(["BEGIN{print toupper(\"%s\") tolower(\"%s\")}" % (text, text)], "")

#       Control flow, arrays and functions, built rather than listed.
bodies = [
    "{ for (i = 1; i <= NF; i++) s = s $i; print s }",
    "{ n = 0; while (n < NF) { n++; if (n == 2) continue; printf \"%s.\", $n }; print \"\" }",
    "{ a[$1]++ } END { n = 0; for (k in a) n += a[k]; print n, length(a) }",
    "{ if (NF > 2) print \"big\"; else if (NF == 0) print \"none\"; else print \"small\" }",
    "{ do { x++ } while (x < NR); print x }",
    "function f(n) { return n <= 1 ? 1 : n * f(n - 1) } { print f(NF) }",
    "function g(a, i) { for (i in a) delete a[i]; return length(a) } { split($0, b); print g(b) }",
    "{ for (i = NF; i > 0; i--) printf \"%s|\", $i; print \"\" }",
    "NR % 2 { next } { print NR }",
    "{ $2 = \"Z\"; print NF, $0 }",
    "{ sub(/a/, \"[&]\"); print }",
    "{ print length($0), length($1), length() }",
    "{ x = $1; y = $2; print (x < y), (x == y), (x \"\" == y \"\") }",
    "{ printf \"%s %d %.2f\\n\", $1, $2, $3 }",
    "{ n = split($0, p, \"a\"); for (i = 1; i <= n; i++) t = t \"<\" p[i] \">\"; print t }",
    "END { print NR, NF, $0 }",
    "{ print toupper($1) tolower($2) }",
    "{ if (match($0, /a+/)) print RSTART, RLENGTH; else print \"no\" }",
    "{ a[NR] = $0 } END { for (i = NR; i >= 1; i--) print a[i] }",
    "{ getline line; print NR, \"[\" line \"]\", $0 }",
    "{ print $1 > \"/dev/stdout\" }",
    "{ print; exit NF }",
]

for _ in range(rounds):
    data = lines(random.randint(1, 5), 7, "ab c1 ")
    both([random.choice(bodies)], data)

#       Whole programs made of several rules at once.
for _ in range(rounds // 2):
    parts = [random.choice(bodies) for _ in range(random.randint(1, 3))]
    data = lines(random.randint(1, 5), 7, "ab c1 ")
    both(["\n".join(parts)], data)

#       The same input through every separator arrangement at once.
for _ in range(rounds // 2):
    data = lines(4, 8, "ab:, \tx1\n")
    both(["BEGIN{FS=\"%s\";OFS=\"%s\";ORS=\"%s\"}{$1=$1; print NF, $0}"
          % (random.choice([":", ",", " ", "[,:]", "ab"]),
             random.choice(["-", "", "::"]),
             random.choice(["\\n", "|", ""]))], data)

#       Programs that are mostly not programs.
#
#       Nothing here compares against the reference: what is being asked is
#       only that a program made of tokens in an order nobody meant is an
#       error rather than a signal. A parser reached for a null in eleven
#       places before this asked.
tokens = ["BEGIN", "END", "{", "}", "(", ")", "[", "]", ";", ",", "$", "print",
          "printf", "if", "else", "while", "for", "do", "in", "delete",
          "getline", "function", "return", "next", "exit", "break", "continue",
          "+", "-", "*", "/", "%", "^", "=", "==", "!=", "<", ">", "<=", ">=",
          "&&", "||", "!", "~", "!~", "?", ":", "++", "--", "+=", "-=", "|",
          ">>", "x", "y", "a", "1", "2", "\"s\"", "/re/", "NF", "NR", "$0",
          "$1", "length", "substr", "split", "sub", "gsub", "sprintf", "sin",
          "int", "\n", "# comment\n"]

for _ in range(rounds * 2):
    total += 1
    program = " ".join(random.choice(tokens)
                       for _ in range(random.randint(1, 14)))
    mine, status = run(ours, [program], "a b\nc d\n")

    if status < 0 or status >= 128:
        bad += 1
        report(program, "", ("no signal", 0), ("", status))

print("\n  %s of %s" % (total - bad, total))
PYTHON

sed -n '/want/p' "$work/generated"
sed -n '/on /p' "$work/generated"

line=$(sed -n 's/^  \([0-9]*\) of \([0-9]*\)$/\1 \2/p' "$work/generated")

if [ -z "$line" ]; then
        echo "  generated    printed no verdict"
        sed 's/^/    /' "$work/generated" | tail -5
        exit 1
fi

made=${line% *}
made_total=${line#* }

printf '  %-12s %s of %s\n' generated "$made" "$made_total"

[ -z "${TEST_TALLY:-}" ] || {
        printf 'awk-listed %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"
        printf 'awk-generated %s %s\n' "$made" "$made_total" >> "$TEST_TALLY"
}

[ "$listed_fail" = 0 ] && [ "$made" = "$made_total" ]
