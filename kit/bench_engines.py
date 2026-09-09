#!/usr/bin/env python3
"""Matched multicall-engine throughput, with output verification before timing.

Pass identically built snapshots as repeated --binary LABEL=PATH arguments.
The first is the performance reference; GNU tools independently verify bytes
and statuses. Timings include process startup and output to a memory-backed
file, not just the assembly kernel. CPU affinity does not isolate its SMT
sibling. JSON also retains child CPU time to distinguish scheduling delays
from time spent executing. Use primitive benchmarks for small-call costs.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import statistics
import subprocess
import tempfile
import time


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", required=True,
                        metavar="LABEL=PATH")
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--rounds", type=int, default=11)
    parser.add_argument("--bytes", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--filter", default="")
    parser.add_argument("--suite", choices=("engines", "stack", "all"), default="engines")
    parser.add_argument("--time-reference", action="store_true",
                        help="include the installed reference tool in paired timings")
    parser.add_argument("--allow-invalid", action="append", default=[], metavar="LABEL",
                        help="report this historical binary's wrong outputs without timing them")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.rounds < 3 or args.bytes < 1:
        parser.error("use at least three rounds and one input byte")
    binaries = {}
    for item in args.binary:
        label, separator, path = item.partition("=")
        if not separator or not label or label in binaries:
            parser.error("binary arguments need distinct nonempty LABEL=PATH pairs")
        binary = Path(path).resolve(strict=True)
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"not an executable file: {binary}")
        binaries[label] = str(binary)
    if "reference" in binaries:
        parser.error("reference is reserved for the installed tool")
    if len(binaries) < 2 and not args.time_reference:
        parser.error("supply at least two binaries built with the same flags")
    if set(args.allow_invalid) - binaries.keys():
        parser.error("allow-invalid must name a supplied binary label")
    if args.cpu is not None:
        if not hasattr(os, "sched_setaffinity"):
            parser.error("CPU affinity is unavailable on this platform")
        os.sched_setaffinity(0, {args.cpu})
    return args, binaries


def workloads(root, size, env):
    # Deterministic, non-uniform bytes with every byte value represented.
    block = bytes((i * 197 + (i >> 8)) & 255 for i in range(65536))
    raw = root / "bytes"
    raw.write_bytes((block * ((size + len(block) - 1) // len(block)))[:size])
    for name, flags in (
        ("base64", ["--base64"]), ("base64url", ["--base64url"]),
        ("base32", ["--base32"]), ("base32hex", ["--base32hex"]),
        ("base16", ["--base16"]), ("base2msbf", ["--base2msbf"]),
        ("base2lsbf", ["--base2lsbf"]),
    ):
        for wrap in (0, 76):
            encode = [*flags, f"-w{wrap}", str(raw)]
            yield f"{name}/encode/w{wrap}", "basenc", encode
            encoded = root / f"{name}-{wrap}"
            with encoded.open("wb") as output:
                subprocess.run(["basenc", *encode], stdout=output, env=env,
                               check=True, timeout=30)
            yield f"{name}/decode/w{wrap}", "basenc", [*flags, "-d", str(encoded)]
    for name, operands in (
        ("integer", ["1", "1000000"]),
        ("stride", ["-200000", "3", "200000"]),
        ("padded", ["-w", "1", "200000"]),
        ("decimal", ["0.00", ".25", "50000"]),
        ("formatted", ["-f", "%+08.2f", "0.00", ".25", "50000"]),
        ("descending", ["1000000", "-1", "1"]),
        ("negative", ["-1000000", "-1"]),
        ("literal", ["-f", "row=%%[%+012.3f]", "-1000", ".125", "1000"]),
    ):
        yield f"seq/{name}", "seq", operands
    log = root / "kernel-log"
    with log.open("wb") as output:
        for i in range(32768):
            # Avoid GNU's zero-as-unset previous-timestamp policy; this row
            # measures formatting, not that unrelated first-delta difference.
            output.write(b"<6>[%5d.%06d] payload %05d\n" %
                         (1 + i // 1000, (i % 1000) * 1000, i))
    for name, flags in (("timestamp", []), ("delta", ["-d"]), ("json", ["-J"])):
        yield f"dmesg/{name}", "dmesg", [*flags, "-F", str(log)]
    # Exercise the real escaping consumers too: their selected categories
    # differ from dmesg and the all-categories primitive microbenchmark.
    uuids = [(f"bad\\value\x1b{i:06d}" if i % 4 == 0 else
              f"{i:08x}-1234-4234-8234-{i:012x}") for i in range(2048)]
    for name, flags in (("raw", ["-r"]), ("json", ["-J"])):
        yield f"uuidparse/{name}", "uuidparse", [*flags, *uuids]


def stack_workloads(root, size):
    records = root / "records"
    count = max(1, size // 16)
    with records.open("wb") as output:
        for i in range(count):
            output.write((f"{'alpha' if i % 3 else 'beta'}:{i % 1000:03d}:"
                          f"{'needle' if i % 17 == 0 else 'plain'}\n").encode())
    groups = root / "groups"
    groups.write_bytes((b"ab" * 32 + b"\n" + b"ac" * 32 + b"\n") * max(1, count // 10))
    scripts = {
        "parse": ': # parser token stream\n' * 100_000 + 'printf "done\\n"\n',
        "loop": 'i=0; while [ "$i" -lt 300000 ]; do i=$((i+1)); done; printf "%s\\n" "$i"',
        "parameter": 'i=0; x=abcdefghijklmnop; while [ "$i" -lt 100000 ]; do a=${x#?}; b=${x%?}; c=${#x}; i=$((i+1)); done; printf "%s:%s:%s:%s\\n" "$a" "$b" "$c" "$i"',
        "function": 'f() { :; }; i=0; while [ "$i" -lt 300000 ]; do f; i=$((i+1)); done; printf "%s\\n" "$i"',
        "redefine": 'i=0; while [ "$i" -lt 100000 ]; do f() { :; }; i=$((i+1)); done; f; printf "%s\\n" "$i"',
    }
    for name, text in scripts.items():
        script = root / (name + ".sh")
        script.write_text(text + "\n")
        yield "shell/" + name, "bash", [str(script)], None
    for name, tool, operands, source in (
        ("cat", "cat", [records], None),
        ("cat-number", "cat", ["-n", records], None),
        ("cat-visible", "cat", ["-v", records], None),
        ("wc-lines", "wc", ["-l"], records),
        ("wc-words", "wc", ["-w"], records),
        ("grep-literal", "grep", ["-c", "needle", records], None),
        ("grep-group8", "grep", ["-Ec", "(ab){8}", groups], None),
        ("grep-group32", "grep", ["-Ec", "(ab){32}", groups], None),
        ("cut-field", "cut", ["-d:", "-f2", records], None),
        ("cut-trim-first", "cut", ["-c2-", records], None),
        ("cut-prefix", "cut", ["-c1-5", records], None),
        ("cut-suffix", "cut", ["-c7-", records], None),
        ("tr-case", "tr", ["a-z", "A-Z"], records),
        ("tr-delete", "tr", ["-d", "a"], records),
        ("tr-squeeze", "tr", ["-s", "a-z"], records),
        ("sed-literal", "sed", ["s/alpha/omega/g", records], None),
        ("sed-capture", "sed", ["-E", r"s/(alpha):([0-9]+)/\2:\1/g", records], None),
        ("awk-fields", "awk", ["-F:", "{s += $2} END {print s}", records], None),
        ("sort", "sort", [records], None),
        ("uniq", "uniq", ["-c", records], None),
        ("head", "head", ["-n", "1000", records], None),
        ("tail", "tail", ["-n", "1000", records], None),
        ("rev", "rev", [records], None),
        ("base64", "base64", ["-w0", records], None),
        ("base32", "base32", ["-w0", records], None),
        ("od-hex", "od", ["-An", "-tx1", records], None),
        ("hexdump", "hexdump", ["-C", records], None),
        ("cksum", "cksum", [records], None),
        ("sha256sum", "sha256sum", [records], None),
    ):
        yield name, tool, [str(value) for value in operands], source
    left, right = root / "left", root / "right"
    left.write_text("".join(f"{i:07d}\n" for i in range(0, count, 2)))
    right.write_text("".join(f"{i:07d}\n" for i in range(0, count, 3)))
    for tool in ("comm", "paste"):
        yield tool, tool, [str(left), str(right)], None
    left.write_text("".join(f"{i:07d} left-{i % 97:02d}\n" for i in range(count // 2)))
    right.write_text("".join(f"{i:07d} right-{i % 89:02d}\n" for i in range(count // 2)))
    yield "join", "join", [str(left), str(right)], None


def selected_workloads(root, size, env, suite):
    if suite in ("engines", "all"):
        for name, tool, operands in workloads(root, size, env):
            yield name, tool, operands, None
    if suite in ("stack", "all"):
        yield from stack_workloads(root, size)


def expired(signum, frame):
    raise TimeoutError("benchmark process exceeded 60 seconds")


def invoke(binary, tool, operands, output, env, timed=False, source=None, expected=None):
    output.seek(0)
    output.truncate()
    input_file = open(source, "rb") if source else None
    before = resource.getrusage(resource.RUSAGE_CHILDREN) if timed else None
    started = time.perf_counter_ns()
    try:
        with subprocess.Popen([tool, *operands], executable=binary, stdout=output,
                              stdin=input_file or subprocess.DEVNULL,
                              stderr=subprocess.PIPE, env=env, start_new_session=True) as process:
            # communicate(timeout) also calls wait(timeout) after pipe EOF,
            # which can add a 1 ms polling sleep. A signal deadline lets
            # both pipe draining and child reaping remain blocking.
            previous_alarm = signal.signal(signal.SIGALRM, expired)
            signal.setitimer(signal.ITIMER_REAL, 60)
            try:
                _, errors = process.communicate()
            except TimeoutError:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.communicate()
                raise
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
                signal.signal(signal.SIGALRM, previous_alarm)
    finally:
        if input_file:
            input_file.close()
    elapsed = (time.perf_counter_ns() - started) / 1e6
    if timed:
        if process.returncode or errors:
            raise RuntimeError((tool, operands, process.returncode, errors))
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu_ms = 1000 * (after.ru_utime + after.ru_stime -
                         before.ru_utime - before.ru_stime)
        output.seek(0)
        digest = hashlib.file_digest(output, "sha256").hexdigest()
        if digest != expected[1]:
            raise RuntimeError((tool, operands, "timed output mismatch", digest, expected[1]))
        return elapsed, cpu_ms
    output.seek(0)
    digest = hashlib.file_digest(output, "sha256").hexdigest()
    return process.returncode, digest, errors


def main():
    args, binaries = arguments()
    env = dict(os.environ, LC_ALL="C", TZ="UTC0", ENV="/dev/null", BASH_ENV="/dev/null")
    labels = [*binaries, *(["reference"] if args.time_reference else [])]
    result = {
        "binaries": {label: {"path": path, "sha256": hashlib.sha256(
            Path(path).read_bytes()).hexdigest()} for label, path in binaries.items()},
        "cpu": args.cpu, "input_bytes": args.bytes, "rounds": args.rounds,
        "qualification": "End-to-end wall time; affinity is not SMT isolation",
        "suite": args.suite,
        "verification": "Exact status, stderr and stdout SHA-256 on every run; hashing is outside timing",
        "references": {},
        "rows": [],
    }
    print("case\t" + "\t".join(f"{label} ms" for label in labels), flush=True)
    with tempfile.TemporaryDirectory(prefix="moonwater-engine-bench-") as work:
        root = Path(work)
        output_file = (os.fdopen(os.memfd_create("moonwater-engine-output"), "w+b")
                       if hasattr(os, "memfd_create") else tempfile.TemporaryFile(dir=root))
        with output_file as output:
            for name, tool, operands, source in selected_workloads(root, args.bytes, env, args.suite):
                if args.filter not in name:
                    continue
                reference = shutil.which(tool)
                if not reference:
                    raise SystemExit(f"missing reference tool: {tool}")
                if tool not in result["references"]:
                    version = subprocess.run([reference, "--version"], stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, env=env, timeout=5).stdout
                    result["references"][tool] = {"path": reference, "sha256": hashlib.sha256(
                        Path(reference).read_bytes()).hexdigest(), "version": version.decode(errors="replace").splitlines()[0]}
                expected = invoke(reference, tool, operands, output, env, source=source)
                if expected[0] or expected[2]:
                    raise RuntimeError((name, "reference failed", expected))
                invalid = {}
                current = dict(binaries, **({"reference": reference} if args.time_reference else {}))
                for label, binary in binaries.items():
                    actual = invoke(binary, tool, operands, output, env, source=source)
                    if actual != expected:
                        if label in args.allow_invalid:
                            invalid[label] = {"status": actual[0], "sha256": actual[1],
                                              "stderr": actual[2].decode(errors="replace")}
                            continue
                        raise RuntimeError((name, label, "output/status mismatch",
                                            actual, expected))
                valid = [label for label in labels if label not in invalid]
                samples = {label: [] for label in valid}
                cpu_samples = {label: [] for label in valid}
                for trial in range(args.rounds + 2):
                    if not valid:
                        break
                    offset = trial % len(valid)
                    order = valid[offset:] + valid[:offset]
                    if (trial // len(valid)) & 1:
                        order = order[::-1]
                    for label in order:
                        elapsed, cpu_ms = invoke(current[label], tool, operands,
                                                 output, env, timed=True, source=source,
                                                 expected=expected)
                        if trial >= 2:
                            samples[label].append(elapsed)
                            cpu_samples[label].append(cpu_ms)
                medians = {label: statistics.median(values)
                           for label, values in samples.items()}
                result["rows"].append({
                    "case": name, "median_ms": medians, "samples_ms": samples,
                    "cpu_median_ms": {label: statistics.median(values)
                                      for label, values in cpu_samples.items()},
                    "cpu_samples_ms": cpu_samples,
                    "applet": tool, "arguments": operands,
                    "stdin_sha256": hashlib.sha256(source.read_bytes()).hexdigest() if source else None,
                    "invalid": invalid,
                    "ratio_to_first": {label: value / medians[labels[0]]
                                       if labels[0] in medians else None
                                       for label, value in medians.items()},
                    "paired_ratio_to_first": {label: [value / samples[labels[0]][i]
                        for i, value in enumerate(values)] for label, values in samples.items()}
                        if labels[0] in samples else {},
                    "verified_sha256": expected[1],
                })
                print(name + "\t" + "\t".join(f"{medians[label]:.3f}"
                      if label in medians else "INVALID"
                      for label in labels), flush=True)
                if args.json:
                    args.json.write_text(json.dumps(result, indent=2) + "\n")
    if not result["rows"]:
        raise SystemExit("filter selected no workloads")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
