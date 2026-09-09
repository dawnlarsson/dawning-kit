#!/bin/sh
#
#       Bootstrap for the build tool, and the half of it a Mac can run.
#
#       Usage:
#           sh build.sh                       build with the default profiles
#           sh build.sh arch/x64 debug_none   build with the profiles named
#           sh build.sh --run                 build, then boot it in a window
#           sh build.sh --run --shell         boot with the console on this terminal
#           sh build.sh --boot                boot the last image, do not rebuild
#           sh build.sh --usb                 build, then write a USB stick
#           sh build.sh --clean               remove what a build produced
#           sh build.sh --host box            build on another machine over ssh
#
#       The build itself is one C program, src/build/build.c, built on this
#       project's own freestanding stack. This file compiles it with one
#       command and hands over. That command is
#
#           cc -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
#              -fno-builtin -w -o build src/build/build.c
#
#       and it is the whole of what a bare machine needs: a C compiler and an
#       assembler, nothing linked and no library required. Run it yourself and
#       use ./build directly if you would rather not go through this file.
#
#       Why this file still exists, and why it still has shell in it below:
#       src/library.c is assembly wearing ELF clothes. ASM_FUNC emits .type
#       and .size and names symbols without a leading underscore, so the stack
#       does not assemble under Mach-O and the tool cannot be built on a Mac
#       at all. Everything a Mac actually does here -- drive a build on another
#       machine, boot the image it fetched back, write a stick, clean up --
#       needs none of the stack, so it stays here in shell rather than growing
#       a second implementation of the whole tool in C that no machine would
#       ever run. On Linux, which is where a kernel is built, nothing below the
#       exec runs.
#
# shellcheck disable=SC2154
# shellcheck disable=SC1091
set -e

# Sourced and compiled by this file's own path rather than a relative one, so
# that being in the wrong directory produces is_safe's explanation rather than
# a bare "No such file or directory". The working directory is deliberately
# left alone: every path a build reads is relative to it, and a test that
# builds a fixture tree elsewhere and runs this file from the repository
# depends on that.
# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "$(uname)" = "Linux" ]; then
        tool=$here/build
        source=$here/src/build/build.c

        # Rebuilt when the source is newer, so an edit to the tool is picked
        # up by the next build rather than at the next person's surprise. -nt
        # is not POSIX but every sh this targets -- dash, bash, ash, zsh -- has
        # it, and this file only runs on those.
        if [ ! -x "$tool" ] || [ "$source" -nt "$tool" ]; then
                ${CC:-cc} -O2 -static -nostdlib -nostartfiles \
                        -fno-stack-protector -fno-builtin -w \
                        -o "$tool" "$source" ||
                        {
                                echo "build.sh: could not build the build tool" >&2
                                exit 1
                        }
        fi

        exec "$tool" "$@"
fi

#
#       Everything below here is the Mac.
#
# shellcheck disable=SC1091
. "$here/kit/common"

is_safe

die() {
        echo "$RED""build failed: $*""$RESET" >&2
        exit 1
}

say() { printf '%s%s%s\n' "$CYAN$BOLD" "$*" "$RESET"; }

#
#       Arguments.
#
#       Anything that is not an option is a profile name, so the two can be
#       mixed in any order: sh build.sh --run desktop.
#
host=${MOONWATER_BUILD_HOST:-}
#
#       One build directory per source tree, not one per machine. The suffix
#       is a checksum of this tree's own path, so the same checkout always
#       gets the same directory and two checkouts never share one. The tool
#       computes the same number with the same checksum, so a directory made
#       by one is found by the other.
#
tree_mark=$(printf '%s' "$here" | cksum | cut -d' ' -f1)
remote=${MOONWATER_BUILD_DIR:-/tmp/moonwater-$(basename "$here")-$tree_mark}
do_run=0
do_build=1
do_clean=0
do_usb=0
console=0
image=""

remaining=$#
while [ "$remaining" -gt 0 ]; do
        argument=$1
        shift
        remaining=$((remaining - 1))
        case "$argument" in
        --clean) do_clean=1 ;;
        --run) do_run=1 ;;
        --boot) do_run=1; do_build=0 ;;
        --shell) console=1 ;;
        --usb) do_usb=1 ;;
        --host)
                [ "$remaining" -gt 0 ] || die "--host wants a machine to build on"
                host=$1
                shift
                remaining=$((remaining - 1))
                ;;
        --host=*) host=${argument#--host=} ;;
        --*) die "unknown option $argument" ;;
        *) set -- "$@" "$argument" ;;
        esac
done

#       Building somewhere else.
#
#       Optional, and only reached when a host was named. The remote command
#       carries no --host of its own and ssh does not forward the environment,
#       so the build over there is an ordinary local one and this cannot
#       recurse.
#
build_remote() {
        say "Checking $host"
        ssh -n -o BatchMode=yes -o ConnectTimeout=20 "$host" true 2>/dev/null ||
                die "cannot reach $host over ssh"

        say "Copying the tree to $host:$remote"
        remote_path=$(shell_quote "$remote")
        remote_rsync="mkdir -p -- $remote_path && cd -- $remote_path && rsync"

        # The kernel source, its artifacts and the built filesystem stay on
        # the build host: they are large, and none of them belong to this
        # checkout. linux/ is the upstream tree, not part of this repository.
        rsync -az --delete --rsync-path="$remote_rsync" \
                --exclude '.git' \
                --exclude '.claude' \
                --exclude 'linux' \
                --exclude 'artifacts' \
                --exclude 'fs' \
                --exclude 'dist' \
                ./ "$host:./" || die "copying the tree failed"

        say "Building on $host: $*"
        # -n so the build does not swallow this script's stdin. Without it the
        # USB prompts below read nothing, because ssh forwards whatever is on
        # stdin to the remote command.
        # shellcheck disable=SC2029,SC2086
        #
        #       sudo drops the environment, so anything the remote build has
        #       to know is named here. env rather than a VAR=value prefix,
        #       which sudo only passes when it has been configured to.
        #
        carry=""
        [ -z "${MOONWATER_STOCK:-}" ] || carry="MOONWATER_STOCK=1"

        # shellcheck disable=SC2029,SC2086
        ssh -n "$host" "cd -- $remote_path && sudo env $carry sh build.sh $(shell_quote "$@")" ||
                die "the build failed on $host"

        # The host which built the configured profile is authoritative about
        # its export.  A stale local artifacts/.config may describe another
        # architecture entirely (an ARM Mac commonly names kernel8.img).
        image=$(ssh -n "$host" \
                "cd -- $remote_path && . ./kit/common && key_one kernel_export") ||
                die "could not identify the built image"
        case "$image" in
        dist/*) ;;
        *) die "remote build reported an invalid image path: $image" ;;
        esac

        say "Fetching $image"
        mkdir -p "$(dirname "$image")"
        ssh -n "$host" "cd -- $remote_path && cat -- $(shell_quote "$image")" > "$image" ||
                die "could not fetch the built image"
}
build_local() {
        die "building a kernel wants a Linux toolchain and a case
sensitive filesystem, and this is $(uname). Name a machine that has them with
--host, or set MOONWATER_BUILD_HOST."
}

#
#       Removing what a build produced.
#
#       artifacts/ keeps the downloaded kernel tarball and is left alone on
#       purpose: throwing it away means fetching a hundred and fifty megabytes
#       again to get back where you were.
#
if [ "$do_clean" -eq 1 ]; then
        say "Removing build output"
        rm -rf dist fs linux \
                artifacts/merge.config artifacts/.config artifacts/info \
                artifacts/asm.applied artifacts/asm.arch artifacts/asm.requested
        rm -f src/*.a src/*.o src/*.o.d src/*.cmd src/*.order
        # The .S `build asm` generates from each .asm, which kbuild writes here
        # because src/ is the kernel tree's kernel/moonwater.
        rm -f src/*.S src/*.asm_tmp
        exit 0
fi

if [ "$do_build" -eq 1 ]; then
        if [ -n "$host" ]; then
                build_remote "$@"
        else
                build_local "$@"
        fi
fi

[ "$do_usb" -eq 1 ] || [ "$do_run" -eq 1 ] || exit 0

#
#       Where the image ended up. A remote build sets this from its own
#       generated configuration. A local build, or --boot without a build,
#       asks the local configuration and finally falls back to x86 EFI.
#
if [ -z "$image" ]; then
        image=$(key_one kernel_export 2>/dev/null || true)
fi
[ -n "$image" ] || image="dist/bootx64.efi"

[ -f "$image" ] ||
        die "no image at $image -- build one first, or drop --boot"

#
#       Writing to a USB stick.
#
#       The image is already an EFI application -- the kernel is built with the
#       EFI stub, which is why it is called bootx64.efi -- so firmware can load
#       it directly and there is no bootloader to install. It goes at the path
#       the UEFI spec reserves for removable media, \EFI\BOOT\BOOTX64.EFI,
#       which is what a machine looks for when told to boot from USB.
#
if [ "$do_usb" -eq 1 ]; then
        #
        #       On anything without diskutil this lists the candidates and
        #       prints the command rather than running it. Writing to a raw
        #       block device with the wrong name destroys the wrong disk, and
        #       the checks below that make that hard are diskutil's -- there
        #       is no honest way to claim the same care against an untested
        #       lsblk and dd, so the last step stays in your hands.
        #
        if ! command -v diskutil >/dev/null 2>&1; then
                say "Removable disks"
                if command -v lsblk >/dev/null 2>&1; then
                        lsblk -dno NAME,SIZE,RM,MODEL 2>/dev/null |
                                awk '$3 == 1 { printf "  /dev/%s  %s  %s\n", $1, $2, $4 }'
                else
                        echo "  (lsblk is missing; find the device yourself)"
                fi
                echo
                echo "Write it with, replacing sdX with the stick:"
                echo
                echo "  sudo mkfs.vfat -F32 /dev/sdX1        # after partitioning it GPT/ESP"
                echo "  sudo mount /dev/sdX1 /mnt"
                echo "  sudo mkdir -p /mnt/EFI/BOOT"
                echo "  sudo cp $image /mnt/EFI/BOOT/BOOTX64.EFI"
                echo "  sudo umount /mnt"
                echo
                echo "Check the device name twice. This erases whatever it names."
                exit 0
        fi

        say "Removable disks"

        # external and physical together exclude internal drives and disk
        # images, so nothing here can be the machine you are sitting at.
        disks=$(diskutil list external physical 2>/dev/null |
                awk '/^\/dev\/disk/ { print $1 }')

        [ -n "$disks" ] || die "no removable disk found -- plug the stick in first"

        index=0
        for disk in $disks; do
                index=$((index + 1))
                name=$(diskutil info "$disk" 2>/dev/null |
                        awk -F": *" '/Device \/ Media Name/ { print $2; exit }')
                size=$(diskutil info "$disk" 2>/dev/null |
                        awk -F": *" '/Disk Size/ { print $2; exit }')
                printf "  %d) %-12s %-28s %s\n" "$index" "$disk" "${name:-unknown}" "${size:-}"
        done

        printf "\nWhich one? (number, or anything else to stop) "
        read -r choice

        case "$choice" in
        ''|*[!0-9]*) die "nothing written" ;;
        esac

        target=$(echo "$disks" | sed -n "${choice}p")
        [ -n "$target" ] || die "no disk $choice in that list"

        # Ask about the chosen disk directly rather than trusting the listing:
        # the two are separate moments, and a mistake here erases the wrong
        # drive. Which field says so varies between macOS versions, so this
        # wants positive evidence from one of them and a contradiction from
        # none -- anything unrecognised is refused rather than assumed safe.
        info=$(diskutil info "$target" 2>/dev/null)
        location=$(echo "$info" | awk -F": *" '/Device Location:/ { print $2; exit }')
        removable=$(echo "$info" | awk -F": *" '/Removable Media:/ { print $2; exit }')
        internal=$(echo "$info" | awk -F": *" '/^ *Internal:/ { print $2; exit }')

        [ "$internal" != "Yes" ] || die "$target is an internal disk -- refusing"

        case "${location:-}${removable:-}" in
        *External* | *Removable*) ;;
        *) die "$target does not look removable (location ${location:-unknown}, media ${removable:-unknown}) -- refusing" ;;
        esac

        name=$(echo "$info" | awk -F": *" '/Device \/ Media Name/ { print $2; exit }')
        size=$(echo "$info" | awk -F": *" '/Disk Size/ { print $2; exit }')

        # The number above was the choice. Asking for the name as well made a
        # second decision out of one, and typing a disk name is not a safety
        # check -- what keeps this off the wrong drive is the refusal above to
        # touch anything internal or not removable.
        printf "\n%sThis erases %s (%s, %s) completely.%s\n" \
                "$RED$BOLD" "$target" "${name:-unknown}" "${size:-unknown size}" "$RESET"
        printf "Enter to write, anything else to stop: "
        read -r confirmation

        [ -z "$confirmation" ] || die "nothing written"

        say "Erasing $target"
        diskutil unmountDisk "$target" >/dev/null 2>&1
        diskutil eraseDisk FAT32 MOONWATER GPT "$target" ||
                die "could not format $target"

        volume="/Volumes/MOONWATER"
        [ -d "$volume" ] || die "formatted, but $volume did not appear"

        say "Writing the image"
        mkdir -p "$volume/EFI/BOOT" || die "could not create $volume/EFI/BOOT"
        cp "$image" "$volume/EFI/BOOT/BOOTX64.EFI" || die "could not copy the image"
        sync

        diskutil eject "$target" >/dev/null 2>&1

        say "Done -- $target is bootable and safe to unplug"
        echo
        echo "On the machine: boot it, choose the USB stick from the firmware"
        echo "boot menu, and make sure it is booting UEFI rather than legacy."
        echo "Secure Boot has to be off: this kernel is not signed."
        exit 0
fi

#
#       Booting it here.
#
command -v qemu-system-x86_64 >/dev/null 2>&1 ||
        die "qemu-system-x86_64 is not installed"

#       drm_client_lib.active= stops the fbdev client claiming the display.
#       It has to be built (DRM_CLIENT_LIB depends on it) but it must not take
#       the screen, or the compositor is drawing underneath something else.
cmdline="console=ttyS0 drm_client_lib.active="

say "Booting $image"
size "$image"

#       virtio-gpu rather than the default VGA: it is the only device here that
#       offers a hardware cursor plane, which is what lets the compositor move
#       the pointer without repainting anything.
#
#       usb-tablet reports absolute positions, so the pointer inside the guest
#       follows the one on the host instead of drifting.
#
#       -vga none matters: without it QEMU also creates a standard VGA device,
#       the window shows that one because it is the boot VGA, and the
#       compositor ends up drawing on the other card where nobody can see it.
#
#       -cpu Nehalem, not the default, and this is a requirement rather than a
#       preference. The kernel is compiled -march=x86-64-v2, whose floor is
#       Nehalem, and QEMU's default model is qemu64 -- SSE3-era, no POPCNT.
#       There are 334 popcnt instructions in vmlinux, so on the default model
#       the image takes an invalid opcode before the console exists and prints
#       nothing whatsoever. This line is what stands between that and here.
#
#       This comment used to say the image booted on the default too. It does
#       not, and did not; see kernel/profile/arch/x64.
set -- \
        -m 2G \
        -smp 2 \
        -cpu Nehalem \
        -kernel "$image" \
        -vga none \
        -device virtio-gpu-pci \
        -device qemu-xhci -device usb-tablet -device usb-kbd \
        -no-reboot

# Hardware acceleration where this QEMU has it: hvf on macOS, kvm on Linux.
# -cpu host replaces the model above, which is what you want when the guest is
# running on the real one.
accelerators=$(qemu-system-x86_64 -accel help 2>/dev/null || true)
if echo "$accelerators" | grep -qw hvf; then
        set -- "$@" -accel hvf -cpu host
elif echo "$accelerators" | grep -qw kvm && [ -w /dev/kvm ]; then
        set -- "$@" -accel kvm -cpu host
fi

if [ "$console" -eq 1 ]; then
        say "Console on this terminal, ctrl-a x to quit"
        exec qemu-system-x86_64 "$@" -append "$cmdline" -display none -serial mon:stdio
fi

# cocoa is the macOS window; elsewhere prefer gtk and fall back to sdl.
display=cocoa
if [ "$(uname)" != "Darwin" ]; then
        displays=$(qemu-system-x86_64 -display help 2>/dev/null || true)
        if echo "$displays" | grep -qw gtk; then
                display=gtk
        elif echo "$displays" | grep -qw sdl; then
                display=sdl
        else
                die "this QEMU has no graphical display backend -- use --shell"
        fi
fi

say "Window opening, ctrl-alt-g releases the mouse"
exec qemu-system-x86_64 "$@" -append "$cmdline" -display "$display" -serial mon:stdio
