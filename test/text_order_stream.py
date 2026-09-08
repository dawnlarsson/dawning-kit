#!/usr/bin/env python3
"""Exact numeric keys and refill/carry boundaries against the installed tools."""
import os
import hashlib
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

farm = Path(sys.argv[1]).resolve()
env = dict(os.environ, LC_ALL="C", TZ="UTC0")
rng = random.Random(0x534F5254)
passed = failed = 0


def check(tool, args, data=b"", status=None):
    global passed, failed
    commands = ([str(farm / tool), *args], [tool, *args])
    outputs = [subprocess.run(command, input=data, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env, timeout=15)
               for command in commands]
    if (outputs[0].returncode, outputs[0].stdout) == (outputs[1].returncode if status is None else status, outputs[1].stdout):
        passed += 1
    else:
        failed += 1
        print("  want", tool, repr(args)[:160], "input", repr(data[:96]),
              "status", [out.returncode for out in outputs],
              "output", [repr(out.stdout[:96]) for out in outputs])


atoms = [b"", b" ", b"-", b"+7", b"x", b"0", b"-0", b"-.000", b".000",
         b"-.0001", b".0001", b"0001.23000", b"1.23", b"-1.23", b"-01.230",
         b"9999999", b"99999999", b"999999999", b"0000000099999999.01",
         b"-99999999", b"-99999999.000", b"-99999999.001",
         b"18446744073709551615", b"18446744073709551616",
         b"9" * 256, b"1" + b"0" * 256, b"-" + b"9" * 256,
         b"0" * 2048 + b"2.000", b".000" + b"0" * 2048 + b"1",
         b"-0." + b"0" * 2048]
for _ in range(80):
    integer = bytes(rng.choice(b"0123456789") for _ in range(rng.randrange(40)))
    fraction = bytes(rng.choice(b"0123456789") for _ in range(rng.randrange(30)))
    atoms.append(rng.choice([b"", b"-", b"+", b" \t-"]) + integer + b"." + fraction
                 + rng.choice([b"", b"000", b"tail"]))

for iteration in range(12):
    rng.shuffle(atoms)
    data = b"\n".join(atoms) + b"\n"
    for flags in (["-n"], ["-nr"], ["-ns"], ["-nu"], ["-nru"], ["-nh"]):
        check("sort", flags, data)
    records = [b"%03d:" % i + number + b":" + bytes([97 + i % 4])
               for i, number in enumerate(atoms)]
    data = b"\n".join(records) + b"\n"
    for flags in (["-t:", "-k2,2n"], ["-t:", "-k2,2nr", "-s"],
                  ["-t:", "-k2,2n", "-u"], ["-t:", "-k3,3", "-k2,2n"],
                  ["-t:", "-k2,2n", "-k3,3r"], ["-n", "-t:", "-k2,2"],
                  ["-r", "-t:", "-k2,2n"], ["-t:", "-k2.2,2.7n"]):
        check("sort", flags, data)
    check("sort", ["-z", "-t:", "-k2,2n"], data.replace(b"\n", b"\0"))

with tempfile.TemporaryDirectory() as directory:
    paths = []
    for index in range(3):
        data = b"\n".join(atoms[index::3]) + b"\n"
        ordered = subprocess.check_output(["sort", "-ns"], input=data, env=env)
        path = Path(directory) / str(index)
        path.write_bytes(ordered)
        paths.append(str(path))
        check("sort", ["-nC", str(path)])
    for flags in (["-nm"], ["-nms"], ["-nmu"]):
        check("sort", [*flags, *paths])

for edge in [0, 8, 9, 98, 99, 998, 999, 9998, 9999, 99998, 99999,
             9999999999999998, 9223372036854775805]:
    stop = min(edge + 4, 9223372036854775807)
    for options in ([], ["-w"], ["-s", ""], ["-s", "::"]):
        check("seq", [*options, str(edge), str(stop)])
for _ in range(60):
    first, last = rng.randrange(-5000, 5000), rng.randrange(-5000, 5000)
    step = rng.choice([-999, -17, -1, 1, 3, 77])
    check("seq", ["-w", "-s", ":", str(first), str(step), str(last)])
for length in [0, 1, 31, 16363, 16383, 16384, 16385, 50000]:
    check("seq", ["-s", "s" * length, "8", "11"])
check("seq", ["1", "100000"])

for size in [0, 1, 15, 16, 17, 63, 64, 65, 65535, 65536, 65537, 131073]:
    data = b"a" * size + b"\0\0..aaBB" + bytes(range(256)) * 3 + b"B" * size
    for args in (["-s", "aB."], ["-s", r"\0"], ["-cs", "a"],
                 ["-s", r"\000-\377"], ["-s", "a", "B"],
                 ["-ds", "a", "B"], ["-d", "aB"]):
        check("tr", args, data)
for _ in range(40):
    data = b"".join(bytes([rng.randrange(256)]) * rng.randrange(1, 130)
                    for _ in range(80))
    check("tr", ["-s", r"\000-\377"], data)

layout_rng = random.Random(0x434F4C)
motifs = [b"ab\rZ", b"A\bB\bC", b"abc\tdef\rP", b"a\x0eB\x0fc",
          b"x\x01Y", b"   a\rX"]
for iteration in range(50):
    data = b"\n".join(layout_rng.choice(motifs)
                      for _ in range(layout_rng.randrange(1, 24)))
    if iteration % 2:
        data += b"\n"
    for flags in ([], ["-b"], ["-x"], ["-b", "-x"], ["-f"],
                  ["-f", "-b", "-x"], ["-p"], ["-p", "-x"]):
        check("col", flags, data)
    # Force the global stable-order fallback as well as rounded/fine lines.
    middle = len(data) // 2
    motion = layout_rng.choice([b"\v\rX", b"\x1b\x07\rX",
                                b"\x1b\x08Y", b"\x1b\x09Z"])
    data = data[:middle] + motion + data[middle:]
    for flags in ([], ["-b", "-x"], ["-f"], ["-p"]):
        check("col", flags, data)

for separator in (None, ":", ",", "|"):
    joiner = (separator or " \t").encode()
    rows = [joiner.join(fields) for fields in
            ([b"A", b"B", b"C", b"D"], [b"", b"x", b"", b"z"],
             [b"q", b"", b"r", b""], [b"one"], [], [b" \t"]) ]
    data = b"\n".join(rows) + b"\n"
    split = ["-s", separator] if separator is not None else []
    for options in ([], ["-L"], ["-N", "A,B,C,D"],
                    ["-N", "A,B,C,D", "-O", "3,1,3,2", "-H", "2"],
                    ["-N", "A,B,C,D", "-O", "D,A,D", "-R", "A"],
                    ["-K"], ["-K", "-O", "D,B,D", "-H", "A"],
                    ["-J", "-N", "A,B,C,D", "-O", "4,1,4", "-H", "2"]):
        check("column", ["-t", *split, *options], data)
for width in (65534, 65535, 65536, 65537):
    data = b"a:" + b"x" * width + b":\nz::tail"
    check("column", ["-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C"], data)
for flags in ([], ["-o", "|"], ["-R", "C"], ["-E", "B"], ["-c", "unlimited"]):
    check("column", ["-t", "-s", ":", "-N", "A,B,C", "-O", "C,A,C", *flags],
          b"a:" + b"x" * 100 + b":\nz::tail")

with tempfile.TemporaryDirectory() as directory:
    first, second = Path(directory) / "a", Path(directory) / "b"
    for iteration in range(80):
        records = [layout_rng.choice([b"foo", b"bar", b" ", b"", b"FOO",
                                      b"bar\t", b"quux"])
                   for _ in range(layout_rng.randrange(30))]
        changed = records.copy()
        if iteration % 4 == 0 and changed:
            changed.pop(layout_rng.randrange(len(changed)))
        if iteration % 4 == 1:
            changed.insert(layout_rng.randrange(len(changed) + 1), b"change")
        if iteration % 4 == 2 and changed:
            changed[layout_rng.randrange(len(changed))] = b"change"
        first.write_bytes(b"\n".join(records) + (b"\n" if iteration % 3 else b""))
        second.write_bytes(b"\n".join(changed) + (b"\n" if iteration % 5 else b""))
        for flags in ([], ["-q"], ["-i"], ["-w"], ["-B"],
                      ["-u", "-L", "a", "-L", "b"]):
            check("diff", [*flags, str(first), str(second)])

# The ordinary line copier and multi-input views share their spill engine.
# Vary its exact bound, termination, embedded non-delimiter NUL/newline and
# retained duplicate records rather than only repeating tiny sorted inputs.
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "spill"
    source.write_bytes(b"a one\na two\nz three\n")
    for number in ("", "+", "-1", "0", "1", "42", "  +1", "1 ", "1,2", "0x10",
                   "18446744073709551614", "18446744073709551615", "18446744073709551616",
                   "9" * 200):
        # join's existing field policy rejects overflow; cmp's decimal
        # grammar intentionally lacks GNU's hex and implicit-one forms.
        if len(number) < 20 or (len(number) == 20 and number <= "18446744073709551615"):
            check("join", ["-1", number, str(source), str(source)])
        for suffix in ("", "K", "KB", "KiB", "junk"):
            if number != "0x10" and (number or not suffix or suffix == "junk"):
                check("cmp", ["-n", number + suffix, str(source), str(source)])
    for delimiter in (b"\n", b"\0"):
        flags = ["-z"] if delimiter == b"\0" else []
        records = [b"", b"a one", b"a one", b"a two", b"b:one", b"b:two",
                   b"m" + (b"\0" if delimiter == b"\n" else b"\n") + b"n"]
        for terminated in (False, True):
            source.write_bytes(delimiter.join(records) + (delimiter if terminated else b""))
            for separators in (("",), ("\n",), ("", ""), ("", "\n"), ("\n", ""),
                               (":", ":"), ("", ":"), (":", ""), (":", ","),
                               ("ab",), ("é",), ("ab", ""), ("", "ab"), (":", "ab")):
                options = [item for separator in separators for item in ("-t", separator)]
                check("join", [*flags, "--nocheck-order", *options, str(source), str(source)])
    for byte in (9, 10, 11, 12, 13, 32, 128, 255):
        source.write_bytes(b"m" + bytes([byte]) + b"n\0")
        for options in ([], ["-t", ":"], ["-t", "\n"], ["-o", "1.2,2.2"]):
            check("join", ["-z", *options, str(source), str(source)])
    for width in (0, 1, 65535, 65536, 65537, (1 << 20) - 1, 1 << 20):
        for delimiter in (b"\n", b"\0"):
            payload = b"m" * width
            if width > 1:
                middle = width // 2
                payload = payload[:middle] + (b"\0" if delimiter == b"\n" else b"\n") + payload[middle + 1:]
            records = sorted([b"a", payload, payload, b"z"])
            flags = ["-z"] if delimiter == b"\0" else []
            for terminated in (False, True):
                data = delimiter.join(records) + (delimiter if terminated else b"")
                source.write_bytes(data)
                for tool, options in (("cut", ["-c1-3"]), ("sed", ["-n", "p"]),
                                      ("uniq", [])):
                    check(tool, [*flags, *options], data)
                for tool, options in (("comm", ["--nocheck-order"]),
                                      ("paste", []), ("join", ["--nocheck-order"])):
                    check(tool, [*flags, *options, str(source), str(source)])

if "--capacity" in sys.argv[2:]:
    # Byte-only head/tail streams need no million-entry line-index reservation.
    # Feed in bounded blocks so this regression does not require a second
    # input-sized allocation in the test runner itself.
    for tool, flags in (("head", ["-c", "-1"]), ("tail", ["-c", "1"])):
        block = b"x" * (1 << 20)
        with tempfile.TemporaryFile() as output:
            process = subprocess.Popen([str(farm / tool), *flags], stdin=subprocess.PIPE,
                                       stdout=output, stderr=subprocess.PIPE, env=env)
            try:
                for _ in range(176):
                    process.stdin.write(block)
                process.stdin.close()
            except BrokenPipeError:
                pass
            process.stdin = None
            _, diagnostic = process.communicate(timeout=30)
            expected_size = 176 * len(block) - 1 if tool == "head" else 1
            exact = output.tell() == expected_size
            output.seek(0)
            while chunk := output.read(len(block)):
                exact &= chunk == block[:len(chunk)]
        if process.returncode == 0 and not diagnostic and exact:
            passed += 1
        else:
            failed += 1
            print("  want byte-only capacity", tool, "status 0 and exact output; got",
                  process.returncode, repr(diagnostic[:96]), "exact", exact)

    # Per-line scratch fits this stream; a vertical rewind needs the full
    # scratch vector. Both ordering-allocation failures must be loud failures,
    # not an empty successful result after the diagnostic was printed.
    for count, tail, expected_status in ((2500000, b"", 0),
                                          (2500000, b"\v\vX\n", 1),
                                          (4000000, b"", 1)):
        result = subprocess.run([str(farm / "col"), "-b", "-x"],
                                input=b"ab\rC\n" * count + tail,
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                env=env, timeout=30)
        if result.returncode == expected_status and bool(result.stderr) == bool(expected_status):
            passed += 1
        else:
            failed += 1
            print("  want col capacity", count, repr(tail), "status", expected_status,
                  "got", result.returncode, repr(result.stderr[:96]))

    # One million 127-byte lines fit the original 192 MiB arena, including
    # slices and sort indexes, but not an additional 32-byte numeric cache.
    # All records are identical: their expected output is known without
    # allocating a second Python copy or relying on an enormous GNU sort.
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "capacity"
        output = Path(directory) / "output"
        block = (b"7" + b" " * 126 + b"\n") * 8192
        expected = hashlib.sha256()
        with source.open("wb") as stream:
            for _ in range(128):
                stream.write(block)
                expected.update(block)
        for flags in (["-n"], ["-k1,1n"], ["-nm"]):
            with output.open("wb") as stream:
                result = subprocess.run([str(farm / "sort"), *flags, str(source)],
                                        stdout=stream, stderr=subprocess.PIPE,
                                        env=env, timeout=90)
            actual = hashlib.sha256()
            with output.open("rb") as stream:
                while chunk := stream.read(1 << 20):
                    actual.update(chunk)
            if result.returncode == 0 and not result.stderr and actual.digest() == expected.digest():
                passed += 1
            else:
                failed += 1
                print("  want capacity", flags, "status 0 and exact full output; got",
                      result.returncode, repr(result.stderr[:96]), output.stat().st_size)

# Exercise every adjacent precedence pair, including false/true short circuit,
# without deriving the oracle from our operator table or parser.
operators = ["|", "&", "=", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%"]
for left in operators:
    for right in operators:
        check("expr", ["7", left, "2", right, "3"])
for operator in ("!", "<<", "<=x", "&&", "||", "+1", "", ">>"):
    check("expr", ["1", operator, "2"])
for value in ("0", "00", "", "word", "-1"):
    for operator in ("|", "&"):
        for tail in (["1", "/", "0"], ["x", "+", "1"],
                     ["(", "1", "+", "2", ")"], ["(", "1", "+", ")"]):
            check("expr", [value, operator, *tail])
for pattern in ("(a|b)*c", "(a?|b)c", "(^a|b$)", "(a*)*b", "a{0,2}b",
                "(a|)b", "(a)(b)?\\1", "^$", "(ab|a)*$", "[ab]*c"):
    for flags in (["-E"], ["-Eo"], ["-Ev"]):
        check("grep", [*flags, pattern], b"\na\nb\naba\nabbc\nzzbc\naaab\n")
for length in (1, 31, 32, 63, 64, 65535, 65536, 65537):
    data = b"\f" + b"x" * length + b"\f\f" + b"y" * length + b"\nend\n"
    for flags in (["-t"], ["-T"], ["-T", "-l", "3"], ["-t", "-n", "-l", "3"]):
        check("pr", flags, data)
for alias in "abcdDhxHXiILlBoOs":
    check("od", ["-An", "-" + alias + "c", "-t", "x1z"], bytes(range(39)))
for group, words in (("conv", ["sync", "noerror", "lcase", "ucase", "swab", "notrunc"]),
                     ("iflag", ["fullblock", "count_bytes", "skip_bytes"]),
                     ("oflag", ["append", "seek_bytes"])):
    for word in words:
        for flags in ([group + "=" + word], [group + "=" + word + "," + word]):
            check("dd", ["status=none", "bs=7", "count=3", "skip=1", *flags],
                  b"aBcDeFgHiJkLmNoPqRsTuVwXyZ\n" * 3)
    for value in ("bogus", words[0] + ",,", words[0] + ",bogus"):
        check("dd", ["status=none", group + "=" + value])
for flags in (["iflag=count_bytes,skip_bytes", "count=8", "skip=3"],
              ["count=8", "skip=3", "iflag=count_bytes,skip_bytes"]):
    check("dd", ["status=none", "bs=7", *flags], b"abcdefghijklmnopqrstuvwxyz")
if shutil.which("uuidparse"):
    values = ["00000000-0000-0000-0000-000000000000", "ffffffff-ffff-ffff-ffff-ffffffffffff",
              "00000000-0000-1000-8000-000000000000", "00000000-0000-6000-8000-000000000000",
              "1b21dd21-3814-6000-8000-000000000000", "01890abc-def0-7000-8000-000000000000",
              "bad-value"]
    for flags in ([], ["-n"], ["-r"], ["-nr"], ["-o", "TIME,UUID,TYPE,UUID"], ["-J"]):
        check("uuidparse", [*flags, *values])
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "tree").mkdir()
    for number in range(160):
        (root / "tree" / ("%03d.txt" % number)).write_bytes(b"needle\n" if number % 9 else b"other\n")
    (root / "link").symlink_to("tree", target_is_directory=True)
    for flags in (["-r"], ["-R"], ["-rl"], ["-rc"], ["-r", "--include=*0.txt"],
                  ["-r", "--exclude-dir=tree"], ["-r", "--exclude=*1.txt"]):
        check("grep", [*flags, "needle", str(root)])
    (root / "denied").mkdir()
    (root / "hit").write_bytes(b"needle\n")
    (root / "denied").chmod(0)
    try:
        for flags in (["-r"], ["-rs"], ["-rq"], ["-rsq"]):
            check("grep", [*flags, "needle", str(root / "denied")])
            check("grep", [*flags, "needle", str(root / "denied"), str(root / "hit")])
    finally:
        (root / "denied").chmod(0o700)
    if shutil.which("strace"):
        for syscall, when in (("openat", 1), ("openat", 2), ("getdents64", 1), ("getdents64", 2)):
            for flags, match in (([], False), (["-s"], False), (["-q"], True), (["-sq"], True)):
                result = subprocess.run(["strace", "-o", os.devnull, "-e",
                    f"inject={syscall}:error=EIO:when={when}", str(farm / "grep"),
                    "-r", *flags, "needle", str(root / "tree"),
                    *([str(root / "hit")] if match else [])], capture_output=True,
                    env=env, timeout=15)
                if result.returncode == (0 if match else 2) and bool(result.stderr) == ("s" not in "".join(flags)):
                    passed += 1
                else:
                    failed += 1
                    print("  want recursive grep injected error", syscall, when, flags,
                          "got", result.returncode, repr(result.stderr[:96]))

# One codec state must cover complete quanta, partial tails, wrapping and
# refills; in particular LSB-first cannot silently become MSB-first on -i.
for index, length in enumerate((0, 1, 2, 3, 4, 5, 7, 8, 31, 65535, 65536, 65537)):
    data = (bytes(range(256)) * ((length + 255) // 256))[:length]
    for codec in ("base64", "base64url", "base32", "base32hex", "base16", "base2msbf", "base2lsbf"):
        flags = ["--" + codec, "-w", str((0, 1, 7, 76)[index % 4])]
        check("basenc", flags, data)
        encoded = subprocess.check_output(["basenc", *flags], input=data, env=env)
        check("basenc", ["--" + codec, "-d"], encoded)
        check("basenc", ["--" + codec, "-di"], b"!" + encoded[:1] + b"!\n" + encoded[1:])
bits = subprocess.check_output(["basenc", "--base2lsbf", "-w0"], input=bytes(range(256)), env=env)
for stride in (1, 3, 7, 8, 9):
    for separator, flags in ((b"\n", ["-d"]), (b"#", ["-di"])):
        check("basenc", ["--base2lsbf", *flags], separator.join(bits[i:i+stride] for i in range(0, len(bits), stride)))
# A 256-byte repeating input can conceal a stale/refill-offset decode bug.
# Change the pattern across both byte and reader periods, with line widths
# that split binary octets at different positions on successive refills.
for length in (65549, 1048579):
    data = bytes((i * 197 + (i >> 8) + (i >> 16)) & 255 for i in range(length))
    for wrap in (7, 76, 79):
        encoded = subprocess.check_output(
            ["basenc", "--base2lsbf", "-w", str(wrap)], input=data, env=env)
        check("basenc", ["--base2lsbf", "-d"], encoded)
# Force the bulk decoder to hand off to stream policy at each SIMD/quantum
# boundary. Invalid input must preserve exactly the prefix GNU emits.
for codec in ("base64", "base64url", "base32", "base32hex", "base16", "base2msbf", "base2lsbf"):
    encoded = subprocess.check_output(["basenc", "--" + codec, "-w0"],
                                      input=bytes(range(97)), env=env)
    for at in (0, 1, 3, 7, 15, 16, 31, 32, 63):
        for marker in (b"=", b"\x00", b"!\n"):
            data = encoded[:at] + marker + encoded[at:]
            for mode in ("-d", "-di"):
                check("basenc", ["--" + codec, mode], data)
# Explicit padding closes a member, not the entire stream. An incomplete
# base32 padded member at EOF also has different prefix output from an error
# byte interrupting that same member; exercise both without normalising it.
for codec in ("base64", "base32"):
    for symbols in range(9):
        for padding in range(9):
            for tail in (b"", b"A", b"!", b"\n"):
                data = b"A" * symbols + b"=" * padding + tail
                for mode in ("-d", "-di"):
                    check(codec, [mode], data)
for codec in ("base64", "base64url", "base32", "base32hex"):
    for size in range(1, 9):
        member = subprocess.check_output(["basenc", "--" + codec, "-w0"],
                                         input=bytes(range(size)), env=env)
        for separator, mode in ((b"", "-d"), (b"\n", "-d"), (b"!", "-di")):
            check("basenc", ["--" + codec, mode], member + separator + member)
            check("basenc", ["--" + codec, mode], member.rstrip(b"="))
    quantum = 4 if codec.startswith("base64") else 8
    alphabet = (b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
                if quantum == 4 else b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
    if codec == "base64url":
        alphabet = alphabet[:-2] + b"-_"
    elif codec == "base32hex":
        alphabet = b"0123456789ABCDEFGHIJKLMNOPQRSTUV"
    # Every unused-bit pattern: padding is optional, zero unused bits are not.
    for symbols in ((2, 3) if quantum == 4 else (2, 4, 5, 7)):
        for final in alphabet:
            data = alphabet[:1] * (symbols - 1) + bytes([final])
            padding = b"=" * (quantum - symbols)
            for tail in (b"", padding, padding + alphabet[:1] * quantum):
                check("basenc", ["--" + codec, "-d"], data + tail)
    for symbols in (65533, 65534, 65535, 65536, 65537):
        data = b"A" * symbols + b"=" * ((-symbols) % quantum) + b"A" * quantum
        check("basenc", ["--" + codec, "-d"], data)
for groups in (13106, 13107, 13108):
    # A padded one-byte member offsets the 64 KiB staging alignment, then the
    # unfinished final member must remain retractable across a flush.
    data = b"AA======" + b"A" * (groups * 8) + b"AA="
    check("base32", ["-d"], data)
    check("base32", ["-d"], data + b"!")
for size in range(65532, 65536):
    member = subprocess.check_output(["base32", "-w0"], input=b"x" * size, env=env)
    for tail in (b"AA=", b"AAAA=", b"AAAAA=", b"AAAAAA="):
        check("base32", ["-d"], member + tail)
with tempfile.TemporaryDirectory() as directory:
    one, two = (Path(directory) / name for name in ("one", "two"))
    for length in (0, 1, 31, 65535, 65536, 65537):
        data = (b"abc\n" * ((length + 3) // 4))[:length]
        one.write_bytes(data)
        for altered in (data, data + b"\n", data[:-1], data[:length//2] + b"!" + data[length//2+1:]):
            two.write_bytes(altered)
            for flags in ([], ["-l"], ["-lb"], ["-s"], ["-n", str(length//2)], ["-i", "1:2"]):
                left, right = (data[1:], altered[2:]) if "-i" in flags else (data, altered)
                if "-n" in flags:
                    left, right = left[:length//2], right[:length//2]
                # GNU diffutils 3.12 can print -l differences yet return 0
                # after a long equal suffix; derive status from the bytes.
                check("cmp", [*flags, str(one), str(two)], status=int(left != right))
    for left, right in ((b"a\n" * 80 + b"b\nc\n", b"b\n" + b"a\n" * 80),
                        (b"a\nb\n" * 70, b"b\na\n" * 71), (b"", b"x\n"), (b"x", b"")):
        one.write_bytes(left); two.write_bytes(right)
        for flags in ([], ["-u"], ["-U0"], ["-U1"], ["-B"], ["-ub"]):
            check("diff", [*flags, str(one), str(two)])

print(f"  order-stream {passed} of {passed + failed}")
if os.environ.get("TEST_TALLY"):
    with open(os.environ["TEST_TALLY"], "a") as tally:
        tally.write(f"text-order-stream {passed} {passed + failed}\n")
sys.exit(bool(failed))
