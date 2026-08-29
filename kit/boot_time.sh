#!/bin/sh
#
#       How long the stack takes to reach a shell, and where the time goes.
#
#           sh kit/boot_time.sh [runs] [extra kernel arguments]
#
#       Boots dist/bootx64.efi under qemu and reports, from the GUEST clock:
#       when moonwater starts, when the canvas has a picture on it, and when
#       the shell runs. Also whether the prompt appeared at all, because a
#       configuration that starts init sooner and never reaches a prompt is
#       not faster, and two of them look identical from a timestamp.
#
#       THREE MEASUREMENT TRAPS ARE ANSWERED HERE, each of which produced a
#       confident wrong number before it was found.
#
#       qemu BLOCK BUFFERS ITS STDOUT when it is redirected to a file. Killing
#       the guest then throws away whatever had not filled a block, so a VM
#       that printed a prompt at 0.78 seconds looks like a VM that never
#       printed one. `-serial file:` writes the guest console straight out and
#       is the only form used here.
#
#       THE PROMPT IS WRITTEN MID-LINE and kernel messages interleave after
#       it, so a line-oriented grep does not see it until some later message
#       happens to end the line. That read as a 15 second regression in a
#       configuration that was fine. The prompt is looked for in raw bytes.
#
#       HOST WALL CLOCK MEASURES qemu, not the guest. Start-up varies by more
#       than the differences being chased. Every number below is a printk
#       timestamp, taken inside the guest.
#
set -u

runs=${1:-5}
shift 2>/dev/null || true
extra=${*:-}
image=${MOONWATER_IMAGE:-dist/bootx64.efi}

command -v qemu-system-x86_64 > /dev/null 2>&1 || {
        echo "  boot_time    NOT RUN -- no qemu-system-x86_64"; exit 2; }
[ -f "$image" ] || { echo "  boot_time    NOT RUN -- no $image"; exit 2; }

marker=$(mktemp)
cat > "$marker" <<'PY'
import sys
try:
        data = open(sys.argv[1], "rb").read()
except OSError:
        sys.exit(1)
sys.exit(0 if b"$ " in data else 1)
PY

one()
{
        log=$(mktemp -u)
        qemu-system-x86_64 -m 2G -smp 2 -cpu host -enable-kvm \
                -kernel "$image" -no-reboot -display none \
                -serial "file:$log" \
                -append "console=ttyS0 drm_client_lib.active= printk.time=1 $extra" \
                < /dev/null > /dev/null 2>&1 &
        pid=$!
        n=0
        while [ $n -lt 250 ]; do
                [ -s "$log" ] && python3 "$marker" "$log" && break
                sleep 0.02
                n=$((n + 1))
        done
        kill $pid 2>/dev/null
        wait $pid 2>/dev/null

        at() { grep -m1 "$1" "$log" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1; }
        mw=$(at 'Moonwater start')
        sc=$(grep -m1 'is on the screen' "$log" 2>/dev/null |
             grep -oE '^\[ *[0-9]+\.[0-9]+' | tr -d '[ ' | head -1)
        it=$(at 'Run /init')
        if [ -s "$log" ] && python3 "$marker" "$log"; then p=1; else p=0; fi
        echo "${mw:-NONE} ${sc:-NONE} ${it:-NONE} $p"
        rm -f "$log"
}

middle() { printf '%s\n' "$@" | tr ' ' '\n' | grep -v '^$\|NONE' | sort -n |
           awk '{a[NR]=$1} END {print a[int((NR + 1) / 2)]}'; }

mws=''; scs=''; its=''; seen=0; n=0
while [ "$n" -lt "$runs" ]; do
        set -- $(one)
        mws="$mws $1"; scs="$scs $2"; its="$its $3"; seen=$((seen + $4))
        n=$((n + 1))
done
rm -f "$marker"

printf '  moonwater starts   %s s\n' "$(middle $mws)"
printf '  canvas on screen   %s s\n' "$(middle $scs)"
printf '  shell runs         %s s\n' "$(middle $its)"
printf '  prompt reached     %d of %d boots\n' "$seen" "$runs"
[ "$seen" -eq "$runs" ] || echo "  NOT EVERY BOOT REACHED A PROMPT -- the timings above are not a result"
