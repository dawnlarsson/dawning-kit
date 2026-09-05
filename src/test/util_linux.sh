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
for name in addpart bits blockdev ctrlaltdel delpart resizepart isosize wipefs mkswap swaplabel coresched pivot_root rename setsid setpgid ionice fadvise fallocate copyfilerange getopt taskset renice prlimit chrt \
        uclampset flock unshare nsenter setarch setpriv waitpid choom exch \
        getino fincore hardlink ipcmk ipcrm ipcs lsblk lsclocks lscpu lsfd lsipc lslocks lsmem lsns namei whereis mcookie mesg rfkill uuidgen uuidparse cal col colcrt colrm column dmesg \
        last logger look line nologin pipesz script scriptreplay ul utmpdump wall write; do
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

compare_full()
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

        if TOOL=$reference sh -c "$script" "$@" \
                > "$work/want" 2> "$work/want.error"; then
                want_status=0
        else
                want_status=$?
        fi
        if TOOL="$work/bin/$utility" sh -c "$script" "$@" \
                > "$work/got" 2> "$work/got.error"; then
                got_status=0
        else
                got_status=$?
        fi

        if cmp -s "$work/want" "$work/got" &&
           cmp -s "$work/want.error" "$work/got.error" &&
           [ "$want_status" = "$got_status" ]; then
                won
        else
                lost "$name" \
                     "want $(shown "$work/want") / $(shown "$work/want.error")[$want_status], got $(shown "$work/got") / $(shown "$work/got.error")[$got_status]"
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
for utility in addpart bits blockdev ctrlaltdel delpart resizepart isosize wipefs mkswap swaplabel coresched pivot_root rename setsid setpgid ionice fadvise fallocate copyfilerange getopt taskset renice prlimit chrt \
        uclampset flock unshare nsenter setarch setpriv waitpid choom exch \
        getino fincore ipcmk ipcrm ipcs lsblk lsclocks lscpu lsfd lsipc lslocks lsmem lsns namei mcookie mesg rfkill uuidgen uuidparse cal col colcrt colrm column dmesg \
        last logger look pipesz script scriptreplay ul utmpdump wall write; do
        version=$($utility --version 2>/dev/null | head -1 || true)
        case $version in
        *'util-linux 2.42.2'*) won ;;
        *) lost "$utility" "need util-linux 2.42.2 reference, got [$version]" ;;
        esac
done

group bits
compare 'list to plain mask' bits \
        '"$TOOL" -m 4,5-8 16,30'
compare 'mask to compressed list' bits \
        '"$TOOL" -l 0xeec2'
compare 'nibble-grouped binary' bits \
        '"$TOOL" -b 4,5-8 16,30'
compare '32-bit grouped mask' bits \
        '"$TOOL" -g 2,22,74,79'
compare 'grouped input mask' bits \
        '"$TOOL" -l ,00300000,03000000,30000003'
compare 'bitwise group operations' bits \
        '"$TOOL" -l 0xff "~1-3" "^8-10" "&0-9"'
compare 'stepped range compression' bits \
        '"$TOOL" -l 0-30:3 40,42,44'
compare 'width truncation' bits \
        '"$TOOL" -w 64 -l 2,22,74,79'
compare 'stdin groups combine' bits \
        'printf "0-30:3\n2\n~1\n0-12:3\n" | "$TOOL" -l'
compare 'last output mode wins' bits \
        '"$TOOL" -m -l 0xf; "$TOOL" -l -m 0xf; "$TOOL" -g -b 32'
compare 'empty masks in every output mode' bits \
        'for mode in -m -g -b -l; do "$TOOL" "$mode" 0x0 || exit; done'
compare 'empty stdin list emits zero bytes' bits \
        'printf "" | "$TOOL" -l'
compare 'generated masks lists widths and modes' bits \
        'for mode in -m -g -b -l; do for width in 1 2 31 32 33 64 65 127 128 129 8192; do for input in 0 1 2 31 32 63 64 74 127 128 1024 1,2 1,2,3 0-10:2 0-10:3 2,22,74,79 0xeec2 0x00000000ffffffff ,00300000,03000000,30000003; do "$TOOL" "$mode" -w "$width" "$input" || exit; done; done; done'
compare 'maximum bounded width' bits \
        '"$TOOL" -w 131072 -l 0,65535,131071,131072'
subject 'zero and oversized widths reject' bits \
        '! "$TOOL" -w 0 1 >/dev/null 2>&1 && ! "$TOOL" -w 131073 1 >/dev/null 2>&1 && ! "$TOOL" -w 18446744073709551616 1 >/dev/null 2>&1'
# util-linux 2.42.2 silently turns several malformed lists into an empty or
# partial mask.  Moonwater deliberately rejects those inputs instead.
subject 'overflow and malformed groups reject' bits \
        '! "$TOOL" -l 18446744073709551616 >/dev/null 2>&1 && ! "$TOOL" -l none >/dev/null 2>&1 && ! "$TOOL" -l abc >/dev/null 2>&1 && ! "$TOOL" -l 3-1 >/dev/null 2>&1 && ! "$TOOL" -l 1-3:0 >/dev/null 2>&1 && ! "$TOOL" -l 1,,2 >/dev/null 2>&1 && ! "$TOOL" -m 0x_1 >/dev/null 2>&1 && ! "$TOOL" -m ,1,,2 >/dev/null 2>&1'

group block-ioctls
blockdev_device=$(lsblk -dn -o PATH 2>/dev/null | sed -n '1p')
if [ -n "$blockdev_device" ] && sudo -n true >/dev/null 2>&1; then
        compare 'all read-only blockdev queries' blockdev \
                'for command in getsz getro getdiscardzeroes getss getpbsz getiomin getioopt getalignoff getmaxsect getbsz getsize getsize64 getra getfra getdiskseq getzonesz; do sudo -n "$TOOL" --$command "$1" || exit; done' \
                sh "$blockdev_device"
        compare 'ordered multi-query output' blockdev \
                'sudo -n "$TOOL" --getsz --getro --getss --getpbsz --getsize64 "$1"' \
                sh "$blockdev_device"
        compare 'verbose query labels' blockdev \
                'sudo -n "$TOOL" -v --getsz --getro --getss "$1"' \
                sh "$blockdev_device"
        compare 'single-device report' blockdev \
                'sudo -n "$TOOL" --report "$1"' sh "$blockdev_device"
fi
compare_full 'get size ioctl error' blockdev '"$TOOL" --getsz /dev/null'
compare_full 'read-only ioctl error' blockdev '"$TOOL" --getro /dev/null'
compare_full 'flush ioctl error' blockdev '"$TOOL" --flushbufs /dev/null'
compare_full 'add partition ioctl error' addpart \
        '"$TOOL" /dev/null 1 2 3'
compare_full 'delete partition ioctl error' delpart \
        '"$TOOL" /dev/null 1'
subject 'partition operands reject overflow before ioctl' addpart \
        '"$TOOL" /dev/null 2147483648 1 1 >/dev/null 2>&1 && exit 1; "$TOOL" /dev/null 1 18014398509481984 1 >/dev/null 2>&1 && exit 1; "$TOOL" /dev/null 1 1 18014398509481984 >/dev/null 2>&1 && exit 1; :'
subject 'blockdev setter rejects numeric overflow' blockdev \
        '"$TOOL" --setra 18446744073709551616 /dev/null >/dev/null 2>&1; [ "$?" -ne 0 ]'

if sudo -n losetup --find >/dev/null 2>&1; then
        subject 'owned loop partition add resize delete' addpart \
                'image="$1/partition-loop"; truncate -s 32M "$image" || exit; loop=$(sudo -n losetup --find --show "$image") || exit; cleanup() { sudo -n losetup -d "$loop" 2>/dev/null || :; rm -f "$image"; }; trap cleanup EXIT INT TERM; sudo -n "$TOOL" "$loop" 1 2048 4096 || exit; base=${loop##*/}; part=${base}p1; [ -e "/sys/class/block/$part" ] || part=${base}1; [ "$(cat "/sys/class/block/$part/start")" = 2048 ] && [ "$(cat "/sys/class/block/$part/size")" = 4096 ] || exit 1; sudo -n "$2/resizepart" "$loop" 1 8192 || exit; [ "$(cat "/sys/class/block/$part/size")" = 8192 ] || exit 1; sudo -n "$2/delpart" "$loop" 1 || exit; tries=0; while [ -e "/sys/class/block/$part" ] && [ "$tries" -lt 20 ]; do sleep .05; tries=$((tries+1)); done; [ ! -e "/sys/class/block/$part" ]' \
                sh "$work" "$work/bin"
        subject 'owned loop blockdev setters and flush' blockdev \
                'image="$1/blockdev-loop"; truncate -s 8M "$image" || exit; loop=$(sudo -n losetup --find --show "$image") || exit; old_ra=$(sudo -n blockdev --getra "$loop") || exit; old_fra=$(sudo -n blockdev --getfra "$loop") || exit; cleanup() { sudo -n blockdev --setrw "$loop" 2>/dev/null || :; sudo -n blockdev --setra "$old_ra" "$loop" 2>/dev/null || :; sudo -n blockdev --setfra "$old_fra" "$loop" 2>/dev/null || :; sudo -n losetup -d "$loop" 2>/dev/null || :; rm -f "$image"; }; trap cleanup EXIT INT TERM; sudo -n "$TOOL" --setro "$loop" && [ "$(sudo -n "$TOOL" --getro "$loop")" = 1 ] || exit; sudo -n "$TOOL" --setrw "$loop" && [ "$(sudo -n "$TOOL" --getro "$loop")" = 0 ] || exit; next=$((old_ra + 8)); sudo -n "$TOOL" --setra "$next" "$loop" && [ "$(sudo -n "$TOOL" --getra "$loop")" = "$next" ] || exit; next_fra=$((old_fra + 8)); sudo -n "$TOOL" --setfra "$next_fra" "$loop" && [ "$(sudo -n "$TOOL" --getfra "$loop")" = "$next_fra" ] || exit; sudo -n "$TOOL" --setbsz 4096 --flushbufs "$loop"' \
                sh "$work"
fi

group storage-signatures
iso_image="$work/volume.iso"
python3 -c 'import struct,sys
b=bytearray(80*2048); o=16*2048
b[o]=1; b[o+1:o+6]=b"CD001"; b[o+6]=1
b[o+40:o+72]=b"MOONWATER"+b" "*23
b[o+80:o+84]=struct.pack("<I",64); b[o+84:o+88]=struct.pack(">I",64)
b[o+128:o+130]=struct.pack("<H",2048); b[o+130:o+132]=struct.pack(">H",2048)
open(sys.argv[1],"wb").write(b)' "$iso_image"
compare 'ISO9660 byte size' isosize '"$TOOL" "$1"' sh "$iso_image"
compare 'ISO9660 sector geometry' isosize '"$TOOL" -x "$1"' sh "$iso_image"
compare 'ISO9660 divisor' isosize '"$TOOL" -d1024 "$1"' sh "$iso_image"
compare 'ISO9660 multiple operands are named' isosize \
        '"$TOOL" "$1" "$1"' sh "$iso_image"
subject 'ISO9660 divisor overflow is rejected' isosize \
        '! "$TOOL" -d 18446744073709551616 "$1"' sh "$iso_image"

if command -v mkfs.ext4 >/dev/null 2>&1; then
        ext_image="$work/wipefs-ext.img"
        truncate -s 8M "$ext_image"
        mkfs.ext4 -q -F -L 'hello world' "$ext_image"
        compare 'filesystem signature table' wipefs \
                '"$TOOL" "$1"' sh "$ext_image"
        compare 'filesystem signature no headings' wipefs \
                '"$TOOL" -i "$1"' sh "$ext_image"
        compare 'filesystem signature parsable' wipefs \
                '"$TOOL" -p "$1"' sh "$ext_image"
        compare 'filesystem signature JSON' wipefs \
                '"$TOOL" -J "$1"' sh "$ext_image"
        compare 'filesystem signature selected columns' wipefs \
                '"$TOOL" -O DEVICE,OFFSET,TYPE,UUID,LABEL,LENGTH,USAGE "$1"' \
                sh "$ext_image"
        compare 'filesystem signature type include' wipefs \
                '"$TOOL" -t ext4 "$1"' sh "$ext_image"
        compare 'filesystem signature type exclude' wipefs \
                '"$TOOL" -t noext4 "$1"' sh "$ext_image"
        compare 'filesystem signature no-act all' wipefs \
                '"$TOOL" -n -a "$1"' sh "$ext_image"
        compare 'filesystem signature no-act offset' wipefs \
                '"$TOOL" -n -o 0x438 "$1"' sh "$ext_image"
        subject 'no-act leaves filesystem magic unchanged' wipefs \
                'before=$(dd if="$1" bs=1 skip=1080 count=2 2>/dev/null); "$TOOL" -n -a "$1" >/dev/null; after=$(dd if="$1" bs=1 skip=1080 count=2 2>/dev/null); [ "$before" = "$after" ]' \
                sh "$ext_image"
        subject 'actual signature erasure is rejected' wipefs \
                '! "$TOOL" -a "$1" >/dev/null 2>&1' sh "$ext_image"

        multi_image="$work/wipefs-multi.img"
        cp "$ext_image" "$multi_image"
        printf '\001\000\000\000' | dd of="$multi_image" bs=1 seek=1024 conv=notrunc status=none
        printf SWAPSPACE2 | dd of="$multi_image" bs=1 seek=4086 conv=notrunc status=none
        compare 'multiple signatures preserve probe order' wipefs \
                '"$TOOL" "$1"' sh "$multi_image"
fi

if command -v mkswap >/dev/null 2>&1; then
        swap_image="$work/wipefs-swap.img"
        truncate -s 2M "$swap_image"
        mkswap -q -L swaps "$swap_image"
        compare 'swap signature table' wipefs \
                '"$TOOL" "$1"' sh "$swap_image"
        compare 'swap signature complete columns' wipefs \
                '"$TOOL" -O DEVICE,OFFSET,TYPE,UUID,LABEL,LENGTH,USAGE "$1"' \
                sh "$swap_image"
fi
truncate -s 64K "$work/no-signature"
compare 'empty signature JSON' wipefs \
        '"$TOOL" -J "$1"' sh "$work/no-signature"

group swap-images
compare 'fixed swap creation output' mkswap \
        'p=$(mktemp "$1/mkswap-output.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; "$TOOL" -L bowl -U 00112233-4455-6677-8899-aabbccddeeff "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
compare 'fixed swap header bytes' mkswap \
        'p=$(mktemp "$1/mkswap-header.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; "$TOOL" -q -L bowl -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; dd if="$p" bs=4096 count=1 status=none | sha256sum; rm -f "$p"' \
        sh "$work"
compare '8192-byte swap page header' mkswap \
        'p=$(mktemp "$1/mkswap-page.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; "$TOOL" -q -p 8192 -L bowl -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; dd if="$p" bs=8192 count=1 status=none | sha256sum; rm -f "$p"' \
        sh "$work"
compare 'legacy KiB size bounds last page' mkswap \
        'p=$(mktemp "$1/mkswap-size.XXXXXX"); truncate -s 3M "$p"; chmod 600 "$p"; "$TOOL" -q -U 00112233-4455-6677-8899-aabbccddeeff "$p" 1024 || exit; od -An -tu4 -j1024 -N12 "$p"; rm -f "$p"' \
        sh "$work"
compare 'quiet swap creation' mkswap \
        'p=$(mktemp "$1/mkswap-quiet.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; "$TOOL" -q -U clear "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
subject 'random swap UUID has RFC version and variant' mkswap \
        'p=$(mktemp "$1/mkswap-random.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; "$TOOL" -q "$p" || exit; uuid=$("$2/swaplabel" "$p" | awk '\''$1 == "UUID:" {print $2}'\''); rm -f "$p"; [ "${uuid#????????-????-4???}" != "$uuid" ] && case ${uuid#????????-????-????-} in [89abAB]*) :;; *) exit 1;; esac' \
        sh "$work" "$work/bin"
subject 'swap creation rejects non-regular targets' mkswap \
        '! "$TOOL" -q /dev/null >/dev/null 2>&1'
subject 'unsupported broad mutation modes reject' mkswap \
        'p=$(mktemp "$1/mkswap-unsupported.XXXXXX"); truncate -s 2M "$p"; ! "$TOOL" -c "$p" >/dev/null 2>&1 && ! "$TOOL" -F "$p" >/dev/null 2>&1 && ! "$TOOL" -s 1M "$p" >/dev/null 2>&1; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"

compare 'swap label and UUID display' swaplabel \
        'p=$(mktemp "$1/swaplabel-show.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; mkswap -q -L bowl -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; "$TOOL" "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
compare 'swap label and UUID update' swaplabel \
        'p=$(mktemp "$1/swaplabel-set.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; mkswap -q -L old -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; "$TOOL" -L new -U ffeeddcc-bbaa-9988-7766-554433221100 "$p" || exit; "$TOOL" "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
compare 'long swap label truncation' swaplabel \
        'p=$(mktemp "$1/swaplabel-long.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; mkswap -q -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; "$TOOL" -L 0123456789abcdefXYZ "$p" 2>/dev/null || exit; "$TOOL" "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
compare 'cleared UUID is omitted' swaplabel \
        'p=$(mktemp "$1/swaplabel-clear.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; mkswap -q -L bowl -U clear "$p" || exit; "$TOOL" "$p"; status=$?; rm -f "$p"; exit "$status"' \
        sh "$work"
subject 'invalid UUID leaves swap metadata unchanged' swaplabel \
        'p=$(mktemp "$1/swaplabel-invalid.XXXXXX"); truncate -s 2M "$p"; chmod 600 "$p"; mkswap -q -L bowl -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; before=$(dd if="$p" bs=1 skip=1036 count=32 status=none | sha256sum); ! "$TOOL" -U invalid "$p" >/dev/null 2>&1 || exit; after=$(dd if="$p" bs=1 skip=1036 count=32 status=none | sha256sum); rm -f "$p"; [ "$before" = "$after" ]' \
        sh "$work"
subject 'swap relabel rejects non-regular mutation' swaplabel \
        '! "$TOOL" -L unsafe /dev/null >/dev/null 2>&1'

if command -v mkfs.ext4 >/dev/null 2>&1; then
        subject 'existing filesystem requires explicit force' mkswap \
                'p=$(mktemp "$1/mkswap-force.XXXXXX"); truncate -s 8M "$p"; chmod 600 "$p"; mkfs.ext4 -q -F "$p" || exit; before=$(sha256sum "$p"); ! "$TOOL" -q "$p" >/dev/null 2>&1 || exit; after=$(sha256sum "$p"); [ "$before" = "$after" ] || exit; "$TOOL" -q -f -U 00112233-4455-6677-8899-aabbccddeeff "$p" || exit; "$2/swaplabel" "$p" | grep -q 00112233-4455-6677-8899-aabbccddeeff; status=$?; rm -f "$p"; exit "$status"' \
                sh "$work" "$work/bin"
fi

group coresched
compare 'fixed process cookie query' coresched \
        '"$TOOL" get -s "$1"' sh "$$"
compare 'zero source rejected' coresched \
        '"$TOOL" get -s 0'
compare 'get rejects operands' coresched \
        '"$TOOL" get unexpected'
compare 'new cookie command handoff' coresched \
        '"$TOOL" new -- true'
subject 'owned process create and copy cycle' coresched \
        'sleep 30 & one=$!; sleep 30 & two=$!; cleanup() { kill "$one" "$two" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; before=$("$TOOL" get -s "$one" | awk '\''{print $NF}'\'') || exit; "$TOOL" new -t pid -d "$one" || exit; made=$("$TOOL" get -s "$one" | awk '\''{print $NF}'\'') || exit; [ "$made" != 0x0 ] && [ "$made" != "$before" ] || exit 1; "$TOOL" copy -s "$one" -t pid -d "$two" || exit; copied=$("$TOOL" get -s "$two" | awk '\''{print $NF}'\'') || exit; [ "$copied" = "$made" ]'
subject 'command exit status survives handoff' coresched \
        '"$TOOL" new -- sh -c '\''exit 7'\''; [ "$?" -eq 7 ]'
subject 'invalid destination type rejected' coresched \
        '"$TOOL" new -t nope -d "$1" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$$"

group root-transition
compare 'pivot root in isolated mount namespace' pivot_root \
        'd=$(mktemp -d "$1/pivot.XXXXXX") || exit; unshare -Urnm sh -c '\''mount --make-rprivate / && mount -t tmpfs tmpfs "$1" && mkdir "$1/old" && cd "$1" && "$TOOL" . old'\'' sh "$d"; answer=$?; rmdir "$d"; exit "$answer"' \
        sh "$work"
compare 'pivot root requires exactly two paths' pivot_root \
        '"$TOOL" only-one'
compare 'ctrlaltdel rejects an unknown policy' ctrlaltdel \
        '"$TOOL" neither'
subject 'ctrlaltdel soft reaches the privileged syscall' ctrlaltdel \
        'unshare -Ur "$TOOL" soft >/dev/null 2>&1; [ "$?" -ne 0 ]'

group rename
compare 'first all last and empty replacements' rename \
        'rm -rf "$1"; mkdir -p "$1"; cd "$1" || exit; touch first-foofoo all-foofoo last-foofoo empty-abc; "$TOOL" foo X first-foofoo || exit; "$TOOL" -a foo X all-foofoo || exit; "$TOOL" -l foo X last-foofoo || exit; "$TOOL" -a "" X empty-abc || exit; find . -mindepth 1 -maxdepth 1 -printf "%P\n" | sort' \
        sh "$work/rename-modes"
compare 'no overwrite preserves destination' rename \
        'rm -rf "$1"; mkdir -p "$1"; cd "$1" || exit; printf source > abc; printf destination > Xbc; "$TOOL" -o a X abc; answer=$?; printf "status=%s source=%s destination=%s\n" "$answer" "$(cat abc)" "$(cat Xbc)"' \
        sh "$work/rename-no-overwrite"
compare 'dry run verbose output and effect' rename \
        'rm -rf "$1"; mkdir -p "$1"; cd "$1" || exit; touch foo; "$TOOL" -nv f X foo; answer=$?; [ -e foo ] || exit 1; exit "$answer"' \
        sh "$work/rename-dry"
compare 'missing substring status' rename \
        'rm -rf "$1"; mkdir -p "$1"; cd "$1" || exit; touch abc; "$TOOL" z X abc' \
        sh "$work/rename-missing"
compare 'interactive overwrite accepted' rename \
        'rm -rf "$1"; mkdir -p "$1"; cd "$1" || exit; printf source > abc; printf old > Xbc; printf y | "$TOOL" -i a X abc 2>/dev/null; answer=$?; printf "%s %s\n" "$answer" "$(cat Xbc)"' \
        sh "$work/rename-interactive"
subject 'symlink-target mode rejected explicitly' rename \
        '"$TOOL" -s old new link >/dev/null 2>&1; [ "$?" -ne 0 ]'

group cal
compare 'single Gregorian leap month' cal \
        'LC_ALL=C TZ=UTC "$TOOL" 2 2000'
compare 'non-leap Gregorian century' cal \
        'LC_ALL=C TZ=UTC "$TOOL" 2 1900'
compare 'British 1752 reform gap' cal \
        'LC_ALL=C TZ=UTC "$TOOL" 9 1752'
compare 'proleptic Gregorian reform' cal \
        'LC_ALL=C TZ=UTC "$TOOL" --reform=gregorian 9 1752'
compare 'Monday first day' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -m 2 2024'
compare 'Julian day-of-year cells' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -j 9 1752'
compare 'three months across year boundary' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -3 1 2024'
compare 'forward multi-month range' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -n 4 11 2024'
compare 'spanning multi-month range' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -S -n 4 11 2024'
compare 'spanning whole-year layout' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -S -y 2024'
compare 'weekday option order' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -m -s 2 2024; LC_ALL=C TZ=UTC "$TOOL" -s -m 2 2024'
compare 'whole leap year layout' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -y 2024'
compare 'next twelve months layout' cal \
        'LC_ALL=C TZ=UTC "$TOOL" -Y 11 2023'
compare 'generated Gregorian and reform edges' cal \
        'for year in 1 4 100 400 1699 1700 1752 1800 1900 2000 2100 2400; do for month in 1 2 3 6 9 12; do LC_ALL=C TZ=UTC "$TOOL" "$month" "$year" || exit; done; done'
subject 'unsupported week numbers reject' cal \
        '! "$TOOL" -w 2 2024'
subject 'unsupported vertical layout rejects' cal \
        '! "$TOOL" -v 2 2024'
subject 'unsupported color mutation rejects' cal \
        '! "$TOOL" --color=always 2 2024'

group pipesz
compare 'get stdin pipe capacity and unread bytes' pipesz \
        'printf x | "$TOOL" -g -i'
compare 'verbose get header' pipesz \
        'printf x | "$TOOL" -g -i -v'
compare 'numeric descriptor selection' pipesz \
        'printf x | sh -c '\''"$1" -g -n 0'\'' sh "$TOOL"'
compare 'set survives command handoff' pipesz \
        'printf x | "$TOOL" -s 4096 -i -- "$TOOL" -g -i'
compare 'named fifo selection' pipesz \
        'rm -f "$1"; mkfifo "$1" || exit; exec 9<>"$1"; "$TOOL" -g -f "$1"; answer=$?; exec 9>&-; rm -f "$1"; exit "$answer"' \
        sh "$work/pipesz-fifo"
compare 'checked non-pipe failure' pipesz \
        '"$TOOL" -g -o -c'
compare 'get and set conflict' pipesz \
        '"$TOOL" -g -s 4096 -i'
subject 'unchecked non-pipe failure remains advisory' pipesz \
        '"$TOOL" -g -o >/dev/null 2>&1'

group system-v-ipc
subject 'owned create list and remove cycle' ipcmk \
        'made=$("$TOOL" -M 4096 -Q -S 3 -p 0640) || exit; m=$(printf "%s\n" "$made" | awk '\''/Shared memory id:/ {print $NF}'\''); q=$(printf "%s\n" "$made" | awk '\''/Message queue id:/ {print $NF}'\''); s=$(printf "%s\n" "$made" | awk '\''/Semaphore id:/ {print $NF}'\''); cleanup() { [ -z "${q:-}" ] || ipcrm -q "$q" 2>/dev/null || :; [ -z "${m:-}" ] || ipcrm -m "$m" 2>/dev/null || :; [ -z "${s:-}" ] || ipcrm -s "$s" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; [ -n "$m" ] && [ -n "$q" ] && [ -n "$s" ] || exit 1; awk -v id="$q" '\''NR > 1 && $2 == id && $3 == 640 {found=1} END {exit !found}'\'' /proc/sysvipc/msg && awk -v id="$m" '\''NR > 1 && $2 == id && $3 == 640 && $4 == 4096 {found=1} END {exit !found}'\'' /proc/sysvipc/shm && awk -v id="$s" '\''NR > 1 && $2 == id && $3 == 640 && $4 == 3 {found=1} END {exit !found}'\'' /proc/sysvipc/sem || exit 1; "$1/ipcrm" -q "$q" -m "$m" -s "$s" || exit; awk -v id="$q" '\''NR > 1 && $2 == id {exit 1}'\'' /proc/sysvipc/msg && awk -v id="$m" '\''NR > 1 && $2 == id {exit 1}'\'' /proc/sysvipc/shm && awk -v id="$s" '\''NR > 1 && $2 == id {exit 1}'\'' /proc/sysvipc/sem || exit; q= m= s=' \
        sh "$work/bin"
subject 'controlled snapshot exact structured modes' lsipc \
        'q=$(ipcmk -Q -p 0640 | awk '\''{print $NF}'\'') || exit; m=$(ipcmk -M 4096 -p 0640 | awk '\''{print $NF}'\'') || exit; s=$(ipcmk -S 2 -p 0640 | awk '\''{print $NF}'\'') || exit; cleanup() { ipcrm -q "$q" 2>/dev/null || :; ipcrm -m "$m" 2>/dev/null || :; ipcrm -s "$s" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; reference=$(command -v lsipc); for args in "-q -J" "-q -r" "-q -n" "-m -J" "-m -r" "-m -n" "-s -J" "-s -r" "-s -n"; do TZ=UTC0 "$reference" $args > "$1/want" || exit; TZ=UTC0 "$TOOL" $args > "$1/got" || exit; cmp -s "$1/want" "$1/got" || exit; done' \
        sh "$work"
subject 'controlled list content and time ordering' lsipc \
        'q=$(ipcmk -Q -p 0640 | awk '\''{print $NF}'\'') || exit; m=$(ipcmk -M 4096 -p 0640 | awk '\''{print $NF}'\'') || exit; s=$(ipcmk -S 2 -p 0640 | awk '\''{print $NF}'\'') || exit; cleanup() { ipcrm -q "$q" 2>/dev/null || :; ipcrm -m "$m" 2>/dev/null || :; ipcrm -s "$s" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; reference=$(command -v lsipc); for args in "-q -l -i $q" "-q -l -i $q -c" "-q -l -i $q -t" "-m -l -i $m" "-m -l -i $m -c" "-m -l -i $m -t" "-s -l -i $s" "-s -l -i $s -c" "-s -l -i $s -t"; do TZ=UTC0 "$reference" $args > "$1/want" || exit; TZ=UTC0 "$TOOL" $args > "$1/got" || exit; cmp -s "$1/want" "$1/got" || exit; done' \
        sh "$work"
subject 'classic ipcs tables preserve values' ipcs \
        'q=$(ipcmk -Q -p 0640 | awk '\''{print $NF}'\'') || exit; m=$(ipcmk -M 4096 -p 0640 | awk '\''{print $NF}'\'') || exit; s=$(ipcmk -S 2 -p 0640 | awk '\''{print $NF}'\'') || exit; cleanup() { ipcrm -q "$q" 2>/dev/null || :; ipcrm -m "$m" 2>/dev/null || :; ipcrm -s "$s" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; reference=$(command -v ipcs); for type in q m s; do "$reference" -$type | sed '\''s/[[:space:]]*$//'\'' > "$1/want" || exit; "$TOOL" -$type | sed '\''s/[[:space:]]*$//'\'' > "$1/got" || exit; cmp -s "$1/want" "$1/got" || exit; done' \
        sh "$work"
subject 'key removal only touches owned queue' ipcrm \
        'q=$(ipcmk -Q -p 0600 | awk '\''{print $NF}'\'') || exit; cleanup() { [ -z "${q:-}" ] || ipcrm -q "$q" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; key=$(lsipc -q -r -o KEY,ID | awk -v id="$q" '\''$2 == id {print $1}'\''); [ -n "$key" ] || exit 1; "$TOOL" -Q "$key" || exit; awk -v id="$q" '\''NR > 1 && $2 == id {exit 1}'\'' /proc/sysvipc/msg; q=' \
        sh
subject '32-bit id and key aliases cannot remove owned queue' ipcrm \
        'q=$(ipcmk -Q -p 0600 | awk '\''{print $NF}'\'') || exit; cleanup() { ipcrm -q "$q" 2>/dev/null || :; }; trap cleanup EXIT INT TERM; key=$(awk -v id="$q" '\''NR > 1 && $2 == id {print $1}'\'' /proc/sysvipc/msg); [ -n "$key" ] || exit 1; [ "$key" -ge 0 ] || key=$((key + 4294967296)); bad_id=$((q + 4294967296)); bad_key=$((key + 4294967296)); "$TOOL" -q "$bad_id" >/dev/null 2>&1 && exit 1; "$TOOL" -Q "$bad_key" >/dev/null 2>&1 && exit 1; awk -v id="$q" '\''NR > 1 && $2 == id {found=1} END {exit !found}'\'' /proc/sysvipc/msg'
subject 'semaphore count is bounded before the syscall' ipcmk \
        'before=$(wc -l < /proc/sysvipc/sem); "$TOOL" -S 4294967297 >/dev/null 2>&1 && exit 1; after=$(wc -l < /proc/sysvipc/sem); [ "$before" = "$after" ]'
subject 'combined creation failure rolls back earlier object' ipcmk \
        'before=$(wc -l < /proc/sysvipc/shm); "$TOOL" -M 4096 -S 2147483647 >/dev/null 2>&1 && exit 1; after=$(wc -l < /proc/sysvipc/shm); [ "$before" = "$after" ]'
subject 'POSIX creation rejected explicitly' ipcmk \
        '"$TOOL" -m 4096 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'bulk removal rejected explicitly' ipcrm \
        '"$TOOL" -a >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'unsupported global summary rejected explicitly' lsipc \
        '"$TOOL" -g >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'default global summary rejected explicitly' lsipc \
        '"$TOOL" >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'list and JSON modes rejected together' lsipc \
        '"$TOOL" -q -l -J >/dev/null 2>&1; [ "$?" -ne 0 ]'
compare 'custom columns and time rejected together' lsipc \
        '"$TOOL" -q -o KEY -t'
subject 'classic detail modes rejected explicitly' ipcs \
        '"$TOOL" -q -t >/dev/null 2>&1; [ "$?" -ne 0 ]'

group lsblk
compare 'default device tree' lsblk '"$TOOL"'
compare 'flat device list' lsblk '"$TOOL" -l'
compare 'raw device tree order' lsblk '"$TOOL" -r'
compare 'byte sizes' lsblk '"$TOOL" -b'
compare 'top-level devices only' lsblk '"$TOOL" -d'
compare 'absolute device paths' lsblk '"$TOOL" -p'
compare 'filesystem inventory' lsblk '"$TOOL" -f'
compare 'device permissions' lsblk '"$TOOL" -m'
compare 'queue topology' lsblk '"$TOOL" -t'
compare 'SCSI inventory' lsblk '"$TOOL" -S'
compare 'selected device identity metadata' lsblk \
        '"$TOOL" -d -n -r -o KNAME,SERIAL,VENDOR,MODEL,REV,HCTL,TRAN'
subject 'all mode matches kernel block census' lsblk \
        'want=$(find /sys/class/block -mindepth 1 -maxdepth 1 | wc -l); got=$("$TOOL" -a -l -n -r -o KNAME | wc -l); [ "$got" = "$want" ]'
subject 'byte sizes follow kernel sectors' lsblk \
        '"$TOOL" -a -l -b -n -r -o KNAME,SIZE | python3 -c '\''import sys
for line in sys.stdin:
 name,size=line.split(); expected=int(open("/sys/class/block/"+name+"/size").read())*512; assert int(size)==expected'\'''
subject 'JSON is parseable and booleans are typed' lsblk \
        '"$TOOL" -J -a -o NAME,KNAME,SIZE,RO,RM,TYPE,MOUNTPOINTS | python3 -c '\''import json,sys
rows=json.load(sys.stdin)["blockdevices"]
def walk(items):
 for item in items:
  assert isinstance(item["ro"],bool) and isinstance(item["rm"],bool)
  assert isinstance(item["mountpoints"],list)
  yield item
  yield from walk(item.get("children",[]))
assert list(walk(rows))'\'''
subject 'named device selection is bounded' lsblk \
        'dev=$("$TOOL" -d -n -r -o PATH | sed -n 1p); [ -n "$dev" ] && [ "$("$TOOL" -d -n -r -o PATH "$dev")" = "$dev" ]'
subject 'discard columns rejected explicitly' lsblk \
        '"$TOOL" -D >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'zoned columns rejected explicitly' lsblk \
        '"$TOOL" -z >/dev/null 2>&1; [ "$?" -ne 0 ]'
if sudo -n unshare -m true >/dev/null 2>&1; then
        compare 'multi-mount aligned continuation rows' lsblk \
                'sudo -n unshare -m sh -c '\''mount --make-rprivate /; mkdir -p "$2/a" "$2/b"; mount --bind / "$2/a"; mount --bind / "$2/b"; LC_ALL=C "$1" -n -o NAME,MOUNTPOINTS'\'' sh "$TOOL" "$0"' \
                "$work"
        compare 'multi-mount raw newline escaping' lsblk \
                'sudo -n unshare -m sh -c '\''mount --make-rprivate /; mkdir -p "$2/a" "$2/b"; mount --bind / "$2/a"; mount --bind / "$2/b"; LC_ALL=C "$1" -n -r -o KNAME,MOUNTPOINTS'\'' sh "$TOOL" "$0"' \
                "$work"
fi

group lsclocks
compare 'static clock metadata raw' lsclocks \
        '"$TOOL" --no-discover-dynamic --no-discover-rtc -r -o ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET'
compare 'static clock metadata JSON' lsclocks \
        '"$TOOL" --no-discover-dynamic --no-discover-rtc -J -o ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET'
compare 'static clock metadata without headings' lsclocks \
        '"$TOOL" --no-discover-dynamic --no-discover-rtc -n -o ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET'
compare 'environment selects columns' lsclocks \
        'LSCLOCKS_COLUMNS=ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET "$TOOL" --no-discover-dynamic --no-discover-rtc -r'
compare 'unknown clock rejects' lsclocks \
        '"$TOOL" -t moonwater-not-a-clock'
compare 'missing explicit dynamic clock rejects' lsclocks \
        '"$TOOL" -d /dev/moonwater-not-a-clock'
subject 'symbolic realtime emits seconds and nanoseconds' lsclocks \
        '"$TOOL" -t realtime | grep -Eq "^[0-9]{10}\\.[0-9]{9}$"'
subject 'all static JSON fields are typed' lsclocks \
        '"$TOOL" --no-discover-dynamic --no-discover-rtc -J --output-all | python3 -c '\''import json,sys
rows=json.load(sys.stdin)["clocks"]
assert rows and {"type","id","clock","name","time","iso_time","resol","resol_raw","rel_time","ns_offset"} == set(rows[0])
assert all(isinstance(r["id"],int) and isinstance(r["name"],str) for r in rows)'\'''
subject 'CPU clock follows an extant process' lsclocks \
        '"$TOOL" --no-discover-dynamic --no-discover-rtc -c $$ -n -r -o TYPE,NAME,TIME | grep -Eq "^cpu [0-9]+ [0-9]+\\.[0-9]{9}$"'

group lscpu
compare 'custom parsable topology' lscpu \
        '"$TOOL" -p=CPU,CORE'
compare 'extended core topology' lscpu \
        '"$TOOL" -e=CPU,CORE,SOCKET,NODE,ONLINE'
compare 'extended JSON types' lscpu \
        '"$TOOL" -J -e=CPU,CORE,ONLINE'
compare 'cache inventory' lscpu '"$TOOL" -C'
compare 'raw byte cache inventory' lscpu '"$TOOL" -C -r -B'
compare 'cache JSON types' lscpu '"$TOOL" -J -C'
subject 'summary core fields agree with sysfs' lscpu \
        'cpus=$(cat /sys/devices/system/cpu/present); online=$(cat /sys/devices/system/cpu/online); "$TOOL" > "$0/out" && grep -Eq "^Architecture:[[:space:]]+$(uname -m)$" "$0/out" && grep -Eq "^On-line CPU.s. list:[[:space:]]+$online$" "$0/out" && grep -q "^Flags:" "$0/out" && grep -q "^NUMA node(s):" "$0/out"' \
        "$work"
subject 'default parse topology invariants' lscpu \
        '"$TOOL" -p | awk -F, '\''!/^#/ { if ($1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ || $4 !~ /^[0-9]+$/ || seen[$1]++) bad=1; rows++ } END { exit bad || !rows }'\'''
subject 'online filter follows kernel set' lscpu \
        'want=$(cat /sys/devices/system/cpu/online); got=$("$TOOL" -b -e=CPU -r | sed 1d | paste -sd, -); case $want in *-*) first=${want%%-*}; last=${want##*-}; expected=$(seq "$first" "$last" | paste -sd, -);; *) expected=$want;; esac; [ "$got" = "$expected" ]'
subject 'summary JSON is structured' lscpu \
        '"$TOOL" -J | grep -q '\''"field": "Architecture:"'\'''
subject 'physical identifiers rejected' lscpu \
        '"$TOOL" -y -e >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'configured column rejected' lscpu \
        '"$TOOL" -e=CPU,CONFIGURED >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'address column rejected' lscpu \
        '"$TOOL" -p=CPU,ADDRESS >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'sysroot rejected' lscpu \
        '"$TOOL" --sysroot / >/dev/null 2>&1; [ "$?" -ne 0 ]'

group lsmem
compare 'default aggregated ranges and summary' lsmem '"$TOOL"'
compare 'individual memory blocks' lsmem '"$TOOL" -a'
compare 'aggregated JSON types' lsmem '"$TOOL" -J'
compare 'raw suppresses summary' lsmem '"$TOOL" -r'
compare 'byte sizes and summary' lsmem '"$TOOL" -b'
compare 'projected topology fields' lsmem \
        '"$TOOL" -n -o RANGE,SIZE,STATE,REMOVABLE,BLOCK,NODE,ZONES'
compare 'additive projection splits ranges' lsmem \
        '"$TOOL" -o +NODE,ZONES'
compare 'explicit zone split' lsmem '"$TOOL" -S ZONES'
compare 'summary only' lsmem '"$TOOL" --summary=only'
compare 'summary disabled' lsmem '"$TOOL" --summary=never'
compare 'JSON summary disabled' lsmem '"$TOOL" -J --summary=never'
compare 'JSON with requested trailing summary' lsmem \
        '"$TOOL" -J --summary=always'
compare 'JSON summary-only rejection' lsmem \
        '"$TOOL" -J --summary=only'
compare 'bare summary-only mode' lsmem '"$TOOL" --summary'
subject 'aggregated ranges exactly cover reported bytes' lsmem \
        '"$TOOL" -b -n --summary=never -o RANGE,SIZE | python3 -c '\''import sys
last=-1
seen=False
for line in sys.stdin:
 r,size=line.split(); lo,hi=(int(x,16) for x in r.split("-")); assert lo>last and hi-lo+1==int(size); last=hi; seen=True
assert seen'\'''
subject 'all mode matches kernel memory-block census' lsmem \
        'want=$(find /sys/devices/system/memory -maxdepth 1 -type d -name "memory[0-9]*" | wc -l); got=$("$TOOL" -a -n -r --summary=never -o BLOCK | wc -l); [ "$got" = "$want" ]'
subject 'JSON is typed and parseable' lsmem \
        '"$TOOL" -J -a -o RANGE,SIZE,STATE,REMOVABLE,BLOCK,NODE,ZONES | python3 -c '\''import json,sys; rows=json.load(sys.stdin)["memory"]; assert rows and all(isinstance(x["removable"],bool) and (x["node"] is None or isinstance(x["node"],int)) for x in rows)'\'''
subject 'configured metadata rejected' lsmem \
        '"$TOOL" -o CONFIGURED >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'pairs rejected explicitly' lsmem \
        '"$TOOL" -P >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'sysroot rejected explicitly' lsmem \
        '"$TOOL" --sysroot / >/dev/null 2>&1; [ "$?" -ne 0 ]'

group uuidgen
compare 'RFC DNS MD5 name UUID' uuidgen \
        '"$TOOL" -n @dns -N x -m'
compare 'RFC DNS SHA1 name UUID' uuidgen \
        '"$TOOL" -n @dns -N x -s'
compare 'hexadecimal SHA1 name' uuidgen \
        '"$TOOL" -n @dns -N 616263 -x -s'
compare 'zero count' uuidgen '"$TOOL" -r -C 0'
subject 'random version, variant and uniqueness' uuidgen \
        '"$TOOL" -r -C 256 | awk '\''length($0) != 36 || substr($0,15,1) != "4" || index("89ab",substr($0,20,1)) == 0 || seen[$0]++ { bad=1 } END { exit bad || NR != 256 }'\'''
subject 'time version, variant and uniqueness' uuidgen \
        '"$TOOL" -t -C 64 | awk '\''length($0) != 36 || substr($0,15,1) != "1" || index("89ab",substr($0,20,1)) == 0 || seen[$0]++ { bad=1 } END { exit bad || NR != 64 }'\'''
subject 'time-v6 version and variant' uuidgen \
        '"$TOOL" -6 -C 64 | awk '\''length($0) != 36 || substr($0,15,1) != "6" || index("89ab",substr($0,20,1)) == 0 || seen[$0]++ { bad=1 } END { exit bad || NR != 64 }'\'''
subject 'time-v7 version and variant' uuidgen \
        '"$TOOL" -7 -C 64 | awk '\''length($0) != 36 || substr($0,15,1) != "7" || index("89ab",substr($0,20,1)) == 0 || seen[$0]++ { bad=1 } END { exit bad || NR != 64 }'\'''
subject 'name count rejected' uuidgen \
        '"$TOOL" -n @dns -N x -m -C 2 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'time-safe rejected explicitly' uuidgen \
        '"$TOOL" --time-safe >/dev/null 2>&1; [ "$?" -ne 0 ]'

group uuidparse
compare 'default random row' uuidparse \
        '"$TOOL" 550e8400-e29b-41d4-a716-446655440000'
compare 'no headings fixed columns' uuidparse \
        '"$TOOL" -n 550e8400-e29b-41d4-a716-446655440000'
compare 'raw type and Gregorian time' uuidparse \
        'TZ=UTC0 "$TOOL" -n -r 00000000-0000-0000-8000-000000000001 00000000-0000-1000-8000-000000000001 00000000-0000-2000-8000-000000000001 00000000-0000-6000-8000-000000000001 00000000-0000-7000-8000-000000000001 00000000-0000-f000-8000-000000000001'
compare 'known RFC time UUID' uuidparse \
        'TZ=UTC0 "$TOOL" -n -r 6ba7b810-9dad-11d1-80b4-00c04fd430c8'
compare 'reordered output columns' uuidparse \
        '"$TOOL" -n -o TYPE,UUID 550e8400-e29b-41d4-a716-446655440000'
compare 'nil max and invalid UUIDs' uuidparse \
        '"$TOOL" 00000000-0000-0000-0000-000000000000 ffffffff-ffff-ffff-ffff-ffffffffffff bad'
compare 'JSON valid and invalid rows' uuidparse \
        '"$TOOL" -J bad 550e8400-e29b-41d4-a716-446655440000'
compare 'JSON projection' uuidparse \
        '"$TOOL" -J -o TYPE,UUID 550e8400-e29b-41d4-a716-446655440000'
subject 'unknown column rejected' uuidparse \
        '"$TOOL" -o IMPOSSIBLE x >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'raw JSON combination rejected' uuidparse \
        '"$TOOL" -r -J x >/dev/null 2>&1; [ "$?" -ne 0 ]'

group mcookie
subject 'cookie width alphabet and uniqueness' mcookie \
        'for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32; do "$TOOL"; done | awk '\''length($0) != 32 || $0 !~ /^[0123456789abcdef]+$/ || seen[$0]++ { bad=1 } END { exit bad || NR != 32 }'\'''
subject 'descriptor input and verbose accounting' mcookie \
        'printf abc | "$TOOL" -f /dev/fd/0 -m 3 -v > "$0/cookie" 2> "$0/error" && [ "$(wc -c < "$0/cookie")" = 33 ] && grep -qx "Got 3 bytes from /dev/fd/0" "$0/error" && grep -qx "Got 128 bytes from getrandom() function" "$0/error"' \
        "$work"
subject 'missing file is nonfatal' mcookie \
        '"$TOOL" -f /definitely/absent > "$0/cookie" 2> "$0/error"; [ "$?" = 0 ] && [ "$(wc -c < "$0/cookie")" = 33 ] && [ -s "$0/error" ]' \
        "$work"
subject 'invalid maximum rejected' mcookie \
        '"$TOOL" -m impossible >/dev/null 2>&1; [ "$?" -ne 0 ]'

group getopt
compare 'short required optional and permutation' getopt \
        '"$TOOL" -o "ab:c::" -- x -a -b y z -cfoo'
compare 'positive ordering stops at operand' getopt \
        '"$TOOL" -o "+ab:" -- x -a -b y'
compare 'return-in-order output' getopt \
        '"$TOOL" -o "-ab:" -- x -a z -b y'
compare 'long required and optional arguments' getopt \
        '"$TOOL" -o a -l "alpha,beta:,charlie::" -- --alpha --beta value x --charlie=maybe'
compare 'repeated long option lists' getopt \
        '"$TOOL" -o a -l foo,bar -l baz: -- --foo --baz value x'
compare 'alternative long spelling and short priority' getopt \
        '"$TOOL" -a -o a -l a,alpha -- -alpha -a'
compare 'sh embedded quote protocol' getopt \
        '"$TOOL" -s sh -o a: -- -a "a'\''b" z'
compare 'csh blank protocol' getopt \
        '"$TOOL" -s csh -o a: -- -a "x y" z'
compare 'unquoted compatibility output' getopt \
        '"$TOOL" -u -o a: -- -a "x y" z'
compare 'legacy invocation is unquoted' getopt \
        '"$TOOL" a: -a "x y" z'
compare 'quiet target diagnostic' getopt \
        '"$TOOL" -q -o a -- -x'
compare 'quiet output still parses' getopt \
        '"$TOOL" -Q -o a: -- -a value x'
compare 'test mode status' getopt '"$TOOL" -T'
compare_full 'custom diagnostic name' getopt \
        '"$TOOL" -n parser -o a -l foo,foobar -- --fo'

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

group fallocate
compare 'allocate new file' fallocate \
        'rm -f "$0/f"; "$TOOL" -l 8KiB "$0/f"; stat -c "%s %b" "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'offset extends file' fallocate \
        'rm -f "$0/f"; "$TOOL" -o 4KiB -l 8KiB "$0/f"; stat -c "%s" "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'keep apparent size' fallocate \
        'rm -f "$0/f"; : > "$0/f"; "$TOOL" -n -l 8KiB "$0/f"; stat -c "%s %b" "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'zero range' fallocate \
        'rm -f "$0/f"; printf abcdefgh > "$0/f"; "$TOOL" -z -o 2 -l 3 "$0/f"; od -An -tx1 "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'punch hole keeps size' fallocate \
        'rm -f "$0/f"; dd if=/dev/zero of="$0/f" bs=4096 count=3 status=none; "$TOOL" -p -o 4096 -l 4096 "$0/f"; stat -c "%s %b" "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'verbose allocation' fallocate \
        'rm -f "$0/f"; "$TOOL" -v -l 8KiB "$0/f"; rm -f "$0/f"' \
        "$work"
compare 'missing length' fallocate '"$TOOL" "$0"' "$work/data"
compare 'zero length' fallocate '"$TOOL" -l 0 "$0"' "$work/data"
compare 'exclusive operations' fallocate \
        '"$TOOL" -c -i -l 1 "$0"' "$work/data"
compare 'too many files' fallocate \
        '"$TOOL" -l 1 "$0" "$0"' "$work/data"

group copyfilerange
subject 'one explicit range' copyfilerange \
        'printf abcdefgh > "$0/in"; printf 00000000 > "$0/out"; "$TOOL" "$0/in" "$0/out" 2:1:3; test "$(cat "$0/out")" = 0cde0000' \
        "$work"
subject 'continued offsets' copyfilerange \
        'printf abcdefgh > "$0/in"; : > "$0/out"; "$TOOL" "$0/in" "$0/out" 0:0:2 ::2; test "$(cat "$0/out")" = abcd' \
        "$work"
subject 'zero means remainder' copyfilerange \
        'printf abcdefgh > "$0/in"; : > "$0/out"; "$TOOL" "$0/in" "$0/out" 3:0:0; test "$(cat "$0/out")" = defgh' \
        "$work"
subject 'invalid range rejected' copyfilerange \
        'printf abc > "$0/in"; ! "$TOOL" "$0/in" "$0/out" invalid' \
        "$work"
subject 'source boundary rejected' copyfilerange \
        'printf abc > "$0/in"; ! "$TOOL" "$0/in" "$0/out" 4:0:1' \
        "$work"

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

group fincore
fincore_empty="$work/fincore-empty"
fincore_cached="$work/fincore-cached"
fincore_sparse="$work/fincore-sparse"
: > "$fincore_empty"
dd if=/dev/zero of="$fincore_cached" bs=4096 count=16 status=none
truncate -s 16777216 "$fincore_sparse"
cat "$fincore_cached" >/dev/null
cat > "$work/fincore-enosys.py" <<'PY'
import ctypes, os, sys

class Filter(ctypes.Structure):
    _fields_ = [('code', ctypes.c_ushort), ('jt', ctypes.c_ubyte),
                ('jf', ctypes.c_ubyte), ('value', ctypes.c_uint)]
class Program(ctypes.Structure):
    _fields_ = [('length', ctypes.c_ushort),
                ('filters', ctypes.POINTER(Filter))]

rules = (Filter * 4)(
    Filter(0x20, 0, 0, 0),             # load seccomp_data.nr
    Filter(0x15, 0, 1, 451),           # cachestat on x86/ARM64
    Filter(0x06, 0, 0, 0x00050000 | 38), # SECCOMP_RET_ERRNO | ENOSYS
    Filter(0x06, 0, 0, 0x7fff0000),    # SECCOMP_RET_ALLOW
)
libc = ctypes.CDLL(None, use_errno=True)
if libc.prctl(38, 1, 0, 0, 0) or libc.prctl(22, 2,
                                            ctypes.byref(Program(4, rules))):
    raise OSError(ctypes.get_errno(), 'prctl seccomp')
os.execv(sys.argv[1], [sys.argv[1], *sys.argv[2:]])
PY

compare 'empty and cached default table' fincore \
        '"$TOOL" "$1" "$2"' sh "$fincore_empty" "$fincore_cached"
compare 'byte counts' fincore \
        '"$TOOL" -b "$1" "$2"' sh "$fincore_empty" "$fincore_cached"
compare 'raw no-heading projection' fincore \
        '"$TOOL" -n -r -o FILE,RES,PAGES,SIZE "$1" "$2"' \
        sh "$fincore_empty" "$fincore_cached"
compare 'human JSON field types' fincore \
        '"$TOOL" -J "$1" "$2"' sh "$fincore_empty" "$fincore_cached"
compare 'byte JSON field types' fincore \
        '"$TOOL" -J -b "$1" "$2"' sh "$fincore_empty" "$fincore_cached"
compare 'ENOSYS mincore fallback' fincore \
        'python3 "$1" "$TOOL" -b -n -r -o RES,PAGES,SIZE "$2"' \
        sh "$work/fincore-enosys.py" "$fincore_cached"
compare_full 'missing file continues with valid rows' fincore \
        '"$TOOL" "$1" "$2/missing" "$3"' \
        sh "$fincore_cached" "$work" "$fincore_empty"

subject 'cached page accounting invariant' fincore \
        'page=$(getconf PAGESIZE); set -- $("$TOOL" -b -n -r -o RES,PAGES,SIZE "$1"); expected=$(($2 * page)); [ "$1" = "$expected" ] && [ "$1" -le "$3" ] && [ "$2" -gt 0 ]' \
        sh "$fincore_cached"
subject 'sparse page accounting invariant' fincore \
        'page=$(getconf PAGESIZE); set -- $("$TOOL" -b -n -r -o RES,PAGES,SIZE "$1"); expected=$(($2 * page)); [ "$1" = "$expected" ] && [ "$1" -le "$3" ] && [ "$3" = 16777216 ]' \
        sh "$fincore_sparse"
subject 'additive core columns accepted' fincore \
        '"$TOOL" -o +PAGES "$1" >/dev/null' sh "$fincore_cached"
subject 'extended cachestat columns rejected' fincore \
        '! "$TOOL" -o DIRTY "$1" >/dev/null 2>&1' sh "$fincore_cached"
subject 'recursive mode rejected' fincore \
        '! "$TOOL" -R "$1" >/dev/null 2>&1' sh "$work"
subject 'forced cachestat mode rejected' fincore \
        '! "$TOOL" -C "$1" >/dev/null 2>&1' sh "$fincore_cached"

group namei
namei_tree="$work/namei-tree"
mkdir -p "$namei_tree/a/b" "$namei_tree/other"
printf data > "$namei_tree/a/b/file"
printf other > "$namei_tree/other/file"
ln -s b/file "$namei_tree/a/relative"
ln -s "$namei_tree/other" "$namei_tree/a/absolute"
ln -s ../a/b "$namei_tree/other/up"
ln -s self "$namei_tree/a/self"

compare 'default relative symlink expansion' namei \
        '"$TOOL" "$1/a/relative"' sh "$namei_tree"
compare 'multiple paths and missing component' namei \
        '"$TOOL" "$1/a/b/file" "$1/a/missing/tail" "$1/other/file"' \
        sh "$namei_tree"
mkdir -p "$namei_tree/locked/child"
chmod 000 "$namei_tree/locked"
compare 'inaccessible component stops cleanly' namei \
        '"$TOOL" "$1/locked/child"' sh "$namei_tree"
chmod 700 "$namei_tree/locked"
compare 'mode component listing' namei \
        '"$TOOL" -m "$1/a/relative"' sh "$namei_tree"
compare 'owner component listing' namei \
        '"$TOOL" -o "$1/a/relative"' sh "$namei_tree"
compare 'long vertical component listing' namei \
        '"$TOOL" -l "$1/a/relative"' sh "$namei_tree"
compare 'explicit vertical indentation' namei \
        '"$TOOL" -v "$1/a/relative"' sh "$namei_tree"
compare 'symlink expansion disabled' namei \
        '"$TOOL" -n "$1/a/relative"' sh "$namei_tree"
compare 'absolute symlink target and suffix' namei \
        '"$TOOL" "$1/a/absolute/file"' sh "$namei_tree"
compare 'mountpoint markers' namei \
        '"$TOOL" -x / /tmp "$1"' sh "$namei_tree"
compare 'repeated slash dot and dotdot components' namei \
        '"$TOOL" "//tmp///$(basename "$1")/a/./b/../b/file"' \
        sh "$namei_tree"
compare 'file trailing slash status' namei \
        '"$TOOL" "$1/a/b/file/"' sh "$namei_tree"
compare_full 'symlink cycle depth guard' namei \
        '"$TOOL" "$1/a/self"' sh "$namei_tree"

python3 - "$namei_tree/generated" <<'PY'
import os, sys
root = sys.argv[1]
os.makedirs(root)
at = root
for i in range(40):
    name = 'd%02d' % i
    at = os.path.join(at, name)
    os.mkdir(at)
open(os.path.join(at, 'leaf'), 'w').close()
PY
subject 'generated deep walk row invariant' namei \
        'path="$1/generated"; i=0; while [ "$i" -lt 40 ]; do piece=$(printf "d%02d" "$i"); path="$path/$piece"; i=$((i+1)); done; out="$1/out"; "$TOOL" "$path/leaf" > "$out"; [ "$(wc -l < "$out")" -ge 43 ] && tail -1 "$out" | grep -q -- "- leaf$"' \
        sh "$namei_tree"

subject 'symlink replacement race is bounded' namei \
        'ln -s a "$1/race"; (i=0; while [ "$i" -lt 200 ]; do ln -sfn a "$1/race"; ln -sfn other "$1/race"; i=$((i+1)); done) & swap=$!; i=0; while [ "$i" -lt 50 ]; do timeout 1 "$TOOL" "$1/race/b/file" >/dev/null 2>&1; status=$?; [ "$status" = 0 ] || [ "$status" = 1 ] || { kill "$swap" 2>/dev/null; exit 1; }; i=$((i+1)); done; wait "$swap"' \
        sh "$namei_tree"

group dmesg
dmesg_log="$work/dmesg-log"
dmesg_kmsg="$work/dmesg-kmsg"
dmesg_unsafe="$work/dmesg-unsafe"
printf '<6>[    1.250000] hello world\n<3>[    3.500000] bad thing\n<13>[    4.000000] user notice\n' \
        > "$dmesg_log"
printf '6,10,1250000,-;hello world\n\0' > "$dmesg_kmsg"
printf '3,11,3500000,-;bad thing\n\0' >> "$dmesg_kmsg"
printf '13,12,4000000,-;user notice\n\0' >> "$dmesg_kmsg"
printf '<6>[    1.250000] a\001b\tb\\c\n' > "$dmesg_unsafe"

compare 'legacy snapshot default' dmesg '"$TOOL" -F "$0"' "$dmesg_log"
compare 'legacy snapshot raw' dmesg '"$TOOL" -r -F "$0"' "$dmesg_log"
compare 'decoded facility and level' dmesg '"$TOOL" -x -F "$0"' "$dmesg_log"
compare 'timestamp suppression' dmesg '"$TOOL" -t -F "$0"' "$dmesg_log"
compare 'level filter' dmesg '"$TOOL" -l err -F "$0"' "$dmesg_log"
compare 'level range filter' dmesg '"$TOOL" -l err+ -F "$0"' "$dmesg_log"
compare 'facility filter' dmesg '"$TOOL" -f user -F "$0"' "$dmesg_log"
compare 'kernel facility shortcut' dmesg '"$TOOL" -k -F "$0"' "$dmesg_log"
compare 'userspace facility shortcut' dmesg '"$TOOL" -u -F "$0"' "$dmesg_log"
compare 'relative and delta time' dmesg 'TZ=UTC0 "$TOOL" -e -F "$0"' "$dmesg_log"
compare 'human time without pager' dmesg \
        'TZ=UTC0 "$TOOL" -H -P -F "$0"' "$dmesg_log"
compare 'ctime from boot clock' dmesg 'TZ=UTC0 "$TOOL" -T -F "$0"' "$dmesg_log"
compare 'raw clock with delta' dmesg '"$TOOL" -d -F "$0"' "$dmesg_log"
compare 'JSON records and types' dmesg '"$TOOL" -J -F "$0"' "$dmesg_log"
compare 'kmsg record framing' dmesg '"$TOOL" -K "$0"' "$dmesg_kmsg"
compare 'safe control escaping' dmesg '"$TOOL" -F "$0"' "$dmesg_unsafe"
compare 'explicit unsafe bytes' dmesg '"$TOOL" --noescape -F "$0"' "$dmesg_unsafe"
compare 'no pager and no color' dmesg \
        'TERM=dumb "$TOOL" -P --color=never -F "$0"' "$dmesg_log"
compare_full 'missing fixture file' dmesg \
        '"$TOOL" -F /definitely/absent/dmesg-log'
subject 'live permission result is loud' dmesg \
        '"$TOOL" >"$0/out" 2>"$0/error" || [ -s "$0/error" ]' "$work"
subject 'raw decode conflict rejected' dmesg \
        '"$TOOL" -r -x -F "$0" >/dev/null 2>&1; [ "$?" -ne 0 ]' "$dmesg_log"
subject 'unsupported time window rejected' dmesg \
        '"$TOOL" --since yesterday -F "$0" >/dev/null 2>&1; [ "$?" -ne 0 ]' "$dmesg_log"
subject 'invalid buffer size rejected before reading' dmesg \
        '"$TOOL" -s nope -F "$0" >/dev/null 2>&1; [ "$?" -ne 0 ]' "$dmesg_log"
subject 'invalid level fuzz rejected' dmesg \
        'for x in nope 8 -1 err,,warn +err+; do "$TOOL" -l "$x" -F "$0" >/dev/null 2>&1 && exit 1; done; exit 0' "$dmesg_log"
subject 'invalid facility fuzz rejected' dmesg \
        'for x in nope 24 -1 kern,,user; do "$TOOL" -f "$x" -F "$0" >/dev/null 2>&1 && exit 1; done; exit 0' "$dmesg_log"

group lsfd
if command -v python3 >/dev/null 2>&1; then
        fd_file="$work/lsfd-file"
        fd_state="$work/lsfd-state"
        printf abc > "$fd_file"
        python3 -c 'import os,select,socket,sys,time
f=open(sys.argv[1], "r+")
d=os.open(sys.argv[2], os.O_RDONLY | os.O_DIRECTORY)
r,w=os.pipe()
s=socket.socket()
s.bind(("127.0.0.1", 0))
s.listen()
e=select.epoll()
e.register(r, select.EPOLLIN)
open(sys.argv[3], "w").write("%d %d %d %d %d %d %d\n" % (os.getpid(), f.fileno(), d, r, w, s.fileno(), e.fileno()))
time.sleep(60)' "$fd_file" "$work" "$fd_state" &
        fd_holder=$!
        fd_tries=0
        while [ ! -s "$fd_state" ] && [ "$fd_tries" -lt 100 ]; do
                sleep .02
                fd_tries=$((fd_tries + 1))
        done

        if [ -s "$fd_state" ]; then
                read -r fd_pid fd_regular fd_directory fd_pipe_r fd_pipe_w fd_socket fd_anon < "$fd_state"
                compare 'numeric descriptor kernel identity' lsfd \
                        '"$TOOL" -p "$1" -n -r -o FD,MODE,KNAME,INODE,MAJ:MIN | awk '\''$1 ~ /^[0-9]+$/ { print }'\''' \
                        sh "$fd_pid"
                compare 'regular directory and pipe names' lsfd \
                        '"$TOOL" -p "$1" -n -r -o FD,NAME | awk -v a="$2" -v b="$3" -v c="$4" -v d="$5" '\''$1 == a || $1 == b || $1 == c || $1 == d { print }'\''' \
                        sh "$fd_pid" "$fd_regular" "$fd_directory" "$fd_pipe_r" "$fd_pipe_w"
                compare 'numeric descriptor JSON types' lsfd \
                        '"$TOOL" -J -p "$1" -o FD,MODE,KNAME,INODE,MAJ:MIN | python3 -c '\''import json,sys; data=json.load(sys.stdin); rows=data[next(iter(data))]; print(json.dumps([x for x in rows if isinstance(x["fd"], int)], sort_keys=True, separators=(",", ":")))'\''' \
                        sh "$fd_pid"
                subject 'basic descriptor classification' lsfd \
                        '"$TOOL" -p "$1" -n -r -o FD,TYPE | awk -v f="$2" -v d="$3" -v r="$4" -v w="$5" -v s="$6" -v a="$7" '\''$1==f { good += $2=="REG" } $1==d { good += $2=="DIR" } $1==r || $1==w { good += $2=="FIFO" } $1==s { good += $2=="SOCK" } $1==a { good += $2=="anon_inode" } END { exit good != 6 }'\''' \
                        sh "$fd_pid" "$fd_regular" "$fd_directory" "$fd_pipe_r" "$fd_pipe_w" "$fd_socket" "$fd_anon"
                subject 'additive output projection' lsfd \
                        '"$TOOL" -p "$1" -o +UID | head -1 | awk '\''{ exit $1!="COMMAND" || $NF!="UID" }'\''' \
                        sh "$fd_pid"
        else
                lost 'controlled descriptor fixture' 'holder did not publish its descriptor set'
        fi
        kill "$fd_holder" 2>/dev/null || true
        wait "$fd_holder" 2>/dev/null || true
else
        lost 'controlled descriptor fixture' 'python3 is required'
fi

subject 'PID list accepted' lsfd \
        '"$TOOL" -p "$$,1" -n -r -o PID,FD >/dev/null'
subject 'display filter rejected' lsfd \
        '"$TOOL" --filter PID=1 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'counter rejected' lsfd \
        '"$TOOL" --counter count:FD:FD -p 1 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'eventpoll detail column rejected' lsfd \
        '"$TOOL" -o EVENTPOLL.TFDS -p 1 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'thread census rejected' lsfd \
        '"$TOOL" --threads -p 1 >/dev/null 2>&1; [ "$?" -ne 0 ]'

group lslocks
lock_file="$work/lslocks-file"
printf '1234567890' > "$lock_file"
flock -x "$lock_file" sleep 15 &
lock_holder=$!
lock_inode=$(stat -Lc %i "$lock_file")
lock_tries=0
while ! grep -q " $lock_holder .*:$lock_inode " /proc/locks 2>/dev/null &&
      [ "$lock_tries" -lt 100 ]; do
        sleep .02
        lock_tries=$((lock_tries + 1))
done

if grep -q " $lock_holder .*:$lock_inode " /proc/locks 2>/dev/null; then
        compare 'default holder row' lslocks '"$TOOL" -p "$1"' "$lock_holder"
        compare 'raw core columns' lslocks \
                '"$TOOL" -p "$1" -n -r -o COMMAND,PID,TYPE,SIZE,INODE,MAJ:MIN,MODE,M,START,END,PATH,BLOCKER' \
                "$lock_holder"
        compare 'byte size' lslocks \
                '"$TOOL" -b -p "$1" -n -r -o SIZE,INODE,PATH' \
                "$lock_holder"
        compare 'json types and null blocker' lslocks \
                '"$TOOL" -J -p "$1" -o COMMAND,PID,TYPE,SIZE,M,BLOCKER' \
                "$lock_holder"
        compare 'additive output list' lslocks \
                '"$TOOL" -p "$1" -n -r -o +INODE' "$lock_holder"

        flock -x "$lock_file" /bin/true &
        lock_waiter=$!
        lock_tries=0
        while ! grep -q -- '->.*'" $lock_waiter " /proc/locks 2>/dev/null &&
              [ "$lock_tries" -lt 100 ]; do
                sleep .02
                lock_tries=$((lock_tries + 1))
        done
        if grep -q -- '->.*'" $lock_waiter " /proc/locks 2>/dev/null; then
                compare 'blocked waiter relation' lslocks \
                        '"$TOOL" -p "$1" -n -r -o PID,MODE,BLOCKER' \
                        "$lock_waiter"
        else
                lost 'blocked waiter relation' 'waiter did not reach /proc/locks'
        fi
        kill "$lock_waiter" 2>/dev/null || true
        wait "$lock_waiter" 2>/dev/null || true

        chmod 000 "$lock_file"
        compare 'noinaccessible resolved fd' lslocks \
                '"$TOOL" -i -u -p "$1" -n -r -o PID,PATH' "$lock_holder"
        chmod 600 "$lock_file"
else
        lost 'controlled flock fixture' 'holder did not reach /proc/locks'
fi
kill "$lock_holder" 2>/dev/null || true
wait "$lock_holder" 2>/dev/null || true

if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import fcntl,sys,time; f=open(sys.argv[1], "r+"); fcntl.lockf(f, fcntl.LOCK_EX, 4, 2); time.sleep(10)' \
                "$lock_file" &
        posix_holder=$!
        lock_tries=0
        while ! grep -q " $posix_holder .*:$lock_inode " /proc/locks 2>/dev/null &&
              [ "$lock_tries" -lt 100 ]; do
                sleep .02
                lock_tries=$((lock_tries + 1))
        done
        if grep -q " $posix_holder .*:$lock_inode " /proc/locks 2>/dev/null; then
                compare 'POSIX byte range' lslocks \
                        '"$TOOL" -p "$1" -n -r -o PID,TYPE,MODE,START,END,PATH' \
                        "$posix_holder"
        else
                lost 'POSIX byte range' 'holder did not reach /proc/locks'
        fi
        kill "$posix_holder" 2>/dev/null || true
        wait "$posix_holder" 2>/dev/null || true
fi

subject 'display filter rejected' lslocks \
        '"$TOOL" --filter PID=1 >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'column metadata rejected' lslocks \
        '"$TOOL" --list-columns >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'holders census rejected' lslocks \
        '"$TOOL" -o HOLDERS >/dev/null 2>&1; [ "$?" -ne 0 ]'

group mesg
cat > "$work/mesg-pty.py" <<'PY'
import errno
import fcntl
import os
import pty
import subprocess
import sys
import termios

tool = sys.argv[1]

def one(arguments, initial):
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.fchmod(slave, initial)

    def session():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    child = subprocess.Popen([tool] + arguments, stdin=slave, stdout=slave,
                             stderr=slave, close_fds=True, preexec_fn=session)
    os.close(slave)
    output = bytearray()
    while True:
        try:
            part = os.read(master, 4096)
        except OSError as error:
            if error.errno == errno.EIO:
                break
            raise
        if not part:
            break
        output += part
    status = child.wait()
    mode = os.stat(path).st_mode & 0o777
    os.close(master)
    said = bytes(output).replace(b"\r", b"").decode("utf-8", "strict")
    print("%s %03o %d %03o <%s>" %
          (" ".join(arguments) or "query", initial, status, mode,
           said.replace("\n", "|")))

one([], 0o620)
one([], 0o600)
one(["-v", "n"], 0o620)
one(["-v", "y"], 0o600)
one(["yes"], 0o600)
one(["no"], 0o620)
PY
compare 'tty query and permission transitions' mesg \
        'python3 "$1/mesg-pty.py" "$TOOL"' sh "$work"
subject 'non-tty status is quiet' mesg \
        '"$TOOL" > "$0/out" 2> "$0/error"; [ "$?" = 2 ] && [ ! -s "$0/out" ] && [ ! -s "$0/error" ]' \
        "$work"
subject 'verbose non-tty diagnostic' mesg \
        '"$TOOL" -v > "$0/out" 2> "$0/error"; [ "$?" = 2 ] && [ ! -s "$0/out" ] && grep -qx "mesg: no tty" "$0/error"' \
        "$work"

group rfkill
rfkill_root="$work/rfkill-sys"
mkdir -p "$rfkill_root/rfkill3" "$rfkill_root/rfkill7" \
         "$rfkill_root/rfkill11"
for record in '3 wifi-card wlan 0 0' '7 blue-chip bluetooth 1 0' \
              '11 modem wwan 0 1'; do
        set -- $record
        printf '%s\n' "$2" > "$rfkill_root/rfkill$1/name"
        printf '%s\n' "$3" > "$rfkill_root/rfkill$1/type"
        printf '%s\n' "$4" > "$rfkill_root/rfkill$1/soft"
        printf '%s\n' "$5" > "$rfkill_root/rfkill$1/hard"
done
: > "$work/rfkill-events"
cat > "$work/rfkill-table" <<'EOF'
ID TYPE      DEVICE         SOFT      HARD
 3 wlan      wifi-card unblocked unblocked
 7 bluetooth blue-chip   blocked unblocked
11 wwan      modem     unblocked   blocked
EOF
cat > "$work/rfkill-legacy" <<'EOF'
3: wifi-card: Wireless LAN
	Soft blocked: no
	Hard blocked: no
EOF
subject 'bounded sysfs snapshot table' rfkill \
        'MOONWATER_RFKILL_ROOT="$1/rfkill-sys" "$TOOL" >"$1/got" && cmp -s "$1/rfkill-table" "$1/got"' \
        sh "$work"
subject 'raw selected columns and filter' rfkill \
        'out=$(MOONWATER_RFKILL_ROOT="$1/rfkill-sys" "$TOOL" -r -o ID,DEVICE,SOFT bluetooth) && [ "$out" = "ID DEVICE SOFT
7 blue-chip blocked" ]' sh "$work"
subject 'legacy list compatibility' rfkill \
        'MOONWATER_RFKILL_ROOT="$1/rfkill-sys" "$TOOL" list wlan >"$1/got" && cmp -s "$1/rfkill-legacy" "$1/got"' \
        sh "$work"
subject 'JSON typed ID and complete rows' rfkill \
        'MOONWATER_RFKILL_ROOT="$1/rfkill-sys" "$TOOL" -J >"$1/got" && grep -q '"'"'"id": 3'"'"' "$1/got" && grep -q '"'"'"device": "blue-chip"'"'"' "$1/got" && grep -q '"'"'"hard": "blocked"'"'"' "$1/got" && [ "$(grep -c '"'"'"id":'"'"' "$1/got")" = 3 ]' \
        sh "$work"
subject 'block emits exact change-all and indexed ABI records' rfkill \
        ': >"$1/rfkill-events"; MOONWATER_RFKILL_ROOT="$1/rfkill-sys" MOONWATER_RFKILL_DEVICE="$1/rfkill-events" "$TOOL" block wlan 7 || exit; bytes=$(od -An -tx1 -v "$1/rfkill-events" | tr -d " \n"); [ "$bytes" = "00000000010301000700000000020100" ]' \
        sh "$work"
subject 'toggle snapshots mixed state into bounded indexed changes' rfkill \
        ': >"$1/rfkill-events"; MOONWATER_RFKILL_ROOT="$1/rfkill-sys" MOONWATER_RFKILL_DEVICE="$1/rfkill-events" "$TOOL" toggle all || exit; bytes=$(od -An -tx1 -v "$1/rfkill-events" | tr -d " \n"); [ "$bytes" = "030000000102010007000000020200000b00000005020100" ]' \
        sh "$work"
subject 'event monitor rejected without waiting' rfkill \
        'MOONWATER_RFKILL_ROOT="$1/rfkill-sys" timeout 2 "$TOOL" event >/dev/null 2>&1; [ "$?" = 1 ]' \
        sh "$work"
compare 'unknown command rejected' rfkill '"$TOOL" unknown'
compare 'JSON and raw conflict rejected' rfkill '"$TOOL" -J -r'

group logger
cat > "$work/logger-wire.py" <<'PY'
import os
import socket
import subprocess
import sys
import tempfile

tool, mode = sys.argv[1:]
environment = dict(os.environ, TZ="UTC", LC_ALL="C")
temporary = None

if mode == "unix":
    temporary = tempfile.mktemp(prefix="moonwater-logger-")
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    listener.bind(temporary)
    transport = ["-u", temporary, "--socket-errors=on"]
elif mode in ("udp", "records", "size", "id", "file"):
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(("127.0.0.1", 0))
    transport = ["-n", "127.0.0.1", "-P",
                 str(listener.getsockname()[1]), "-d"]
else:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    transport = ["-n", "127.0.0.1", "-P",
                 str(listener.getsockname()[1]), "-T"]

stable = ["--rfc5424=notime,notq,nohost", "-t", "moon"]
payload = None
messages = 1
arguments = ["alpha", "beta"]

if mode == "octet":
    stable.insert(0, "--octet-count")
elif mode == "octet-long":
    stable += ["--octet-count", "--size", "8192"]
    payload = b"x" * 8192 + b"\n"
    arguments = []
elif mode == "records":
    stable += ["--prio-prefix", "--skip-empty"]
    payload = b"<134>hello\n\n<3>bad\n"
    arguments = []
    messages = 2
elif mode == "size":
    stable += ["--size", "5"]
    payload = b"abcdefghijkl\n"
    arguments = []
    messages = 3
elif mode == "id":
    stable += ["--id=4242"]
    payload = b"hello\n"
    arguments = []
elif mode == "file":
    source = tempfile.NamedTemporaryFile(delete=False)
    source.write(b"one\ntwo\n")
    source.close()
    stable += ["--file", source.name]
    arguments = []
    messages = 2
elif mode == "long":
    stable += ["--size", "70000"]
    payload = b"x" * 70000 + b"\n"
    arguments = []

subprocess.run([tool] + transport + stable + arguments, input=payload,
               check=True, env=environment)

received = []
if mode in ("tcp", "octet", "octet-long", "long"):
    connection, _ = listener.accept()
    while True:
        part = connection.recv(65536)
        if not part:
            break
        received.append(part)
    connection.close()
    received = [b"".join(received)]
else:
    listener.settimeout(2)
    received = [listener.recv(131072) for _ in range(messages)]

listener.close()
if temporary:
    os.unlink(temporary)
if mode == "file":
    os.unlink(source.name)
sys.stdout.buffer.write(b"\0".join(received))
PY

compare 'Unix datagram wire' logger \
        'python3 "$1/logger-wire.py" "$TOOL" unix' sh "$work"
compare 'loopback UDP wire' logger \
        'python3 "$1/logger-wire.py" "$TOOL" udp' sh "$work"
compare 'loopback TCP newline framing' logger \
        'python3 "$1/logger-wire.py" "$TOOL" tcp' sh "$work"
compare 'RFC6587 octet-count framing' logger \
        'python3 "$1/logger-wire.py" "$TOOL" octet' sh "$work"
compare 'RFC6587 overlapping long frame' logger \
        'python3 "$1/logger-wire.py" "$TOOL" octet-long' sh "$work"
compare 'priority-prefixed stdin records' logger \
        'python3 "$1/logger-wire.py" "$TOOL" records' sh "$work"
compare 'message-size record chunks' logger \
        'python3 "$1/logger-wire.py" "$TOOL" size' sh "$work"
compare 'explicit RFC5424 process id' logger \
        'python3 "$1/logger-wire.py" "$TOOL" id' sh "$work"
compare 'file records' logger \
        'python3 "$1/logger-wire.py" "$TOOL" file' sh "$work"
compare 'refill-crossing TCP record' logger \
        'python3 "$1/logger-wire.py" "$TOOL" long' sh "$work"
printf 'MESSAGE=one\nFIELD=value\n' > "$work/logger-journal"
compare 'journald native payload dry run' logger \
        '"$TOOL" --journald="$1" --no-act --stderr 2>&1' sh "$work/logger-journal"
subject 'structured data extension rejected' logger \
        '"$TOOL" --sd-id meta --no-act message >/dev/null 2>&1; [ "$?" -ne 0 ]'

group login-history
python3 - "$work/login-history" "$work/login-dump" <<'PY'
import ipaddress
import platform
import struct
import sys

history, dump = sys.argv[1:]
order = "<" if sys.byteorder == "little" else ">"
compat = struct.calcsize("P") == 4 or platform.machine() in (
    "x86_64", "amd64", "i386", "i686")
size = 384 if compat else 400

def record(kind, pid, line, user, host, seconds, session=0, usec=0,
           address="0.0.0.0", ident="id", termination=0, status=0):
    row = bytearray(size)
    struct.pack_into(order + "h", row, 0, kind)
    struct.pack_into(order + "i", row, 4, pid)
    for offset, width, value in ((8, 32, line), (40, 4, ident),
                                 (44, 32, user), (76, 256, host)):
        value = value.encode()
        row[offset:offset + min(width, len(value))] = value[:width]
    struct.pack_into(order + "hh", row, 332, termination, status)
    if compat:
        struct.pack_into(order + "iii", row, 336, session, seconds, usec)
        address_offset = 348
    else:
        struct.pack_into(order + "q", row, 336, session)
        struct.pack_into(order + "qq", row, 344, seconds, usec)
        address_offset = 360
    packed = ipaddress.ip_address(address).packed
    row[address_offset:address_offset + len(packed)] = packed
    return row

rows = [
    record(2, 0, "~", "reboot", "6.9.0", 1700000000),
    record(7, 101, "pts/1", "alice", "alpha.example", 1700000100,
           11, 123456, "127.0.0.1", "p1"),
    record(7, 102, "tty1", "bob", "", 1700000200, 12, ident="t1"),
    record(8, 101, "pts/1", "", "", 1700000300, 11, ident="p1"),
    record(1, ord("3") + ord("2") * 256, "~", "runlevel", "6.9.0",
           1700000350),
    record(7, 103, "pts/2", "alice", "beta.example", 1700000400,
           13, 999999, "127.0.0.2", "p2"),
    record(1, 0, "~", "shutdown", "6.9.0", 1700000500),
    record(2, 0, "~", "reboot", "6.10.0", 1700000600),
    record(7, 104, "pts/3", "carol", "gamma.example", 1700000700,
           14, 7, "127.0.0.3", "p3"),
    record(8, 104, "pts/3", "", "", 1700000800, 14, ident="p3"),
]
with open(history, "wb") as stream:
    stream.write(b"".join(rows))
with open(dump, "wb") as stream:
    stream.write(b"".join(rows))
    stream.write(record(7, 105, "pts/4", "v6", "ipv6.example", 1700000900,
                        15, 42, "2001:db8::1", "p4"))
    stream.write(b"partial record")
PY

compare 'reverse sessions and reboot boundaries' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -f "$1"' sh "$work/login-history"
compare 'hostname suppression' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -R -f "$1"' sh "$work/login-history"
compare 'hostname last projection' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -a -f "$1"' sh "$work/login-history"
compare 'full timestamps' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -F -f "$1"' sh "$work/login-history"
compare 'tab separated records' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -T -f "$1"' sh "$work/login-history"
compare 'shutdown and runlevel records' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -x -f "$1"' sh "$work/login-history"
compare 'bounded result count' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -n 3 -f "$1"' sh "$work/login-history"
compare 'zero count means unlimited' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -n 0 -f "$1"' sh "$work/login-history"
compare 'user operand filter' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -f "$1" alice' sh "$work/login-history"
compare 'numeric address projection' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -i -f "$1"' sh "$work/login-history"
compare 'ISO timestamp projection' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" --time-format iso -f "$1"' \
        sh "$work/login-history"
compare 'native dump including IPv6 and partial tail' utmpdump \
        'LC_ALL=C TZ=UTC0 "$TOOL" "$1"' sh "$work/login-dump"
compare 'stdin dump' utmpdump \
        'LC_ALL=C TZ=UTC0 "$TOOL" < "$1"' sh "$work/login-dump"
compare 'named dump output' utmpdump \
        'rm -f "$2/utmpdump.out"; LC_ALL=C TZ=UTC0 "$TOOL" -o "$2/utmpdump.out" "$1" || exit; cat "$2/utmpdump.out"' \
        sh "$work/login-dump" "$work"
compare 'missing history file' last \
        'LC_ALL=C TZ=UTC0 "$TOOL" -f "$1/missing"' sh "$work"
compare 'missing dump file' utmpdump \
        'LC_ALL=C TZ=UTC0 "$TOOL" "$1/missing"' sh "$work"
subject 'last DNS policy rejected explicitly' last \
        '"$TOOL" -d -f "$1" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$work/login-history"
subject 'last time selection rejected explicitly' last \
        '"$TOOL" -s yesterday -f "$1" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$work/login-history"
subject 'utmp reverse mutation rejected explicitly' utmpdump \
        '"$TOOL" -r "$1" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$work/login-dump"
subject 'utmp follow policy rejected explicitly' utmpdump \
        '"$TOOL" -f "$1" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$work/login-dump"

group terminal-recording
script_work="$work/script"
mkdir -p "$script_work"
cat > "$script_work/controlling-tty.py" <<'PY'
import os

descriptor = os.open("/dev/tty", os.O_RDONLY)
print(os.tcgetpgrp(descriptor) == os.getpgrp())
os.close(descriptor)
PY
compare 'quiet command output' script \
        'rm -f "$1/out"; LC_ALL=C TZ=UTC0 "$TOOL" -q -e -O "$1/out" -c "printf \"hello\\n\"" </dev/null' \
        sh "$script_work"
compare 'piped input and terminal echo' script \
        'rm -f "$1/out"; printf "input\n" | LC_ALL=C TZ=UTC0 "$TOOL" -q -e -O "$1/out" -c "read x; printf \"OUT:%s\\n\" \"\$x\""' \
        sh "$script_work"
compare 'input echo disabled' script \
        'rm -f "$1/out"; printf "input\n" | LC_ALL=C TZ=UTC0 "$TOOL" -q -e -E never -O "$1/out" -c "read x; printf \"OUT:%s\\n\" \"\$x\""' \
        sh "$script_work"
compare 'child status returned' script \
        'rm -f "$1/out"; LC_ALL=C TZ=UTC0 "$TOOL" -q -e -O "$1/out" -c "printf x; exit 7" </dev/null' \
        sh "$script_work"
compare 'direct command vector' script \
        'rm -f "$1/out"; LC_ALL=C TZ=UTC0 "$TOOL" -q -e -O "$1/out" -- /usr/bin/printf direct </dev/null' \
        sh "$script_work"
compare 'ordinary start and done notices' script \
        'rm -f "$1/out"; LC_ALL=C TZ=UTC0 "$TOOL" -e -O "$1/out" -c "printf x" </dev/null' \
        sh "$script_work"
subject 'log aliases cannot interleave streams' script \
        'rm -f "$1/alias"; ! "$TOOL" -q -O "$1/alias" -T "$1/./alias" -c true </dev/null >/dev/null 2>&1' \
        sh "$script_work"
subject 'log symlink is rejected without force' script \
        'printf sentinel > "$1/target"; rm -f "$1/link"; ln -s target "$1/link"; ! "$TOOL" -q -O "$1/link" -c true </dev/null >/dev/null 2>&1 && [ "$(cat "$1/target")" = sentinel ]' \
        sh "$script_work"
subject 'detached recorder gives child controlling terminal' script \
        'rm -f "$1/ctty.log"; out=$(timeout 5 setsid "$TOOL" -q -e -O "$1/ctty.log" -c "python3 $1/controlling-tty.py" </dev/null 2>/dev/null | tr -d "\r"); [ "$out" = True ]' \
        sh "$script_work"

printf 'Script started on fixture\nabcDEF\nScript done on fixture\n' > "$script_work/classic.log"
printf '0.000000 3\n0.000000 3\n' > "$script_work/classic.time"
compare 'classic zero-delay playback' scriptreplay \
        'LC_ALL=C "$TOOL" -m0 "$1/classic.time" "$1/classic.log"' \
        sh "$script_work"

printf 'Script started on fixture\nOUT\r\n\nScript done on fixture\n' > "$script_work/advanced.out"
printf 'Script started on fixture\nIN\n\nScript done on fixture\n' > "$script_work/advanced.in"
printf 'H 0.000000 START_TIME fixture\nI 0.000000 3\nO 0.000000 5\nH 0.000000 EXIT_CODE 0\n' > "$script_work/advanced.time"
compare 'advanced output stream playback' scriptreplay \
        'LC_ALL=C "$TOOL" -m0 -t "$1/advanced.time" -I "$1/advanced.in" -O "$1/advanced.out" -x out' \
        sh "$script_work"
compare 'advanced input stream playback' scriptreplay \
        'LC_ALL=C "$TOOL" -m0 -t "$1/advanced.time" -I "$1/advanced.in" -O "$1/advanced.out" -x in' \
        sh "$script_work"
compare 'always CR conversion' scriptreplay \
        'LC_ALL=C "$TOOL" -m0 -c always -t "$1/advanced.time" -I "$1/advanced.in" -O "$1/advanced.out" -x out' \
        sh "$script_work"
subject 'script output-limit policy rejected explicitly' script \
        '"$TOOL" -q -o 1K -c true "$1/out" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$script_work"
subject 'scriptreplay summary policy rejected explicitly' scriptreplay \
        '"$TOOL" --summary "$1/classic.time" "$1/classic.log" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$script_work"
subject 'malformed timing fails without hanging' scriptreplay \
        'printf "999999999999999999999 1\n" > "$1/bad.time"; timeout 2 "$TOOL" "$1/bad.time" "$1/classic.log" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$script_work"
subject 'divisor scaling overflow fails without waiting' scriptreplay \
        'printf "9999999999.5 0\n" > "$1/overflow.time"; timeout 2 "$TOOL" -d 10000000000 "$1/overflow.time" "$1/classic.log" >/dev/null 2>&1; [ "$?" -ne 0 ]' \
        sh "$script_work"

group write-wall
compare 'write requires a login' write '"$TOOL"'
compare 'write rejects extra operands' write '"$TOOL" user pts/0 extra'
compare 'wall rejects a zero timeout' wall '"$TOOL" -t 0 hello'
compare 'wall rejects an unknown group' wall \
        '"$TOOL" -g moonwater-group-that-does-not-exist hello'

# Synthetic utmp is admitted only by an unprivileged process and every entry
# below names this process's account and a PTY it created.  It never consults
# or writes a host session.  The same harness covers x86's 384-byte utmp and
# the asm-generic 400-byte layout used by ARM64/RISC-V.
if TOOL_WRITE="$work/bin/write" TOOL_WALL="$work/bin/wall" python3 <<'PY'
import errno
import fcntl
import os
import platform
import pwd
import pty
import select
import struct
import subprocess
import termios
import time
import tty

write = os.environ["TOOL_WRITE"]
wall = os.environ["TOOL_WALL"]
user = pwd.getpwuid(os.getuid()).pw_name
machine = platform.machine()
small = machine in ("x86_64", "i386", "i686")
record_size = 384 if small else 400
seconds_offset = 340 if small else 344
temporary = os.path.join(os.path.dirname(write), "controlled.utmp")

def terminal():
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.fchmod(slave, 0o620)
    path = os.ttyname(slave)
    assert path.startswith("/dev/")
    return master, slave, path[5:]

def record(line):
    data = bytearray(record_size)
    struct.pack_into("<h", data, 0, 7)       # USER_PROCESS
    struct.pack_into("<i", data, 4, os.getpid())
    data[8:8 + min(32, len(line))] = line.encode()[:32]
    data[44:44 + min(32, len(user))] = user.encode()[:32]
    if small:
        struct.pack_into("<i", data, seconds_offset, int(time.time()))
    else:
        struct.pack_into("<q", data, seconds_offset, int(time.time()))
    return data

def database(lines):
    with open(temporary, "wb") as stream:
        for line in lines:
            stream.write(record(line))

def drain(master):
    output = bytearray()
    os.set_blocking(master, False)
    while select.select([master], [], [], 0.15)[0]:
        try:
            part = os.read(master, 65536)
        except OSError as error:
            if error.errno == errno.EIO:
                break
            raise
        if not part:
            break
        output += part
    return bytes(output)

environment = dict(os.environ, MOONWATER_UTMP=temporary,
                   LC_ALL="C", TZ="UTC")

# Explicit write: control bytes and a high C-locale byte must be made safe.
master, slave, line = terminal()
database([line])
result = subprocess.run([write, user, line], input=b"A\x01B\nC\x80\n",
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                        env=environment)
assert result.returncode == 0, result.stderr
payload = drain(master)
assert b"A^AB\r\nC\\200\r\nEOF\r\n" in payload, payload
os.close(master); os.close(slave)

# Implicit write chooses the least-idle session, and says so when there are
# two.  atime belongs to the tty, not the utmp timestamp.
m1, s1, l1 = terminal()
m2, s2, l2 = terminal()
os.utime("/dev/" + l1, (100, 100))
os.utime("/dev/" + l2, (200, 200))
database([l1, l2])
result = subprocess.run([write, user], input=b"chosen\n",
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                        env=environment)
assert result.returncode == 0, result.stderr
first, second = drain(m1), drain(m2)
assert b"chosen\r\n" not in first and b"chosen\r\n" in second
assert b"logged in more than once" in result.stderr
for descriptor in (m1, s1, m2, s2): os.close(descriptor)

# One wall snapshot fans one prepared message to distinct controlled PTYs;
# duplicate utmp rows do not duplicate delivery.  Group selection exercises
# the existing passwd/group membership engine.
m1, s1, l1 = terminal()
m2, s2, l2 = terminal()
database([l1, l1, l2])
result = subprocess.run([wall, "-g", str(os.getgid()), "hello", "world"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                        env=environment)
assert result.returncode == 0, result.stderr
first, second = drain(m1), drain(m2)
for payload in (first, second):
    assert payload.count(b"hello world\r\n") == 1, payload
    assert b"Broadcast message from " in payload
for descriptor in (m1, s1, m2, s2): os.close(descriptor)

# Tab state advances to the next eight-column stop before wall pads the row.
master, slave, line = terminal()
database([line])
result = subprocess.run([wall], input=b"1234567\tZ\n",
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                        env=environment)
assert result.returncode == 0, result.stderr
payload = drain(master)
assert b"1234567\tZ" + b" " * 70 + b"\r\n" in payload, payload
os.close(master); os.close(slave)

# File/stdin share the text reader and preserve long records across refills.
master, slave, line = terminal()
database([line])
source = temporary + ".message"
with open(source, "wb") as stream:
    stream.write(b"x" * 70000 + b"\nend\n")
process = subprocess.Popen([wall, source], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, env=environment)
payload = bytearray()
os.set_blocking(master, False)
while process.poll() is None:
    if select.select([master], [], [], 0.05)[0]:
        try:
            payload += os.read(master, 65536)
        except BlockingIOError:
            pass
stderr = process.communicate()[1]
payload += drain(master)
assert process.returncode == 0, stderr
assert b"end" in payload and payload.count(b"x") >= 70000
os.unlink(source)
os.close(master); os.close(slave)
os.unlink(temporary)
PY
then
        won
else
        lost 'controlled PTY write/wall delivery' 'safe local delivery failed'
fi

group ul
printf 'plain\n_\bU x\bx R\bS \016g\017\n\0339sub\0338 normal \0338sup\0339\nform\fnext\n' \
        > "$work/ul-controls"
compare 'dumb terminal overstrikes' ul \
        'TERM=dumb "$TOOL" < "$1"' sh "$work/ul-controls"
compare 'indicated attribute plane' ul \
        'TERM=dumb "$TOOL" -i < "$1"' sh "$work/ul-controls"
compare 'ANSI attribute capabilities' ul \
        'TERM=ansi "$TOOL" < "$1"' sh "$work/ul-controls"
compare 'xterm attribute capabilities' ul \
        'TERM=xterm "$TOOL" < "$1"' sh "$work/ul-controls"
compare 'terminal override aliases' ul \
        'TERM=dumb "$TOOL" -T linux < "$1"' sh "$work/ul-controls"
compare 'full and repeated half-line motion' ul \
        'printf "one\n\0337two\nab\0339c\0339d\nab\0338c\0338d\n" | TERM=ansi "$TOOL" -i'
compare 'multiple input files' ul \
        'printf "_\bA\n" > "$1"; printf "b\bb\n" > "$2"; TERM=dumb "$TOOL" -i "$1" "$2"' \
        sh "$work/ul-one" "$work/ul-two"
compare 'unknown terminal falls back to dumb' ul \
        'TERM=ansi "$TOOL" -t no-such-moonwater-terminal < "$1"' \
        sh "$work/ul-controls"
compare 'unknown escape is fatal' ul \
        'printf "before\033Xafter\n" | TERM=dumb "$TOOL"'
compare 'missing input is fatal' ul \
        'TERM=dumb "$TOOL" "$1"' sh "$work/no-such-ul-input"

python3 - "$work/ul-generated" <<'PY'
import random, sys
random.seed(9241)
atoms = [b'a', b'b', b'_', b' ', b'\b', b'\t', b'\r', b'\n', b'\f',
         b'\016', b'\017', b'\0337', b'\0338', b'\0339']
open(sys.argv[1], 'wb').write(b''.join(random.choice(atoms)
                                      for _ in range(50000)))
PY
compare 'generated mixed control stream' ul \
        'TERM=ansi "$TOOL" -i < "$1"' sh "$work/ul-generated"
python3 - "$work/ul-long" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'x' * 200000 + b'\r' + b'x' * 200000 + b'\n')
PY
compare 'long bold plane' ul \
        'TERM=dumb "$TOOL" -i < "$1"' sh "$work/ul-long"

group look-line
printf 'aardvark\napp\napple\napricot\nbanana\n' > "$work/look-raw"
compare 'look raw prefix range' look \
        '"$TOOL" app "$1"' sh "$work/look-raw"
compare 'look absent prefix status' look \
        '"$TOOL" azure "$1"' sh "$work/look-raw"

printf 'A pple\nA!pricot\nbanana\n' > "$work/look-dictionary"
LC_ALL=C sort -df "$work/look-dictionary" -o "$work/look-dictionary"
compare 'look dictionary and case order' look \
        '"$TOOL" -df ap "$1"' sh "$work/look-dictionary"
compare 'look termination character included' look \
        'printf "a:1\na:2\nab:3\nb:4\n" > "$1"; "$TOOL" -t: a:any "$1"' \
        sh "$work/look-terminate"
compare 'look WORDLIST default flags' look \
        'WORDLIST="$1" "$TOOL" ap' sh "$work/look-dictionary"
compare 'look unterminated matching record' look \
        'printf "a\nlast value" > "$1"; "$TOOL" last "$1"' \
        sh "$work/look-unterminated"

python3 - "$work/look-generated" <<'PY'
import random, string, sys
random.seed(8712)
rows = []
for _ in range(30000):
    rows.append(''.join(random.choice(string.ascii_letters + string.digits + ' ._-\t')
                        for _ in range(random.randrange(0, 55))))
rows.sort(key=lambda row: row.encode())
open(sys.argv[1], 'wb').write(('\n'.join(rows) + '\n').encode())
PY
compare 'look generated lower bound' look \
        '"$TOOL" M "$1"' sh "$work/look-generated"

python3 - "$work/look-long" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'a\nlong' + b'x' * 200000 + b'\nz\n')
PY
compare 'look refill crossing record' look \
        '"$TOOL" long "$1"' sh "$work/look-long"
subject 'look streaming fallback' look \
        'out="$1.out"; printf "a\nlong one\nlong two\nz\n" | "$TOOL" long - > "$out"; printf "long one\nlong two\n" | cmp -s - "$out"' \
        sh "$work/look-stream"
subject 'look legacy binary selector' look \
        'out="$1.out"; "$TOOL" -b app "$1" > "$out"; printf "app\napple\n" | cmp -s - "$out"' \
        sh "$work/look-raw"

subject 'line emits one terminated record' line \
        'out="$1"; printf "one\ntwo\n" | "$TOOL" > "$out"; [ "$?" = 0 ] && printf "one\n" | cmp -s - "$out"' \
        sh "$work/line-terminated"
subject 'line adds newline at unterminated EOF' line \
        'out="$1"; status=0; printf one | "$TOOL" > "$out" || status=$?; [ "$status" = 1 ] && printf "one\n" | cmp -s - "$out"' \
        sh "$work/line-unterminated"
subject 'line empty EOF status and output' line \
        'out="$1"; status=0; "$TOOL" </dev/null > "$out" || status=$?; [ "$status" = 1 ] && printf "\n" | cmp -s - "$out"' \
        sh "$work/line-empty"
subject 'line leaves following pipe record unread' line \
        'out="$1"; printf "one\ntwo\n" | { "$TOOL"; cat; } > "$out"; printf "one\ntwo\n" | cmp -s - "$out"' \
        sh "$work/line-pipe"
subject 'line rejects operands' line \
        '! "$TOOL" extra >/dev/null 2>&1'
python3 - "$work/line-long" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'x' * 1500000 + b'\nsecond\n')
PY
subject 'line has no record-size ceiling' line \
        'out="$1.out"; "$TOOL" < "$1" > "$out"; [ "$(wc -c < "$out")" = 1500001 ]' \
        sh "$work/line-long"

group col-family
compare 'col basic line normalization' col \
        'printf "abc" | "$TOOL"'
compare 'col retained overstrike' col \
        'printf "A\bB\n" | "$TOOL"'
compare 'col last overstrike only' col \
        'printf "abc\rZ\n" | "$TOOL" -b'
compare 'col reverse full line motions' col \
        'printf "a\nb\vX\n" | "$TOOL"'
compare 'col reverse escape motion' col \
        'printf "a\nb\033\007X\n" | "$TOOL"'
compare 'col rounded half line' col \
        'printf "a\033\tb\n" | "$TOOL"'
compare 'col fine half lines' col \
        'printf "a\033\tb\n" | "$TOOL" -f'
compare 'col expands tab gaps' col \
        'printf "a\tb\n" | "$TOOL" -x'
compare 'col passed unknown control' col \
        'printf "a\001b\n" | "$TOOL" -p'
compare 'col character-set shifts' col \
        'printf "a\016B\017c\n" | "$TOOL"'
compare 'col accepts deep line buffer' col \
        'printf "a\n\nb\n" | "$TOOL" -l 512'

compare 'colcrt plain unterminated input' colcrt \
        'printf "abc" | "$TOOL"'
compare 'colcrt underline plane' colcrt \
        'printf "_\bA\n" | "$TOOL"'
compare 'colcrt underline suppression' colcrt \
        'printf "_\bA\n" | "$TOOL" -'
compare 'colcrt half-line display' colcrt \
        'printf "foo\nbar\n" | "$TOOL" -2'
compare 'colcrt tab expansion and escape rubout' colcrt \
        'printf "a\tb\nab\033\067Z\n" | "$TOOL"'
compare 'colcrt multiple files' colcrt \
        'printf "one\n" > "$1/a"; printf "two\n" > "$1/b"; "$TOOL" "$1/a" "$1/b"' \
        sh "$work"

compare 'colrm pass through' colrm \
        'printf "abcdef\n" | "$TOOL"'
compare 'colrm open ended removal' colrm \
        'printf "abcdef\n" | "$TOOL" 3'
compare 'colrm bounded removal' colrm \
        'printf "abcdef\n" | "$TOOL" 3 4'
compare 'colrm tab intersection padding' colrm \
        'printf "a\tb\n" | "$TOOL" 3 4'
compare 'colrm backspace columns' colrm \
        'printf "abc\bZdef\n" | "$TOOL" 1 1'
compare 'colrm carriage return byte' colrm \
        'printf "ab\rZ\n" | "$TOOL" 3 4'

awk 'BEGIN { for (i = 0; i < 800; i++) printf "r%04d\tv%d\bX\rP\n", i, i % 97 }' \
        > "$work/col-family-generated"
compare 'generated col controls' col \
        '"$TOOL" -b -x < "$1"' sh "$work/col-family-generated"
compare 'generated colrm controls' colrm \
        '"$TOOL" 5 11 < "$1"' sh "$work/col-family-generated"

awk 'BEGIN { for (i = 0; i < 70000; i++) printf "x"; printf "\tb\n" }' \
        > "$work/col-family-long"
compare 'colrm refill crossing record' colrm \
        '"$TOOL" 65534 65538 < "$1"' sh "$work/col-family-long"

group column
compare 'fill columns at fixed width' column \
        'printf "a\nbb\nccc\ndddd\neeeee\n" | "$TOOL" -c 20'
compare 'fill rows at fixed width' column \
        'printf "a\nbb\nccc\ndddd\neeeee\n" | "$TOOL" -c 20 -x'
compare 'space separated fill columns' column \
        'printf "a\nbb\nccc\ndddd\neeeee\n" | "$TOOL" -c 20 -S 2'
compare 'kept empty list entries' column \
        'printf "a\n\nb\n" | "$TOOL" -L'
compare 'blank separated table' column \
        'printf " a  bb ccc \nx yyyy z\n" | "$TOOL" -t'
compare 'literal separator empty cells' column \
        'printf "a,,c\n,bb,\nx,y,z,q\n" | "$TOOL" -t -s, -o "|"'
compare 'named hidden ordered right table' column \
        'printf "a 1 c d\nx 22 z q\n" | "$TOOL" -t -N A,B,C,D -H D -O C,A -R B'
compare 'header from first record' column \
        'printf "A B C\na bb c\nx yy z\n" | "$TOOL" -t -K'
compare 'table JSON with null fields' column \
        'printf "a b\nx\n" | "$TOOL" -J -N A,B,C -n things'
compare 'wrapped table width' column \
        'printf "a abcdefghi c\nx y z\n" | "$TOOL" -t -c 12 -N A,B,C -W B'
compare 'truncated table width' column \
        'printf "a bb abcdefghijkl\nx yy z\n" | "$TOOL" -t -c 10 -N A,B,C -T C'
compare 'multiple input files' column \
        'printf "one 1\n" > "$1/a"; printf "two 22\n" > "$1/b"; "$TOOL" -t "$1/a" "$1/b"' \
        sh "$work"

awk 'BEGIN { for (i = 0; i < 1200; i++) printf "r%04d,%s,%d,%s\n", i, (i % 5 ? "alpha" : ""), i % 97, (i % 11 ? "tail" : "") }' \
        > "$work/column-generated"
compare 'generated CSV-ish projection' column \
        '"$TOOL" -t -s, -N ROW,WORD,NUMBER,TAIL -O NUMBER,ROW -H TAIL -R NUMBER "$1"' \
        sh "$work/column-generated"

awk 'BEGIN { for (i = 0; i < 70000; i++) printf "x"; printf ",right\n" }' \
        > "$work/column-long"
compare 'record crossing reader refill' column \
        '"$TOOL" -t -s, -o : "$1"' sh "$work/column-long"
subject 'complex column properties rejected' column \
        'printf "a b\n" | "$TOOL" -t --table-column name=A >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'tree column mode rejected' column \
        'printf "a b\n" | "$TOOL" -t --tree 1 --tree-id 1 --tree-parent 2 >/dev/null 2>&1; [ "$?" -ne 0 ]'

group lsns
compare 'task namespaces raw' lsns \
        'target=$$; "$TOOL" -p "$target" -n -r -o NS,TYPE'
compare 'type filter and identity columns' lsns \
        'target=$$; "$TOOL" -t mnt -p "$target" -n -r -o NS,TYPE,PID,PPID,UID,USER'
compare 'path and reordered columns' lsns \
        'target=$$; "$TOOL" -p "$target" -n -r -o TYPE,PATH'
compare 'json projection' lsns \
        'target=$$; "$TOOL" -p "$target" -J -o NS,TYPE'
compare 'namespace inode operand' lsns \
        'inode=$(stat -Lc %i /proc/$$/ns/mnt); "$TOOL" -l -n -r -o NS,TYPE "$inode"'
compare 'unknown namespace type' lsns '"$TOOL" -t impossible'
compare 'unknown output column' lsns '"$TOOL" -o IMPOSSIBLE'
subject 'default rows unique and aggregated' lsns \
        '"$TOOL" -n -r -o NS,TYPE,NPROCS,PID,UID,USER | awk '\''NF < 6 || $3 < 1 || $4 < 1 { bad=1 } { key=$1 SUBSEP $2; if (seen[key]++) bad=1; rows++ } END { exit bad || !rows }'\'''
subject 'tree mode rejected' lsns \
        '"$TOOL" -T parent >/dev/null 2>&1; [ "$?" -ne 0 ]'
subject 'persistent mode rejected' lsns \
        '"$TOOL" -P >/dev/null 2>&1; [ "$?" -ne 0 ]'

if unshare -Urn /bin/true >/dev/null 2>&1; then
        compare 'isolated user and net namespace' lsns \
                'unshare -Urn /bin/sh -c '\''"$1" -p $$ -n -r -o TYPE'\'' sh "$TOOL"'
        subject 'isolated namespace inode is distinct' lsns \
                'parent=$(stat -Lc %i /proc/$$/ns/net); export TOOL parent; unshare -Urn /bin/sh -c '\''now=$(stat -Lc %i /proc/$$/ns/net); [ "$now" != "$parent" ] && "$TOOL" -t net -p $$ -n -r -o NS,TYPE | grep -q "^$now net$"'\'''
fi

section hardlink
compare 'recursive duplicate consolidation' hardlink \
        'd=$(mktemp -d "$1/hardlink-recursive.XXXXXX"); mkdir -p "$d/sub"; printf same >"$d/a"; cp -p "$d/a" "$d/sub/b"; "$TOOL" -q "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/sub/b")" ] && echo linked' \
        sh "$work"
compare 'dry run preserves separate inodes' hardlink \
        'd=$(mktemp -d "$1/hardlink-dry.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -qn "$d"; [ "$(stat -c %i "$d/a")" != "$(stat -c %i "$d/b")" ] && echo separate' \
        sh "$work"
compare 'default respects mode and timestamp' hardlink \
        'd=$(mktemp -d "$1/hardlink-meta.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; chmod 600 "$d/b"; touch -d @1000000000 "$d/a"; touch -d @1000000001 "$d/b"; "$TOOL" -q "$d"; [ "$(stat -c %i "$d/a")" != "$(stat -c %i "$d/b")" ] && echo separate' \
        sh "$work"
compare 'content mode ignores metadata' hardlink \
        'd=$(mktemp -d "$1/hardlink-content.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; chmod 600 "$d/b"; touch -d @1000000000 "$d/a"; touch -d @1000000001 "$d/b"; "$TOOL" -cq "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'equal-size unlike data is proven by bytes' hardlink \
        'd=$(mktemp -d "$1/hardlink-unlike.XXXXXX"); printf abcd >"$d/a"; printf abce >"$d/b"; touch -r "$d/a" "$d/b"; "$TOOL" -q "$d"; [ "$(stat -c %i "$d/a")" != "$(stat -c %i "$d/b")" ] && echo separate' \
        sh "$work"
compare 'minimum size excludes candidate' hardlink \
        'd=$(mktemp -d "$1/hardlink-min.XXXXXX"); printf abc >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -q -s4 "$d"; [ "$(stat -c %i "$d/a")" != "$(stat -c %i "$d/b")" ] && echo separate' \
        sh "$work"
compare 'maximum size includes boundary' hardlink \
        'd=$(mktemp -d "$1/hardlink-max.XXXXXX"); printf abc >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -q -S3 "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'zero files require explicit minimum' hardlink \
        'd=$(mktemp -d "$1/hardlink-zero.XXXXXX"); : >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -q -s0 "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'respect name keeps differently named files' hardlink \
        'd=$(mktemp -d "$1/hardlink-name.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -q -f "$d"; [ "$(stat -c %i "$d/a")" != "$(stat -c %i "$d/b")" ] && echo separate' \
        sh "$work"
compare 'symlinks are not candidates' hardlink \
        'd=$(mktemp -d "$1/hardlink-link.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; ln -s a "$d/link"; "$TOOL" -q "$d"; [ -L "$d/link" ] && [ "$(readlink "$d/link")" = a ] && echo symlink' \
        sh "$work"
compare 'sparse duplicate content' hardlink \
        'd=$(mktemp -d "$1/hardlink-sparse.XXXXXX"); truncate -s 8M "$d/a"; printf tail | dd of="$d/a" bs=1 seek=8388604 conv=notrunc status=none; cp --sparse=always -p "$d/a" "$d/b"; "$TOOL" -q "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'already linked files stay valid' hardlink \
        'd=$(mktemp -d "$1/hardlink-existing.XXXXXX"); printf same >"$d/a"; ln "$d/a" "$d/alias"; cp -p "$d/a" "$d/b"; before=$(stat -c %i "$d/a"); "$TOOL" -q "$d"; [ "$before" = "$(stat -c %i "$d/alias")" ] && [ "$before" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'reflink never selects hardlinks' hardlink \
        'd=$(mktemp -d "$1/hardlink-reflink-never.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -q --reflink=never "$d"; [ "$(stat -c %i "$d/a")" = "$(stat -c %i "$d/b")" ] && echo linked' \
        sh "$work"
compare 'bounded io-size consolidates a whole group' hardlink \
        'd=$(mktemp -d "$1/hardlink-io.XXXXXX"); printf "nonempty payload\n" >"$d/a"; i=1; while [ "$i" -le 7 ]; do cp -p "$d/a" "$d/$i"; i=$((i+1)); done; "$TOOL" -q -b1 "$d"; [ "$(find "$d" -type f -printf "%i\n" | sort -u | wc -l)" -eq 1 ] && [ -z "$(find "$d" -name "*.moonwater-hardlink-*" -print -quit)" ] && echo consolidated' \
        sh "$work"
compare 'list mode reports both duplicates' hardlink \
        'd=$(mktemp -d "$1/hardlink-list.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; "$TOOL" -l "$d" | cut -f2 | sort | wc -l' \
        sh "$work"
subject 'missing top-level operand is an error' hardlink \
        '! "$TOOL" -q "$1/hardlink-definitely-absent"' sh "$work"
subject 'unsupported reflink mutation is rejected' hardlink \
        'd=$(mktemp -d "$1/hardlink-reflink.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; ! "$TOOL" -q --reflink=auto "$d"' \
        sh "$work"
subject 'cross-filesystem candidates are not linked' hardlink \
        'd=$(mktemp -d "$1/hardlink-xdev.XXXXXX"); e=$(mktemp -d /dev/shm/hardlink-xdev.XXXXXX); trap '\''rm -rf "$e"'\'' EXIT; printf same >"$d/a"; cp -p "$d/a" "$e/b"; "$TOOL" -q "$d" "$e"; [ "$(stat -c %d "$d/a")" != "$(stat -c %d "$e/b")" ]' \
        sh "$work"
subject 'atomic replacement tolerates name races' hardlink \
        'd=$(mktemp -d "$1/hardlink-race.XXXXXX"); printf same >"$d/a"; cp -p "$d/a" "$d/b"; (i=0; while [ "$i" -lt 300 ]; do printf racing >"$d/new"; mv -f "$d/new" "$d/b"; cp -p "$d/a" "$d/new"; mv -f "$d/new" "$d/b"; i=$((i+1)); done) & racer=$!; i=0; while [ "$i" -lt 30 ]; do "$TOOL" -cq "$d" >/dev/null 2>&1 || :; i=$((i+1)); done; wait "$racer"; [ "$(cat "$d/a")" = same ] && [ -f "$d/b" ]' \
        sh "$work"
subject 'truncate and ctime races never signal' hardlink \
        'd=$(mktemp -d "$1/hardlink-truncate.XXXXXX"); dd if=/dev/zero of="$d/a" bs=1M count=16 status=none; cp -p "$d/a" "$d/b"; (i=0; while [ "$i" -lt 400 ]; do truncate -s 8M "$d/b"; chmod 600 "$d/b"; truncate -s 16M "$d/b"; chmod 644 "$d/b"; i=$((i+1)); done) & racer=$!; "$TOOL" -qn -b4096 "$d" >/dev/null 2>&1; result=$?; wait "$racer"; [ "$result" -eq 0 ] || [ "$result" -eq 1 ]' \
        sh "$work"
subject 'unreadable traversal is not partial success' hardlink \
        'd=$(mktemp -d "$1/hardlink-unreadable.XXXXXX"); mkdir "$d/closed"; printf same >"$d/closed/a"; chmod 000 "$d/closed"; if [ "$(id -u)" -eq 0 ]; then chmod 700 "$d/closed"; exit 0; fi; "$TOOL" -q "$d" >/dev/null 2>&1; result=$?; chmod 700 "$d/closed"; [ "$result" -ne 0 ]' \
        sh "$work"
subject 'overlapping oversized tree is indexed once' hardlink \
        'd=$(mktemp -d "$1/hardlink-many.XXXXXX"); i=0; while [ "$i" -lt 2000 ]; do printf x >"$d/f$i"; i=$((i+1)); done; lines=$(timeout 10 "$TOOL" -l -b4096 "$d" "$d" | wc -l) || exit; [ "$lines" -eq 2000 ]' \
        sh "$work"

section nologin
compare 'default refusal' nologin '"$TOOL"'
compare 'su command compatibility is ignored' nologin '"$TOOL" -c ignored'
subject 'help still refuses the login' nologin \
        '"$TOOL" -h >/dev/null 2>&1; [ "$?" -eq 1 ]'
subject 'missing compatibility command is rejected' nologin \
        '"$TOOL" -c >/dev/null 2>&1; [ "$?" -eq 1 ]'

section whereis
whereis_tree="$work/whereis-tree"
mkdir -p "$whereis_tree/bin" "$whereis_tree/man" "$whereis_tree/src"
touch "$whereis_tree/bin/foo" "$whereis_tree/bin/single" \
      "$whereis_tree/man/foo.1" "$whereis_tree/man/foo.1.gz" \
      "$whereis_tree/man/foo.info.xz" "$whereis_tree/man/foo.conf.5" \
      "$whereis_tree/src/foo.c" "$whereis_tree/src/foo.h" \
      "$whereis_tree/src/s.foo.c" "$whereis_tree/src/foo.c.gz"
ln -s "$whereis_tree/bin" "$whereis_tree/bin-alias"

compare 'custom binary path' whereis \
        '"$TOOL" -b -B "$1/bin" -f foo single absent' sh "$whereis_tree"
compare 'custom manual suffix and compression matching' whereis \
        '"$TOOL" -m -M "$1/man" -f foo' sh "$whereis_tree"
compare 'custom source and SCCS matching' whereis \
        '"$TOOL" -s -S "$1/src" -f foo' sh "$whereis_tree"
compare 'combined custom categories' whereis \
        '"$TOOL" -B "$1/bin" -M "$1/man" -S "$1/src" -f foo' sh "$whereis_tree"
compare 'glob applies to raw entry names' whereis \
        '"$TOOL" -g -s -S "$1/src" -f "*foo*"' sh "$whereis_tree"
compare 'unusual suppresses zero and singleton results' whereis \
        '"$TOOL" -u -b -B "$1/bin" -f foo single absent' sh "$whereis_tree"
compare 'directory identity is deduplicated' whereis \
        '"$TOOL" -b -B "$1/bin" "$1/bin-alias" -f foo' sh "$whereis_tree"
compare 'missing search directories are quiet' whereis \
        '"$TOOL" -b -B "$1/missing" "$1/bin" -f foo' sh "$whereis_tree"
compare 'unsupported long category option is rejected' whereis \
        '"$TOOL" --binary foo' sh "$whereis_tree"
subject 'list emits canonical existing directories' whereis \
        '"$TOOL" -B "$1/bin" -l 2>/dev/null | grep -qx "bin: $1/bin"' sh "$whereis_tree"

# Exact upstream executable denominator. The supported list is intentionally
# separate: every upstream name must be in exactly one side, and implementing
# a remaining name makes this fail until the capability claim is moved.
upstream='addpart agetty bits blkdiscard blkid blkpr blkzone blockdev cal cfdisk chcpu chfn chmem choom chrt chsh col colcrt colrm column copyfilerange coresched ctrlaltdel delpart dmesg eject enosys exch fadvise fallocate fdisk fincore findfs findmnt flock fsck fsck.cramfs fsck.minix fsfreeze fstrim getino getopt hardlink hexdump hwclock ionice ipcmk ipcrm ipcs irqtop isosize kill last lastlog2 ldattach line logger login look losetup lsblk lsclocks lscpu lsfd lsipc lsirq lslocks lslogins lsmem lsns mcookie mesg mkfs mkfs.bfs mkfs.cramfs mkfs.minix mkswap more mount mountpoint namei newgrp nologin nsenter partx pg pipesz pivot_root prlimit readprofile rename renice resizepart rev rfkill rtcwake runuser script scriptlive scriptreplay setarch setpgid setpriv setsid setterm sfdisk su sulogin swaplabel swapoff swapon switch_root taskset tunelp uclampset ul umount unshare utmpdump uuidd uuidgen uuidparse vipw waitpid wall wdctl whereis wipefs write zramctl'
supported='addpart bits blkid blockdev cal choom chrt col colcrt colrm column copyfilerange coresched ctrlaltdel delpart dmesg exch fadvise fallocate fincore findfs findmnt flock getino getopt hardlink hexdump ionice ipcmk ipcrm ipcs isosize kill last line logger look lsblk lsclocks lscpu lsfd lsipc lslocks lsmem lsns mcookie mesg mkswap mount mountpoint namei nologin nsenter pipesz pivot_root prlimit rename renice resizepart rev rfkill script scriptreplay setarch setpgid setpriv setsid swaplabel taskset uclampset ul umount unshare utmpdump uuidgen uuidparse waitpid wall whereis wipefs write'

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
