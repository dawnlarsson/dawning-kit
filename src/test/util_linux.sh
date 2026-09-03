#!/bin/sh
#
#       util-linux 2.42.2 denominator and kernel-policy utilities.
#
#       The 130-name denominator comes from the signed upstream release at
#       https://kernel.org/pub/linux/utils/util-linux/v2.42/ and is the sorted
#       set of installed executable targets reported by a default Meson setup.
#       Artifact SHA-256:
#       03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a
#       It is kept here rather than discovered from the host so a package split
#       or PATH change cannot silently move the target.
#
set -u

subject=${1:-/tmp/mwsh}
subject=$(CDPATH= cd -- "$(dirname -- "$subject")" && pwd)/$(basename "$subject")
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/moonwater-util-linux.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

. "$root/src/test/tally.sh"

mkdir "$work/bin"
for name in setsid setpgid ionice fadvise taskset renice prlimit chrt \
        uclampset flock unshare nsenter setarch setpriv waitpid choom exch \
        getino; do
        ln -s "$subject" "$work/bin/$name"
done

shown() { head -c 80 "$1" | tr '\n' '|'; }

compare()
{
        name=$1
        utility=$2
        script=$3
        shift 3
        reference=$(command -v "$utility" || true)

        if [ -z "$reference" ]; then
                lost "$name" "system util-linux $utility is required"
                return
        fi

        if TOOL=$reference sh -c "$script" "$@" > "$work/want" 2>/dev/null; then
                want_status=0
        else
                want_status=$?
        fi
        if TOOL="$work/bin/$utility" sh -c "$script" "$@" > "$work/got" 2>/dev/null; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
           [ "$want_status" = "$got_status" ]; then
                won
        else
                lost "$name" \
                     "want $(shown "$work/want")[$want_status], got $(shown "$work/got")[$got_status]"
        fi
}

compare_signal()
{
        name=$1
        utility=$2
        shift 2
        reference=$(command -v "$utility" || true)

        want=$(python3 -c 'import subprocess,sys; print(subprocess.run(sys.argv[1:]).returncode)' \
                "$reference" "$@")
        got=$(python3 -c 'import subprocess,sys; print(subprocess.run(sys.argv[1:]).returncode)' \
                "$work/bin/$utility" "$@")
        if [ "$want" = "$got" ]; then
                won
        else
                lost "$name" "want signal return $want, got $got"
        fi
}

subject()
{
        name=$1
        utility=$2
        script=$3
        shift 3
        if TOOL="$work/bin/$utility" sh -c "$script" "$@" \
                > "$work/got" 2>/dev/null; then
                won
        else
                lost "$name" "subject failed: $(shown "$work/got")"
        fi
}

section util-linux

group reference
for utility in setsid setpgid ionice fadvise taskset renice prlimit chrt \
        uclampset flock unshare nsenter setarch setpriv waitpid choom exch \
        getino; do
        version=$($utility --version 2>/dev/null | head -1 || true)
        case $version in
        *'util-linux 2.42.2'*) won ;;
        *) lost "$utility" "need util-linux 2.42.2 reference, got [$version]" ;;
        esac
done

group setsid
compare 'requires command' setsid '"$TOOL"'
compare 'passes exit status' setsid '"$TOOL" /bin/sh -c "exit 7"'
compare 'new session identity' setsid \
        '"$TOOL" /bin/sh -c '\''sid=$(ps -o sid= -p $$ | tr -d " "); [ "$sid" = "$$" ]; echo $?'\'''
compare 'forced fork and wait' setsid '"$TOOL" -f -w /bin/sh -c "exit 7"'
compare 'signaled child is signal number' setsid \
        '"$TOOL" -f -w /bin/sh -c '\''kill -TERM $$'\'''
compare 'long fork and wait' setsid \
        '"$TOOL" --fork --wait /bin/sh -c "printf child; exit 3"'
compare 'missing executable' setsid '"$TOOL" /no/such/util-linux-command'
compare 'option boundary' setsid '"$TOOL" -- /bin/sh -c "echo boundary"'

group setpgid
compare 'requires command' setpgid '"$TOOL"'
compare 'passes exit status' setpgid '"$TOOL" /bin/sh -c "exit 7"'
compare 'new process group' setpgid \
        '"$TOOL" /bin/sh -c '\''pg=$(ps -o pgid= -p $$ | tr -d " "); [ "$pg" = "$$" ]; echo $?'\'''
compare 'foreground without tty' setpgid \
        '"$TOOL" -f /bin/sh -c "printf foreground"'
compare 'missing executable' setpgid '"$TOOL" /no/such/util-linux-command'
compare 'option boundary' setpgid '"$TOOL" -- /bin/sh -c "echo boundary"'

group ionice
compare 'query self' ionice '"$TOOL"'
compare 'default command policy' ionice \
        '"$TOOL" /bin/sh -c '\''ionice -p $$'\'''
compare 'query pid' ionice '"$TOOL" -p $$'
compare 'leading plus pid' ionice '"$TOOL" -p +$$'
compare 'leading blank pid' ionice '"$TOOL" -p " $$"'
compare 'query repeated pid' ionice '"$TOOL" -p $$ $$'
compare 'reject repeated identity option' ionice '"$TOOL" -p $$ -p $$'
compare 'reject mixed identity options' ionice '"$TOOL" -p $$ -P $$'
compare 'idle command' ionice \
        '"$TOOL" -c idle /bin/sh -c '\''ionice -p $$'\'''
compare 'best effort data' ionice \
        '"$TOOL" -c best-effort -n 6 /bin/sh -c '\''ionice -p $$'\'''
compare 'numeric class' ionice \
        '"$TOOL" -c 3 /bin/sh -c '\''ionice -p $$'\'''
compare 'case insensitive class' ionice \
        '"$TOOL" -c IDLE /bin/sh -c '\''ionice -p $$'\'''
compare 'invalid class' ionice '"$TOOL" -c impossible /bin/true'
compare 'tolerant missing pid' ionice '"$TOOL" -t -c idle -p 2147483647'
compare 'reject wrapped pid' ionice \
        '"$TOOL" -t -c idle -p 4294967296'
compare 'reject overlong pid' ionice \
        '"$TOOL" -t -c idle -p 999999999999999999999999999999'
compare 'reject wrapped class' ionice \
        '"$TOOL" -t -c 4294967298 -p $$'
compare 'reject wrapped class data' ionice \
        '"$TOOL" -t -c best-effort -n 4294967298 -p $$'
compare 'missing executable' ionice \
        '"$TOOL" -c idle /no/such/util-linux-command'

group taskset
compare 'query pid mask' taskset \
        '"$TOOL" -p $$ | sed "s/pid [0-9]*/pid PID/"'
compare 'query pid list' taskset \
        '"$TOOL" -pc $$ | sed "s/pid [0-9]*/pid PID/"'
compare 'command current list' taskset \
        'list=$("$TOOL" -pc $$ | sed "s/.*: //"); "$TOOL" -c "$list" /bin/sh -c '\''"$TOOL" -pc $$ | sed "s/pid [0-9]*/pid PID/"'\'''
compare 'set pid current list' taskset \
        'list=$("$TOOL" -pc $$ | sed "s/.*: //"); "$TOOL" -pc "$list" $$ | sed "s/pid [0-9]*/pid PID/g"'
compare 'interleaved stride list' taskset \
        '"$TOOL" -c 0,2,4-6 /bin/sh -c '\''"$TOOL" -pc $$ | sed "s/pid [0-9]*/pid PID/"'\'''
compare 'invalid CPU list' taskset '"$TOOL" -c impossible /bin/true'
compare 'missing command' taskset '"$TOOL" -c 0'

group chrt
compare 'query pid' chrt \
        '"$TOOL" -p $$ | sed "s/pid [0-9]*/pid PID/g"'
compare 'set pid other policy' chrt \
        '"$TOOL" -v -o -p 0 $$ | sed "s/pid [0-9]*/pid PID/g"'
compare 'reset alone queries pid' chrt \
        '"$TOOL" -R -p $$ | sed "s/pid [0-9]*/pid PID/g"'
compare 'other command' chrt \
        '"$TOOL" -o /bin/sh -c '\''"$TOOL" -p $$ | sed "s/pid [0-9]*/pid PID/g"'\'''
compare 'priority ranges' chrt '"$TOOL" --max'
compare 'missing real-time priority' chrt '"$TOOL" -f /bin/true'
compare 'invalid pid' chrt '"$TOOL" -p impossible'

group renice
compare 'process priority' renice \
        '"$TOOL" 0 -p $$ | sed "s/^[0-9]*/PID/"'
compare 'explicit priority option' renice \
        '"$TOOL" -n 0 -p $$ | sed "s/^[0-9]*/PID/"'
compare 'relative zero' renice \
        '"$TOOL" --relative 0 -p $$ | sed "s/^[0-9]*/PID/"'
compare 'relative overflow clamps' renice \
        'nice -n 1 sleep 1 & pid=$!; sleep .02; out=$("$TOOL" --relative 9223372036854775807 -p "$pid"); status=$?; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; printf "%s\n" "$out" | sed "s/^[0-9]*/PID/"; exit "$status"'
compare 'priority option after pid' renice \
        'out=$("$TOOL" -p $$ -n 0); status=$?; printf "%s" "$out"; exit "$status"'
compare 'invalid priority' renice '"$TOOL" impossible -p $$'
#       A digit string that wraps a 64-bit word is not a number, as strtol
#       says; the shell's own wrapping scanner once let it through as zero.
compare 'wrapped priority' renice '"$TOOL" 18446744073709551616 -p $$'
compare 'wrapped relative' renice '"$TOOL" --relative 18446744073709551615 -p $$'
compare 'missing identity' renice '"$TOOL" 0 -p'

group prlimit
compare 'query all resources' prlimit '"$TOOL" -p $$'
compare 'query nofile' prlimit '"$TOOL" -p $$ --nofile'
compare 'selected columns' prlimit \
        '"$TOOL" -p $$ --nofile --output RESOURCE,SOFT,HARD'
compare 'raw without headings' prlimit \
        '"$TOOL" -p $$ --nofile --raw --noheadings --output RESOURCE,DESCRIPTION,SOFT'
compare 'command limit pair' prlimit \
        '"$TOOL" --nofile=100:200 /bin/sh -c "ulimit -Sn; ulimit -Hn"'
compare 'negative one is unlimited' prlimit \
        '"$TOOL" --core=-1 /bin/sh -c "ulimit -Hc"'
compare 'setting only is silent' prlimit \
        '"$TOOL" --nofile=100:200'
compare 'reject empty limit' prlimit '"$TOOL" --nofile= /bin/true'
compare 'reject empty pair' prlimit '"$TOOL" --nofile=: /bin/true'
compare 'invalid limit' prlimit '"$TOOL" --nofile=bad /bin/true'

group uclampset
compare 'query pid' uclampset \
        '"$TOOL" -p $$ | sed "s/.* util_clamp:/util_clamp:/"'
compare 'command clamps' uclampset \
        '"$TOOL" -m 0 -M 1024 /bin/sh -c '\''"$TOOL" -p $$ | sed "s/.* util_clamp:/util_clamp:/"'\'''
compare 'set pid clamps' uclampset \
        '"$TOOL" -v -m 0 -M 1024 -p $$ | sed "s/.* util_clamp:/util_clamp:/"'
compare 'reset alone queries pid' uclampset \
        '"$TOOL" -R -p $$ | sed "s/.* util_clamp:/util_clamp:/"'
compare 'command needs a clamp' uclampset \
        '"$TOOL" /bin/sh -c "exit 7" >/dev/null'
compare 'verbose command clamps' uclampset \
        '"$TOOL" -v -m 0 -M 1024 /bin/true | sed "s/.* util_clamp:/util_clamp:/"'
compare 'invalid clamp' uclampset '"$TOOL" -m 1025 /bin/true'

printf 'content\n' > "$work/data"
group fadvise
compare 'default advice' fadvise '"$TOOL" "$0"' "$work/data"
for advice in normal sequential random noreuse willneeded dontneed; do
        compare "advice $advice" fadvise \
                '"$TOOL" -a "$1" "$0"' "$work/data" "$advice"
done
compare 'offset and length' fadvise \
        '"$TOOL" -o 1K -l 2KiB "$0"' "$work/data"
for size in 1KB 1kiB 1kib 1p 0x10 1.5K 1.9K 0.5MB 0.5MiB; do
        compare "range grammar $size" fadvise \
                '"$TOOL" -o "$1" "$0"' "$work/data" "$size"
done
for size in 1Ki 1KIB 1B 1Q -1; do
        compare "invalid range $size" fadvise \
                '"$TOOL" -o "$1" "$0"' "$work/data" "$size"
done
compare 'inherited descriptor' fadvise \
        'exec 9<"$0"; "$TOOL" --fd 9' "$work/data"
compare 'plus inherited descriptor' fadvise \
        'exec 9<"$0"; "$TOOL" --fd +9' "$work/data"
compare 'blank inherited descriptor' fadvise \
        'exec 9<"$0"; "$TOOL" --fd " 9"' "$work/data"
compare 'reject wrapped descriptor' fadvise \
        'exec 3<"$0"; "$TOOL" --fd 4294967299' "$work/data"
compare 'fd and file conflict' fadvise \
        'exec 9<"$0"; "$TOOL" -d 9 "$0"' "$work/data"
compare 'invalid advice' fadvise '"$TOOL" -a impossible "$0"' "$work/data"
compare 'missing file' fadvise '"$TOOL" /no/such/fadvise-file'
compare 'too many files' fadvise '"$TOOL" "$0" "$0"' "$work/data"

lock=$work/lock
ro_lock=$work/read-only-lock
: > "$ro_lock"
chmod 444 "$ro_lock"
group flock
compare 'file command status' flock \
        '"$TOOL" "$0" /bin/sh -c "printf locked; exit 7"' "$lock"
compare 'command string' flock \
        '"$TOOL" "$0" -c "printf command"' "$lock"
compare 'unlock still executes command' flock \
        '"$TOOL" -u "$0" /bin/sh -c "printf unlocked; exit 7"' "$lock"
compare 'read-only classic lock' flock \
        '"$TOOL" "$0" /bin/true' "$ro_lock"
compare 'fraction-only timeout' flock \
        '"$TOOL" -w .01 "$0" /bin/true' "$lock"
compare 'scientific timeout' flock \
        '"$TOOL" -w 1e-3 "$0" /bin/true' "$lock"
compare 'plus timeout' flock '"$TOOL" -w +0.01 "$0" /bin/true' "$lock"
compare 'blank timeout' flock '"$TOOL" -w " 0.01" "$0" /bin/true' "$lock"
compare 'verbose acquisition and execution' flock \
        '"$TOOL" --verbose "$0" /bin/true | sed "s/took [0-9.]* seconds/took TIME seconds/"' "$lock"
compare 'missing executable is unavailable' flock \
        '"$TOOL" "$0" /no/such/util-linux-command' "$lock"
compare 'command string rejects extras' flock \
        '"$TOOL" "$0" -c "printf wrong" extra' "$lock"
compare 'sole non-descriptor has no side effect' flock \
        'cd "$0"; "$TOOL" abc >/dev/null 2>&1; status=$?; [ ! -e abc ]; clean=$?; printf "%s:%s" "$status" "$clean"' "$work"
compare 'negative descriptor' flock '"$TOOL" -- -1'
compare 'overflow descriptor' flock '"$TOOL" 4294967296'
compare 'closed descriptor' flock '"$TOOL" 9'
compare 'nonblocking conflict code' flock \
        '"$TOOL" -n "$0" /bin/sh -c '\''"$TOOL" -n -E 42 "$1" /bin/true'\'' sh "$0"' "$lock"
compare 'timed conflict code' flock \
        '"$TOOL" -n "$0" /bin/sh -c '\''"$TOOL" -w 0.01 -E 42 "$1" /bin/true'\'' sh "$0"' "$lock"
compare 'close keeps parent lock' flock \
        '"$TOOL" --close "$0" /bin/sh -c '\''"$TOOL" -n -E 42 "$1" /bin/true'\'' sh "$0"' "$lock"
compare 'inherited descriptor' flock \
        'exec 9>"$0"; "$TOOL" -n 9' "$lock"
compare 'fcntl byte range' flock \
        '"$TOOL" --fcntl --start 0 --length 1 "$0" /bin/true' "$lock"
compare 'incompatible no-fork close' flock \
        '"$TOOL" --no-fork --close "$0" /bin/true' "$lock"

group choom
compare 'show pid one' choom '"$TOOL" -p 1'
compare 'set command score' choom \
        '"$TOOL" -n 0 -- /bin/sh -c '\''cat /proc/self/oom_score_adj'\'''
compare 'adjust own process' choom \
        '"$TOOL" -p $$ -n 1 | sed -E "s/pid [0-9]+/pid PID/"'
compare 'pid excludes command' choom '"$TOOL" -p $$ /bin/true'
compare 'command requires adjust' choom '"$TOOL" /bin/true'
compare 'invalid adjustment' choom '"$TOOL" -n impossible /bin/true'

group exch
compare 'exchange paths' exch \
        'printf A > "$0/a"; printf B > "$0/b"; "$TOOL" "$0/a" "$0/b"; cat "$0/a" "$0/b"' \
        "$work"
compare 'too few paths' exch '"$TOOL" only-one'
compare 'too many paths' exch '"$TOOL" one two three'
compare 'missing path' exch '"$TOOL" /no/such/one /no/such/two'

group getino
compare 'pidfd inode' getino '"$TOOL" 1'
compare 'pid and inode output' getino '"$TOOL" --print-pid 1'
compare 'validated pidfd inode' getino \
        'inode=$("$TOOL" 1) && "$TOOL" "1:$inode"'
compare 'multiple pids' getino '"$TOOL" 1 1'
compare 'user namespace inode' getino '"$TOOL" --userns 1'
compare 'namespace options exclusive' getino \
        '"$TOOL" --pidfs --userns 1'
compare 'invalid pid' getino '"$TOOL" invalid-pid'
compare 'wrong pidfd inode' getino '"$TOOL" 1:1'

group setarch
compare 'show current personality' setarch '"$TOOL" --show'
compare 'show named personality flags' setarch '"$TOOL" --show=0x40000'
compare 'show signed personality' setarch '"$TOOL" --show=-1'
compare 'show unnamed base personality' setarch '"$TOOL" --show=1'
compare 'abbreviated show option' setarch '"$TOOL" --sho=0'
compare 'show own process personality' setarch \
        'exec "$TOOL" --show -p $$'
compare 'list architectures' setarch '"$TOOL" --list'
compare 'no-argument option rejects value' setarch \
        '"$TOOL" --list=garbage'
compare 'native architecture command' setarch \
        '"$TOOL" "$(uname -m)" /bin/uname -m'
compare 'linux32 architecture command' setarch \
        '"$TOOL" linux32 /bin/uname -m'
compare 'architecture option boundary' setarch \
        '"$TOOL" "$(uname -m)" -- /bin/echo boundary'
compare 'uname 2.6 personality' setarch \
        '"$TOOL" -v --uname-2.6 /bin/uname -r'
compare 'pid requires show' setarch '"$TOOL" -p $$ /bin/true'
compare 'unknown architecture' setarch '"$TOOL" impossible /bin/true'
compare 'ignored 4gb alone has no policy' setarch '"$TOOL" --4gb /bin/true'

group setpriv
compare 'dump process privileges' setpriv '"$TOOL" -d'
compare 'dump capability sets' setpriv '"$TOOL" -dd'
compare 'dump saved identities' setpriv '"$TOOL" -ddd'
compare 'list known capabilities' setpriv '"$TOOL" --list-caps'
compare 'no new privileges' setpriv \
        '"$TOOL" --nnp /bin/sh -c '\''"$TOOL" -d | grep "^no_new_privs:"'\'''
compare 'real and effective uid' setpriv \
        '"$TOOL" --reuid "$(id -u)" /usr/bin/id -u'
compare 'real uid only' setpriv \
        '"$TOOL" --ruid "$(id -ru)" /usr/bin/id -ru'
compare 'effective uid only' setpriv \
        '"$TOOL" --euid "$(id -u)" /usr/bin/id -u'
compare 'real and effective gid' setpriv \
        '"$TOOL" --regid "$(id -g)" --keep-groups /usr/bin/id -g'
compare 'keep supplementary groups' setpriv \
        '"$TOOL" --keep-groups /bin/true'
compare 'invalid groups stop before command' setpriv \
        '"$TOOL" --groups impossible /bin/sh -c "printf wrong"'
compare 'parent death signal' setpriv \
        '"$TOOL" --pdeathsig TERM /bin/sh -c '\''"$TOOL" -d | tail -1'\'''
compare 'lowercase parent death signal' setpriv \
        '"$TOOL" --pdeathsig term /bin/true'
compare 'zero parent death signal rejected' setpriv \
        '"$TOOL" --pdeathsig 0 /bin/true'
compare 'ptracer none' setpriv '"$TOOL" --ptracer none /bin/true'
compare 'zero ptracer rejected' setpriv '"$TOOL" --ptracer 0 /bin/true'
compare 'gid requires group policy' setpriv \
        '"$TOOL" --regid "$(id -g)" /bin/true'
compare 'duplicate no new privileges' setpriv \
        '"$TOOL" --nnp --nnp /bin/true'
compare 'dump excludes commands' setpriv '"$TOOL" -d /bin/true'

# These security policies need substantially different state engines.  The
# denominator recognizes every spelling and refuses it before exec, so a
# caller can never mistake an ignored privilege request for success.
setpriv_gap()
{
        if "$work/bin/setpriv" "$@" /bin/true >/dev/null 2>&1; then
                lost "$1" 'unsupported policy was silently accepted'
        elif [ "$?" = 1 ]; then won
        else lost "$1" 'unsupported policy did not fail with usage status 1'
        fi
}
group setpriv-explicit-gaps
setpriv_gap --inh-caps=-all
setpriv_gap --ambient-caps=-all
setpriv_gap --bounding-set=-all
setpriv_gap --securebits=-all
setpriv_gap --init-groups --ruid "$(id -u)"
setpriv_gap --selinux-label=test
setpriv_gap --apparmor-profile=test
setpriv_gap --landlock-access=fs
setpriv_gap --landlock-rule=path-beneath:read-file:/
setpriv_gap --seccomp-filter=/no/such/filter
setpriv_gap --reset-env

group waitpid
compare 'already exited pid allowed' waitpid '"$TOOL" -e 2147483647'
compare 'missing pid rejected' waitpid '"$TOOL" 2147483647'
compare 'leading plus pid' waitpid '"$TOOL" -e +2147483647'
compare 'leading blank pid' waitpid '"$TOOL" -e " 2147483647"'
compare 'option after pid' waitpid '"$TOOL" 2147483647 -e'
compare 'timeout after pid' waitpid '"$TOOL" 1 -t .01'
compare 'zero pid rejected' waitpid '"$TOOL" -e 0'
compare 'timeout status' waitpid '"$TOOL" -t .01 1'
compare 'whole trailing-dot timeout' waitpid '"$TOOL" -t 0. -e 2147483647'
compare 'wait for process' waitpid \
        'sleep .03 & pid=$!; "$TOOL" -v -t 1 "$pid" | sed "s/PID [0-9]*/PID PID/"; wait "$pid"'
compare 'wait for one of two' waitpid \
        'sleep .03 & one=$!; sleep .2 & two=$!; "$TOOL" -v -t 1 -c 1 "$one" "$two" | sed "s/PID [0-9]*/PID PID/"; wait "$one"; wait "$two"'
compare 'pidfd inode validation' waitpid \
        'sleep .05 & pid=$!; ino=$(python3 -c '\''import os,sys; fd=os.pidfd_open(int(sys.argv[1])); print(os.fstat(fd).st_ino)'\'' "$pid"); "$TOOL" -t 1 "$pid:$ino"; wait "$pid"'
compare 'wrong pidfd inode rejected' waitpid '"$TOOL" -t .01 1:1'
compare 'count exceeds operands' waitpid '"$TOOL" -c 2 -t .01 1'
compare 'count excludes exited mode' waitpid \
        '"$TOOL" -c 1 -e 2147483647'
compare 'invalid timeout' waitpid '"$TOOL" -t impossible 1'


group unshare
compare 'map root user' unshare \
        '"$TOOL" -Ur /bin/sh -c '\''id -u; id -g; cat /proc/self/uid_map; cat /proc/self/gid_map'\'''
compare 'map current user' unshare \
        '"$TOOL" -Uc /bin/sh -c '\''id -u; id -g; cat /proc/self/uid_map; cat /proc/self/gid_map'\'''
compare 'map chosen identities' unshare \
        '"$TOOL" -U --map-user=7 --map-group=8 /bin/sh -c '\''id -u; id -g'\'''
compare 'mapping precedence current' unshare \
        '"$TOOL" -Urc /bin/sh -c '\''id -u; id -g'\'''
compare 'mapping precedence root' unshare \
        '"$TOOL" -Ucr /bin/sh -c '\''id -u; id -g'\'''
compare 'chosen user supersedes root' unshare \
        '"$TOOL" -Ur --map-user=7 /bin/sh -c '\''id -u; id -g'\'''
compare 'root supersedes chosen user' unshare \
        '"$TOOL" -U --map-user=7 -r /bin/sh -c '\''id -u; id -g'\'''
compare 'wide mapped identity' unshare \
        '"$TOOL" -U --map-user=2147483648 /bin/sh -c '\''id -u; cat /proc/self/uid_map'\'''
compare 'setgroups without gid map' unshare \
        '"$TOOL" -U --setgroups=deny /bin/sh -c '\''cat /proc/self/setgroups'\'''
if [ "$(id -u)" != 0 ] && command -v newuidmap >/dev/null 2>&1; then
        compare 'range merged around single' unshare \
                '"$TOOL" -U --map-users=0:100000:10 --map-user=5 /bin/sh -c '\''cat /proc/self/uid_map'\'''
        compare 'repeated mapping ranges' unshare \
                '"$TOOL" -U --map-users=0:100000:5 --map-users=10:100010:5 /bin/sh -c '\''cat /proc/self/uid_map'\'''
fi
compare 'combined namespace cluster' unshare \
        'out=$("$TOOL" -Urnm --propagation unchanged /bin/sh -c '\''for n in user mnt net; do readlink /proc/self/ns/$n; done'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; exit "$status"'
compare 'pid namespace fork' unshare \
        '"$TOOL" -Urpf /bin/sh -c '\''echo $$'\'''
compare 'forked exit status' unshare \
        '"$TOOL" -Urf /bin/sh -c "exit 7"'
compare 'forked signal status' unshare \
        '"$TOOL" -Urf /bin/sh -c '\''kill -TERM $$'\'''
compare 'time offsets' unshare \
        '"$TOOL" -UrTf --monotonic 7 --boottime -3 /bin/sh -c '\''cat /proc/self/timens_offsets'\'''
compare 'working directory' unshare \
        '"$TOOL" -Ur --wd "$0" /bin/pwd' "$work"
compare 'long namespace options' unshare \
        'out=$("$TOOL" --user --map-root-user --net --propagation unchanged /bin/sh -c '\''id -u; readlink /proc/self/ns/net'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; exit "$status"'
compare 'invalid propagation' unshare \
        '"$TOOL" -Um --propagation impossible /bin/true'
compare 'time offset requires namespace' unshare \
        '"$TOOL" --monotonic 1 /bin/true'
compare 'setgroups requires namespace' unshare \
        '"$TOOL" --setgroups deny /bin/true'
compare 'ignored SIGCHLD before fork' unshare \
        'trap '\'''\'' CHLD; "$TOOL" -Urf /bin/true'
compare 'default command honors SHELL' unshare \
        'SHELL=/bin/false "$TOOL" -Ur'
if command -v python3 >/dev/null 2>&1; then
        compare_signal 'fork preserves signal death' unshare -Urf \
                /bin/sh -c 'kill -TERM $$'
fi

group nsenter
compare 'enter user mount and net' nsenter \
        'unshare -Urnm /bin/sh -c "sleep 5" & target=$!; sleep .1; out=$("$TOOL" -t "$target" -U -m -n --preserve-credentials /bin/sh -c '\''id -u; for n in user mnt net; do readlink /proc/self/ns/$n; done'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'long namespace options' nsenter \
        'unshare -Urn /bin/sh -c "sleep 5" & target=$!; sleep .1; out=$("$TOOL" --target "$target" --user --net --preserve-credentials /bin/sh -c '\''id -u; readlink /proc/self/ns/net'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'target working directory' nsenter \
        'unshare -Ur /bin/sh -c '\''cd "$1" && sleep 5'\'' sh "$0" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials -w /bin/pwd; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"' "$work"
compare 'long target working directory' nsenter \
        'unshare -Ur /bin/sh -c '\''cd "$1" && sleep 5'\'' sh "$0" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials --wd /bin/pwd; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"' "$work"
compare 'long target root' nsenter \
        'unshare -Ur /bin/sh -c "sleep 5" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials --root /bin/pwd; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'explicit credentials override preserve' nsenter \
        'unshare -U --map-user=7 --map-group=8 /bin/sh -c "sleep 5" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials --setuid=7 --setgid=8 /bin/sh -c '\''id -u; id -g'\''; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'follow target credentials' nsenter \
        'unshare -U --map-user=7 --map-group=8 /bin/sh -c "sleep 5" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials --setuid=follow --setgid=follow /bin/sh -c '\''id -u; id -g'\''; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
subject 'credential aliases last win' nsenter \
        'unshare -U --map-user=7 --map-group=8 /bin/sh -c "sleep 5" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials -S7 --setuid=invalid -G8 --setgid=invalid /bin/true; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; test "$status" -ne 0'
subject 'bare root and wd are sticky' nsenter \
        'unshare -Ur /bin/sh -c "cd /tmp; sleep 5" & target=$!; sleep .1; out=$("$TOOL" -t "$target" -U --preserve-credentials --root=/no --root --wd=/no --wd /bin/pwd); status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; test "$status" = 0 && test "$out" = /tmp'
compare 'wide entered identity' nsenter \
        'unshare -U --map-user=2147483648 /bin/sh -c "sleep 5" & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials --setuid=2147483648 /bin/sh -c '\''id -u'\''; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'explicit namespace files' nsenter \
        'unshare -Urn /bin/sh -c "sleep 5" & target=$!; sleep .1; out=$("$TOOL" --user="/proc/$target/ns/user" --net="/proc/$target/ns/net" --preserve-credentials /bin/sh -c '\''id -u; readlink /proc/self/ns/net'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'pid namespace forks command' nsenter \
        'mark=$0/pid-target; rm -f "$mark"; unshare -Urp /bin/sh -c '\''sleep 5 & echo $! > "$1"; wait'\'' sh "$mark" & owner=$!; tries=0; while [ ! -s "$mark" ] && [ "$tries" -lt 50 ]; do sleep .02; tries=$((tries + 1)); done; target=$(cat "$mark"); out=$("$TOOL" -t "$target" -U -p --preserve-credentials /bin/sh -c '\''echo $$; readlink /proc/self/ns/pid'\''); status=$?; printf "%s\n" "$out" | sed -E "s/\[[0-9]+\]/[ID]/"; kill "$owner" 2>/dev/null; wait "$owner" 2>/dev/null; rm -f "$mark"; exit "$status"' "$work"
compare 'attached namespace path' nsenter \
        '"$TOOL" -m/proc/self/ns/mnt /bin/true'
compare 'bare wdns keeps command' nsenter \
        'unshare -Ur /bin/sh -c '\''cd /tmp; sleep 5'\'' & target=$!; sleep .1; "$TOOL" -t "$target" -U --preserve-credentials -W /bin/pwd; status=$?; kill "$target" 2>/dev/null; wait "$target" 2>/dev/null; exit "$status"'
compare 'default command honors SHELL' nsenter \
        'SHELL=/bin/false "$TOOL" -U/proc/self/ns/user --preserve-credentials'
compare 'requires target' nsenter '"$TOOL" -m /bin/true'
compare 'invalid target' nsenter '"$TOOL" -t impossible -m /bin/true'

if [ "$(id -u)" = 0 ]; then
        mkdir "$work/root"
        mkdir "$work/root/proc"
        cp "$subject" "$work/root/shell"
        compare 'root precedes proc mount' unshare \
                '"$TOOL" -m --root="$0" --mount-proc=/proc /shell -c '\''test -r /proc/self/status'\''' \
                "$work/root"
        compare 'detached proc root and wd' nsenter \
                'mark=$0/detached; rm -f "$mark"; unshare -m /bin/sh -c '\''echo $$ > "$1"; umount -l /proc; cd /tmp; sleep 5'\'' sh "$mark" & owner=$!; while [ ! -s "$mark" ]; do sleep .02; done; target=$(cat "$mark"); "$TOOL" -t "$target" -m -r -w /bin/pwd; status=$?; kill "$target" 2>/dev/null; wait "$owner" 2>/dev/null; exit "$status"' \
                "$work"
        compare 'all skips current user ns' nsenter \
                'mark=$0/all; rm -f "$mark"; unshare -mn /bin/sh -c '\''echo $$ > "$1"; sleep 5'\'' sh "$mark" & owner=$!; while [ ! -s "$mark" ]; do sleep .02; done; target=$(cat "$mark"); "$TOOL" -a -t "$target" --preserve-credentials /bin/true; status=$?; kill "$target" 2>/dev/null; wait "$owner" 2>/dev/null; exit "$status"' \
                "$work"
fi

# Exact upstream executable denominator. The supported list is intentionally
# separate: every upstream name must be in exactly one side, and implementing
# a remaining name makes this fail until the capability claim is moved.
upstream='addpart agetty bits blkdiscard blkid blkpr blkzone blockdev cal cfdisk chcpu chfn chmem choom chrt chsh col colcrt colrm column copyfilerange coresched ctrlaltdel delpart dmesg eject enosys exch fadvise fallocate fdisk fincore findfs findmnt flock fsck fsck.cramfs fsck.minix fsfreeze fstrim getino getopt hardlink hexdump hwclock ionice ipcmk ipcrm ipcs irqtop isosize kill last lastlog2 ldattach line logger login look losetup lsblk lsclocks lscpu lsfd lsipc lsirq lslocks lslogins lsmem lsns mcookie mesg mkfs mkfs.bfs mkfs.cramfs mkfs.minix mkswap more mount mountpoint namei newgrp nologin nsenter partx pg pipesz pivot_root prlimit readprofile rename renice resizepart rev rfkill rtcwake runuser script scriptlive scriptreplay setarch setpgid setpriv setsid setterm sfdisk su sulogin swaplabel swapoff swapon switch_root taskset tunelp uclampset ul umount unshare utmpdump uuidd uuidgen uuidparse vipw waitpid wall wdctl whereis wipefs write zramctl'
supported='blkid choom chrt exch fadvise findfs findmnt flock getino ionice kill mount mountpoint nsenter prlimit renice rev setarch setpgid setpriv setsid taskset uclampset umount unshare waitpid'

awk -F '[(),[:space:]]+' '$1 == "SHELL_TOOL" { print $3 }' \
        "$root/src/sh/tools.inc" | sort -u > "$work/dispatched"

printf '%s\n' $upstream | sort -u > "$work/upstream"
printf '%s\n' $supported | sort -u > "$work/supported"
comm -23 "$work/upstream" "$work/supported" > "$work/remaining"

section util-linux-ledger
group supported
while IFS= read -r name; do
        if grep -qx "$name" "$work/dispatched"; then
                won
        else
                lost "$name" 'claimed supported but absent from dispatch'
        fi
done < "$work/supported"

group remaining
while IFS= read -r name; do
        if grep -qx "$name" "$work/dispatched"; then
                lost "$name" 'now dispatched -- move it to supported'
        else
                won
        fi
done < "$work/remaining"

section
printf '\n  %s of %s\n' "$pass" "$((pass + fail))"
[ "$fail" = 0 ]
