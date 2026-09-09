"""The file utilities: what each one says, and what each one leaves behind.

Every mutating tool here is run inside the recreated directory, so its whole
effect -- names, modes, sizes, contents, link targets, deliberate times -- is
compared as well as its output. The fixture carries a stamp on every entry,
which is what makes a listing's dates, a walk's ages and a copied time the
same on both runs; only the times a run itself creates are unstable, and the
normaliser below blanks exactly those.
"""
import dataclasses
import datetime
import os
import random
import re
import time

from differential import Utility, Option, INPUTS, FIXTURES


# ----------------------------------------------------------------------------
#       Inputs and the fixture.
# ----------------------------------------------------------------------------

INPUTS.update({
    "files_yes": b"y\n" * 16,
    "files_no": b"n\n" * 16,
    "files_mixed": b"y\nn\n" * 8,
    "files_words": b"one two  three\n'four five' six\n\"seven eight\" nine\nten\\ eleven\n\ntwelve\n",
    "files_nul_words": b"a.txt\0b.txt\0two words\0\0missing\0dir\0",
    "files_paths": b"a.txt\nb.txt\nmissing\ndir\nlink\n",
    "files_eof_words": b"a\nb\n_\nc\nd\n",
    "files_dates": b"2001-09-09\n@1000000000\n1999-12-31 23:59:59\nSep 9 2001 12:00\nnonsense\n\n2024-02-29 +1 year\n",
    "files_colors": b"TERM xterm*\nDIR 01;34\nLINK 01;36\n.tar 01;31\n",
})

# Access times run the other way from modification times, so -u and -t sort
# differently; both stay ten digits, clear of the inode masking below.
files_ATIME_BASE = 2800000000


def files_stamp(moment):
    return (files_ATIME_BASE - moment, moment)


def files_file(contents, moment, mode=0o644):
    return ("mode", contents, mode, files_stamp(moment))


def files_dir(moment, mode=None):
    return ("dir", mode, files_stamp(moment))


def files_link(target, moment):
    return ("link", target, files_stamp(moment))


files_COLORS = (b"TERM xterm*\nCOLORTERM true*\nRESET 1\nNORMAL 2\nFILE 3\nDIR 4\nLINK target\n"
                b"MULTIHARDLINK 5\nFIFO 6\nSOCK 7\nDOOR 8\nBLK 9\nCHR 10\nORPHAN 11\nMISSING 12\n"
                b"SETUID 13\nSETGID 14\nCAPABILITY 15\nSTICKY_OTHER_WRITABLE 16\nOTHER_WRITABLE 17\n"
                b"STICKY 18\nEXEC 19\nLEFTCODE \\e[\nRIGHTCODE m\nENDCODE \\e[0m\n.tar 20\n*.wow 21\n")

# 2026-07-15 08:30:00 UTC: within ls's six-month "recent" window until
# 2027-01-15, when it silently joins the old files; nothing breaks then.
files_RECENT = 1784104200

FIXTURES["files"] = {
    "a.txt": files_file(b"alpha\nbeta\ngamma\n", 1000000000),
    "b.txt": files_file(b"two\nbeta\n", 1100000000),
    "c.txt": files_file(b"10\n9\n100\n2\n", 1200000000),
    "two words": files_file(b"spaced\n", 1300000000),
    ".hidden": files_file(b"hidden\n", 1400000000),
    "empty": files_file(b"", 1500000000),
    "nonl": files_file(b"no newline at the end", 1600000000),
    "recent.txt": files_file(b"recent\n", files_RECENT),
    "b.txt~": files_file(b"backup\n", 1410000000),
    "exe": files_file(b"#!/bin/sh\nprintf 'ran:%s\\n' \"$*\"\n", 1250000000, 0o755),
    "unreadable": files_file(b"secret\n", 1350000000, 0o000),
    "binary": files_file(bytes(range(256)) * 3, 1450000000),
    "many": files_file(INPUTS["many_lines"], 1460000000),
    "repeats": files_file(INPUTS["repeats"], 1470000000),
    "words": files_file(INPUTS["files_words"], 1550000000),
    "nulwords": files_file(INPUTS["files_nul_words"], 1560000000),
    "paths": files_file(INPUTS["files_paths"], 1570000000),
    "dates": files_file(INPUTS["files_dates"], 1580000000),
    "colors": files_file(files_COLORS, 1590000000),
    "colors_quote": files_file(b"dir a'b!c\n.foo z'q\n", 1591000000),
    "colors_colon": files_file(b"DIR 31:44\n", 1592000000),
    "tab\tname": files_file(b"tab\n", 1610000000),
    "new\nline": files_file(b"newline\n", 1620000000),
    "quote'mark": files_file(b"quote\n", 1630000000),
    'dq"uote': files_file(b"dquote\n", 1640000000),
    "esc\x1bape": files_file(b"escape\n", 1660000000),
    "back\\slash": files_file(b"backslash\n", 1670000000),
    "star*glob": files_file(b"star\n", 1680000000),
    "-dash": files_file(b"dash\n", 1690000000),
    "dir/inside": files_file(b"nested\n", 1150000000),
    "dir/sub/deep": files_file(b"deeper\n", 1160000000),
    "dir/sub/deep.txt": files_file(b"deep text\n", 1165000000),
    "dir/sub/back": files_link("..", 1310000000),
    "self": files_link(".", 1315000000),
    "dir/sub": files_dir(1710000000),
    "dir": files_dir(1700000000),
    "deep/one/two/three/leaf": files_file(b"leaf\n", 1170000000),
    "deep/one/two/three": files_dir(1735000000),
    "deep/one/two": files_dir(1734000000),
    "deep/one": files_dir(1733000000),
    "deep": files_dir(1732000000),
    "nest/a/b": files_dir(1742000000),
    "nest/a": files_dir(1741000000),
    "nest": files_dir(1740000000),
    "hollow": files_dir(1745000000),
    "shut/inside": files_file(b"shut in\n", 1180000000),
    "shut": files_dir(1750000000, 0o000),
    "dup/one": files_file(b"same\n", 1190000000),
    "dup/two": files_file(b"same\n", 1191000000),
    "dup/three": files_file(b"other\n", 1192000000),
    "dup/sub/four": files_file(b"same\n", 1193000000),
    "dup/sub": files_dir(1761000000),
    "dup": files_dir(1760000000),
    "twin": ("hard", "b.txt"),
    "link": files_link("a.txt", 1260000000),
    "dirlink": files_link("dir", 1270000000),
    "dangling": files_link("nowhere", 1280000000),
    "loop": files_link("loop", 1290000000),
    "badwalk": files_link("a.txt/..", 1295000000),
    # Long and short link targets in turn move the unread rest of a path
    # both ways while it is resolved; chain hops through the long one.
    "longlink": files_link("./" * 64 + "dir", 1296000000),
    "chain" + "x" * 93: files_link("longlink", 1297000000),
    "updown": files_link("dir/sub/..", 1298000000),
    ".": files_dir(1650000000),
}

files_UID = str(os.getuid())
files_GID = str(os.getgid())


# ----------------------------------------------------------------------------
#       Normalisers. Each erases one legitimately unstable column, on both
#       sides, and nothing else.
# ----------------------------------------------------------------------------

files_MONTHS = (b"Jan", b"Feb", b"Mar", b"Apr", b"May", b"Jun",
                b"Jul", b"Aug", b"Sep", b"Oct", b"Nov", b"Dec")
files_NOW_SLACK = 900


def files_near_now(seconds):
    return abs(seconds - time.time()) <= files_NOW_SLACK


def files_moment(year, month, day, hour, minute, second=0):
    try:
        return datetime.datetime(year, month, day, hour, minute, second,
                                 tzinfo=datetime.timezone.utc).timestamp()
    except ValueError:
        return None


def files_hide_now(data):
    """A time within a quarter hour of now is when the run happened -- a
    change time, a birth time, the ".." entry, a file the tool just made --
    and is blanked in every spelling the tools use. Anything else is kept."""
    year_now = datetime.datetime.now(datetime.timezone.utc).year

    def iso(match):
        moment = files_moment(int(match[1]), int(match[2]), int(match[3]),
                              int(match[4]), int(match[5]), int(match[6] or 0))
        return b"<NOW>" if moment is not None and files_near_now(moment) else match[0]

    data = re.sub(rb"(\d{4})-(\d\d)-(\d\d)[ T](\d\d):(\d\d)(?::(\d\d)(?:[.,]\d+)?)?"
                  rb"(?: ?[+-]\d\d:?\d\d| ?UTC| ?Z)?", iso, data)

    def short_iso(match):
        moment = files_moment(year_now, int(match[1]), int(match[2]), int(match[3]), int(match[4]))
        return b"<NOW>" if moment is not None and files_near_now(moment) else match[0]

    data = re.sub(rb"(?<!\d)(\d\d)-(\d\d) (\d\d):(\d\d)(?!\d)", short_iso, data)

    def named(match):
        month = files_MONTHS.index(match[1]) + 1 if match[1] in files_MONTHS else 0
        year = int(match[6]) if match[6] else year_now
        moment = files_moment(year, month, int(match[2]), int(match[3]), int(match[4]),
                              int(match[5] or 0)) if month else None
        return b"<NOW>" if moment is not None and files_near_now(moment) else match[0]

    data = re.sub(rb"(?:(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun),? )?(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
                  rb" +(\d\d?) (\d\d):(\d\d)(?::(\d\d)(?:\.\d+)?)?(?: [A-Z]{3,4})?(?: (\d{4}))?",
                  named, data)

    def epoch(match):
        return b"<NOW>" if files_near_now(int(match[1])) else match[0]

    return re.sub(rb"(?<![\d.])(\d{10})(?:\.\d+)?(?![\d.])", epoch, data)


def files_hide_inodes(data):
    """Inode numbers are the filesystem's, not the tool's: the six to nine
    digit numbers at a line's start (ls -i, find -ls), inline (ls -Ci), after
    stat's label, or in a format this grammar labels ino=."""
    data = re.sub(rb"Inode: \d+", b"Inode: #", data)
    data = re.sub(rb"ino=\d+", b"ino=#", data)
    return re.sub(rb"(?<![\w.:/+-])\d{6,9}(?![\w.:/+-])", b"#", data)


def files_hide_digits(data):
    return re.sub(rb"\d", b"X", data)


def files_hide_help_hint(data):
    """GNU follows a usage error with "Try 'x --help' for more information."
    This suite has no --help to try, on purpose, and says nothing there; the
    line is dropped from both sides and the diagnostic itself is compared."""
    return re.sub(rb"(?m)^Try '[^'\n]*' for more information\.\n", b"", data)


def files_normal(*steps):
    def normalize(channel, data):
        if channel == "stderr":
            data = files_hide_help_hint(data)
        for step in steps:
            data = step(data)
        return data
    return normalize


files_plain = files_normal()


def files_sorted_records(data):
    """shuf's answer is a permutation; the multiset is what is checked."""
    separator = b"\0" if b"\0" in data and b"\n" not in data.rstrip(b"\n") else b"\n"
    records = data.split(separator)
    tail = records.pop() if records and records[-1] == b"" else None
    records.sort()
    if tail is not None:
        records.append(tail)
    return separator.join(records)


def files_hide_duration(data):
    return re.sub(rb"Duration: [0-9.]+ seconds", b"Duration: # seconds", data)


files_when = files_normal(files_hide_now)
files_listing = files_normal(files_hide_now, files_hide_inodes)
files_stdout_sorted = files_normal(files_sorted_records)


# ----------------------------------------------------------------------------
#       Words the grammars share.
# ----------------------------------------------------------------------------

files_UID_OPERANDS = (
    (files_UID, "a.txt"), (files_UID + ":" + files_GID, "a.txt"), (":" + files_GID, "b.txt"),
    ("root", "a.txt"), ("nosuchuser", "a.txt"), (files_UID, "missing"), (files_UID, "link"),
    (files_UID, "dangling"), (files_UID, "dir"), (files_UID, "a.txt", "b.txt", "dir"),
    (files_UID, "shut"), (files_UID + ":", "a.txt"), (":", "a.txt"), ("", "a.txt"),
    (files_UID,), (), (files_UID, "dirlink"), (files_UID, "two words"), (files_UID, "unreadable"),
    ("0:0", "a.txt"), (files_UID + ":nosuchgroup", "a.txt"), (files_UID, "deep"),
)

files_GID_OPERANDS = (
    (files_GID, "a.txt"), ("root", "a.txt"), ("nosuchgroup", "a.txt"), (files_GID, "missing"),
    (files_GID, "link"), (files_GID, "dangling"), (files_GID, "dir"), (files_GID, "a.txt", "b.txt", "dir"),
    (files_GID, "shut"), ("", "a.txt"), (files_GID,), (), (files_GID, "dirlink"),
    (files_GID, "two words"), (files_GID, "unreadable"), ("0", "a.txt"), (files_GID, "deep"),
)

files_CHMOD_OPERANDS = tuple(
    (mode, target) for mode, target in (
        ("0600", "a.txt"), ("0000600", "a.txt"), ("0788", "a.txt"), ("0711", "dir"), ("u+x", "a.txt"),
        ("go-rwx", "b.txt"), ("a=rw", "a.txt"), ("u+rw,g=r,o-rwx", "b.txt"), ("a+X", "dir"), ("a+X", "a.txt"),
        ("0755", "dir"), ("4755", "exe"), ("1777", "dir"), ("2755", "dir"), ("755", "hollow"), ("00755", "dir"),
        ("=rwx", "a.txt"), ("u=rwx", "dir"), ("=r", "a.txt"), ("a-s", "exe"), ("u-s", "exe"), ("+w", "a.txt"),
        ("-w", "a.txt"), ("-rwx", "a.txt"), ("-u+x", "exe"), ("o+t", "dir"), ("+t", "dir"), ("+u", "a.txt"),
        ("+X", "dir"), ("a=rwX", "dir"), ("g+s", "dir"), ("u+s", "a.txt"), ("ug=rw", "a.txt"), ("o=", "a.txt"),
        ("a+r,a-w", "a.txt"), ("+", "a.txt"), ("-", "a.txt"), ("=", "a.txt"), ("rwx", "a.txt"), ("x", "a.txt"),
        ("8", "a.txt"), ("u+q", "a.txt"), ("770", "unreadable"), ("0600", "link"), ("0600", "dangling"),
        ("0600", "missing"), ("0600", "shut"), ("0600", "two words"), ("0600", "empty"), ("0600", "dirlink"),
        ("0600", "loop"), ("644", "deep"), ("u+x,g-w", "dir"), ("0600", "twin"),
    )
) + (("0600", "a.txt", "b.txt", "dir"), ("0600",), (), ("--", "-w", "a.txt"), ("0600", "--", "-dash"))


# ----------------------------------------------------------------------------
#       find: the expression is a language, so its cases are composed.
# ----------------------------------------------------------------------------

files_FIND_ROOTS = ((".",), ("dir",), ("a.txt",), ("link",), ("dirlink",), ("dangling",), ("missing",),
                    ("dir", "a.txt"), ("shut",), ("loop",), ("deep",), ("two words",), ("dir/sub/back",),
                    ("dup", "nest"), (), ("hollow",), ("badwalk",), ("dir/",), ("dirlink/",))

files_FIND_GLOBALS = (("-maxdepth", "0"), ("-maxdepth", "1"), ("-maxdepth", "2"), ("-maxdepth", "3"),
                      ("-mindepth", "1"), ("-mindepth", "2"), ("-mindepth", "1", "-maxdepth", "2"),
                      ("-depth",), ("-daystart",), ("-xdev",), ("-mount",), ("-noleaf",),
                      ("-ignore_readdir_race",), ("-noignore_readdir_race",), ("-maxdepth", "x"),
                      ("-maxdepth", "-1"), ("-mindepth", "+1"), ("-follow",), ("-warn",), ("-nowarn",),
                      ("-regextype", "posix-egrep"), ("-regextype", "emacs"), ("-regextype", "bogus"))

files_FIND_TESTS = (
    ("-name", "*.txt"), ("-name", "a*"), ("-name", "?.txt"), ("-name", "[ab]*"), ("-name", "[!a]*"),
    ("-name", "a.txt"), ("-name", "*"), ("-name", "two words"), ("-name", "new*"), ("-name", "*\\**"),
    ("-name", "dir/inside"), ("-name", ".*"), ("-iname", "A*"), ("-iname", "*.TXT"), ("-iname", "LEAF"),
    ("-path", "*dir*"), ("-path", "./dir/*"), ("-path", "*/sub"), ("-ipath", "*SUB*"), ("-wholename", "*sub*"),
    ("-iwholename", "*DEEP*"), ("-lname", "*.txt"), ("-lname", "nowhere"), ("-ilname", "NOWHERE"),
    ("-regex", ".*\\.txt"), ("-regex", "\\./d.*"), ("-regex", ".*/[a-c]"), ("-iregex", ".*TXT"), ("-regex", "["),
    ("-type", "f"), ("-type", "d"), ("-type", "l"), ("-type", "f,d"), ("-type", "p"), ("-type", "x"),
    ("-type", "s"), ("-type", "b,c"), ("-xtype", "l"), ("-xtype", "f"), ("-xtype", "d"),
    ("-size", "0"), ("-size", "1"), ("-size", "+0"), ("-size", "-1"), ("-size", "+10c"), ("-size", "-10c"),
    ("-size", "1k"), ("-size", "+1w"), ("-size", "-1M"), ("-size", "0G"), ("-size", "+0b"), ("-size", "1x"),
    ("-size", "+5c"), ("-size", "-1000c"), ("-size", "6c"),
    ("-empty",), ("-mtime", "+1"), ("-mtime", "-1"), ("-mtime", "0"), ("-mtime", "+100"), ("-mtime", "-100000"),
    ("-mtime", "+5000"), ("-mmin", "+60"), ("-mmin", "-60"), ("-atime", "+1"), ("-atime", "-100000"),
    ("-amin", "+1"), ("-ctime", "-1"), ("-ctime", "+1"), ("-cmin", "-60"), ("-cmin", "+60"),
    ("-used", "+1"), ("-used", "-100000"), ("-mtime", "x"), ("-mmin", ""), ("-atime", "1.5"),
    ("-newer", "a.txt"), ("-newer", "b.txt"), ("-newer", "missing"), ("-newer", "recent.txt"),
    ("-anewer", "a.txt"), ("-cnewer", "a.txt"), ("-newermt", "2003-01-01"), ("-newermt", "2001-09-09 01:46:40"),
    ("-newermt", "nonsense"), ("-newerat", "2010-01-01"), ("-newerct", "2000-01-01"), ("-newermm", "b.txt"),
    ("-newerBt", "2000-01-01"), ("-newerxy", "a.txt"), ("-newermt", "@1300000000"),
    ("-perm", "644"), ("-perm", "-644"), ("-perm", "/222"), ("-perm", "-u+x"), ("-perm", "/u+w,g+w"),
    ("-perm", "0"), ("-perm", "/0"), ("-perm", "a=r"), ("-perm", "-000"), ("-perm", "755"), ("-perm", "x"),
    ("-perm", "+111"), ("-perm", "/u=x"), ("-perm", "-a="), ("-perm", "0000"), ("-perm", "u=rw,go=r"),
    ("-links", "1"), ("-links", "2"), ("-links", "+1"), ("-links", "-2"), ("-links", "+2"), ("-inum", "+0"),
    ("-inum", "-1"), ("-inum", "x"), ("-user", "root"), ("-user", "nosuchuser"), ("-user", files_UID),
    ("-uid", "0"), ("-uid", "+0"), ("-uid", files_UID), ("-gid", "0"), ("-gid", "+0"), ("-gid", files_GID),
    ("-group", "root"), ("-group", "nosuchgroup"), ("-group", files_GID), ("-nouser",), ("-nogroup",),
    ("-readable",), ("-writable",), ("-executable",), ("-true",), ("-false",), ("-samefile", "a.txt"),
    ("-samefile", "link"), ("-samefile", "twin"), ("-samefile", "missing"), ("-fstype", "ext4"),
    ("-fstype", "nfs"), ("-fstype", "nope"), ("-context", "*"),
)

files_FIND_ACTIONS = (
    ("-print",), ("-print0",), ("-ls",), ("-prune",), ("-quit",), ("-delete",),
    ("-printf", "%p\\n"), ("-printf", "%f %s %m\\n"), ("-printf", "%y %Y %l|%p\\n"),
    ("-printf", "%TY-%Tm-%Td %TH:%TM:%TS %Tz|%p\\n"), ("-printf", "%t|%p\\n"), ("-printf", "%d %h %P\\n"),
    ("-printf", "%n %u %g %M %p\\n"), ("-printf", "%b %k %S %p\\n"), ("-printf", "%AY %CY %p\\n"),
    ("-printf", "ino=%i dev=%D %p\\n"), ("-printf", "%H|%p\\n"), ("-printf", "%%|%10s|%-10f|\\n"),
    ("-printf", "%F %p\\n"), ("-printf", "%a|%c|%p\\n"), ("-printf", "%A@ %T@ %C@ %p\\n"),
    ("-printf", "%p\\0"), ("-printf", "%f\\t%s\\n"), ("-printf", "%Ta %Tb %TA %TB %p\\n"),
    ("-printf", "%Tj %TU %Tw %p\\n"), ("-printf", "%Tk %Tl %Tp %Tr %TR %TT %p\\n"),
    ("-printf", "%TD %TF %Ts %p\\n"), ("-printf", "%TZ %Tz %TG %Tg %TV %Tu %p\\n"),
    ("-printf", "%Ty %TC %Te %TS %p\\n"), ("-printf", "%Q\\n"), ("-printf", "%5p|%-5f|%.2f|%05s\\n"),
    ("-printf", "%p\\a\\b\\f\\r\\v\\\\\\n"), ("-printf", "%p\\101\\n"), ("-printf", "%p"), ("-printf", "%"),
    ("-printf", "%u %U %g %G %p\\n"), ("-printf", "%T+ %p\\n"), ("-printf", "%Tx %TX %Tc %p\\n"),
    ("-printf", "%Z %p\\n"), ("-printf", "%{bogus} %p\\n"),
    ("-fprint", "out"), ("-fprint0", "out"), ("-fprintf", "out", "%f\\n"), ("-fls", "out"),
    ("-fprint", "missing/out"), ("-fprint", "out", "-fprint", "out"),
    ("-exec", "echo", "{}", ";"), ("-exec", "./exe", "{}", ";"), ("-exec", "printf", "%s\\n", "{}", "+"),
    ("-exec", "./exe", "{}", "+"), ("-exec", "echo", "x{}y", ";"), ("-exec", "test", "-s", "{}", ";"),
    ("-exec", "false", ";"), ("-exec", "rm", "{}", ";"), ("-exec", "rm", "-r", "{}", "+"),
    ("-exec", "sh", "-c", "echo $#", "sh", "{}", "+"), ("-exec", "missing-command", "{}", ";"),
    ("-exec", "echo", "{}"), ("-exec", "echo", "{}", "{}", "+"), ("-exec", "echo", "{}", "+", "-print"),
    ("-execdir", "printf", "%s\\n", "{}", ";"), ("-execdir", "printf", "%s\\n", "{}", "+"),
    ("-execdir", "./exe", "{}", ";"), ("-execdir", "rm", "{}", ";"), ("-execdir", "echo", ";"),
    ("-ok", "echo", "{}", ";"), ("-ok", "rm", "{}", ";"), ("-okdir", "printf", "%s\\n", "{}", ";"),
    ("-ok", "echo", "{}", "+"),
)

files_FIND_BROKEN = (
    ("-nonsense",), (".", "-nonsense"), (".", "(", "-name", "a.txt"), (".", "-name", "a.txt", ")"),
    (".", "-exec", "echo", "{}"), (".", "-name"), (".", "-name", "a.txt", "dir"), (".", "-o", "-name", "a.txt"),
    (".", "-name", "a.txt", "-a"), (".", "!"), (".", "(", ")"), (".", "-type"), (".", "-size", ""),
    (".", "-maxdepth"), (".", "-newer"), (".", "-printf"), (".", "-exec"), (".", "-exec", ";"),
    (".", "-name", "a.txt", "-maxdepth", "1"), ("-L", "-P", ".", "-type", "l"), ("-H", "-L", "link", "-type", "f"),
    ("-P", "-H", "link", "-type", "f"), (".", "-name", "*.txt", "-o"), (".", "-depth", "-prune", "-o", "-print"),
    (".", "-delete", "-name", "a.txt"), (".", "-name", "a.txt", "-delete", "-prune"),
    (".", "-files0-from", "nulwords"), ("-files0-from", "nulwords", "-type", "f"), ("-files0-from", "missing"),
    ("-files0-from", "-", "-name", "*.txt"), (".", "-files0-from", "nulwords", "-print"), ("-files0-from", "empty"),
    (".", "-D", "tree", "-name", "a.txt"), ("-D", "help"), ("-O3", ".", "-name", "a.txt"), ("-O", ".", "-name", "a.txt"),
    ("-O9", "."), (".", "-newer"), (".", "-samefile"), (".", "-perm"), (".", "-user"), (".", "-user", ""),
    (".", "-uid", "x"), (".", "-links", ""), (".", "-empty", "-empty"), (".", "-name", "a.txt", ",", "-name", "b.txt"),
    (".", ",", "-print"), (".", "-print", ","), (".", "-true", "-quit", "-print"),
    ("-L", ".", "-name", "a.txt", "-print"), ("-L", "dir", "-type", "f"), ("-L", "dir/sub/back", "-maxdepth", "3"),
    ("-L", "loop"), ("-L", "dangling", "-type", "l"), ("-L", "badwalk"), ("-H", "dirlink", "-type", "d"),
    ("-H", "dir", "-type", "l"), ("-L", "-P", "-L", ".", "-type", "d"), (".", "-follow", "-type", "f"),
    (".", "-xdev", "-type", "d"), (".", "-noleaf", "-type", "d"), (".", "-depth", "-type", "d"),
    (".", "-mindepth", "1", "-maxdepth", "1", "-type", "d"), (".", "-maxdepth", "0"), ("dir", "-mindepth", "5"),
    ("a.txt", "-type", "f", "-print", "-print"), ("missing", "a.txt", "-print"), ("", "-print"), ("a.txt", ""),
) + tuple((".",) + ("-true",) * n for n in (40, 160))


def files_find_tree(rng, depth):
    """An expression tree: tests joined by every operator, negated and
    grouped, three levels deep at most."""
    roll = rng.random()
    if depth >= 2 or roll < 0.4:
        return list(rng.choice(files_FIND_TESTS))
    if roll < 0.55:
        return [rng.choice(("!", "-not"))] + files_find_tree(rng, depth + 1)
    if roll < 0.85:
        joiner = rng.choice(([], ["-a"], ["-and"], ["-o"], ["-or"], [","]))
        return files_find_tree(rng, depth + 1) + joiner + files_find_tree(rng, depth + 1)
    inner = files_find_tree(rng, depth + 1)
    return ["("] + inner + [")"] + (files_find_tree(rng, depth + 1) if rng.random() < 0.5 else [])


def files_find_expressions(count):
    rng = random.Random(0x46494e44)
    made = []
    seen = set()
    while len(made) < count:
        words = []
        if rng.random() < 0.25:
            words.append(rng.choice(("-L", "-H", "-P")))
        words.extend(rng.choice(files_FIND_ROOTS))
        if rng.random() < 0.35:
            words.extend(rng.choice(files_FIND_GLOBALS))
        words.extend(files_find_tree(rng, 0))
        roll = rng.random()
        if roll < 0.5:
            words.extend(rng.choice(files_FIND_ACTIONS))
        elif roll < 0.62:
            words.extend(rng.choice(files_FIND_ACTIONS))
            words.extend(rng.choice(files_FIND_ACTIONS))
        key = tuple(words)
        if key not in seen:
            seen.add(key)
            made.append(key)
    return tuple(made)


files_FIND_WALKED = (
    (".",), ("dir", "-name", "*.txt"), (".", "-type", "f"), (".", "-type", "d"), (".", "-type", "l"),
    (".", "-maxdepth", "1"), (".", "-mindepth", "2"), (".", "-name", "a.txt", "-o", "-name", "b.txt"),
    (".", "!", "-type", "d"), (".", "(", "-name", "*.txt", "-o", "-name", "leaf", ")", "-type", "f"),
    (".", "-name", "dir", "-prune", "-o", "-print"), (".", "-depth"), (".", "-newer", "b.txt"),
    (".", "-perm", "-u+x"), (".", "-empty"), (".", "-size", "+0"), (".", "-links", "+1"),
    (".", "-name", "*.txt", "-print0"), (".", "-name", "a.txt", "-printf", "%p %s %m\\n"), (".", "-ls"),
    (".", "-type", "f", "-exec", "echo", "{}", ";"), (".", "-type", "f", "-exec", "printf", "%s\\n", "{}", "+"),
    ("dir", "-delete"), ("dir", "-name", "inside", "-delete"), (".", "-name", "a.txt", "-ok", "rm", "{}", ";"),
    ("dir/sub/back", "-name", "a.txt"), ("dirlink", "-type", "f"), ("link", "-type", "f"), ("dangling",),
    ("missing",), ("shut",), ("loop",), (), (".", "-name", "a.txt", "-quit"), (".", "-regex", ".*\\.txt"),
    (".", "-mtime", "+100"), (".", "-newermt", "2003-01-01"), (".", "-iname", "*.TXT", "-fprint", "out"),
)


# ----------------------------------------------------------------------------
#       The grammars.
# ----------------------------------------------------------------------------

files_LS_OPTIONS = (
    Option("-a"), Option("-A"), Option("-l"), Option("-1"), Option("-C"), Option("-x"), Option("-m"),
    Option("-d"), Option("-R"), Option("-F"), Option("-p"), Option("-i"), Option("-s"), Option("-h"),
    Option("-S"), Option("-t"), Option("-u"), Option("-c"), Option("-r"), Option("-U"), Option("-v"),
    Option("-X"), Option("-f"), Option("-g"), Option("-o"), Option("-n"), Option("-G"), Option("-k"),
    Option("-B"), Option("-N"), Option("-Q"), Option("-b"), Option("-q"), Option("-H"), Option("-L"),
    Option("-D"), Option("-Z"), Option("--author"), Option("--si"), Option("--full-time"),
    Option("--group-directories-first"), Option("--file-type"), Option("--show-control-chars"),
    Option("--zero"), Option("--dereference-command-line-symlink-to-dir"), Option("--color"),
    Option("--color", ("never", "always", "auto", "yes", "no", "tty", "sometimes"), True),
    Option("--classify", ("never", "always", "auto"), True),
    Option("--hyperlink", ("never", "always", "auto"), True),
    Option("--indicator-style", ("none", "slash", "file-type", "classify", "bogus"), True),
    Option("--format", ("across", "horizontal", "commas", "long", "single-column", "verbose", "vertical", "bogus"), True),
    Option("--sort", ("none", "size", "time", "version", "extension", "name", "width", "bogus"), True),
    Option("--time", ("atime", "access", "use", "ctime", "status", "mtime", "modification", "bogus"), True),
    Option("--time-style", ("full-iso", "long-iso", "iso", "locale", "+%Y-%m-%d", "+%s", "+%F %T",
                            "+%b %e\n%H:%M", "posix-long-iso", "bogus"), True),
    Option("--quoting-style", ("literal", "locale", "shell", "shell-always", "shell-escape",
                               "shell-escape-always", "c", "escape", "bogus"), True),
    Option("--block-size", ("K", "M", "1", "512", "1K", "KB", "'1", "bogus"), True),
    Option("-w", ("0", "1", "20", "40", "80", "200", "x"), None),
    Option("-T", ("0", "1", "4", "8", "x"), None),
    Option("-I", ("*.txt", "a*", "?", "dir", "*"), None),
    Option("--hide", ("*.txt", "a*", "dir"), True),
)

files_LS_OPERANDS = (
    (), ("a.txt",), ("dir",), ("dir", "a.txt", "link"), ("link",), ("dirlink",), ("dangling",), ("missing",),
    ("unreadable",), ("shut",), ("two words", "b.txt"), ("--", "-dash"), (".hidden", "hollow"), ("deep",),
    ("exe", "twin"), ("dir/sub",), ("missing", "a.txt"), ("loop",), ("dir/sub/back",), ("new\nline",),
    ("recent.txt",), ("dirlink/",), ("dup",), ("/dev/null",), ("badwalk",),
)

files_LS_COLORS = "di=34:ln=36:pi=33:ex=32:fi=0:*.txt=35:rs=0:or=31:mi=35:*.tar=01;31"

files_DATE_FORMAT = "+%Y-%m-%d %H:%M:%S"

files_DATE_READS = (
    "2001-09-09", "2001-09-09 01:46:40", "2001-09-09T01:46:40", "2001-09-09 01:46", "2001-9-9", "01-09-09",
    "69-09-09", "68-09-09", "2000-02-29", "1999-12-31 23:59:59", "1900-01-01", "2100-06-15 12:00:00",
    "2001-09-09 12:00", "2001-09-09 UTC", "2001-09-09 12:00 UTC", "2001-09-09 +1 day", "2001-09-09 -1 day",
    "2001-09-09 1 day ago", "2001-09-09 yesterday", "2001-09-09 tomorrow", "2001-09-09 +1 month",
    "2001-09-09 -1 year", "2001-09-09 +4 months", "2024-01-31 +1 month", "2024-03-31 -1 month",
    "2024-02-29 +1 year", "2001-09-09 next week", "2001-09-09 last month", "2001-09-09 day",
    "2001-09-09 fortnight", "2001-09-09 +2 fortnights", "2001-09-09 3 hours", "2001-09-09 -90 minutes",
    "2001-09-09 5 seconds", "2001-09-09 -1 sec", "2001-09-09 1 min", "2001-09-09 1 hour 1 min 1 sec",
    "2001-09-09 2 days 3 hours ago", "2001-09-09 3 hours 2 days ago", "2001-09-09 01:46:40 1 day ago",
    "2001-09-09 2 weeks", "2001-09-09 YESTERDAY", "@1000000000.5", "@-100", "nonsense",
    "@1000000000 +1 day", "2001-13-09", "2001-09-32", "2001-09-31", "1900-02-29", "2001-09-09 24:00",
    "2001-09-09 2002-01-01", "2001-09-09 01:00 02:00", "", "2001-09-09 01:46:40 +0100",
    "2001-09-09 01:46:40 -0130", "2001-09-09 01:46:40 +01:30", "2001-09-09 01:46:40 +01",
    "2001-09-09 01:46:40 +100", "2001-09-09T01:46:40+0100", "2001-09-09 01:46:40 UTC +0100",
    "2001-09-09 01:46:40 +0100 +1 day", "2001-09-09 01:46:40 +0100 1 day ago", "2001-09-09 12:00 +1 day",
    "2001-09-09 01:46:40.5", "2001-09-09 01:46:40,5", "Sep 9 2001", "9 Sep 2001", "Sep 9, 2001",
    "September 9 2001", "sept 9 2001", "Sun Sep 9 2001", "Mon Sep 9 2001", "Sun, 09 Sep 2001",
    "Fri 2001-09-09", "Sep 9 2001 12:00", "12:00 Sep 9 2001", "Sep 9 01:46:40 2001", "01:46:40 2001-09-09",
    "Feb 29 2000", "Feb 29 2001", "Sep 32 2001", "2001-09-09 01:46:40 +0100 +0200",
    "2001-09-09 01:46:40 +0100 UTC", "2001-09-09 +0100", "Sep 9 2001-09-09", "2001-09-09 01:46:40 PM",
    "2001-09-09 1:46 am", "2001-09-09 noon", "2001-09-09 midnight", "2001-09-09 12pm",
    "1 Jan 1970 00:00:00 +0000", "2001-09-09 01:46:40.123456789 +0000", "20010909", "2001-09-09 01:46:40 GMT",
    "2001-09-09 01:46:40 Z", "2001/09/09", "09/09/2001", "9/9/01", "2001-09-09 monday", "monday",
    "TZ=\"Asia/Tokyo\" 2001-09-09 12:00", "2001-09-09 EST", "2001-09-09 01:46:40 CEST", "@1e9",
    "2001-09-09 25 hours ago", "2001-09-09 -0 day", "2001-09-09 +0 seconds", "2001-09-09 1 week ago",
    "2001-09-09 first day", "2001-09-09 third month", "2001-09-09 next year", "2001-09-09 this minute",
    "10 September 2001", "2001-09-09 01:46:40 am", "2001-09-09 00:00:00", "2001-09-09 23:59:60",
    "2001-09-09 12:60", "2001-09-09 1:2:3", "2001-09-09 1:2", "2001-2-30", "2001-09-09 1 days",
    "2001-09-09 +1 days", "2001-09-09 -1 months", "2001-09-09 sun", "2001-09-09 next sun",
    "2001-09-09 last sun", "2001-09-09 this sun", "2001-09-09 sunday ago", "2001-09-09 2 sundays",
    "1 September 2001 12:00 +0200", "@9223372036854775807", "@-9223372036854775808", "@99999999999999999999",
    "1000-01-01", "9999-12-31 23:59:59", "0001-01-01", "2038-01-19 03:14:08", "1901-12-13 20:45:52",
)

files_DATE_FORMATS = (
    ("+%Y-%m-%d",), ("+%F %T",), ("+%a %A %b %B",), ("+%j %u %w %y %C %e",), ("+%I %p %l %P %r",), ("+%s",),
    ("+%Z %z %:z %::z %:::z",), ("+a%%b%nc%td",), ("+%-d/%-m/%-H",), ("+%_d|%_m|%_H",), ("+%0e|%0k|%0l",),
    ("+%U %W %V %G %g",), ("+%q %N",), ("+%c|%x|%X|%D|%R",), ("+",), ("+%Q%%",), ("+%^a %^b %#p %#Z %^Z",),
    ("+%10Y|%-10d|%_5H|%010s",), ("+%h %k %M %S %T %e %n%t",), ("+%Ey %Od %EY %OH",), ("+%3N %6N %9N %1N",),
    ("+%a %b %e %H:%M:%S %Z %Y",), ("+%%",), ("+%",), ("+%5",), ("+%-",), ("+%^",), ("+%#",), ("+%_",),
    ("+%%%Y%%",), ("+%s %N",), ("+%d.%m.%Y",), ("+%:z|%::z",), ("+%b %e  %Y",), ("1000000000",),
    ("+%F", "+%T"), (), ("+%Y", "extra"),
)

files_TOUCH_DATES = (
    "@1234567890", "@1000000000", "@-1.5", "@1000000000.25", "nonsense", "2001-09-09", "2001-09-09 01:46:40",
    "2001-09-09 1 day ago", "2001-09-09 +1 month", "Sep 9 2001", "9 Sep 2001", "monday", "", "now",
    "1970-01-01", "1970-01-01 00:00:03.123456789", "2000-02-29", "2001-09-09 01:46:40.123456789",
    "2100-01-01", "1900-01-01", "2024-01-31 +1 month", "2001-09-09T01:46:40+0100", "2001-09-09 12:00 UTC",
    "midnight", "@0", "@-1", "2001-09-09 24:00", "1 second", "-1 day",
)

files_TOUCH_STAMPS = (
    "200109090146", "20010909014640", "200109090146.40", "09090146", "0109090146", "6909090146", "6809090146",
    "99", "202113011234", "202602310000", "202601010000.60", "202601010000.61", "197001010000",
    "202601010000.0", "202601010000junk", "", "20260101000000", "200002290000", "190002290000", "210002290000",
    "202604310000", "202601012400", "202601010060", "202601010000.99", "1", "202601010000.59",
)

files_STAT_FORMATS = (
    "%n", "%s", "%a %A", "%F", "%h", "%u %U %g %G", "%b %B", "ino=%i", "%Y", "%X", "%y", "%x", "%Z", "%z", "%W",
    "%w", "%f", "%d", "%D", "%t %T", "%o", "%N", "%m", "%C", "%r %R", "%n|%s|%a|%F|%h", "%n:%s", "100%%",
    "%n\\t%s", "%n\\n", "%-10n|%10s|%05a", "%.3s", "%Q", "%", "\\x41\\101\\a\\b\\e\\f\\r\\v\\\\", "%n %",
    "%n|%i|%l|%s|%S|%b|%c|%T|%t", "free=%a|%f|%d", "%b %c", "%%s %s", "%10.3s|%-12.4n|", "%#o %#a",
)

files_MKTEMP_TEMPLATES = (
    ("tmp.XXXXXXXXXX",), ("run.XXXXXX",), ("run.XXXXXXXXXXXX",), ("run.XXX",), ("run.XXXXXX.log",), ("run.XX",),
    ("run",), ("sub/run.XXXXXX",), ("dir/run.XXXXXX",), (), ("run.XXXXXX", "run.XXXXXX"), ("XXXXXX",),
    ("run.XXXXXXXXXXXXXXXXXXXXXXXX",), ("dangling/run.XXXXXX",), ("shut/run.XXXXXX",), ("a.txt/run.XXXXXX",),
    ("run.XXXXXX/",), ("",),
)


def files_mktemp_valid(argv):
    """A made name is random on both sides and cannot be compared; -u and
    the refusals can. A template with fewer than three X's, a directory
    that is not there, or -u keep a case comparable."""
    if "-u" in argv or "--dry-run" in argv:
        return True
    if any(word.startswith("-p") or word.startswith("--tmpdir") for word in argv):
        if any(word in ("missing", "a.txt", "-pmissing", "--tmpdir=missing", "--tmpdir=a.txt")
               for word in argv):
            return True
    templates = [word for word in argv if not word.startswith("-") and "XXX" in word]
    return not templates and any(not word.startswith("-") for word in argv) or \
        ("--suffix=/bad" in argv)


def files_ls_valid(argv):
    """A listing ordered by change time is not comparable: the fixture's
    change times are written by the run that creates it, a moment apart on
    each side, so the order is the harness's and not the program's. Showing
    one is fine -- the normaliser blanks a time that near now -- so only the
    shapes that sort by it are pruned."""
    asked = False
    long_form = False
    by_time = False

    for word in argv:
        if word in ("--time=ctime", "--time=status"):
            asked = True
        elif word in ("--sort=time", "-t"):
            by_time = True
        elif word.startswith("--"):
            long_form |= word in ("--format=long", "--format=verbose", "--full-time",
                                  "--dired", "--numeric-uid-gid")
        elif word.startswith("-"):
            asked |= "c" in word[1:]
            by_time |= "t" in word[1:]
            long_form |= any(letter in word[1:] for letter in "lgonD")

    return not asked or (long_form and not by_time)


def files_date_valid(argv):
    """Now moves between the two runs; every case names its moment."""
    return any(word.startswith(("-d", "--date", "-r", "--reference", "-f", "--file")) for word in argv)


def files_truncate_valid(argv):
    """-o counts the size in io blocks, so a size meant as bytes becomes
    thousands of times larger and asks for a file past the size limit this
    harness runs under; the reference is killed for it rather than
    answering. Only the small counts are walked with -o."""
    if not any(word in ("-o", "--io-blocks") for word in argv):
        return True

    for index, word in enumerate(argv):
        value = None

        if word == "-s" and index + 1 < len(argv):
            value = argv[index + 1]
        elif word.startswith("-s") and not word.startswith("--"):
            value = word[2:]
        elif word.startswith("--size="):
            value = word[len("--size="):]

        if value is None:
            continue

        digits = value.lstrip("+-<>/%")

        if not digits.isdigit() or int(digits) > 32:
            return False

    return True


def files_shred_valid(argv):
    """Random passes leave random bytes, which no two runs share; the final
    state is compared where a zero pass, a removal or no pass leaves it
    determined. -n0 alone, -z and -u are the deterministic shapes."""
    zeroed = any(word in ("-z", "--zero", "-u") or word.startswith("--remove") or
                 (word.startswith("-") and not word.startswith("--") and
                  ("z" in word[1:] or "u" in word[1:])) for word in argv)
    passes = None
    for index, word in enumerate(argv):
        if word == "-n" and index + 1 < len(argv):
            passes = argv[index + 1]
        elif word.startswith("-n") and not word.startswith("--"):
            passes = word[2:]
        elif word.startswith("--iterations="):
            passes = word[len("--iterations="):]
    return zeroed or passes == "0" or passes not in (None, "1", "2", "3")


def files_xargs_valid(argv):
    """Parallel batches finish in whichever order the scheduler picks; -P
    beyond one is compared only where there is a single batch."""
    parallel = any(word in ("-P", "--max-procs") for word in argv) or \
        any(word.startswith(("-P", "--max-procs=")) and word not in ("-P", "--max-procs=1", "-P1")
            for word in argv)
    if not parallel:
        return True
    if "-P" in argv and argv[argv.index("-P") + 1:argv.index("-P") + 2] == ["1"]:
        return True
    batching = ("-n", "-L", "-l", "-s", "-I", "-i", "--max-args", "--max-lines", "--max-chars", "--replace")
    return not any(word.startswith(batching) for word in argv)


UTILITIES = (
    Utility("basename", options=(Option("-a"), Option("-z"), Option("--multiple"), Option("--zero"),
                                 Option("-s", (".txt", "txt", "", "a.txt", "x", "/"), None),
                                 Option("--suffix", (".txt", ""), True)),
            operands=(("/usr/bin/ls",), ("/usr/bin/",), ("usr",), ("/",), ("//",), (".",), ("..",),
                      ("file.txt", ".txt"), ("file.txt", ".c"), (".txt", ".txt"), ("/a/b", "/c/d"),
                      ("a.txt", "b.txt", "c.txt"), (), ("",), ("a b/c d",), ("--", "-dash"), ("abc", "b", "extra"),
                      ("x" * 20000,), ("/short/" + "x" * 20000,), ("x" * 20000 + ".tail", ".tail"), ("a/", "/"),
                      ("///a///",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("dirname", options=(Option("-z"), Option("--zero")),
            operands=(("/usr/bin/ls",), ("/usr/bin/",), ("usr",), ("",), ("/",), ("//",), ("///",), ("/a",),
                      ("//a//b///",), ("/a/b", "/c", "./d"), (), ("a b/c d",), ("--", "-dash"), ("x" * 20000 + "/tail",),
                      ("/" + "x" * 20000 + "/tail",), ("x" * 20000 + "/tail/",), ("a/b/",), ("./a",), ("../a/..",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("seq", options=(Option("-w"), Option("--equal-width"),
                            Option("-s", (",", "", "|", "::", "\n", " ", "x" * 17000), False),
                            Option("--separator", (",", ""), True),
                            Option("-f", ("%.2f", "%06.2f", "%-6.2f", "%+06.2f", "%% %.1f", "%.0f", "%#.0f", "%Lf",
                                          "%g", "%e", "%x", "%f-%f", "%d", "%", "%5.3g", "%a", "%'f", "%.f"), False),
                            Option("--format", ("%.2f", "%d"), True)),
            operands=(("5",), ("3", "9"), ("2", "3", "20"), ("-4", "4"), ("10", "-2", "1"), ("9", "2"), ("8", "11"),
                      ("-16", "-18", "-70"), ("1", "1"), ("1x",), ("x1",), ("",), ("+",), (".",),
                      ("9223372036854775807", "1", "9223372036854775807"),
                      ("-9223372036854775808", "-1", "-9223372036854775808"), ("0", ".1", ".3"),
                      ("1.00", ".25", "2.00"), ("1", "-.25", "0"), ("1e0", "2e-1", "1.6e0"), ("-0.0", ".1", ".2"),
                      ("0", ".1", ".29999999999999999"), ("0", ".1", ".30000000000000001"), ("-1", ".5", "1"),
                      ("-2.53", "-.52", "-15.19"), ("9.9", ".1", "10.05"), ("1", "0.0", "2"), ("1e", "2"),
                      ("1.2.3", "2"), ("-0", ".25", "1"), ("000", "1", "2"), ("+0.00", ".25", "1"), ("2", "-1", "-0"),
                      ("001e1", "1", "001e1"), ("123e-1", "1", "123e-1"), ("1", "50000"), ("25000", "-1", "-25000"),
                      ("-9000", "-3", "-70000"), ("00001", "50000"), ("-100.00", ".25", "100.00"),
                      ("-1.125", ".25", "1.125"), ("1", "2", "3", "4"), (), ("nan",), ("1", "nan"), ("1", "0", "3"),
                      ("0x10",), ("1", "3", "0x10"), ("1e3",), ("-.5", ".5"), ("3", "1"), ("1", "1", "1"),
                      ("0.1", "0.1", "0.5"), ("1", "2", "1"), ("--", "-3"), ("1", "-", "3"), ("1.5", "1", "1"),
                      ("-inf", "1", "-inf"),),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("readlink", options=(Option("-f"), Option("-e"), Option("-m"), Option("-n"), Option("-q"),
                                 Option("-s"), Option("-v"), Option("-z"), Option("--canonicalize"),
                                 Option("--canonicalize-existing"), Option("--canonicalize-missing"),
                                 Option("--no-newline"), Option("--quiet"), Option("--silent"),
                                 Option("--verbose"), Option("--zero")),
            operands=(("link",), ("a.txt",), ("dirlink",), ("dangling",), ("loop",), ("missing",), ("dir/sub/back",),
                      ("link/",), ("a.txt/",), ("a.txt/.",), ("a.txt/..",), ("missing/at/all",), ("link", "dirlink"),
                      ("dir/../a.txt",), ("dirlink/inside",), ("dirlink/..",), ("dir/sub/back/inside",), ("two words",),
                      (), ("",), ("/",), ("badwalk",), ("badwalk/",), ("shut/inside",), ("dir/sub/back/sub/back/sub",),
                      ("link", "missing", "dirlink"), ("loop/",), ("-",), ("/dev/null",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("realpath", options=(Option("-e"), Option("-m"), Option("-L"), Option("-P"), Option("-q"),
                                 Option("-s"), Option("-z"), Option("--canonicalize-existing"),
                                 Option("--canonicalize-missing"), Option("--logical"), Option("--physical"),
                                 Option("--quiet"), Option("--no-symlinks"), Option("--strip"), Option("--zero"),
                                 Option("--relative-to", ("dir", ".", "dir/sub", "missing", "", "/", "dirlink", "a.txt"), True),
                                 Option("--relative-base", (".", "dir", "/usr", "", "/", "deep/one"), True)),
            operands=(("a.txt",), ("link",), ("dir/../a.txt",), ("dir/sub",), ("/",), ("missing",), ("missing/at/all",),
                      ("dangling",), ("loop",), ("dirlink/..",), ("dir/sub/back/..",), ("dir/sub/back/inside",), ("link/",),
                      ("a.txt/",), ("a.txt/.",), ("a.txt//",), ("a.txt/..",), ("badwalk",), ("badwalk/",), ("two words",),
                      ("a.txt", "link", "missing", "dir"), (), ("",), ("dirlink",), ("dirlink/inside",), ("deep/one/two/three/leaf",),
                      ("shut/inside",), ("./././a.txt",), ("dir//sub//",), ("x" * 20000,), ("-",), ("dir/sub/back/sub/back/inside",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("pathchk", options=(Option("-p"), Option("-P"), Option("--portability")),
            operands=(("absent",), ("a.txt",), ("",), ("A-z_09.ok/path",), ("bad+name",), ("okay/-bad",),
                      ("okay/name",), ("okay/" + "x" * 14,), ("okay/" + "x" * 15,), ("missing/" + "x" * 255,),
                      ("missing/" + "x" * 256,), ("ordinary", "-P"), ("okay", "bad+name", "also"), (),
                      ("a\tb",), ("dir/inside",), ("/" + "x" * 5000,), ("a.txt/x",), ("dangling/x",), ("-lead",),
                      ("--", "-lead"), ("okay//double",), ("shut/inside",), ("x" * 300,), ("dir/" + "x" * 256,)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("id", options=(Option("-u"), Option("-g"), Option("-G"), Option("-n"), Option("-r"), Option("-z"),
                           Option("-Z"), Option("-a"), Option("--user"), Option("--group"), Option("--groups"),
                           Option("--name"), Option("--real"), Option("--zero"), Option("--context")),
            operands=((), ("root",), ("nosuchuser",), ("0",), (files_UID,), ("root", "root"), ("root", "nosuchuser"),
                      ("",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("groups", operands=((), ("root",), ("root", "root"), ("nosuchuser",), ("root", "-x"), (files_UID,), ("",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("whoami", operands=((), ("root",), ("root", "-x")), stdin=("empty",), fixture="files", stderr="exact"),
    Utility("logname", operands=((), ("root",), ("root", "-x")), stdin=("empty",), fixture="files", stderr="exact"),
    Utility("tty", options=(Option("-s"), Option("--silent"), Option("--quiet")),
            operands=((), ("extra",), ("extra", "-x")), stdin=("empty", "text"), fixture="files", stderr="exact"),
    Utility("nproc", options=(Option("--all"),
                              Option("--ignore", ("1", "+1", "999999999999999999999999", "0", "42",
                                                  "18446744073709551615", "18446744073709551616", "", "+", "-1",
                                                  "0x10", "  +1", "1 ", "1,2"), True)),
            operands=((), ("extra",), ("extra", "--bad")), stdin=("empty",), fixture="files", stderr="exact"),
    Utility("uname", options=(Option("-a"), Option("-s"), Option("-n"), Option("-r"), Option("-v"), Option("-m"),
                              Option("-p"), Option("-i"), Option("-o"), Option("--all"), Option("--kernel-name"),
                              Option("--nodename"), Option("--kernel-release"), Option("--kernel-version"),
                              Option("--machine"), Option("--processor"), Option("--hardware-platform"),
                              Option("--operating-system")),
            operands=((), ("extra",)), stdin=("empty",), fixture="files", stderr="exact"),
    Utility("nice", options=(Option("-n", ("1", "10", "19", "20", "-1", "nope", "9223372036854775807",
                                          "9223372036854775808", "18446744073709551616", "", "+5"), None),
                             Option("--adjustment", ("1", "-1", "x"), True), Option("--adj", ("1",), True)),
            operands=((), ("nice",), ("sh", "-c", "exit 7"), ("nosuchcommand",), ("/",), ("printf", "%s\\n", "-n"),
                      ("true",), ("./exe", "one", "two"), ("./unreadable",), ("-100", "sh", "-c", "printf ok"),
                      ("--100", "sh", "-c", "printf ok"), ("-+100", "sh", "-c", "printf ok"), ("-x",), ("--", "nice")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("find", options=(Option("-H"), Option("-L"), Option("-P"),
                             Option("-O", ("0", "1", "2", "3"), True)),
            operands=files_FIND_WALKED, stdin=("files_yes", "files_no"), fixture="files", stderr="exact",
            normalize=files_listing, extra=files_find_expressions(360) + files_FIND_BROKEN, max_flags=2),
    Utility("stat", options=(Option("-L"), Option("-f"), Option("-t"), Option("--dereference"),
                             Option("--file-system"), Option("--terse"),
                             Option("--cached", ("never", "always", "default", "bogus"), True),
                             Option("-c", files_STAT_FORMATS, False), Option("--format", ("%n %s", "%i"), True),
                             Option("--printf", ("%n|%s\\n", "%n", "\\n", "%a %A\\n"), True)),
            operands=(("a.txt",), ("dir",), ("link",), ("dangling",), ("missing",), ("empty",), ("exe",), ("two words",),
                      ("unreadable",), ("shut/inside",), ("a.txt", "dir", "link"), ("twin",), ("loop",), ("/dev/null",),
                      ("hollow",), ("-",), (), ("dirlink",), ("badwalk",), ("recent.txt",), ("a.txt", "missing", "b.txt"),
                      ("/",), ("dir/sub/back",), (".",), ("new\nline",)),
            stdin=("empty", "text"), fixture="files", stderr="exact", normalize=files_listing),
    Utility("ls", options=files_LS_OPTIONS, operands=files_LS_OPERANDS, stdin=("empty",), fixture="files",
            stderr="exact", normalize=files_listing, env=(("LS_COLORS", files_LS_COLORS),), max_flags=5,
            valid=files_ls_valid,
            extra=(("-la",), ("-lart",), ("-lisa",), ("-lhS", "dir"), ("-1R", "deep"), ("-dl", "dir", "dirlink", "link"),
                   ("-lL", "dirlink"), ("-lH", "dirlink"), ("--color=always", "-1"), ("--color=always", "-l"),
                   ("-l", "--time-style=full-iso"), ("-lu", "--time-style=+%s"), ("-lc",),
                   ("-C", "-w", "40"), ("-x", "-w", "40"), ("-m", "-w", "30"), ("-Q", "-1"), ("-b", "-1"), ("-N", "-1"),
                   ("--quoting-style=shell-escape", "-1"), ("--quoting-style=c", "-l"), ("-F", "-1"), ("-p", "-1"),
                   ("--file-type", "-1"), ("-R", "shut"), ("-R", "dir/sub/back"), ("-LR", "dir"), ("-ls",), ("-lS", "-r"),
                   ("-lX",), ("-lv",), ("-lU",), ("-f",), ("-lf",), ("-la", "--group-directories-first"),
                   ("-l", "--block-size=K"), ("-l", "--si", "-s"), ("-D", "-l"), ("--zero", "-1"), ("-g", "-o"),
                   ("-n", "-l"), ("--dired",), ("-I", "*.txt", "-a"), ("--hide=*.txt", "-A"), ("-B",),
                   ("--hyperlink=always", "-1"), ("-Z", "-l"), ("--author", "-l"), ("--full-time",),
                   ("-l", "--time-style=+%b %e\n%H:%M"), ("-l", "-T", "1", "-x"), ("-l", "unreadable", "shut"))),
    Utility("dir", options=(Option("-l"), Option("-1"), Option("-a"), Option("-A"), Option("-R"), Option("-d"),
                            Option("-n"), Option("-b"), Option("-Q"), Option("-N"), Option("-C"), Option("-x"),
                            Option("-m"), Option("-F"), Option("-i"), Option("-s"), Option("-t"), Option("-S"),
                            Option("-r"), Option("--color", ("never", "always"), True),
                            Option("-w", ("0", "40", "80"), None)),
            operands=((), ("a.txt",), ("dir",), ("missing",), ("new\nline",), ("esc\x1bape",), ("--", "-dash"),
                      ("dir", "a.txt"), ("unreadable",), ("dangling",)),
            stdin=("empty",), fixture="files", stderr="exact", normalize=files_listing,
            valid=files_ls_valid, env=(("LS_COLORS", files_LS_COLORS),)),
    Utility("vdir", options=(Option("-l"), Option("-1"), Option("-a"), Option("-A"), Option("-R"), Option("-d"),
                             Option("-n"), Option("-b"), Option("-Q"), Option("-N"), Option("-C"), Option("-x"),
                             Option("-m"), Option("-F"), Option("-i"), Option("-s"), Option("-t"), Option("-S"),
                             Option("-r"), Option("-h"), Option("--color", ("never", "always"), True)),
            operands=((), ("a.txt",), ("dir",), ("missing",), ("new\nline",), ("esc\x1bape",), ("--", "-dash"),
                      ("dir", "a.txt"), ("unreadable",), ("dangling",), ("link", "dirlink")),
            stdin=("empty",), fixture="files", stderr="exact", normalize=files_listing,
            valid=files_ls_valid, env=(("LS_COLORS", files_LS_COLORS),)),
    Utility("dircolors", options=(Option("-b"), Option("-c"), Option("-p"), Option("--sh"), Option("--bourne-shell"),
                                  Option("--csh"), Option("--c-shell"), Option("--print-database"),
                                  Option("--print-ls-colors")),
            operands=((), ("colors",), ("colors_quote",), ("colors_colon",), ("missing",), ("colors", "colors_quote"),
                      ("a.txt",), ("empty",), ("-",), ("dir",), ("unreadable",)),
            stdin=("files_colors", "empty"), fixture="files", stderr="exact",
            env=(("TERM", "xterm-256color"), ("COLORTERM", "no"), ("SHELL", "/bin/sh"))),
    Utility("du", options=(Option("-a"), Option("-s"), Option("-c"), Option("-h"), Option("-k"), Option("-m"),
                           Option("-b"), Option("-l"), Option("-L"), Option("-P"), Option("-x"), Option("-S"),
                           Option("-0"), Option("-D"), Option("-H"), Option("--all"), Option("--summarize"),
                           Option("--total"), Option("--human-readable"), Option("--si"), Option("--bytes"),
                           Option("--apparent-size"), Option("--count-links"), Option("--dereference"),
                           Option("--dereference-args"), Option("--no-dereference"), Option("--one-file-system"),
                           Option("--separate-dirs"), Option("--null"), Option("--inodes"), Option("--time"),
                           Option("--time", ("atime", "access", "use", "ctime", "status", "mtime", "bogus"), True),
                           Option("--time-style", ("full-iso", "long-iso", "iso", "+%Y-%m", "bogus"), True),
                           Option("-B", ("1", "512", "K", "M", "KB", "x"), None),
                           Option("--block-size", ("1", "K", "x"), True),
                           Option("-d", ("0", "1", "2", "nope", "-1", "+1", "99"), None),
                           Option("--max-depth", ("0", "1", "x"), True),
                           Option("--exclude", ("two", "*.txt", "dir", "*/sub", "a.txt", "*"), True),
                           Option("-X", ("paths", "missing", "empty"), None),
                           Option("--exclude-from", ("paths",), True),
                           Option("--files0-from", ("nulwords", "missing", "-", "empty"), True),
                           Option("-t", ("1", "-1", "4K", "+10K", "x", "0", "-4K"), None),
                           Option("--threshold", ("1", "-1"), True)),
            operands=((), ("dir",), ("a.txt",), ("dir", "a.txt", "link"), ("link",), ("dirlink",), ("dangling",),
                      ("missing",), ("shut",), ("twin", "b.txt"), ("deep",), ("two words",), ("loop",), ("dir/sub/back",),
                      (".",), ("hollow",), ("unreadable",), ("dup",), ("badwalk",), ("dirlink/",), ("missing", "a.txt")),
            stdin=("files_nul_words", "empty"), fixture="files", stderr="exact", normalize=files_when, max_flags=5,
            extra=(tuple("--exclude=never-match-%d" % n for n in range(40)) + (".",),) +
                  (("-L", "dir"), ("-L", "."), ("-L", "dir/sub/back"), ("-aL", "dup"), ("-D", "dirlink"), ("-H", "dirlink"),
                    ("-Lx", "."), ("-a", "--time", "."), ("-sh", "."), ("-sb", "."), ("-c", "dir", "dup"), ("-s", "-a", "dir"),
                    ("-d", "1", "-s", "dir"), ("-b", "-m", "a.txt"), ("-m", "-b", "a.txt"), ("-k", "-m", "a.txt"),
                   ("--inodes", "-a", "."), ("--apparent-size", "-a", "dir"), ("-l", "dup"), ("--files0-from=-",))),
    Utility("df", options=(Option("-a"), Option("-h"), Option("-H"), Option("-i"), Option("-k"), Option("-l"),
                           Option("-P"), Option("-T"), Option("--all"), Option("--human-readable"), Option("--si"),
                           Option("--inodes"), Option("--local"), Option("--portability"), Option("--print-type"),
                           Option("--total"), Option("--sync"), Option("--no-sync"), Option("--output"),
                           Option("-B", ("1", "K", "M", "512", "x"), None), Option("--block-size", ("1", "K"), True),
                           Option("-t", ("ext4", "tmpfs", "nosuch"), None), Option("-x", ("ext4", "tmpfs", "nosuch"), None),
                           Option("--type", ("ext4",), True), Option("--exclude-type", ("tmpfs",), True),
                           Option("--output", ("source,target", "size,used,avail,pcent", "itotal,iused,iavail,ipcent",
                                               "fstype,file", "target", "bogus", "source,size,size",
                                               "source,fstype,itotal,iused,iavail,ipcent,size,used,avail,pcent,file,target"), True)),
            operands=((), ("/",), (".",), ("a.txt",), ("missing",), ("/", "."), ("/dev/null",), ("a.txt", "missing"),
                      ("/proc",), ("dir", "a.txt"), ("dangling",), ("link",), ("shut/inside",), ("/dev",), ("/tmp",),
                      ("/", "/", ".")),
            stdin=("empty",), fixture="files", stderr="exact", normalize=files_normal(files_hide_digits)),
    Utility("env", options=(Option("-i"), Option("-0"), Option("-v"), Option("--ignore-environment"), Option("--null"),
                            Option("--debug"), Option("--list-signal-handling"),
                            Option("-u", ("HOME", "PATH", "NOPE", "A", ""), None), Option("--unset", ("HOME", "A"), True),
                            Option("-C", ("dir", "/", "missing", "a.txt", "dirlink", "shut"), None),
                            Option("--chdir", ("dir", "missing"), True),
                            Option("-S", ("/bin/echo one two", "echo -n a", "", "'quoted arg' b", "$PATH", "echo ${HOME}",
                                          "echo \\c\\t", "echo # comment", "echo \"a b\" c", "echo \\$x", "echo a\\ b"), None),
                            Option("--split-string", ("/bin/echo three four", "echo x"), True),
                            Option("-a", ("zero", "", "sh"), None), Option("--argv0", ("zero",), True),
                            Option("--default-signal"), Option("--default-signal", ("PIPE", "INT,TERM", "13", "bogus", ""), True),
                            Option("--ignore-signal"), Option("--ignore-signal", ("PIPE", "INT", "bogus"), True),
                            Option("--block-signal"), Option("--block-signal", ("PIPE", "bogus"), True)),
            operands=((), ("A=1", "B=2"), ("A=1", "sh", "-c", "echo $A"), ("sh", "-c", "echo [$PATH]"),
                      ("PATH=/bin:/usr/bin", "echo", "through"), ("PATH=", "echo", "x"), ("-", "A=1"), ("sh", "-c", "echo $0"),
                      ("nosuchcommand",), ("/",), ("./unreadable",), ("./exe", "arg one", "two"), ("true",),
                      ("sh", "-c", "exit 7"), ("A=1", "A=2"), ("=x",), ("A=1", "-", "B=2"),
                      ("sh", "-c", "trap 'echo caught' INT; kill -INT $$; echo alive"), ("sh", "-c", "pwd"), ("./exe",), ("dir",), ("A=1", "sh", "-c", "echo $A $B"),
                      ("printf", "%s\\n", "-n", "--", "x"), ("--", "sh", "-c", "echo dashed"), ("a b=c", "sh", "-c", "env | grep -c ^a"),
                      ("A==1", "sh", "-c", "echo $A"), ("PATH=/nowhere", "echo", "x")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("printenv", options=(Option("-0"), Option("--null")),
            operands=((), ("PATH",), ("PATH", "HOME"), ("NOPE",), ("PATH", "NOPE"), ("PATH=anything",), ("PATH", "-0"),
                      ("",), ("HOME",), ("TZ", "LC_ALL", "LANG"), ("--bad",), ("COLUMNS", "LINES", "TERM")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("chown", options=(Option("-c"), Option("-f"), Option("-v"), Option("-h"), Option("-R"), Option("-H"),
                              Option("-L"), Option("-P"), Option("--changes"), Option("--silent"), Option("--quiet"),
                              Option("--verbose"), Option("--no-dereference"), Option("--dereference"),
                              Option("--recursive"), Option("--preserve-root"), Option("--no-preserve-root"),
                              Option("--from", (files_UID, "root", ":" + files_GID, "nosuch", files_UID + ":" + files_GID, ":"), True),
                              Option("--reference", ("b.txt", "missing", "link", "dir", "dangling", "shut/inside"), True)),
            operands=files_UID_OPERANDS, stdin=("empty",), fixture="files", stderr="exact"),
    Utility("chgrp", options=(Option("-c"), Option("-f"), Option("-v"), Option("-h"), Option("-R"), Option("-H"),
                              Option("-L"), Option("-P"), Option("--changes"), Option("--silent"), Option("--quiet"),
                              Option("--verbose"), Option("--no-dereference"), Option("--dereference"),
                              Option("--recursive"), Option("--preserve-root"), Option("--no-preserve-root"),
                              Option("--reference", ("b.txt", "missing", "link", "dir", "dangling", "shut/inside"), True)),
            operands=files_GID_OPERANDS, stdin=("empty",), fixture="files", stderr="exact"),
    Utility("chmod", options=(Option("-c"), Option("-f"), Option("-v"), Option("-R"), Option("--changes"),
                              Option("--silent"), Option("--quiet"), Option("--verbose"), Option("--recursive"),
                              Option("--preserve-root"), Option("--no-preserve-root"),
                              Option("--reference", ("b.txt", "missing", "link", "dir", "exe", "dangling", "shut/inside"), True)),
            operands=files_CHMOD_OPERANDS, stdin=("empty",), fixture="files", stderr="exact"),
    Utility("ln", options=(Option("-s"), Option("-f"), Option("-i"), Option("-n"), Option("-r"), Option("-v"),
                           Option("-T"), Option("-L"), Option("-P"), Option("-b"), Option("--symbolic"),
                           Option("--force"), Option("--interactive"), Option("--no-dereference"), Option("--relative"),
                           Option("--verbose"), Option("--no-target-directory"), Option("--logical"), Option("--physical"),
                           Option("--backup"), Option("--backup", ("numbered", "simple", "none", "existing", "nil", "t", "bogus"), True),
                           Option("-S", ("~", ".bak", ""), None), Option("--suffix", (".orig",), True),
                           Option("-t", ("dir", "missing", "a.txt", "dir/sub", "dirlink", "hollow"), None),
                           Option("--target-directory", ("dir", "missing"), True)),
            operands=(("a.txt", "pointer"), ("b.txt", "link"), ("dir", "pointer"), ("a.txt", "dir"), ("a.txt", "dir/"),
                      ("missing", "pointer"), ("a.txt", "b.txt", "dir"), ("a.txt", "a.txt"), ("a.txt", "twin"),
                      ("b.txt", "twin"), ("link", "pointer"), ("dangling", "pointer"), ("../x", "pointer"), ("a.txt",),
                      (), ("a.txt", "b.txt", "missing"), ("dir/inside", "dir/sub/rel"), ("a.txt", "dirlink"),
                      ("a.txt", "dirlink/"), ("nowhere", "dangling"), ("two words", "sp ace"), ("a.txt", "shut/x"),
                      ("a.txt", "new\nline"), ("dir/sub/deep", "deep/one/rel"), ("deep/one/two/three/leaf", "up"),
                      ("a.txt", "hollow"), ("dir", "dir/sub/self"), ("link",), ("dangling",), ("a.txt", "b.txt"),
                      ("a.txt", "dir/inside"), ("/dev/null", "devnull"), ("a.txt", "-dash")),
            stdin=("files_yes", "files_no"), fixture="files", stderr="exact",
            extra=(("-sr", "/" + "/".join(["b"] * 1300), "/" + "/".join(["a"] * 1300) + "/link"),
                   ("-sr", "dir/inside", "dirlink/rel"), ("-srf", "dir/inside", "dirlink/rel"),
                   ("-sr", "a.txt", "deep/one/two/three/rel"), ("-sfn", "a.txt", "dirlink"), ("-sf", "a.txt", "dirlink"),
                   ("-si", "b.txt", "link"), ("-f", "-i", "-s", "b.txt", "link"), ("-i", "-f", "-s", "b.txt", "link"),
                   ("-L", "link", "followed"), ("-P", "link", "kept"), ("-st", "dir", "a.txt"), ("-sT", "a.txt", "dir"),
                   ("-f", "missing", "a.txt"), ("-f", "a.txt", "a.txt"), ("-f", "b.txt", "a.txt"), ("-W", "a.txt", "pointer"))),
    Utility("link", operands=(("a.txt", "hard"), ("link", "hard"), ("a.txt", "-W"), ("--", "-dash", "hard"), ("missing", "hard"),
                              ("dir", "hard"), ("a.txt", "b.txt"), ("a.txt",), (), ("one", "two", "three"), ("a.txt", "dir"),
                              ("a.txt", "dir/"), ("dangling", "hard"), ("a.txt", "shut/hard"), ("two words", "sp ace"),
                              ("a.txt", "twin"), ("a.txt", "new\nline")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("unlink", operands=(("a.txt",), ("link",), ("dir",), ("dangling",), ("missing",), ("--", "-dash"), (), ("a.txt", "b.txt"),
                                ("dirlink",), ("shut",), ("two words",), ("shut/inside",), ("twin",), ("hollow",), ("dir/",),
                                ("a.txt/",), (".",), ("loop",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("mkdir", options=(Option("-p"), Option("-v"), Option("-Z"), Option("--parents"), Option("--verbose"),
                              Option("--context"), Option("--context", ("x",), True),
                              Option("-m", ("0700", "0705", "755", "u=rwx,go=", "=rwx", "+w", "a+X", "x", "1777", "2755",
                                            "u+s", "o+t", "-w", "", "0"), None),
                              Option("--mode", ("0700", "u=rwx"), True)),
            operands=(("made",), ("a/b/c/d",), ("dir/sub/deep",), ("dir",), ("a.txt",), ("a.txt/child",), ("made1", "made2"),
                      (), ("dir/new",), ("shut/new",), ("dangling",), ("dangling/x",), ("dirlink/new",), ("two new",),
                      ("x" * 300,), ("deep/one/two/three/four/five",), ("./made/",), ("made/",), ("link",), ("link/x",),
                      ("hollow",), ("made", "made"), ("", ), ("-dash",), ("--", "-dash"), ("new\nline/x",), ("x" * 20000,)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("rmdir", options=(Option("-p"), Option("-v"), Option("--parents"), Option("--verbose"),
                              Option("--ignore-fail-on-non-empty")),
            operands=(("hollow",), ("dir",), ("a.txt",), ("missing",), ("nest/a/b",), ("dirlink",), ("hollow", "dir"), ("shut",),
                      (), ("hollow/",), ("dir/sub",), ("nest/a/b/",), ("nest",), ("dangling",), ("nest/a/b", "hollow"),
                      ("deep/one/two/three",), ("./hollow",), ("hollow/.",), ("dirlink/",), ("hollow", "missing")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("mkfifo", options=(Option("-Z"), Option("--context"),
                               Option("-m", ("0620", "u=rw,g=r,o=", "1777", "6777", "a+t", "u+s", "x", "=rwx", "u=rwx,g+r", "0"), None),
                               Option("--mode", ("0600",), True)),
            operands=(("pipe",), ("one", "two", "three"), ("a.txt",), ("dir",), ("missing/pipe",), (), ("one", "-m", "0600", "two"),
                      ("dangling",), ("shut/pipe",), ("two words",), ("-dash",), ("--", "-dash2"), ("hollow/pipe",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("mknod", options=(Option("-Z"), Option("-m", ("0620", "0600", "1777", "x", "u=rw"), None), Option("--mode", ("0600",), True)),
            operands=(("pipe", "p"), ("pipe", "potato"), ("block", "b", "1", "7"), ("char", "c", "1", "3"), ("char", "u", "0x1", "0x3"),
                      ("char", "c", "01", "03"), ("char", "c", "1", "256"), ("block", "b", "4095", "1048575"), ("node", "b", "4096", "0"),
                      ("node", "c", "1", "1048576"), ("node", "c"), ("node", "p", "1", "2"), ("node", "x"), ("node", "c", "0x", "1"),
                      ("node", "c", "1", "08"), ("node", "c", "-1", "2"), ("node", "c", "1", "4294967296"), (), ("node",),
                      ("a.txt", "p"), ("pipe", "-m", "0600", "p"), ("missing/pipe", "p"), ("dir", "p"), ("node", "b", "1"),
                      ("node", "p", "1")),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("sync", options=(Option("-d"), Option("-f"), Option("--data"), Option("--file-system")),
            operands=((), ("a.txt",), ("missing",), ("dir",), ("a.txt", "b.txt"), ("dangling",), ("unreadable",), ("link",),
                      ("shut/inside",), ("two words",), ("/dev/null",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("touch", options=(Option("-a"), Option("-m"), Option("-c"), Option("-h"), Option("-f"), Option("--no-create"),
                              Option("--no-dereference"),
                              Option("-r", ("a.txt", "b.txt", "missing", "link", "dangling", "dir", "recent.txt"), None),
                              Option("--reference", ("b.txt",), True),
                              Option("-d", files_TOUCH_DATES, False), Option("--date", ("@5", "2001-09-09"), True),
                              Option("-t", files_TOUCH_STAMPS, False),
                              Option("--time", ("access", "atime", "use", "modify", "mtime", "furlongs", ""), True)),
            operands=(("a.txt",), ("fresh",), ("link",), ("dangling",), ("dir",), ("a.txt", "b.txt", "fresh"), ("shut/inside",),
                      ("unreadable",), ("two words",), (), ("--", "-dash"), ("dirlink",), ("-",), ("loop",), ("missing/x",),
                      ("new\nline",), ("hollow",), ("twin",), ("a.txt", "a.txt"), ("x" * 300,)),
            stdin=("empty",), fixture="files", stderr="exact",
            extra=tuple(("-d", when, "fresh") for when in files_TOUCH_DATES) +
                  tuple(("-t", when, "fresh") for when in files_TOUCH_STAMPS) +
                  (("-d", "@1000000000", "-r", "b.txt", "a.txt"), ("-r", "b.txt", "-d", "1 second", "a.txt"),
                   ("-a", "-r", "b.txt", "-d", "1 second", "a.txt"), ("-m", "-r", "b.txt", "-d", "-1 day", "a.txt"),
                   ("-r", "b.txt", "-d", "@-1.5", "a.txt"), ("-a", "-r", "b.txt", "link"), ("-h", "-r", "b.txt", "link"),
                   ("-m", "-r", "b.txt", "link"), ("-c", "-t", "202602310000", "missing"), ("-t", "202602310000", "missing"),
                   ("-d", "2001-09-09 01:46:40.123456789", "fresh"), ("-d", "@1000000000.25", "fresh"),
                   ("--time=access", "-d", "@5", "a.txt"), ("--time=modify", "-d", "@5", "a.txt"), ("-am", "-d", "@5", "a.txt"),
                   ("-h", "-d", "@5", "dangling"), ("-c", "dangling"), ("dangling",), ("-h", "dangling"))),
    Utility("truncate", options=(Option("-c"), Option("-o"), Option("--no-create"), Option("--io-blocks"),
                                 Option("-s", ("4", "+4", "-2", "<3", ">9", "/4", "%4", "2KB", "2KiB", "K", "0", "1B", "1p", "/0",
                                               "2M", "-1", "x", "", "+0", "%0", "<0", ">0", "1k", "1kB", "1KiB", "2R", "1Y"), None),
                                 Option("--size", ("4", "+1"), True),
                                 Option("-r", ("b.txt", "/dev/null", "missing", "empty", "link", "dangling", "unreadable", "binary"), None),
                                 Option("--reference", ("b.txt",), True)),
            operands=(("a.txt",), ("made",), ("a.txt", "b.txt", "made"), ("dir",), ("link",), ("dangling",), ("missing/x",),
                      ("unreadable",), (), ("two words",), ("a.txt", "-s", "8"), ("shut/inside",), ("twin",), ("hollow",),
                      ("-",), ("empty",), ("binary",)),
            stdin=("empty",), fixture="files", stderr="exact", valid=files_truncate_valid),
    Utility("shred", options=(Option("-f"), Option("-u"), Option("-v"), Option("-x"), Option("-z"), Option("--force"),
                              Option("--verbose"), Option("--exact"), Option("--zero"), Option("--remove"),
                              Option("--remove", ("unlink", "wipe", "wipesync", "bogus"), True),
                              Option("-n", ("0", "1", "2", "nope", "-1"), None), Option("--iterations", ("0", "1"), True),
                              Option("-s", ("0", "2", "17", "4K", "nope", "1"), None), Option("--size", ("3",), True),
                              Option("--random-source", ("a.txt", "/dev/zero", "missing"), True)),
            operands=(("a.txt",), ("empty",), ("link",), ("dir",), ("missing",), ("a.txt", "b.txt"), ("unreadable",), ("dangling",),
                      ("two words",), (), ("exe",), ("twin",), ("binary",), ("many",), ("shut/inside",), ("hollow",)),
            stdin=("empty",), fixture="files", stderr="exact", valid=files_shred_valid,
            extra=(("-n0", "-x", "a.txt"), ("-n0", "-zx", "a.txt"), ("-n0", "-z", "a.txt"), ("-n0", "-z", "-s17", "a.txt"),
                   ("-n0", "-z", "-s2", "a.txt"), ("-n1", "-s0", "a.txt"), ("-n2", "-zx", "a.txt"), ("-n1", "-zx", "a.txt", "b.txt"),
                   ("-n0", "--remove=unlink", "a.txt"), ("-n0", "-u", "a.txt"), ("-n1", "-vzx", "a.txt"), ("-n0", "-zx", "link"),
                   ("-f", "-n0", "-x", "unreadable"), ("-n0", "dir"), ("a.txt", "-n0", "-zx"), ("-n0", "--remove=wipe", "a.txt"),
                   ("-n0", "--random-source=a.txt", "a.txt"), ("-n0",), ("-n", "nope", "a.txt"), ("-s", "nope", "a.txt"),
                   ("-u", "-n0", "empty"), ("-u", "-n0", "hollow"), ("-zu", "-n0", "twin"), ("-n3", "-z", "a.txt"), ("-z", "a.txt"),
                   ("-u", "a.txt"), ("--remove=wipesync", "-z", "a.txt"), ("-v", "-z", "-n1", "a.txt"))),
    Utility("shuf", options=(Option("-e"), Option("-z"), Option("--echo"), Option("--zero-terminated"),
                             Option("-i", ("3-9", "7-7", "9-3", "1-3", "x", "0-0", "-1-3", "5-", "1-1000"), None),
                             Option("--input-range", ("2-4",), True),
                             Option("-n", ("0", "9999", "nope", "-1", ""), None), Option("--head-count", ("0", "9999"), True),
                             Option("--random-source", ("a.txt", "/dev/zero", "missing"), True)),
            operands=(("c.txt",), ("empty",), ("-",), ("missing",), ("a.txt", "b.txt"), ("dir",), (), ("nonl",), ("binary",),
                      ("unreadable",), ("many",), ("two words",), ("--", "-dash")),
            stdin=("text", "empty", "nonl", "many_lines", "nul", "blanks"), fixture="files", stderr="exact",
            normalize=files_stdout_sorted,
            extra=(("-r", "-n20", "-e", "only"), ("-r", "-n0", "-e", "a", "b"), ("-r", "-n1", "empty"), ("-r", "-n3", "-i", "4-4"),
                   ("-n", "0", "-o", "out", "c.txt"), ("-o", "out", "empty"), ("-o", "out", "-e"), ("-e", "-o", "out", "only"),
                   ("-i", "7-7", "-o", "out"), ("-o", "missing/out", "c.txt"), ("-e", "--", "-c", "-a"),
                   ("-e",), ("-i", "1-3", "a.txt"), ("-e", "-i", "1-3", "a"), ("--repeat", "-n", "5", "-e", "x"),
                   ("-o", "out", "-n0", "-i", "1-9"), ("-r", "-e"), ("-zr", "-n2", "-e", "z"))),
    Utility("split", options=(Option("-d"), Option("-x"), Option("-e"), Option("-u"), Option("--verbose"),
                              Option("--elide-empty-files"), Option("--unbuffered"), Option("--numeric-suffixes"),
                              Option("--numeric-suffixes", ("7", "0", "x"), True), Option("--hex-suffixes"),
                              Option("--hex-suffixes", ("a", "0"), True),
                              Option("-a", ("1", "2", "3", "0", "x"), None), Option("--suffix-length", ("2",), True),
                              Option("-b", ("4", "2K", "1M", "0", "1B", "x", "3", "1000", "2k", "1KB", "10", "1"), None),
                              Option("--bytes", ("5",), True),
                              Option("-C", ("4", "7", "1", "3", "0", "x", "100"), None), Option("--line-bytes", ("6",), True),
                              Option("-l", ("1", "3", "0", "x", "173", "2", "7"), None), Option("--lines", ("2",), True),
                              Option("-n", ("3", "1", "0", "l/2", "r/2", "2/3", "x", "l/1/3", "r/1/3", "1/1", "4/3", "5"), None),
                              Option("--number", ("2",), True),
                              Option("-t", ("\\0", ",", "x", "ab", "\\n", ""), None), Option("--separator", (";",), True),
                              Option("--additional-suffix", (".part", "", "/bad"), True),
                              Option("--filter", ("cat > $FILE.filtered", "wc -c", "false", "cat"), True),
                              Option("-2"), Option("-1")),
            operands=(("c.txt",), ("c.txt", "part-"), ("-", "stdin-"), ("empty", "e-"), ("binary", "bin-"), ("nonl", "n-"),
                      ("missing",), ("dir",), (), ("a.txt", "a.txt"), ("nulwords", "nul-"), ("many", "m-"), ("c.txt", "shut/x-"),
                      ("two words", "sp-"), ("repeats", "r-"), ("c.txt", "part-", "extra"), ("unreadable",), ("words", "w-")),
            stdin=("text", "nul_lines", "edge_65537", "many_lines", "empty", "nonl"), fixture="files", stderr="exact",
            extra=(("-b2", "-2", "-a3", "c.txt", "old-"), ("-2", "-b2", "-a3", "c.txt", "old-"), ("-d", "-a3", "-b3", "-", "numbered-"),
                   ("--numeric-suffixes=7", "-b3", "-", "numbered-"), ("-d7", "-b3", "-", "numbered-"), ("-x", "-b3", "-", "hex-"),
                   ("-t", "\\0", "-l2", "nulwords", "nul-"), ("-C7", "-t", "\\0", "nulwords", "cap-"), ("-a1", "-b1", "binary", "short-"),
                   ("-n3", "c.txt", "dist-"), ("--number=3", "empty", "empty-"), ("-n3", "-", "pipe-"), ("-l173", "many", "part-"),
                   ("-n", "l/2", "a.txt"), ("-n", "r/2", "a.txt"), ("-b0", "a.txt"), ("-l0", "a.txt"), ("-b3", "-l2", "c.txt"),
                   ("-l2", "-b3", "c.txt"), ("-e", "-n", "9", "c.txt", "e-"), ("-d", "-a1", "-l1", "many", "over-"),
                   ("--filter=cat > $FILE.out", "-l2", "c.txt", "f-"), ("--filter=wc -c", "-b3", "binary"), ("-b3", "c.txt", "hit-"),
                   ("--additional-suffix=.part", "-b3", "-", "extra-"), ("-u", "-l1", "-", "u-"), ("--verbose", "-l1", "c.txt"))),
    Utility("csplit", options=(Option("-k"), Option("-s"), Option("-z"), Option("--keep-files"), Option("--quiet"), Option("--silent"),
                               Option("--elide-empty-files"), Option("--suppress-matched"),
                               Option("-b", ("%03d", "%d", "%x", "%02d.txt", "bogus%", "%s", "%d%d", "%"), None),
                               Option("--suffix-format", ("%02x",), True),
                               Option("-f", ("part-", "same", "", "shut/x", "dir/p"), None), Option("--prefix", ("pre",), True),
                               Option("-n", ("1", "3", "5", "x", "0"), None), Option("--digits", ("4",), True)),
            operands=(("c.txt", "2"), ("c.txt", "2", "3"), ("many", "1000", "{2}"), ("c.txt", "/9/"), ("c.txt", "/9/+1"), ("c.txt", "/9/-1"),
                      ("repeats", "/cherry/", "{*}"), ("repeats", "%cherry%"), ("repeats", "/apple/", "{1}"), ("c.txt", "99"),
                      ("c.txt", "/nomatch/"), ("c.txt", "{2}"), ("c.txt", "/[/"), ("-", "2"), ("c.txt",), (), ("missing", "2"),
                      ("c.txt", "0"), ("c.txt", "1", "1"), ("repeats", "/a/", "/c/"), ("c.txt", "2", "{9}"), ("empty", "1"),
                      ("repeats", "/an/+1", "{*}"), ("repeats", "%apple%", "/cherry/"), ("c.txt", "/9/", "{2}"), ("c.txt", "3", "2"),
                      ("c.txt", "/^1/"), ("repeats", "/A/"), ("many", "500", "{*}"), ("c.txt", "/9/+9"), ("c.txt", "x"),
                      ("c.txt", "/9/x"), ("nonl", "1"), ("binary", "1"), ("dir", "1"), ("unreadable", "1"), ("c.txt", "2", "%3%")),
            stdin=("text", "repeats", "many_lines", "empty", "nonl"), fixture="files", stderr="exact",
            extra=(("-f", "same", "-n", "2", "c.txt", "3"), ("--suppress-matched", "c.txt", "/9/+1"), ("-b", "%03d", "a.txt", "1"),
                   ("--suppress-matched", "many", "/5/", "/10/"), ("-k", "c.txt", "/9/", "{2}"), ("-k", "c.txt", "99"),
                   ("-z", "c.txt", "1", "3"), ("-s", "c.txt", "2", "3"), ("-f", "part-", "-n3", "c.txt", "2", "3"))),
    Utility("cp", options=(Option("-a"), Option("-b"), Option("-d"), Option("-f"), Option("-H"), Option("-i"), Option("-l"),
                           Option("-L"), Option("-n"), Option("-p"), Option("-P"), Option("-r"), Option("-R"), Option("-s"),
                           Option("-T"), Option("-u"), Option("-v"), Option("-x"), Option("--archive"), Option("--backup"),
                           Option("--backup", ("numbered", "simple", "none", "existing", "nil", "t", "bogus"), True),
                           Option("--attributes-only"), Option("--copy-contents"), Option("--dereference"), Option("--force"),
                           Option("--interactive"), Option("--link"), Option("--no-clobber"), Option("--no-dereference"),
                           Option("--no-preserve", ("mode", "ownership", "timestamps", "links", "all", "bogus"), True),
                           Option("--one-file-system"), Option("--parents"), Option("--preserve"),
                           Option("--preserve", ("mode", "ownership", "timestamps", "links", "context", "xattr", "all", "bogus",
                                                 "mode,timestamps"), True),
                           Option("--recursive"), Option("--reflink"), Option("--reflink", ("always", "auto", "never", "bogus"), True),
                           Option("--remove-destination"), Option("--sparse", ("always", "auto", "never", "bogus"), True),
                           Option("--strip-trailing-slashes"), Option("--symbolic-link"), Option("-S", ("~", ".bak", ""), None),
                           Option("--suffix", (".orig",), True), Option("-t", ("dir", "missing", "a.txt", "dir/sub", "dirlink", "hollow"), None),
                           Option("--target-directory", ("dir", "hollow"), True), Option("--no-target-directory"), Option("--update"),
                           Option("--update", ("all", "none", "none-fail", "older", "bogus"), True), Option("--verbose"), Option("-Z")),
            operands=(("a.txt", "copy"), ("a.txt", "b.txt"), ("a.txt", "dir"), ("a.txt", "dir/"), ("dir", "copied"), ("a.txt", "b.txt", "dir"),
                      ("link", "copy"), ("dangling", "copy"), ("dirlink", "copy"), ("a.txt", "a.txt"), ("b.txt", "twin"), ("missing", "copy"),
                      ("missing", "a.txt", "dir"), ("a.txt",), (), ("dir", "a.txt"), ("dir/.", "hollow"), ("a.txt", "missing/copy"),
                      ("unreadable", "copy"), ("shut", "copied"), ("two words", "sp ace"), ("exe", "copy"), ("empty", "copy"),
                      ("dir/sub/deep", "copy"), ("a.txt", "link"), ("a.txt", "dangling"), ("a.txt", "dirlink"), ("dir", "dirlink"),
                      ("loop", "copy"), ("deep", "copied"), ("a.txt", "b.txt", "missing"),
                      ("-", "copy"), ("a.txt", "shut/copy"), ("dir/", "copied"), ("dir", "hollow"), ("dup", "copied"), ("a.txt", "recent.txt"),
                      ("recent.txt", "a.txt"), ("binary", "copy"), ("new\nline", "copy"), ("dir/sub/back", "copy"), ("dir", "deep"),
                      ("a.txt", "b.txt", "c.txt", "hollow"), ("dir/inside", "dir/sub"), ("many", "copy"), ("a.txt", "twin")),
            stdin=("files_yes", "files_no"), fixture="files", stderr="exact", max_flags=4,
            extra=(("-rL", "dir", "copied"), ("-rL", ".", "copied"), ("-rL", "dir/sub/back", "copied"), ("-rp", "dir", "kept"), ("-a", "dir", "arch"),
                   ("-a", "link", "arch"), ("-rl", "dir", "linked"), ("-rs", "dir", "linked"), ("-r", "dir", "dirlink/x"), ("-r", "dir", "dir/sub/x"),
                   ("-P", "-H", "link", "kept"), ("-H", "-P", "link", "kept"), ("-n", "-i", "a.txt", "b.txt"), ("-i", "-n", "a.txt", "b.txt"),
                   ("-rv", "dir", "copied"), ("-T", "-t", "dir", "a.txt"), ("-f", "a.txt", "missing/copy"), ("-l", "missing", "a.txt", "dir"),
                   ("-rT", "dir/sub", "aimed"), ("-u", "a.txt", "b.txt"), ("-u", "b.txt", "a.txt"), ("-r", "unreadable", "dir", "copied"),
                   ("--parents", "dir/inside", "hollow"), ("--parents", "dir/sub/deep", "hollow"), ("--parents", "-r", "dir/sub", "hollow"),
                   ("-r", "dir/.", "hollow"), ("-r", "loop", "copied"), ("-rL", "loop", "copied"), ("-r", "/dev/null", "made"),
                   ("--attributes-only", "a.txt", "copy"), ("--attributes-only", "a.txt", "b.txt"), ("-b", "a.txt", "b.txt"),
                   ("-b", "-S", ".bak", "a.txt", "b.txt"), ("--backup=numbered", "a.txt", "b.txt"), ("--backup=numbered", "a.txt", "b.txt~"),
                   ("--remove-destination", "a.txt", "link"), ("-f", "a.txt", "link"), ("-d", "link", "kept"), ("-r", "dir", "copied", "extra"))),
    Utility("install", options=(Option("-b"), Option("-c"), Option("-C"), Option("-d"), Option("-D"), Option("-p"), Option("-s"),
                                Option("-T"), Option("-v"), Option("-Z"), Option("--backup"),
                                Option("--backup", ("numbered", "simple", "none", "bogus"), True), Option("--compare"),
                                Option("--directory"), Option("--preserve-timestamps"), Option("--strip"), Option("--verbose"),
                                Option("--no-target-directory"), Option("--preserve-context"), Option("--context"),
                                Option("--context", ("x",), True), Option("--debug"),
                                Option("-g", (files_GID, "root", "nosuch", ""), None), Option("--group", (files_GID,), True),
                                Option("-o", (files_UID, "root", "nosuch", ""), None), Option("--owner", (files_UID,), True),
                                Option("-m", ("0640", "u=rw,go=r", "755", "x", "4755", "0", "a+X", "=rwx", "1777"), None),
                                Option("--mode", ("0600",), True), Option("-S", ("~", ".bak"), None), Option("--suffix", (".orig",), True),
                                Option("--strip-program", ("true", "false", "missing-strip"), True),
                                Option("-t", ("dir", "missing", "a.txt", "hollow", "dirlink"), None),
                                Option("--target-directory", ("dir",), True)),
            operands=(("a.txt", "made"), ("a.txt", "dir"), ("a.txt", "dir/"), ("a.txt", "b.txt", "dir"), ("a.txt", "new/deep/made"),
                      ("new/deep/made",), ("a.txt", "a.txt"), ("missing", "made"), ("a.txt",), (), ("dir", "made"), ("link", "made"),
                      ("exe", "made"), ("a.txt", "link"), ("a.txt", "dangling"), ("a.txt", "twin"), ("unreadable", "made"),
                      ("a.txt", "shut/made"), ("two words", "made"), ("empty", "made"), ("a.txt", "made1", "made2", "dir"),
                      ("a.txt", "b.txt"), ("a.txt", "hollow"), ("a.txt", "dirlink"), ("dir", "dir2"), ("a.txt", "dir/sub/deep"),
                      ("binary", "made"), ("a.txt", "-dash")),
            stdin=("empty",), fixture="files", stderr="exact",
            extra=(("-d", "new/deep/made"), ("-d", "-m", "0710", "new/deep/made"), ("-d", "dir"), ("-d", "a.txt"), ("-D", "a.txt", "new/deep/made"),
                   ("-D", "-t", "new/deep", "a.txt"), ("-p", "a.txt", "made"), ("-v", "a.txt", "made"), ("-m", "0640", "a.txt", "made"),
                   ("-m", "u=rw,go=r", "a.txt", "made"), ("-C", "a.txt", "made"), ("-C", "a.txt", "b.txt"), ("-b", "a.txt", "b.txt"),
                   ("-s", "a.txt", "made"), ("-s", "--strip-program=true", "a.txt", "made"), ("-T", "a.txt", "made"), ("-T", "a.txt", "dir"),
                   ("-d", "new", "new/a", "dir/x"), ("-t", "dir", "a.txt", "b.txt"), ("-d", "-v", "new/x"), ("-p", "-m", "600", "a.txt", "b.txt"))),
    Utility("mv", options=(Option("-b"), Option("-f"), Option("-i"), Option("-n"), Option("-u"), Option("-v"), Option("-T"),
                           Option("--backup"), Option("--backup", ("numbered", "simple", "none", "existing", "bogus"), True),
                           Option("--force"), Option("--interactive"), Option("--no-clobber"), Option("--update"),
                           Option("--update", ("all", "none", "none-fail", "older", "bogus"), True), Option("--verbose"),
                           Option("--strip-trailing-slashes"), Option("--no-target-directory"), Option("--exchange"),
                           Option("--no-copy"), Option("--debug"), Option("-S", ("~", ".bak", ""), None), Option("--suffix", (".orig",), True),
                           Option("-t", ("dir", "missing", "a.txt", "hollow", "dirlink", "dir/sub"), None),
                           Option("--target-directory", ("dir", "hollow"), True)),
            operands=(("a.txt", "renamed"), ("a.txt", "dir"), ("a.txt", "dir/"), ("dir", "moved"), ("a.txt", "b.txt"), ("a.txt", "b.txt", "dir"),
                      ("a.txt", "a.txt"), ("b.txt", "twin"), ("link", "renamed"), ("dangling", "renamed"), ("dirlink", "renamed"),
                      ("dir", "dir/sub/x"), ("dir", "hollow"), ("dir", "a.txt"), ("a.txt", "dir/sub"), ("missing", "renamed"), ("a.txt",), (),
                      ("dir", "dirlink"), ("dir", "dirlink/"), ("a.txt", "shut/x"), ("shut", "moved"), ("two words", "sp ace"),
                      ("a.txt", "dangling"), ("loop", "renamed"), ("deep", "moved"), ("dir", "missing/x"), ("dir/", "moved"),
                      ("a.txt", "recent.txt"), ("recent.txt", "a.txt"), ("dir", "deep"), ("hollow", "dir"), ("a.txt", "link"),
                      ("link", "a.txt"), ("a.txt", "b.txt", "missing"), ("dir/inside", "dir/sub/x"), ("dup", "dir"), ("dir", "dup"),
                      ("a.txt", "new\nline"), ("-dash", "renamed")),
            stdin=("files_yes", "files_no"), fixture="files", stderr="exact",
            extra=(("-f", "-i", "a.txt", "b.txt"), ("-i", "-f", "a.txt", "b.txt"), ("-n", "-f", "a.txt", "b.txt"), ("-f", "-n", "a.txt", "b.txt"),
                   ("-T", "dir/sub", "elsewhere"), ("-T", "a.txt", "b.txt", "dir"), ("-t", "dir/sub", "a.txt"), ("-v", "a.txt", "renamed"),
                   ("-b", "a.txt", "b.txt"), ("--backup=numbered", "a.txt", "b.txt"), ("-u", "a.txt", "b.txt"), ("-u", "b.txt", "a.txt"),
                   ("--exchange", "a.txt", "b.txt"), ("--exchange", "dir", "a.txt"), ("--exchange", "a.txt", "missing"),
                   ("--strip-trailing-slashes", "dir/", "moved"), ("-W", "a.txt", "renamed"))),
    Utility("rm", options=(Option("-f"), Option("-i"), Option("-I"), Option("-r"), Option("-R"), Option("-d"), Option("-v"),
                           Option("--force"), Option("--interactive"), Option("--interactive", ("always", "once", "never", "bogus"), True),
                           Option("--one-file-system"), Option("--no-preserve-root"), Option("--preserve-root"),
                           Option("--preserve-root", ("all",), True), Option("--recursive"), Option("--dir"), Option("--verbose")),
            operands=(("a.txt",), ("dir",), ("missing",), ("link",), ("dangling",), ("dirlink",), ("dirlink/",), ("hollow",),
                      ("a.txt", "b.txt", "missing"), ("shut",), ("unreadable",), ("deep",), ("two words",), ("--", "-dash"), (),
                      ("dir", "a.txt", "hollow"), ("loop",), ("dir/sub/back",), (".",), ("./",), ("a.txt/",), ("dir/",), ("twin",),
                      ("nest",), ("dup", "nest", "hollow"), ("a.txt", "a.txt"), ("new\nline",), ("shut/inside",), ("dir/sub",)),
            stdin=("files_yes", "files_no", "files_mixed"), fixture="files", stderr="exact",
            extra=(("-rf", "dir", "a.txt", "missing"), ("-ri", "dir"), ("-rv", "dir"), ("-dv", "hollow"), ("-fd", "dir"),
                   ("-f", "-i", "a.txt"), ("-i", "-f", "a.txt"), ("-r", "--one-file-system", "dir"), ("-I", "a.txt", "b.txt", "c.txt", "empty"),
                   ("-rI", "dir"), ("-rf", "shut"), ("-r", "shut"), ("-rf", "unreadable"), ("-r", "dir/sub/back"), ("-rf", "dir/back/"),
                   ("-r", "dirlink"), ("-r", "dirlink/"), ("-rf", "deep", "nest", "dup"), ("-d", "dir"), ("-d", "hollow", "nest/a/b"),
                   ("-r", "."), ("-rf", "./"), ("-W", "a.txt"), ("-v", "a.txt", "link", "dangling"))),
    Utility("mktemp", options=(Option("-d"), Option("-u"), Option("-q"), Option("-t"), Option("--directory"), Option("--dry-run"),
                               Option("--quiet"), Option("--tmpdir"),
                               Option("-p", ("dir", ".", "missing", "", "a.txt", "hollow"), None),
                               Option("--tmpdir", ("dir", "missing", "a.txt", ""), True),
                               Option("--suffix", (".txt", "-XXX", "", "/bad", "X"), True)),
            operands=files_MKTEMP_TEMPLATES, stdin=("empty",), fixture="files", stderr="exact", valid=files_mktemp_valid,
            normalize=files_normal(lambda data: re.sub(rb"(?<=(?:run|tmp)\.)[A-Za-z0-9]{3,}", b"#", data))),
    Utility("sleep", operands=(("0",), ("0.1",), ("0.3",), ("1",), ("1s",), ("0.5s",), ("0.01m",), ("0.0003h",), ("0.00001d",),
                               ("1", "0.5"), ("nonsense",), ("",), (), ("-1",), ("1x",), ("1e-1",), ("nan",), ("0x1",), ("0.1", "x"),
                               ("1.5.2",), ("0s", "0m", "0h", "0d"), ("+1",), (".5",), ("1.",), ("1 s",), ("s",), ("1ss",), ("0", "-1")),
            stdin=("empty",), fixture="files", stderr="exact", timeout=8.0),
    Utility("xargs", options=(Option("-0"), Option("-r"), Option("-t"), Option("-x"), Option("--null"),
                              Option("--no-run-if-empty"), Option("--verbose"), Option("--exit"),
                              Option("--show-limits"), Option("-i"), Option("-l"), Option("--replace"),
                              Option("--max-lines"),
                              Option("-a", ("words", "nulwords", "missing", "empty", "paths"), None), Option("--arg-file", ("words",), True),
                              Option("-d", ("\\n", ",", " ", "x", "ab", "\\0", "\\t", ""), None), Option("--delimiter", (",",), True),
                              Option("-E", ("_", "end", "", "twelve"), None), Option("--eof", ("_",), True), Option("-e"),
                              Option("-I", ("{}", "%", "x", "", "{x}"), None), Option("--replace", ("{}",), True),
                              Option("-L", ("1", "2", "0", "x", "3"), None), Option("--max-lines", ("2",), True),
                              Option("-n", ("1", "2", "3", "1000", "0", "x", "5"), None), Option("--max-args", ("2",), True),
                              Option("-s", ("10", "100", "4096", "x", "0", "60", "131072", "999999999"), None), Option("--max-chars", ("50",), True),
                              Option("-P", ("0", "1", "2", "x"), None), Option("--max-procs", ("1", "0"), True),
                              Option("--process-slot-var", ("SLOT",), True)),
            operands=((), ("echo",), ("./exe",), ("printf", "%s\\n"), ("sh", "-c", "echo $#"), ("true",), ("false",), ("nosuchcommand",),
                      ("./unreadable",), ("ls", "-d"), ("rm",), ("sh", "-c", "exit 255"), ("sh", "-c", "kill -TERM $$"), ("echo", "-n"),
                      ("sh", "-c", "printf %s\\\\n \"$@\"", "sh"), ("dir",), ("printf", "[%s]"), ("sh", "-c", "echo $SLOT"), ("--", "echo")),
            stdin=("files_words", "files_nul_words", "files_paths", "text", "empty", "many_lines", "edge_65537", "spaces", "nonl",
                   "files_eof_words", "blanks", "nul"),
            fixture="files", stderr="exact", valid=files_xargs_valid, max_flags=3),
    Utility("cal", options=(Option("-1"), Option("-3"), Option("-S"), Option("-s"), Option("-m"), Option("-j"), Option("-y"),
                            Option("-Y"), Option("-v"), Option("--one"), Option("--three"), Option("--span"), Option("--sunday"),
                            Option("--monday"), Option("--julian"), Option("--year"), Option("--twelve"), Option("--vertical"),
                            Option("--iso"), Option("-w"), Option("-w", ("1", "53", "x", "0"), True), Option("--week"),
                            Option("--week", ("10",), True),
                            Option("-n", ("1", "2", "4", "0", "x", "13", "24"), None), Option("--months", ("3",), True),
                            Option("-c", ("1", "2", "3", "x", "0"), None), Option("--columns", ("2",), True),
                            Option("--reform", ("1752", "gregorian", "iso", "julian", "bogus"), True),
                            Option("--color", ("never", "always", "auto", "bogus"), True)),
            operands=(("2001",), ("9", "2001"), ("9", "9", "2001"), ("2", "2000"), ("2", "1900"), ("1752",), ("9", "1752"), ("1", "1"),
                      ("13", "2001"), ("0",), ("10000",), ("x",), ("september", "2001"), ("2001-09-09",), (), ("1", "2", "3", "4"),
                      ("12", "9999"), ("31", "12", "2001"), ("32", "12", "2001"), ("29", "2", "2001"), ("29", "2", "2000"), ("sep",),
                      ("2", "1", "1"), ("10", "1582"), ("9",), ("9999",), ("1", "9999"), ("12", "1")),
            stdin=("empty",), fixture="files", stderr="exact", env=(("TERM", "dumb"),)),
    Utility("hardlink", options=(Option("-c"), Option("-d"), Option("-f"), Option("-F"), Option("-l"), Option("-m"), Option("-M"),
                                 Option("--mount"), Option("-n"), Option("-o"), Option("-O"), Option("-p"), Option("-q"), Option("-t"),
                                 Option("-v"), Option("-X"), Option("-z"), Option("--content"), Option("--respect-dir"),
                                 Option("--respect-name"), Option("--prioritize-trees"), Option("--list-duplicates"), Option("--maximize"),
                                 Option("--minimize"), Option("--dry-run"), Option("--ignore-owner"), Option("--keep-oldest"),
                                 Option("--ignore-mode"), Option("--quiet"), Option("--ignore-time"), Option("--verbose"),
                                 Option("--respect-xattrs"), Option("--zero"), Option("--skip-reflinks"), Option("--reflink"),
                                 Option("--reflink", ("auto", "always", "never", "bogus"), True),
                                 Option("-b", ("1", "4096", "x", "1M"), None), Option("--io-size", ("8K",), True),
                                 Option("-r", ("1", "1M", "x"), None), Option("--cache-size", ("1M",), True),
                                 Option("-s", ("0", "1", "5", "x", "1K"), None), Option("--minimum-size", ("1",), True),
                                 Option("-S", ("1", "5", "100", "x"), None), Option("--maximum-size", ("100",), True),
                                 Option("-i", ("one", ".*", "[", "sub"), None), Option("--include", ("two",), True),
                                 Option("-x", ("one", ".*", "[", "sub"), None), Option("--exclude", ("two",), True),
                                 Option("--exclude-subtree", ("sub", "dup"), True),
                                 Option("-y", ("sha256", "memcmp", "crc32c", "bogus", "sha1", "xxh128"), None), Option("--method", ("memcmp",), True)),
            operands=(("dup",), (".",), ("a.txt", "b.txt"), ("dup", "dir"), ("missing",), (), ("dup/one", "dup/two"), ("dup/one", "dup/three"),
                      ("twin", "b.txt"), ("shut",), ("two words", "dup"), ("dangling", "dup"), ("dup", "dup"), ("link", "a.txt")),
            stdin=("empty",), fixture="files", stderr="exact", normalize=files_normal(files_hide_duration, files_hide_now)),
    Utility("namei", options=(Option("-x"), Option("-m"), Option("-o"), Option("-l"), Option("-n"), Option("-v"), Option("--mountpoints"),
                              Option("--modes"), Option("--owners"), Option("--long"), Option("--nosymlinks"), Option("--vertical")),
            operands=(("a.txt",), ("dir/sub/deep",), ("link",), ("dirlink/inside",), ("dangling",), ("loop",), ("missing",), ("/",),
                      ("/usr/bin/ls",), ("dir/sub/back/inside",), (), ("a.txt", "link"), ("two words",), ("shut/inside",), ("badwalk",),
                      ("dir/../a.txt",), ("./a.txt",), ("//a.txt",), ("dir//sub",), ("dirlink/..",), ("missing/x/y",), ("a.txt/x",),
                      ("--", "-dash"), ("",), ("/dev/null",), ("deep/one/two/three/leaf",), ("x" * 300,)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("rename", options=(Option("-v"), Option("-s"), Option("-n"), Option("-a"), Option("-l"), Option("-o"), Option("-i"),
                               Option("--verbose"), Option("--symlink"), Option("--no-act"), Option("--all"), Option("--last"),
                               Option("--no-overwrite"), Option("--interactive")),
            operands=((".txt", ".md", "a.txt"), (".txt", ".md", "a.txt", "b.txt", "c.txt"), ("a", "z", "a.txt", "dangling"),
                      ("", "x", "a.txt"), ("txt", "", "a.txt"), ("a", "b", "missing"), ("a.txt", "b.txt", "a.txt"), ("link", "renamed", "link"),
                      ("nowhere", "a.txt", "dangling"), ("t", "T", "a.txt"), (".", "_", "a.txt", "two words"), ("x",), (), ("a", "b"),
                      ("dir", "d", "dir/inside"), ("inside", "in", "dir/inside"), ("dir", "moved", "dir"), ("a", "a", "a.txt"),
                      ("a.txt", "dir", "a.txt"), ("a.txt", "dir/x", "a.txt"), ("a", "aa", "a.txt"), ("e", "E", "dir/sub/deep", "deep"),
                      ("two", "2", "two words"), ("--", "-", "_", "-dash"), ("n", "m", "new\nline"), ("one", "uno", "dup/one", "dup/sub/four"),
                      ("in", "out", "shut/inside"), ("b", "c", "b.txt", "b.txt~"), ("a", "", "a.txt")),
            stdin=("files_yes", "files_no"), fixture="files", stderr="exact"),
    Utility("whereis", options=(Option("-b"), Option("-m"), Option("-s"), Option("-u"), Option("-l"), Option("-g"),
                                Option("-B", ("/usr/bin", "dir", "missing"), None), Option("-M", ("/usr/share/man/man1", "dir"), None),
                                Option("-S", ("dir", "/usr/src"), None), Option("-f")),
            operands=(("ls",), ("nosuchprogram",), ("ls", "sh"), (), ("inside",), ("a.txt",), ("ls*",), ("l?",), ("-f", "ls"),
                      ("-B", "/usr/bin", "-f", "ls"), ("-S", "dir", "-f", "inside"), ("ls", "-b"), ("",), ("ls.1",), ("bash",)),
            stdin=("empty",), fixture="files", stderr="exact"),
    Utility("nologin", options=(Option("-c", ("true", "", "x"), None), Option("--command", ("true",), True)),
            operands=((), ("extra",), ("-x",)), stdin=("empty",), fixture="files", stderr="exact"),
    Utility("kill", options=(Option("-s", ("TERM", "KILL", "0", "9", "HUP", "bogus", "64", "-1", "SIGTERM", "term", ""), None),
                             Option("--signal", ("TERM", "0"), True), Option("-l"), Option("-l", ("9", "15", "TERM", "x", "0", "64", "-1", "128"), True),
                             Option("--list"), Option("--list", ("9",), True), Option("-L"), Option("--table"), Option("-p"),
                             Option("--verbose"), Option("-a"), Option("-q", ("1", "x"), None), Option("--queue", ("1",), True),
                             Option("--timeout", ("1000", "x"), True)),
            operands=(("999999",), ("1",), ("abc",), ("",), (), ("999999", "999998"), ("-", "999999"), ("999999999999",), ("+1",),
                      ("--", "999999"), ("999999", "abc")),
            stdin=("empty",), fixture="files", stderr="exact",
            extra=(("-0", "999999"), ("-9", "999999"), ("-TERM", "999999"), ("-KILL", "999999"),
                   ("-SIGTERM", "999999"), ("-bogus", "999999"), ("-sterm", "999999"),
                   ("-s0", "999999"), ("-l15", "999999"), ("-q1", "999999"), ("-lx", "999999"),
                   ("-9", "abc"), ("-TERM", "+1"), ("-0", ""), ("-RT1", "999999"),
                   ("-l", "RTMIN"), ("-l", "SIGKILL"), ("-L", "999999"))),
    Utility("stty", options=(Option("-a"), Option("-g"), Option("--all"), Option("--save"),
                             Option("-F", ("/dev/null", "missing", "a.txt", "dir"), None), Option("--file", ("/dev/null",), True)),
            operands=(("size",), ("speed",), ("sane",), ("-echo",), ("echo",), ("raw",), ("cols", "80"), ("rows", "24"), ("cs8",), ("bogus",),
                      ("erase", "^H"), ("intr", "^C"), (), ("size", "speed"), ("size", "bogus"), ("9600",), ("min", "1"), ("-a", "size")),
            stdin=("empty", "text"), fixture="files", stderr="loose", timeout=3.0),
    Utility("date", options=(Option("-u"), Option("-R"), Option("-I"), Option("--utc"), Option("--universal"), Option("--rfc-email"),
                             Option("--iso-8601"), Option("--debug"), Option("--resolution"),
                             Option("-I", ("date", "hours", "minutes", "seconds", "ns", "furlongs", ""), True),
                             Option("--iso-8601", ("seconds", "date", "ns"), True),
                             Option("--rfc-3339", ("date", "seconds", "ns", "bogus", ""), True),
                             Option("-d", ("@1000000000", "@0", "@-1", "@951782400", "@2147483647", "@-2208988800", "@4102444800",
                                           "@1000000000.5", "@1000000000.123456789", "2001-09-09", "2001-09-09 01:46:40",
                                           "2001-09-09 +1 month", "Sep 9 2001 12:00", "nonsense", "", "@9223372036854775807",
                                           "1900-01-01", "2100-06-15 12:00:00", "2001-09-09 01:46:40 +0100", "monday"), False),
                             Option("--date", ("@1000000000", "2001-09-09"), True),
                             Option("-r", ("a.txt", "missing", "link", "dangling", "dir", "shut/inside", "recent.txt", ".", "unreadable"), None),
                             Option("--reference", ("b.txt",), True),
                             Option("-f", ("dates", "missing", "-", "empty", "dir"), None), Option("--file", ("dates",), True),
                             Option("-s", ("2001-09-09", "nonsense"), None), Option("--set", ("@0",), True)),
            operands=files_DATE_FORMATS, stdin=("files_dates", "empty"), fixture="files", stderr="exact", valid=files_date_valid,
            extra=tuple(("-d", when, files_DATE_FORMAT) for when in files_DATE_READS) +
                  tuple(("-d", "@%d" % moment, shape) for moment in
                        (0, 1, -1, 86399, 86400, 951782400, 951868800, 68169600, 1709164800, 1709251200, 2147483647, -2208988800,
                         4102444800, 1000000000, 1234567890, 1709251199, 946684800) + tuple(-2208988800 + n * 45000000 for n in range(138))
                        for shape in ("+%Y-%m-%d|%H:%M:%S|%a %A %b %B|%j %u %w %y %C|%U %W %V %G %g|%e|%-d|%_m|%0k|%l|%s %q|%c",)) +
                  tuple(("-d", "%d-03-01 -1 day" % year, "+%F") for year in range(1900, 2101, 7)) +
                  tuple(("-d", "%d-02-29 +1 year" % year, "+%F") for year in range(1900, 2101, 4)) +
                  tuple(("-d", "%d-01-31 +1 month" % year, "+%F") for year in range(1900, 2101, 11)) +
                  (("-d", "@1000000000"), ("-u", "-d", "@1000000000"), ("--date=@1000000000",), ("-R", "-d", "@1000000000"),
                   ("-d", "@1000000000", "-I"), ("-d", "@1000000000", "-Ihours"), ("-d", "@1000000000", "-Iminutes"),
                   ("-d", "@1000000000", "-Iseconds"), ("-d", "@1000000000", "-Ins"), ("-d", "@1000000000", "--iso-8601=seconds"),
                   ("-d", "@1000000000", "--iso-8601"), ("-d", "@1000000000", "-Ifurlongs"), ("-d", "@1000000000", "--rfc-3339=ns"),
                   ("-r", "a.txt", "+%F"), ("--reference=a.txt", "+%F"), ("-r", "missing"), ("-r", "a.txt", "-d", "@0"), ("-d", "@0", "-r", "a.txt"),
                   ("-f", "dates"), ("-f", "dates", "+%s"), ("-f", "-"), ("-f", "missing"), ("-d", "@1000000000", "-f", "dates"),
                   ("-s", "2001-09-09", "-d", "@0"), ("--set=@0", "+%s"), ("-d", "@1000000000", "-s", "nonsense"),
                   ("-d", "@1000000000", "+%N"), ("-d", "@1000000000.123456789", "+%N|%3N|%s"), ("-d", "@1000000000", "--debug"),
                   ("-d", "2001-09-09 +1 day", "--debug", "+%F"), ("--resolution",), ("-d", "@1000000000", "--resolution"),
                   ("-x",), ("-d",), ("-r",), ("-f",), ("-I", "-R", "-d", "@0"), ("-R", "-I", "-d", "@0"), ("-u", "-d", "2001-09-09 12:00 +0200", "+%H %Z"))),
)

# Every utility drops GNU's --help hint from its diagnostics, whether or not it
# has a normaliser of its own.
UTILITIES = tuple(utility if utility.normalize else dataclasses.replace(utility, normalize=files_plain)
                  for utility in UTILITIES)
