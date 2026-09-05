#!/usr/bin/env python3
"""Seeded, bounded differential shell tests over shared domain generators.

This is a discovery lane, not a list of known-good examples. A case is run
against both personalities in the same recreated directory, with output,
status and filesystem effects compared. Failures print the seed and script;
--artifacts retains byte-exact reproducers and binary identities outside the
test runner's temporary directory. --shrink reduces failures by whole lines
without changing their status pair or mismatch category.
"""

import argparse
import collections
import hashlib
import importlib
import json
import os
from pathlib import Path
import random
import resource
import shutil
import signal
import subprocess
import sys
import tempfile


MODULES = ("shell_cases_expand", "shell_cases_exec", "shell_cases_lex", "shell_cases_builtin")
MODES = {"bash": ("/bin/bash", []),
         "posix": ("/bin/bash", ["--posix"]),
         "dash": ("/bin/dash", [])}
FIXTURES = {"a.txt": b"alpha\nbeta\n", "b.txt": b"two\n",
            "two words": b"spaced\n", ".hidden": b"hidden\n",
            "empty": b"", "dir/inside": b"nested\n",
            "input": b"one two\nthree\\four\nlast"}
OUTPUT_LIMIT = 131072


def identity(path):
    return {"path": str(path), "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}


def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (OUTPUT_LIMIT, OUTPUT_LIMIT))
    resource.setrlimit(resource.RLIMIT_CPU, (3, 3))
    resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))


def effects(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        name = str(path.relative_to(directory))
        if path.is_symlink():
            result[name] = ["link", os.readlink(path)]
        elif path.is_dir():
            result[name] = ["directory", path.stat().st_mode & 0o777]
        elif path.is_file():
            result[name] = ["file", path.stat().st_mode & 0o777,
                            path.stat().st_size,
                            hashlib.sha256(path.read_bytes()).hexdigest()]
        else:
            result[name] = ["special", path.lstat().st_mode]
    return result


class Runner:
    def __init__(self, subject, root, timeout, emulator=None, input_kind="command"):
        self.subject = subject
        self.root = root
        self.directory = root / "case"
        self.timeout = timeout
        self.emulator = emulator
        self.input_kind = input_kind

    def run(self, mode, script, candidate):
        if self.directory.exists():
            shutil.rmtree(self.directory)
        self.directory.mkdir()
        for name, contents in FIXTURES.items():
            path = self.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
            path.chmod(0o644)
        reference, flags = MODES[mode]
        executable = self.subject if candidate else reference
        argv = ["dash" if mode == "dash" else "bash", *flags]
        if self.input_kind == "command":
            argv += ["-c", script, "generated"]
        elif self.input_kind == "file":
            source = self.root / "generated-script"
            source.write_text(script)
            argv += [str(source)]
        if candidate and self.emulator:
            argv = [self.emulator, "-0", argv[0], self.subject, *argv[1:]]
            executable = self.emulator
        environment = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "TZ": "UTC",
                       "HOME": str(self.directory), "TMPDIR": str(self.directory)}
        with tempfile.TemporaryFile(dir=self.root) as out, \
             tempfile.TemporaryFile(dir=self.root) as err, \
             tempfile.TemporaryFile(dir=self.root) as source_input:
            if self.input_kind == "stdin":
                source_input.write(script.encode())
                source_input.seek(0)
            process = subprocess.Popen(argv, executable=executable, cwd=self.directory,
                                       env=environment, stdin=source_input,
                                       stdout=out, stderr=err, start_new_session=True,
                                       preexec_fn=limits)
            timed_out = False
            try:
                process.wait(timeout=self.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
            finally:
                # A completed parent can leave a background descendant alive.
                # Kill only this case's new process group, then reap the parent.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            out.seek(0)
            err.seek(0)
            return {"status": process.returncode, "timeout": timed_out,
                    "stdout": out.read(OUTPUT_LIMIT + 1),
                    "stderr": err.read(OUTPUT_LIMIT + 1),
                    "effects": effects(self.directory)}

    def pair(self, mode, script):
        return self.run(mode, script, False), self.run(mode, script, True)


def differences(want, got):
    # Diagnostics have shell-specific prefixes/wording; retain them for the
    # reproducer but do not mistake those differences for execution bugs.
    return tuple(key for key in ("status", "stdout", "effects", "timeout")
                 if want[key] != got[key])


def signature(want, got):
    return want["status"], got["status"], differences(want, got)


def shrink(runner, mode, script, want, got):
    lines = script.splitlines(keepends=True)
    target = signature(want, got)
    attempts = 0
    chunk = max(1, len(lines) // 2)
    while len(lines) > 1 and attempts < 32:
        changed = False
        for start in range(0, len(lines), chunk):
            smaller = lines[:start] + lines[start + chunk:]
            if not smaller:
                continue
            attempts += 1
            left, right = runner.pair(mode, "".join(smaller))
            if not left["timeout"] and not right["timeout"] and signature(left, right) == target:
                lines, want, got = smaller, left, right
                changed = True
                break
            if attempts >= 32:
                break
        if not changed:
            if chunk == 1:
                break
            chunk = max(1, chunk // 2)
    return "".join(lines), want, got


def serializable(result):
    return {key: value.hex() if isinstance(value, bytes) else value
            for key, value in result.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("subject", type=Path)
    parser.add_argument("--seed", action="append", type=lambda value: int(value, 0))
    parser.add_argument("--cases", type=int, default=int(os.environ.get("MW_SHELL_CASES", "96")),
                        help="case budget per domain and seed; generators balance their families")
    parser.add_argument("--domain", action="append", choices=("expand", "exec", "lex", "builtin"))
    parser.add_argument("--mode", action="append", choices=tuple(MODES))
    parser.add_argument("--input", dest="input_kind", choices=("command", "file", "stdin"),
                        help="execute with -c (default), a script file, or standard input")
    parser.add_argument("--case", help="replay a printed case identifier")
    parser.add_argument("--replay", type=Path,
                        help="run the exact script/mode saved in an artifact JSON")
    parser.add_argument("--timeout", type=float, default=4)
    parser.add_argument("--max-failures", type=int, default=20,
                        help="stop after this many distinct family/mode/status failure classes")
    parser.add_argument("--emulator", type=Path,
                        help="qemu-user binary for a cross-architecture subject")
    parser.add_argument("--artifacts", type=Path, default=os.environ.get("MW_SHELL_ARTIFACTS"))
    parser.add_argument("--shrink", action="store_true")
    args = parser.parse_args()
    if args.replay and (args.case or args.domain or args.mode or args.seed or args.input_kind):
        parser.error(
            "--replay cannot be combined with --case, --domain, --mode, --seed or --input")
    subject = args.subject.resolve(strict=True)
    emulator = str(args.emulator.resolve(strict=True)) if args.emulator else None
    seeds = args.seed or [int(value, 0) for value in
                          os.environ.get("MW_SHELL_SEEDS", "0x4d574253").split(",")]
    if args.cases < 1 or args.timeout <= 0 or args.max_failures < 1:
        parser.error("case budget, timeout and failure limit must be positive")
    for reference, _ in MODES.values():
        if not os.access(reference, os.X_OK):
            print(f"  shell_generated NOT RUN -- missing {reference}")
            return 2
    if args.artifacts:
        args.artifacts = args.artifacts.resolve()
        args.artifacts.mkdir(parents=True, exist_ok=True)
    if args.replay:
        try:
            saved = json.loads(args.replay.resolve(strict=True).read_text())
            saved_family = saved["family"]
            saved_mode = saved["mode"]
            saved_script = saved["script"]
            saved_seed = saved["seed"]
            args.input_kind = saved.get("input", "command")
            if (not isinstance(saved_family, str) or saved_mode not in MODES or
                    not isinstance(saved_script, str) or
                    not isinstance(saved_seed, int) or
                    args.input_kind not in ("command", "file", "stdin")):
                raise ValueError("invalid field type")
        except (OSError, ValueError, KeyError, TypeError,
                json.JSONDecodeError) as error:
            parser.error(f"invalid replay artifact: {error}")

        class SavedArtifact:
            __name__ = "saved_artifact"

            @staticmethod
            def cases(rng, budget):
                del rng, budget
                yield saved_family, (saved_mode,), saved_script

        modules = [SavedArtifact]
        seeds = [saved_seed]
    else:
        modules = [importlib.import_module(name) for name in MODULES
                   if not args.domain or
                   name.removeprefix("shell_cases_") in args.domain]
    passed = failed = invalid = 0
    args.input_kind = args.input_kind or "command"
    coverage = collections.Counter()
    failures = collections.Counter()
    seen = set()
    identities = {"subject": identity(subject), "bash": identity("/bin/bash"),
                  "dash": identity("/bin/dash")}
    if emulator:
        identities["emulator"] = identity(emulator)
    print("  generated seeds=" + ",".join(hex(seed) for seed in seeds) +
          f" budget={args.cases}/domain input={args.input_kind}; compares status, stdout and filesystem effects")
    with tempfile.TemporaryDirectory(prefix="moonwater-generated-") as temporary:
        runner = Runner(str(subject), Path(temporary), args.timeout, emulator, args.input_kind)
        for seed in seeds:
            for module in modules:
                # Per-domain randomness stays stable when another domain gains
                # cases, or when --domain is used to reproduce one failure.
                domain_seed = int.from_bytes(hashlib.sha256(
                    f"{seed}:{module.__name__}".encode()).digest()[:8], "little")
                for family, modes, script in module.cases(random.Random(domain_seed), args.cases):
                    for mode in modes:
                        if args.mode and mode not in args.mode:
                            continue
                        if mode not in MODES:
                            raise ValueError(f"invalid generator mode: {mode}")
                        identity_input = mode + "\0" + script
                        if args.input_kind != "command":
                            identity_input += "\0" + args.input_kind
                        key = hashlib.sha256(identity_input.encode()).hexdigest()[:16]
                        if key in seen or (args.case and args.case != key):
                            continue
                        seen.add(key)
                        want, got = runner.pair(mode, script)
                        coverage[f"{family}/{mode}"] += 1
                        if want["timeout"] or want["status"] < 0:
                            invalid += 1
                            print(f"  INVALID ORACLE {key} {family}/{mode} status={want['status']} timeout={want['timeout']}")
                        elif not differences(want, got):
                            passed += 1
                            continue
                        else:
                            failed += 1
                        failure = (family, mode, signature(want, got))
                        failures[failure] += 1
                        first_failure = failures[failure] == 1
                        original = script
                        if args.shrink and first_failure and not want["timeout"] and not got["timeout"]:
                            script, want, got = shrink(runner, mode, script, want, got)
                        if first_failure:
                            print(f"  FAIL seed={hex(seed)} case={key} family={family} mode={mode} differences={differences(want, got)}")
                            print(f"    reference status={want['status']} stdout={want['stdout'][:512]!r}")
                            print(f"    candidate status={got['status']} stdout={got['stdout'][:512]!r}")
                            print("    script=" + repr(script))
                        else:
                            # Keep grouped diagnostics compact without hiding
                            # any failing program's directly replayable ID.
                            print(f"  ALSO FAIL seed={hex(seed)} case={key} family={family} mode={mode} differences={differences(want, got)}")
                        if args.artifacts:
                            base = args.artifacts / key
                            base.with_suffix(".sh").write_text(script)
                            base.with_suffix(".json").write_text(json.dumps({
                                "seed": seed, "budget": args.cases, "case": key,
                                "input": args.input_kind,
                                "family": family, "mode": mode, "original": original,
                                "script": script, "identities": identities,
                                "reference": serializable(want), "candidate": serializable(got),
                            }, indent=2) + "\n")
                        if len(failures) >= args.max_failures:
                            break
                    if len(failures) >= args.max_failures:
                        break
                if len(failures) >= args.max_failures:
                    break
            if len(failures) >= args.max_failures:
                break
    for family, count in sorted(coverage.items()):
        print(f"  variance {family}: {count}")
    total = passed + failed + invalid
    for (family, mode, failure), count in sorted(failures.items()):
        print(f"  divergence {family}/{mode} {failure}: {count} cases")
    print(f"  generated {passed} of {total}; divergences={failed}, invalid_oracles={invalid}")
    if len(failures) >= args.max_failures:
        print("  STOPPED at failure limit; remaining generated cases were NOT RUN")
    if not total:
        print("  shell_generated NOT RUN -- selection produced no cases")
        return 2
    if os.environ.get("TEST_TALLY"):
        with open(os.environ["TEST_TALLY"], "a") as tally:
            tally.write(f"shell_generated {passed} {total}\n")
    return 1 if failed or invalid else 0


if __name__ == "__main__":
    sys.exit(main())
