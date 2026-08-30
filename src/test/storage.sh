#!/bin/sh
#
#       The common util-linux storage surface, by identity and by effect.
#
#           sh src/test/storage.sh [multicall farm] [shell]
#
#       Block identities are compared field by field because util-linux also
#       reports optional fields Moonwater does not promise, such as ext4's
#       block size. Mounts run in a private user/mount namespace: the test
#       changes a real kernel mount table without needing root and without
#       touching the machine running the suite.
#
set -u

farm=${1:-/tmp/mwfarm}
shell=${2:-$farm/shell}
work=$(mktemp -d "${TMPDIR:-/tmp}/moonwater-storage.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

good()
{
        pass=$((pass + 1))
}

bad()
{
        fail=$((fail + 1))
        printf '  storage  %-30s %s\n' "$1" "$2"
}

same_file()
{
        if cmp -s "$2" "$3"; then
                good
        else
                bad "$1" "want [$(head -c 80 "$2" | tr '\n' '|')] got [$(head -c 80 "$3" | tr '\n' '|')]"
        fi
}

same_status()
{
        if [ "$2" = "$3" ]; then
                good
        else
                bad "$1" "want status $2, got $3"
        fi
}

probe_fields()
{
        name=$1
        image=$2
        shift 2

        for field in "$@"; do
                blkid -s "$field" -o value "$image" > "$work/want"
                "$farm/blkid" -s "$field" -o value "$image" > "$work/got"
                same_file "$name $field" "$work/want" "$work/got"
        done
}

probe_refused()
{
        name=$1
        image=$2

        if "$farm/blkid" "$image" > /dev/null 2>&1; then
                got_status=0
        else
                got_status=$?
        fi
        same_status "$name magic-only refused" 2 "$got_status"
}

for command in mount umount mountpoint blkid findmnt findfs; do
        if "$shell" -c "type $command" > "$work/type" 2>/dev/null &&
           grep -q "$command is a shell builtin" "$work/type"; then
                good
        else
                bad "$command builtin" "$(tr '\n' '|' < "$work/type")"
        fi
done

# Builtins and their conventional executable aliases are two dispatch paths
# into one core.  Keep representative read-only output, status, and diagnostic
# behavior byte-for-byte identical so neither wrapper acquires policy.
"$farm/mount" -t proc > "$work/direct"
"$shell" -c 'mount -t proc' > "$work/builtin"
same_file "mount builtin/direct" "$work/direct" "$work/builtin"

# No-argument flags are freely clusterable. Compare each implementation with
# its own unclustered listing because util-linux and Moonwater intentionally
# differ in how they combine VFS and filesystem option columns.
mount > "$work/system-mount-plain"
mount -vn > "$work/system-mount-cluster"
same_file "system mount -vn" "$work/system-mount-plain" \
        "$work/system-mount-cluster"
"$farm/mount" > "$work/moon-mount-plain"
"$farm/mount" -vn > "$work/moon-mount-cluster"
same_file "moon mount -vn" "$work/moon-mount-plain" \
        "$work/moon-mount-cluster"

"$farm/findmnt" -n -r -o TARGET,FSTYPE -T / > "$work/direct"
"$shell" -c 'findmnt -n -r -o TARGET,FSTYPE -T /' > "$work/builtin"
same_file "findmnt builtin/direct" "$work/direct" "$work/builtin"

if "$farm/mountpoint" -d / > "$work/direct"; then
        direct_status=0
else
        direct_status=$?
fi
if "$shell" -c 'mountpoint -d /' > "$work/builtin"; then
        builtin_status=0
else
        builtin_status=$?
fi
same_status "mountpoint builtin status" "$direct_status" "$builtin_status"
same_file "mountpoint builtin output" "$work/direct" "$work/builtin"

missing_uuid=00000000-0000-0000-0000-000000000000
if "$farm/findfs" "UUID=$missing_uuid" > "$work/direct" 2> "$work/direct.err"; then
        direct_status=0
else
        direct_status=$?
fi
if "$shell" -c 'findfs "UUID=$1"' storage "$missing_uuid" \
        > "$work/builtin" 2> "$work/builtin.err"; then
        builtin_status=0
else
        builtin_status=$?
fi
same_status "findfs builtin status" "$direct_status" "$builtin_status"
same_file "findfs builtin output" "$work/direct" "$work/builtin"

missing_mount="$work/not-a-mount"
if "$farm/umount" "$missing_mount" > "$work/direct" 2> "$work/direct.err"; then
        direct_status=0
else
        direct_status=$?
fi
if "$shell" -c 'umount "$1"' storage "$missing_mount" \
        > "$work/builtin" 2> "$work/builtin.err"; then
        builtin_status=0
else
        builtin_status=$?
fi
same_status "umount builtin status" "$direct_status" "$builtin_status"
same_file "umount builtin diagnostic" "$work/direct.err" "$work/builtin.err"

if ! command -v mkfs.ext4 >/dev/null 2>&1 ||
   ! command -v mkswap >/dev/null 2>&1 ||
   ! command -v blkid >/dev/null 2>&1; then
        echo "  storage  block probes NOT RUN -- mkfs.ext4, mkswap and blkid are required"
        exit 2
fi

truncate -s 16M "$work/ext4.img"
mkfs.ext4 -q -F -L moondata \
        -U 12345678-1234-5678-9abc-def012345678 "$work/ext4.img"

for field in TYPE UUID LABEL; do
        blkid -s "$field" -o value "$work/ext4.img" > "$work/want"
        "$farm/blkid" -s "$field" -o value "$work/ext4.img" > "$work/got"
        same_file "ext4 $field" "$work/want" "$work/got"
done

"$farm/blkid" -s UUID -o value "$work/ext4.img" > "$work/direct"
"$shell" -c 'blkid -s UUID -o value "$1"' storage "$work/ext4.img" \
        > "$work/builtin"
same_file "blkid builtin/direct" "$work/direct" "$work/builtin"

blkid "$work/ext4.img" -s UUID -o value > "$work/want"
"$farm/blkid" "$work/ext4.img" -s UUID -o value > "$work/got"
same_file "blkid options after device" "$work/want" "$work/got"

blkid --match-tag=UUID --output=value "$work/ext4.img" > "$work/want"
"$farm/blkid" --match-tag=UUID --output=value "$work/ext4.img" > "$work/got"
same_file "blkid long equals options" "$work/want" "$work/got"

truncate -s 16M "$work/swap.img"
mkswap -q -L moonswap \
        -U 87654321-4321-8765-cba9-876543210fed "$work/swap.img"

for field in TYPE UUID LABEL; do
        blkid -s "$field" -o value "$work/swap.img" > "$work/want"
        "$farm/blkid" -s "$field" -o value "$work/swap.img" > "$work/got"
        same_file "swap $field" "$work/want" "$work/got"
done

truncate -s 511 "$work/short.img"
if "$farm/blkid" "$work/short.img" > /dev/null 2>&1; then
        got_status=0
else
        got_status=$?
fi
same_status "short image refused" 2 "$got_status"

printf '\377\377\377\377\377\377\377\377' > "$work/hostile.img"
if "$farm/blkid" "$work/hostile.img" > /dev/null 2>&1; then
        got_status=0
else
        got_status=$?
fi
same_status "hostile image refused" 2 "$got_status"

# An OEM word alone is not an NTFS boot sector.  This used to be accepted as
# TYPE=ntfs despite having no geometry, volume size, MFT, or boot signature.
truncate -s 512 "$work/ntfs-magic.img"
printf '\353' | dd of="$work/ntfs-magic.img" bs=1 conv=notrunc \
        >/dev/null 2>&1
printf 'NTFS    ' | dd of="$work/ntfs-magic.img" bs=1 seek=3 conv=notrunc \
        >/dev/null 2>&1
printf '\125\252' | dd of="$work/ntfs-magic.img" bs=1 seek=510 conv=notrunc \
        >/dev/null 2>&1
if "$farm/blkid" "$work/ntfs-magic.img" > /dev/null 2>&1; then
        got_status=0
else
        got_status=$?
fi
same_status "ntfs magic-only refused" 2 "$got_status"

# Likewise, Btrfs's magic has to live in a self-consistent primary
# superblock; eight attacker-controlled bytes at 64 KiB are not a filesystem.
truncate -s 69632 "$work/btrfs-magic.img"
printf '_BHRfS_M' | dd of="$work/btrfs-magic.img" bs=1 seek=65600 conv=notrunc \
        >/dev/null 2>&1
if "$farm/blkid" "$work/btrfs-magic.img" > /dev/null 2>&1; then
        got_status=0
else
        got_status=$?
fi
same_status "btrfs magic-only refused" 2 "$got_status"

# Optional formatters exercise the common server filesystems without making
# the whole storage lane depend on every mkfs package.  Each real image is
# compared field-for-field with util-linux; the tiny companion carries only
# the public magic and must not be trusted as a filesystem.
if command -v mkfs.xfs >/dev/null 2>&1; then
        truncate -s 400M "$work/xfs.img"
        if mkfs.xfs -f -L moonxfs \
                -m uuid=11111111-2222-3333-4444-555555555555 \
                "$work/xfs.img" >/dev/null 2>&1; then
                probe_fields xfs "$work/xfs.img" TYPE UUID LABEL
        fi
fi
truncate -s 4096 "$work/xfs-magic.img"
printf XFSB | dd of="$work/xfs-magic.img" conv=notrunc >/dev/null 2>&1
probe_refused xfs "$work/xfs-magic.img"

if command -v mkfs.f2fs >/dev/null 2>&1; then
        truncate -s 64M "$work/f2fs.img"
        if mkfs.f2fs -f -l moonf2fs \
                -U 22222222-3333-4444-5555-666666666666 \
                "$work/f2fs.img" >/dev/null 2>&1; then
                probe_fields f2fs "$work/f2fs.img" TYPE UUID LABEL
        fi
fi
truncate -s 4096 "$work/f2fs-magic.img"
printf '\020\040\365\362' | dd of="$work/f2fs-magic.img" bs=1 \
        seek=1024 conv=notrunc >/dev/null 2>&1
probe_refused f2fs "$work/f2fs-magic.img"

if command -v mksquashfs >/dev/null 2>&1; then
        mkdir "$work/squash-root"
        printf moon > "$work/squash-root/file"
        if mksquashfs "$work/squash-root" "$work/squashfs.img" \
                -noappend -quiet >/dev/null 2>&1; then
                probe_fields squashfs "$work/squashfs.img" TYPE
        fi
fi
truncate -s 4096 "$work/squashfs-magic.img"
printf hsqs | dd of="$work/squashfs-magic.img" conv=notrunc \
        >/dev/null 2>&1
probe_refused squashfs "$work/squashfs-magic.img"

if command -v mkudffs >/dev/null 2>&1; then
        truncate -s 32M "$work/udf.img"
        if mkudffs --utf8 --label=moonudf "$work/udf.img" \
                >/dev/null 2>&1; then
                probe_fields udf "$work/udf.img" TYPE UUID LABEL
        fi
fi
truncate -s 65536 "$work/udf-magic.img"
for descriptor in '32768 BEA01' '34816 NSR03' '36864 TEA01'; do
        set -- $descriptor
        printf '\000%s\001' "$2" | dd of="$work/udf-magic.img" bs=1 \
                seek="$1" conv=notrunc >/dev/null 2>&1
done
probe_refused udf "$work/udf-magic.img"

if command -v cryptsetup >/dev/null 2>&1; then
        printf secret > "$work/luks.key"
        truncate -s 32M "$work/luks1.img"
        if cryptsetup luksFormat --type luks1 --batch-mode \
                --uuid 33333333-4444-5555-6666-777777777777 \
                --key-file "$work/luks.key" "$work/luks1.img" \
                >/dev/null 2>&1; then
                probe_fields luks1 "$work/luks1.img" TYPE UUID
        fi

        truncate -s 32M "$work/luks2.img"
        if cryptsetup luksFormat --type luks2 --batch-mode --label moonluks \
                --uuid 44444444-5555-6666-7777-888888888888 \
                --key-file "$work/luks.key" "$work/luks2.img" \
                >/dev/null 2>&1; then
                probe_fields luks2 "$work/luks2.img" TYPE UUID LABEL
        fi
fi
truncate -s 4096 "$work/luks-magic.img"
printf 'LUKS\272\276\000\001' | dd of="$work/luks-magic.img" \
        conv=notrunc >/dev/null 2>&1
probe_refused luks "$work/luks-magic.img"

if mkswap --help 2>&1 | grep -q -- '--pagesize'; then
        truncate -s 16M "$work/swap64k.img"
        mkswap -q --pagesize 65536 -L moon64k \
                -U 11223344-5566-7788-99aa-bbccddeeff00 \
                "$work/swap64k.img"
        for field in TYPE UUID LABEL; do
                blkid -s "$field" -o value "$work/swap64k.img" > "$work/want"
                "$farm/blkid" -s "$field" -o value "$work/swap64k.img" \
                        > "$work/got"
                same_file "swap64k $field" "$work/want" "$work/got"
        done
fi

# PARTUUID and GPT PARTLABEL are properties of a partition device, not of the
# filesystem inside it. Exercise the kernel's GPT CRC-validated and DOS table
# parsers through disposable loop devices when this runner has that authority.
if [ "$(uname -s)" = Linux ] && command -v sfdisk >/dev/null 2>&1 &&
   command -v losetup >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
        truncate -s 64M "$work/gpt.img"
        printf '%s\n' \
                'label: gpt' \
                'label-id: 11111111-2222-3333-4444-555555555555' \
                'unit: sectors' \
                '' \
                'start=2048, size=32768, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE, name="moon data"' |
                sfdisk "$work/gpt.img" >/dev/null

        truncate -s 64M "$work/mbr.img"
        printf '%s\n' \
                'label: dos' \
                'label-id: 0x1234abcd' \
                'unit: sectors' \
                '' \
                'start=2048, size=32768, type=83' |
                sfdisk "$work/mbr.img" >/dev/null

        if (
                gpt_loop=
                mbr_loop=
                cleanup_loops()
                {
                        [ -z "$gpt_loop" ] || sudo losetup -d "$gpt_loop"
                        [ -z "$mbr_loop" ] || sudo losetup -d "$mbr_loop"
                }
                trap cleanup_loops EXIT INT TERM

                gpt_loop=$(sudo losetup --find --show --partscan "$work/gpt.img")
                mbr_loop=$(sudo losetup --find --show --partscan "$work/mbr.img")
                gpt_part=${gpt_loop}p1
                mbr_part=${mbr_loop}p1

                for field in PARTUUID PARTLABEL; do
                        sudo blkid -s "$field" -o value "$gpt_part" > "$work/want"
                        sudo "$farm/blkid" -s "$field" -o value "$gpt_part" \
                                > "$work/got"
                        cmp -s "$work/want" "$work/got"
                done

                sudo blkid -s PARTUUID -o value "$mbr_part" > "$work/want"
                sudo "$farm/blkid" -s PARTUUID -o value "$mbr_part" > "$work/got"
                cmp -s "$work/want" "$work/got"

                [ "$(sudo "$shell" -c \
                        'findfs PARTUUID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee')" = \
                  "$gpt_part" ]
                [ "$(sudo "$shell" -c 'findfs PARTLABEL=moon\ data')" = \
                  "$gpt_part" ]

                if command -v unshare >/dev/null 2>&1; then
                        mkdir "$work/part-mount" "$work/system-part-mount"
                        sudo mkfs.ext4 -q -F -L moonpart \
                                -U 01234567-89ab-cdef-0123-456789abcdef \
                                "$gpt_part"

                        # Compare the identity selected by util-linux and by
                        # Moonwater, with both mutations confined to fresh
                        # mount namespaces.
                        sudo unshare -m sh -c '
                                set -e
                                mount -L moonpart "$1"
                                findmnt -n -r -o SOURCE,FSTYPE -T "$1"
                                umount "$1"
                        ' storage "$work/system-part-mount" > "$work/want-mount"
                        sudo unshare -m "$shell" -c '
                                set -e
                                mount -L moonpart "$1"
                                findmnt -n -r -o SOURCE,FSTYPE -T "$1"
                                umount "$1"
                        ' storage "$work/part-mount" > "$work/got-mount"
                        cmp -s "$work/want-mount" "$work/got-mount"

                        # All common short/long source spellings and the
                        # source/target long aliases reach the same resolver.
                        sudo unshare -m sh -c '
                                set -e
                                farm=$1; target=$2
                                "$farm/mount" -U \
                                  01234567-89ab-cdef-0123-456789abcdef "$target"
                                "$farm/mountpoint" -q "$target"
                                "$farm/umount" -c -i "$target"
                                "$farm/mount" --uuid=01234567-89ab-cdef-0123-456789abcdef "$target"
                                "$farm/umount" --no-canonicalize \
                                  --internal-only "$target"
                                "$farm/mount" --label moonpart "$target"
                                "$farm/umount" "$target"
                                "$farm/mount" --source LABEL=moonpart \
                                  --target "$target"
                                "$farm/umount" "$target"
                                if "$farm/umount" "$target" \
                                     2> "$3"; then
                                        exit 1
                                fi
                        ' storage "$farm" "$work/part-mount" \
                                "$work/umount-error"
                        grep -q 'Invalid argument' "$work/umount-error"
                        ! grep -Eq 'failed: -[0-9]+' "$work/umount-error"

                        sudo unshare -m "$shell" -c '
                                set -e
                                mount PARTUUID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee "$1"
                                mountpoint -q "$1"
                                umount "$1"
                                mount PARTLABEL=moon\ data "$1"
                                mountpoint -q "$1"
                                umount "$1"
                        ' storage "$work/part-mount"
                fi

                # -U is filesystem UUID lookup and must not silently become
                # PARTUUID lookup merely because the strings look alike.
                if sudo "$farm/blkid" -U \
                        aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee >/dev/null 2>&1; then
                        exit 1
                fi
        ); then
                good
        else
                bad "GPT/MBR partition identity" "loop-device differential failed"
        fi
else
        echo "  storage  partition identity NOT RUN -- sudo loop authority required"
fi

findmnt -n -r -o TARGET -T / > "$work/want"
"$farm/findmnt" -n -r -o TARGET -T / > "$work/got"
same_file "findmnt root target" "$work/want" "$work/got"

findmnt -n -r -o FSTYPE -T / > "$work/want"
"$farm/findmnt" -n -r -o FSTYPE -T / > "$work/got"
same_file "findmnt root type" "$work/want" "$work/got"

findmnt -n -r -o FSROOT,MAJ:MIN,ID,PARENT,VFS-OPTIONS,FS-OPTIONS -T / \
        > "$work/want"
"$farm/findmnt" -n -r \
        -o FSROOT,MAJ:MIN,ID,PARENT,VFS-OPTIONS,FS-OPTIONS -T / \
        > "$work/got"
same_file "findmnt identity columns" "$work/want" "$work/got"

findmnt --pairs --output TARGET,FSROOT,MAJ:MIN,ID --target / > "$work/want"
"$farm/findmnt" --pairs --output TARGET,FSROOT,MAJ:MIN,ID --target / \
        > "$work/got"
same_file "findmnt pairs/long options" "$work/want" "$work/got"

findmnt -n -r --output=TARGET,FSTYPE --target=/ > "$work/want"
"$farm/findmnt" -n -r --output=TARGET,FSTYPE --target=/ > "$work/got"
same_file "findmnt attached long values" "$work/want" "$work/got"

findmnt -n -r --first-only --types=proc --options=rw -o TARGET \
        > "$work/want"
"$farm/findmnt" -n -r --first-only --types=proc --options=rw -o TARGET \
        > "$work/got"
same_file "findmnt first/options filter" "$work/want" "$work/got"

findmnt -n -r -f -i -T / -o TARGET > "$work/want"
"$farm/findmnt" -n -r -f -i -T / -o TARGET > "$work/got"
same_file "findmnt inverted target" "$work/want" "$work/got"

# Empty values still carry getopt meaning.  In util-linux an empty type list
# selects no filesystem types, while an empty path query selects the root.
# Neither is the same as omitting its option.
if findmnt -n -r -o TARGET --types= > "$work/want"; then
        want_status=0
else
        want_status=$?
fi
if "$farm/findmnt" -n -r -o TARGET --types= > "$work/got"; then
        got_status=0
else
        got_status=$?
fi
same_status "findmnt empty types status" "$want_status" "$got_status"
same_file "findmnt empty types output" "$work/want" "$work/got"

findmnt -n -r -o TARGET --target= > "$work/want"
want_status=$?
"$farm/findmnt" -n -r -o TARGET --target= > "$work/got"
got_status=$?
same_status "findmnt empty target status" "$want_status" "$got_status"
same_file "findmnt empty target output" "$work/want" "$work/got"

findmnt -n -r -o TARGET '' > "$work/want"
want_status=$?
"$farm/findmnt" -n -r -o TARGET '' > "$work/got"
got_status=$?
same_status "findmnt empty operand status" "$want_status" "$got_status"
same_file "findmnt empty operand output" "$work/want" "$work/got"

findmnt / -n -r -o TARGET > "$work/want"
want_status=$?
"$farm/findmnt" / -n -r -o TARGET > "$work/got"
got_status=$?
same_status "findmnt options after path" "$want_status" "$got_status"
same_file "findmnt options after path output" "$work/want" "$work/got"

findmnt -n -r -o TARGET -M / > "$work/want"
want_status=$?
"$farm/findmnt" -n -r -o TARGET -M / > "$work/got"
got_status=$?
same_status "findmnt short mountpoint status" "$want_status" "$got_status"
same_file "findmnt short mountpoint output" "$work/want" "$work/got"

mount --types= > "$work/want"
want_status=$?
"$farm/mount" --types= > "$work/got"
got_status=$?
same_status "mount empty types status" "$want_status" "$got_status"
same_file "mount empty types output" "$work/want" "$work/got"

# findfs is also the conventional no-op source canonicalizer for paths with
# no TAG= prefix.  This is used by callers that accept either a device path or
# a UUID=/LABEL= expression.
findfs /dev/not-present > "$work/want"
want_status=$?
"$farm/findfs" /dev/not-present > "$work/got"
got_status=$?
same_status "findfs plain path status" "$want_status" "$got_status"
same_file "findfs plain path output" "$work/want" "$work/got"

findfs '' > "$work/want"
want_status=$?
"$farm/findfs" '' > "$work/got"
got_status=$?
same_status "findfs empty path status" "$want_status" "$got_status"
same_file "findfs empty path output" "$work/want" "$work/got"

mountpoint -q /
want_status=$?
"$farm/mountpoint" -q /
got_status=$?
same_status "root mountpoint" "$want_status" "$got_status"

mkdir "$work/not-mounted"
if mountpoint -q "$work/not-mounted"; then want_status=0; else want_status=$?; fi
if "$farm/mountpoint" -q "$work/not-mounted"; then got_status=0; else got_status=$?; fi
same_status "ordinary directory" "$want_status" "$got_status"

# Stay below Linux's PATH_MAX while crossing every small fixed-buffer size a
# parser might accidentally impose.  Mount-table lookup and stat should keep
# the complete valid path.
long_path=$work/long
mkdir "$long_path"
long_component=abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmn
while [ "${#long_path}" -lt 3000 ]; do
        long_path=$long_path/$long_component
        mkdir "$long_path"
done
findmnt -n -r -o TARGET -T "$long_path" > "$work/want"
want_status=$?
"$farm/findmnt" -n -r -o TARGET -T "$long_path" > "$work/got"
got_status=$?
same_status "findmnt long path status" "$want_status" "$got_status"
same_file "findmnt long path output" "$work/want" "$work/got"

if mountpoint -q "$long_path"; then want_status=0; else want_status=$?; fi
if "$farm/mountpoint" -q "$long_path"; then got_status=0; else got_status=$?; fi
same_status "mountpoint long path" "$want_status" "$got_status"

long_types=proc
while [ "${#long_types}" -lt 3000 ]; do
        long_types=$long_types,proc
done
findmnt -n -r -o TARGET --mountpoint /proc --types "$long_types" \
        > "$work/want"
want_status=$?
"$farm/findmnt" -n -r -o TARGET --mountpoint /proc --types "$long_types" \
        > "$work/got"
got_status=$?
same_status "findmnt long option status" "$want_status" "$got_status"
same_file "findmnt long option output" "$work/want" "$work/got"

if mountpoint -q ''; then want_status=0; else want_status=$?; fi
if "$farm/mountpoint" -q ''; then got_status=0; else got_status=$?; fi
same_status "mountpoint empty path" "$want_status" "$got_status"

mountpoint -d / > "$work/want"
want_status=$?
"$farm/mountpoint" -d / > "$work/got"
got_status=$?
same_status "mountpoint fs device status" "$want_status" "$got_status"
same_file "mountpoint fs device" "$work/want" "$work/got"

ln -s / "$work/root-link"
if mountpoint --nofollow -q "$work/root-link"; then want_status=0; else want_status=$?; fi
if "$farm/mountpoint" --nofollow -q "$work/root-link"; then got_status=0; else got_status=$?; fi
same_status "mountpoint nofollow" "$want_status" "$got_status"

if mountpoint -q / "$work/not-mounted" "$work/root-link" 2>/dev/null; then
        want_status=0
else
        want_status=$?
fi
if "$farm/mountpoint" -q / "$work/not-mounted" "$work/root-link" 2>/dev/null; then
        got_status=0
else
        got_status=$?
fi
same_status "mountpoint extra paths" "$want_status" "$got_status"

block_device=""
for candidate in /sys/class/block/*; do
        [ -e "$candidate" ] || continue
        candidate=/dev/$(basename "$candidate")
        if [ -b "$candidate" ]; then
                block_device=$candidate
                break
        fi
done

if [ -n "$block_device" ]; then
        mountpoint -x "$block_device" > "$work/want"
        want_status=$?
        "$farm/mountpoint" -x "$block_device" > "$work/got"
        got_status=$?
        same_status "mountpoint block device status" "$want_status" "$got_status"
        same_file "mountpoint block device" "$work/want" "$work/got"

        mountpoint -dx "$block_device" > "$work/want"
        "$farm/mountpoint" -dx "$block_device" > "$work/got"
        same_file "mountpoint devno precedence" "$work/want" "$work/got"
else
        echo "  storage  mountpoint -x NOT RUN -- no block device node"
fi

if [ "$(uname -s)" = Linux ] && command -v unshare >/dev/null 2>&1; then
        mkdir -p "$work/ns/a" "$work/ns/b" "$work/ns/c" \
                 "$work/ns/fstab target" "$work/ns/noauto" \
                 "$work/ns/bad-before" "$work/ns/bad-after" \
                 "$work/ns/cluster-system" "$work/ns/cluster-moon"
        fstab_target=$(printf '%s' "$work/ns/fstab target" | sed 's/ /\\040/g')
        {
                printf 'tmpfs %s tmpfs nodev,nosuid 0 0\n' "$fstab_target"
                printf 'tmpfs %s tmpfs noauto 0 0\n' "$work/ns/noauto"
        } > "$work/fstab"
        {
                printf 'tmpfs %s tmpfs defaults 0 0\n' "$work/ns/bad-before"
                printf 'malformed\n'
                printf 'tmpfs %s tmpfs defaults 0 0\n' "$work/ns/bad-after"
        } > "$work/fstab-malformed"

        if unshare -Urnm sh -c '
                set -e
                farm=$1; work=$2
                "$farm/mount" -t tmpfs -o nodev,nosuid tmpfs "$work/ns/a"
                "$farm/mountpoint" -q "$work/ns/a"
                [ "$("$farm/findmnt" -n -r -o FSTYPE -T "$work/ns/a")" = tmpfs ]
                findmnt -n -r -o OPTIONS,VFS-OPTIONS,FS-OPTIONS \
                        -T "$work/ns/a" > "$work/want-options"
                "$farm/findmnt" -n -r -o OPTIONS,VFS-OPTIONS,FS-OPTIONS \
                        -T "$work/ns/a" > "$work/got-options"
                cmp "$work/want-options" "$work/got-options"
                mkdir "$work/ns/a/sub"
                "$farm/mount" --bind "$work/ns/a/sub" "$work/ns/b"
                "$farm/mountpoint" -q "$work/ns/b"
                findmnt -n -r -o SOURCE,FSROOT,MAJ:MIN,ID,PARENT \
                        -T "$work/ns/b" > "$work/want-bind"
                "$farm/findmnt" -n -r -o SOURCE,FSROOT,MAJ:MIN,ID,PARENT \
                        -T "$work/ns/b" > "$work/got-bind"
                cmp "$work/want-bind" "$work/got-bind"
                findmnt -n -r -v -o SOURCE -T "$work/ns/b" \
                        > "$work/want-nofsroot"
                "$farm/findmnt" -n -r -v -o SOURCE -T "$work/ns/b" \
                        > "$work/got-nofsroot"
                cmp "$work/want-nofsroot" "$work/got-nofsroot"
                "$farm/umount" "$work/ns/b"
                "$farm/umount" "$work/ns/a"
                ! "$farm/mountpoint" -q "$work/ns/a"
        ' storage "$farm" "$work"; then
                good
        else
                bad "direct mount lifecycle" "private namespace failed"
        fi

        if unshare -Urnm sh -c '
                set -e
                farm=$1; work=$2
                system=$work/ns/cluster-system
                moon=$work/ns/cluster-moon

                mount -t tmpfs tmpfs "$system"
                umount -lv "$system" >/dev/null
                ! mountpoint -q "$system"
                "$farm/mount" -t tmpfs tmpfs "$moon"
                "$farm/umount" -lv "$moon" >/dev/null
                ! "$farm/mountpoint" -q "$moon"

                mount -t tmpfs tmpfs "$system"
                umount -vl "$system" >/dev/null
                "$farm/mount" -t tmpfs tmpfs "$moon"
                "$farm/umount" -vl "$moon" >/dev/null

                mount -t tmpfs tmpfs "$system"
                umount -nvi "$system" >/dev/null
                "$farm/mount" -t tmpfs tmpfs "$moon"
                "$farm/umount" -nvi "$moon" >/dev/null

                # t takes the rest of its cluster policy-wise, or the next
                # word here, after the preceding no-argument flags.
                mount -t tmpfs tmpfs "$system"
                umount -lvt tmpfs "$system" >/dev/null
                "$farm/mount" -t tmpfs tmpfs "$moon"
                "$farm/umount" -lvt tmpfs "$moon" >/dev/null
                ! mountpoint -q "$system"
                ! "$farm/mountpoint" -q "$moon"
        ' storage "$farm" "$work"; then
                good
        else
                bad "mount short option clusters" \
                    "util-linux/Moonwater namespace effects differ"
        fi

        if unshare -Urnm sh -c '
                set -e
                farm=$1; work=$2
                set +e
                mount -a -T "$work/fstab-malformed" >/dev/null 2>&1
                want=$?
                set -e
                mountpoint -q "$work/ns/bad-before"
                mountpoint -q "$work/ns/bad-after"
                umount "$work/ns/bad-before"
                umount "$work/ns/bad-after"

                set +e
                "$farm/mount" -a -T "$work/fstab-malformed" >/dev/null 2>&1
                got=$?
                set -e
                [ "$want" = "$got" ]
                "$farm/mountpoint" -q "$work/ns/bad-before"
                "$farm/mountpoint" -q "$work/ns/bad-after"
                "$farm/umount" "$work/ns/bad-before"
                "$farm/umount" "$work/ns/bad-after"
        ' storage "$farm" "$work"; then
                good
        else
                bad "fstab malformed recovery" "valid surrounding records lost"
        fi

        if unshare -Urnm "$shell" -c '
                set -e
                mount -t tmpfs tmpfs "$1/ns/c"
                mountpoint -q "$1/ns/c"
                [ "$(findmnt -n -r -o FSTYPE -T "$1/ns/c")" = tmpfs ]
                umount "$1/ns/c"
        ' storage "$work"; then
                good
        else
                bad "builtin mount lifecycle" "private namespace failed"
        fi

        if unshare -Urnm sh -c '
                set -e
                farm=$1; work=$2
                "$farm/mount" -a -T "$work/fstab"
                "$farm/mountpoint" -q "$work/ns/fstab target"
                ! "$farm/mountpoint" -q "$work/ns/noauto"
                "$farm/umount" "$work/ns/fstab target"
        ' storage "$farm" "$work"; then
                good
        else
                bad "fstab escaped/noauto" "private namespace failed"
        fi
else
        echo "  storage  mount mutation NOT RUN -- Linux unshare is required"
fi

total=$((pass + fail))
printf '  storage      %s of %s\n' "$pass" "$total"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'storage %s %s\n' "$pass" "$total" >> "$TEST_TALLY"
[ "$fail" = 0 ]
