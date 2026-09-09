original = open('build.sh').read().split('\n')

# build_remote(), verbatim: lines 112..172 in the original (1-based).
remote = '\n'.join(original[111:172])
# Everything from "Removing what a build produced" to the end.
tail = '\n'.join(original[647:])

head = r'''#!/bin/sh
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

# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd -- "$here"

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

'''

middle = r'''
build_local() {
        die "building a kernel wants a Linux toolchain and a case
sensitive filesystem, and this is $(uname). Name a machine that has them with
--host, or set MOONWATER_BUILD_HOST."
}

'''

open('build.sh', 'w').write(head + remote + middle + tail)
print("ok")
