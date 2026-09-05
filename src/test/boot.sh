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
        echo "  boot         NOT RUN -- no qemu-system-x86_64"
        exit 2
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
printf 'bootenv-%s-%s\n' "${HOME-unset}" "${TERM-unset}"
export SPARK_CACHE=first
/shell -c 'printf "cache-%s\n" "$SPARK_CACHE"'
export SPARK_CACHE=second
/shell -c 'printf "cache-%s\n" "$SPARK_CACHE"'
export SPARK_OWNER=base
/shell -c ':'
( export SPARK_OWNER=child; /shell -c ':' )
export SPARK_OWNER=parent
/shell -c 'printf "owner-%s\n" "$SPARK_OWNER"'
ls /monitor.sh | rev
printf 'alias-%s\n' "$(readlink /mointor.sh | rev)"
printf 'path-%s\n' "$(readlink /bin/monitor.sh | rev)"
printf 'mount-link-%s\n' "$(readlink /bin/mount | rev)"
printf 'blkid-link-%s\n' "$(readlink /sbin/blkid | rev)"
printf 'bash-link-%s\n' "$(readlink /bin/bash | rev)"
printf 'dash-link-%s\n' "$(readlink /bin/dash | rev)"
cat > /tmp/bash-entry <<'BASH_ENTRY'
#!/bin/bash
set -eu
shopt -s lastpipe
printf 'ready\n' | read value
printf 'shell-gap-%s\n' "$value"
BASH_ENTRY
chmod +x /tmp/bash-entry
/tmp/bash-entry
printf 'boot_start=ready\n' > /tmp/bash-env
BASH_ENV=/tmp/bash-env /bin/bash -c 'printf "startup-%s\n" "$boot_start"'
cat > /tmp/onecmd <<'ONECMD'
printf 'onecmd-%s\n' $((7 * 9))
echo late > /tmp/onecmd-late
ONECMD
/bin/bash -t /tmp/onecmd
[ ! -e /tmp/onecmd-late ] && printf 'dne-dmceno\n' | rev
/bin/bash -c 'f() { return -1; }; f; printf "return-%s\n" "$?"'
/bin/dash -c 'trap '\''printf "dot-exit-%s\n" "$?"'\'' EXIT; . /tmp/missing-dot-file' 2>/dev/null
/bin/bash -c 'a=([2]=ready); declare -n n=a; printf "nameref-%s\n" "${n[2]}"'
/bin/bash -c 'a=([2]=old); declare -n n="a[2]"; n=ready; printf "element-ref-%s\n" "${a[2]}"'
/bin/bash -c 'set -k; f() { printf "keyword-%s-%s\n" "$MWKEY" "$*"; }; f one MWKEY=ready two'
/bin/bash -c 'X=old; X=temporary :; printf "prefix-kept-%s\n" "$X"; X=ready export X; printf "prefix-export-%s\n" "$X"'
/bin/bash -pc 'case $- in *p*) printf "privileged-%s\n" ready;; esac; set +p; case $- in *p*) :;; *) printf "privileged-%s\n" dropped;; esac'
cat > /tmp/nested-here <<'NESTED_HERE'
value=$(cat <<EOF
)
EOF
)
printf 'here-%s\n' "$value"
NESTED_HERE
/bin/dash /tmp/nested-here
/bin/mountpoint -q / && printf 'storage-root\n'
printf 'storage-target-%s\n' "$(/usr/bin/findmnt -n -r -o TARGET -T /)"
mointor.sh 1 3 2> /tmp/monitor.err
printf 'rotinom%s\n' $? | rev
[ ! -s /tmp/monitor.err ] && printf 'naelc-rotinom\n' | rev
printf 'echo plain-$1\nexit 17\n' > /tmp/plain-script
chmod +x /tmp/plain-script
/tmp/plain-script alpha
printf 'txet%s\n' $? | rev
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
#       memory_copy_apart, which is VEX encoded however narrow its operands --
#       and init died of it before anything else in this file could run.
#
#       Without these two the failure reads as six assertions about arithmetic
#       and uname going missing, which is a long way from the cause.
#
never 'no invalid opcode'  'invalid opcode'
never 'no protection fault' 'general protection fault'
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
says 'empty init environment' 'bootenv-unset-unset'
says 'cached environment'   'cache-first'
says 'changed environment'  'cache-second'
says 'cache process owner'  'owner-parent'
says 'monitor at root'     'hs.rotinom/'
says 'mointor alias'       'alias-hs.rotinom'
says 'monitor linked'      'path-hs.rotinom/..'
says 'mount linked'        'mount-link-llehs/..'
says 'blkid linked'        'blkid-link-llehs/..'
says 'bash linked'         'bash-link-llehs/..'
says 'dash linked'         'dash-link-llehs/..'
says 'bash shebang policy' 'shell-gap-ready'
says 'BASH_ENV startup'    'startup-ready'
says 'onecmd first line'   'onecmd-63'
says 'onecmd stopped'      'onecmd-end'
says 'Bash signed return'  'return-255'
says 'dash source failure trap' 'dot-exit-2'
says 'Bash nameref array'  'nameref-ready'
says 'Bash element nameref' 'element-ref-ready'
says 'Bash keyword assignments' 'keyword-ready-one two'
says 'Bash prefix lifetime' 'prefix-kept-old'
says 'Bash prefix export'  'prefix-export-ready'
says 'Bash privileged mode' 'privileged-ready'
says 'Bash privileged reset' 'privileged-dropped'
says 'nested heredoc boundary' 'here-)'
says 'root is mounted'     'storage-root'
says 'findmnt linked'      'storage-target-/'
says 'monitor ran'         '0monitor'
says 'monitor was visible' 'cpu%'
says 'monitor was clean'   'monitor-clean'
says 'plain text script'   'plain-alpha'
says 'plain text status'   '71text'
says 'powered down'        'reboot: Power down'

never 'no panic'           'Kernel panic'
never 'no oops'            'Oops:'
never 'nothing refused'    'Permission denied'
never 'no missing command' 'failed with error: -2'

#
#       And the other end of the same argument.
#
#       Everything above runs on a Nehalem, which is the floor: it catches an
#       instruction the target cannot execute. It cannot catch the opposite,
#       an instruction the target executes perfectly well and something
#       underneath it cannot -- and that is a real failure mode, not a
#       hypothetical one.
#
#       A displaced memcpy with an AVX-512 body booted here and died on a host
#       that has AVX-512, in the fbdev path, on
#       vmovdqu64 -256(%r11), %zmm4. Plain memcpy gets called on ioremap'd
#       memory; a device mapping is trapped and emulated; KVM's emulator has
#       no EVEX. Nehalem never reaches that instruction because it never takes
#       that path, so this file said seventeen of seventeen while the kernel
#       did not boot on the machine it was built on.
#
#       So: once more on the widest processor there is, with KVM, which is
#       what puts a real emulator behind the device mapping. Skipped rather
#       than failed where there is no KVM -- a Mac has none -- because the
#       floor pass above is the one that must always run.
#
if [ -r /dev/kvm ]; then
        timeout 180 qemu-system-x86_64 -m 2G -smp 2 -cpu host -enable-kvm \
                -kernel "$image" -no-reboot -display none -serial stdio \
                -append "console=ttyS0 drm_client_lib.active=" \
                < "$work/commands" > "$work/transcript" 2>&1 || true

        says 'init started on this host'   'Run /init as init process'
        says 'powered down on this host'   'reboot: Power down'
        never 'nothing went unemulated'    'emulation failure'
        never 'no invalid opcode here'     'invalid opcode'
        never 'no protection fault here'   'general protection fault'
        never 'no oops here'               'Oops:'
        never 'no panic here'              'Kernel panic'
else
        printf '  %-24s no /dev/kvm, the widest pass did not run\n' 'this host'
fi

printf '  %-12s %s of %s\n' boot "$pass" "$((pass + fail))"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'boot %s %s\n' "$pass" "$((pass + fail))" >> "$TEST_TALLY"

[ "$fail" = 0 ]
