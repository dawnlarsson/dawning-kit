#!/bin/sh
#
#       What our shell does not answer the way bash and dash do.
#
#           sh kit/shell_gaps.sh [path-to-moonwater-shell]
#
#       One line per feature: "ok" when the byte-for-byte answer and the exit
#       status both match the reference, "GAP" with both answers when they do
#       not. The bash family is compared against /bin/bash and the POSIX
#       family against dash, because those are the two the test lanes pin
#       against and disagreeing with either is the thing worth seeing.
#
#       This is a map and not a test. src/test/shell.sh is the test: every gap
#       closed there moves a row out of bash_remaining or posix_remaining and
#       into the supported surface, and the harness refuses to let a closed
#       row stay in the ledger. Run this to decide what to work on next, run
#       the lane to know whether it works.
#
#       With no path, programs/shell.c is built into a temporary binary.
#
set -u
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export root
cd "$root" || exit 1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

OURS=${1:-}
if [ -z "$OURS" ]; then
        OURS=$work/shell
        ${CC:-gcc} -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
                -fno-builtin -w -T kit/spark.ld -Wl,-e,_start \
                -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
                -o "$OURS" programs/shell.c || exit 1
fi
probe() {
        ref=$1; name=$2; script=$3
        want=$($ref -c "$script" 2>&1; echo "[$?]")
        got=$($OURS -c "$script" 2>&1; echo "[$?]")
        if [ "$want" = "$got" ]; then echo "  ok    $name"; else printf '  GAP   %-34s want %s  got %s\n' "$name" "$(printf %s "$want" | tr '\n' '|' | cut -c1-60)" "$(printf %s "$got" | tr '\n' '|' | cut -c1-60)"; fi
}
B=/bin/bash; D=dash
echo "== bash features"

probe $B 'brace list' 'echo {a,b}{1,2}'
probe $B 'brace range' 'echo {1..5} {a..e} {01..03} {5..1..2}'
probe $B 'ansi c quoting' 'printf '\''%s\n'\'' $'\''a\tb\x41\101'\'''
probe $B 'locale quoting' 'echo $"hello"'
probe $B 'substring' 'v=abcdef; echo ${v:1:3} ${v: -2} ${v:2}'
probe $B 'indirection' 'a=b; b=c; echo ${!a}'
probe $B 'prefix names' 'ab1=1; ab2=2; echo ${!ab*} ${!ab@}'
probe $B 'case mod' 'v=aBc; echo ${v^^} ${v,,} ${v^} ${v,}'
probe $B 'pattern replace' 'v=aXbXc; echo ${v/X/-} ${v//X/-} ${v/#a/A} ${v/%c/C}'
probe $B 'quote transform' 'v="a b"; echo ${v@Q} ${v@a}'
probe $B 'length of star' 'set -- a b c; echo ${#*} ${#@}'
probe $B 'pipe stderr' 'ls /nonexistent |& wc -l'
probe $B 'redirect both' 'ls /nonexistent &> /dev/null; echo $?'
probe $B 'append both' ': &>> /dev/null; echo ok'
probe $B 'let' 'let x=3+4 y=x*2; echo $x $y'
probe $B 'printf -v' 'printf -v v "%03d" 7; echo $v'
probe $B 'printf %q' 'printf "%q\n" "a b"'
probe $B 'read -a' 'echo "1 2 3" | { read -a a; echo ${a[1]}; }'
probe $B 'read -n -N' 'printf "abcdef" | { read -n 2 x; read -N 3 y; echo $x $y; }'
probe $B 'read -s' 'echo x | { read -s v; echo $v; }'
probe $B 'RANDOM set' '[ -n "$RANDOM" ] && echo yes'
probe $B 'SECONDS LINENO' 'echo $SECONDS $LINENO'
probe $B 'EPOCHSECONDS' '[ "$EPOCHSECONDS" -gt 1000000 ] && echo yes'
probe $B 'PIPESTATUS' 'false | true; echo ${PIPESTATUS[0]} ${PIPESTATUS[1]}'
probe $B 'FUNCNAME' 'f() { echo $FUNCNAME; }; f'
probe $B 'caller' 'f() { caller; }; f'
probe $B 'BASH_VERSION set' '[ -n "$BASH_VERSION" ] && echo yes'
probe $B 'shopt query' 'shopt -q extglob; echo $?; shopt -s nullglob; shopt -q nullglob; echo $?'
probe $B 'nullglob' 'shopt -s nullglob; echo /nonexistent/*x; echo end'
probe $B 'nocaseglob' 'shopt -s nocaseglob; echo /TMP/MW-MERGE/KIT/floor.c'
probe $B 'pushd popd dirs' 'cd /; pushd /tmp > /dev/null; dirs; popd > /dev/null; pwd'
probe $B 'type -t' 'type -t cd; type -t /bin/ls; f() { :; }; type -t f; type -t if'
probe $B 'command -V' 'command -V cd'
probe $B 'declare -p' 'declare -i n=5; declare -p n'
probe $B 'declare -r' 'declare -r r=1; r=2; echo $?'
probe $B 'local -n nameref' 'f() { local -n ref=$1; ref=changed; }; v=orig; f v; echo $v'
probe $B 'indexed arrays' 'a=(x y z); a[5]=w; echo ${a[0]} ${a[@]} ${#a[@]} ${!a[@]}'
probe $B 'array append slice' 'a=(1 2); a+=(3 4); echo ${a[@]:1:2} "${a[*]}"'
probe $B 'assoc arrays' 'declare -A m; m[k]=v; m[j]=w; echo ${m[k]} ${#m[@]}'
probe $B 'array unset' 'a=(1 2 3); unset a[1]; echo ${a[@]} ${#a[@]}'
probe $B 'array in for' 'a=("x y" z); for e in "${a[@]}"; do echo "<$e>"; done'
probe $B 'regex captures' '[[ abc =~ ^(a)(b) ]]; echo ${BASH_REMATCH[0]}:${BASH_REMATCH[2]}'
probe $B 'extglob' 'shopt -s extglob; eval '\''case aab in +(a)b) echo yes;; esac'\'''
probe $B 'globstar' 'shopt -s globstar; cd "$root"/kit && echo **/*.py | wc -w'
probe $B 'process substitution' 'cat <(echo x) <(echo y)'
probe $B 'coproc' 'coproc C { read x; echo got $x; }; echo hi >&${C[1]}; read y <&${C[0]}; echo $y'
probe $B 'select' 'echo 2 | select x in a b; do echo $x; break; done'
probe $B 'time keyword' 'TIMEFORMAT=%R; { time :; } 2>&1 | wc -l'
probe $B 'mapfile' 'printf "a\nb\n" | { mapfile -t l; echo ${l[1]} ${#l[@]}; }'
probe $B 'noclobber status' 'set -C; echo a > /tmp/mw-nc.$$; echo b 2>/dev/null > /tmp/mw-nc.$$; echo $?; rm -f /tmp/mw-nc.$$'
probe $B 'case fallthrough' 'case a in a) echo one;& b) echo two;; esac; case a in a) echo x;;& a) echo y;; esac'
probe $B 'trap RETURN DEBUG ERR' 'trap "echo err" ERR; false; f() { :; }; trap "echo ret" RETURN; f'
probe $B 'shopt -o' 'shopt -so pipefail; shopt -o pipefail'
probe $B 'lastpipe' 'shopt -s lastpipe; echo 5 | read v; echo $v'
probe $B 'exec -a' 'exec -a name sh -c "echo \$0"'
probe $B 'echo -e -n -E' 'echo -e "a\tb"; echo -n x; echo -E "\n"'
probe $B 'test == and [[' '[ a == a ] && echo yes; [[ a == a ]] && echo yes'
probe $B 'arith exponent ternary bases' 'echo $((2**10)) $((1?2:3)) $((16#ff)) $((0x10)) $((010))'
probe $B 'arith assignment ops' 'x=1; ((x+=2, x<<=1)); echo $x $((x++)) $x'
probe $B 'negative array index' 'a=(1 2 3); echo ${a[-1]}'
probe $B 'wait -n' 'sleep 0.1 & sleep 0.2 & wait -n; echo $?'
probe $B 'jobs -p' 'sleep 0.2 & jobs -p | wc -l'
probe $B 'disown' 'sleep 0.1 & disown; echo ok'
probe $B 'ulimit -a' 'ulimit -a | wc -l'
probe $B 'getopts silent' 'getopts ":a:" o -a; echo $o $OPTARG'
probe $B 'read -t 0' 'read -t 0 v; echo $?'
probe $B 'printf %(fmt)T' 'printf "%(%Y)T\n" 0'
probe $B 'unset -f' 'f() { :; }; unset -f f; type f 2>&1 | wc -l'
probe $B 'builtin enable' 'builtin echo x; enable -n echo; enable echo; echo y'
probe $B 'source with args' 'echo "echo \$1" > /tmp/mw-src.$$; source /tmp/mw-src.$$ arg; rm -f /tmp/mw-src.$$'
probe $B 'here doc dash tabs' 'cat <<-XX
	a
	XX'
probe $B 'BASHPID' '[ "$BASHPID" = "$$" ] && echo same'
probe $B 'errexit in function' 'set -e; f() { false; echo no; }; f; echo no2'
probe $B 'inherit_errexit' 'shopt -s inherit_errexit; set -e; v=$(false; echo x); echo $v'
probe $B '$_ last argument' 'echo a b; echo $_'
probe $B 'string compare < in [[' '[[ a < b ]] && echo yes'
probe $B 'declare -l -u' 'declare -l lo=ABC; declare -u up=abc; echo $lo $up'
probe $B 'typeset -i arithmetic' 'typeset -i n; n=2+3; echo $n'
probe $B 'OPTERR' 'OPTERR=0; getopts a o -z; echo $?'
probe $B 'compgen' 'compgen -A function | wc -l'
probe $B 'complete' 'complete -F f g 2>&1; echo $?'
probe $B 'history' 'history 2>&1 | wc -l'
probe $B 'fc' 'fc -l 2>&1 | wc -l'
echo "== POSIX features (against dash)"
probe $D 'special params' 'set -- a b; echo $# "$*" "$@" $? $-; [ "$$" -gt 1 ]'
probe $D 'parameter forms' 'unset u; v=x; echo ${u-d} ${u:-d} ${v+s} ${v:+s} ${#v} ${u=e} $u'
probe $D 'parameter error' '(unset u; echo ${u?msg}) 2>/dev/null'
probe $D 'prefix suffix' 'v=a.b.c; echo ${v#*.} ${v##*.} ${v%.*} ${v%%.*}'
probe $D 'arith forms' 'echo $((1+2*3)) $((7%3)) $(( (1<<3)|1 )) $((3>2)) $((!0)) $((~0))'
probe $D 'arith assign' 'x=1; echo $((x+=1)) $x $((y=5)) $y'
probe $D 'command subst backtick' 'echo `echo x` "`echo y`"'
probe $D 'field splitting IFS' 'IFS=:; v=a:b::c; set -- $v; echo $#; unset IFS'
probe $D 'pathname expansion' 'cd "$root"/kit && echo bench_a*.c | wc -w'
probe $D 'here doc quoted' 'x=1; cat <<"XX"
$x
XX'
probe $D 'redirect dup close' 'exec 3>&1; echo x >&3; exec 3>&-; echo y'
probe $D 'and or lists' 'true && echo a || echo b; false || echo c && echo d'
probe $D 'subshell group' '(x=1; echo $x); { x=2; }; echo $x'
probe $D 'case patterns' 'case abc in a*) echo one;; esac; case x in [!y]) echo two;; esac'
probe $D 'loops' 'for i in 1 2; do echo $i; done; i=0; while [ $i -lt 2 ]; do i=$((i+1)); done; until [ $i -ge 4 ]; do i=$((i+1)); done; echo $i'
probe $D 'break continue levels' 'for i in 1 2; do for j in 1 2; do [ $j = 2 ] && continue 2; echo $i$j; done; done'
probe $D 'functions and return' 'f() { return 3; }; f; echo $?; g() { echo ${1:-none}; }; g; g a'
probe $D 'readonly export' 'readonly r=1; (r=2) 2>/dev/null; echo $?; export e=1; sh -c "echo \$e"'
probe $D 'set options' 'set -u; echo ${zz-unset}; set +u; set -f; echo *; set +f; set -a; v=1; sh -c "echo \$v"'
probe $D 'trap and exit' 'trap "echo bye" EXIT; trap "echo usr" USR1; kill -USR1 $$; exit 3'
probe $D 'getopts' 'set -- -a -b val c; while getopts ab: o; do echo $o $OPTARG; done; shift $((OPTIND-1)); echo $1'
probe $D 'read raw and split' 'printf "a b\\\\c\n" | { read x y; echo "$x|$y"; }'
probe $D 'wait status' 'sh -c "exit 7" & wait $!; echo $?'
probe $D 'exec builtin' 'exec echo done'
probe $D 'eval' 'v="a b"; eval "set -- $v"; echo $#'
probe $D 'cd dash and dots' 'cd /tmp; cd /; cd - > /dev/null; pwd; cd ..; pwd'
probe $D 'umask symbolic' 'umask 022; umask -S'
probe $D 'test operators' 'test -n a -a -z ""; echo $?; [ 1 -lt 2 ] && [ a != b ] && echo ok; [ ! -e /nope ] && echo ok2'
probe $D 'command builtin' 'command -v echo; command -V cd | head -c 2; echo'
probe $D 'alias in script' 'alias ll="echo LL"; eval ll'
probe $D 'shift' 'set -- a b c; shift 2; echo $# $1'
probe $D 'IFS newline splitting' 'IFS="
"; v="a b
c"; set -- $v; echo $#'
probe $D 'tilde' 'HOME=/hh; echo ~ ~/x; echo ~nosuchuser'
probe $D 'unset function' 'f() { echo f; }; unset -f f; f 2>/dev/null || echo gone'
probe $D 'exit in subshell' '(exit 5); echo $?'
probe $D 'printf conversions' 'printf "%d %5.2f %s %c %x %o %e|\n" 3 2.5 str A 255 8 1.5'
probe $D 'kill -l' 'kill -l | wc -w'
probe $D 'times' 'times | wc -l'
probe $D 'hash' 'hash -r; echo $?'
probe $D 'ulimit -n' 'ulimit -n | grep -c "^[0-9]"'
probe $D 'set -- dash args' 'set -- -x -y; echo $1 $2'
probe $D 'heredoc in subst' 'v=$(cat <<XX
x
XX
); echo $v'
probe $D 'octal escape printf' 'printf "\\101\\n"'
