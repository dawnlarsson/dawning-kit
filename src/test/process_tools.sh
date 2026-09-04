#!/bin/sh
#
#       chroot, nohup and timeout against GNU coreutils.
#
#       These are process wrappers, so their visible contract is a command's
#       output and wait status rather than a transformed file.  Timing cases
#       use generous separation between the deadline and the command's sleep;
#       they do not compare elapsed wall time, which would make load a test
#       input nobody asked for.

LC_ALL=C
export LC_ALL

bin=${1:-/tmp/mwfarm}
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

report()
{
        fail=$((fail + 1))
        printf '  %-12s %-32s %s\n' process "$1" "$2"
}

show()
{
        head -c 48 "$1" | tr '\n\t' '|>'
}

compare()
{
        name=$1
        tool=$2
        shift 2

        "$tool" "$@" < /dev/null > "$work/want" 2> "$work/want_err"
        want_status=$?
        "$bin/$tool" "$@" < /dev/null > "$work/got" 2> "$work/got_err"
        got_status=$?

        if cmp -s "$work/want" "$work/got" &&
                cmp -s "$work/want_err" "$work/got_err" &&
                [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return
        fi

        report "$name" "want $(show "$work/want")[$want_status] / $(show "$work/want_err") got $(show "$work/got")[$got_status] / $(show "$work/got_err")"
}

compare_status()
{
        name=$1
        tool=$2
        shift 2

        "$tool" "$@" < /dev/null > "$work/want" 2> "$work/want_err"
        want_status=$?
        "$bin/$tool" "$@" < /dev/null > "$work/got" 2> "$work/got_err"
        got_status=$?

        if [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return
        fi

        report "$name" "want status $want_status, got $got_status"
}

for needed in timeout nohup chroot; do
        if ! command -v "$needed" > /dev/null 2>&1; then
                echo "  process      NOT RUN -- no coreutils $needed reference"
                exit 2
        fi
done

compare 'exit and streams' timeout 1 /bin/sh -c \
        'printf output; printf error >&2; exit 7'
compare 'zero disables timer' timeout 0 /bin/sh -c 'exit 23'
compare_status 'deadline' timeout .02 /bin/sleep 1
compare_status 'named signal' timeout -s HUP .02 /bin/sleep 1
compare_status 'long signal' timeout --signal=KILL .02 /bin/sleep 1
compare_status 'preserve status' timeout --preserve-status .02 /bin/sleep 1
compare_status 'foreground' timeout --foreground .02 /bin/sleep 1
compare_status 'minute suffix' timeout .001m /bin/sleep 1
compare_status 'TERM then KILL' timeout -k .02 .02 /bin/sh -c \
        'trap "" TERM; sleep 1'
compare_status 'invalid interval' timeout impossible /bin/true
compare_status 'missing command' timeout 1 no-such-moonwater-command

"$bin/timeout" -v .02 /bin/sleep 1 > "$work/got" 2> "$work/got_err"
verbose_status=$?
if [ "$verbose_status" = 124 ] &&
        grep -q "sending signal TERM to command '/bin/sleep'" \
                "$work/got_err"; then
        pass=$((pass + 1))
else
        report 'verbose timeout' "status $verbose_status / $(show "$work/got_err")"
fi

relay_pid="$work/relay.pid"
"$bin/timeout" 5 /bin/sh -c \
        'echo $$ > "$1"; sleep 5' process-relay "$relay_pid" &
relay_wrapper=$!
relay_tries=0
while [ ! -s "$relay_pid" ] && [ "$relay_tries" -lt 100 ]; do
        relay_tries=$((relay_tries + 1))
        sleep .01
done

relay_child=$(cat "$relay_pid" 2>/dev/null)
kill -TERM "$relay_wrapper" 2>/dev/null
wait "$relay_wrapper"
relay_status=$?

if [ -n "$relay_child" ] && [ "$relay_status" = 143 ] &&
        ! kill -0 "$relay_child" 2>/dev/null; then
        pass=$((pass + 1))
else
        report 'relay TERM' "status $relay_status, child ${relay_child:-missing} survived"
        [ -z "$relay_child" ] || kill -KILL "$relay_child" 2>/dev/null
fi

compare 'SIGHUP ignored' nohup /bin/sh -c \
        'kill -HUP $$; printf alive; printf error >&2'
compare_status 'nohup missing' nohup no-such-moonwater-command

# A real root transition requires privilege.  Root compares both the normal
# chdir-to-slash policy and --skip-chdir; every other account still compares
# the stable syntax/failure statuses rather than silently omitting chroot.
compare_status 'chroot missing' chroot

if [ "$(id -u)" = 0 ]; then
        compare 'root and command' chroot / /bin/sh -c \
                'printf rooted; exit 9'

        old=$PWD
        cd "$work" || exit 1
        compare 'skip chdir' chroot --skip-chdir / /bin/pwd
        cd "$old" || exit 1
else
        compare_status 'root permission' chroot / /bin/true
fi

# With all three descriptors attached to a terminal, stdout and stderr append
# to one mode-0600 file.  script's transcript contains its own timestamps and
# command path, so compare the deterministic file and the diagnostic shape.
if command -v script > /dev/null 2>&1; then
        tty_work="$work/tty"
        mkdir "$tty_work" || exit 1

        if (cd "$tty_work" && script -qefc \
                "$bin/nohup /bin/sh -c 'printf out; printf err >&2'" \
                terminal > /dev/null 2>&1) &&
                [ "$(cat "$tty_work/nohup.out")" = outerr ] &&
                [ "$(stat -c %a "$tty_work/nohup.out" 2>/dev/null)" = 600 ] &&
                grep -q "ignoring input and appending output" \
                        "$tty_work/terminal"; then
                pass=$((pass + 1))
        else
                report 'nohup terminal' 'redirection, mode, or diagnostic differed'
        fi
fi

total=$((pass + fail))
printf '\n  %-12s %s of %s\n' process "$pass" "$total"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'process-utils %s %s\n' "$pass" "$total" >> "$TEST_TALLY"

[ "$fail" = 0 ]
