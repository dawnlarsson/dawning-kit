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

if [ ! -f "$image" ]; then
        echo "  boot         no image at $image, skipped (sh build.sh)"
        exit 0
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
