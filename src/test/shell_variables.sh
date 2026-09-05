#!/bin/sh
#
#       Dynamic declaration attributes and Bash-mode ulimit compatibility.
#
#       Usage: sh src/test/shell_variables.sh [shell]
#
# The subject is reached through both `bash` and `sh` links: basename is part
# of the contract being tested. Bash cases compare with the pinned host Bash;
# the final policy cases compare the ordinary identity with dash.

set -e

subject=${1:-/tmp/mwsh}
bash_reference=${BASH_REFERENCE:-/bin/bash}
dash_reference=${DASH_REFERENCE:-/bin/dash}

[ -x "$subject" ] || { echo "no shell at $subject" >&2; exit 1; }
[ -x "$bash_reference" ] || { echo "no Bash at $bash_reference" >&2; exit 1; }
[ -x "$dash_reference" ] || { echo "no dash at $dash_reference" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

case $subject in
/*) target=$subject ;;
*) target=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename -- "$subject") ;;
esac

ln -s "$target" "$work/bash"
ln -s "$target" "$work/sh"

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/tally.sh"

shown()
{
        head -c 80 "$1" | tr '\n' '|'
}

compare()
{
        mode=$1
        name=$2
        shift 2

        printf '%s\n' "$*" > "$work/case.sh"

        if [ "$mode" = bash ]; then
                reference=$bash_reference
                ours=$work/bash
        else
                reference=$dash_reference
                ours=$work/sh
        fi

        if timeout 5 "$reference" "$work/case.sh" > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi

        if timeout 5 "$ours" "$work/case.sh" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
                [ "$want_status" = "$got_status" ]; then
                won
        else
                lost "$name" \
                        "want $(shown "$work/want")[$want_status] got $(shown "$work/got")[$got_status]"
        fi
}

section variables
group readonly

compare bash 'declare -r scoped value' \
        'x=global; f() { declare -r x=local; declare -p x; }; f; x=after; printf "%s\n" "$x"'
compare bash 'local -r scoped value' \
        'x=global; f() { local -r x=local; declare -p x; }; f; x=after; printf "%s\n" "$x"'
compare bash 'nested readonly visibility' \
        'x=global; inner() { printf "<%s>\n" "$x"; }; outer() { local -r x=outer; inner; declare -p x; }; outer; x=after; printf "<%s>\n" "$x"'
compare bash 'nested attribute unwind' \
        'x=global; outer() { local x=mutable; inner; printf "<%s>\n" "$x"; }; inner() { local -r x=inner; declare -p x; }; outer; printf "<%s>\n" "$x"'
compare bash 'readonly marks a local' \
        'x=global; f() { local x=local; readonly x; declare -p x; }; f; x=after; printf "<%s>\n" "$x"'
compare bash 'unset declaration unwinds' \
        'f() { declare -r absent; declare -p absent; }; f; declare -p absent 2>/dev/null; printf "%s\n" "$?"'
compare bash 'readonly assignment status' \
        'x=outer; (f() { local x=inner; readonly x; x=bad; echo no; }; f) 2>/dev/null; printf "%s:%s\n" "$?" "$x"'
compare bash 'readonly cannot be hidden' \
        'readonly x=global; f() { local x=local; printf "%s:<%s>\n" "$?" "$x"; }; f 2>/dev/null'
compare bash 'nested declare readonly' \
        'x=global; outer() { local x=outer; inner; x=changed; printf "<%s>\n" "$x"; }; inner() { declare -r x=inner; declare -p x; }; outer; printf "<%s>\n" "$x"'
compare bash 'readonly read status' \
        'r=old; f() { local -r r=keep; printf new | read r; printf "%s:%s\n" "$?" "$r"; }; f'
compare bash 'local unset restores outer value' \
        'x=outer; f() { local x=inner; unset x; printf "[%s]" "${x-gone}"; }; f; printf "|%s|\n" "$x"'

section identity
group basename

compare bash 'Bash identity variables' \
        'printf "%s|%s\n" "$BASH_VERSION" "${BASH_VERSINFO[0]}"'
compare dash 'sh omits Bash identity' \
        'unset BASH_VERSION BASH_VERSINFO 2>/dev/null; printf "<%s>|<%s>\n" "$BASH_VERSION" "${BASH_VERSINFO:-}"'
compare bash 'Bash nounset status' \
        'set -u; printf "%s\n" "$missing"; printf after'
compare dash 'dash nounset status' \
        'set -u; printf "%s\n" "$missing"; printf after'
compare bash 'POSIXLY_CORRECT tracks posix mode' \
        'set +o posix; POSIXLY_CORRECT=1; set -o | grep -q "^posix.*on$"; printf "on:%s\n" "$?"; unset POSIXLY_CORRECT; set -o | grep -q "^posix.*off$"; printf "off:%s\n" "$?"'
compare bash 'POSIXLY_CORRECT nameref tracks posix mode' \
        'set +o posix; declare -n n=POSIXLY_CORRECT; n=1; set -o | grep -q "^posix.*on$"; printf "on:%s\n" "$?"; unset n; set -o | grep -q "^posix.*off$"; printf "off:%s\n" "$?"'
compare bash 'default RHS sees preceding substitution status' \
        'false; a=$(true) b=$? c=$(false) d=$?; printf "%s:%s:%s\n" "$b" "$d" "$?"'
compare bash 'POSIX RHS preserves pre-command status' \
        'set -o posix; false; a=$(true) b=$? c=$(false) d=$?; printf "%s:%s:%s\n" "$b" "$d" "$?"'
compare bash 'brace expansion enabled' \
        'printf "<%s>\n" pre{a,b}post'
compare bash 'brace expansion disabled' \
        'set +B; printf "<%s>\n" pre{a,b}post'

section ulimit
group bash

compare bash 'complete soft listing' 'ulimit -a'
compare bash 'complete hard listing' 'ulimit -Ha'
compare bash 'resource query scales' \
        'for option in c d e f i l m n p q r s t u v x R; do ulimit -$option; done'
compare bash 'file and core scaling' \
        '(ulimit -c 7; grep "Max core file size" /proc/self/limits); (ulimit -f 9; grep "Max file size" /proc/self/limits)'
compare bash 'soft and hard keywords' \
        '(ulimit -S -n hard; ulimit -S -n); (ulimit -H -n soft; ulimit -H -n)'
compare bash 'error statuses' \
        'ulimit -b >/dev/null 2>&1; printf "%s|" "$?"; ulimit -n nope >/dev/null 2>&1; printf "%s|" "$?"; ulimit -p 7 >/dev/null 2>&1; printf "%s\n" "$?"'

group dash
compare dash 'default listing unchanged' 'ulimit -a'
compare dash 'default scales unchanged' 'ulimit -c; ulimit -f; ulimit -p; ulimit -n'

section pipestatus
group lazy

compare bash 'initial vector' \
        'printf "<%s>:%s\n" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
compare bash 'simple status materializes' \
        'false; printf "<%s>:%s\n" "${PIPESTATUS[*]}" "${#PIPESTATUS[@]}"'
compare bash 'pipeline vector stays whole' \
        'true | false | true; printf "%s:<%s>:<%s>:<%s>\n" "${#PIPESTATUS[@]}" "${PIPESTATUS[0]}" "${PIPESTATUS[1]}" "${PIPESTATUS[2]}"'
compare bash 'copy sees prior pipeline' \
        'false | true; copy=("${PIPESTATUS[@]}"); printf "<%s>|<%s>\n" "${copy[*]}" "${PIPESTATUS[*]}"'
compare bash 'user assignment superseded' \
        'PIPESTATUS=(7 8); printf "%s:<%s>\n" "${#PIPESTATUS[@]}" "${PIPESTATUS[*]}"'
compare bash 'scalar copy then reset' \
        'false | true; copy=${PIPESTATUS[*]}; printf "<%s>|<%s>\n" "$copy" "${PIPESTATUS[*]}"'
compare bash 'eval observes pending' \
        'false; eval '\''printf "<%s>\n" "${PIPESTATUS[*]}"'\'''
compare bash 'unset recreates on read' \
        'unset PIPESTATUS; printf "%s:<%s>\n" "${#PIPESTATUS[@]}" "${PIPESTATUS[*]}"'
compare bash 'empty readonly stays empty' \
        'readonly PIPESTATUS; false; declare -p PIPESTATUS'
compare bash 'valued readonly publishes' \
        'false; readonly PIPESTATUS; declare -p PIPESTATUS'
compare bash 'declare sees absent pending' \
        ':; declare -p PIPESTATUS'
compare bash 'prefix sees absent pending' \
        ':; printf "<%s>\n" ${!PIPESTATUS*}'
compare bash 'set includes absent pending' \
        ':; set | grep -q "^PIPESTATUS="; printf "%s\n" "$?"'
compare bash 'selected PIPESTATUS subscript' \
        'PIPESTATUS[4]=9; declare -p PIPESTATUS'
compare bash 'selected subscript tracks simple status' \
        'PIPESTATUS[4]=9; false; declare -p PIPESTATUS'
compare bash 'selected subscript receives pipeline head' \
        'PIPESTATUS[4]=9; false | true; declare -p PIPESTATUS'
compare bash 'selected pipeline subscript collision' \
        'PIPESTATUS[1]=9; false | true; declare -p PIPESTATUS'
compare bash 'existing vector resets indexed write' \
        'false | true; PIPESTATUS[4]=9; declare -p PIPESTATUS'
compare bash 'selected readonly PIPESTATUS refreshes' \
        'PIPESTATUS[4]=9; readonly PIPESTATUS; false | true; declare -p PIPESTATUS'

group serialization

compare bash 'set serializes sparse indexed arrays' \
        'a=([2]="x y" [4]=""); set | grep "^a="'
compare bash 'set serializes associative arrays' \
        'declare -A m=([x]="a b"); set | grep "^m="'
compare bash 'set serializes assigned empty arrays' \
        'a=(); set | grep "^a="'
compare bash 'set omits unassigned declared arrays' \
        'declare -a a; set | grep "^a="'

group nameref-array

compare bash 'nameref indexed element views' \
        'a=([2]=x); declare -n n=a; printf "<%s>:%s:<%s>\n" "${n[2]}" "${#n[@]}" "${!n[@]}"'
compare bash 'nameref indexed element writes' \
        'a=([2]=x); declare -n n=a; n[4]=y; declare -p a n'
compare bash 'nameref associative element access' \
        'declare -A m=([x]=a); declare -n n=m; n[y]=b; printf "<%s>|<%s>:%s\n" "${n[x]}" "${n[y]}" "${#n[@]}"'
compare bash 'nameref element unset' \
        'a=([2]=x [4]=y); declare -n n=a; unset '\''n[2]'\''; declare -p a n'
compare bash 'nameref target unset' \
        'a=([2]=x); declare -n n=a; unset n; declare -p n; declare -p a'
compare bash 'unset n keeps nameref target' \
        'a=([2]=x); declare -n n=a; unset -n n; declare -p a; declare -p n'
compare bash 'nameref compound replacement' \
        'a=([2]=x); declare -n n=a; n=(y z); declare -p a n'
compare bash 'nameref compound append' \
        'a=([2]=x); declare -n n=a; n+=(y z); declare -p a n'
compare bash 'readonly nameref writes target' \
        'a=old; declare -rn n=a; n=new; printf "%s:%s:%s\n" "$?" "$a" "$n"'
compare bash 'readonly scalar target status' \
        'readonly a=old; declare -n n=a; n=new; printf after'
compare bash 'readonly array target status' \
        'declare -ar a=([0]=old); declare -n n=a; n[2]=new; printf after'
compare bash 'readonly target unset status' \
        'declare -ar a=([2]=x); declare -n n=a; unset '\''n[2]'\''; printf "status:%s\n" "$?"'
compare bash 'chained array nameref' \
        'a=([2]=x); declare -n m=a; declare -n n=m; n[4]=y; printf "<%s>:%s\n" "${n[4]}" "${#n[@]}"'

group nameref-element

compare bash 'element-bound nameref read' \
        'a=([2]=x); n="a[2]"; declare -n n; printf "<%s>\n" "$n"'
compare bash 'element-bound nameref write' \
        'a=([2]=x); n="a[2]"; declare -n n; n=y; declare -p a n'
compare bash 'element-bound nameref append' \
        'a=([2]=x); n="a[2]"; declare -n n; n+=y; declare -p a n'
compare bash 'element-bound nameref unset' \
        'a=([2]=x [4]=z); n="a[2]"; declare -n n; unset n; declare -p a n'
compare bash 'element-bound readonly unset' \
        'declare -ar a=([2]=x); n="a[2]"; declare -n n; unset n; printf "%s:<%s>\n" "$?" "$n"; declare -p a n'
compare bash 'element-bound readonly write status' \
        'declare -ar a=([2]=x); n="a[2]"; declare -n n; n=y; printf after'
compare bash 'element-bound dynamic index' \
        'a=([2]=x [3]=y); i=2; n="a[i]"; declare -n n; printf "<%s>" "$n"; i=3; printf ":<%s>\n" "$n"'
compare bash 'indexed assignment evaluates subscript once' \
        'j=0; i="j++"; a[i]=x; printf "%s:<%s>:<%s>\n" "$j" "${a[0]-}" "${a[1]-}"'
compare bash 'element nameref assignment evaluates subscript once' \
        'j=0; i="j++"; declare -n n="a[i]"; n=x; printf "%s:<%s>:<%s>\n" "$j" "${a[0]-}" "${a[1]-}"'
compare bash 'element-bound associative write' \
        'declare -A m=([x]=old); n="m[x]"; declare -n n; n=new; declare -p m n'
compare bash 'element-bound rejects another index' \
        'a=([2]=x); n="a[2]"; declare -n n; n[4]=y; printf after'
compare bash 'element-bound rejects compound value' \
        'a=([2]=x); n="a[2]"; declare -n n; n=(y z); printf after'
compare bash 'repeated nameref declaration rebinds' \
        'a=x; b=y; declare -n n=a; declare -n n=b; declare -p n; printf "%s:%s\n" "$a" "$b"'
compare bash 'repeated local nameref rebinds' \
        'f() { local a=x b=y; local -n n=a; local -n n=b; declare -p n; printf "%s:%s\n" "$a" "$b"; }; f'
compare bash 'circular nameref element status' \
        'declare -n a=b; declare -n b=a; a[2]=x; printf "status:%s\n" "$?"'
compare bash 'self-referential element assignment status' \
        'n="n[2]"; declare -n n; n=x; printf after'
compare bash 'nameref target length differs' \
        'longname=([0]=zero [2]=x); declare -n n=longname; printf "<%s>:%s\n" "${n[*]}" "${#n[@]}"'

group nameref-attributes

compare bash 'export follows nameref target' \
        'a=old; declare -n n=a; export n; declare -p a n'
compare bash 'readonly follows nameref target' \
        'a=old; declare -n n=a; readonly n; declare -p a n'
compare bash 'declare export follows nameref target' \
        'a=old; declare -n n=a; declare -x n; declare -p a n'
compare bash 'declare readonly follows nameref target' \
        'a=old; declare -n n=a; declare -r n; declare -p a n'
compare bash 'combined nameref export marks reference' \
        'a=old; declare -n n=a; declare -xn n; declare -p a n'
compare bash 'combined nameref readonly marks reference' \
        'a=old; declare -n n=a; declare -rn n; declare -p a n'
compare bash 'temporary export follows nameref target' \
        'a=old; declare -n n=a; n=new /usr/bin/printenv a; declare -p a n'
compare bash 'temporary element nameref exports assignment name' \
        'a[x]=old; declare -n n="a[x]"; n=new /usr/bin/printenv n; declare -p a n'
compare bash 'nested element nameref export unwinds' \
        'a[x]=old; declare -n n="a[x]"; f() { /usr/bin/printenv n; n=two /usr/bin/printenv n; /usr/bin/printenv n; }; n=one f; printf "state:%s:%s\n" "${a[x]}" "$n"'
compare bash 'explicit reference export does not promote target' \
        'a=old; declare -n n=a; n=new export n; declare -p a n'
compare bash 'explicit target export promotes target' \
        'a=old; declare -n n=a; n=new export a; declare -p a n'
compare bash 'nested readonly restores prefix attributes' \
        'X=old; f() { readonly X; }; X=new f; declare -p X; X=x; printf "status:%s\n" "$?"'
compare bash 'eval readonly restores prefix attributes' \
        'X=old; X=new eval "readonly X"; declare -p X; X=x; printf "status:%s\n" "$?"'
compare bash 'scoped nameref rebind unwinds through prefix' \
        'a=old; b=bee; declare -n n=a; f() { declare -n n=b; }; n=new f; declare -p a b n'

section ""
total=$((pass + fail))
echo
printf '  %s of %s\n' "$pass" "$total"

[ "$fail" = 0 ]
