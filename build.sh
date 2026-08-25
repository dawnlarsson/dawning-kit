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

        # "any" carries what every image needs, "general" the hardware
        # baseline for an ordinary x86_64 desktop, "gpu" the modesetting the
        # compositor draws through, "guests" what it takes to run somebody
        # else's userspace, "latency" the scheduling and timer settings the
        # whole design rests on, and "prod" the removal of the machinery only
        # a kernel developer uses. All six are composed in ahead of whatever
        # was asked for, in that order, so the last two win the choices the
        # earlier ones happen to touch.
        if [ -z "${1:-}" ]; then
                profiles="any general gpu guests latency prod arch/x64 debug_none limbo desktop"
        else
                profiles="any general gpu guests latency prod $*"
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
                sh std/spark "programs/$program" "fs/$program" ||
                        die "building $program"
        done

label KERNEL BUILD
        sudo sh script/kernel_build || die "kernel build"

label POST BUILD
        eval "$(key "post")"
        echo "$BOLD""Done Building Kernel""$GREEN"
        size "$(key kernel_export)"
        echo "$RESET"