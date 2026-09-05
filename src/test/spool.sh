#!/bin/sh
#
#       The rest of <stdio.h> against glibc's, on three machines.
#
#           sh src/test/spool.sh
#
#       Everything here is a comparison, for the reason src/test/stream.sh
#       gives: glibc is on the machine, glibc defines what remove, mkstemp,
#       tmpfile, popen, pclose, fgetpos and the unlocked spellings answer, and
#       an implementation that is merely self-consistent is worth nothing.
#
#       One program, built four times from one body. The reference links glibc
#       through the host compiler; the other three are freestanding spark
#       links for x86_64, arm64 and riscv64 and run under their emulators.
#       Both the trace each one prints and the tree of files each one leaves
#       behind are compared.
#
#       The pipeline half of the trace forks and execs /bin/sh, which is a
#       host binary in every case -- qemu-user hands execve to the host kernel
#       -- so all four runs drive the same shell and any difference in the
#       trace is a difference in this library.
#
#       The reference needs glibc and a host compiler; without them the lane
#       reports that it did not run rather than passing on nothing. Each
#       target needs its cross compiler and its emulator, and a target that
#       has neither is skipped by name.
#
set -u

# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
root=$(CDPATH= cd -- "$here/.." && cd .. && pwd)

cd "$root" || exit 1

work=$(mktemp -d "${TMPDIR:-/tmp}/dawning-spool.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

report() {
        if [ "$1" = ok ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-10s %-34s %s\n' spool "$2" "$3"
}

finish() {
        printf '  %-12s %s of %s\n' spool "$pass" "$((pass + fail))"
        [ -z "${TEST_TALLY:-}" ] ||
                printf 'spool %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"
        [ "$fail" = 0 ]
}

host=${CC:-gcc}

if ! command -v "$host" > /dev/null 2>&1; then
        printf '  no host compiler, the reference cannot be built -- not run\n'
        exit 2
fi

#       The link a spark program gets. Kept in step with src/test/run's own
#       freestanding: no libgcc under an arm64 atomic, and the documented
#       RISC-V floor rather than whatever the driver defaults to.
build_ours() {
        source=$1
        output=$2
        target=$3
        cross=$4
        where=$5

        case $target in
        arm64)   set -- -mno-outline-atomics ;;
        riscv64) set -- -march=rv64imafd_zicsr_zicntr -mabi=lp64d ;;
        *)       set -- ;;
        esac

        $cross -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
                -fno-builtin "$@" -DWORK="\"$where\"" -w -T kit/spark.ld \
                -Wl,-e,_start -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
                -o "$output" "$source" 2> "$output.err"
}

#
#       The reference, built and run once.
#
mkdir -p "$work/reference"

if ! $host -O2 -w -DWORK="\"$work/reference\"" \
        -o "$work/reference.elf" src/test/spool_reference.c 2> "$work/ref.err"
then
        printf '  the glibc reference did not build -- not run\n'
        sed 's/^/    /' "$work/ref.err" | head -10
        exit 2
fi

"$work/reference.elf" > "$work/reference.out" 2>&1

if [ ! -s "$work/reference.out" ]; then
        printf '  the glibc reference printed nothing -- not run\n'
        exit 2
fi

#
#       Each target, built, run, and diffed against it.
#
missing=0

for target in x86_64 arm64 riscv64; do
        case $target in
        x86_64)
                cross=$host
                runner=
                ;;
        arm64)
                cross=aarch64-linux-gnu-gcc
                runner=qemu-aarch64
                ;;
        riscv64)
                cross=riscv64-linux-gnu-gcc
                runner=qemu-riscv64
                ;;
        esac

        if ! command -v "$cross" > /dev/null 2>&1; then
                printf '  %-10s no %s -- skipped\n' spool "$cross"
                missing=1
                continue
        fi

        if [ -n "$runner" ] && ! command -v "$runner" > /dev/null 2>&1; then
                printf '  %-10s no %s -- skipped\n' spool "$runner"
                missing=1
                continue
        fi

        mkdir -p "$work/$target"

        if ! build_ours src/test/spool.c "$work/spool.$target" "$target" \
                "$cross" "$work/$target"
        then
                report bad "$target" "did not build"
                sed 's/^/    /' "$work/spool.$target.err" | head -10
                continue
        fi

        if [ -n "$runner" ]; then
                if $runner "$work/spool.$target" > "$work/$target.out" 2>&1
                then
                        report ok
                else
                        report bad "$target" "internal invariant failed"
                fi
        else
                if "$work/spool.$target" > "$work/$target.out" 2>&1
                then
                        report ok
                else
                        report bad "$target" "internal invariant failed"
                fi
        fi

        if [ ! -s "$work/$target.out" ]; then
                report bad "$target" "printed nothing"
                continue
        fi

        if diff -u "$work/reference.out" "$work/$target.out" \
                > "$work/$target.diff" 2>&1
        then
                report ok
        else
                report bad "$target" "the trace differs from glibc"
                sed 's/^/    /' "$work/$target.diff" | head -30
        fi

        #       The files each run left behind. Every entry here that creates
        #       something also removes it, so both trees must be empty; a
        #       temporary that outlives its stream, or a remove that removed
        #       nothing, shows up here and nowhere in the trace.
        if diff -r "$work/reference" "$work/$target" \
                > "$work/$target.tree" 2>&1
        then
                report ok
        else
                report bad "$target" "the files left behind differ"
                sed 's/^/    /' "$work/$target.tree" | head -20
        fi
done

finish || exit 1
[ "$missing" = 0 ] || exit 2
exit 0
