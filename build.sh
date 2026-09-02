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
#           sh build.sh --clean                remove what a build produced
#           sh build.sh --host box            build on another machine over ssh
#           sh build.sh arch/x64 debug_none limbo desktop serial terminal
#                                               boot to a console; keep Canvas built
#           sh build.sh arch/x64 debug_none limbo desktop serial server
#                                               omit Canvas from the kernel
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

# Sourced by this script's own path rather than a relative one, so that being
# in the wrong directory produces is_safe's explanation rather than a bare
# "kit/common: No such file or directory" from the shell.
# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1091
. "$here/kit/common"

# Every path below is relative to the repository root. kit/common no longer
# checks that on being sourced -- it is a library, and libraries that refuse to
# load are why its helpers got copied around -- so the entry point checks.
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
#       One build directory per source tree, not one per machine.
#
#       This used to be /tmp/moonwater-build for everybody. Two people, or two
#       sessions, or a person and an agent building at the same time wrote
#       their objects and their image into the same place and neither was told.
#       An incremental build then reuses whatever is there: the userspace half
#       from one tree and the kernel module from another, linked into one image
#       that matches no checkout anybody has. Every measurement taken off such
#       an image is about a tree that does not exist.
#
#       The suffix is a checksum of this tree's own path, so the same checkout
#       always gets the same directory and two checkouts never share one.
#       MOONWATER_BUILD_DIR still overrides it.
#
tree_mark=$(printf '%s' "$here" | cksum | cut -d' ' -f1)
remote=${MOONWATER_BUILD_DIR:-/tmp/moonwater-$(basename "$here")-$tree_mark}
extra=""
do_run=0
do_build=1
do_clean=0
do_usb=0
console=0
image=""

while [ "$#" -gt 0 ]; do
        case "$1" in
        --clean) do_clean=1 ;;
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
        #
        #       sudo drops the environment, so anything the remote build has
        #       to know is named here. env rather than a VAR=value prefix,
        #       which sudo only passes when it has been configured to.
        #
        carry=""
        [ -z "${MOONWATER_STOCK:-}" ] || carry="MOONWATER_STOCK=1"

        # shellcheck disable=SC2029,SC2086
        ssh -n "$host" "cd $remote && sudo env $carry sh build.sh $extra" ||
                die "the build failed on $host"

        # The host which built the configured profile is authoritative about
        # its export.  A stale local artifacts/.config may describe another
        # architecture entirely (an ARM Mac commonly names kernel8.img).
        image=$(ssh -n "$host" \
                "cd $remote && . ./kit/common && key_one kernel_export") ||
                die "could not identify the built image"
        case "$image" in
        dist/*) ;;
        *) die "remote build reported an invalid image path: $image" ;;
        esac

        say "Fetching $image"
        mkdir -p "$(dirname "$image")"
        scp -q "$host:$remote/$image" "$image" ||
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
                mkdir -p artifacts fs dist || die "repository setup"

        label DISTRO INFO
                echo "CONFIG_LOCALVERSION=\"$info_full_name\"" > artifacts/info
                echo "CONFIG_DEFAULT_HOSTNAME=\"$info_name-box\"" >> artifacts/info
                #
                #       The device nodes the image boots with. mknod fails
                #       when the node already exists, which made every rebuild
                #       after the first noisy and, under set -e, fatal -- so
                #       each is created only when missing.
                #
                # tmp, etc and root because everything expects them to be there:
                # a redirection into /tmp is the first thing anybody tries.
                mkdir -p fs/sys fs/proc fs/dev fs/tmp fs/etc fs/root \
                        fs/bin fs/sbin fs/usr ||
                        die "filesystem setup"

                make_node() {
                        [ -e "$1" ] && return 0
                        mknod "$1" "$2" "$3" "$4"
                }

                make_node fs/dev/tty     c 5 0
                make_node fs/dev/console c 5 1
                make_node fs/dev/null    c 1 3
                make_node fs/dev/zero    c 1 5
                make_node fs/dev/random  c 1 8
                make_node fs/dev/urandom c 1 9
                #       Where a system service says what it did. printk
                #       serialises whole records, so a daemon writing
                #       here can never land in the middle of a line
                #       somebody else is writing to the console.
                make_node fs/dev/kmsg    c 1 11

                # /dev/spark is how userspace asks the kernel to spawn a
                # program without forking first. The minor has to match
                # SPARK_DEVICE_MINOR in src/spark.c.
                make_node fs/dev/spark   c 10 250

        label KERNEL SOURCE
                kernel_extract_dir="linux/"
                download_artifacts_dir="artifacts/"
                trusted_keys="torvalds@kernel.org gregkh@kernel.org"
                kernel_version="7.2"

                # Derived rather than written out, so moving to another release means editing
                # the version and the signature and nothing else. kernel.org lays every series
                # out under vMAJOR.x.
                kernel_series="v${kernel_version%%.*}.x"
                kernel_download="https://cdn.kernel.org/pub/linux/kernel/$kernel_series/linux-$kernel_version.tar.xz"

                # The signature is pinned here rather than downloaded next to the tarball.
                # Fetching both would still verify, but only that the archive is signed by a
                # trusted key -- pinning ties the build to this exact release, so a validly
                # signed but different kernel cannot be substituted.
                #
                # To move to a new release: take the .sign file from
                # cdn.kernel.org/pub/linux/kernel/vX.x/linux-VERSION.tar.sign and paste it
                # here along with the version and URL above.
                kernel_pgp="
-----BEGIN PGP SIGNATURE-----
Comment: This signature is for the .tar version of the archive
Comment: git archive --format tar --prefix=linux-7.2/ v7.2
Comment: git version 2.55.0

iQIzBAABCgAdFiEEZH8oZUiU471FcZm+ONu9yGCSaT4FAmqCjM4ACgkQONu9yGCS
aT6jEBAAi+dDv3sQNuZPoSOjnv3be79xilhgbYRjXjYGyYr/axHwyCfRxYkV/sL0
SHOXT9ZGKp/GPjc8i21Pgca4c4UhckX48RTH7xNO3dR9X8n3g+8OLqP8FF2iFqdv
TWnagMo6CFyMmWj75WRwcZGKw2fOjCr9tSTSklAkLc8gytgUyHJKxcDHrYDpcdRF
GbhXn9GauSYu0ablmf6pSInjicXDMzPj9QVSt9NkO6FcrSoAfUfmU4c9EEsKW9T6
K5LsiyhRgcQfE0zrw1hYQBr2gFSXt8pa2u2XPVVukIBB9XSPdSG2x228b+yHmp/Y
zPRUzPDVkkK1BkU1D7XJdVmt2C3kfeBUJEcAlVKcDWf9rY80SU6FVyc45TwRfw8h
kq86+ERAmWOCwYsZjMK4i3PK4Zs60Q0rQZgmMY/mfqSxzMoCV2O9FGea8ZZQIlGH
m3qZw79igreY852bLihddRDgXAz47VFAwRnqzKaSJVMtUdigEPb34idC2ZE0yp07
PnHgCqFYktDu3+Enpm7RItsK0b0oQHdmeB8eOPgGSJ3gcJVmGKVaS4zd46gGDgJC
yt0LTonkwQO8q3jTN/2ffkVjzdrvk4IeYX5k3SQ6rinfebi0OMCQ9xDyR7MYvdwu
wgcVXSeiHcXa9SSFDvKn0L1q5nSLQGHp38qUi1ZPf/1uQSuB3ME=
=D53G
-----END PGP SIGNATURE-----
"

                kernel_tarball=$download_artifacts_dir"linux-$kernel_version.tar"
                kernel_archive=$kernel_tarball".xz"

                # check for build dependencies -- this used to sit after the early exit below,
                # so it never ran on any build after the first.
                build_required="bison flex bc gpg make gcc clang rustc"

                for pkg in $build_required; do
                    if ! command -v "$pkg" > /dev/null 2>&1; then
                        echo "$pkg is required to build the kernel. Please install it and try again." >&2
                        exit 1
                    fi
                done

                # A Makefile is the marker that the tree is really there. Testing only for the
                # directory treated an empty or half extracted linux/ as a finished extraction.
                # Was "exit 0" when this was its own script; inlined, that would end the
                # build rather than this step.
                kernel_present=0
                if [ -f "$kernel_extract_dir/Makefile" ]; then
                    echo $BOLD "Kernel already extracted..." $RESET
                    kernel_present=1
                fi

                [ "$kernel_present" -eq 1 ] || extract_kernel() {
                    echo $BOLD "Checking kernel signature" $RESET

                    # Every step below gates the next one. None of these exit statuses were
                    # checked before, so a failed download, a failed key fetch or a failed
                    # signature verification all still ended in a compiled kernel.
                    if ! gpg --locate-keys $trusted_keys; then
                        echo "$RED""ERROR: could not fetch the kernel signing keys ($trusted_keys)." >&2
                        echo "Refusing to build an unverified kernel.""$RESET" >&2
                        exit 1
                    fi

                    if ! unxz -k $kernel_archive; then
                        echo "$RED""ERROR: could not decompress $kernel_archive""$RESET" >&2
                        exit 1
                    fi

                    echo "$kernel_pgp" >$kernel_tarball".sign"

                    if ! gpg --verify $kernel_tarball".sign" $kernel_tarball; then
                        echo "$RED""ERROR: SIGNATURE VERIFICATION FAILED for $kernel_tarball" >&2
                        echo "The archive does not match the signature pinned in this script." >&2
                        echo "Refusing to extract or build it. Delete $kernel_archive and retry.""$RESET" >&2
                        rm -f $kernel_tarball
                        exit 1
                    fi

                    if ! tar -xf $kernel_tarball --strip-components=1 -C $kernel_extract_dir; then
                        echo "$RED""ERROR: could not extract $kernel_tarball""$RESET" >&2
                        rm -f $kernel_tarball
                        exit 1
                    fi

                    rm $kernel_tarball

                    echo $BOLD "Kernel extracted to $kernel_extract_dir" $RESET
                }


                mkdir -p $download_artifacts_dir
                mkdir -p $kernel_extract_dir

                if [ "$kernel_present" -eq 0 ] && ! is_file $kernel_archive; then
                    if ! curl -fL $kernel_download -o $kernel_archive; then
                        echo "ERROR: failed to download $kernel_download" >&2
                        rm -f $kernel_archive
                        exit 1
                    fi
                fi

                [ "$kernel_present" -eq 1 ] || extract_kernel

        label KERNEL CONFIGURATION

                # "any" carries what every image needs, "general" the hardware
                # baseline for an ordinary x86_64 desktop, "gpu" the modesetting the
                # compositor draws through, "guests" what it takes to run somebody
                # else's userspace, "latency" the scheduling and timer settings the
                # whole design rests on, and "prod" the removal of the machinery only
                # a kernel developer uses. All six are composed in ahead of whatever
                # was asked for, in that order, so the last two win the choices the
                # earlier ones happen to touch.
                if [ -z "$extra" ]; then
                        #
                        #       serial is last on purpose, and it is not
                        #       optional.
                        #
                        #       src/test/boot.sh drives the image over a
                        #       serial line and reads its answers back, so a
                        #       default image without CONFIG_SERIAL_8250 has
                        #       no way to be tested at all: the kernel comes
                        #       up, runs /init, and says nothing, because
                        #       there is no ttyS0 for /dev/console to be. The
                        #       lane had been passing on a configuration
                        #       carried over from an earlier build rather than
                        #       on one these profiles produce, and the first
                        #       regeneration of it turned the boot silent.
                        #
                        #       Last, because it also asks for the loglevel
                        #       and the timestamps that make the transcript
                        #       readable, and debug_none quietens both.
                        #
                        profiles="any general gpu guests latency prod arch/x64 debug_none limbo desktop serial"
                else
                        # shellcheck disable=SC2086
                        profiles="any general gpu guests latency prod $extra"
                fi

                # Always compose the selected profiles. Reusing artifacts/.config
                # made a plain default build inherit whichever special profile
                # had run before it -- notably leaving Canvas disabled after a
                # server build even though `sh build.sh` promises the defaults.
                # kit/config preserves incremental builds when the result is
                # unchanged, so deterministic selection costs no rebuild by
                # itself.
                # shellcheck disable=SC2086
                sudo sh kit/config $profiles || die "configuration"

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
                #
                #       Every edit we make to Linux's own source now lives in
                #       kernel/patch/apply, because everything that touches the
                #       kernel belongs under kernel/ and nothing else does.
                #       What was here was a hundred and eighty lines of claims,
                #       displacements and grafts, which is the answer to "what
                #       did you change about the kernel" and was findable only
                #       by reading a build script.
                #
                sh kernel/patch/apply || die "patching the kernel source"
                if is_newer artifacts/.config linux/.config; then
                        (
                                cd linux || exit 1
                                # shellcheck disable=SC2086 # two arguments, not one
                        sudo make allnoconfig $make_flags > /dev/null || exit 1
                                # Quiet: merge_config compares the combined
                                # fragment against an allnoconfig baseline and
                                # calls most of what the profiles ask for a
                                # "redefinition" -- 125 lines meaning nothing.
                                # kit/config reports the disagreements that
                                # matter, between profiles.
                                #
                                # make_flags goes in the environment, not on
                                # the command line: merge_config.sh reads
                                # trailing arguments as fragment paths, so
                                # "ARCH=arm64" became a file that did not
                                # exist and stopped every cross build.
                                # shellcheck disable=SC2086
                                env $make_flags sh scripts/kconfig/merge_config.sh -m .config ../artifacts/.config > /dev/null || exit 1
                                # shellcheck disable=SC2086 # two arguments, not one
                        sudo make olddefconfig $make_flags > /dev/null || exit 1
                        ) || die "kernel configuration"
                else
                        echo "No changes"
                fi

        label CONFIGURATION CHECK
                # merge_config and olddefconfig drop unmet options without a word, so
                # anything a profile asked for and did not get is reported here rather
                # than discovered later as hardware that does not work.
                # shellcheck disable=SC2086
                sh kit/verify_config linux/.config $profiles

        label ASSEMBLY
                # Where a profile asked for a .asm from src/ to stand in for a file
                # the kernel already builds. The .asm files that belong to the module
                # rather than to the kernel need nothing here -- src/Makefile builds
                # those as part of it.
                sudo sh kernel/replace/apply || die "assembly"

        label PRE BUILD
                eval "$(key "pre")"

        label USER SPACE BUILD
                #
                #       What was here last time, gone.
                #
                #       Nothing ever removed a build product from fs/, so
                #       anything that stopped being built stayed in the image
                #       forever: an 857 kilobyte binary from a fortnight ago
                #       was still shipping, along with every program that had
                #       since become a name for the shell. Only the top level
                #       and only files and links -- the directories below hold
                #       the device nodes and are made once.
                #
                find fs -maxdepth 1 \( -type f -o -type l \) -delete ||
                        die "clearing the last image"
                find fs/bin -maxdepth 1 \( -type f -o -type l \) -delete ||
                        die "clearing the last /bin image"
                find fs/sbin -maxdepth 1 \( -type f -o -type l \) -delete ||
                        die "clearing the last /sbin image"
                find fs/usr -maxdepth 1 \( -type f -o -type l \) -delete ||
                        die "clearing the last /usr image"

                # Read the component switches once. An existing build tree has
                # no lines for newly added symbols until olddefconfig next
                # runs; their Kconfig defaults are y, while the core must be
                # explicitly built in for the initial filesystem to use it.
                eval "$(awk '
                        BEGIN { print "moon_core=0\nmoon_shell=1\nmoon_utilities=1\n" \
                                      "moon_util_linux=1\nmoon_shell_monitor=1" }
                        /^CONFIG_MOONWATER_[A-Z_]*=y$/ {
                                name = $0; sub(/^CONFIG_MOONWATER_/, "", name)
                                sub(/=y$/, "", name); print "moon_" tolower(name) "=1"
                        }
                        /^# CONFIG_MOONWATER_[A-Z_]* is not set$/ {
                                name = $0; sub(/^# CONFIG_MOONWATER_/, "", name)
                                sub(/ is not set$/, "", name); print "moon_" tolower(name) "=0"
                        }
                ' linux/.config)"

                # The X-macro is both the compiled dispatch registry and the
                # installed surface, including its component categories.
                shell_tool_names() {
                        awk -F '[(),[:space:]]+' -v util="$moon_util_linux" \
                            -v monitor="$moon_shell_monitor" \
                            '$1 == "SHELL_TOOL" &&
                             ($2 == "GENERAL" ||
                              (monitor && $2 == "MONITOR") ||
                              (util && ($2 == "UTIL_BIN" ||
                                        $2 == "UTIL_SBIN"))) {
                                     print $3
                             }' src/sh/tools.inc
                }

                shell_system_names() {
                        awk -F '[(),[:space:]]+' \
                            '$1 == "SHELL_TOOL" && $2 == "SYSTEM" { print $3 }' \
                            src/sh/tools.inc
                }

                shell_conventional_names() {
                        awk -F '[(),[:space:]]+' \
                            '$2 == "UTIL_BIN"  { print "bin",  $3 }
                             $2 == "UTIL_SBIN" { print "sbin", $3 }' \
                            src/sh/tools.inc
                }

                applet_binary=
                if [ "$moon_core" -eq 1 ] && [ "$moon_shell" -eq 1 ]; then
                        # Every program in the default image is spark, including
                        # the one the kernel execs as /init, so no ELF is loaded
                        # on its boot path.
                        spark_cppflags=
                        [ "$moon_utilities" -eq 1 ] ||
                                spark_cppflags="$spark_cppflags -DSHELL_NO_UTILITIES"
                        [ "$moon_util_linux" -eq 1 ] ||
                                spark_cppflags="$spark_cppflags -DSHELL_NO_UTIL_LINUX"
                        [ "$moon_shell_monitor" -eq 1 ] ||
                                spark_cppflags="$spark_cppflags -DSHELL_NO_MONITOR"
                        SPARK_CPPFLAGS=$spark_cppflags \
                                sh kit/spark programs/shell fs/shell ||
                                die "building the shell"
                        applet_binary=shell

                        # Scripts need a real interpreter path: /shell is the
                        # image's binary, but a #!/bin/sh shebang is resolved
                        # by the kernel before the shell gets any say.
                        ln -sf ../shell fs/bin/sh || die "linking /bin/sh"
                        ln -sf ../bin fs/usr/bin || die "linking /usr/bin"
                        ln -sf ../sbin fs/usr/sbin || die "linking /usr/sbin"
                        for utility in $(shell_system_names); do
                                ln -sf shell "fs/$utility" || die "linking $utility"
                        done

                        if [ "$moon_shell_monitor" -eq 1 ] && [ "$moon_utilities" -eq 1 ]; then
                                cp programs/monitor.sh fs/monitor.sh || die "installing /monitor.sh"
                                chmod 0755 fs/monitor.sh || die "making /monitor.sh executable"
                                ln -sf monitor.sh fs/mointor.sh || die "linking /mointor.sh"
                                ln -sf ../monitor.sh fs/bin/monitor.sh || die "linking /bin/monitor.sh"
                                ln -sf ../monitor.sh fs/bin/mointor.sh || die "linking /bin/mointor.sh"
                        fi
                elif [ "$moon_core" -eq 1 ] && [ "$moon_utilities" -eq 1 ]; then
                        spark_cppflags=-DSHELL_NO_MONITOR
                        [ "$moon_util_linux" -eq 1 ] ||
                                spark_cppflags="$spark_cppflags -DSHELL_NO_UTIL_LINUX"
                        SPARK_CPPFLAGS=$spark_cppflags \
                                sh kit/spark programs/utilities fs/shell ||
                                die "building the utilities"
                        # The kernel's SPAWN_TOOL ABI accelerates through this
                        # fixed path. This binary has no shell fallback.
                        applet_binary=shell
                        ln -sf ../bin fs/usr/bin || die "linking /usr/bin"
                        ln -sf ../sbin fs/usr/sbin || die "linking /usr/sbin"
                fi

                #
                #       Utilities are one multicall Spark program under other names.
                #
                #       With the shell present they share its binary. A utility-only
                #       image has the same dispatch table but no shell fallback.
                #
                if [ "$moon_core" -eq 1 ] && [ "$moon_utilities" -eq 1 ]; then
                        for utility in $(shell_tool_names); do
                                ln -sf "$applet_binary" "fs/$utility" ||
                                        die "linking $utility"
                        done

                        if [ "$moon_util_linux" -eq 1 ]; then
                                # Conventional util-linux locations for scripts
                                # which use absolute paths.
                                shell_conventional_names |
                                while read -r directory utility; do
                                        ln -sf "../$applet_binary" \
                                                "fs/$directory/$utility" ||
                                                die "linking /$directory/$utility"
                                done
                        fi
                fi

        label KERNEL BUILD
                cpu_cores=$(nproc)

                make_flags=$(key make_flags)

                # The kernel used to be built with whatever arch/x86/Makefile chose, because
                # nothing reached the C compiler. KCFLAGS is that gap closed.
                kernel_cflags=$(key kernel_cflags)

                kernel_image=$(key_one kernel_image)
                kernel_export=$(key_one kernel_export)

                [ -n "$kernel_image" ] && [ -n "$kernel_export" ] ||
                        die "kernel_image / kernel_export not set in artifacts/.config"

                # In a subshell, so the working directory comes back on its
                # own. This used to be a separate script and got it back by
                # being a separate process.
                #
                # make's exit status was discarded once, so a failed build fell
                # through to the copy below and shipped whatever image was left
                # over from the run before.
                #
                # KCPPFLAGS, KAFLAGS, LDFLAGS and RUSTFLAGS were passed here too,
                # from keys no profile has ever set -- four empty variables
                # handed to make on every build.
                # shellcheck disable=SC2086
                ( cd linux && make -j"$cpu_cores" $make_flags KCFLAGS="$kernel_cflags" ) ||
                        die "kernel build"

                [ -f "$kernel_image" ] ||
                        die "expected image '$kernel_image' was not produced"

                mkdir -p "$(dirname "$kernel_export")"
                sudo cp "$kernel_image" "$kernel_export"

        label POST BUILD
                eval "$(key "post")"
                echo "$BOLD""Done Building Kernel""$GREEN"
                size "$(key kernel_export)"
                echo "$RESET"
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
        # The .S kit/asm generates from each .asm, which kbuild writes here
        # because src/ is the kernel tree's kernel/moonwater.
        rm -f src/*.S src/*.asm_tmp
        exit 0
fi

if [ "$do_build" -eq 1 ]; then
        if [ -n "$host" ]; then
                build_remote
        else
                build_local
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
