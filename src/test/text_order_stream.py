#!/usr/bin/env python3
"""Exact numeric keys and refill/carry boundaries against the installed tools."""
import os
import hashlib
from pathlib import Path
import random
import subprocess
import sys
import tempfile

farm = Path(sys.argv[1]).resolve()
env = dict(os.environ, LC_ALL="C", TZ="UTC0")
rng = random.Random(0x534F5254)
passed = failed = 0


def check(tool, args, data=b""):
    global passed, failed
    commands = ([str(farm / tool), *args], [tool, *args])
    outputs = [subprocess.run(command, input=data, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env, timeout=15)
               for command in commands]
    if (outputs[0].returncode, outputs[0].stdout) == (outputs[1].returncode, outputs[1].stdout):
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

print(f"  order-stream {passed} of {passed + failed}")
if os.environ.get("TEST_TALLY"):
    with open(os.environ["TEST_TALLY"], "a") as tally:
        tally.write(f"text-order-stream {passed} {passed + failed}\n")
sys.exit(bool(failed))
