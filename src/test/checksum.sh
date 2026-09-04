#!/bin/sh
#
#       POSIX cksum and the optional kernel-backed digest aliases against GNU
#       coreutils.
#
#       The fixtures cross the shared fallback buffer, exercise regular and
#       pipe input, and put portable filename escapes at both parities in a
#       check file. Output, diagnostics and status are compared together.

set -u

bin=${1:-/tmp/multi}
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

for tool in b2sum cksum md5sum sha1sum sha224sum sha256sum sha384sum sha512sum; do
        if ! command -v "$tool" >/dev/null 2>&1; then
                echo "  checksum: NOT RUN -- no GNU $tool reference"
                exit 2
        fi
done

pass=0
total=0

report()
{
        total=$((total + 1))

        if cmp -s "$work/want" "$work/got" &&
           cmp -s "$work/want.err" "$work/got.err" &&
           [ "$want_status" = "$got_status" ]; then
                pass=$((pass + 1))
                return
        fi

        printf '  %-28s want [%s][%s] got [%s][%s]\n' "$1" \
                "$(head -c 44 "$work/want" | tr '\n' '|')" "$want_status" \
                "$(head -c 44 "$work/got" | tr '\n' '|')" "$got_status"
        if ! cmp -s "$work/want.err" "$work/got.err"; then
                printf '      stderr want [%s] got [%s]\n' \
                        "$(head -c 54 "$work/want.err" | tr '\n' '|')" \
                        "$(head -c 54 "$work/got.err" | tr '\n' '|')"
        fi
}

same()
{
        name=$1
        tool=$2
        shift 2

        if "$tool" "$@" >"$work/want" 2>"$work/want.err"; then
                want_status=0
        else
                want_status=$?
        fi

        if "$bin/$tool" "$@" >"$work/got" 2>"$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        report "$name"
}

fed()
{
        name=$1
        feed=$2
        tool=$3
        shift 3

        if cat "$feed" | "$tool" "$@" >"$work/want" 2>"$work/want.err"; then
                want_status=0
        else
                want_status=$?
        fi

        if cat "$feed" | "$bin/$tool" "$@" >"$work/got" 2>"$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi

        report "$name"
}

cd "$work" || exit 1
: > empty
printf 'abc' > abc
head -c 262157 /dev/zero | tr '\0' q > large

for tool in b2sum md5sum sha1sum sha224sum sha256sum sha384sum sha512sum; do
        same "$tool empty" "$tool" empty
        same "$tool regular" "$tool" abc large
        fed "$tool stdin bytes" large "$tool"
done

# cksum's shared input block is 128 KiB. Pin both edges of a refill, the
# no-name stdin form above, explicit stdin, multiple operands and continued
# work after an open failure.
head -c 131071 large > crc-short
head -c 131072 large > crc-exact
head -c 131073 large > crc-long
head -c 262159 /dev/urandom > crc-random
head -c 127 crc-random > crc-vector-short
head -c 128 crc-random > crc-vector-exact
head -c 129 crc-random > crc-vector-long
head -c 191 crc-random > crc-vector-twice-short
head -c 192 crc-random > crc-vector-twice-exact
head -c 193 crc-random > crc-vector-twice-long
same 'cksum refill edges' cksum crc-short crc-exact crc-long
same 'cksum vector edges' cksum crc-vector-short crc-vector-exact crc-vector-long \
        crc-vector-twice-short crc-vector-twice-exact crc-vector-twice-long
same 'cksum high bytes' cksum crc-random
fed 'cksum explicit stdin' large cksum -
same 'cksum algorithm short' cksum -a crc abc
same 'cksum algorithm long' cksum --algorithm=crc abc
same 'cksum algorithm after file' cksum abc --algorithm=crc
same 'cksum missing continues' cksum abc absent large
mkdir crc-directory
same 'cksum read error' cksum crc-directory

total=$((total + 1))
if "$bin/cksum" --algorithm=sha256 abc > "$work/reject.out" \
     2> "$work/reject.err"; then
        echo '  cksum unsupported algorithm  was silently accepted'
elif [ ! -s "$work/reject.out" ] &&
     grep -q 'not supported' "$work/reject.err"; then
        pass=$((pass + 1))
else
        echo '  cksum unsupported algorithm  was not clearly rejected'
fi

b2sum abc > b2.list
same 'b2 check' b2sum -c b2.list
md5sum abc > md5.list
same 'md5 check' md5sum -c md5.list

same 'sha256 binary marker' sha256sum -b abc
same 'sha256 last text marker' sha256sum -b -t abc
same 'sha256 last binary marker' sha256sum -t -b abc
same 'sha256 missing input' sha256sum absent
mkdir unreadable-directory
same 'sha256 read failure' sha256sum unreadable-directory

# A capability/policy failure may take the next transfer path; a genuine I/O
# failure must escape immediately instead of being hidden by a retry.  Keep
# this optional because not every test host has ptrace/strace available.
if command -v strace >/dev/null 2>&1 &&
   strace -qq -o "$work/strace.probe" -e trace=none true 2>/dev/null; then
        sha256sum abc > "$work/want"
        : > "$work/want.err"
        want_status=0
        if strace -qq -o "$work/einval.trace" \
           -e inject=sendfile:error=EINVAL:when=1 \
           -e trace=sendfile,splice,read \
           "$bin/sha256sum" abc > "$work/got" 2> "$work/got.err"; then
                got_status=0
        else
                got_status=$?
        fi
        report 'sendfile capability fallback'

        total=$((total + 1))
        if ! strace -qq -o "$work/eio.trace" \
             -e inject=sendfile:error=EIO:when=1 \
             -e trace=sendfile,splice,read \
             "$bin/sha256sum" abc > "$work/eio.out" \
             2> "$work/eio.err" &&
           [ ! -s "$work/eio.out" ] && [ -s "$work/eio.err" ] &&
           grep -q '^sendfile(.*EIO' "$work/eio.trace" &&
           ! grep -q '^splice(' "$work/eio.trace"; then
                pass=$((pass + 1))
        else
                echo '  sendfile data failure       was hidden or retried'
        fi
fi

# The backslash sits at byte one and the newline at byte two, so the in-place
# decoder cannot accidentally pass by either based on source/destination
# parity while compacting GNU's portable escapes.
slash_name='a\b'
newline_name=$(printf 'ab\nc')
printf 'slash' > "$slash_name"
printf 'newline' > "$newline_name"
same 'escaped filenames output' sha256sum "$slash_name" "$newline_name"

sha256sum "$slash_name" "$newline_name" > escaped.list
same 'escaped filenames check' sha256sum -c escaped.list
same 'check quiet' sha256sum -c --quiet escaped.list
same 'check status' sha256sum -c --status escaped.list

printf 'changed' > "$newline_name"
same 'check mismatch' sha256sum -c escaped.list
printf 'newline' > "$newline_name"

printf 'not a checksum\n' >> escaped.list
same 'check malformed ordinary' sha256sum -c escaped.list
same 'check malformed strict' sha256sum -c --strict escaped.list
same 'check malformed warning' sha256sum -c --warn escaped.list

printf 'not a checksum\n' > malformed.list
same 'check malformed only' sha256sum -c malformed.list
same 'check malformed only warn' sha256sum -c --warn malformed.list

digest=$(printf x | sha256sum | cut -d' ' -f1)
printf '%s  absent\n' "$digest" > missing.list
same 'check missing' sha256sum -c missing.list
same 'check ignore missing' sha256sum -c --ignore-missing missing.list

for words in 'sha256sum --tag abc' 'sha256sum --zero abc' \
             'b2sum --length=256 abc'; do
        # shellcheck disable=SC2086
        set -- $words
        tool=$1
        shift
        total=$((total + 1))

        if "$bin/$tool" "$@" >"$work/reject.out" 2>"$work/reject.err"; then
                printf '  %-28s accepted an unsupported mode\n' "$words"
        elif [ ! -s "$work/reject.out" ] &&
             grep -q 'not supported' "$work/reject.err"; then
                pass=$((pass + 1))
        else
                printf '  %-28s did not reject transparently\n' "$words"
        fi
done

printf '  %-12s %s of %s\n' listed "$pass" "$total"
[ -z "${TEST_TALLY:-}" ] ||
        printf 'checksum-listed %s %s\n' "$pass" "$total" >> "$TEST_TALLY"

[ "$pass" = "$total" ]
