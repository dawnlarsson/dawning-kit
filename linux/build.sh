#!/bin/sh
# shellcheck disable=SC2154
# shellcheck disable=SC1091
. script/common

label REPOSITORY SETUP
        echo "Building $info_full_name"
        sudo sh script/setup

label DISTRO INFO
        echo "CONFIG_LOCALVERSION=\"$info_full_name\"" > artifacts/info
        echo "CONFIG_DEFAULT_HOSTNAME=\"$info_name-box\"" >> artifacts/info
        sudo sh script/fs_setup

label KERNEL CONFIGURATION
        sudo sh script/kernel_setup
        
        [ -z "$1" ] || sudo sh script/config any "$@"

        is_file artifacts/.config || \
                sudo sh script/config any arch/x64 debug_none limbo desktop

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

        if [ ! -d linux/kernel/dawning ]; then
                sudo ln -s "$(pwd)/src" linux/kernel/dawning
        fi

        if is_newer artifacts/.config linux/.config; then
                (
                        cd linux || exit 1
                        sudo make allnoconfig "$make_flags" > /dev/null
                        sh scripts/kconfig/merge_config.sh -m .config ../artifacts/.config "$make_flags" > /dev/null
                        sudo make olddefconfig "$make_flags" > /dev/null
                )
        else
                echo "No changes"
        fi

label PRE BUILD
        eval "$(key "pre")"

label USER SPACE BUILD
        sh ../standard/build_kernel programs/init fs/init
        sh ../standard/build_kernel programs/shell fs/shell
        sh ../standard/build_kernel programs/duck fs/duck
        sh ../standard/build_kernel programs/edit fs/edit

label KERNEL BUILD
        sudo sh script/kernel_build

label POST BUILD
        eval "$(key "post")"
        echo "$BOLD""Done Building Kernel""$GREEN"
        size "$(key kernel_export)"
        echo "$RESET"