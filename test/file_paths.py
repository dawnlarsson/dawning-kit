#!/usr/bin/env python3
"""Vary symlink splice direction, tail length and canonicalization policy."""
import argparse
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("farm", type=Path)
    parser.add_argument("--reference", default=shutil.which("realpath"))
    parser.add_argument("--exact-errors", action="store_true")
    args = parser.parse_args()
    if not args.reference:
        parser.error("a reference realpath executable is required")
    candidate = str(args.farm.resolve() / "realpath")
    reference = os.path.abspath(args.reference)
    environment = dict(os.environ, LC_ALL="C", TZ="UTC0")
    passed = failed = 0

    with tempfile.TemporaryDirectory(prefix="moonwater-paths-") as temporary:
        root = Path(temporary)
        actual = root / "actual"
        (actual / "inner").mkdir(parents=True)
        (actual / "file").write_bytes(b"path fixture\n")
        # Alternating long/short link targets move the unread rest both ways.
        targets = ["actual", "./actual", "./" * 64 + "actual",
                   str(actual), "actual/inner/.."]
        links = []
        for index, target in enumerate(targets):
            link = root / ("link-" + str(index) + "x" * (index * 31))
            link.symlink_to(target)
            links.append(link.name)
        (root / "chain").symlink_to(links[2])
        links.append("chain")
        (root / "back").symlink_to(".")
        (root / "loop").symlink_to("loop")
        (root / "broken").symlink_to("absent")

        modes = [[], ["-e"], ["-m"], ["-L"], ["-P"], ["-s"],
                 ["-m", "-s"], ["-L", "-s"], ["-z"], ["-q"],
                 ["--relative-to=" + str(actual)],
                 ["--relative-base=" + str(root)]]
        tails = ["", "/", "/.", "/file", "//file", "/inner/../file",
                 "/inner/..", "/" + "./" * 96 + "file"]
        paths = [link + tail for link, tail in itertools.product(links, tails)]
        paths += ["", ".", "actual/file", "absent", "broken", "loop",
                  "back/" * 12 + links[2] + "/file"]
        # Quiet failures and successful operands together exercise one error
        # exit followed by reuse of the same command's scratch buffers.
        cases = [(mode, [path]) for mode, path in itertools.product(modes, paths)]
        cases += [(mode, ["absent/child", links[2] + "/file", "actual"])
                  for mode in modes]

        def run(executable, mode, operands):
            result = subprocess.run([executable, *mode, "--", *operands],
                                    cwd=root, env=environment, capture_output=True,
                                    timeout=5)
            channels = (result.returncode, result.stdout)
            return channels + (result.stderr,) if args.exact_errors else channels

        for mode, operands in cases:
            wanted = run(reference, mode, operands)
            got = run(candidate, mode, operands)
            if got == wanted:
                passed += 1
            else:
                failed += 1
                if failed <= 12:
                    print("path splice mismatch", mode, operands,
                          "wanted", wanted, "got", got)

    print(f"file path splices: {passed}/{passed + failed} checks")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
