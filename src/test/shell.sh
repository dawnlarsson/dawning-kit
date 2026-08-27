#!/bin/sh
#
#       The shell: what POSIX asks of it, what it answers to, and what it
#       does not have.
#
#           sh src/test/shell.sh [shell] [directory of the names it answers to]
#
#       Every case in the first section is a fragment from the shell command
#       language in POSIX XCU section 2. Each is run through dash, which is
#       taken as the reference because it is the smallest thing that is
#       actually correct, and through the shell being tested; agreeing with
#       dash is passing.
#
#       That is deliberately a harder bar than "does not crash": a shell that
#       prints nothing agrees with nothing, and a shell that prints something
#       almost right fails loudly instead of being counted.
#
#       Four kinds of case, because four things are being asked:
#
#         check     the two shells write the same bytes to standard output.
#         answer    the same bytes and the same exit status. The status a
#                   script leaves behind is what every caller of it reads, and
#                   for a long time nothing here looked at it.
#         differs   ours and dash disagree, and this is exactly what ours says
#                   today. Written down rather than left out, so that closing
#                   the gap fails here and says so.
#         absent    a name the shell has no answer for, and the status it
#                   gives instead.
#
set -e

subject=${1:-/tmp/mwsh}
names=${2:-}
reference=${3:-/bin/dash}

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x "$reference" ] || { echo "no reference at $reference" >&2; exit 1; }

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

# A case is a name and a fragment. The fragment is fed to both shells on
# standard input, which is how a script arrives, and the two outputs compared.
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

shown() { head -c 40 "$1" | tr '\n' '|'; }

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

# What ours says where dash says something else. The recorded output has its
# newlines written as | so a case stays one line. Both halves are checked: if
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

# A name with nothing behind it. PATH is emptied first, so what is being
# asked is whether the shell has the thing itself rather than whether this
# machine happens to have one.
absent()
{
        name=$1
        want=$2
        shift 2
        printf 'PATH=\n%s\n' "$*" > "$work/case.sh"

        timeout 5 "$subject" < "$work/case.sh" > "$work/got" 2>/dev/null || true

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$want" ]; then
                won
                return 0
        fi

        lost "$name" "expected $want, got $got_ours"
}

section posix
group quoting
check 'single'          "echo 'a b'"
check 'double'          'echo "a b"'
check 'backslash'       'echo a\ b'
check 'quoted dollar'   "echo '\$x'"
check 'escaped quote'   'echo "a\"b"'
check 'empty'           'echo ""'
check 'adjacent'        'echo a"b"c'

group parameters
check 'plain'           'x=1; echo $x'
check 'braced'          'x=1; echo ${x}'
check 'default'         'echo ${u-def}'
check 'default colon'   'echo ${u:-def}'
check 'assign'          'echo ${u:=def}$u'
check 'alternate'       'x=1; echo ${x:+yes}'
check 'length'          'x=abcd; echo ${#x}'
check 'suffix'          'x=a.b.c; echo ${x%.*}'
check 'suffix greedy'   'x=a.b.c; echo ${x%%.*}'
check 'prefix'          'x=a.b.c; echo ${x#*.}'
check 'prefix greedy'   'x=a.b.c; echo ${x##*.}'
check 'unset'           'echo "[$nosuch]"'
check 'positional'      'set -- a b c; echo $2'
check 'count'           'set -- a b c; echo $#'
check 'all'             'set -- a b; echo $@'
check 'status'          'true; echo $?'
check 'status false'    'false; echo $?'

group expansion
check 'command sub'     'echo $(echo hi)'
check 'backtick'        'echo `echo hi`'
check 'nested sub'      'echo $(echo $(echo deep))'
check 'arithmetic'      'echo $((2 + 3))'
check 'arith vars'      'x=4; echo $((x * 2))'
check 'arith compare'   'echo $((3 > 2))'
check 'field split'     'x="a b c"; set -- $x; echo $#'
check 'glob'            'cd /; echo /de*'
check 'tilde'           'HOME=/tmp; echo ~'

group control
check 'if true'         'if true; then echo yes; fi'
check 'if else'         'if false; then echo a; else echo b; fi'
check 'elif'            'if false; then echo a; elif true; then echo b; fi'
check 'while'           'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
check 'until'           'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
check 'for'             'for i in a b c; do echo $i; done'
check 'for positional'  'set -- x y; for i; do echo $i; done'
check 'case'            'case abc in a*) echo match;; *) echo no;; esac'
check 'break'           'for i in 1 2 3; do [ $i = 2 ] && break; echo $i; done'
check 'continue'        'for i in 1 2 3; do [ $i = 2 ] && continue; echo $i; done'
check 'subshell'        '(echo inside); echo outside'
check 'group'           '{ echo a; echo b; }'

group operators
check 'and'             'true && echo yes'
check 'or'              'false || echo yes'
check 'not'             'if ! false; then echo yes; fi'
check 'sequence'        'echo a; echo b'
check 'pipe'            'echo hello | cat'
check 'pipe chain'      'echo a | cat | cat'

group functions
check 'define call'     'f() { echo body; }; f'
check 'args'            'f() { echo $1; }; f arg'
check 'return'          'f() { return 3; }; f; echo $?'

group redirection
check 'out'             'echo x > /tmp/pt1; cat /tmp/pt1'
check 'append'          'echo a > /tmp/pt2; echo b >> /tmp/pt2; cat /tmp/pt2'
check 'in'              'echo z > /tmp/pt3; cat < /tmp/pt3'
check 'stderr'          'ls /nonexistent 2>/dev/null; echo done'
check 'stderr to out'   'ls /nonexistent 2>&1 | wc -l'
check 'heredoc'         'cat <<EOF
line
EOF'
check 'heredoc quoted'  'cat <<"EOF"
$notexpanded
EOF'

group builtins
check 'cd pwd'          'cd /; pwd'
check 'test string'     '[ a = a ] && echo yes'
check 'test number'     '[ 2 -gt 1 ] && echo yes'
check 'test file'       '[ -d / ] && echo yes'
check 'shift'           'set -- a b c; shift; echo $1'
check 'unset'           'x=1; unset x; echo "[$x]"'
check 'export'          'export E=1; echo $E'
check 'read'            'echo data | { read v; echo $v; }'
check 'eval'            'eval echo hi'
check 'exit status'     'sh -c "exit 4" 2>/dev/null; echo $?'
check 'true false'      'true; echo $?; false; echo $?'
check 'printf'          'printf "%s-%s\n" a b'

group arithmetic
check 'shift left'      'echo $((1<<4))'
check 'shift right'     'echo $((64>>3))'
check 'bit and'         'echo $((12&10))'
check 'bit or'          'echo $((12|3))'
check 'bit xor'         'echo $((12^10))'
check 'bit precedence'  'echo $((1|2&3)) $((2^3|4)) $((5&3|8))'
check 'bit not'         'echo $((7&~2))'
check 'shift chain'     'echo $((1<<3>>1))'
check 'plus equals'     'x=1; : $((x+=2)); echo $x'
check 'minus equals'    'x=9; : $((x-=4)); echo $x'
check 'times equals'    'x=3; : $((x*=4)); echo $x'
check 'divide equals'   'x=9; : $((x/=2)); echo $x'
check 'modulo equals'   'x=9; : $((x%=4)); echo $x'
check 'shift equals'    'x=1; : $((x<<=4)); echo $x'
check 'or equals'       'x=12; : $((x|=3)); echo $x'
check 'assign value'    'echo $((y=7)); echo $y'
check 'mixed'           'echo $((2+3*4)) $((0||1&&1))'

group sourcing
check 'dot runs'        'echo echo sourced > /tmp/pd1; . /tmp/pd1'
check 'dot sets'        'echo x=42 > /tmp/pd2; . /tmp/pd2; echo $x'
check 'dot status'      'echo false > /tmp/pd3; . /tmp/pd3; echo $?'
check 'dot missing'     '. /tmp/nosuchfile 2>/dev/null; echo $?'

group naming
check 'type builtin'    'type echo'
check 'type function'   'f() { :; }; type f'
check 'type missing'    'type nosuchthing 2>/dev/null; echo $?'
check 'command dash v'  'command -v echo'
check 'command runs'    'command echo hi'
check 'command v miss'  'command -v nosuchthing 2>/dev/null; echo $?'

group traps
check 'exit trap'       'trap "echo bye" EXIT; echo hi'
check 'exit trap code'  'trap "echo bye" EXIT; exit 3'
check 'exit trap gone'  'trap "echo bye" EXIT; trap - EXIT; echo hi'
check 'wait for all'    'true & wait; echo done'

#
#       The same language again, with the exit status looked at too.
#
#       A script's status is what its caller reads, and the section above
#       compares standard output alone -- which is how a shell that printed
#       the right thing and then said the wrong number about it went years
#       without anybody noticing.
#

section strict

group status
answer 'pipeline last'   'false | true; echo $?'
answer 'group status'    '{ exit 0; }; echo $?'
answer 'assignment'      'x=1; echo $?'
answer 'nothing at all'  ''
answer 'colon'           ': ; echo $?'
answer 'exit direct'     'exit 42'
answer 'exit bare'       'true; exit'
answer 'exit 300'        'exit 300'
answer 'and then or'     'true && false || true; echo $?'
answer 'while status'    'i=0; while [ $i -lt 2 ]; do i=$((i+1)); done; echo $?'
answer 'case no match'   'case x in a) echo a;; esac; echo $?'
answer 'nested sub'      'echo $(exit 2)$?; echo $?'

group quoting
answer 'quoted star'     'echo "*"'
answer 'nested quotes'   'echo "a'"'"'b"'
answer 'empty single'    "echo ''x''"
answer 'backslash dollar' 'echo \$x'
answer 'quote in word'   'echo ab"cd"ef'
answer 'quoted spaces'   'set -- "a  b"; echo $#'
answer 'unquoted spaces' 'set -- a  b; echo $#'
answer 'dollar in single' "echo '\$(echo hi)'"

group parameters
answer 'default nested'  'echo ${a:-${b:-x}}'
answer 'alternate unset' 'echo "[${u:+set}]"'
answer 'alternate empty' 'u=; echo "[${u:+set}]" "[${u+set}]"'
answer 'length unset'    'echo ${#nosuch}'
answer 'length braced'   'set -- a b c; echo ${#}'
answer 'star quoted'     'set -- a b; echo "$*"'
answer 'star ifs'        'IFS=-; set -- a b; echo "$*"'
answer 'dollar zero'     'echo ${0:+set}'
answer 'prefix no match' 'x=abc; echo ${x#z}'
answer 'suffix no match' 'x=abc; echo ${x%z}'
answer 'prefix all'      'x=abc; echo "[${x#abc}]"'
answer 'indirect eval'   'x=y; y=z; eval echo \$$x'
answer 'positional ten'  'set -- 1 2 3 4 5 6 7 8 9 10; echo ${10}'

group splitting
answer 'ifs colon'       'IFS=:; x=a:b:c; set -- $x; echo $#'
answer 'ifs empty'       'IFS=; x="a b"; set -- $x; echo $#'
answer 'read two lines'  'printf "1\n2\n" | { read a; read b; echo "$a$b"; }'
answer 'read no newline' 'printf "x" | { read v; echo "[$v]"; }'
answer 'read extra'      'echo a b c | { read x y; echo "$y"; }'
answer 'read joins'      'printf "a\\\\b\n" | { read v; printf "[%s]\n" "$v"; }'
answer 'read raw keeps'  'printf "a\\\\b\n" | { read -r v; printf "[%s]\n" "$v"; }'

group arithmetic
answer 'unary minus'     'echo $((-5 + 2))'
answer 'unary plus'      'echo $((+5))'
answer 'logical not'     'echo $((!0)) $((!5))'
answer 'parentheses'     'echo $(((2+3)*4))'
answer 'modulo'          'echo $((7%3)) $((-7%3))'
answer 'divide negative' 'echo $((-7/2))'
answer 'equality'        'echo $((3==3)) $((3!=3))'
answer 'logical pair'    'echo $((0||3)) $((1&&0))'
answer 'shift and add'   'echo $((2+3<6)) $((1<<2+1))'
answer 'unset variable'  'echo $((nosuch+1))'
answer 'inner spaces'    'echo $(( 1 + 2 ))'
answer 'four terms'      'echo $((1+2*3-4/2))'
answer 'left to right'   'echo $((100/10/2))'
answer 'shift negative'  'echo $((-8>>1))'
answer 'past thirty two' 'echo $((1<<40))'
answer 'xor precedence'  'echo $((1^2&3)) $((1|2^3))'
answer 'comparison run'  'echo $((1<2)) $((2<=2)) $((3>=4))'

group redirection
answer 'to stderr'       'echo x 1>&2 2>/dev/null; echo done'
answer 'stderr to pipe'  'sh -c "echo e 1>&2" 2>&1 | cat'
answer 'read write'      'echo abc > /tmp/sr1; exec 3<> /tmp/sr1; read v <&3; echo $v'
answer 'heredoc expand'  'x=1; cat <<EOF
$x
EOF'
answer 'heredoc escape'  'cat <<EOF
a\$b
EOF'
answer 'two heredocs'    'cat <<A; cat <<B
one
A
two
B'
answer 'order of words'  'echo x > /tmp/sr2 2>&1; cat /tmp/sr2'
answer 'loop redirected' 'for i in 1 2; do echo $i; done > /tmp/sr3; cat /tmp/sr3'

group control
answer 'case alternates' 'case b in a|b) echo yes;; esac'
answer 'case escaped'    'case "a*b" in a\*b) echo yes;; esac'
answer 'case class'      'case 5 in [0-9]) echo digit;; esac'
answer 'case catch all'  'case x in *) echo any;; esac'
answer 'while read'      'printf "1\n2\n" | while read v; do echo "[$v]"; done'
answer 'until once'      'until true; do echo no; done; echo done'
answer 'for nothing'     'for i in; do echo $i; done; echo done'
answer 'glob no match'   'cd /tmp; for i in nosuchglob*; do echo "$i"; done'
answer 'break two'       'for i in 1 2; do for j in a b; do break 2; done; echo $i; done; echo done'
answer 'continue two'    'for i in 1 2; do for j in a b; do continue 2; done; echo $i; done; echo done'
answer 'recursion'       'f() { [ $1 -gt 0 ] && { echo $1; f $(($1-1)); }; }; f 3'
answer 'function args'   'f() { set -- x; echo $1; }; set -- y; f; echo $1'
answer 'function in one' 'f() { g() { echo inner; }; g; }; f'

group builtins
answer 'printf kinds'    'printf "%d %s %c\n" 42 str x'
answer 'printf bases'    'printf "%x %o\n" 255 8'
answer 'printf short'    'printf "%s-%s\n" a'
answer 'printf repeats'  'printf "%s\n" a b c'
answer 'printf nothing'  'printf "%s" abc; echo'
answer 'printf percent'  'printf "100%%\n"'
answer 'echo minus n'    'echo -n hi; echo'
answer 'echo alone'      'echo; echo done'
answer 'test empty'      '[ -z "" ] && echo yes'
answer 'test negated'    '[ ! -z x ] && echo yes'
answer 'test and'        '[ 1 = 1 -a 2 = 2 ] && echo yes'
answer 'test or'         '[ 1 = 2 -o 2 = 2 ] && echo yes'
answer 'test parens'     '[ \( 1 = 1 \) ] && echo yes'
answer 'test file kinds' '[ -f /etc/hostname ] && [ -r / ] && [ -x / ] && echo yes'
answer 'test numbers'    '[ 1 -ne 2 ] && [ 1 -le 1 ] && [ 2 -ge 1 ] && echo yes'
answer 'pwd after cd'    'cd /tmp; pwd'
answer 'export to child' 'export X=1; sh -c "echo \$X"'
answer 'unset unknown'   'unset nosuch; echo $?'
answer 'alias runs'      'alias e=echo; e hi'
answer 'exec replaces'   'exec echo replaced; echo not reached'
answer 'trap listed'     'trap "echo x" EXIT; trap'
answer 'wait alone'      'wait; echo $?'
answer 'background wait' 'sleep 0 & wait $!; echo $?'

group globbing
answer 'question mark'   'cd /; echo /et?'
answer 'one of a class'  'cd /; echo /[e]tc'
answer 'no match kept'   'cd /tmp; echo nosuchthing*'
answer 'glob in value'   'cd /; x="/et*"; echo $x'
answer 'glob quoted'     'cd /; echo "/et*"'

group substitution
answer 'trailing gone'   'x=$(printf "a\n\n\n"); echo "[$x]"'
answer 'keeps the middle' 'x=$(printf "a\nb\n"); echo "$x"'
answer 'inside a string' 'echo "pre $(echo mid) post"'
answer 'quotes inside'   'echo "$(echo "inner")"'
answer 'status after'    'echo $(false); echo $?'
answer 'backtick nested' 'echo `echo \`echo deep\``'
answer 'two of them'     'x=$(echo a)$(echo b); echo $x'
answer 'inside arith'    'echo $(( $(echo 2) + 3 ))'

#
#       Every byte, including the ones a terminal will not show.
#
#       str() once included the string terminator, so every literal the
#       shell wrote carried a stray NUL after it. A terminal draws nothing
#       for that and no comparison here was looking at the bytes, so it
#       shipped for years. These read the output back through od.
#

group bytes
answer 'echo is three'   'echo hi | wc -c'
answer 'printf is one'   'printf x | wc -c'
answer 'no nul in echo'  'echo hi | tr -d "\0" | wc -c'
answer 'no nul in printf' 'printf "a\nb\n" | tr -d "\0" | wc -c'
answer 'no nul in pwd'   'cd /; pwd | tr -d "\0" | wc -c'
answer 'no nul in a sub' 'x=$(echo hi); printf "%s" "$x" | tr -d "\0" | wc -c'
answer 'no nul in a loop' 'for i in 1 2 3; do echo $i; done | tr -d "\0" | wc -c'
answer 'no nul in type'  'type echo | tr -d "\0" | wc -c'

#
#       One binary, forty six names.
#
#       Every utility is a function inside the shell reached by the name the
#       binary was called as, so a link is the whole of what /bin/grep is.
#       Nothing tested that the name is what chooses, which is the only thing
#       holding the arrangement up.
#

section named

group dispatch

if [ -n "$names" ] && [ -d "$names" ]; then

emits()
{
        name=$1
        want=$2
        shift 2

        if /bin/sh -c "$*" > "$work/got" 2>/dev/null; then
                emitted_status=0
        else
                emitted_status=$?
        fi

        got_ours=$(shown "$work/got")

        if [ "$got_ours" = "$want" ]; then
                won
                return 0
        fi

        lost "$name" "expected $want, got ${got_ours}[$emitted_status]"
}

emits 'called grep'      'alpha|'  "printf 'alpha\nbeta\n' | '$names/grep' alpha"
emits 'called wc'        '2|'      "printf 'a\nb\n' | '$names/wc' -l | tr -d ' '"
emits 'called rev'       'cba|'    "printf 'abc\n' | '$names/rev'"
emits 'called basename'  'c|'      "'$names/basename' /a/b/c"
emits 'called seq'       '1|2|3|'  "'$names/seq' 3"
emits 'called uname'     'Linux|'  "'$names/uname'"
emits 'through a dot'    'cba|'    "printf 'abc\n' | '$names/./rev'"
emits 'link elsewhere'   'cba|'    "mkdir -p /tmp/sn && ln -sf '$names/rev' /tmp/sn/rev && printf 'abc\n' | /tmp/sn/rev"
emits 'another name'     'hi|'     "ln -sf '$names/rev' /tmp/notatool && printf 'echo hi\n' | /tmp/notatool"
emits 'the shell itself' 'hi|'     "printf 'echo hi\n' | '$subject'"
emits 'bare with no path' 'alpha|' "printf 'PATH=\necho alpha | grep alpha\n' | '$subject'"
emits 'type says utility' '0|'     "printf 'type grep > /dev/null; echo \$?\n' | '$subject'"
emits 'command v finds'  '0|'      "printf 'PATH=\ncommand -v grep > /dev/null; echo \$?\n' | '$subject'"
emits 'which finds'      '0|'      "printf 'PATH=\nwhich grep > /dev/null; echo \$?\n' | '$subject'"
emits 'help lists them'  '0|'      "printf 'help > /dev/null; echo \$?\n' | '$subject'"

fi

#
#       Where ours and dash part company.
#
#       Each of these is a thing the shell does not do yet, recorded as what
#       it does instead. A case here failing means the answer moved: either
#       the gap closed, in which case it belongs above, or something else
#       changed and nobody meant it to.
#

section differs

group status
differs 'builtin clears it' 'hi|1|' 1 'false; echo hi; echo $?'
differs 'pipeline leaves it' '1|' 1 'true | false; echo $?'
differs 'subshell keeps it' '5|' 5 '(exit 5); echo $?'
differs 'function keeps it' '4|' 4 'f() { return 4; }; f; echo $?'
differs 'body keeps it'  '1|' 1 'f() { false; }; f; echo $?'
differs 'loop keeps it'  '1|' 1 'for i in 1; do false; done; echo $?'
differs 'branch keeps it' 'b|' 1 'if true; then if false; then echo a; else echo b; fi; fi'
differs 'sub status lost' '0|' 0 'x=$(exit 3); echo $?'
differs 'unknown command' '127|' 127 'nosuchcommand12345; echo $?'
differs 'negative exit'  '' 255 'exit -1'

group arithmetic
differs 'no ternary'     '1|' 0 'echo $((1 ? 2 : 3))'
differs 'no hex'         '0|' 0 'echo $((0x10))'
differs 'no octal'       '10|' 0 'echo $((010))'
differs 'empty is zero'  '0|' 0 'echo $(( ))'
differs 'comma is not'   '1|' 0 'echo $((1,2))'
differs 'has increment'  '1 2|' 0 'x=1; echo $((x++)) $x'

group language
differs 'no continuation' 'a\|' 127 'echo a\
b'
differs 'echo keeps them' 'a\b|' 0 'echo "a\b"'
differs 'at is one word' '[a b c]|' 0 'set -- "a b" c; for i in "$@"; do echo "[$i]"; done'
differs 'no dash heredoc' '' 0 'cat <<-EOF
	indented
	EOF'
differs 'no exec fd'     '' 0 'exec 4>/tmp/sd1; echo hi >&4; exec 4>&-; cat /tmp/sd1'
differs 'no set e'       'not reached|' 1 'set -e; false; echo not reached'
differs 'no set u'       '|after|' 0 'set -u; echo $nosuch; echo after'
differs 'no cd dash'     '/|' 1 'cd /tmp; cd /; cd - > /dev/null; pwd'
differs 'read ignores ifs' 'a:b-|' 0 'IFS=: ; echo a:b | { read x y; echo "$x-$y"; }'
differs 'readonly is not' '0|' 0 'readonly r=1; r=2; echo $?'
differs 'unset dash f'   'a|0|' 0 'f() { echo a; }; unset -f f; f 2>/dev/null; echo $?'
differs 'shift past end' '1|' 1 'set -- a; shift 2; echo $?'
differs 'missing input'  '1|' 1 'cat < /nonexistent12345; echo $?'
differs 'closed fd'      '0|' 0 'echo x >&- 2>/dev/null; echo $?'

#
#       What the shell has no answer for at all.
#
#       PATH is emptied first, so these ask whether the shell carries the
#       thing rather than whether this machine has one lying about.
#

section absent

group missing
absent 'awk'      '127|' 'awk "{print}" < /dev/null; echo $?'
absent 'date'     '127|' 'date; echo $?'
absent 'expr'     '127|' 'expr 1 + 1; echo $?'
absent 'xargs'    '127|' 'echo a | xargs echo; echo $?'
absent 'kill'     '127|' 'kill -0 $$; echo $?'
absent 'dd'       '127|' 'dd if=/dev/null; echo $?'
absent 'cmp'      '127|' 'cmp /etc/hostname /etc/hostname; echo $?'
absent 'local'    '127|' 'f() { local v=1; }; f; echo $?'
absent 'command v awk' '127|' 'command -v awk > /dev/null; echo $?'
absent 'type awk' '127|' 'type awk > /dev/null 2>&1; echo $?'

section ""

total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
