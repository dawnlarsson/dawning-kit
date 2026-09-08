"""util-linux grammar: the kernel-policy utilities and the storage surface.

Every program the reference documents is walked by option grammar. Three
shapes cover what needs root, a device or a namespace: the unprivileged
behaviour is compared exactly (parsing, refusals, statuses); effects run in
user namespaces where the box allows it (`unshare -Urm` for mounts, `-Uri`
for System V IPC, so shared kernel state never leaks between workers); and
fixtures are byte-built (ext4 superblock, swap header, ISO9660 descriptor,
magic-only impostors) so probing needs no mkfs. Programs that must observe a
live holder (locks, descriptors, IPC objects, a namespace target) run as
bash-mode scripts: the script resolves the tool under test from its own
/proc/self/exe -- the system tool when bash is running it, the multicall
binary under `exec -a NAME` when ours is -- so both sides observe the same
fixture through their own implementation.

CHECKS carries the three properties the differential cannot express: the
130-name util-linux 2.42.2 denominator, the rfkill fake-sysfs oracle and the
lscpu summary over synthetic topologies (257 CPUs, NUMA node -1, an
exhausted arena), generated from function-name markers at run time.
"""
import os
import re
import shlex
import struct
import subprocess
import tempfile
import uuid
from pathlib import Path

from differential import Utility, Option, INPUTS, FIXTURES

# ---------------------------------------------------------------------------
#       Constants shared by fixtures and grammars.
# ---------------------------------------------------------------------------

UL_UUID = "00112233-4455-6677-8899-aabbccddeeff"
UL_UUID2 = "ffeeddcc-bbaa-9988-7766-554433221100"
UL_EXT4_UUID = "12345678-1234-5678-9abc-def012345678"
UL_SWAP_UUID = "87654321-4321-8765-cba9-876543210fed"
UL_MISSING_UUID = "00000000-0000-0000-0000-000000000000"
UL_OBS = "./observe"
UL_PIDS = ("1", "bad", "999999999", "0", "-1")


# ---------------------------------------------------------------------------
#       Byte-built images: what blkid, wipefs, swaplabel and isosize probe.
# ---------------------------------------------------------------------------

def ul_ext4_image(size=256 * 1024, label=b"moondata", fs_uuid=UL_EXT4_UUID):
    """A consistent ext4 superblock at 1 KiB; blkid and wipefs accept it."""
    image = bytearray(size)
    sb = bytearray(1024)
    blocks = size // 1024
    for offset, value in ((0x00, 32), (0x04, blocks), (0x08, 12), (0x0C, blocks - 56),
                          (0x10, 21), (0x14, 1), (0x18, 0), (0x1C, 0), (0x20, 8192),
                          (0x24, 8192), (0x28, 32), (0x2C, 0), (0x30, 1700000000),
                          (0x40, 0), (0x44, 0), (0x48, 0), (0x4C, 1), (0x54, 11),
                          (0x5C, 0x3C), (0x60, 0x242), (0x64, 0x7B)):
        struct.pack_into("<I", sb, offset, value)
    for offset, value in ((0x34, 0), (0x36, 0xFFFF), (0x38, 0xEF53), (0x3A, 1),
                          (0x3C, 1), (0x3E, 0), (0x50, 0), (0x52, 0), (0x58, 256),
                          (0x5A, 0)):
        struct.pack_into("<H", sb, offset, value)
    sb[0x68:0x78] = uuid.UUID(fs_uuid).bytes
    sb[0x78:0x78 + len(label)] = label
    image[1024:2048] = sb
    return bytes(image)


def ul_swap_image(size=128 * 1024, page=4096, label=b"moonswap", swap_uuid=UL_SWAP_UUID):
    image = bytearray(size)
    struct.pack_into("<III", image, 1024, 1, size // page - 1, 0)
    image[1036:1052] = uuid.UUID(swap_uuid).bytes
    image[1052:1052 + len(label)] = label
    image[page - 10:page] = b"SWAPSPACE2"
    return bytes(image)


def ul_iso_image():
    image = bytearray(80 * 2048)
    at = 16 * 2048
    image[at] = 1
    image[at + 1:at + 6] = b"CD001"
    image[at + 6] = 1
    image[at + 40:at + 72] = b"MOONWATER" + b" " * 23
    image[at + 80:at + 84] = struct.pack("<I", 64)
    image[at + 84:at + 88] = struct.pack(">I", 64)
    image[at + 128:at + 130] = struct.pack("<H", 2048)
    image[at + 130:at + 132] = struct.pack(">H", 2048)
    return bytes(image)


def ul_magic_image(size, *pieces):
    """Only the public magic of a filesystem: not a filesystem."""
    image = bytearray(size)
    for offset, magic in pieces:
        image[offset:offset + len(magic)] = magic
    return bytes(image)


def ul_multi_image():
    """An ext4 superblock plus a swap magic: two signatures in probe order."""
    image = bytearray(ul_ext4_image())
    image[1024:1028] = b"\x01\x00\x00\x00"
    image[4086:4096] = b"SWAPSPACE2"
    return bytes(image)


# ---------------------------------------------------------------------------
#       The observer: what a command run by a policy tool reports about
#       itself, using only system tools identical on both sides.
# ---------------------------------------------------------------------------

UL_OBSERVE = r"""#!/bin/sh
mode=$1
shift
case $mode in
echo) printf 'ran:%s\n' "$*" ;;
exit7) exit 7 ;;
signal) kill -TERM $$ ;;
affinity) grep Cpus_allowed_list /proc/self/status ;;
sched) chrt -p $$ | sed 's/pid [0-9]*/pid PID/g' ;;
io) ionice -p $$ ;;
clamp) uclampset -p $$ | sed 's/.* util_clamp:/util_clamp:/' ;;
oom) cat /proc/self/oom_score_adj ;;
limits) cat /proc/self/limits ;;
session) sid=$(ps -o sid= -p $$ | tr -d ' '); pg=$(ps -o pgid= -p $$ | tr -d ' ')
        [ "$sid" = "$$" ] && echo sid=self || echo sid=other
        [ "$pg" = "$$" ] && echo pgid=self || echo pgid=other ;;
ids) id -u; id -g; cat /proc/self/uid_map /proc/self/gid_map /proc/self/setgroups ;;
ns) for n in user mnt net pid uts ipc cgroup time; do
        readlink /proc/self/ns/$n | sed -E 's/\[[0-9]+\]/[ID]/'
    done
    id -u; wc -l < /proc/net/dev; [ "$$" = 1 ] && echo pid1 || echo pidN ;;
nice) ps -o ni= -p $$ | tr -d ' ' ;;
arch) uname -m ;;
personality) cat /proc/self/personality ;;
pwd) pwd ;;
time) cat /proc/self/timens_offsets ;;
dump) setpriv -d ;;
env) env | sort ;;
flock_nb) flock -n -E 42 lock true; echo "conflict:$?" ;;
*) echo "observe: unknown mode $mode" >&2; exit 3 ;;
esac
"""

UL_SMALL = {
    "data": b"content\n",
    "lock": b"",
    "rolock": ("mode", b"", 0o444),
    "in": b"abcdefgh",
    "out": b"00000000",
    "ranges": b"0:0:2\n::2\n",
    "a": b"A",
    "b": b"B",
    "a.txt": b"alpha\nbeta\n",
    "zeros": bytes(12288),
    "dir/inside": b"nested\n",
    "dir2/x": b"",
    "link": ("link", "a.txt"),
    "dirlink": ("link", "dir"),
    "rootlink": ("link", "/"),
    "dangling": ("link", "nowhere"),
    "observe": ("mode", UL_OBSERVE.encode(), 0o755),
}

FIXTURES["ul"] = UL_SMALL
FIXTURES["ul_images"] = {
    **UL_SMALL,
    "swap.img": ul_swap_image(),
    "ext4.img": ul_ext4_image(),
    "iso.img": ul_iso_image(),
    "multi.img": ul_multi_image(),
    "nosig.img": bytes(65536),
    "short.img": bytes(511),
    "hostile.img": b"\xff" * 8,
    "ntfs-magic.img": ul_magic_image(512, (0, b"\xeb"), (3, b"NTFS    "), (510, b"\x55\xaa")),
    "btrfs-magic.img": ul_magic_image(69632, (65600, b"_BHRfS_M")),
    "xfs-magic.img": ul_magic_image(4096, (0, b"XFSB")),
    "f2fs-magic.img": ul_magic_image(4096, (1024, b"\x10\x20\xf5\xf2")),
    "squashfs-magic.img": ul_magic_image(4096, (0, b"hsqs")),
    "udf-magic.img": ul_magic_image(65536, (32768, b"\x00BEA01\x01"), (34816, b"\x00NSR03\x01"),
                                    (36864, b"\x00TEA01\x01")),
    "luks-magic.img": ul_magic_image(4096, (0, b"LUKS\xba\xbe\x00\x01")),
}

INPUTS["ul_bits"] = b"0-30:3\n2\n~1\n0-12:3\n"
INPUTS["ul_masks"] = b"0xeec2\n,00300000,03000000,30000003\n"
INPUTS["ul_bad_bits"] = b"1\nnone\n"


# ---------------------------------------------------------------------------
#       Normalisers: the few columns that legitimately change between the
#       reference run and ours a few milliseconds later.
# ---------------------------------------------------------------------------

def ul_normalizer(*rules):
    compiled = [(re.compile(pattern), replacement) for pattern, replacement in rules]

    def normalize(channel, data):
        if channel != "stdout":
            return data
        for pattern, replacement in compiled:
            data = pattern.sub(replacement, data)
        return data
    return normalize


ul_norm_lscpu = ul_normalizer(
    (rb"(scaling MHz:\s+)\d+%", rb"\1N%"),
    (rb"(scaling MHz:\", \"data\": \")\d+%", rb"\1N%"),
    (rb"\b\d{3,4}\.\d{4}\b", b"MHZ.N"),
    (rb"\b\d{3,4}\.\d{2}\b", b"BOGO.N"))
ul_norm_clocks = ul_normalizer(
    (rb"\b\d+\.\d{9}\b", b"T.N"),
    (rb"\b\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{9}[+-]\d\d:\d\d", b"ISO"),
    (rb"\b\d+\.\d{9}(?=\b|,|\s|\")", b"T.N"),
    (rb"(\d+y )?(\d+d )?\s*\d+h\s+\d+m\s+\d+s \d+ns", b"REL"))
ul_norm_lsblk = ul_normalizer(
    (rb"\b\d+(\.\d+)?[KMGTP]?\s+\d+%", b"AVAIL USE%"),
    (rb"(\"fsavail\": )(\"[^\"]*\"|\d+|null)", rb"\1AVAIL"),
    (rb"(\"fsused\": )(\"[^\"]*\"|\d+|null)", rb"\1USED"),
    (rb"(\"fsuse%\": )(\"[^\"]*\"|null)", rb"\1USE"),
    (rb"(FSAVAIL=)\"[^\"]*\"", rb"\1AVAIL"),
    (rb"(FSUSED=)\"[^\"]*\"", rb"\1USED"),
    (rb"(FSUSE%=)\"[^\"]*\"", rb"\1USE"))
ul_norm_live = ul_normalizer(
    (rb"\b\d{5,}\b", b"N"),
    (rb"\b40265\d{5}\b", b"NS"),
    (rb"0x[0-9a-f]{8}\b", b"0xKEY"),
    (rb"\b\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d", b"ISO"),
    (rb"\b[A-Z][a-z]{2} [A-Z][a-z]{2}\s+\d+ \d\d:\d\d:\d\d \d{4}\b", b"DATE"),
    (rb"\b[A-Z][a-z]{2} ?\d{1,2} \d\d:\d\d\b", b"DATE"),
    (rb"\b\d{1,2}:\d\d(:\d\d)?\b", b"TIME"))
ul_norm_mounts = ul_normalizer(
    (rb"\b0:\d+\b", b"0:N"),
    (rb"\b\d{3,}\b", b"N"))
ul_norm_flock = ul_normalizer((rb"took [0-9.]+ seconds", b"took TIME seconds"))
ul_norm_pids = ul_normalizer((rb"\b\d{5,}\b", b"N"))


# ---------------------------------------------------------------------------
#       Live fixtures: a script that resolves the tool under test.
# ---------------------------------------------------------------------------

def ul_words(argv):
    """Quoted words; @NAME placeholders become the script's "$name"."""
    out = []
    for word in argv:
        if word.startswith("@") and word[1:].isalpha() and word[1:].isupper():
            out.append('"$' + word[1:].lower() + '"')
        elif word.startswith("@") and ":" in word:
            head, tail = word.split(":", 1)
            out.append('"$' + head[1:].lower() + ":" + shlex.quote(tail) + '"')
        else:
            out.append(shlex.quote(word))
    return " ".join(out)


def ul_live(name, body, wrap=None, observe=False):
    """A bash-mode script. Fixture tools are spelled `env NAME`: under our
    shell a bare name would dispatch to our own implementation, and the
    fixture must be built by the system tools on both sides. The system tool serves the reference shell; the
    multicall binary, invoked under NAME, serves ours. wrap re-executes the
    body inside a namespace so effects never touch shared kernel state."""
    prelude = ('exe=$(readlink /proc/$$/exe)\n'
               'case $exe in */bash|*/dash|*/sh) tool=$(command -v %s) ;; *) tool=$exe ;; esac\n' % name)
    setup = ""
    if observe:
        setup = "cat > observe <<'UL_OBSERVE'\n" + UL_OBSERVE + "UL_OBSERVE\nchmod 755 observe\n"
    run = 'run() { (exec -a %s "$tool" "$@"); }\n' % name
    if wrap is None:
        return prelude + setup + run + body
    return (prelude + setup + "cat > ul_inner.sh <<'UL_EOF'\n" + 'tool=$1\nshift\n' + run + body +
            "UL_EOF\n" + 'exec env %s "$exe" ul_inner.sh "$tool"\n' % wrap)


def ul_script(name, before, after="", wrap=None, observe=False, status="status=$?"):
    def script(argv, stdin_name):
        body = before + "\nrun " + ul_words(argv) + "\n" + status + "\n" + after + "\nexit $status\n"
        return ul_live(name, body, wrap, observe)
    return script


UL_HOLDER_PY = r"""cat > holder.py <<'UL_PY'
import os, select, socket, sys, time
f = open("fdfile", "r+")
d = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
r, w = os.pipe()
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen()
e = select.epoll()
e.register(r, select.EPOLLIN)
time.sleep(4)
UL_PY
"""

UL_MESG_PY = r"""cat > mesg.py <<'UL_PY'
import errno, fcntl, os, pty, subprocess, sys, termios
tool = sys.argv[1]
def one(arguments, initial):
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.fchmod(slave, initial)
    def session():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    child = subprocess.Popen(["mesg"] + arguments, executable=tool, stdin=slave, stdout=slave,
                             stderr=slave, close_fds=True, preexec_fn=session)
    os.close(slave)
    output = bytearray()
    while True:
        try:
            part = os.read(master, 4096)
        except OSError as error:
            if error.errno == errno.EIO:
                break
            raise
        if not part:
            break
        output += part
    status = child.wait()
    mode = os.stat(path).st_mode & 0o777
    os.close(master)
    said = bytes(output).replace(b"\r", b"").decode("utf-8", "replace")
    print("%s %03o %d %03o <%s>" % (" ".join(arguments) or "query", initial, status, mode,
                                   said.replace("\n", "|")))
initial = 0o620 if sys.argv[2] == "on" else 0o600
one(sys.argv[3:], initial)
UL_PY
"""

UL_MOUNT_SETUP = r"""mkdir -p a b c noauto "fstab target"
printf 'tmpfs %s/fstab\\040target tmpfs nodev,nosuid 0 0\ntmpfs %s/noauto tmpfs noauto 0 0\n' "$PWD" "$PWD" > fstab
printf 'tmpfs %s/a tmpfs defaults 0 0\nmalformed\ntmpfs %s/b tmpfs defaults 0 0\n' "$PWD" "$PWD" > fstab-malformed
ln -s c clink
"""
# The storage tools are builtins of our shell: `env` reaches the system ones
# from either shell, so the fixture is built and viewed identically.
UL_MOUNT_PREMOUNT = r"""env mount -t tmpfs tmpfs c && mkdir -p c/sub && env mount -t tmpfs -o ro tmpfs c/sub
env mount --bind c b
"""
UL_MOUNT_VIEW = r"""env findmnt -n -r -o TARGET,SOURCE,FSTYPE,OPTIONS,FSROOT,PROPAGATION | grep -F "$PWD" | sort
for p in a b c c/sub "fstab target" noauto; do env mountpoint -q "$p" && echo "mounted $p"; done
"""

UL_IPC_SETUP = r"""q=$(env ipcmk -Q -p 0640 | env awk '{print $NF}')
m=$(env ipcmk -M 4096 -p 0640 | env awk '{print $NF}')
s=$(env ipcmk -S 2 -p 0640 | env awk '{print $NF}')
"""
UL_IPC_VIEW = "env ipcs -q -m -s\n"

UL_LOCK_SETUP = r""": > lock
env flock -x lock sleep 2 </dev/null >/dev/null 2>&1 &
holder=$!
python3 -c 'import fcntl,time; f=open("lock","r+"); fcntl.lockf(f, fcntl.LOCK_EX, 4, 2); time.sleep(2)' </dev/null >/dev/null 2>&1 &
posix=$!
i=0
until [ "$(env lslocks -n -p $holder -o PID 2>/dev/null)" ] && [ "$(env lslocks -n -p $posix -o PID 2>/dev/null)" ] || [ $i -ge 150 ]; do sleep .02; i=$((i+1)); done
env flock -x lock true </dev/null >/dev/null 2>&1 &
waiter=$!
i=0
until env grep -q -- "->.* $waiter " /proc/locks 2>/dev/null || [ $i -ge 150 ]; do sleep .02; i=$((i+1)); done
"""
UL_LOCK_TEARDOWN = "kill $holder $posix $waiter 2>/dev/null; wait $holder $posix $waiter 2>/dev/null\n"

UL_FD_SETUP = UL_HOLDER_PY + r"""printf abc > fdfile
python3 holder.py </dev/null >/dev/null 2>&1 &
pid=$!
i=0
until [ -e /proc/$pid/fd/8 ] || [ $i -ge 150 ]; do sleep .02; i=$((i+1)); done
"""
UL_FD_TEARDOWN = "kill $pid 2>/dev/null; wait $pid 2>/dev/null\n"

UL_TARGET_SETUP = r"""env unshare -Urnm sleep 3 </dev/null >/dev/null 2>&1 &
a=$!
env unshare -U --map-user=7 --map-group=8 sleep 3 </dev/null >/dev/null 2>&1 &
b=$!
mkdir -p dir
env unshare -Ur sh -c 'cd dir && exec sleep 3' </dev/null >/dev/null 2>&1 &
c=$!
i=0
until [ "$(env readlink /proc/$a/ns/user 2>/dev/null)" != "$(env readlink /proc/$$/ns/user)" ] && [ "$(env awk '{print $1}' /proc/$b/uid_map 2>/dev/null)" = 7 ] || [ $i -ge 150 ]; do sleep .02; i=$((i+1)); done
sleep .05
"""
UL_TARGET_TEARDOWN = "kill $a $b $c 2>/dev/null; wait $a $b $c 2>/dev/null\n"

UL_WAIT_SETUP = r"""env sleep .05 </dev/null &
pid=$!
env sleep .3 </dev/null &
two=$!
true &
gone=$!
wait $gone
ino=$(python3 -c 'import os,sys; print(os.fstat(os.pidfd_open(int(sys.argv[1]))).st_ino)' $two)
pidino="$two:$ino"
"""
UL_WAIT_TEARDOWN = "wait $pid $two 2>/dev/null\n"


# ---------------------------------------------------------------------------
#       Small helpers for the grammars.
# ---------------------------------------------------------------------------

def ul_flags(*spells):
    return tuple(Option(spell) for spell in spells)


def ul_has(argv, *spells):
    for word in argv:
        for spell in spells:
            if word == spell or (spell.startswith("--") and word.startswith(spell + "=")) or \
                    (not spell.startswith("--") and len(spell) == 2 and word.startswith(spell)
                     and not word.startswith("--")):
                return True
    return False


def ul_needs_pid(argv):
    """Listing every process or lock on a busy box is not an oracle."""
    return ul_has(argv, "-p", "--pid", "--task", "-H", "--list-columns", "--dump-counters")


def ul_waitpid_valid(argv):
    live = any(word in ("1", "1:1", "+1", " 1") for word in argv)
    return not live or ul_has(argv, "-t", "--timeout")


def ul_lsns_live_valid(argv):
    return ul_has(argv, "-o", "--output") and not any("COMMAND" in word for word in argv)


UL_COLUMNS_OPTION = ("-o", ("NAME",), False)


# ---------------------------------------------------------------------------
#       The grammars.
# ---------------------------------------------------------------------------

UL_BITS_OPERANDS = (
    ("4,5-8", "16,30"), ("0xeec2",), ("2,22,74,79",), (",00300000,03000000,30000003",),
    ("0xff", "~1-3", "^8-10", "&0-9"), ("0-30:3", "40,42,44"), ("0x0",), (), ("0-129",),
    ("1-63",), ("63-65",), ("64-127",), ("65-129",), ("0-129:2",), ("1-129:3",),
    ("0x1ffffffffffffffff",), (",7,ffffffffffffffff",), ("0x00000000ffffffff",),
    ("0,62,64,127,128", "1-61", "65-126", "~32-96", "^16-111:3"), ("0,65535,131071,131072",),
    ("18446744073709551616",), ("none",), ("abc",), ("3-1",), ("1-3:0",), ("1,,2",), ("0x_1",),
    (",1,,2",), ("0x,1",), ("0x1,",), ("0x1,,2",), ("0xg0000000000000001",), (",,1",), (",1,",),
    ("1024",), ("0-10:2",), ("|3-5", "^1"), ("31",), ("32",), ("63",), ("64",), ("128",),
    ("0x0", "&0-9"), ("1-3", "~1-3"), ("0x10", "|0x01"),
)

UL_GETOPT_OPERANDS = (
    ("--", "x", "-a", "-b", "y", "z", "-cfoo"), ("--", "x", "-a", "-b", "y"), ("--", "x", "-a", "z", "-b", "y"),
    ("--", "--alpha", "--beta", "value", "x", "--charlie=maybe"), ("--", "--foo", "--baz", "value", "x"),
    ("--", "-alpha", "-a"), ("--", "-a", "a'b", "z"), ("--", "-a", "x y", "z"), ("--", "-x"), ("--", "--fo"),
    ("--",), (), ("--", "-a", "", "z"), ("--", "-a", "new\nline", "--", "-b"), ("--", "-a", "caf\u00e9", "\u65e5\u672c"),
    ("--", "-a", "\\", "$", "`", "!", "*", "~"), ("--", "--beta"), ("--", "-b"), ("--", "-c"), ("--", "--charlie", "x"),
    ("--", "--al"), ("--", "-ab", "v"), ("--", "-a", "-", "x"), ("--", "--", "-a"), ("-a", "x"),
    ("a:", "-a", "x y", "z"), ("a:",), ("--", "-a", "'", "\"", "a b'c\"d"), ("--", "--beta=v", "--alpha=bad"),
    ("--", "-abc"), ("--", "-a", "-b"), ("--", "--charlie=", "--beta="), ("--", "x", "y", "-a"),
    ("--", "-a", "\t", " "), ("--", "-", "--"), ("ab:c::", "-a", "-b", "y", "-cfoo", "z"),
)


def ul_prlimit_options():
    resources = (("-c", "--core"), ("-d", "--data"), ("-e", "--nice"), ("-f", "--fsize"),
                 ("-i", "--sigpending"), ("-l", "--memlock"), ("-m", "--rss"), ("-n", "--nofile"),
                 ("-q", "--msgqueue"), ("-r", "--rtprio"), ("-s", "--stack"), ("-t", "--cpu"),
                 ("-u", "--nproc"), ("-v", "--as"), ("-x", "--locks"), ("-y", "--rttime"))
    options = [Option("-p", ("1", "bad", "999999999"), False),
               Option("-o", ("SOFT", "UNITS,SOFT", "HARD,SOFT,RESOURCE", "RESOURCE,RESOURCE",
                             "DESCRIPTION,UNITS", "RESOURCE,SOFT,HARD", "BAD", ""), False),
               Option("--noheadings"), Option("--raw"), Option("--verbose")]
    for short, long in resources:
        options.append(Option(short))
        values = ("100:200", "-1", "bad")
        if long == "--nofile":
            values = ("100:200", "100", "-1", ":200", "", "bad", ":", "unlimited:unlimited", "200:100")
        options.append(Option(long, values, True))
    return tuple(options)


UTILITIES = (
    # -- bit masks, ISO sizes, block ioctls --------------------------------
    Utility("bits", options=(Option("-w", ("1", "2", "31", "32", "33", "63", "64", "65", "127", "128",
                                          "129", "8192", "131072", "0", "131073",
                                          "18446744073709551616", "bad"), None),
                             *ul_flags("-m", "-g", "-b", "-l", "--mask", "--grouped-mask", "--binary", "--list")),
            operands=UL_BITS_OPERANDS, stdin=("ul_bits", "empty", "ul_masks", "ul_bad_bits", "text", "newline"),
            fixture="none", stderr="loose", max_flags=5),
    Utility("isosize", options=(Option("-d", ("1", "1024", "2048", "0", "-1", "abc", "18446744073709551616"), None),
                                Option("-x")),
            operands=(("iso.img",), ("iso.img", "iso.img"), ("short.img",), ("missing",), ("data",), (),
                      ("ext4.img",), ("dir",)),
            stdin=("empty",), fixture="ul_images"),
    Utility("blockdev", options=(*ul_flags("-q", "-v", "--report", "--getsz", "--setro", "--setrw", "--getro",
                                           "--getdiscardzeroes", "--getss", "--getpbsz", "--getiomin",
                                           "--getioopt", "--getalignoff", "--getmaxsect", "--getbsz",
                                           "--getsize", "--getsize64", "--getra", "--getfra", "--getdiskseq",
                                           "--getzonesz", "--flushbufs", "--rereadpt"),
                                 Option("--setbsz", ("4096", "bad", "0"), False),
                                 Option("--setra", ("256", "18446744073709551616", "bad"), False),
                                 Option("--setfra", ("256", "bad"), False)),
            operands=(("/dev/null",), ("missing",), ("data",), (), ("/dev/null", "/dev/null"), ("dir",)),
            stdin=("empty",), fixture="ul", stderr="exact", max_flags=3),
    Utility("addpart", operands=(("/dev/null", "1", "2", "3"), ("/dev/null", "1"), ("/dev/null", "2147483648", "1", "1"),
                                 ("/dev/null", "1", "18014398509481984", "1"), ("/dev/null", "1", "1", "18014398509481984"),
                                 ("missing", "1", "2", "3"), (), ("/dev/null",), ("/dev/null", "a", "b", "c"),
                                 ("/dev/null", "1", "2", "3", "4"), ("data", "1", "2", "3"), ("/dev/null", "-1", "2", "3")),
            stdin=("empty",), fixture="ul", stderr="exact"),
    Utility("delpart", operands=(("/dev/null", "1"), ("/dev/null",), ("/dev/null", "1", "2"), ("missing", "1"), (),
                                 ("data", "1"), ("/dev/null", "bad"), ("/dev/null", "2147483648")),
            stdin=("empty",), fixture="ul", stderr="exact"),
    Utility("resizepart", operands=(("/dev/null", "1", "8192"), ("/dev/null", "1"), ("/dev/null",), (),
                                    ("missing", "1", "8192"), ("data", "1", "8192"), ("/dev/null", "bad", "8192"),
                                    ("/dev/null", "1", "bad"), ("/dev/null", "1", "18014398509481984")),
            stdin=("empty",), fixture="ul", stderr="exact"),

    # -- storage signatures ------------------------------------------------
    Utility("wipefs", options=(*ul_flags("-a", "-b", "-f", "-i", "-J", "-n", "-p", "-q", "--all", "--noheadings",
                                         "--json", "--no-act", "--parsable", "--quiet", "--lock"),
                               Option("--backup", ("dir", "missing"), True),
                               Option("-o", ("0x438", "0", "0xff6", "1080", "bad", "-1"), None),
                               Option("-O", ("DEVICE,OFFSET,TYPE,UUID,LABEL,LENGTH,USAGE", "TYPE", "UUID,LABEL",
                                             "+USAGE", "BAD", ""), False),
                               Option("-t", ("ext4", "noext4", "swap", "noswap", "ext4,swap", "bad", ""), False),
                               Option("--lock", ("yes", "no", "nonblock", "bad"), True)),
            operands=(("ext4.img",), ("swap.img",), ("multi.img",), ("nosig.img",), ("ext4.img", "swap.img"),
                      ("short.img",), ("missing",), (), ("iso.img",), ("btrfs-magic.img",), ("dir",)),
            stdin=("empty",), fixture="ul_images", max_flags=4),
    Utility("mkswap", options=(*ul_flags("-c", "-f", "-q", "-F", "--check", "--force", "--quiet", "--file", "--verbose",
                                         "--lock"),
                               Option("-p", ("4096", "8192", "65536", "1024", "bad", "0"), None),
                               Option("-L", ("bowl", "0123456789abcdefXYZ", "", "with space"), False),
                               Option("-v", ("1", "0", "2"), False),
                               Option("-U", (UL_UUID2, "clear", "invalid"), False),
                               Option("-e", ("native", "little", "big", "bad"), False),
                               Option("-o", ("0", "4096", "bad"), False),
                               Option("-s", ("1M", "65536", "bad"), False),
                               Option("--lock", ("yes", "no", "nonblock"), True)),
            operands=(("-U", UL_UUID, "swap.img"), ("-U", UL_UUID, "swap.img", "64"), ("-U", UL_UUID, "swap.img", "1024"),
                      ("-U", UL_UUID, "nosig.img"), ("-U", UL_UUID, "ext4.img"), ("-U", UL_UUID, "short.img"),
                      ("-U", UL_UUID, "missing"), ("-U", UL_UUID, "data"), ("-U", UL_UUID), ("-U", UL_UUID, "/dev/null"),
                      ("-U", UL_UUID, "swap.img", "999999999"), ("-U", UL_UUID, "swap.img", "bad"), ("-U", UL_UUID, "dir")),
            stdin=("empty",), fixture="ul_images", max_flags=4),
    Utility("swaplabel", options=(Option("-L", ("new", "0123456789abcdefXYZ", "", "bowl"), None),
                                  Option("-U", (UL_UUID2, "clear", "invalid", UL_UUID), False),
                                  Option("--label", ("new",), True), Option("--uuid", (UL_UUID2,), True)),
            operands=(("swap.img",), ("ext4.img",), ("nosig.img",), ("short.img",), ("missing",), (),
                      ("swap.img", "swap.img"), ("/dev/null",), ("dir",)),
            stdin=("empty",), fixture="ul_images"),
    Utility("blkid", options=(Option("-s", ("TYPE", "UUID", "LABEL", "NOT_A_TAG", "type", "PTTYPE", "BLOCK_SIZE"), None, repeat=True),
                              Option("--match-tag", ("UUID", "TYPE"), True),
                              Option("-o", ("full", "value", "device", "export", "list", "udev", "bad", ""), None),
                              Option("--output", ("value", "export"), True),
                              Option("-t", ("TYPE=ext4", "TYPE=swap", "LABEL=moondata", "UUID=" + UL_EXT4_UUID,
                                            "TYPE=nope", "BAD", "LABEL=", "=x"), False),
                              Option("-U", (UL_EXT4_UUID, UL_MISSING_UUID, "bad"), False),
                              Option("-L", ("moondata", "moonswap", "nope"), False),
                              *ul_flags("-l", "-p", "-i", "-k", "-d", "-g", "-D", "--probe", "--list-one"),
                              Option("-c", ("/dev/null", "missing"), False),
                              Option("-n", ("ext4", "vfat,ext3", "swap"), False),
                              Option("-u", ("filesystem", "raid", "other"), False),
                              Option("-S", ("1024", "bad"), False), Option("-O", ("0", "1024"), False),
                              Option("-H", ("session_offset=0",), False)),
            operands=(("ext4.img",), ("swap.img",), ("ext4.img", "swap.img"), ("iso.img",), ("multi.img",), ("short.img",),
                      ("hostile.img",), ("ntfs-magic.img",), ("btrfs-magic.img",), ("xfs-magic.img",), ("f2fs-magic.img",),
                      ("squashfs-magic.img",), ("udf-magic.img",), ("luks-magic.img",), ("nosig.img",), ("missing",), (),
                      ("dir",), ("data",)),
            stdin=("empty",), fixture="ul_images", max_flags=4,
            extra=(("ext4.img", "-s", "UUID", "-o", "value"), ("--match-tag=UUID", "--output=value", "ext4.img"),
                   ("-s", "TYPE", "-s", "UUID", "ext4.img"), ("-s", "NOT_A_TAG", "ext4.img"),
                   ("-s", "TYPE", "-o", "value", "swap.img"), ("-s", "LABEL", "-o", "value", "ext4.img"))),
    Utility("findfs", operands=(("UUID=" + UL_EXT4_UUID,), ("LABEL=moondata",), ("UUID=" + UL_MISSING_UUID,),
                                ("PARTUUID=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",), ("PARTLABEL=moon data",),
                                ("/dev/not-present",), ("",), ("BAD",), (), ("a", "b"), ("UUID=",), ("uuid=" + UL_EXT4_UUID,),
                                ("NAME=value",), ("LABEL=x=y",), ("/dev/null",), ("data",)),
            stdin=("empty",), fixture="ul_images", stderr="exact"),

    # -- the mount table, read only ----------------------------------------
    Utility("findmnt", options=(*ul_flags("-n", "-r", "-l", "-v", "-P", "-f", "-i", "-J", "-a", "-b", "-c", "-C", "-D",
                                          "-e", "-I", "-k", "-m", "-s", "-A", "-R", "-u", "-U", "-x", "-y", "--tree",
                                          "--real", "--pseudo", "--verbose", "--vfs-all", "--output-all", "--shadowed",
                                          "--noheadings", "--raw", "--pairs", "--first-only", "--invert", "--nofsroot"),
                                Option("-S", ("/dev/nope", "proc", "tmpfs", "LABEL=x", "sysfs"), False),
                                Option("-T", ("/", "/proc", "", "/nonexistent", "dir", "/sys/kernel"), None),
                                Option("--target", ("/", ""), True),
                                Option("-M", ("/", "/proc", "", "dir"), False),
                                Option("-t", ("proc", "tmpfs", "proc,sysfs", "noproc", "", "notmpfs,noproc"), False),
                                Option("--types", ("proc", ""), True),
                                Option("-O", ("rw", "norw", "ro", "nosuid", "rw,nosuid", "nodev", "noexec,norw"), False),
                                Option("-o", ("TARGET", "TARGET,FSTYPE", "SOURCE,FSROOT,MAJ:MIN,ID,PARENT",
                                              "OPTIONS,VFS-OPTIONS,FS-OPTIONS", "FSROOT,MAJ:MIN,ID,PARENT,VFS-OPTIONS,FS-OPTIONS",
                                              "+FSTYPE", "LABEL", "UUID", "PROPAGATION", "SIZE", "BAD", "", "TARGET,TARGET"), False),
                                Option("--output", ("TARGET,FSTYPE",), True),
                                Option("-F", ("fstab", "/etc/fstab", "missing"), False),
                                Option("-d", ("forward", "backward", "bad"), False),
                                Option("-N", ("1", "bad"), False), Option("-w", ("1",), False),
                                Option("-Q", ("FSTYPE=='proc'",), False), Option("--id", ("1", "bad"), True),
                                Option("--kernel", ("mountinfo", "listmount", "bad"), True)),
            operands=((), ("/",), ("/proc",), ("proc",), ("",), ("/nonexistent",), ("/", "/proc"), ("dir",), ("sysfs",)),
            stdin=("empty",), fixture="ul", max_flags=4),
    Utility("mountpoint", options=ul_flags("-q", "-d", "-x", "--nofollow", "--show", "--quiet", "--fs-devno", "--devno"),
            operands=(("/",), ("/proc",), ("dir",), ("missing",), ("",), ("/", "dir"), ("rootlink",), ("dirlink",),
                      ("dangling",), ("/dev/null",), ("a.txt",), (), ("/proc/",), ("/./proc",), ("dir/",)),
            stdin=("empty",), fixture="ul"),
    Utility("mount", options=(*ul_flags("-a", "-c", "-f", "-F", "-i", "-l", "-n", "-r", "-v", "-w", "-B", "-M", "-R",
                                        "--make-shared", "--make-slave", "--make-private", "--make-unbindable",
                                        "--make-rshared", "--make-rslave", "--make-rprivate", "--make-runbindable",
                                        "--fake", "--onlyonce", "--beneath", "--exclusive", "--options-source-force",
                                        "--mkdir", "--all", "--verbose", "--read-only", "--rw", "--ro", "--bind", "--move",
                                        "--rbind", "--no-mtab", "--no-canonicalize", "--internal-only", "--show-labels"),
                              Option("-T", ("fstab", "/dev/null", "missing"), False),
                              Option("-o", ("ro", "rw", "nodev,nosuid", "remount", "bind", "size=1m", "", "defaults"), False),
                              Option("-O", ("rw", "nodev"), False),
                              Option("-t", ("proc", "tmpfs", "", "proc,sysfs", "noproc", "auto"), False),
                              Option("--types", ("proc", ""), True),
                              Option("-L", ("moondata",), False), Option("-U", (UL_EXT4_UUID,), False),
                              Option("--source", ("tmpfs", "LABEL=x", ""), True),
                              Option("--target", ("dir", ""), True), Option("--target-prefix", ("dir",), True),
                              Option("-N", ("1", "bad"), False), Option("--options-mode", ("ignore", "append", "bad"), True),
                              Option("--options-source", ("fstab", "mtab", "disable"), True),
                              Option("--map-users", ("0:1000:1", "bad"), True), Option("--map-groups", ("0:1000:1",), True)),
            operands=((), ("tmpfs", "dir"), ("dir",), ("missing",), ("a", "b", "c"), ("/proc",), ("/dev/nope", "dir"),
                      ("LABEL=moondata", "dir"), ("UUID=" + UL_EXT4_UUID, "dir"), ("--", "dir")),
            stdin=("empty",), fixture="ul", max_flags=4,
            extra=(("--types=",), ("-t", "proc"), ("-vn",), ("-l",), ("-t", "tmpfs", "-o", "size=1m", "tmpfs", "dir"))),
    Utility("umount", options=(*ul_flags("-a", "-A", "-c", "-d", "--fake", "-f", "-i", "-n", "-l", "-R", "-r", "-v", "-q",
                                         "--all", "--all-targets", "--no-canonicalize", "--detach-loop", "--force",
                                         "--internal-only", "--no-mtab", "--lazy", "--recursive", "--read-only",
                                         "--verbose", "--quiet"),
                               Option("-O", ("rw", "nodev"), False),
                               Option("-t", ("proc", "tmpfs", "noproc", ""), False), Option("--types", ("proc",), True),
                               Option("-N", ("1", "bad"), False)),
            operands=(("dir",), ("missing",), ("/proc",), (), ("dir", "missing"), ("/",), ("dirlink",), ("data",), ("",)),
            stdin=("empty",), fixture="ul", max_flags=4),

    # -- scheduling and resource policy -----------------------------------
    Utility("taskset", options=ul_flags("-a", "-p", "-c", "--all-tasks", "--pid", "--cpu-list"),
            operands=(("1",), ("1", UL_OBS, "affinity"), ("0x3", UL_OBS, "affinity"), ("0,2,4-6", UL_OBS, "affinity"),
                      ("impossible", UL_OBS, "affinity"), ("0",), (), ("1", "1"), ("0xffffffff", UL_OBS, "affinity"),
                      ("0x100000000", UL_OBS, "affinity"), ("", UL_OBS, "affinity"), ("0-3:2", UL_OBS, "affinity"),
                      ("3", "1"), ("0x1", "999999999"), ("1", "missing"), ("--", "1", UL_OBS, "affinity"),
                      ("0,,1", UL_OBS, "affinity"), ("0-", UL_OBS, "affinity"), ("all", UL_OBS, "affinity")),
            stdin=("empty",), fixture="ul"),
    Utility("chrt", options=(*ul_flags("-b", "-d", "-e", "-f", "-i", "-o", "-r", "-R", "-a", "-m", "-p", "-v", "--batch",
                                       "--deadline", "--ext", "--fifo", "--idle", "--other", "--rr", "--reset-on-fork",
                                       "--all-tasks", "--max", "--pid", "--verbose"),
                             Option("-T", ("1000000", "0", "bad"), None), Option("-P", ("1000000", "0"), False),
                             Option("-D", ("1000000", "0"), False)),
            operands=(("1",), ("0", "1"), ("0", UL_OBS, "sched"), ("1", UL_OBS, "sched"), ("impossible",), (),
                      (UL_OBS, "sched"), ("99", UL_OBS, "sched"), ("-1", UL_OBS, "sched"), ("0", "999999999"),
                      ("0", "missing"), ("100", UL_OBS, "sched"), ("0", "1", "1")),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("renice", options=(Option("-n", ("0", "5", "-5", "impossible", "18446744073709551616", "20", "-21"), None),
                               Option("--priority", ("0", "5"), True),
                               Option("--relative", ("0", "1", "18446744073709551615", "-1"), True),
                               *ul_flags("-p", "-g", "-u", "--pid", "--pgrp", "--user")),
            operands=(("1",), ("0", "1"), ("5", "1"), ("impossible", "1"), ("0",), (), ("0", "-p"), ("0", "-u", "root"),
                      ("0", "-u", "nobody"), ("0", "-u", "impossible-user"), ("0", "-g", "1"), ("0", "-p", "1", "1"),
                      ("0", "-p", "999999999"), ("18446744073709551616", "-p", "1"), ("-p", "1", "-n", "0"),
                      ("0", "-p", "bad"), ("+1", "1")),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("prlimit", options=ul_prlimit_options(),
            operands=((), (UL_OBS, "limits"), (UL_OBS, "limits", "x"), ("missing",), (UL_OBS, "exit7")),
            stdin=("empty",), fixture="ul", max_flags=4),
    Utility("uclampset", options=(Option("-m", ("0", "512", "1024", "1025", "-1", "bad", ""), None),
                                  Option("-M", ("0", "512", "1024", "1025", "bad"), False),
                                  Option("-p", ("1", "bad", "999999999"), False),
                                  *ul_flags("-a", "-s", "-R", "-v", "--all-tasks", "--system", "--reset-on-fork", "--verbose")),
            operands=((), (UL_OBS, "clamp"), (UL_OBS, "exit7"), ("missing",), ("1",)),
            stdin=("empty",), fixture="ul", max_flags=4),
    Utility("ionice", options=(Option("-c", ("0", "1", "2", "3", "idle", "best-effort", "realtime", "none", "IDLE",
                                            "impossible", "4294967298", ""), None),
                               Option("-n", ("0", "4", "7", "8", "4294967298", "bad"), False),
                               Option("-p", ("1", "+1", " 1", "2147483647", "4294967296", "999999999999999999999999999999",
                                             "bad"), False),
                               Option("-P", ("1", "bad"), False), Option("-u", ("0", "bad"), False),
                               *ul_flags("-t", "--ignore")),
            operands=((), (UL_OBS, "io"), ("1", "1"), ("missing",), (UL_OBS, "exit7"), ("1",)),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("choom", options=(Option("-n", ("0", "1", "-1", "1000", "-1001", "1001", "impossible", ""), None),
                              Option("-p", ("1", "bad", "999999999"), False),
                              Option("--adjust", ("0",), True), Option("--pid", ("1",), True)),
            operands=((), (UL_OBS, "oom"), ("missing",), (UL_OBS, "exit7"), ("1",)),
            stdin=("empty",), fixture="ul"),
    Utility("setsid", options=ul_flags("-c", "-f", "-w", "--ctty", "--fork", "--wait"),
            operands=((), (UL_OBS, "session"), (UL_OBS, "exit7"), (UL_OBS, "signal"), ("missing",),
                      ("--", UL_OBS, "session"), (UL_OBS, "echo", "boundary")),
            stdin=("empty",), fixture="ul"),
    Utility("setpgid", options=ul_flags("-f", "--foreground"),
            operands=((), (UL_OBS, "session"), (UL_OBS, "exit7"), (UL_OBS, "signal"), ("missing",),
                      ("--", UL_OBS, "session"), (UL_OBS, "echo", "boundary")),
            stdin=("empty",), fixture="ul"),
    Utility("waitpid", options=(*ul_flags("-v", "-e", "--verbose", "--exited"),
                                Option("-t", (".01", "0.", "1", "impossible", "0", "-1"), None),
                                Option("--timeout", (".01",), True),
                                Option("-c", ("1", "2", "0", "bad"), None), Option("--count", ("1",), True)),
            operands=(("2147483647",), ("1",), ("0",), ("+2147483647",), (" 2147483647",), ("1:1",), (),
                      ("1", "2147483647"), ("bad",), ("2147483647:1",), ("-5",)),
            stdin=("empty",), fixture="none", valid=ul_waitpid_valid),

    # -- descriptors, files, ranges ----------------------------------------
    Utility("flock", options=(*ul_flags("-s", "-x", "-u", "-n", "-o", "-F", "--shared", "--exclusive", "--unlock", "--nb",
                                        "--nonblocking", "--close", "--no-fork", "--fcntl", "--verbose"),
                              Option("-w", (".01", "1e-3", "+0.01", " 0.01", "0", "bad", "-1"), None),
                              Option("--wait", (".01",), True), Option("--timeout", (".01",), True),
                              Option("-E", ("42", "0", "300", "bad", "-1"), False),
                              Option("-c", ("printf command", "exit 7", "printf wrong"), False),
                              Option("--start", ("0", "1", "bad"), True), Option("--length", ("1", "0", "bad"), True)),
            operands=(("lock", UL_OBS, "exit7"), ("lock", UL_OBS, "echo"), ("lock", UL_OBS, "echo", "extra"), ("lock",),
                      ("rolock", UL_OBS, "echo"), ("abc",), ("--", "-1"), ("4294967296",), ("9",), ("0",),
                      ("dir", UL_OBS, "echo"), ("missing", UL_OBS, "echo"), ("lock", "missing"), (),
                      ("lock", UL_OBS, "flock_nb"), ("lock", "-c", "printf inner")),
            stdin=("text",), fixture="ul", normalize=ul_norm_flock, max_flags=4),
    Utility("fadvise", options=(Option("-a", ("normal", "sequential", "random", "noreuse", "willneeded", "dontneed",
                                              "impossible", "", "NORMAL"), False),
                                Option("-d", ("0", "9", "+9", " 9", "4294967299", "bad", "-1"), None),
                                Option("--fd", ("0",), True),
                                Option("-l", ("2KiB", "1", "0", "bad", "-1"), False),
                                Option("-o", ("1K", "1KB", "1kiB", "1kib", "1p", "0x10", "1.5K", "1.9K", "0.5MB", "0.5MiB",
                                              "1Ki", "1KIB", "1B", "1Q", "-1", "0", "2KiB"), False)),
            operands=(("data",), ("missing",), ("data", "data"), (), ("dir",), ("zeros",)),
            stdin=("text",), fixture="ul", max_flags=3),
    Utility("fallocate", options=(*ul_flags("-c", "-d", "-i", "-n", "-p", "-r", "-v", "-w", "-x", "-z", "--collapse-range",
                                            "--dig-holes", "--insert-range", "--keep-size", "--punch-hole",
                                            "--report-holes", "--verbose", "--write-zeroes", "--posix", "--zero-range"),
                                  Option("-l", ("8KiB", "0", "1", "4096", "3", "bad", "-1", "1M"), None),
                                  Option("--length", ("8KiB",), True),
                                  Option("-o", ("4KiB", "0", "2", "4096", "bad"), False), Option("--offset", ("4096",), True)),
            operands=(("data",), ("newfile",), ("zeros",), ("missing/x",), ("data", "data"), (), ("dir",), ("in",)),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("copyfilerange", options=(Option("-r", ("ranges", "missing", "data"), None), Option("--ranges", ("ranges",), True),
                                      *ul_flags("-v", "--verbose")),
            operands=(("in", "out", "2:1:3"), ("in", "out", "0:0:2", "::2"), ("in", "out", "3:0:0"), ("in", "out", "invalid"),
                      ("in", "out", "4:0:1"), ("in", "out"), ("in",), (), ("in", "out", "0:0:8"), ("missing", "out", "0:0:1"),
                      ("in", "out", "1:1:1", "2:2:2"), ("in", "dir", "0:0:1"), ("in", "newfile", "0:0:4"),
                      ("in", "out", "0:0:0"), ("in", "out", "::"), ("in", "out", "8:0:1"), ("in", "in", "0:4:4"),
                      ("in", "out", "-1:0:1"), ("in", "out", "0:0:1", "extra")),
            stdin=("empty",), fixture="ul"),
    Utility("exch", operands=(("a", "b"), ("only-one",), ("a", "b", "c"), ("missing", "missing2"), ("a", "missing"),
                              ("a", "dir"), ("dir", "dir2"), (), ("a", "a"), ("link", "b"), ("dangling", "a"), ("", "a")),
            stdin=("empty",), fixture="ul"),
    Utility("getino", options=ul_flags("-p", "--print-pid", "--pidfs", "--userns", "--mntns", "--netns", "--pidns",
                                       "--utsns", "--ipcns", "--cgroupns", "--timens"),
            operands=(("1",), ("1", "1"), ("1:1",), ("invalid-pid",), (), ("0",), ("999999999",), ("1:bad",), ("-1",),
                      ("1:",)),
            stdin=("empty",), fixture="none", max_flags=3),

    # -- getopt: a procedural grammar of its own ---------------------------
    Utility("getopt", options=(Option("-o", ("ab:c::", "+ab:", "-ab:", "a", ":ab:", "", "a:b::c", "+:ab", "abc", "a:",
                                            "ab:c::d"), None),
                               Option("--options", ("ab:c::", "a:"), True),
                               Option("-l", ("alpha,beta:,charlie::", "foo,bar", "a,alpha", "", "long:", "foo,foobar",
                                             "name:::", "alpha beta:", "al", "beta:,beta"), False, repeat=True),
                               Option("--longoptions", ("alpha,beta:", "baz:"), True),
                               Option("-n", ("parser", ""), False), Option("--name", ("parser",), True),
                               Option("-s", ("sh", "bash", "csh", "tcsh", "zsh", ""), False),
                               Option("--shell", ("csh",), True),
                               *ul_flags("-a", "-q", "-Q", "-T", "-u", "-U", "--alternative", "--quiet", "--quiet-output",
                                         "--test", "--unquoted", "--unknown")),
            operands=UL_GETOPT_OPERANDS, stdin=("empty",), fixture="none", stderr="exact", max_flags=4,
            extra=(("a:", "-a", "x y", "z"), ("-o", "a", "-l", "foo,bar", "-l", "baz:", "--", "--foo", "--baz", "value", "x"),
                   ("-n", "parser", "-o", "a", "-l", "foo,foobar", "--", "--fo"), ("-T",), ("-a", "-o", "a", "-l", "a,alpha", "--", "-alpha", "-a"),
                   ("-q", "-o", "a", "--", "-x"), ("-Q", "-o", "a:", "--", "-a", "value", "x"),
                   ("-s", "sh", "-o", "a:", "--", "-a", "a'b", "z"), ("-s", "csh", "-o", "a:", "--", "-a", "x y", "z"),
                   ("-u", "-o", "a:", "--", "-a", "x y", "z"), ("-o", "+ab:", "--", "x", "-a", "-b", "y"),
                   ("-o", "-ab:", "--", "x", "-a", "z", "-b", "y"), ("-o", "ab:c::", "--", "x", "-a", "-b", "y", "z", "-cfoo"))),

    # -- personality, privilege, namespaces --------------------------------
    Utility("setarch", options=(*ul_flags("-v", "-B", "-F", "-I", "-L", "-R", "-S", "-T", "-X", "-Z", "-3", "--4gb", "--uname-2.6",
                                          "--list", "--show", "--verbose", "--32bit", "--fdpic-funcptrs", "--short-inode",
                                          "--addr-compat-layout", "--addr-no-randomize", "--whole-seconds",
                                          "--sticky-timeouts", "--read-implies-exec", "--mmap-page-zero", "--3gb"),
                                Option("--show", ("0x40000", "-1", "1", "0", "bad", "0x0040000"), True),
                                Option("--sho", ("0",), True), Option("--list", ("garbage",), True),
                                Option("-p", ("1", "bad", "999999999"), False), Option("--pid", ("1",), True)),
            operands=(("x86_64", UL_OBS, "personality"), ("linux32", UL_OBS, "arch"), ("linux64", UL_OBS, "arch"),
                      ("i386", UL_OBS, "arch"), ("impossible", UL_OBS, "arch"), ("x86_64",), (), (UL_OBS, "arch"),
                      ("x86_64", "--", UL_OBS, "echo", "boundary"), ("x86_64", UL_OBS, "exit7"), ("uname26", UL_OBS, "arch"),
                      ("x86_64", "missing"), ("i686", UL_OBS, "personality")),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("setpriv", options=(*ul_flags("-d", "--dump", "--nnp", "--no-new-privs", "--list-caps", "--clear-groups",
                                          "--keep-groups", "--init-groups", "--reset-env"),
                                Option("--reuid", ("1000", "0", "nobody", "bad", "root"), True),
                                Option("--ruid", ("1000", "0"), True), Option("--euid", ("1000", "0"), True),
                                Option("--regid", ("1000", "0", "bad"), True), Option("--rgid", ("1000",), True),
                                Option("--egid", ("1000",), True),
                                Option("--groups", ("1000", "impossible", "1000,1000", "0", ""), True),
                                Option("--pdeathsig", ("TERM", "term", "0", "keep", "clear", "SIGKILL", "bad", "64"), True),
                                Option("--ptracer", ("none", "any", "0", "1", "bad"), True),
                                Option("--inh-caps", ("-all", "+all", "bad"), True),
                                Option("--ambient-caps", ("-all", "+all"), True),
                                Option("--bounding-set", ("-all",), True), Option("--securebits", ("-all", "+noroot"), True),
                                Option("--selinux-label", ("test",), True), Option("--apparmor-profile", ("test",), True),
                                Option("--landlock-access", ("fs",), True),
                                Option("--landlock-rule", ("path-beneath:read-file:/",), True),
                                Option("--seccomp-filter", ("/no/such/filter",), True)),
            operands=((), (UL_OBS, "ids"), (UL_OBS, "dump"), (UL_OBS, "env"), ("missing",), (UL_OBS, "exit7"), ("-dd",),
                      ("-ddd",)),
            stdin=("empty",), fixture="ul", max_flags=3),
    Utility("unshare", options=(*ul_flags("-m", "-u", "-i", "-n", "-p", "-U", "-C", "-T", "-f", "-r", "-c", "--mount", "--uts",
                                          "--ipc", "--net", "--pid", "--user", "--cgroup", "--time", "--fork",
                                          "--map-root-user", "--map-current-user", "--map-auto", "--map-subids",
                                          "--kill-child", "--forward-signals", "--keep-caps", "--mount-proc", "--mount-binfmt"),
                                Option("--map-user", ("7", "2147483648", "bad", "0", "nobody"), True),
                                Option("--map-group", ("8", "bad"), True),
                                Option("--map-users", ("0:100000:10", "bad", "auto"), True),
                                Option("--map-groups", ("0:100000:10",), True), Option("--owner", ("0:0", "bad"), True),
                                Option("--setgroups", ("deny", "allow", "bad"), True),
                                Option("--propagation", ("unchanged", "private", "slave", "shared", "impossible"), True),
                                Option("--mount-proc", ("dir", "/proc"), True), Option("--kill-child", ("TERM", "bad"), True),
                                Option("-R", ("dir", "/nonexistent"), False), Option("-w", ("dir", "/nonexistent", ""), False),
                                Option("-S", ("0", "1000", "bad"), False), Option("-G", ("0", "1000"), False),
                                Option("-l", ("data",), False),
                                Option("--monotonic", ("7", "-1", "bad", "0"), True), Option("--boottime", ("-3", "2"), True)),
            operands=((), (UL_OBS, "ids"), (UL_OBS, "ns"), (UL_OBS, "exit7"), (UL_OBS, "signal"), (UL_OBS, "pwd"),
                      (UL_OBS, "time"), ("missing",), (UL_OBS, "echo", "x")),
            stdin=("empty", "text"), fixture="ul", max_flags=4, env=(("SHELL", "/bin/false"),),
            extra=(("-Ur", UL_OBS, "ids"), ("-Uc", UL_OBS, "ids"), ("-U", "--map-user=7", "--map-group=8", UL_OBS, "ids"),
                   ("-Urc", UL_OBS, "ids"), ("-Ucr", UL_OBS, "ids"), ("-Ur", "--map-user=7", UL_OBS, "ids"),
                   ("-U", "--map-user=7", "-r", UL_OBS, "ids"), ("-U", "--map-user=2147483648", UL_OBS, "ids"),
                   ("-U", "--setgroups=deny", UL_OBS, "ids"), ("-Urnm", "--propagation", "unchanged", UL_OBS, "ns"),
                   ("-Urpf", UL_OBS, "ns"), ("-Urf", UL_OBS, "exit7"), ("-Urf", UL_OBS, "signal"),
                   ("-UrTf", "--monotonic", "7", "--boottime", "-3", UL_OBS, "time"), ("-Ur", "--wd", "dir", UL_OBS, "pwd"),
                   ("--user", "--map-root-user", "--net", "--propagation", "unchanged", UL_OBS, "ns"),
                   ("-Um", "--propagation", "impossible", UL_OBS, "echo"), ("--monotonic", "1", UL_OBS, "echo"),
                   ("--setgroups", "deny", UL_OBS, "echo"), ("-Ur",), ("-Urm", "--mount-proc", UL_OBS, "ns"))),
    Utility("nsenter", options=(*ul_flags("-a", "-m", "-u", "-i", "-n", "-p", "-C", "-U", "-T", "-e", "-F", "-c", "-r", "-w",
                                          "--all", "--mount", "--uts", "--ipc", "--net", "--pid", "--cgroup", "--user",
                                          "--time", "--preserve-credentials", "--keep-caps", "--root", "--wd", "--env",
                                          "--no-fork", "--join-cgroup", "--user-parent"),
                                Option("-t", ("1", "bad", "999999999"), False), Option("--target", ("1",), True),
                                Option("--mount", ("/proc/self/ns/mnt", "/proc/1/ns/mnt", "missing", ":1"), True),
                                Option("--net", ("/proc/self/ns/net",), True), Option("--user", ("/proc/self/ns/user",), True),
                                Option("-S", ("0", "follow", "bad", "1000"), None), Option("--setuid", ("follow",), True),
                                Option("-G", ("0", "follow", "1000"), False), Option("--setgid", ("follow",), True),
                                Option("--root", ("dir", "/nonexistent"), True), Option("--wd", ("dir", "/nonexistent"), True),
                                Option("-W", ("/tmp", "dir"), False), Option("-N", ("3", "bad"), False)),
            operands=((), (UL_OBS, "ids"), ("missing",), (UL_OBS, "echo", "x"), (UL_OBS, "ns"), (UL_OBS, "pwd")),
            stdin=("empty",), fixture="ul", max_flags=3, env=(("SHELL", "/bin/false"),),
            extra=(("-m/proc/self/ns/mnt", UL_OBS, "echo"), ("-U/proc/self/ns/user", "--preserve-credentials"),
                   ("-m", UL_OBS, "echo"), ("-t", "impossible", "-m", UL_OBS, "echo"),
                   ("--user=/proc/self/ns/user", "--preserve-credentials", UL_OBS, "ids"))),

    # -- the ls* inventories -----------------------------------------------
    Utility("lsclocks", options=(*ul_flags("-J", "-n", "-r", "--json", "--noheadings", "--raw", "--output-all",
                                           "--no-discover-dynamic", "--no-discover-rtc"),
                                 Option("-o", ("ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET", "TYPE,NAME", "ID", "TIME",
                                               "ISO_TIME", "REL_TIME", "+NS_OFFSET", "BAD", "", "TYPE,TYPE"), False),
                                 Option("--output", ("ID,NAME",), True),
                                 Option("-t", ("realtime", "monotonic", "boottime", "tai", "realtime-coarse",
                                               "moonwater-not-a-clock", "CLOCK_REALTIME", "0", ""), None),
                                 Option("-d", ("/dev/moonwater-not-a-clock", "/dev/ptp0", "data"), False),
                                 Option("-c", ("1", "bad", "999999999"), False),
                                 Option("-x", ("/dev/rtc0", "/dev/nope", "data"), False)),
            operands=((), ("x",)), stdin=("empty",), fixture="ul", normalize=ul_norm_clocks, max_flags=4),
    Utility("lscpu", options=(*ul_flags("-a", "-b", "-B", "-C", "-c", "-J", "-e", "-p", "-r", "-x", "-y", "-H", "--all",
                                        "--online", "--offline", "--bytes", "--json", "--extended", "--parse", "--raw",
                                        "--hex", "--physical", "--output-all", "--caches", "--list-columns"),
                              Option("-C", ("NAME,ONE-SIZE", "ALL-SIZE,LEVEL,TYPE,WAYS", "NAME,SETS,PHY-LINE,COHERENCY-SIZE",
                                            "BAD", ""), True),
                              Option("-e", ("CPU,CORE,SOCKET,NODE,ONLINE", "CPU,CORE", "CPU,CONFIGURED", "CPU,ADDRESS",
                                            "CPU,MHZ", "CPU,MAXMHZ,MINMHZ", "CPU,CACHE", "CPU,POLARIZATION", "BAD", "",
                                            "CPU,BOGOMIPS", "CPU,MODELNAME"), True),
                              Option("-p", ("CPU,CORE", "CPU,CORE,SOCKET,NODE", "CPU,ADDRESS", "BAD", "CPU,CACHE"), True),
                              Option("-s", ("/", "dir", "missing"), False),
                              Option("--annotate", ("never", "always", "auto"), True),
                              Option("--hierarchic", ("never", "always", "auto", "bad"), True)),
            operands=((), ("x",)), stdin=("empty",), fixture="ul", normalize=ul_norm_lscpu, max_flags=3),
    Utility("lsmem", options=(*ul_flags("-J", "-P", "-a", "-b", "-n", "-r", "--json", "--pairs", "--all", "--bytes",
                                        "--noheadings", "--raw", "--output-all", "--summary"),
                              Option("-o", ("RANGE,SIZE,STATE,REMOVABLE,BLOCK,NODE,ZONES", "+NODE,ZONES", "RANGE",
                                            "CONFIGURED", "MEMMAP-ON-MEMORY", "BAD", "", "NODE", "ZONES"), None),
                              Option("-S", ("ZONES", "NODE", "STATE", "BAD", "REMOVABLE"), False),
                              Option("-s", ("/", "dir"), False),
                              Option("--summary", ("only", "never", "always", "bad"), True)),
            operands=((), ("x",)), stdin=("empty",), fixture="ul", max_flags=4),
    Utility("lsblk", options=(*ul_flags("-A", "-D", "-J", "-M", "-O", "-P", "-S", "-N", "-v", "-T", "-a", "-b", "-d", "-f", "-i",
                                        "-l", "-m", "-n", "-p", "-r", "-s", "-t", "-y", "-z", "-H", "--json", "--list",
                                        "--raw", "--bytes", "--nodeps", "--paths", "--fs", "--perms", "--topology",
                                        "--scsi", "--all", "--noheadings", "--output-all", "--pairs", "--tree", "--ascii",
                                        "--inverse", "--noempty", "--discard", "--zoned"),
                              Option("-E", ("NAME", "SIZE"), False), Option("-I", ("259", "8", "bad", "253"), False),
                              Option("-e", ("7", "253", "bad", "259"), None), Option("-Q", ("SIZE>0",), False),
                              Option("-T", ("NAME", "BAD"), True),
                              Option("-o", ("NAME", "KNAME,SERIAL,VENDOR,MODEL,REV,HCTL,TRAN", "NAME,KNAME,SIZE,RO,RM,TYPE,MOUNTPOINTS",
                                            "KNAME,MOUNTPOINTS", "NAME,UUID", "NAME,OWNER", "NAME,VENDOR", "+SIZE", "PATH",
                                            "MAJ:MIN,FSTYPE,LABEL", "BAD", "", "NAME,PARTUUID,PARTTYPE,PTTYPE,PTUUID",
                                            "NAME,ROTA,SCHED,RQ-SIZE,MIN-IO,OPT-IO,PHY-SEC,LOG-SEC,RA,WSAME",
                                            "NAME,MODE,GROUP,OWNER", "NAME,STATE,ALIGNMENT,DISC-ALN,DISC-GRAN,DISC-MAX,DISC-ZERO",
                                            "NAME,ZONED,ZONE-SZ", "NAME,FSAVAIL,FSUSE%,FSSIZE,FSUSED", "NAME,MOUNTPOINT",
                                            "NAME,PKNAME,START,PARTN,PARTFLAGS", "NAME,SUBSYSTEMS,HOTPLUG,RAND,DAX",
                                            "NAME,MAJ,MIN,DISK-SEQ,MQ,WWN,ID,ID-LINK"), False),
                              Option("--output", ("NAME,SIZE",), True),
                              Option("-w", ("40", "200", "bad"), False), Option("-x", ("NAME", "SIZE", "BAD"), False),
                              Option("--sysroot", ("/", "dir"), True), Option("--hyperlink", ("never", "always"), True),
                              Option("--properties-by", ("udev", "file"), True)),
            operands=((), ("missing",), ("/dev/null",), ("dir",), ("/dev/zero", "/dev/null")),
            stdin=("empty",), fixture="ul", normalize=ul_norm_lsblk, max_flags=4),
    Utility("lsfd", options=(*ul_flags("-l", "-J", "-n", "-r", "-u", "-i", "-H", "--threads", "--json", "--noheadings", "--raw",
                                       "--notruncate", "--debug-filter", "--dump-counters", "--summary", "--list-columns"),
                             Option("-i", ("4", "6", "bad"), True), Option("--inet", ("4",), True),
                             Option("-o", ("FD,MODE,KNAME,INODE,MAJ:MIN", "FD,NAME", "FD,TYPE", "+UID", "FD,MODE", "FD,XMODE",
                                           "FD,POS", "FD,UID", "FD,USER", "COMMAND,PID,ASSOC", "EVENTPOLL.TFDS", "BAD", "",
                                           "ASSOC,STTYPE,SOURCE,FLAGS", "FD,DEV,RDEV,SIZE,NLINK,FUID,OWNER"), False),
                             Option("-p", ("1", "1,2", "bad", "999999999", ""), None), Option("--pid", ("1",), True),
                             Option("-Q", ("PID==1", "bad"), False), Option("-C", ("count:FD:FD",), False),
                             Option("--hyperlink", ("never",), True), Option("--summary", ("only", "append", "never", "bad"), True)),
            operands=((), ("x",)), stdin=("empty",), fixture="ul", valid=ul_needs_pid, normalize=ul_norm_pids, max_flags=4),
    Utility("lslocks", options=(*ul_flags("-b", "-J", "-i", "-n", "-r", "-u", "-H", "--bytes", "--json", "--noinaccessible",
                                          "--noheadings", "--raw", "--notruncate", "--output-all", "--list-columns"),
                                Option("-o", ("COMMAND,PID,TYPE,SIZE,INODE,MAJ:MIN,MODE,M,START,END,PATH,BLOCKER", "PID,MODE,BLOCKER",
                                              "SIZE,INODE,PATH", "+INODE", "HOLDERS", "BAD", "", "PID,TYPE,MODE,START,END,PATH"), False),
                                Option("-p", ("1", "bad", "999999999"), None), Option("--pid", ("1",), True),
                                Option("-Q", ("PID==1",), False)),
            operands=((), ("x",)), stdin=("empty",), fixture="ul", valid=ul_needs_pid, max_flags=4),
    Utility("lsns", options=(*ul_flags("-J", "-l", "-n", "-r", "-u", "-W", "-P", "-T", "-H", "--json", "--list", "--noheadings",
                                       "--raw", "--notruncate", "--nowrap", "--persistent", "--tree", "--output-all",
                                       "--list-columns"),
                             Option("-o", ("NS,TYPE", "NS,TYPE,PID,PPID,UID,USER", "TYPE,PATH", "NS,TYPE,NPROCS,PID,UID,USER",
                                           "+COMMAND", "NETNSID,NSFS", "PNS,ONS", "BAD", "", "TYPE,TYPE"), False),
                             Option("-p", ("1", "bad", "999999999"), False), Option("--task", ("1",), True),
                             Option("-t", ("mnt", "net", "user", "pid", "uts", "ipc", "cgroup", "time", "impossible", "mnt,net"), None),
                             Option("-T", ("parent", "owner", "process", "bad"), True), Option("-Q", ("TYPE=='mnt'",), False)),
            operands=((), ("x",), ("4026531840",)), stdin=("empty",), fixture="ul", valid=ul_needs_pid, max_flags=4),
    Utility("rfkill", options=(*ul_flags("-J", "-n", "-r", "--json", "--noheadings", "--raw", "--output-all"),
                               Option("-o", ("ID,DEVICE,SOFT", "ID,TYPE,DEVICE,SOFT,HARD", "TYPE-DESC", "BAD", ""), False)),
            operands=((), ("list",), ("list", "wlan"), ("list", "7"), ("block", "wlan"), ("unblock", "all"), ("toggle", "all"),
                      ("unknown",), ("help",), ("block",), ("list", "bad-type")),
            stdin=("empty",), fixture="none"),
    Utility("mesg", options=ul_flags("-v", "--verbose"),
            operands=(("y",), ("n",), ("yes",), ("no",), (), ("x",), ("y", "n"), ("Y",), ("",)),
            stdin=("empty",), fixture="none"),

    # -- live fixtures: a holder, a target, a namespace ---------------------
    Utility("lslocks_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("lslocks", UL_LOCK_SETUP, UL_LOCK_TEARDOWN),
            options=(*ul_flags("-b", "-J", "-i", "-n", "-r", "-u"),
                     Option("-o", ("COMMAND,PID,TYPE,SIZE,INODE,MAJ:MIN,MODE,M,START,END,PATH,BLOCKER", "PID,MODE,BLOCKER",
                                   "SIZE,INODE,PATH", "+INODE", "PID,TYPE,MODE,START,END,PATH", "COMMAND,TYPE,MODE,PATH"), False)),
            operands=(("-p", "@HOLDER"), ("-p", "@POSIX"), ("-p", "@WAITER")), stdin=("empty",), max_flags=4),
    Utility("lsfd_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("lsfd", UL_FD_SETUP, UL_FD_TEARDOWN),
            options=(*ul_flags("-J", "-n", "-r", "-u"),
                     Option("-o", ("FD,MODE,KNAME,MAJ:MIN", "FD,NAME", "FD,TYPE", "+UID", "FD,MODE", "FD,XMODE", "FD,POS",
                                   "FD,USER", "ASSOC,STTYPE,TYPE", "FD,FLAGS", "FD,DEV,SIZE,NLINK,FUID,OWNER",
                                   "FD,SOCK.TYPE,SOCK.STATE,SOCK.LISTENING,TCP.LADDR", "FD,AINODECLASS,EVENTPOLL.TFDS"), False)),
            operands=(("-p", "@PID"),), stdin=("empty",), max_flags=4),
    Utility("ipcs_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("ipcs", UL_IPC_SETUP, "", wrap="unshare -Uri"),
            options=(*ul_flags("-m", "-q", "-s", "-a", "-t", "-p", "-c", "-l", "-u", "--human", "-b"),
                     Option("-i", ("@Q", "@M", "@S", "999999", "bad"), False)),
            operands=((),), stdin=("empty",), max_flags=3),
    Utility("lsipc_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("lsipc", UL_IPC_SETUP, "", wrap="unshare -Uri"),
            options=(*ul_flags("-m", "-M", "-q", "-Q", "-s", "-S", "-g", "--noheadings", "--notruncate", "-b", "-c", "-e", "-J",
                               "-n", "-l", "-P", "-r", "-t", "-y"),
                     Option("-i", ("@Q", "@M", "@S", "999999", "bad"), False), Option("-N", ("x",), False),
                     Option("--time-format", ("short", "full", "iso", "bad"), True),
                     Option("-o", ("KEY,ID", "KEY", "ID,OWNER,PERMS", "+CTIME", "BAD", "", "ID,CUID,CGID,UID,GID"), False)),
            operands=((),), stdin=("empty",), max_flags=4),
    Utility("ipcmk_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("ipcmk", UL_IPC_SETUP, UL_IPC_VIEW, wrap="unshare -Uri"),
            options=(Option("-M", ("4096", "0", "bad", "1"), False), Option("-S", ("3", "0", "4294967297", "2147483647", "bad"), False),
                     *ul_flags("-Q", "-s", "-q"), Option("-m", ("4096",), False),
                     Option("-p", ("0640", "0600", "bad", "777"), False), Option("-n", ("name",), False)),
            operands=((),), stdin=("empty",), max_flags=4),
    Utility("ipcrm_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("ipcrm", UL_IPC_SETUP, UL_IPC_VIEW, wrap="unshare -Uri"),
            options=(Option("-q", ("@Q", "999999", "bad"), False), Option("-m", ("@M", "999999"), False),
                     Option("-s", ("@S", "999999"), False), Option("-Q", ("0", "bad"), False), Option("-M", ("0",), False),
                     Option("-S", ("0",), False), *ul_flags("-a", "-v"), Option("--all", ("shm", "msg", "sem", "bad"), True),
                     Option("--posix-shmem", ("x",), True)),
            operands=((), ("shm", "@M"), ("msg", "@Q"), ("sem", "@S"), ("shm", "999999"), ("bad", "@M"), ("shm",)),
            stdin=("empty",), max_flags=3),
    Utility("mount_live", modes=("bash",), stderr="loose", normalize=ul_norm_mounts,
            script=ul_script("mount", UL_MOUNT_SETUP, UL_MOUNT_VIEW, wrap="unshare -Urm"),
            options=(*ul_flags("-v", "-n", "-c", "-i", "-f", "-r", "-w", "-l"),
                     Option("-o", ("nodev,nosuid", "ro", "size=1m", "remount,ro", "bind", "rbind", "noexec", "bad", ""), False)),
            operands=(("-t", "tmpfs", "tmpfs", "a"), ("--bind", "c", "b"), ("-a", "-T", "fstab"), ("-a", "-T", "fstab-malformed"),
                      ("-L", "moondata", "a"), ("tmpfs", "a"), ("-t", "tmpfs", "tmpfs", "missing"), ("--make-rprivate", "/"),
                      ("-B", "c", "b"), ("-M", "c", "b"), ("-t", "proc", "proc", "a"), ("--source", "tmpfs", "--target", "a", "-t", "tmpfs"),
                      ("-t", "tmpfs", "-o", "ro", "tmpfs", "a"), ("-t", "tmpfs", "tmpfs", "clink"), ("-t", "tmpfs", "tmpfs", "a", "b"),
                      ("--types=tmpfs", "tmpfs", "a"), ("-t", "tmpfs", "tmpfs", "fstab target"), ("-R", "c", "b"),
                      ("-t", "tmpfs", "tmpfs", "a", "--make-private")),
            stdin=("empty",), max_flags=3),
    Utility("umount_live", modes=("bash",), stderr="loose", normalize=ul_norm_mounts,
            script=ul_script("umount", UL_MOUNT_SETUP + UL_MOUNT_PREMOUNT, UL_MOUNT_VIEW, wrap="unshare -Urm"),
            options=(*ul_flags("-l", "-v", "-n", "-i", "-c", "-f", "-r", "-d", "-q"),
                     Option("-t", ("tmpfs", "notmpfs", "proc"), False)),
            operands=(("c",), ("b",), ("-R", "c"), ("c/sub",), ("missing",), ("a",), ("c", "b"), ("clink",), ("./c",), ("c/",),
                      ("-lv", "c"), ("-vl", "c"), ("-nvi", "c"), ("-lvt", "tmpfs", "c"), ("tmpfs",), ("-A", "tmpfs")),
            stdin=("empty",), max_flags=3),
    Utility("mountpoint_live", modes=("bash",), stderr="loose", normalize=ul_norm_mounts,
            script=ul_script("mountpoint", UL_MOUNT_SETUP + UL_MOUNT_PREMOUNT, "", wrap="unshare -Urm"),
            options=ul_flags("-q", "-d", "--nofollow", "-x"),
            operands=(("c",), ("b",), ("a",), ("c/sub",), ("clink",), ("./c",), ("c/",), ("missing",), ("c", "a")),
            stdin=("empty",), max_flags=3),
    Utility("findmnt_live", modes=("bash",), stderr="loose", normalize=ul_norm_mounts,
            script=ul_script("findmnt", UL_MOUNT_SETUP + UL_MOUNT_PREMOUNT, "", wrap="unshare -Urm"),
            options=(*ul_flags("-n", "-r", "-l", "-v", "-P", "-f", "-i", "-R", "-J"),
                     Option("-o", ("TARGET,FSTYPE", "OPTIONS,VFS-OPTIONS,FS-OPTIONS", "SOURCE,FSROOT,MAJ:MIN,ID,PARENT", "SOURCE",
                                   "TARGET,PROPAGATION", "TARGET,SOURCE,FSTYPE,OPTIONS"), False),
                     Option("-t", ("tmpfs", "notmpfs"), False), Option("-O", ("ro", "norw", "nosuid"), False)),
            operands=(("-T", "c"), ("-T", "b"), ("-M", "c"), ("-T", "c/sub"), ("-M", "c/sub"), ("-T", "a"), ("-S", "tmpfs"),
                      ("-T", "clink"), ("c",), ("-M", "b")),
            stdin=("empty",), max_flags=3),
    Utility("lsns_live", modes=("bash",), stderr="loose", normalize=ul_norm_live,
            script=ul_script("lsns", "self=$$\nns=$(env stat -Lc %i /proc/$$/ns/mnt)\n", "", wrap="unshare -Urn"),
            options=(*ul_flags("-n", "-r", "-J", "-l", "-u"),
                     Option("-o", ("NS,TYPE", "NS,TYPE,PID,PPID,UID,USER", "TYPE,PATH", "NS,TYPE,NPROCS,UID,USER", "TYPE,NETNSID,NSFS",
                                   "TYPE,PNS,ONS"), False),
                     Option("-t", ("mnt", "net", "user", "impossible"), False)),
            operands=(("-p", "@SELF"), ("-l", "-p", "@SELF", "@NS")), stdin=("empty",), valid=ul_lsns_live_valid, max_flags=4),
    Utility("waitpid_live", modes=("bash",), stderr="loose", normalize=ul_norm_pids,
            script=ul_script("waitpid", UL_WAIT_SETUP, UL_WAIT_TEARDOWN),
            options=(*ul_flags("-v", "-e"), Option("-t", ("1", ".01", "0"), False), Option("-c", ("1", "2"), False)),
            operands=(("@PID",), ("@PID", "@TWO"), ("@GONE",), ("@PIDINO",), ("@TWO:1",), ("@PID", "1"), ("@GONE", "@PID")),
            stdin=("empty",), valid=ul_waitpid_valid, max_flags=3),
    Utility("nsenter_live", modes=("bash",), stderr="loose", normalize=ul_norm_pids,
            script=ul_script("nsenter", UL_TARGET_SETUP, UL_TARGET_TEARDOWN, observe=True),
            options=(*ul_flags("-U", "-m", "-n", "-i", "-u", "-p", "--preserve-credentials", "-F", "-w", "-r", "-W", "-e"),
                     Option("-S", ("7", "follow", "0", "invalid"), False), Option("-G", ("8", "follow", "invalid"), False),
                     Option("--wd", ("dir",), True), Option("--root", ("dir",), True)),
            operands=(("-t", "@A", UL_OBS, "ns"), ("-t", "@B", UL_OBS, "ids"), ("-t", "@C", UL_OBS, "pwd"), ("-t", "@A", UL_OBS, "ids"),
                      ("--target", "@A", UL_OBS, "exit7"), ("-t", "@C", "-U", "--preserve-credentials", "-w", UL_OBS, "pwd")),
            stdin=("empty",), max_flags=3),
    Utility("renice_live", modes=("bash",), stderr="loose", normalize=ul_norm_pids,
            script=ul_script("renice", "env sleep 2 </dev/null &\npid=$!\n", "env ps -o ni= -p $pid | tr -d ' '\nkill $pid; wait $pid 2>/dev/null\n"),
            options=(Option("-n", ("0", "5", "19", "-5", "20", "bad"), False), Option("--priority", ("3",), True),
                     Option("--relative", ("0", "2", "9223372036854775807", "-1"), True)),
            operands=(("-p", "@PID"), ("5", "-p", "@PID"), ("5", "@PID"), ("1", "-p", "@PID", "@PID"), ("-p", "@PID", "-n", "4"),
                      ("--relative", "1", "-p", "@PID")),
            stdin=("empty",), max_flags=2),
    Utility("flock_live", modes=("bash",), stderr="loose", normalize=ul_norm_flock,
            script=ul_script("flock", ": > lock\nenv flock -x lock sleep 2 </dev/null >/dev/null 2>&1 &\nholder=$!\ni=0\nuntil [ \"$(env lslocks -n -p $holder -o PID 2>/dev/null)\" ] || [ $i -ge 150 ]; do sleep .02; i=$((i+1)); done\n",
                             "kill $holder 2>/dev/null; wait $holder 2>/dev/null\n", observe=True),
            options=(*ul_flags("-x", "-s", "-u", "-n", "-o", "-F", "--fcntl", "--verbose"),
                     Option("-w", ("0.05", "0", "1e-2"), False), Option("-E", ("42", "0", "300"), False)),
            operands=(("lock", UL_OBS, "echo"), ("lock", UL_OBS, "flock_nb"), ("lock", "-c", "echo held"), ("-n", "-E", "42", "lock", "true"),
                      ("-w", "0.05", "-E", "42", "lock", "true"), ("--close", "lock", UL_OBS, "flock_nb"), ("lock",)),
            stdin=("empty",), max_flags=3),
    Utility("mesg_live", modes=("bash",), stderr="loose",
            script=ul_script("mesg", UL_MESG_PY, "", status="status=$?"),
            operands=((),), stdin=("empty",), max_flags=1,
            extra=(("on",), ("off",), ("on", "-v", "n"), ("off", "-v", "y"), ("off", "yes"), ("on", "no"), ("on", "-v"),
                   ("off", "-v"), ("on", "x"), ("off", "y", "n"))),
    Utility("lsblk_live", modes=("bash",), stderr="loose", normalize=ul_norm_lsblk,
            script=ul_script("lsblk", "dev=$(env lsblk -d -n -r -o PATH | env sed -n 1p)\n", ""),
            options=(*ul_flags("-d", "-n", "-r", "-J", "-p", "-b", "-l", "-P", "-f", "-m", "-t", "-S"),
                     Option("-o", ("PATH", "NAME,SIZE", "KNAME,TYPE,MOUNTPOINTS", "NAME,MAJ:MIN,RM,RO"), False)),
            operands=(("@DEV",), ("@DEV", "@DEV"), ("@DEV", "missing")), stdin=("empty",), max_flags=3),
    Utility("lsclocks_live", modes=("bash",), stderr="loose", normalize=ul_norm_clocks,
            script=ul_script("lsclocks", "export LSCLOCKS_COLUMNS=ID,CLOCK,NAME,TYPE,RESOL,RESOL_RAW,NS_OFFSET\n", ""),
            options=ul_flags("-r", "-n", "-J", "--no-discover-dynamic", "--no-discover-rtc"),
            operands=((), ("-o", "ID,NAME")), stdin=("empty",), max_flags=3),
)


def ul_mesg_script_fix():
    """mesg_live's first operand chooses the pty mode; the python harness
    consumes it. Rewrite the generic script builder so `run` is not used."""
    def script(argv, stdin_name):
        mode = argv[0] if argv else "on"
        rest = argv[1:]
        body = UL_MESG_PY + "python3 mesg.py \"$tool\" %s %s\nstatus=$?\nexit $status\n" % (
            shlex.quote(mode), ul_words(rest))
        return ul_live("mesg", body)
    return script


UTILITIES = tuple(
    Utility(**{**u.__dict__, "script": ul_mesg_script_fix()}) if u.name == "mesg_live" else u
    for u in UTILITIES)


# ---------------------------------------------------------------------------
#       CHECKS: properties beyond the differential.
# ---------------------------------------------------------------------------

# The 130-name denominator: the sorted set of installed executable targets of
# a default Meson build of the signed util-linux 2.42.2 release,
# https://kernel.org/pub/linux/utils/util-linux/v2.42/, artifact SHA-256
# 03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a. Kept here
# rather than discovered from the host so a package split cannot move it.
UL_RELEASE_SHA256 = "03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
UL_UPSTREAM = ("addpart agetty bits blkdiscard blkid blkpr blkzone blockdev cal cfdisk chcpu chfn chmem choom "
               "chrt chsh col colcrt colrm column copyfilerange coresched ctrlaltdel delpart dmesg eject enosys "
               "exch fadvise fallocate fdisk fincore findfs findmnt flock fsck fsck.cramfs fsck.minix fsfreeze "
               "fstrim getino getopt hardlink hexdump hwclock ionice ipcmk ipcrm ipcs irqtop isosize kill last "
               "lastlog2 ldattach line logger login look losetup lsblk lsclocks lscpu lsfd lsipc lsirq lslocks "
               "lslogins lsmem lsns mcookie mesg mkfs mkfs.bfs mkfs.cramfs mkfs.minix mkswap more mount "
               "mountpoint namei newgrp nologin nsenter partx pg pipesz pivot_root prlimit readprofile rename "
               "renice resizepart rev rfkill rtcwake runuser script scriptlive scriptreplay setarch setpgid "
               "setpriv setsid setterm sfdisk su sulogin swaplabel swapoff swapon switch_root taskset tunelp "
               "uclampset ul umount unshare utmpdump uuidd uuidgen uuidparse vipw waitpid wall wdctl whereis "
               "wipefs write zramctl").split()
# Every upstream name is on exactly one side; dispatching a remaining name
# fails this until the capability claim moves.
UL_SUPPORTED = ("addpart bits blkid blockdev cal choom chrt col colcrt colrm column copyfilerange coresched "
                "ctrlaltdel delpart dmesg exch fadvise fallocate fincore findfs findmnt flock getino getopt "
                "hardlink hexdump ionice ipcmk ipcrm ipcs isosize kill last line logger look lsblk lsclocks lscpu "
                "lsfd lsipc lslocks lsmem lsns mcookie mesg mkswap mount mountpoint namei nologin nsenter pipesz "
                "pivot_root prlimit rename renice resizepart rev rfkill script scriptreplay setarch setpgid "
                "setpriv setsid swaplabel taskset uclampset ul umount unshare utmpdump uuidgen uuidparse waitpid "
                "wall whereis wipefs write").split()


def ul_dispatched(farm):
    """Names the multicall binary answers to: from tools.inc when the source
    is at hand, else the farm."""
    tools = Path(__file__).resolve().parents[1] / "src" / "sh" / "tools.inc"
    if tools.exists():
        names = set()
        for line in tools.read_text().splitlines():
            parts = re.split(r"[(),\s]+", line)
            if parts and parts[0] == "SHELL_TOOL" and len(parts) > 2:
                names.add(parts[2])
        return names
    return set(os.listdir(farm))


def ul_check_denominator(farm):
    """Every executable of a default util-linux 2.42.2 build is either
    provided or a listed deliberate absence, and nothing is both."""
    dispatched = ul_dispatched(farm)
    upstream = set(UL_UPSTREAM)
    supported = set(UL_SUPPORTED)
    notes = []
    if len(upstream) != 130:
        notes.append(f"denominator has {len(upstream)} names, not 130")
    for name in sorted(supported - upstream):
        notes.append(f"{name}: claimed supported but not an upstream executable")
    for name in sorted(supported):
        if name not in dispatched:
            notes.append(f"{name}: claimed supported but absent from dispatch")
    for name in sorted(upstream - supported):
        if name in dispatched:
            notes.append(f"{name}: now dispatched -- move it to UL_SUPPORTED")
    return len(upstream) - len(notes), len(upstream), notes


def ul_run(argv, env=None, stdin=b"", timeout=10):
    environment = dict(os.environ, LC_ALL="C", TZ="UTC0")
    environment.update(env or {})
    try:
        done = subprocess.run(argv, input=stdin, capture_output=True, env=environment, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, b"", b"timeout"
    return done.returncode, done.stdout, done.stderr


def ul_check_rfkill(farm):
    """Ours reads a bounded sysfs snapshot and writes the exact rfkill ABI
    records; the box has no rfkill devices, so the oracle is these bytes."""
    farm = Path(farm)
    notes = []
    total = 7
    with tempfile.TemporaryDirectory(prefix="ul-rfkill-") as work:
        root = Path(work) / "sys"
        for index, device, kind, soft, hard in ((3, "wifi-card", "wlan", 0, 0), (7, "blue-chip", "bluetooth", 1, 0),
                                                (11, "modem", "wwan", 0, 1)):
            directory = root / f"rfkill{index}"
            directory.mkdir(parents=True)
            for name, value in (("name", device), ("type", kind), ("soft", soft), ("hard", hard)):
                (directory / name).write_text(f"{value}\n")
        events = Path(work) / "events"
        env = {"MOONWATER_RFKILL_ROOT": str(root), "MOONWATER_RFKILL_DEVICE": str(events)}
        tool = str(farm / "rfkill")
        table = (b"ID TYPE      DEVICE         SOFT      HARD\n"
                 b" 3 wlan      wifi-card unblocked unblocked\n"
                 b" 7 bluetooth blue-chip   blocked unblocked\n"
                 b"11 wwan      modem     unblocked   blocked\n")
        legacy = b"3: wifi-card: Wireless LAN\n\tSoft blocked: no\n\tHard blocked: no\n"
        status, out, _ = ul_run([tool], env)
        if (status, out) != (0, table):
            notes.append(f"table: status {status} output {out!r}")
        status, out, _ = ul_run([tool, "-r", "-o", "ID,DEVICE,SOFT", "bluetooth"], env)
        if (status, out) != (0, b"ID DEVICE SOFT\n7 blue-chip blocked\n"):
            notes.append(f"raw selection: status {status} output {out!r}")
        status, out, _ = ul_run([tool, "list", "wlan"], env)
        if (status, out) != (0, legacy):
            notes.append(f"legacy list: status {status} output {out!r}")
        status, out, _ = ul_run([tool, "-J"], env)
        if status != 0 or b'"id": 3' not in out or b'"device": "blue-chip"' not in out or \
                b'"hard": "blocked"' not in out or out.count(b'"id":') != 3:
            notes.append(f"json: status {status} output {out!r}")
        events.write_bytes(b"")
        status, _, _ = ul_run([tool, "block", "wlan", "7"], env)
        if status != 0 or events.read_bytes().hex() != "00000000010301000700000000020100":
            notes.append(f"block records: status {status} bytes {events.read_bytes().hex()}")
        events.write_bytes(b"")
        status, _, _ = ul_run([tool, "toggle", "all"], env)
        if status != 0 or events.read_bytes().hex() != "030000000102010007000000020200000b00000005020100":
            notes.append(f"toggle records: status {status} bytes {events.read_bytes().hex()}")
        status, _, _ = ul_run([tool, "event"], env, timeout=3)
        if status != 1:
            notes.append(f"event monitor: expected refusal status 1, got {status}")
    return total - len(notes), total, notes


UL_LSCPU_FIXTURE_PREFIX = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
typedef uint64_t positive;
typedef int64_t bipolar;
typedef int32_t b32;
typedef uint32_t p32;
typedef unsigned char p8;
typedef const char *string_address;
typedef void *address_any;
typedef void fn;
#define address_to *
#define address_of &
#define null NULL
#define end 0
#define positive_bits 64
#define positive_max UINT64_MAX
#define UL_CPU_WORDS 1024
#define FILE_PATH_MAX 4096
#define TEXT_ARENA_BYTES (192u << 20)
#define X64 1
#define X86 0
#define TEXT_ARENA_GROW
#define memory_copy_apart memcpy
#define memory_growth(room,wanted,first) ((wanted) > (room)*2 ? ((wanted) > (first) ? (wanted) : (first)) : (room)*2)
#define memory_fill memset
#define memory_copy memcpy
#define memory_copy_end(d,s,n) ((p8 *)memcpy((d),(s),(n))+(n))
#define string_length(s) strlen((const char *)(s))
#define string_get(s) (*(s))
#define string_is(s,c) (*(s)==(c))
#define string_search(s,n) strstr((const char *)(s),(n))
#define string_compare_max(s,t,n) strncmp((const char *)(s),(t),(n))
#define byte_to_upper(c) toupper((unsigned char)(c))
#define bits_counted(n) __builtin_popcountll(n)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define array_count(a) (sizeof(a)/sizeof((a)[0]))
typedef void (*writer)(address_any,positive);
typedef struct { p8 system[65],node[65],release[65],version[65],machine[65],domain[65]; } file_machine;
static p8 fixture_arena[TEXT_ARENA_BYTES];
static p8 *text_arena;
static positive text_arena_used;
static bool ul_lscpu_failed;
static positive fixture_errors;
static void text_error(void *unused, const char *message) { (void)unused; (void)message; fixture_errors++; }
static void *memory(positive bytes) { (void)bytes; abort(); }
static bool system_failed(positive value) { return value >= positive_max-4095; }
static positive positive_into_string(p8 *p,positive n) { return (positive)sprintf((char *)p,"%llu",(unsigned long long)n); }
static void positive_to_string(writer w,positive n) { p8 b[24]; positive k=positive_into_string(b,n); w(b,k); }
static positive storage_hex_padded(p8 *p,p32 n,positive width,bool upper) { (void)upper; return (positive)sprintf((char *)p,"%0*x",(int)width,n); }
typedef struct { const char *key,*heading; int width; bool numeric; int kind; } ul_table_column;
#define UL_TABLE_STRING 0
static positive captured_count;
static void *captured_items;
static bool captured_json;
#define ul_table(name,items,count,definitions,columns,n,headings,raw,field) do { captured_count=(count); captured_items=(items); captured_json=((name)!=NULL); } while(0)
typedef struct {int unused;} file_walk;
struct linux_dirent64 {char d_name[256];};
#define AT_FDCWD -100
static positive mock_next_calls;
static bool file_walk_open(file_walk *w,int fd,const char *path) { (void)w;(void)fd; if(strcmp(path,"/sys/devices/system/cpu/vulnerabilities"))abort(); return true; }
static struct linux_dirent64 *file_walk_next(file_walk *w) { (void)w; mock_next_calls++; return NULL; }
static void file_walk_close(file_walk *w) { (void)w; }
static void path_join(p8 *p,positive n,const char *base,const char *name) { (void)p;(void)n;(void)base;(void)name;abort(); }
static bipolar ul_slurp_word(p8 *path,p8 *value,positive size) { (void)path;(void)value;(void)size;abort(); }
'''

UL_LSCPU_FIXTURE_MAIN = r'''
static void run_case(positive n,bool duplicate,bool negative,bool json,bool exhaust) {
  text_arena=fixture_arena; text_arena_used=0; ul_lscpu_failed=false;
  captured_count=0; captured_items=NULL; fixture_errors=0;
  memset(&ul_lscpu,0,sizeof ul_lscpu);
  memset(ul_lscpu_present,0,sizeof ul_lscpu_present);
  memset(ul_lscpu_online,0,sizeof ul_lscpu_online);
  ul_lscpu.cpus=text_arena_take(n*sizeof(*ul_lscpu.cpus));
  ul_lscpu.cpu_count=ul_lscpu.present_count=ul_lscpu.online_count=ul_lscpu.core_count=n;
  ul_lscpu.node_count=duplicate?1:n;
  memcpy(ul_lscpu.machine.machine,"fixture64",10);
  for(positive i=0;i<n;i++) {
    ul_lscpu.cpus[i].id=i;
    ul_lscpu.cpus[i].node=negative?-1:duplicate?0:(bipolar)i;
    ul_lscpu_present[i/64]|=(positive)1<<(i%64);
    ul_lscpu_online[i/64]|=(positive)1<<(i%64);
  }
  if(exhaust) text_arena_used=TEXT_ARENA_BYTES;
  mock_next_calls=0;
  ul_lscpu_summary(json,false,false);
  if(exhaust) {
    if(!ul_lscpu_failed || !fixture_errors || captured_items) abort();
  } else {
    ul_lscpu_summary_item *items=captured_items;
    char expected[24]; sprintf(expected,"%llu",(unsigned long long)n);
    positive expected_count=10+(negative?0:duplicate?1:n);
    if(ul_lscpu_failed || captured_count!=expected_count || captured_json!=json ||
       strcmp(items[3].data,expected)) abort();
    positive nodes=negative?0:duplicate?1:n;
    for(positive i=0;i<nodes;i++) {
      char field[64]; sprintf(field,"NUMA node%llu CPU(s):",(unsigned long long)i);
      if(strcmp(items[10+i].field,field)) abort();
      if(!duplicate) {
        sprintf(expected,"%llu",(unsigned long long)i);
        if(strcmp(items[10+i].data,expected)) abort();
      }
    }
  }
  printf("summary cpus=%llu duplicate=%d negative=%d json=%d exhausted=%d rows=%llu OK\n",
         (unsigned long long)n,duplicate,negative,json,exhaust,(unsigned long long)captured_count);
}
int main(void) {
  for(positive json=0;json<2;json++) {
    run_case(1,false,false,json,false);
    run_case(86,false,false,json,false);
    run_case(87,false,false,json,false);
    run_case(97,false,false,json,false);
    run_case(257,false,false,json,false);
    run_case(97,true,false,json,false);
    run_case(97,false,true,json,false);
    run_case(97,false,false,json,true);
  }
  ul_lscpu_summary_rows rows={0}; ul_lscpu_failed=false;
  text_arena_used=0;
  for(positive i=0;i<96;i++) ul_lscpu_summary_add(&rows,"field",ul_lscpu_number(i));
  void *before=rows.items; text_arena_used=TEXT_ARENA_BYTES;
  ul_lscpu_summary_add(&rows,"field","97");
  if(!ul_lscpu_failed || rows.count!=96 || rows.items!=before || strcmp(rows.items[95].data,"95")) abort();
  puts("summary growth allocation failure keeps prior rows OK");
  return 0;
}
'''


def ul_c_function(source, name):
    """The body of one top-level static function, found by its name."""
    match = re.search(r"^static [^\n]*\b" + re.escape(name) + r"\(", source, re.M)
    if not match:
        raise ValueError(name)
    opening = source.index("{", match.start())
    depth, at = 1, opening + 1
    while depth:
        depth += (source[at] == "{") - (source[at] == "}")
        at += 1
    return source[match.start():at]


def ul_c_span(source, begin, finish):
    """Source between two line-anchored markers (the first line matched by
    each regular expression), the finish line excluded."""
    start = re.search(begin, source, re.M)
    stop = re.search(finish, source[start.start():], re.M)
    return source[start.start():start.start() + stop.start()]


def ul_c_typedefs(source, *names):
    """The typedef struct blocks ending in `} NAME;`, in order."""
    out = []
    for name in names:
        finish = source.index("} %s;" % name)
        start = source.rindex("typedef struct", 0, finish)
        out.append(source[start:finish + len("} %s;" % name)])
    return "\n".join(out)


def ul_check_lscpu_summary(farm):
    """The lscpu summary over synthetic topologies the box cannot present:
    1, 86, 87, 97 and 257 CPUs (word boundaries of the CPU set writer), one
    duplicated node, node -1, an exhausted arena and a failed row growth.
    Built at run time from the current source by function-name markers."""
    root = Path(__file__).resolve().parents[1]
    source_path = root / "src" / "sh" / "util_linux.c"
    if not source_path.exists():
        return 0, 1, ["source tree not at hand"]
    compiler = None
    for candidate in ("clang", "gcc", "cc"):
        if any(os.access(os.path.join(directory, candidate), os.X_OK)
               for directory in os.environ.get("PATH", "").split(":") if directory):
            compiler = candidate
            break
    if not compiler:
        return 0, 1, ["no C compiler"]
    source = source_path.read_text()
    try:
        definitions = ul_c_span(source, r"^#define UL_LSCPU_CACHE_MAX\b", r"^static positive ul_lscpu_set_count\(")
        summary_types = ul_c_typedefs(source, "ul_lscpu_summary_item", "ul_lscpu_summary_rows")
        set_globals = ul_c_span(source, r"^static p8 address_to ul_lscpu_set_into;", r"^static fn ul_lscpu_set_write\(")
        names = ("ul_lscpu_set_count", "ul_lscpu_keep", "ul_lscpu_number", "ul_cpu_has", "ul_cpu_list_write",
                 "ul_cpu_mask_write", "ul_lscpu_set_write", "ul_lscpu_set_text", "ul_lscpu_summary_add",
                 "ul_lscpu_cache_size", "ul_lscpu_cache_summary", "ul_lscpu_summary")
        common = (root / "src" / "library.common.c").read_text()
        arena_macro = ul_c_span(common, r"^#define array_arena_reserve\(", r"^#define byte_store_reserve\(")
        text_source = (root / "src" / "sh" / "text.c").read_text()
        file_source = (root / "src" / "sh" / "file.c").read_text()
        program = "\n".join([UL_LSCPU_FIXTURE_PREFIX, arena_macro, ul_c_function(text_source, "text_arena_take"),
                             ul_c_function(file_source, "text_arena_grow"), definitions, summary_types, set_globals]
                            + [ul_c_function(source, name) for name in names] + [UL_LSCPU_FIXTURE_MAIN])
    except (ValueError, AttributeError) as error:
        return 0, 1, [f"marker not found: {error}"]
    with tempfile.TemporaryDirectory(prefix="ul-lscpu-") as work:
        c_path = Path(work) / "lscpu-summary.c"
        c_path.write_text(program)
        binary = Path(work) / "lscpu-summary"
        flags = ["-g", "-O1", "-fno-omit-frame-pointer", "-Wno-pointer-sign", "-w"]
        if compiler != "cc":
            flags += ["-fsanitize=address,undefined"]
        build = subprocess.run([compiler, *flags, str(c_path), "-o", str(binary)], capture_output=True, text=True)
        if build.returncode:
            return 0, 1, ["compile failed: " + build.stderr.strip().splitlines()[-1][:200]]
        try:
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            return 0, 1, ["fixture timed out"]
        if run.returncode != 0:
            return 0, 1, ["fixture aborted: " + (run.stdout + run.stderr).strip()[-300:]]
        cases = run.stdout.count(" OK")
        if cases != 17:
            return 0, 1, [f"expected 17 cases, saw {cases}"]
    return 1, 1, []


CHECKS = (ul_check_denominator, ul_check_rfkill, ul_check_lscpu_summary)
