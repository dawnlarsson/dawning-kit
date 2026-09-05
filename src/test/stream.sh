#!/bin/sh
#
#       <stdio.h>'s FILE against glibc's, on three machines.
#
#           sh src/test/stream.sh
#
#       Everything here is a comparison. glibc is on the machine, it defines
#       what these functions answer, and a stdio that is merely self-consistent
#       is worth nothing -- so each of the three targets is built, run, and
#       diffed against a reference binary built by the host compiler from the
#       same source body.
#
#       Three programs, because three different things are being asked:
#
#         stream_body            files: seeks, ends, line reads, item counts,
#                                and after every call ftell, feof and ferror.
#                                Both the trace and the files produced are
#                                compared.
#         stream_standard_body   the three standard streams, once with regular
#                                files on them and once with pipes. A pipe
#                                cannot seek and that is the point.
#         stream_buffering_body  which buffering policy a terminal produces
#                                and which a file produces, observed from
#                                outside the process while it is paused.
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

work=$(mktemp -d "${TMPDIR:-/tmp}/dawning-stream.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

report() {
        if [ "$1" = ok ]; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-10s %-34s %s\n' stream "$2" "$3"
}

finish() {
        printf '  %-12s %s of %s\n' stream "$pass" "$((pass + fail))"
        [ -z "${TEST_TALLY:-}" ] ||
                printf 'stream %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"
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
        extra=$5

        case $target in
        arm64)   set -- -mno-outline-atomics ;;
        riscv64) set -- -march=rv64imafd_zicsr_zicntr -mabi=lp64d ;;
        *)       set -- ;;
        esac

        # shellcheck disable=SC2086
        $cross -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
                -fno-builtin "$@" $extra -w -T kit/spark.ld -Wl,-e,_start \
                -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
                -o "$output" "$source" 2> "$output.err"
}

#
#       The reference, built once.
#
mkdir -p "$work/reference"

if ! $host -O2 -w -DWORK="\"$work/reference\"" \
        -o "$work/t.reference" src/test/stream_reference.c 2> "$work/ref.err"; then
        printf '  the glibc reference did not build -- not run\n'
        sed 's/^/    /' "$work/ref.err" | head -10
        exit 2
fi

$host -O2 -w -o "$work/s.reference" src/test/stream_standard_reference.c \
        2>> "$work/ref.err" || exit 2
$host -O2 -w -o "$work/b.reference" src/test/stream_buffering_reference.c \
        2>> "$work/ref.err" || exit 2

"$work/t.reference" > "$work/t.reference.out" 2>&1

printf 'alpha\nbeta\n\ngamma' > "$work/input.txt"

"$work/s.reference" < "$work/input.txt" \
        > "$work/s.reference.out" 2> "$work/s.reference.err" 3> "$work/s.reference.trace"

printf 'alpha\nbeta\n\ngamma' |
        "$work/s.reference" 2> "$work/p.reference.err" 3> "$work/p.reference.trace" |
        cat > "$work/p.reference.out"

#
#       What is on disk while the program is still holding it. script's own
#       typescript is buffered unless it is told to flush, which is why -f is
#       not optional here.
#
terminal_ready=no

if command -v script > /dev/null 2>&1; then
        terminal_ready=yes
fi

held_on_file() {
        rm -f "$work/held.out"
        "$@" > "$work/held.out" 2>/dev/null &
        held=$!
        sleep 0.35
        wc -c < "$work/held.out" | tr -d ' '
        wait $held 2>/dev/null
}

held_on_terminal() {
        rm -f "$work/held.tty"
        script -q -f -c "$*" "$work/held.tty" > /dev/null 2>&1 &
        held=$!
        sleep 0.35
        # The typescript carries a header line of its own; only what follows
        # the program's own first byte is being counted, so look for the text.
        if grep -q onetwo "$work/held.tty" 2>/dev/null; then
                echo yes
        else
                echo no
        fi
        wait $held 2>/dev/null
}

reference_on_file=$(held_on_file "$work/b.reference")
report "$([ "$reference_on_file" = 0 ] && echo ok || echo no)" \
        'reference blocks on a file' "held $reference_on_file bytes, wanted 0"

if [ "$terminal_ready" = yes ]; then
        reference_on_terminal=$(held_on_terminal "$work/b.reference")
        report "$([ "$reference_on_terminal" = yes ] && echo ok || echo no)" \
                'reference lines on a terminal' 'nothing had been written'
fi

#
#       Each target: build, run, diff.
#
for target in x86_64 arm64 riscv64; do
        case $target in
        x86_64)
                cross=$host
                runner=""
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
                printf '  %-10s %s\n' stream "$target: no $cross, skipped"
                continue
        fi

        if [ -n "$runner" ] && ! command -v "$runner" > /dev/null 2>&1; then
                printf '  %-10s %s\n' stream "$target: no $runner, skipped"
                continue
        fi

        mkdir -p "$work/$target"

        if ! build_ours src/test/stream.c "$work/t.$target" "$target" "$cross" \
                "-DWORK=\"$work/$target\""; then
                report no "$target build" "$(head -3 "$work/t.$target.err")"
                continue
        fi

        if ! build_ours src/test/stream_standard.c "$work/s.$target" "$target" \
                "$cross" ""; then
                report no "$target standard build" "$(head -3 "$work/s.$target.err")"
                continue
        fi

        if ! build_ours src/test/stream_buffering.c "$work/b.$target" "$target" \
                "$cross" ""; then
                report no "$target buffering build" "$(head -3 "$work/b.$target.err")"
                continue
        fi

        if $runner "$work/t.$target" > "$work/t.$target.out" 2>&1; then
                report ok "$target internal invariants"
        else
                report no "$target internal invariants" "test returned failure"
        fi

        if diff -q "$work/t.reference.out" "$work/t.$target.out" > /dev/null; then
                report ok "$target file trace"
        else
                report no "$target file trace" \
                        "$(diff "$work/t.reference.out" "$work/t.$target.out" | head -6 | tr '\n' '|')"
        fi

        if diff -r "$work/reference" "$work/$target" > /dev/null 2>&1; then
                report ok "$target files produced"
        else
                report no "$target files produced" \
                        "$(diff -r "$work/reference" "$work/$target" 2>&1 | head -4 | tr '\n' '|')"
        fi

        $runner "$work/s.$target" < "$work/input.txt" \
                > "$work/s.$target.out" 2> "$work/s.$target.err" 3> "$work/s.$target.trace"

        for part in out err trace; do
                if diff -q "$work/s.reference.$part" "$work/s.$target.$part" > /dev/null; then
                        report ok "$target standard on files ($part)"
                else
                        report no "$target standard on files ($part)" \
                                "$(diff "$work/s.reference.$part" "$work/s.$target.$part" | head -4 | tr '\n' '|')"
                fi
        done

        printf 'alpha\nbeta\n\ngamma' |
                $runner "$work/s.$target" \
                        2> "$work/p.$target.err" 3> "$work/p.$target.trace" |
                cat > "$work/p.$target.out"

        for part in out err trace; do
                if diff -q "$work/p.reference.$part" "$work/p.$target.$part" > /dev/null; then
                        report ok "$target standard on pipes ($part)"
                else
                        report no "$target standard on pipes ($part)" \
                                "$(diff "$work/p.reference.$part" "$work/p.$target.$part" | head -4 | tr '\n' '|')"
                fi
        done

        held=$(held_on_file $runner "$work/b.$target")
        report "$([ "$held" = 0 ] && echo ok || echo no)" \
                "$target blocks on a file" "held $held bytes, wanted 0"

        if [ "$terminal_ready" = yes ]; then
                held=$(held_on_terminal "$runner $work/b.$target")
                report "$([ "$held" = yes ] && echo ok || echo no)" \
                        "$target lines on a terminal" 'nothing had been written'
        fi
done

if [ "$terminal_ready" != yes ]; then
        printf '  %-10s %s\n' stream 'no script(1), the terminal half did not run'
fi

finish
