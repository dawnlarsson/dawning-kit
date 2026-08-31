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
for name in setsid setpgid ionice fadvise; do
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

section util-linux

group reference
for utility in setsid setpgid ionice fadvise; do
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

# Exact upstream executable denominator. The supported list is intentionally
# separate: every upstream name must be in exactly one side, and implementing
# a remaining name makes this fail until the capability claim is moved.
upstream='addpart agetty bits blkdiscard blkid blkpr blkzone blockdev cal cfdisk chcpu chfn chmem choom chrt chsh col colcrt colrm column copyfilerange coresched ctrlaltdel delpart dmesg eject enosys exch fadvise fallocate fdisk fincore findfs findmnt flock fsck fsck.cramfs fsck.minix fsfreeze fstrim getino getopt hardlink hexdump hwclock ionice ipcmk ipcrm ipcs irqtop isosize kill last lastlog2 ldattach line logger login look losetup lsblk lsclocks lscpu lsfd lsipc lsirq lslocks lslogins lsmem lsns mcookie mesg mkfs mkfs.bfs mkfs.cramfs mkfs.minix mkswap more mount mountpoint namei newgrp nologin nsenter partx pg pipesz pivot_root prlimit readprofile rename renice resizepart rev rfkill rtcwake runuser script scriptlive scriptreplay setarch setpgid setpriv setsid setterm sfdisk su sulogin swaplabel swapoff swapon switch_root taskset tunelp uclampset ul umount unshare utmpdump uuidd uuidgen uuidparse vipw waitpid wall wdctl whereis wipefs write zramctl'
supported='blkid fadvise findfs findmnt ionice kill mount mountpoint rev setpgid setsid umount'

awk '
        /static shell_tool shell_tools\[\]/ { inside=1; next }
        inside && /\{null, null\}/ { exit }
        inside && match($0, /\{"[^"]+"/) {
                name=substr($0, RSTART + 2, RLENGTH - 3)
                print name
        }
' "$root/src/sh/builtin.c" | sort -u > "$work/dispatched"

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
