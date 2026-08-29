#!/bin/sh
#
#       The allocator, on three machines and against the one on this one.
#
#           sh src/test/allocator.sh
#
#       src/test/allocator.c is built four times: once for each architecture
#       against src/standard/allocator.c with no C library under it, and once
#       against glibc. Everything the test prints before its "shared-end" line
#       is a statement about behaviour rather than about addresses, so those
#       bytes have to be identical in all four, and that is what is diffed.
#       What each build prints after that line is about its own internals and
#       is shown rather than compared.
#
set -u

# shellcheck disable=SC1007
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck disable=SC1007
root=$(CDPATH= cd -- "$here/.." && cd .. && pwd)

cd "$root" || exit 1

work=${TMPDIR:-/tmp}/dawning-allocator.$$
mkdir -p "$work" || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

flags="-O2 -static -nostdlib -nostartfiles -fno-stack-protector -fno-builtin -w"
link="-T kit/spark.ld -Wl,-e,_start -Wl,--build-id=none -Wl,--no-warn-rwx-segments"

failed=0

note()
{
        printf '%s\n' "$*"
}

#       The reference first, because everything else is compared against it.
if ! gcc -O2 -std=gnu17 -w -DALLOCATOR_REFERENCE -o "$work/reference" \
        src/test/allocator.c 2>"$work/reference.log"
then
        note "allocator: the glibc reference did not build"
        cat "$work/reference.log"
        exit 1
fi

if ! "$work/reference" > "$work/reference.out"
then
        note "allocator: glibc itself failed the test, which is a bug here"
        failed=1
fi

sed -n '1,/^shared-end$/p' "$work/reference.out" > "$work/reference.shared"

one()
{
        name=$1
        compiler=$2
        runner=$3
        extra=$4

        if ! command -v "$compiler" > /dev/null 2>&1
        then
                note "allocator $name: no $compiler here, not run"
                return 0
        fi

        if [ -n "$runner" ] && ! command -v "$runner" > /dev/null 2>&1
        then
                note "allocator $name: no $runner here, not run"
                return 0
        fi

        # shellcheck disable=SC2086
        if ! $compiler $flags $extra $link -o "$work/$name" \
                src/test/allocator.c 2>"$work/$name.log"
        then
                note "allocator $name: did not build"
                cat "$work/$name.log"
                failed=1
                return 0
        fi

        if ! $runner "$work/$name" > "$work/$name.out"
        then
                note "allocator $name: the test reported failures"
                failed=1
        fi

        sed -n '1,/^shared-end$/p' "$work/$name.out" > "$work/$name.shared"

        if ! diff -u "$work/reference.shared" "$work/$name.shared" \
                > "$work/$name.diff"
        then
                note "allocator $name: differs from glibc"
                cat "$work/$name.diff"
                failed=1
                return 0
        fi

        note "allocator $name: agrees with glibc"
        sed -n '/^shared-end$/,$p' "$work/$name.out" | sed '1d;s/^/    /'
}

one x86_64  gcc                    ""             ""
one arm64   aarch64-linux-gnu-gcc  qemu-aarch64   "-mno-outline-atomics"
one riscv64 riscv64-linux-gnu-gcc  qemu-riscv64   "-march=rv64imafd_zicsr_zicntr -mabi=lp64d"

exit $failed
