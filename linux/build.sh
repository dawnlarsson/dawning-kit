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

        [ -z "${1:-}" ] || sudo sh script/config any "$@" || die "profile configuration"

        is_file artifacts/.config || \
                sudo sh script/config any arch/x64 debug_none limbo desktop || \
                die "default configuration"

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

label PRE BUILD
        eval "$(key "pre")"

label USER SPACE BUILD
        sh ../standard/build_kernel programs/init fs/init || die "building init"
        sh ../standard/build_kernel programs/shell fs/shell || die "building shell"
        sh ../standard/build_kernel programs/duck fs/duck || die "building duck"
        sh ../standard/build_kernel programs/edit fs/edit || die "building edit"

        # Built in the spark format rather than as an ELF, so the loader in
        # src/dawning_core.c is exercised by every image rather than sitting
        # registered and unused.
        sh ../standard/spark programs/sparktest fs/sparktest || die "building sparktest"

label KERNEL BUILD
        sudo sh script/kernel_build || die "kernel build"

label POST BUILD
        eval "$(key "post")"
        echo "$BOLD""Done Building Kernel""$GREEN"
        size "$(key kernel_export)"
        echo "$RESET"