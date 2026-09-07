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
    if len(binaries) < 2:
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


def invoke(binary, tool, operands, output, env, timed=False):
    output.seek(0)
    output.truncate()
    before = resource.getrusage(resource.RUSAGE_CHILDREN) if timed else None
    started = time.perf_counter_ns()
    with subprocess.Popen([tool, *operands], executable=binary, stdout=output,
                          stderr=subprocess.PIPE, env=env) as process:
        # communicate observes the stderr pipe closing; unlike wait(timeout),
        # it does not quantize fast commands with a sleep-based polling loop.
        _, errors = process.communicate(timeout=60)
    elapsed = (time.perf_counter_ns() - started) / 1e6
    if timed:
        if process.returncode or errors:
            raise RuntimeError((tool, operands, process.returncode, errors))
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu_ms = 1000 * (after.ru_utime + after.ru_stime -
                         before.ru_utime - before.ru_stime)
        return elapsed, cpu_ms
    output.seek(0)
    digest = hashlib.file_digest(output, "sha256").hexdigest()
    return process.returncode, digest, errors


def main():
    args, binaries = arguments()
    env = dict(os.environ, LC_ALL="C", TZ="UTC0")
    labels = list(binaries)
    result = {
        "binaries": {label: {"path": path, "sha256": hashlib.sha256(
            Path(path).read_bytes()).hexdigest()} for label, path in binaries.items()},
        "cpu": args.cpu, "input_bytes": args.bytes, "rounds": args.rounds,
        "qualification": "End-to-end wall time; affinity is not SMT isolation",
        "rows": [],
    }
    print("case\t" + "\t".join(f"{label} ms" for label in labels), flush=True)
    with tempfile.TemporaryDirectory(prefix="moonwater-engine-bench-") as work:
        root = Path(work)
        output_file = (os.fdopen(os.memfd_create("moonwater-engine-output"), "w+b")
                       if hasattr(os, "memfd_create") else tempfile.TemporaryFile(dir=root))
        with output_file as output:
            for name, tool, operands in workloads(root, args.bytes, env):
                if args.filter not in name:
                    continue
                reference = shutil.which(tool)
                if not reference:
                    raise SystemExit(f"missing reference tool: {tool}")
                expected = invoke(reference, tool, operands, output, env)
                if expected[0] or expected[2]:
                    raise RuntimeError((name, "reference failed", expected))
                invalid = {}
                for label, binary in binaries.items():
                    actual = invoke(binary, tool, operands, output, env)
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
                        elapsed, cpu_ms = invoke(binaries[label], tool, operands,
                                                 output, env, timed=True)
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
                    "invalid": invalid,
                    "ratio_to_first": {label: value / medians[labels[0]]
                                       if labels[0] in medians else None
                                       for label, value in medians.items()},
                    "verified_sha256": expected[1],
                })
                print(name + "\t" + "\t".join(f"{medians[label]:.3f}"
                      if label in medians else "INVALID"
                      for label in labels), flush=True)
    if not result["rows"]:
        raise SystemExit("filter selected no workloads")
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
