#!/bin/sh
#
#       The whole thing, started.
#
#           sh src/test/boot.sh [path to bootx64.efi]
#
#       Everything else here tests a piece on the machine the tests run on.
#       This boots the image under qemu, lets the kernel exec /init -- which
#       is the shell, in the spark format, loaded by the loader in the kernel
#       and by nothing else -- and asks it questions down the serial line.
#
#       The answers are checked as transformed strings rather than as the
#       words that were typed, because the console echoes everything sent to
#       it: grepping the transcript for "hello" after sending "echo hello"
#       finds the echo whether or not a shell ever ran. Sending it through rev
#       or through arithmetic means the answer cannot be in the question.
#
#       The first line is thrown away on purpose. Piping into the serial
#       console loses the first few bytes of the first line, every time.
#
set -u

image=${1:-dist/bootx64.efi}

if ! command -v qemu-system-x86_64 > /dev/null 2>&1; then
        echo "  boot         no qemu-system-x86_64, skipped"
        exit 0
fi

#
#       Same reason the missing image is loud.
#
#       The boot below is wrapped in timeout, which a Mac does not have. When
#       it is missing the whole pipeline fails before qemu is reached, the
#       transcript is written empty, and every check below says the thing it
#       wanted is not in it -- eleven failures naming eleven features, none of
#       which was ever asked a question. That is worse than not running: it
#       points at the kernel for the absence of a shell utility.
#
if ! command -v timeout > /dev/null 2>&1; then
        echo "  boot         NOT RUN -- no timeout here"
        echo "               the boot is wrapped in it, and without it the"
        echo "               transcript comes out empty and every check below"
        echo "               fails naming a feature it never got to ask about."
        echo "               Run this where there is one: sh kit/onbox"
        exit 2
fi

if [ ! -f "$image" ]; then
        #
        #       Loud, and not zero.
        #
        #       This lane is the only one that runs on a processor without
        #       AVX, so it is the only one that can see an instruction the
        #       target cannot execute. It said "skipped" on every run for a
        #       day while exactly that bug went in, and the suite still
        #       printed that everything agreed, because a skip exited zero.
        #
        echo "  boot         NOT RUN -- no image at $image"
        echo "               this is the only lane that runs without AVX, so"
        echo "               nothing else here can see an instruction the"
        echo "               target cannot execute. Build one: sh build.sh"
        exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

pass=0
fail=0

cat > "$work/commands" <<'COMMANDS'
this line is eaten by the console
printf 'answer%s\n' $((6 * 7))
printf 'shift%s\n' $((1 << 4))
uname | rev
printf 'alpha\nbeta\n' | grep alpha | rev
seq 3 | tr '\n' ','
ls /dev/spark | rev
ls /shell | rev
echo written > /tmp/boot_probe
rev < /tmp/boot_probe
printf 'bytes%s\n' $(wc -c < /tmp/boot_probe)
poweroff
COMMANDS

timeout 180 qemu-system-x86_64 -m 2G -smp 2 -cpu Nehalem \
        -kernel "$image" -no-reboot -display none -serial stdio \
        -append "console=ttyS0 drm_client_lib.active=" \
        < "$work/commands" > "$work/transcript" 2>&1 || true

says()
{
        if grep -q -F -- "$2" "$work/transcript"; then
                pass=$((pass + 1))
                return 0
        fi

        fail=$((fail + 1))
        printf '  %-24s no %s in the transcript\n' "$1" "$2"
}

never()
{
        if grep -q -F -- "$2" "$work/transcript"; then
                fail=$((fail + 1))
                printf '  %-24s %s is in the transcript\n' "$1" "$2"
                return 0
        fi

        pass=$((pass + 1))
}

#
#       The two that say the machine rejected what we shipped it.
#
#       qemu runs this as -cpu Nehalem, which has SSE4.2 and no AVX, and that
#       is the point: an AVX instruction on a path with no feature test in
#       front of it is an invalid opcode here and a working program on the
#       machine this was built on. It happened -- vmovdqu on the small path of
#       memory_copy_fast, which is VEX encoded however narrow its operands --
#       and init died of it before anything else in this file could run.
#
#       Without these two the failure reads as six assertions about arithmetic
#       and uname going missing, which is a long way from the cause.
#
never 'no invalid opcode'  'invalid opcode'
never 'init survived'      'Attempted to kill init'
never 'no kernel panic'    'Kernel panic'

says 'init started'        'Run /init as init process'
says 'arithmetic'          'answer42'
says 'shifts'              'shift16'
says 'uname'               'xuniL'
says 'grep by its name'    'ahpla'
says 'seq and tr'          '1,2,3,'
says 'the spark device'    'kraps/ved/'
says 'the shell on disk'   'llehs/'
says 'a file written'      'nettirw'
says 'and measured'        'bytes8'
says 'powered down'        'reboot: Power down'

never 'no panic'           'Kernel panic'
never 'no oops'            'Oops:'
never 'nothing refused'    'Permission denied'

printf '  %-12s %s of %s\n' boot "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'boot %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
