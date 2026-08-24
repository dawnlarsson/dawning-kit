#!/bin/sh
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

label REPOSITORY SETUP
        echo "Building $info_full_name"
        sudo sh script/setup || die "repository setup"

label DISTRO INFO
        echo "CONFIG_LOCALVERSION=\"$info_full_name\"" > artifacts/info
        echo "CONFIG_DEFAULT_HOSTNAME=\"$info_name-box\"" >> artifacts/info
        sudo sh script/fs_setup || die "filesystem setup"

label KERNEL CONFIGURATION
        sudo sh script/kernel_setup || die "kernel setup"

        # "any" carries what every image needs, "general" the hardware baseline
        # for an ordinary x86_64 desktop, and "gpu" the modesetting the
        # compositor draws through. All three are composed in ahead of whatever
        # was asked for.
        if [ -z "${1:-}" ]; then
                profiles="any general gpu arch/x64 debug_none limbo desktop"
        else
                profiles="any general gpu $*"
        fi

        # shellcheck disable=SC2086
        is_file artifacts/.config ||
                sudo sh script/config $profiles ||
                die "configuration"

        [ -z "${1:-}" ] || sudo sh script/config $profiles || die "configuration"

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
        line_add_padded "linux/Kconfig" "source \"kernel/dawning/Kconfig\""
        line_add_padded "linux/kernel/Makefile" "obj-y += dawning/"

        if [ ! -e linux/kernel/dawning ]; then
                sudo ln -s "$(pwd)/src" linux/kernel/dawning || die "linking kernel module source"
        fi

        if is_newer artifacts/.config linux/.config; then
                (
                        cd linux || exit 1
                        sudo make allnoconfig "$make_flags" > /dev/null || exit 1
                        sh scripts/kconfig/merge_config.sh -m .config ../artifacts/.config "$make_flags" > /dev/null || exit 1
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

label PRE BUILD
        eval "$(key "pre")"

label USER SPACE BUILD
        # Every program in the image is spark, including the one the kernel
        # execs as /init, so no ELF is ever loaded on the boot path.
        for program in init shell duck edit sparktest pointer; do
                sh ../standard/spark "programs/$program" "fs/$program" ||
                        die "building $program"
        done

label KERNEL BUILD
        sudo sh script/kernel_build || die "kernel build"

label POST BUILD
        eval "$(key "post")"
        echo "$BOLD""Done Building Kernel""$GREEN"
        size "$(key kernel_export)"
        echo "$RESET"