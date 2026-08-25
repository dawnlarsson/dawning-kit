#!/bin/sh
#
#       Builds a Moonwater image, and optionally boots or writes it.
#
#       Usage:
#           sh build.sh                       build with the default profiles
#           sh build.sh arch/x64 debug_none   build with the profiles named
#           sh build.sh --run                 build, then boot it in a window
#           sh build.sh --run --shell         boot with the console on this terminal
#           sh build.sh --boot                boot the last image, do not rebuild
#           sh build.sh --usb                 build, then write a USB stick
#           sh build.sh --host box            build on another machine over ssh
#
#       The build happens here by default. Building a kernel wants a Linux
#       toolchain and a case sensitive filesystem, so on anything else -- a Mac,
#       most obviously -- point --host at a machine that has them, or set
#       MOONWATER_BUILD_HOST once and forget about it. That machine keeps its
#       own copy of the kernel source, so rebuilds there are incremental, and
#       the finished image is copied back here.
#
#       Everything after the build is local either way: QEMU runs on this
#       machine so the window, the mouse and the keyboard are real.
#
# shellcheck disable=SC2154
# shellcheck disable=SC1091

# Nothing in this pipeline used to check a return value: a failed userspace
# build, a failed kernel config merge or a failed make all still ended with
# "Done Building Kernel" and a stale image copied to dist/.
set -e

. script/common

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
remote=${MOONWATER_BUILD_DIR:-/tmp/moonwater-build}
extra=""
do_run=0
do_build=1
do_usb=0
console=0

while [ "$#" -gt 0 ]; do
        case "$1" in
        --run) do_run=1 ;;
        --boot) do_run=1; do_build=0 ;;
        --shell) console=1 ;;
        --usb) do_usb=1 ;;
        --host)
                [ "$#" -ge 2 ] || die "--host wants a machine to build on"
                host=$2
                shift
                ;;
        --host=*) host=${1#--host=} ;;
        --*) die "unknown option $1" ;;
        *) extra="$extra $1" ;;
        esac
        shift
done

#
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
        ssh -n "$host" "mkdir -p $remote" || die "could not create $remote"

        # The kernel source, its artifacts and the built filesystem stay on
        # the build host: they are large, and none of them belong to this
        # checkout. linux/ is the upstream tree, not part of this repository.
        rsync -az --delete \
                --exclude '.git' \
                --exclude 'linux' \
                --exclude 'artifacts' \
                --exclude 'fs' \
                --exclude 'dist' \
                ./ "$host:$remote/" || die "copying the tree failed"

        say "Building on $host:$extra"
        # -n so the build does not swallow this script's stdin. Without it the
        # USB prompts below read nothing, because ssh forwards whatever is on
        # stdin to the remote command.
        # shellcheck disable=SC2029,SC2086
        ssh -n "$host" "cd $remote && sudo sh build.sh $extra" ||
                die "the build failed on $host"

        say "Fetching the image"
        mkdir -p dist
        scp -q "$host:$remote/dist/bootx64.efi" dist/bootx64.efi ||
                die "could not fetch the built image"
}

build_local() {
        # Only this path needs it. Booting an image, writing a stick and
        # driving a build on another machine all run as you.
        if [ "$(id -u)" != "0" ]; then
                label $YELLOW WARNING !!!
                echo "Building here wants root: sudo sh build.sh" 1>&2
                echo
        fi

        [ "$(uname)" = "Linux" ] ||
                die "building a kernel wants a Linux toolchain and a case
sensitive filesystem, and this is $(uname). Name a machine that has them with
--host, or set MOONWATER_BUILD_HOST."


        label REPOSITORY SETUP
                echo "Building $info_full_name"
                sudo sh script/setup || die "repository setup"

        label DISTRO INFO
                echo "CONFIG_LOCALVERSION=\"$info_full_name\"" > artifacts/info
                echo "CONFIG_DEFAULT_HOSTNAME=\"$info_name-box\"" >> artifacts/info
                sudo sh script/fs_setup || die "filesystem setup"

        label KERNEL CONFIGURATION
                sudo sh script/kernel_setup || die "kernel setup"

                # "any" carries what every image needs, "general" the hardware
                # baseline for an ordinary x86_64 desktop, "gpu" the modesetting the
                # compositor draws through, "guests" what it takes to run somebody
                # else's userspace, "latency" the scheduling and timer settings the
                # whole design rests on, and "prod" the removal of the machinery only
                # a kernel developer uses. All six are composed in ahead of whatever
                # was asked for, in that order, so the last two win the choices the
                # earlier ones happen to touch.
                if [ -z "$extra" ]; then
                        profiles="any general gpu guests latency prod arch/x64 debug_none limbo desktop"
                else
                        # shellcheck disable=SC2086
                        profiles="any general gpu guests latency prod $extra"
                fi

                # shellcheck disable=SC2086
                is_file artifacts/.config ||
                        sudo sh script/config $profiles ||
                        die "configuration"

                [ -z "$extra" ] || sudo sh script/config $profiles || die "configuration"

                make_flags=$(key make_flags)

        label BUILD ENVIRONMENT CHECK
                compiler=$(key compiler)

                if ! command -v "$compiler" > /dev/null 2>&1; then
                        label "$YELLOW" WARNING !!!
                        echo "$compiler not found. Attempting to install it."
                        echo

                        build_environment_check

                        $build_install "$compiler" || echo "ERROR: Unable to install $compiler. Please install it manually."
        
                else
                        echo "Using compiler:" "$BOLD$compiler"
                fi

        label KERNEL CONFIG
                line_add_padded "linux/Kconfig" "source \"kernel/moonwater/Kconfig\""
                line_add_padded "linux/kernel/Makefile" "obj-y += moonwater/"

                if [ ! -e linux/kernel/moonwater ]; then
                        sudo ln -s "$(pwd)/src" linux/kernel/moonwater || die "linking kernel module source"
                fi

                if is_newer artifacts/.config linux/.config; then
                        (
                                cd linux || exit 1
                                sudo make allnoconfig "$make_flags" > /dev/null || exit 1
                                # Quiet on purpose. merge_config compares the one
                                # combined fragment against an allnoconfig baseline, so
                                # it reports a "redefined" line for most of what the
                                # profiles ask for -- 125 of them, none of which mean
                                # anything. Where two profiles genuinely disagree is
                                # inside that fragment, and script/config reports it.
                                #
                                # make_flags goes in the environment rather than on the
                                # command line: merge_config.sh takes fragment paths as
                                # its trailing arguments, so an "ARCH=arm64" handed to
                                # it positionally is read as a file that does not exist
                                # and the build stops -- which is what every cross
                                # build has been doing. It runs make internally, so the
                                # environment reaches the same place anyway.
                                # shellcheck disable=SC2086
                                env $make_flags sh scripts/kconfig/merge_config.sh -m .config ../artifacts/.config > /dev/null || exit 1
                                sudo make olddefconfig "$make_flags" > /dev/null || exit 1
                        ) || die "kernel configuration"
                else
                        echo "No changes"
                fi

        label CONFIGURATION CHECK
                # merge_config and olddefconfig drop unmet options without a word, so
                # anything a profile asked for and did not get is reported here rather
                # than discovered later as hardware that does not work.
                # shellcheck disable=SC2086
                sh script/verify_config linux/.config $profiles

        label GLUE ASSEMBLY
                # Where a profile asked for a .asm from src/ to stand in for a file
                # the kernel already builds. The .asm files that belong to the module
                # rather than to the kernel need nothing here -- src/Makefile builds
                # those as part of it.
                sudo sh script/glue_replace || die "glue"

        label PRE BUILD
                eval "$(key "pre")"

        label USER SPACE BUILD
                # Every program in the image is spark, including the one the kernel
                # execs as /init, so no ELF is ever loaded on the boot path.
                for program in init shell duck edit sparktest pointer; do
                        sh kit/spark "programs/$program" "fs/$program" ||
                                die "building $program"
                done

        label KERNEL BUILD
                sudo sh script/kernel_build || die "kernel build"

        label POST BUILD
                eval "$(key "post")"
                echo "$BOLD""Done Building Kernel""$GREEN"
                size "$(key kernel_export)"
                echo "$RESET"
}

if [ "$do_build" -eq 1 ]; then
        if [ -n "$host" ]; then
                build_remote
        else
                build_local
        fi
fi

[ "$do_usb" -eq 1 ] || [ "$do_run" -eq 1 ] || exit 0

#
#       Where the image ended up. After a local build the profile says; after
#       a remote one there is no local config to ask, and the EFI stub image
#       is fetched to the same place either way.
#
image=$(key kernel_export 2>/dev/null || true)
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

        printf "\n%sThis erases %s (%s, %s) completely.%s\n" \
                "$RED$BOLD" "$target" "${name:-unknown}" "${size:-unknown size}" "$RESET"
        printf "Type the disk name to confirm (%s): " "$(basename "$target")"
        read -r confirmation

        [ "$confirmation" = "$(basename "$target")" ] || die "nothing written"

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
#       -cpu Nehalem, not the default. The kernel is compiled -march=x86-64-v2,
#       whose floor is Nehalem, and QEMU's default model is older than any
#       machine this targets. The image does still boot on the default -- that
#       was checked rather than assumed -- so this is not a requirement; it is
#       so the loop runs on a machine inside the stated hardware range instead
#       of one below all of it.
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
