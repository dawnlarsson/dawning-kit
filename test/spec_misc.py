"""Grammar for the utilities that are neither text nor files.

dd, diff, od, hexdump, factor, numfmt and tsort; the checksums; the process
wrappers (chroot, nohup, stdbuf, timeout and util-linux's coresched,
ctrlaltdel, pipesz, pivot_root, script, scriptreplay); ps; and the login,
identity and kernel-log tools (who, users, pinky, last, utmpdump, wall,
write, logger, uuidgen, uuidparse, mcookie, hostid, dmesg, fincore).

Everything a case reads is generated here from a seed or a struct layout:
the utmp and wtmp databases, checksum manifests (valid, malformed, tagged),
kernel log files in both the syslog and /dev/kmsg formats, script timing
files and typescripts, and the random line pairs and directory trees that
make diff's choice of which identical line to call the changed one
observable. Nothing is written by hand as a case.

Three tools cannot be compared on their live subject: wall and write are
setgid tty on the reference machine and would broadcast, so only their
argument errors are walked; last reads a fixture wtmp because the live one
carries durations against the wall clock; dmesg reads fixture logs because
the ring buffer is restricted to root. logger without --no-act really logs
(both sides do; the messages are tagged mw-differential).
"""

import hashlib
import random
import re
import shlex
import struct
import time

from differential import Utility, Option, INPUTS, FIXTURES


# ----------------------------------------------------------------------------
#       Generated inputs and fixtures.
# ----------------------------------------------------------------------------

def misc_basic_bytes(name):
    """The bytes of a basic fixture file (mode entries included)."""
    entry = FIXTURES["basic"][name]
    if isinstance(entry, tuple):
        return entry[1] if entry[0] == "mode" else b""
    return entry


# utmp: the x86-64 layout is the 384-byte record with 32-bit time fields.
misc_UTMP_SIZE = 384


def misc_utmp_record(kind, pid, line, user, host, seconds, termination=0,
                     status=0, address=b"", identity="id"):
    row = bytearray(misc_UTMP_SIZE)
    struct.pack_into("<h", row, 0, kind)
    struct.pack_into("<i", row, 4, pid)
    for offset, width, value in ((8, 32, line), (40, 4, identity),
                                 (44, 32, user), (76, 256, host)):
        data = value.encode()[:width]
        row[offset:offset + len(data)] = data
    struct.pack_into("<hh", row, 332, termination, status)
    struct.pack_into("<iii", row, 336, 0, seconds, 0)
    row[348:348 + len(address)] = address
    return bytes(row)


def misc_utmp_text_line(kind, pid, line, user, host, seconds, termination=0,
                        status=0, address=b"", identity="id"):
    if len(address) == 16 and any(address[4:]):
        words = ["%x" % (address[i] << 8 | address[i + 1]) for i in range(0, 16, 2)]
        shown = ":".join(words)
    else:
        four = address[:4] + b"\0" * (4 - len(address[:4]))
        shown = "%d.%d.%d.%d" % tuple(four)
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(seconds)) + ",000000+00:00"
    return ("[%d] [%05d] [%-4s] [%-8s] [%-12s] [%-20s] [%-15s] [%s]\n"
            % (kind, pid, identity, user, line, host, shown, stamp)).encode()


# PIDs above pid_max cannot be alive, so the reference's kill(pid, 0) says
# ESRCH and its "gone - no logout" verdict is deterministic; pid 1 is alive
# for everyone (EPERM is not ESRCH), which is its "still logged in".
misc_DEAD = 2000000000
misc_UTMP_ROWS = (
    (2, 0, "~", "reboot", "6.1.0-misc", 1699990000, 0, 0, b"", "~~"),
    (7, misc_DEAD + 1, "pts/99", "root", "remote:3", 1700000000, 0, 0, bytes((192, 0, 2, 1))),
    (7, misc_DEAD + 2, "ttyS0123456789", "daemon", "", 1700003600),
    (7, misc_DEAD + 3, "pts/3", "root", "zeta", 1700007200, 0, 0,
     b"\x20\x01\x0d\xb8" + b"\0" * 11 + b"\x01"),
    (6, misc_DEAD + 4, "tty2", "LOGIN", "", 1700010000),
    (5, misc_DEAD + 5, "tty3", "", "", 1700011000),
    (8, misc_DEAD + 6, "pts/4", "", "", 1700012000, 15, 2),
    (1, ord("3") + ord("2") * 256, "~", "runlevel", "", 1700013000, 0, 0, b"", "~~"),
    (3, 0, "|", "", "", 1700014000),
)
misc_UTMP = b"".join(misc_utmp_record(*row) for row in misc_UTMP_ROWS) + b"partial record"
misc_UTMP_TEXT = b"".join(misc_utmp_text_line(*row) for row in misc_UTMP_ROWS)

# wtmp: three boots. The first segment ends in a shutdown (bob is "down"),
# the second in a crash (carol), the third is current: dave is alive,
# erin's process is dead, frank logged out, and the last user and host are
# longer than their columns.
misc_WTMP_ROWS = (
    (2, 0, "~", "reboot", "5.0.0", 1690000000, 0, 0, b"", "~~"),
    (7, misc_DEAD + 1, "pts/0", "alice", "host1", 1690000600),
    (8, misc_DEAD + 1, "pts/0", "", "", 1690003600),
    (7, misc_DEAD + 2, "tty1", "bob", "", 1690004000),
    (1, ord("0") + ord("3") * 256, "~", "shutdown", "5.0.0", 1690010000, 0, 0, b"", "~~"),
    (2, 0, "~", "reboot", "6.1.0", 1690020000, 0, 0, b"", "~~"),
    (7, misc_DEAD + 3, "pts/1", "carol", "10.0.0.5", 1690020600, 0, 0, bytes((10, 0, 0, 5))),
    (8, misc_DEAD + 9, "pts/7", "", "", 1690021300),
    (6, misc_DEAD + 4, "tty2", "LOGIN", "", 1690021400),
    (1, ord("5") + ord("3") * 256, "~", "runlevel", "6.1.0", 1690021500, 0, 0, b"", "~~"),
    (2, 0, "~", "reboot", "6.2.0", 1690030000, 0, 0, b"", "~~"),
    (7, 1, "pts/2", "dave", "", 1690030600),
    (7, misc_DEAD + 5, "pts/3", "erin", "2001:db8::1", 1690031200, 0, 0,
     b"\x20\x01\x0d\xb8" + b"\0" * 11 + b"\x01"),
    (7, misc_DEAD + 6, "pts/4", "frank", "", 1690031800),
    (8, misc_DEAD + 6, "pts/4", "", "", 1690032400),
    (7, misc_DEAD + 7, "ttyS0123456789", "verylongusername",
     "averyveryverylonghostname.example.com", 1690033000),
)
misc_WTMP = b"".join(misc_utmp_record(*row) for row in misc_WTMP_ROWS)


# Checksum manifests, computed from the fixture bytes they name.
misc_SUMS = {"md5sum": ("md5", b"MD5"), "sha1sum": ("sha1", b"SHA1"),
             "sha224sum": ("sha224", b"SHA224"), "sha256sum": ("sha256", b"SHA256"),
             "sha384sum": ("sha384", b"SHA384"), "sha512sum": ("sha512", b"SHA512"),
             "b2sum": ("blake2b", b"BLAKE2b")}


def misc_hex(algorithm, data):
    return hashlib.new(algorithm, data).hexdigest().encode()


def misc_manifests(algorithm, tag, contents):
    def digest(name):
        return misc_hex(algorithm, contents[name])
    good = b"".join((
        digest("a.txt") + b"  a.txt\n",
        digest("b.txt") + b" *b.txt\n",
        digest("empty") + b"  empty\n",
        digest("two words") + b"  two words\n",
        digest("binary") + b" *binary\n",
        digest("edge_65536") + b"  edge_65536\n",
        digest("dir/inside") + b"  dir/inside\n",
        b"\\" + digest("a\\b") + b"  a\\\\b\n",
        b"\\" + digest("ab\nc") + b"  ab\\nc\n",
    ))
    bad = b"".join((
        digest("a.txt") + b"  a.txt\n",
        b"not a checksum\n",
        digest("a.txt") + b"  c.txt\n",
        digest("nonl") + b"  missing\n",
        digest("a.txt") + b" a.txt\n",
        digest("a.txt").upper() + b"  a.txt\n",
        digest("a.txt")[:-2] + b"  a.txt\n",
        digest("a.txt") + b"  a.txt\r\n",
        b"\n",
        tag + b" (b.txt) = " + digest("b.txt") + b"\n",
        digest("dir/inside") + b"  dir\n",
        digest("unreadable") + b" *unreadable\n",
        b"\\" + digest("a\\b") + b"  a\\qb\n",
    ))
    tagged = b"".join(tag + b" (" + name.encode() + b") = " + digest(name) + b"\n"
                      for name in ("a.txt", "b.txt", "empty", "two words"))
    return good, bad, tagged


misc_KERN_LOG = (
    b"<5>[    0.000000] Linux version 6.1.0-misc (dawn@box) #1 SMP\n"
    b"<6>[    0.000000] Command line: root=/dev/sda1 quiet\n"
    b"<4>[    0.001234] warning: something odd\n"
    b"<3>[   12.500000] error: device failed\n"
    b"<6>[   12.500001] multi-line message first\n"
    b"<6>[   12.500001] continued second line\n"
    b"<14>[  100.000000] user space message via /dev/kmsg\n"
    b"<30>[  100.500000] daemon notice\n"
    b"<7>[ 3600.250000] debug \x1b[31mescape\x1b[0m and tab\tbyte \xff high\n"
    b"no priority no timestamp line\n"
    b"<2>[90000.000000] crit after a day\n"
    b"<0>[90001.000000] emerg\n"
    b"<1>[90002.000000] alert\n"
    b"<191>[90003.000000] local7.debug\n"
    b"<6>[90004.000000] last line without newline")
misc_KMSG_LOG = (
    b"5,1,0,-;Linux version 6.1.0-misc\n"
    b"6,2,0,-;Command line: root=/dev/sda1\n SUBSYSTEM=kernel\n DEVICE=+platform:x\n"
    b"4,3,1234,-;warning: something odd\n"
    b"3,4,12500000,-;error: device failed\n"
    b"6,5,12500001,c;continuation fragment\n"
    b"6,6,12500001,+;another fragment\n"
    b"14,7,100000000,-;user space message\n"
    b"30,8,100500000,-;daemon notice\n"
    b"7,9,3600250000,-;debug \\x1b[31mescape\\x1b[0m tab\\x09 high\\xff\n"
    b"garbage line without structure\n"
    b"2,10,90000000000,-;crit after a day\n"
    b"191,11,90003000000,-;local7.debug\n"
    b"6,12,90004000000,-;last without newline")

misc_TYPESCRIPT = (
    b"Script started on 2023-01-01 00:00:00+00:00 [COMMAND=\"echo hi\" <not executed on terminal>]\n"
    b"hello\r\nworld\r\n"
    b"\nScript done on 2023-01-01 00:00:01+00:00 [COMMAND_EXIT_CODE=\"0\"]\n")
misc_TIMING = b"0.010 5\n0.010 2\n0.020 7\n"
misc_TIMING_ADVANCED = (
    b"H 0.000000 START_TIME=1672531200\n"
    b"H 0.000000 TERM=dumb\n"
    b"O 0.010000 5\n"
    b"I 0.005000 3\n"
    b"O 0.010000 2\n"
    b"S 0.001000 SIGWINCH ROWS=24 COLS=80\n"
    b"O 0.020000 7\n"
    b"H 0.000000 DURATION=0.046000\n")
misc_TYPESCRIPT_IN = b"Script started on 2023-01-01 00:00:00+00:00 [<not executed on terminal>]\nhi\r"


def misc_tsort_random(seed, count):
    """Shuffled names, forward edges (a DAG), duplicates, self pairs and,
    for odd seeds, a ring; separators alternate space, tab and newline."""
    rng = random.Random(seed)
    names = ["n%02d" % i for i in range(count)]
    rng.shuffle(names)
    pairs = [(name, name) for name in names if rng.random() < 0.35]
    for _ in range(rng.randint(0, count * 3)):
        one = rng.randrange(count)
        two = rng.randrange(one, count)
        pairs.append((names[one], names[two]))
        if rng.random() < 0.25:
            pairs.append((names[one], names[two]))
    if seed & 1 and count >= 2:
        ring = rng.sample(names, rng.randint(2, min(7, count)))
        pairs.extend(zip(ring, ring[1:] + ring[:1]))
    rng.shuffle(pairs)
    if not pairs:
        pairs.append((names[0], names[0]))
    tokens = [word for pair in pairs for word in pair]
    separators = (" ", "\t", "\n", " \n")
    return "".join(token + separators[i % 4] for i, token in enumerate(tokens)).encode()


def misc_tsort_chain(count):
    pairs = []
    for i in range(count):
        pairs.append("v%05d v%05d" % (i, i + 1))
        if i % 31 == 0:
            pairs.append("side%05d side%05d" % (i, i))
    return ("\n".join(pairs) + "\n").encode()


def misc_factor_random(seed, count):
    rng = random.Random(seed)
    values = [1000000016000000063, 18446743979220271189, 18446744030759878681,
              2305843009213693951, 18446744073709551557]
    for _ in range(count):
        kind = rng.randrange(4)
        if kind == 0:
            values.append(rng.getrandbits(64))
        elif kind == 1:
            values.append(rng.randrange(2, 1 << 32) * rng.randrange(2, 1 << 32))
        elif kind == 2:
            values.append(rng.randrange(2, 1 << 21) * rng.randrange(2, 1 << 21)
                          * rng.randrange(2, 1 << 21))
        else:
            values.append((rng.getrandbits(32) | 1) ** 2)
    return (" ".join(str(value) for value in values) + "\n").encode()


def misc_numfmt_random(seed, count):
    """Binary-exact fractions only, so this compares numfmt rather than the
    reference's long double representation errors."""
    rng = random.Random(seed)
    values = []
    for _ in range(count):
        whole = rng.randrange(1000000000)
        sign = "-" if rng.randrange(2) else ""
        tail = rng.choice(("", ".0", ".00", ".5", ".50", ".25", ".75", ".125", ".625"))
        values.append("%s%d%s" % (sign, whole, tail))
    return ("\n".join(values) + "\n").encode()


def misc_hexdump_formats():
    """Format strings for hexdump -e, drawn from its grammar: an optional
    iteration count and byte count, a conversion, literal text."""
    rng = random.Random(0x6865)
    conversions = ("%02x", "%d", "%o", "%c", "%_p", "%_a[dox]", "%_A[dox]", "%u", "%3_c", "%08.8_ax")
    made = ['1/1 "%02x"', '"%08.8_ax  " 16/1 "%02x " "\\n"', '8/2 "%04x " "\\n"',
            '"%d\\n"', 'bad', '']
    for _ in range(6):
        count = rng.choice(("", "1/", "4/", "16/", "2/1 ", "8/2 "))
        piece = rng.choice(conversions)
        made.append('%s"%s%s"' % (count, piece, rng.choice(("", " ", "\\n"))))
    return tuple(made)


INPUTS.update({
    "misc_utmp": misc_UTMP,
    "misc_utmp_text": misc_UTMP_TEXT,
    "misc_wtmp": misc_WTMP,
    "misc_tsort_dag": b"c d\na b\na c\n",
    "misc_tsort_dup": b"z z\na b\na b\nb c\na c\nd d\n",
    "misc_tsort_cycle": b"a b\nb c\nc a\na d\n",
    "misc_tsort_cycles": b"a b\nb a\nc d\nd c\ne f\n",
    "misc_tsort_odd": b"one two three\n",
    "misc_tsort_r1": misc_tsort_random(1, 9),
    "misc_tsort_r2": misc_tsort_random(2, 28),
    "misc_tsort_r3": misc_tsort_random(3, 17),
    "misc_tsort_r4": misc_tsort_random(4, 3),
    "misc_tsort_long_token": b"x" * 70000 + b" tail\n",
    "misc_tsort_chain": misc_tsort_chain(12000),
    "misc_numfmt_table": b"heading\nsecond\n1000 foo  2000\n1\t2\t3000\n",
    "misc_numfmt_units": b"1K\n1Ki\n2.5M\n2.5Mi\n1000\n-1001000\n1.5\nbad\n 4096\n1048576 tail\nx 1048576\n1k\n1KB\n1MiB\n",
    "misc_numfmt_fields": b"1024: 2048 \n1024:\t2048\t\n1024:2048 \t\n1024:bad \n1024: \t\n1024:2048\r\n1024:2048\v\na:1000:c\n::bad\n2048K \t:x\n2048 K \t:x\n2048_KB:x\n2048 _KB:x\n",
    "misc_numfmt_widths": b"1048576\n 1048576\n\t1048576\n1048576 tail\nx 1048576\n    1024\n  1048576\n1024\n\n \t\n  bad\nx \tbad\n",
    "misc_numfmt_random": misc_numfmt_random(0x6e66, 40),
    "misc_factor": b"12 13\n15\t17 18446744073709551615\n0\n1\n+7\nbad\n-2\n0x10\n 97 \n\n" +
                   (" ".join(str(n) for n in range(0, 300)) + "\n").encode(),
    "misc_factor_random": misc_factor_random(0x6661, 60),
    "misc_logger_prefixed": b"<13>hello prefixed\n<3>an error line\nno prefix here\n<999>bad prio\n<abc>\n\n<0>emerg\n",
    "misc_journal": b"MESSAGE=mw-differential journald entry\nPRIORITY=6\nSYSLOG_IDENTIFIER=mwtest\n",
    "misc_dd_case": b"AaZz09-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 3,
    "misc_dd_1000": b"q" * 1000,
    "misc_sums_malformed": b"not a checksum\n\n garbage line\n",
})

misc_FIXTURE = dict(FIXTURES["basic"])
misc_FIXTURE.update({
    "edge_65535": INPUTS["edge_65535"],
    "edge_65536": INPUTS["edge_65536"],
    "edge_65537": INPUTS["edge_65537"],
    "long": INPUTS["long"],
    "many": INPUTS["many_lines"],
    "a\\b": b"slash\n",
    "ab\nc": b"newline\n",
    "utmp": misc_UTMP,
    "wtmp": misc_WTMP,
    "utmp.txt": misc_UTMP_TEXT,
    "kern.log": misc_KERN_LOG,
    "kmsg.log": misc_KMSG_LOG,
    "typescript": misc_TYPESCRIPT,
    "typescript.in": misc_TYPESCRIPT_IN,
    "timing": misc_TIMING,
    "timing.adv": misc_TIMING_ADVANCED,
    "tsort.dag": INPUTS["misc_tsort_dag"],
    "tsort.cycle": INPUTS["misc_tsort_cycle"],
    "tsort.odd": INPUTS["misc_tsort_odd"],
    "tsort.chain": INPUTS["misc_tsort_chain"],
    "sums.malformed": INPUTS["misc_sums_malformed"],
    "case.txt": b"ALPHA\nBeta\nGamma\n",
    "spaced.txt": b"alpha \n  beta\ngam ma\n",
    "crlf.txt": b"alpha\r\nbeta\r\ngamma\r\n",
    "ten": b"abcdefghij",
    "zeros": b"\0" * 4096,
    "blob": bytes(random.Random(0x6464).randrange(256) for _ in range(17408)),
})
for misc_name, (misc_algorithm, misc_tag) in misc_SUMS.items():
    misc_good, misc_bad, misc_tagged = misc_manifests(
        misc_algorithm, misc_tag,
        {name: misc_basic_bytes(name) for name in ("a.txt", "b.txt", "empty", "two words",
                                                   "binary", "dir/inside", "nonl", "unreadable")}
        | {"edge_65536": INPUTS["edge_65536"], "a\\b": b"slash\n", "ab\nc": b"newline\n"})
    misc_FIXTURE["sums." + misc_name] = misc_good
    misc_FIXTURE["sums." + misc_name + ".bad"] = misc_bad
    misc_FIXTURE["sums." + misc_name + ".tagged"] = misc_tagged
    INPUTS["misc_sums_" + misc_name] = misc_good
misc_FIXTURE["sums.crc"] = b"".join(
    b"%d %d %s\n" % (0, len(misc_basic_bytes(name)), name.encode()) for name in ("a.txt", "empty"))
FIXTURES["misc"] = misc_FIXTURE


def misc_pair_fixture():
    """diff's subjects: the hand shapes every diff lane once asserted, then
    random pairs over small alphabets (repeats everywhere are what make the
    choice of changed line observable), pairs past GNU's 64-line heuristic
    ceiling, lightly edited copies, word pairs for the ignore flags, files
    without a final newline, and directory trees."""
    rng = random.Random(20260828)
    files = dict(FIXTURES["basic"])

    def lines(alphabet, count):
        return "".join(rng.choice(alphabet) + "\n" for _ in range(count)).encode()

    files.update({
        "a2": b"alpha\nbeta\ngamma\n",
        "b": b"alpha\nbeta\nBETA\ngamma\ndelta\n",
        "case": b"ALPHA\nBeta\nGamma\n",
        "spaced": b"alpha \n  beta\ngam ma\n",
        "tabbed": b"alpha\t\nbeta\ngam\t\tma\n",
        "blanks": b"alpha\n\n\nbeta\n",
        "tight": b"alpha\nbeta\n",
        "withnl": b"one\ntwo\nthree\n",
        "nonl1": b"one\ntwo\nthree",
        "nonl2": b"one\ntwo\nthre",
        "longnonl": b"abcdefghijklmnopqrstuvAb",
        "longwithnl": b"abcdefghijklmnopqrstuvAb\n",
        "collision1": b"abcdefghijklmnopqrstuvAb\n",
        "collision2": b"abcdefghijklmnopqrstuvBA\n",
        "bin1": b"a\0b\0c\n",
        "bin2": b"a\0b\0d\n",
        "bin1copy": b"a\0b\0c\n",
        "bin3": b"a\0b\0c\0\n",
        "binnonl": b"a\0b\0c",
        "trailing1": b"alpha  \nbeta\t\n",
        "trailing2": b"alpha\nbeta\n",
        "crlf": b"alpha\r\nbeta\r\n",
        "tabexpand1": b"a\tb\nchange\n",
        "tabexpand2": b"a       b\nCHANGE\n",
        "nulx": b"prefix\0x\n",
        "nulX": b"prefix\0X\n",
        "nul_x_": b"prefix\0 x \n",
        "nultx": b"prefix\0\tx",
        "nuly": b"prefix\0y\n",
        "long1": b"".join(b"%d\n" % n for n in range(1, 41)),
        "long2": b"".join((b"SEVEN\n" if n == 7 else b"TWENTYTHREE\n" if n == 23 else b"%d\n" % n)
                          for n in range(1, 41)),
        "long3": b"".join((b"" if n in (5, 6) else b"inserted\n%d\n" % n if n == 30 else b"%d\n" % n)
                          for n in range(1, 41)),
        "tail1": b"left\n" + b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 4096,
        "tail2": b"right\n" + b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 4096,
        "d1/same": b"one\ntwo\n", "d2/same": b"one\ntwo\n",
        "d1/differs": b"one\ntwo\n", "d2/differs": b"one\nTWO\n",
        "d1/onlyleft": b"only\n", "d2/onlyright": b"only\n",
        "d1/sub/inner": b"deep\n", "d2/sub/inner": b"DEEP\n",
        "d1/only.d/x": b"x\n", "d2/deep/er/file": b"f\n",
        "d2/bin": bytes(range(256)),
        "d1/bin": bytes(range(256)),
        "d1/two words": b"spaced\n", "d2/two words": b"spaced too\n",
        "d1/emptydir": ("dir",),
        "d1/link": ("link", "same"), "d2/link": ("link", "differs"),
        "d3/same": b"one\ntwo\n",
    })
    for i in range(24):
        alphabet = ("ab", "abcde", "abcdefghij")[i % 3]
        files["p%02da" % i] = lines(alphabet, rng.randint(0, 24))
        files["p%02db" % i] = lines(alphabet, rng.randint(0, 24))
    for i in range(6):
        alphabet = ("abcde", "abcdefghij")[i % 2]
        files["q%da" % i] = lines(alphabet, rng.randint(60, 400))
        files["q%db" % i] = lines(alphabet, rng.randint(0, 400))
    for i in range(6):
        base = ["line %d of it" % n for n in range(rng.randint(20, 300))]
        other = list(base)
        for _ in range(rng.randint(0, 14)):
            if not other:
                break
            where = rng.randrange(len(other))
            what = rng.randrange(4)
            if what == 0:
                other[where] = "changed %d" % rng.randrange(40)
            elif what == 1:
                del other[where]
            elif what == 2:
                other.insert(where, "inserted %d" % rng.randrange(40))
            else:
                other.insert(where, other[where])
        files["e%da" % i] = "".join(line + "\n" for line in base).encode()
        files["e%db" % i] = "".join(line + "\n" for line in other).encode()
    words = ["alpha", "ALPHA", "Alpha", "beta", "BETA", " beta", "beta ",
             "gam ma", "gam  ma", "gamma", "", "  ", "delta\t", "a\tb", "a       b"]
    for i in range(8):
        files["w%da" % i] = "".join(rng.choice(words) + "\n" for _ in range(rng.randint(0, 14))).encode()
        files["w%db" % i] = "".join(rng.choice(words) + "\n" for _ in range(rng.randint(0, 14))).encode()
    for i in range(4):
        left = lines("abc", rng.randint(1, 12))
        right = lines("abc", rng.randint(1, 12))
        files["n%da" % i] = left[:-1] if i & 1 else left
        files["n%db" % i] = right[:-1] if i & 2 else right

    def tree(root, seed):
        own = random.Random(seed)
        files[root + "/.keep"] = b""
        for _ in range(own.randint(0, 8)):
            files["%s/f%d" % (root, own.randint(0, 10))] = "".join(
                own.choice("abcde") + "\n" for _ in range(own.randint(0, 12))).encode()
        for _ in range(own.randint(0, 3)):
            under = "%s/d%d" % (root, own.randint(0, 4))
            for _ in range(own.randint(0, 4)):
                files["%s/g%d" % (under, own.randint(0, 6))] = "".join(
                    own.choice("xyz") + "\n" for _ in range(own.randint(0, 8))).encode()
            files[under + "/deeper/h"] = "".join(
                own.choice("pq") + "\n" for _ in range(own.randint(0, 5))).encode()
            if own.random() < 0.3:
                files[under + "/empty"] = ("dir",)

    for i in range(4):
        tree("t%da" % i, i * 2)
        tree("t%db" % i, i * 2 + (0, 1, 7, 1)[i])
    return files


FIXTURES["misc_pair"] = misc_pair_fixture()


# ----------------------------------------------------------------------------
#       Normalisers: what legitimately cannot agree between two runs.
# ----------------------------------------------------------------------------

def misc_dd_normalize(channel, data):
    # The duration and the rate are what this machine did in that second.
    if channel == "stderr":
        return re.sub(rb" copied, .*", b" copied, <RATE>", data)
    return data


def misc_diff_normalize(channel, data):
    # A -u header carries the fixture's mtime, recreated for each program.
    if channel == "stdout":
        return re.sub(rb"(?m)^([-+]{3} .*\t)\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{9} [-+]\d{4}$",
                      lambda m: m.group(1) + b"####-##-## ##:##:##.######### +####", data)
    return data


def misc_ps_normalize(channel, data):
    """A live listing (many rows) keeps only its heading and the fact that
    rows followed; a selected process keeps its content with the columns
    that tick (times, elapsed seconds, sizes) reduced to their width."""
    if channel != "stdout":
        return data
    lines = data.split(b"\n")
    if len(lines) > 5:
        heading = lines[0] if not re.search(rb"[0-9]", lines[0]) else b""
        return heading + b"\n<ROWS>\n"
    data = re.sub(rb"\d+-\d\d:\d\d:\d\d", b"##-##:##:##", data)
    data = re.sub(rb"\d\d:\d\d:\d\d", b"##:##:##", data)
    data = re.sub(rb"\d\d:\d\d", b"##:##", data)
    return re.sub(rb"\d{4,}", lambda m: b"#" * len(m.group()), data)


def misc_ps_multicall_normalize(channel, data):
    if channel == "stdout":
        return re.sub(rb"\s*\d+", b" #", data)
    return data


def misc_hex_shape(match):
    return b"H" * len(match.group())


def misc_uuid_normalize(channel, data):
    # Random and time UUIDs (versions 1, 4, 6, 7) keep their version and
    # variant nibbles; name UUIDs (3, 5) are deterministic and stay.
    return re.sub(rb"[0-9a-f]{8}-[0-9a-f]{4}-([1467])[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",
                  lambda m: b"xxxxxxxx-xxxx-" + m.group(1) + b"xxx-vxxx-xxxxxxxxxxxx",
                  data)


def misc_mcookie_normalize(channel, data):
    if channel == "stdout":
        return re.sub(rb"[0-9a-f]{32}", misc_hex_shape, data)
    return data


def misc_logger_normalize(channel, data):
    data = re.sub(rb"[A-Z][a-z]{2} [ \d]\d \d\d:\d\d:\d\d", b"MON DD HH:MM:SS", data)
    data = re.sub(rb"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?[-+]\d\d:\d\d", b"<ISO>", data)
    data = re.sub(rb'syncAccuracy="\d+"', b'syncAccuracy="#"', data)
    return re.sub(rb"(?<=[ \[])\d{4,}(?=[ \]])", b"#", data)


def misc_coresched_normalize(channel, data):
    data = re.sub(rb"PID \d+", b"PID #", data)
    return re.sub(rb"0x[0-9a-f]+", b"0x#", data)


def misc_script_normalize(channel, data):
    # -t writes timings to stderr: a float and a byte count per write.
    if channel == "stderr":
        return re.sub(rb"(?m)^\d+\.\d+ (\d+)$", rb"#.# \1", data)
    return data


def misc_dmesg_normalize(channel, data):
    # Local-time renderings depend on the boot epoch both read from the
    # kernel; the sub-second remainder can round differently between two
    # readings taken apart, so only the seconds are kept.
    if channel == "stdout":
        return re.sub(rb"(\d\d:\d\d:\d\d)[.,]\d{6}", rb"\1.######", data)
    return data


# ----------------------------------------------------------------------------
#       Pruning what cannot be compared safely.
# ----------------------------------------------------------------------------

def misc_wall_valid(argv):
    # Only a bad timeout or an unknown group makes the reference stop before
    # it writes to every terminal (it is setgid tty); --nobanner is merely a
    # warning for non-root and a bare message would broadcast.
    return any(word.startswith(("-t", "-g", "--timeout", "--group")) for word in argv)


misc_SCRIPT_VALUED = ("-c", "-E", "-m", "-O", "-I", "-B", "-T", "-o")


def misc_script_valid(argv):
    # The output log must be /dev/null: a typescript in the directory would
    # carry the wall clock in its header and fail the effects comparison.
    index = 0
    while index < len(argv):
        word = argv[index]
        if word in ("-O", "-B") or word.startswith(("--log-out=", "--log-io=")):
            return True
        if word == "/dev/null":
            return True
        if word == "--":
            return False
        index += 2 if word in misc_SCRIPT_VALUED else 1
    return False


def misc_diff_valid(argv):
    # -L takes the next word, so an option written after it is its label
    # rather than an option; those argvs say nothing about either.
    for index, word in enumerate(argv[:-1]):
        if word == "-L" and argv[index + 1].startswith("-"):
            return False
    return True


def misc_dd_valid(argv):
    # The runner's standard input and output are unnamed temporary files, and
    # dd re-sets the open flags of a side whenever a byte-suffixed quantity or
    # an iflag/oflag names it; the kernel refuses that on an O_TMPFILE with
    # ENOTDIR, which is the harness rather than dd. Those cases name a file.
    named_input = any(word.startswith("if=") for word in argv)
    named_output = any(word.startswith("of=") for word in argv)
    for word in argv:
        if word.startswith("iflag=") and not named_input:
            return False
        if word.startswith("oflag=") and not named_output:
            return False
        if word.startswith(("count=", "skip=", "iseek=")) and word.endswith("B") and not named_input:
            return False
        if word.startswith(("seek=", "oseek=")) and word.endswith("B") and not named_output:
            return False
    return True


def misc_last_normalize(channel, data):
    # An empty or unreadable database "begins" now, in the reference's clock.
    if channel != "stdout":
        return data

    def recent(match):
        stamp = match.group(2).decode()
        for layout in ("%a %b %d %H:%M:%S %Y", "%Y-%m-%dT%H:%M:%S+00:00"):
            try:
                when = time.mktime(time.strptime(stamp.replace("  ", " "), layout))
            except ValueError:
                continue
            if abs(time.time() - when) < 7200:
                return match.group(1) + b"<NOW>"
        return match.group(0)
    return re.sub(rb"( begins )(.+)$", recent, data)


# ----------------------------------------------------------------------------
#       Shell-wrapped ps: header overrides are invocation-local, and two ps
#       runs through one multicall shell once spun for three days.
# ----------------------------------------------------------------------------

def misc_ps_multicall_script(argv, stdin_name):
    words = " ".join(shlex.quote(word) for word in argv)
    return ("ps -p $$ %s; echo \"[$?]\"; ps -p $$ -o pid; echo \"[$?]\"; "
            "ps -p $$ -o pid,comm; echo \"[$?]\"\n" % words)


# ----------------------------------------------------------------------------
#       The grammars.
# ----------------------------------------------------------------------------

misc_DUMP_OPERANDS = ((), ("a.txt",), ("binary",), ("a.txt", "binary"), ("-",), ("missing",),
                      ("dir",), ("empty", "dir"), ("edge_65537",), ("nonl",), ("two words",),
                      ("unreadable",), ("binary", "-", "a.txt"), ("long",), ("blob",))
misc_DUMP_STDIN = ("text", "empty", "nul", "high", "controls", "edge_65536", "long", "blanks")


def misc_checksum(name, variable_length=False):
    options = [Option("-b"), Option("--binary"), Option("-t"), Option("-c"), Option("--check"), Option("--tag"),
               Option("-z"), Option("--ignore-missing"), Option("--quiet"), Option("--status"),
               Option("--strict"), Option("-w")]
    if variable_length:
        options.append(Option("-l", ("8", "128", "256", "512", "0", "7", "513", "bad"), None))
    return Utility(
        name, options=tuple(options),
        extra=(("--text", "a.txt"), ("--zero", "a.txt", "b.txt"), ("--warn", "-c", "sums." + name + ".bad"),
               ("--length=256", "a.txt") if variable_length else ("--check", "sums." + name)),
        operands=((), ("a.txt",), ("a.txt", "b.txt", "empty"), ("-",), ("missing",), ("dir",),
                  ("binary",), ("edge_65535",), ("edge_65536",), ("edge_65537",), ("long",),
                  ("unreadable",), ("two words",), ("a\\b", "ab\nc"), ("a.txt", "missing", "b.txt"),
                  ("sums." + name,), ("sums." + name + ".bad",), ("sums.malformed",),
                  ("sums." + name, "sums.malformed"), ("sums.malformed", "sums." + name),
                  ("sums." + name, "empty"), ("sums." + name, "missing"), ("-", "-"),
                  ("sums." + name + ".tagged",), ("sums.md5sum",), ("sums.sha256sum.bad",)),
        stdin=("text", "empty", "misc_sums_" + name, "misc_sums_malformed", "edge_65535",
               "edge_65536", "edge_65537", "long", "nul", "high", "nonl"),
        fixture="misc", stderr="exact")


UTILITIES = (
    # -- coreutils: copying, comparing, dumping, arithmetic ------------------
    Utility("hostid", operands=((), ("x",), ("--",), ("--", "x")), stdin=("empty",),
            fixture="misc", stderr="exact"),

    Utility("dd",
            options=(Option("if=", ("a.txt", "binary", "edge_65537", "empty", "nonl", "missing", "dir",
                                    "unreadable", "two words", "link", "dangling", "ten", "zeros", "blob"), True),
                     Option("of=", ("out", "a.txt", "dir", "/dev/null", "/dev/full", "dangling",
                                    "unreadable", "two words"), True),
                     Option("bs=", ("1", "4", "7", "512", "1K", "1KB", "1KiB", "1M", "2x3", "1b", "1w",
                                    "1c", "0", "bad", "18446744073709551616", "1p", "0x10", "2*3",
                                    "18446744073709551615x2", "1024x1024x1024x1024x1024x1024x1024"), True),
                     Option("ibs=", ("1", "3", "7", "512", "1000", "8192", "997"), True),
                     Option("obs=", ("1", "3", "7", "64", "512", "1000", "311"), True),
                     Option("count=", ("0", "1", "3", "99", "3B", "1KiB", "bad", "9223372036854775808"), True),
                     Option("skip=", ("0", "1", "3", "9", "3B", "99", "9223372036854775808", "9223372036854775808B"), True),
                     Option("seek=", ("0", "1", "2", "2B", "5B", "bad", "9223372036854775808B"), True),
                     Option("iseek=", ("1B", "2"), True),
                     Option("oseek=", ("2B", "1"), True),
                     Option("cbs=", ("3", "bad", "not-a-number"), True),
                     Option("conv=", ("sync", "noerror", "fsync", "fdatasync", "lcase", "ucase", "swab",
                                      "notrunc", "excl", "nocreat", "sync,swab", "lcase,ucase",
                                      "sparse", "", "bad", "notrunc,sync,noerror"), True),
                     Option("iflag=", ("fullblock", "count_bytes", "skip_bytes", "count_bytes,skip_bytes",
                                       "directory", "nofollow", "direct", "sync", "nonblock", "noatime",
                                       "nocache", "", "bad", "not-a-flag"), True),
                     Option("oflag=", ("append", "seek_bytes", "append,seek_bytes", "direct", "sync",
                                       "dsync", "nocache", "", "bad"), True),
                     Option("status=", ("none", "noxfer", "progress", "bad"), True)),
            operands=((),),
            stdin=("text", "empty", "nul", "high", "mixed_case", "misc_dd_case", "misc_dd_1000",
                   "edge_65537", "long", "many_lines", "nonl"),
            fixture="misc", stderr="exact", normalize=misc_dd_normalize, valid=misc_dd_valid, max_flags=5,
            extra=(("conv=block", "cbs=4", "if=ten", "status=noxfer"),
                   ("conv=unblock", "cbs=4", "if=ten", "status=noxfer"),
                   ("conv=ascii", "if=ten", "status=noxfer"), ("conv=ebcdic", "if=ten", "status=noxfer"),
                   ("conv=ibm", "if=ten", "status=noxfer"),
                   ("bs=4", "status=noxfer"), ("bs=16", "status=noxfer"), ("ibs=3", "obs=7", "status=noxfer"),
                   ("bs=4", "ibs=2", "status=noxfer"), ("bs=4", "obs=2", "status=noxfer"),
                   ("ibs=2", "obs=3", "bs=4", "status=noxfer"), ("bs=16", "conv=sync", "status=noxfer"),
                   ("bs=4", "conv=sync", "status=noxfer"), ("bs=4", "conv=sync,swab", "status=noxfer"),
                   ("bs=3", "conv=swab", "status=noxfer"), ("bs=1", "count=1"), ("bs=1000", "count=1"),
                   ("bs=1024", "count=1"), ("bs=4", "count=3B", "status=noxfer"),
                   ("bs=4", "count=3", "iflag=count_bytes", "status=noxfer"),
                   ("iflag=count_bytes", "bs=4", "count=7", "status=noxfer"),
                   ("bs=4", "skip=3", "iflag=skip_bytes", "status=noxfer"),
                   ("if=ten", "bs=4", "count=3B", "status=noxfer"), ("if=blob", "bs=700", "count=1KiB", "status=noxfer"),
                   ("if=ten", "bs=4", "skip=3B", "status=noxfer"), ("if=ten", "bs=4", "iseek=1B", "status=noxfer"),
                   ("if=ten", "of=out", "bs=4", "seek=2", "conv=notrunc", "status=noxfer"),
                   ("if=ten", "of=out", "bs=4", "seek=2", "status=noxfer"),
                   ("if=ten", "of=out", "bs=4", "oseek=2B", "status=none"),
                   ("if=ten", "of=out", "bs=4", "seek=5B", "status=none"),
                   ("if=ten", "of=out", "bs=4", "seek=1", "status=none"),
                   ("if=ten", "of=out", "bs=4", "oflag=seek_bytes", "seek=2", "status=none"),
                   ("if=ten", "of=a.txt", "bs=4", "oflag=append", "conv=notrunc", "status=none"),
                   ("if=ten", "of=/dev/null", "bs=4", "seek=2", "status=noxfer"),
                   ("if=blob", "bs=2x512", "count=2", "status=noxfer"),
                   ("if=blob", "ibs=8192", "obs=1000", "status=noxfer"),
                   ("if=blob", "ibs=997", "obs=311", "status=noxfer"),
                   ("if=misc", "of=/dev/null"), ("if=missing", "of=/dev/null"),
                   ("if=ten", "of=/dev/full", "bs=2", "status=none"),
                   ("if=ten", "of=/dev/full", "ibs=8", "obs=64", "status=none"))),

    Utility("od",
            options=(Option("-A", ("d", "o", "x", "n", "bad", "dd"), None),
                     Option("-t", ("a", "c", "d1", "d2", "d4", "d8", "u1", "u4", "o1", "o2", "x1", "x2", "x8",
                                   "x1z", "cz", "d8z", "x1c", "xSxI", "bad"), None, repeat=True),
                     Option("-j", ("0", "3", "5", "0x3", "1K", "100000", "bad", "-1", "3b"), None),
                     Option("-N", ("0", "1", "5", "16", "17", "33", "65536", "bad", "0x10", "1KiB"), None),
                     Option("-w"), Option("-w", ("1", "2", "3", "4", "8", "16", "32", "64", "0", "bad", "5", "6", "128", "257"), True),
                     Option("--endian", ("big", "little", "bad"), True),
                     Option("-v"), Option("-c"), Option("-b"), Option("-o"), Option("-d"), Option("-x"), Option("-s"),
                     Option("-a"), Option("-i"), Option("-l"), Option("-h"), Option("-f"),
                     Option("-S", ("3", "0", "1", "bad"), None), Option("--traditional")),
            operands=misc_DUMP_OPERANDS + (("a.txt", "+3"), ("binary", "0x10")),
            stdin=misc_DUMP_STDIN, fixture="misc", stderr="exact", max_flags=5,
            extra=(("-t", "dC", "binary"), ("-t", "dS", "binary"), ("-t", "dI", "binary"), ("-t", "dL", "binary"),
                   ("-t", "u2", "binary"), ("-t", "u8", "binary"), ("-t", "o4", "binary"), ("-t", "o8", "binary"),
                   ("-t", "x4", "binary"), ("-t", "az", "a.txt"), ("-t", "u1z", "binary"), ("-t", "o2z", "binary"),
                   ("-t", "f", "binary"), ("-t", "fD", "binary"), ("-t", "fL", "binary"), ("-t", "d", "binary"),
                   ("-t", "u", "binary"), ("-t", "o", "binary"), ("-t", "x", "binary"), ("-t", "", "binary"),
                   ("-t", "x3", "binary"), ("-t", "x16", "binary"), ("-t", "dz", "binary"), ("-t", "aC", "binary"),
                   ("-A", "x", "-t", "x1z", "-v", "binary"), ("-A", "d", "-t", "d2", "binary"),
                   ("-A", "n", "-t", "c", "binary"), ("-A", "x", "-j", "3", "-N", "5", "-t", "x1z", "a.txt"),
                   ("-A", "x", "-t", "x1", "-t", "c", "a.txt"), ("-An", "-t", "dC", "-t", "uS", "-t", "xI", "-t", "oL", "binary"),
                   ("-An", "-t", "xSxI", "binary"), ("-An", "-bc", "binary"), ("-An", "-a", "binary"),
                   ("-A", "n", "-N", "17", "-t", "x1", "-t", "d8", "-v", "binary"),
                   ("-A", "x", "-N", "33", "-t", "x1z", "-v", "binary"), ("-A", "x", "-t", "az", "-v", "a.txt"),
                   ("-A", "x", "-t", "cz", "-v", "a.txt"), ("-An", "-t", "x1z", "-v", "binary"),
                   ("-w8", "-t", "x1z", "binary"), ("--endian=big", "-t", "x2", "-t", "d4", "-An", "binary"),
                   ("-w3", "-t", "x2", "-An", "a.txt"), ("-w6", "-t", "x2", "-t", "d4", "-An", "a.txt"),
                   ("-An", "-c", "-w1", "a.txt"), ("-v", "-w32", "-c", "long"),
                   ("--format=x1z", "--skip-bytes=2", "--read-bytes=7", "--output-duplicates", "--address-radix=x", "a.txt"),
                   ("--width=4", "-c", "a.txt"), ("--width", "-c", "long"), ("--width=5", "-t", "x2", "a.txt"),
                   ("--strings=4", "binary"), ("--strings", "binary"), ("--endian=little", "-t", "x2", "binary"),
                   ("-w128", "-t", "x1", "-v", "long"), ("-w256", "-c", "-v", "long"))),

    Utility("hexdump",
            options=(Option("-b"), Option("-c"), Option("-C"), Option("-d"), Option("-o"), Option("-x"),
                     Option("-X"), Option("-v"),
                     Option("-n", ("0", "1", "5", "16", "17", "33", "65536", "1K", "1KiB", "bad"), None),
                     Option("-s", ("0", "3", "5", "16", "100000", "1KiB", "bad"), None),
                     Option("-e", misc_hexdump_formats(), None),
                     Option("-f", ("a.txt", "missing", "empty"), None),
                     Option("-L", ("never", "always", "auto", "bad"), True), Option("--color")),
            operands=misc_DUMP_OPERANDS, stdin=misc_DUMP_STDIN, fixture="misc", stderr="loose", max_flags=5,
            extra=(("-Cv", "-n", "17", "binary"), ("-C", "-s", "3", "-n", "5", "a.txt"), ("-b", "-c", "a.txt"),
                   ("-Cv", "binary"), ("-C", "long"), ("-X", "a.txt"), ("-X", "-v", "binary"),
                   ("--one-byte-octal", "--one-byte-char", "a.txt"), ("--one-byte-hex", "--canonical", "a.txt"),
                   ("--two-bytes-decimal", "--two-bytes-octal", "--two-bytes-hex", "binary"),
                   ("--no-squeezing", "--length=5", "--skip=3", "a.txt"), ("--format", '1/1 "%02x"', "a.txt"),
                   ("--format-file", "a.txt", "b.txt"), ("--color=never", "-C", "a.txt"), ("--color=bad", "-C", "a.txt"))),

    Utility("diff",
            options=(Option("-u"), Option("-U", ("0", "1", "2", "3", "nope", "-1", "99"), None),
                     Option("--unified", ("0", "2"), True), Option("--unified"),
                     Option("-q"), Option("--brief"), Option("-s"), Option("--report-identical-files"),
                     Option("-i"), Option("--ignore-case"), Option("-w"), Option("-b"), Option("-B"), Option("-E"),
                     Option("-Z"), Option("-r"), Option("--recursive"), Option("-N"), Option("--new-file"),
                     Option("--unidirectional-new-file"), Option("-a"), Option("--text"),
                     Option("--strip-trailing-cr"), Option("-L", ("left", "right", "two words", ""), None, repeat=True),
                     Option("--label", ("L1",), None), Option("--normal"), Option("--speed-large-files"),
                     Option("--no-ignore-file-name-case"), Option("-T"), Option("-c"), Option("-y")),
            operands=(("a.txt", "b.txt"), ("a.txt", "a.txt"), ("a.txt", "a2"), ("a.txt", "b"), ("b", "a.txt"),
                      ("a.txt", "case"), ("a.txt", "spaced"), ("spaced", "tabbed"), ("tight", "blanks"),
                      ("trailing1", "trailing2"), ("crlf", "trailing2"), ("tabexpand1", "tabexpand2"),
                      ("withnl", "nonl1"), ("nonl1", "withnl"), ("nonl1", "nonl2"), ("longwithnl", "longnonl"),
                      ("collision1", "collision2"), ("bin1", "bin2"), ("bin1", "bin1copy"), ("bin1", "bin3"),
                      ("bin1", "binnonl"), ("binnonl", "bin1"), ("long1", "long2"), ("long1", "long3"),
                      ("tail1", "tail2"), ("empty", "a.txt"), ("a.txt", "empty"), ("empty", "empty"),
                      ("a.txt", "-"), ("-", "a.txt"), ("-", "-"), ("a.txt", "missing"), ("missing", "a.txt"),
                      ("missing", "missing"), ("d1", "d2"), ("d2", "d1"), ("d1", "d1"), ("d1", "d3"), ("d1", "d2/same"),
                      ("d1/same", "d2"), ("dir", "d1"), ("a.txt",), (), ("a.txt", "b.txt", "c.txt"),
                      ("link", "a.txt"), ("dangling", "a.txt"), ("unreadable", "a.txt"), ("two words", "a.txt"),
                      ("nulx", "nulX"), ("nulx", "nul_x_"), ("nulx", "nultx"), ("nulx", "nuly"), ("nultx", "nul_x_"))
            + tuple(("p%02da" % i, "p%02db" % i) for i in range(12))
            + tuple(("q%da" % i, "q%db" % i) for i in range(3))
            + tuple(("e%da" % i, "e%db" % i) for i in range(3))
            + tuple(("w%da" % i, "w%db" % i) for i in range(4))
            + tuple(("n%da" % i, "n%db" % i) for i in range(4))
            + tuple(("t%da" % i, "t%db" % i) for i in range(4)),
            stdin=("text", "empty", "nonl", "crlf"), fixture="misc_pair", stderr="exact",
            normalize=misc_diff_normalize, valid=misc_diff_valid, max_flags=5,
            # The other generated pairs, plain and unified, as fixed cases.
            extra=tuple(("p%02da" % i, "p%02db" % i) for i in range(12, 24))
            + tuple(("-u", "p%02da" % i, "p%02db" % i) for i in range(12, 24))
            + tuple(("q%da" % i, "q%db" % i) for i in range(3, 6))
            + tuple(("-u", "q%da" % i, "q%db" % i) for i in range(3, 6))
            + tuple(("e%da" % i, "e%db" % i) for i in range(3, 6))
            + tuple(("-q", "e%da" % i, "e%db" % i) for i in range(3, 6))
            + tuple((flag, "w%da" % i, "w%db" % i) for i in range(4, 8) for flag in ("-i", "-w", "-b", "-B", "-Z", "-u"))
            + (("-u", "-L", "left", "-L", "right", "a.txt", "b"), ("-u", "-L", "one", "-L", "two", "-L", "three", "a.txt", "b"),
                   ("-U0", "a.txt", "b"), ("-U", "1", "a.txt", "b"), ("--unified=2", "a.txt", "b"), ("-qs", "a.txt", "a2"),
                   ("-u", "--normal", "a.txt", "b"), ("-rU", "1", "d1", "d2"), ("-rs", "d1", "d2"), ("-rN", "d1", "d2"),
                   ("-rq", "d1", "d2"), ("-ru", "d1", "d2"), ("-r", "--unidirectional-new-file", "d1", "d2"),
                   ("-r", "--unidirectional-new-file", "d2", "d1"), ("--unidirectional-new-file", "missing", "a.txt"),
                   ("-a", "-iw", "nulx", "nul_x_"), ("-a", "-ib", "nulx", "nulX"), ("-u", "-B", "tight", "blanks"),
                   ("-u", "-b", "w0a", "w0b"), ("-i", "-w", "w1a", "w1b"),
                   tuple(["-i"] * 80 + ["-r", "d1", "d2"]),
                   ("-r", "-N", "-u", "t0a", "t0b"), ("-r", "-q", "t1a", "t1b"), ("-r", "-N", "t2a", "t2b"),
                   ("-q", "tail1", "tail2"), ("-a", "bin1", "bin2"), ("-s", "binnonl", "binnonl"),
                   # Long spellings of the comparison flags.
                   ("--ignore-all-space", "a.txt", "spaced"), ("--ignore-space-change", "spaced", "tabbed"),
                   ("--ignore-blank-lines", "tight", "blanks"), ("--ignore-tab-expansion", "tabexpand1", "tabexpand2"),
                   ("--ignore-trailing-space", "trailing1", "trailing2"), ("--ignore-case", "-u", "a.txt", "case"),
                   # Refused on purpose (test/surface_ledger.json); carried as option rows.
                   ("--initial-tab", "a.txt", "b"), ("-t", "a.txt", "b"), ("--expand-tabs", "a.txt", "b"),
                   ("-C", "2", "a.txt", "b"), ("--context=1", "a.txt", "b"), ("--context", "a.txt", "b"),
                   ("-e", "a.txt", "b"), ("--ed", "a.txt", "b"), ("-n", "a.txt", "b"), ("--rcs", "a.txt", "b"),
                   ("--side-by-side", "a.txt", "b"), ("-W", "80", "-y", "a.txt", "b"), ("--width=60", "-y", "a.txt", "b"),
                   ("--left-column", "-y", "a.txt", "b"), ("--suppress-common-lines", "-y", "a.txt", "b"),
                   ("-p", "a.txt", "b"), ("--show-c-function", "a.txt", "b"), ("-F", "^[a-z]", "a.txt", "b"),
                   ("--show-function-line=^a", "a.txt", "b"), ("-I", "alpha", "a.txt", "b"),
                   ("--ignore-matching-lines=beta", "a.txt", "b"), ("-x", "same", "-r", "d1", "d2"),
                   ("--exclude=*.txt", "-r", "d1", "d2"), ("-X", "a.txt", "-r", "d1", "d2"),
                   ("--exclude-from=b.txt", "-r", "d1", "d2"), ("-S", "same", "-r", "d1", "d2"),
                   ("--starting-file=differs", "-r", "d1", "d2"), ("--from-file=a.txt", "b", "a2"),
                   ("--to-file=a.txt", "b", "a2"), ("-D", "NAME", "a.txt", "b"), ("--ifdef=X", "a.txt", "b"),
                   ("--line-format=%l\\n", "a.txt", "b"), ("--old-line-format=<%l\\n", "a.txt", "b"),
                   ("-d", "a.txt", "b"), ("--minimal", "a.txt", "b"), ("--horizon-lines=2", "a.txt", "b"),
                   ("--color=never", "a.txt", "b"), ("--color=always", "a.txt", "b"), ("--color", "a.txt", "b"),
                   ("--palette=ad=1", "--color=always", "a.txt", "b"), ("--tabsize=4", "a.txt", "b"),
                   ("--suppress-blank-empty", "a.txt", "b"), ("-l", "a.txt", "b"), ("--paginate", "a.txt", "b"),
                   ("--ignore-file-name-case", "-r", "d1", "d2"), ("--no-dereference", "-r", "d1", "d2"))),

    Utility("factor",
            options=(Option("-h"), Option("--exponents")),
            operands=((), ("0", "1"), ("2", "3", "4", "12", "97", "65537", "99991"), ("+000", "0001", "00012"),
                      ("12", "360", "65536"), ("999950000", "18446744073709551615"), ("9223372036854775807",),
                      ("18446744073709551615",), ("18446744073709551616",),
                      ("1000000016000000063", "18446743979220271189", "18446744030759878681"),
                      ("2305843009213693951", "18446744073709551557"), ("bad",), ("-2",), ("--", "12", "bad", "-2", "13"),
                      ("1e3",), ("0x10",), (" 12",), ("12abc",), ("",), ("18446744073709551617",),
                      ("--", "-1"), ("2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2", "2")),
            stdin=("misc_factor", "misc_factor_random", "numbers", "empty", "text", "nonl", "blanks", "long",
                   "many_lines", "nul", "high", "wide_words"),
            fixture="misc", stderr="exact"),

    Utility("numfmt",
            options=(Option("--to", ("si", "iec", "iec-i", "none", "auto", "bad"), True),
                     Option("--from", ("auto", "si", "iec", "iec-i", "none", "bad"), True),
                     Option("--suffix", ("B", " B", "B ", "", "_KB", " "), True),
                     Option("--padding", ("10", "-10", "0", "1", "bad", "9"), True),
                     Option("--round", ("up", "down", "from-zero", "towards-zero", "nearest", "bad"), True),
                     Option("--format", ("%f", "%.1f", "%10f", "%-10f", "%010.2f", "[%8.1f]", "%d", "%f%f", "x",
                                         "%'f", "%.2f", "%0.2f", "X%.2fY", "%06.2f", "%6.2f", "%-6.2f",
                                         "[%08.3f]", "[value=%f]"), True),
                     Option("--field", ("1", "2", "1,3", "2-", "-2", "-", "1-2", "0", "bad", "3"), True),
                     Option("-d", (":", " ", "\t", "ab", ""), None), Option("--delimiter", (":",), True),
                     Option("--header"), Option("--header", ("1", "2", "0", "bad"), True),
                     Option("--invalid", ("abort", "fail", "warn", "ignore", "bad"), True),
                     Option("-z"), Option("--zero-terminated"),
                     Option("--to-unit", ("1", "1024", "1000", "0", "bad", "2"), True),
                     Option("--from-unit", ("1", "3", "1024", "bad"), True),
                     Option("--unit-separator", ("_", " ", "", ".", "-", "K", "i", "0", "_ "), True),
                     Option("--grouping"), Option("--debug")),
            operands=((), ("1000",), ("1000", "1K", "1Ki", "2.5M", "-1001000", "0", "1.5", "bad"),
                      ("--", "-0", "-1", "1.2300", "9223372036854775807"), ("18446744073709551616",),
                      ("999", "1000000", "1001000", "9999000", "999499000", "-1001000"),
                      ("1023", "1024", "1025", "9999", "-1025"), ("1024", "1048576"), ("1K", "1Ki", "2.5M", "2.5Mi"),
                      ("--", "1.50", "-2.00"), ("1_KB", "2_MiB"), ("--", "9990000", "10010000", "-9990000", "-10010000", "999950000"),
                      ("--", ".5", "-.5", "1.5", "-1.5", "999949000", "999950000"), ("1000000",), ("",), (" 12",),
                      ("1e3",), ("0x10",), ("1,000",), ("--", "-"), ("nan",), ("inf",)),
            stdin=("numbers", "misc_numfmt_table", "misc_numfmt_units", "misc_numfmt_fields", "misc_numfmt_widths",
                   "misc_numfmt_random", "fields", "empty", "spaces", "tabs", "nul", "long", "text", "nonl"),
            fixture="misc", stderr="exact", max_flags=5),

    Utility("tsort",
            operands=((), ("tsort.dag",), ("tsort.cycle",), ("tsort.odd",), ("tsort.chain",), ("a.txt",), ("missing",),
                      ("dir",), ("-",), ("empty",), ("tsort.dag", "tsort.dag"), ("binary",), ("--", "tsort.dag"),
                      ("unreadable",), ("-q",)),
            stdin=("misc_tsort_dag", "misc_tsort_dup", "misc_tsort_cycle", "misc_tsort_cycles", "misc_tsort_odd",
                   "misc_tsort_r1", "misc_tsort_r2", "misc_tsort_r3", "misc_tsort_r4", "misc_tsort_long_token",
                   "misc_tsort_chain", "text", "empty", "nonl", "nul", "long", "words", "repeats", "sorted",
                   "wide_words", "blanks"),
            fixture="misc", stderr="exact"),

    # -- the checksums ------------------------------------------------------
    misc_checksum("md5sum"), misc_checksum("sha1sum"), misc_checksum("sha224sum"),
    misc_checksum("sha256sum"), misc_checksum("sha384sum"), misc_checksum("sha512sum"),
    misc_checksum("b2sum", variable_length=True),
    Utility("cksum",
            options=(Option("-a", ("crc", "md5", "sha1", "sha256", "blake2b", "bad", ""), None),
                     Option("--algorithm", ("crc", "sha256", "blake2b"), True),
                     Option("--base64"), Option("--raw"), Option("--tag"), Option("--untagged"), Option("-z"),
                     Option("-c"), Option("--check"), Option("-l", ("256", "0", "8"), None),
                     Option("--ignore-missing"), Option("--quiet"), Option("--status"), Option("--strict"),
                     Option("-w"), Option("--debug")),
            operands=((), ("a.txt",), ("a.txt", "b.txt", "empty"), ("-",), ("missing",), ("dir",), ("binary",),
                      ("edge_65535",), ("edge_65536",), ("edge_65537",), ("long",), ("unreadable",), ("two words",),
                      ("a\\b", "ab\nc"), ("a.txt", "missing", "b.txt"), ("sums.sha256sum.tagged",), ("sums.md5sum.tagged",),
                      ("sums.b2sum.tagged",), ("sums.sha256sum",), ("sums.crc",), ("sums.malformed",), ("a.txt", "--algorithm=crc"),
                      ("a.txt", "--algorithm=sha256")),
            # The algorithms this engine does not carry, and the check mode
            # that would have to read every one of their spellings.
            extra=(("-a", "crc32b", "a.txt"), ("-acrc32b", "a.txt"), ("-a", "sm3", "a.txt"),
                   ("-a", "sha3", "a.txt"), ("-a", "sha2", "a.txt"), ("-a", "bsd", "a.txt"),
                   ("-a", "sysv", "a.txt"), ("-c", "sums.crc"), ("--check", "sums.sha256sum"),
                   ("-a", "sha256", "-c", "sums.sha256sum.tagged")),
            stdin=("text", "empty", "misc_sums_sha256sum", "edge_65535", "edge_65536", "edge_65537", "long", "nul",
                   "high", "nonl", "many_lines"),
            fixture="misc", stderr="exact"),

    # -- process wrappers -----------------------------------------------------
    Utility("timeout",
            options=(Option("-s", ("TERM", "KILL", "HUP", "INT", "9", "15", "0", "bad", "SIGTERM", "64", "sigkill"), None),
                     Option("--signal", ("KILL",), True),
                     Option("-k", ("1", "0", "2", "bad"), None), Option("--kill-after", ("1",), True),
                     Option("-p"), Option("--preserve-status"), Option("-f"), Option("--foreground"),
                     Option("-v"), Option("--verbose")),
            operands=(("1", "./exe", "a"), (".1", "sleep", "2"), ("0", "./exe", "b"), (".1", "sh", "-c", "trap '' TERM; sleep 5"),
                      ("1", "missing"), ("1",), (), ("bad", "./exe"), ("1s", "./exe"), ("1m", "./exe"), ("1h", "./exe"),
                      ("1d", "./exe"), ("1x", "./exe"), ("1e1", "./exe"), ("inf", "./exe"), ("0x1", "./exe"), ("", "./exe"),
                      ("1", "dir"), ("1", "unreadable"), ("1", "sh", "-c", "exit 7"),
                      (".2", "sh", "-c", "printf output; printf error >&2; exit 7"), ("--", "1", "./exe"),
                      ("1", "--", "./exe"), (".1", "sleep"), ("1.5", "./exe", "c"), ("-1", "./exe"), ("1", "cat"),
                      ("1", "exe")),
            stdin=("empty", "text"), fixture="misc", stderr="exact", timeout=12.0),

    Utility("nohup",
            operands=(("./exe", "a"), ("./exe",), ("missing",), (), ("--", "./exe"), ("dir",), ("unreadable",),
                      ("sh", "-c", "kill -HUP $$; printf alive; printf error >&2"), ("sh", "-c", "exit 3"),
                      ("cat",), ("-x", "./exe"), ("--bad",), ("sh", "-c", "trap - HUP; kill -HUP $$; echo survived"),
                      ("exe",)),
            stdin=("text", "empty"), fixture="misc", stderr="exact"),

    Utility("stdbuf",
            options=(Option("-i", ("0", "L", "1", "4K", "bad", "-1"), None), Option("--input", ("0",), True),
                     Option("-o", ("0", "L", "1", "4K", "4KB", "4KiB", "1M", "l", "bad", "0Q", "+1", "004K"), None),
                     Option("--output", ("L",), True),
                     Option("-e", ("0", "L", "4KiB", "bad"), None), Option("--error", ("0",), True)),
            operands=(("./exe", "a"), ("sh", "-c", "echo \"$_STDBUF_I|$_STDBUF_O|$_STDBUF_E|$LD_PRELOAD\""),
                      ("missing",), (), ("dir",), ("true",), ("cat",), ("--", "./exe", "b"),
                      ("env",), ("exe",)),
            stdin=("text", "empty"), fixture="misc", stderr="exact"),

    Utility("chroot",
            options=(Option("--skip-chdir"), Option("--groups", ("0", "bad", "0,1"), True),
                     Option("--userspec", ("0:0", "nobody", "bad:bad", ":0"), True)),
            operands=(("/", "./exe"), ("/", "true"), ("dir", "./exe"), ("dir",), ("missing", "./exe"), (), ("/",),
                      ("a.txt", "./exe"), ("/", "missing"), ("--", "/", "./exe"), ("/", "sh", "-c", "pwd")),
            stdin=("empty",), fixture="misc", stderr="exact"),

    Utility("pipesz",
            options=(Option("-g"), Option("-s", ("4096", "65536", "1M", "0", "bad", "1048576"), None),
                     Option("-f", ("a.txt", "dir", "missing", "/dev/null"), None),
                     Option("-n", ("0", "1", "2", "3", "99", "bad", "-1"), None),
                     Option("-i"), Option("-o"), Option("-e"), Option("-c"), Option("-q"), Option("-v"), Option("--get")),
            operands=((), ("./exe", "a"), ("missing",), ("--", "./exe", "b"), ("sh", "-c", "exit 3")),
            stdin=("text", "empty"), fixture="misc", stderr="loose",
            extra=(("--set=8192", "--stdout", "--verbose", "./exe", "a"), ("--get", "--stdin", "--stderr", "--quiet"),
                   ("--file=b.txt", "--check", "--get"), ("--fd=0", "--get", "--verbose"), ("--set", "4096", "--", "./exe"))),

    Utility("coresched",
            options=(Option("-s", ("1", "99999999", "bad", "0"), None), Option("--source", ("1",), True),
                     Option("-d", ("1", "99999999", "bad"), None), Option("--dest", ("1",), True),
                     Option("-t", ("pid", "tgid", "pgid", "bad"), None), Option("--dest-type", ("pgid",), True),
                     Option("-v"), Option("--verbose")),
            operands=((), ("get",), ("new",), ("new", "--", "./exe", "a"), ("copy",), ("copy", "--", "./exe", "b"),
                      ("bad",), ("get", "extra"), ("--", "./exe"), ("new", "--", "missing"), ("new", "./exe"),
                      ("new", "--", "exe")),
            stdin=("empty",), fixture="misc", stderr="loose", normalize=misc_coresched_normalize),

    Utility("ctrlaltdel",
            operands=((), ("hard",), ("soft",), ("bad",), ("hard", "soft"), ("--", "hard"), ("HARD",)),
            stdin=("empty",), fixture="misc", stderr="loose"),

    Utility("pivot_root",
            operands=((), ("dir",), ("dir", "dir/sub"), ("dir", "missing"), ("/", "dir"), ("a", "b", "c"),
                      ("a.txt", "dir"), ("--", "dir", "dir/sub")),
            stdin=("empty",), fixture="misc", stderr="loose"),

    Utility("script",
            options=(Option("-c", ("./exe a", "exit 3", "printf out; printf err >&2", "cat", "", "echo $0", "exe a"), None),
                     Option("-e"), Option("-q"), Option("-f"), Option("-a"),
                     Option("-E", ("auto", "always", "never", "bad"), None),
                     Option("-m", ("classic", "advanced", "bad"), None),
                     Option("-O", ("/dev/null",), None), Option("-I", ("/dev/null",), None), Option("-B", ("/dev/null",), None),
                     Option("-T", ("/dev/null",), None), Option("-t"), Option("--force"), Option("-o", ("1K", "bad"), None)),
            operands=(("/dev/null",), ("/dev/null", "--", "./exe", "a"), ("--", "./exe", "a"), (), ("/dev/null", "extra"),
                      ("--",), ("/dev/null", "--", "sh", "-c", "exit 5"), ("/dev/null", "--", "false"),
                      ("/dev/null", "--", "exe", "two words")),
            stdin=("empty", "text"), fixture="misc", stderr="loose", valid=misc_script_valid,
            normalize=misc_script_normalize, timeout=8.0, env=(("SHELL", "/bin/sh"),),
            extra=(("--command=./exe b", "--return", "--quiet", "/dev/null"), ("--flush", "--append", "--log-out=/dev/null", "-c", "./exe a"),
                   ("--echo=never", "--logging-format=advanced", "--log-io=/dev/null", "-c", "./exe a"),
                   ("--log-timing=/dev/null", "--log-out=/dev/null", "-qc", "./exe a"), ("--timing=/dev/null", "-q", "-c", "./exe a", "/dev/null"),
                   ("--output-limit=1M", "-q", "-c", "./exe a", "/dev/null"), ("-qec", "exit 3", "/dev/null"))),

    Utility("scriptreplay",
            options=(Option("-t", ("timing", "timing.adv", "missing", "empty", "a.txt"), None),
                     Option("--timing", ("timing",), True), Option("-T", ("timing",), None),
                     Option("-I", ("typescript.in",), None), Option("-O", ("typescript", "missing"), None),
                     Option("-B", ("typescript",), None), Option("-s", ("typescript",), None), Option("--summary"),
                     Option("-d", ("1000", "10", "0", "bad", "0.5"), None), Option("--divisor", ("100",), True),
                     Option("-m", ("0", "0.001", "bad"), None), Option("--maxdelay", ("0",), True),
                     Option("-x", ("out", "in", "signal", "info", "bad"), None),
                     Option("-c", ("auto", "never", "always", "bad"), None), Option("--cr-mode", ("never",), True)),
            operands=(("timing",), ("timing", "typescript"), ("timing", "typescript", "1000"), ("timing", "missing"),
                      ("missing",), (), ("timing", "typescript", "1000", "extra"), ("timing.adv", "typescript"),
                      ("timing.adv", "typescript", "10"), ("timing", "typescript", "bad"), ("timing", "a.txt"),
                      ("empty", "typescript")),
            stdin=("empty",), fixture="misc", stderr="loose", timeout=8.0),

    # -- ps ----------------------------------------------------------------------
    Utility("ps",
            options=(Option("-e"), Option("-A"), Option("-f"), Option("-l"), Option("-j"), Option("-w", repeat=True),
                     Option("-o", ("pid", "pid,ppid", "pid=", "pid=ONE", "pid=,comm=", "pid=,comm", "comm", "args",
                                   "user,pid,stat", "cmd", "command", "ucmd", "time,etime,etimes", "rss,vsz", "tty",
                                   "uid", "c", "stime", "sid,pgid,nlwp", "pcpu", "nice", "pri", "lstart", "start",
                                   "ruser", "nosuchcolumn", "", "pid ppid", "pid,,comm", "pid=A B",
                                   ",".join(["pid"] * 35), "pid,ppid,user,comm,args,stat,time,etime,rss,vsz,tty"),
                            None, repeat=True),
                     Option("--format", ("pid,comm", "pid=", ""), True), Option("--format"),
                     Option("-p", ("1", "1,2", "2,1", "999999999", "0", "bad", "1,1", "1 2", ""), None),
                     Option("--pid", ("1", "2", "1,2"), True),
                     Option("--ppid", ("1", "0", "2", "999999999", "bad"), True),
                     Option("-C", ("systemd", "kthreadd", "nosuchcomm", "systemd,kthreadd", "systemd-journal"), None),
                     Option("--sort", ("pid", "-pid", "+pid", "comm", "pid,-pid", "bad", ""), True),
                     Option("--no-headers"), Option("--headers"), Option("-H"), Option("-h")),
            operands=((), ("aux",), ("ax",), ("wwo", "pid,comm"), ("u",), ("--",), ("1",), ("-",)),
            stdin=("empty",), fixture="misc", stderr="loose", normalize=misc_ps_normalize, max_flags=5,
            extra=(("-p", "1", "-o", "pid=,comm="), ("-p1", "-o", "pid=,comm="), ("--pid=1", "-o", "pid=PIDX"),
                   ("-e", "-p", "1", "-o", "pid="), ("-p", "1,2", "-o", "pid="), ("-p", "1", "-p", "2", "-o", "pid="),
                   ("-eo", "pid,comm", "-p", "1"), ("-fp", "1"), ("-ep1", "-o", "pid=,comm="),
                   ("-wwo", "pid,comm", "-p", "1"), ("-p", "1", "wwo", "pid,comm"),
                   ("--no-headers", "-wo", "pid=,comm=", "-p", "1"), ("-ww", "--sort", "-pid", "-p", "1,2", "-o", "pid="),
                   ("--format",), ("--format=",), ("-p", "1", "--pid=1", "-o", "pid="), ("-p", "1", "-o", "tty="),
                   ("-p", "1,1", "-o", "pid="), ("-p", "1,2", "--sort", "pid", "-o", "pid="),
                   ("-p", "1,2", "--sort=-pid", "-o", "pid="), ("-p", "2,999999999,1,2", "--sort=-pid", "-o", "pid=,ppid=,comm="),
                   ("-p", "1,2", "--sort", "pid,-pid", "-o", "pid="), ("-C", "systemd", "-o", "pid,comm"),
                   ("-Csystemd", "-o", "pid=,comm="), ("-wC", "systemd", "-o", "pid,comm"), ("-C", "systemd,systemd", "-o", "pid="),
                   ("--ppid", "0", "-o", "pid=,ppid=,comm="), ("--ppid", "1", "--ppid", "1", "-o", "pid="),
                   ("-p", "0", "-o", "pid="), ("-o", "nosuchcolumn"), ("-o", ",".join(["pid"] * 40)),
                   ("-p", "1", "-o", "pid,ppid,uid,user,comm,sid,pgid,nlwp"), ("-p", "1", "-f"), ("-ef", "-p", "1"),
                   ("-A", "-p", "1", "-o", "comm,args,ppid,stat,time"), ("-p", "1", "--format=pid,ppid,comm"),
                   ("-p", "1", "-o", "cmd,command,ucmd"), ("-p", "1", "-o", "pid=,comm"), ("-p", "1", "-o", "pid=PROCESS"),
                   ("-p", "1", "--no-headers", "-o", "pid,comm"), ("-p", "1", "--headers", "-o", "pid=,comm="),
                   ("-p", "1", "-o", "etimes="), ("-e", "-o", "time="), ("-e", "-o", "pid,ppid"), ("-e",), ("-ef",),
                   ("-A",), ("-f",), ("-l",), ("-j",), ("-p", "1", "-l"), ("-p", "1", "-j"), ("aux",), ("--sort", "comm", "-p", "1"))),

    Utility("ps_multicall",
            options=(Option("-o", ("pid=ONE", "pid=", "pid=,comm=", "pid,comm=X", "pid=ONE,ppid", "pid"), None),
                     Option("--no-headers"), Option("--headers"), Option("-w")),
            operands=((), ("-o", "pid"), ("-o", "pid,comm"), ("-o", "pid=")),
            stdin=("empty",), fixture="shell", stderr="loose", modes=("bash",),
            script=misc_ps_multicall_script, normalize=misc_ps_multicall_normalize),

    # -- login records --------------------------------------------------------
    Utility("who",
            options=(Option("-a"), Option("-b"), Option("-d"), Option("-H"), Option("-l"), Option("-m"), Option("-p"),
                     Option("-q"), Option("-r"), Option("-s"), Option("-t"), Option("-T"), Option("-w"), Option("-u"),
                     Option("--lookup")),
            operands=((), ("utmp",), ("wtmp",), ("empty",), ("missing",), ("a.txt",), ("binary",), ("utmp", "x"),
                      ("am", "i"), ("utmp", "a", "b"), ("dir",), ("unreadable",)),
            stdin=("empty",), fixture="misc", stderr="exact",
            extra=(("--all", "utmp"), ("--boot", "--dead", "--heading", "utmp"), ("--login", "--process", "--runlevel", "utmp"),
                   ("--count", "utmp"), ("--short", "--time", "utmp"), ("--mesg", "utmp"), ("--message", "--users", "utmp"),
                   ("--writable", "wtmp"), ("--all", "--heading", "wtmp"))),

    Utility("users",
            operands=((), ("utmp",), ("wtmp",), ("empty",), ("missing",), ("a.txt",), ("utmp", "wtmp"), ("dir",),
                      ("--", "utmp"), ("-x",)),
            stdin=("empty",), fixture="misc", stderr="exact"),

    Utility("pinky",
            options=(Option("-b"), Option("-h"), Option("-p"), Option("-s"), Option("-f"), Option("-w"),
                     Option("-i"), Option("-q"), Option("--lookup")),
            operands=((), ("root",), ("nosuchuser",), ("root", "daemon"), ("--", "root")),
            stdin=("empty",), fixture="misc", stderr="loose",
            # The long format is refused; it says so the same way whatever
            # else is asked for, so it is walked once.
            extra=(("-l",), ("-l", "root"), ("-l", "-b", "-h", "-p"), ("-s", "-l"), ("-l", "-s"))),

    Utility("last",
            options=(Option("-a"), Option("-d"), Option("-F"), Option("-i"), Option("-n", ("0", "1", "2", "3", "100", "bad"), None),
                     Option("-R"), Option("-w"), Option("-x"), Option("-T"),
                     Option("--time-format", ("notime", "short", "full", "iso", "bad"), True),
                     Option("-p", ("2023-07-22", "now", "bad"), None), Option("-s", ("2023-07-22", "bad"), None),
                     Option("-t", ("2023-07-22", "bad"), None), Option("-3"), Option("-1"), Option("-0")),
            extra=(("--hostlast", "--ip", "-f", "wtmp"), ("--dns", "-f", "wtmp"), ("--fulltimes", "--nohostname", "-f", "wtmp"),
                   ("--limit=2", "--fullnames", "--system", "-f", "wtmp"), ("--tab-separated", "--file=wtmp"),
                   ("--present=2023-07-22 05:00", "-f", "wtmp"), ("--since=yesterday", "-f", "wtmp"),
                   ("--until=2023-07-22 06:00", "-f", "wtmp"), ("-f", "wtmp", "-2", "-x")),
            operands=(("-f", "wtmp"), ("-f", "wtmp", "root"), ("-f", "wtmp", "alice"), ("-f", "wtmp", "dave", "tty1"),
                      ("-f", "wtmp", "pts/0"), ("-f", "wtmp", "reboot"), ("-f", "wtmp", "nosuchuser"), ("-f", "utmp"),
                      ("-f", "empty"), ("-f", "missing"), ("-f", "a.txt"), ("--file=wtmp",), ("-f", "dir"),
                      ("-f", "wtmp", "--", "root")),
            stdin=("empty",), fixture="misc", stderr="loose", normalize=misc_last_normalize),

    Utility("utmpdump",
            options=(Option("-o", ("dump.out", "dir", "missing/x"), None), Option("--output", ("dump.out",), True),
                     Option("-r"), Option("--reverse")),
            operands=((), ("utmp",), ("wtmp",), ("empty",), ("a.txt",), ("missing",), ("utmp.txt",), ("utmp", "wtmp"),
                      ("dir",), ("--", "utmp")),
            stdin=("misc_utmp", "empty", "text", "misc_utmp_text", "misc_wtmp"), fixture="misc", stderr="loose"),

    Utility("wall",
            options=(Option("-t", ("0", "bad"), None), Option("--timeout", ("0",), True),
                     Option("-g", ("nosuchgroup",), None), Option("--group", ("nosuchgroup",), True)),
            operands=((), ("message",), ("a.txt",), ("two", "words"), ("missing",)),
            stdin=("text", "empty"), fixture="misc", stderr="loose", valid=misc_wall_valid),

    Utility("write",
            operands=(("nosuchuser",), ("nosuchuser", "pts/99"), ("root",), ("root", "tty1"), (), ("a", "b", "c"),
                      ("nosuchuser", "/dev/pts/99"), ("--", "nosuchuser"), ("-x", "nosuchuser")),
            stdin=("text", "empty"), fixture="misc", stderr="loose"),

    Utility("logger",
            options=(Option("-i"), Option("--id", ("123", "bad", "0"), True), Option("--id"),
                     Option("-f", ("a.txt", "empty", "missing", "nonl", "dir"), None),
                     Option("-e"), Option("--no-act"),
                     Option("-p", ("user.info", "daemon.err", "local0.debug", "13", "bad", "user.bad", "kern.info",
                                   "info", "0", "191", "192", "user"), None),
                     Option("--octet-count"), Option("--prio-prefix"),
                     Option("-s"), Option("-S", ("1", "64", "1024", "1KiB", "bad"), None),
                     Option("-t", ("tg", "", "a b"), None),
                     Option("-n", ("localhost", "127.0.0.1"), None), Option("-P", ("1", "514", "bad"), None),
                     Option("-T"), Option("-d"), Option("--rfc3164"),
                     Option("--rfc5424", ("notime", "notq", "nohost", "notime,notq,nohost", "bad"), True),
                     Option("--rfc5424"), Option("--sd-id", ("x@1",), True), Option("--sd-param", ('a="b"',), True),
                     Option("--msgid", ("id1", "bad id"), True), Option("-u", ("/dev/log", "missing", "a.txt", "dir"), None),
                     Option("--socket-errors", ("on", "off", "auto", "bad"), True),
                     Option("--journald", ("a.txt", "missing", "empty"), True), Option("--journald")),
            operands=((), ("mw-differential hello",), ("mw-differential", "two words"), ("",), ("mw-differential", "--no-act")),
            stdin=("text", "empty", "misc_logger_prefixed", "nonl", "long", "blanks", "misc_journal"),
            fixture="misc", stderr="exact", normalize=misc_logger_normalize, max_flags=5,
            extra=(("--skip-empty", "--file=b.txt", "--no-act", "--stderr"),
                   ("--priority=user.notice", "--stderr", "--tag=tg2", "--no-act", "mw-differential"),
                   ("--size=100", "--rfc5424", "--no-act", "-s", "mw-differential"),
                   ("--server=localhost", "--port=1", "--udp", "--no-act", "-s", "mw-differential"),
                   ("--server=localhost", "--port=1", "--tcp", "--no-act", "mw-differential"),
                   ("--socket=/dev/log", "--no-act", "-s", "mw-differential"), ("--journald",), ("--journald=a.txt",),
                   ("-is", "-t", "tg", "--no-act", "mw-differential"), ("-p", "user.info", "-s", "--no-act", "--", "-x", "--not-an-option"))),

    # -- identities -----------------------------------------------------------
    Utility("uuidgen",
            options=(Option("-r"), Option("-t"), Option("-6"), Option("-7"), Option("-m"), Option("-s"),
                     Option("-n", ("@dns", "@url", "@oid", "@x500", "6ba7b810-9dad-11d1-80b4-00c04fd430c8", "bad", "@bad"), None),
                     Option("-N", ("www.example.com", "", "deadbeef", "xyz", "two words"), None),
                     Option("-x"), Option("-C", ("0", "1", "3", "bad", "-1"), None), Option("--count", ("2",), True)),
            operands=((), ("x",)), stdin=("empty",), fixture="misc", stderr="loose", normalize=misc_uuid_normalize,
            extra=(("--random",), ("--time",), ("--time-v6",), ("--time-v7",), ("--md5", "--namespace", "@dns", "--name", "example"),
                   ("--sha1", "-n", "@url", "-N", "http://example.com/"), ("--sha1", "--namespace=@oid", "--name=1.2.3", "--hex"),
                   ("--hex", "--md5", "-n", "@x500", "-N", "deadbeef"), ("--random", "--count=3"), ("-r", "-C", "2"))),

    Utility("uuidparse",
            options=(Option("-J"), Option("--json"), Option("-n"), Option("--noheadings"), Option("-r"), Option("--raw"),
                     Option("-o", ("UUID", "VARIANT", "TYPE", "TIME", "UUID,TIME", "TIME,UUID,TYPE,VARIANT", "uuid,type",
                                   "bad", ""), None),
                     Option("--output", ("TYPE",), True)),
            operands=((), ("00000000-0000-0000-0000-000000000000",), ("ffffffff-ffff-ffff-ffff-ffffffffffff",),
                      ("6ba7b810-9dad-11d1-80b4-00c04fd430c8",), ("5df41881-3aed-3515-88a7-2f4a814cf09e",),
                      ("2ed6657d-e927-568b-95e1-2665a8aea6a2",), ("1ec9414c-232a-6b00-b3c8-9f6bdeced846",),
                      ("017f22e2-79b0-7cc3-98c4-dc0c0c07398f",), ("f81d4fae-7dec-11d0-a765-00a0c91e6bf6",),
                      ("F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6",), ("garbage",), ("",), ("f81d4fae-7dec-11d0-a765-00a0c91e6bf6", "garbage",
                      "00000000-0000-0000-0000-000000000000"), ("f81d4fae7dec11d0a76500a0c91e6bf6",),
                      ("{f81d4fae-7dec-11d0-a765-00a0c91e6bf6}",), ("c232ab00-9414-11ec-b3c8-9f6bdeced846",)),
            stdin=("empty",), fixture="misc", stderr="loose"),

    Utility("mcookie",
            options=(Option("-f", ("a.txt", "binary", "edge_65537", "missing", "dir", "-", "empty"), None),
                     Option("--file", ("b.txt",), True),
                     Option("-m", ("0", "1", "16", "4096", "1KiB", "bad", "1K"), None), Option("--max-size", ("8",), True),
                     Option("-v"), Option("--verbose")),
            operands=((), ("x",)), stdin=("text", "empty"), fixture="misc", stderr="loose",
            normalize=misc_mcookie_normalize),

    # -- kernel log and page cache -------------------------------------------
    Utility("dmesg",
            options=(Option("-F", ("kern.log", "kmsg.log", "a.txt", "empty", "missing", "binary"), None),
                     Option("-K", ("kmsg.log", "kern.log", "missing"), None),
                     Option("-x"), Option("-t"), Option("-T"), Option("-e"), Option("-d"), Option("-H"),
                     Option("-J"), Option("-k"), Option("-u"), Option("-r"),
                     Option("-l", ("err", "warn,info", "err+", "debug", "bad", "emerg,alert,crit,err,warn,notice,info,debug"), None),
                     Option("-f", ("kern", "user,daemon", "local7", "bad", "user"), None),
                     Option("-L", ("never", "always", "auto"), True), Option("-P"), Option("-p"), Option("--noescape"),
                     Option("-S"), Option("-s", ("1", "4096", "0", "bad"), None),
                     Option("-n", ("1", "err", "bad", "8"), None), Option("-C"), Option("-c"), Option("-D"), Option("-E"),
                     Option("--time-format", ("delta", "reltime", "ctime", "notime", "iso", "raw", "bad"), True),
                     Option("--since", ("1 hour ago", "bad"), True), Option("--until", ("now",), True)),
            operands=((), ("x",)), stdin=("empty",), fixture="misc", stderr="loose", normalize=misc_dmesg_normalize,
            max_flags=5,
            extra=(("-F", "kern.log", "-w"), ("-F", "kern.log", "-W"), ("-w",), ("-F", "kern.log", "-x", "-T"),
                   ("-K", "kmsg.log", "-x", "--time-format=iso"), ("-F", "kern.log", "-J", "-x"), ("-F", "kern.log", "-H"),
                   ("-F", "kern.log", "-L=always"), ("--file=kern.log", "--decode", "--notime"),
                   ("--file=kern.log", "--ctime", "--show-delta"), ("--kmsg-file=kmsg.log", "--reltime"),
                   ("--file=kern.log", "--human", "--nopager"), ("--file=kern.log", "--json"),
                   ("--file=kern.log", "--kernel", "--force-prefix"), ("--file=kern.log", "--userspace", "--raw"),
                   ("--file=kern.log", "--level=notice", "--facility=daemon"), ("--file=kern.log", "--color=always"),
                   ("--file=kern.log", "--color"), ("--file=kern.log", "--syslog", "--buffer-size=8192"),
                   ("--console-level=warn",), ("--clear",), ("--read-clear",), ("--console-off",), ("--console-on",),
                   ("--follow",), ("--follow-new",))),

    Utility("fincore",
            options=(Option("-J"), Option("-b"), Option("-n"),
                     Option("-o", ("RES", "PAGES", "SIZE", "FILE", "RES,FILE", "FILE,SIZE,PAGES,RES", "DIRTY_PAGES",
                                   "res,file", "bad", ""), None),
                     Option("--output-all"), Option("-r"), Option("-c"), Option("-R"), Option("-C"),
                     Option("--json"), Option("--raw")),
            operands=(("a.txt",), ("a.txt", "b.txt"), ("edge_65537",), ("empty",), ("dir",), ("missing",),
                      ("binary", "missing", "a.txt"), (), ("two words",), ("long",), ("link",), ("dangling",), ("unreadable",)),
            stdin=("empty",), fixture="misc", stderr="loose",
            extra=(("--bytes", "--noheadings", "a.txt"), ("--output=SIZE", "--raw", "a.txt", "b.txt"), ("--total", "a.txt"),
                   ("--recursive", "dir"), ("--cachestat", "a.txt"), ("--json", "--bytes", "edge_65537"))),
)
