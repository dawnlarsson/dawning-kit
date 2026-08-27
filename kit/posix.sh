#!/bin/sh
#
#       What POSIX asks of a shell, and whether ours does it.
#
#           sh kit/posix.sh [shell]
#
#       Every case below is a fragment from the shell command language in
#       POSIX XCU section 2. Each is run through dash, which is taken as the
#       reference because it is the smallest thing that is actually correct,
#       and through the shell being tested; agreeing with dash is passing.
#
#       That is deliberately a harder bar than "does not crash": a shell that
#       prints nothing agrees with nothing, and a shell that prints something
#       almost right fails loudly instead of being counted.
#
#       The list is not exhaustive and does not pretend to be. It is the parts
#       a script written by somebody else will actually use, in the order they
#       will hit them.
#
set -e

subject=${1:-/tmp/mwsh}
reference=${2:-/bin/dash}

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x "$reference" ] || { echo "no reference at $reference" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0
current=""

group() { current=$1; }

# A case is a name and a fragment. The fragment is fed to both shells on
# standard input, which is how a script arrives, and the two outputs compared.
check() {
        name=$1
        shift
        printf '%s\n' "$*" > "$work/case.sh"

        "$reference" < "$work/case.sh" > "$work/want" 2>/dev/null || true
        "$subject"   < "$work/case.sh" > "$work/got"  2>/dev/null || true

        if cmp -s "$work/want" "$work/got"; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-14s %-26s want %-22s got %s\n' \
                "$current" "$name" \
                "$(head -c 40 "$work/want" | tr '\n' '|')" \
                "$(head -c 40 "$work/got" | tr '\n' '|')"
}

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

total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"
