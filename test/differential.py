#!/usr/bin/env python3
"""One differential runner for every utility and both shell personalities.

    python3 test/differential.py FARM --domain text             the default budget
    python3 test/differential.py FARM --domain text --utility grep --budget full
    python3 test/differential.py FARM --domain shell --mode dash
    python3 test/differential.py FARM --replay ARTIFACT.json
    python3 test/differential.py --self-test

A spec module (spec_<domain>.py for each name in DOMAINS)
declares the surface of each program as a grammar: its options, the values an
option takes, the operand shapes it accepts and the inputs worth feeding it.
Nobody writes the cases. This walks the grammar -- every option alone, every
pair of option values together, every operand and input shape, and a seeded
random tier that goes deeper -- and runs each case through the system's tool
and through ours in the same recreated directory, comparing the exit status,
standard output, the effect on the directory and (per the spec's policy) the
diagnostic. Agreeing is passing; there is no separate idea of a right answer.

A spec may also declare FAMILIES: seeded generators of whole shell programs,
for the parts of a shell that are a language rather than an option list, and
CHECKS: functions check(farm) -> (passed, total, notes) for the few properties
a differential cannot express, such as a denominator of names or a census the
box cannot present.

Two pinned lists live at the end of this file, written by --record and never
by hand. The ledger records the cases where ours deliberately answers
differently, with the reason, and pins the answer: a row that starts agreeing
fails and says to remove it, a row whose answer drifts fails too. The
regressions record reproducers of bugs that were found and fixed, so a case
the random tier happened upon is run every time after.
"""

import argparse
import collections
import dataclasses
import hashlib
import importlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import resource
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import concurrent.futures

# Run as a script this module is __main__, so a spec's "from differential
# import ..." would load a second copy and see a different INPUTS and
# FIXTURES from the one the workers walk. Register this one under the name.
sys.modules.setdefault("differential", sys.modules[__name__])

HERE = Path(__file__).resolve().parent
PIN_FILE = Path(__file__).resolve()

# A spec says `from differential import INPUTS, FIXTURES` and adds its own
# shapes to them. Run as a script this file is the module __main__ (and a pool
# worker's copy is __mp_main__), so without this alias the spec would import
# a second copy of this file and its additions would land in the other one:
# a fixture it declared is not found, and an input it named feeds nothing.
if __name__ in ("__main__", "__mp_main__"):
    sys.modules.setdefault("differential", sys.modules[__name__])
PIN_BEGIN = "# ---- pinned rows begin (written by --record; never by hand) ----"
PIN_END = "# ---- pinned rows end ----"
OUTPUT_LIMIT = 1 << 20
DOMAINS = ("text", "files", "misc", "util_linux", "shell", "builtins", "awk")
SHELL_MODES = {"bash": ("/bin/bash", [], "bash"),
               "posix": ("/bin/bash", ["--posix"], "bash"),
               "dash": ("/bin/dash", [], "dash")}


# ----------------------------------------------------------------------------
#       Inputs and fixtures every spec can name.
#
#       The awkward shapes are here rather than in each spec because every
#       stream tool meets them: nothing at all, a last line never terminated,
#       lines that are empty rather than absent, a line wider than any buffer,
#       bytes that are not text, and the reader's own refill boundary.
# ----------------------------------------------------------------------------

def _boundary(size, byte=b"x"):
    return byte * size + b"\n"


INPUTS = {
    "text": b"alpha beta gamma\ndelta epsilon\n\nzeta eta theta iota\nalpha beta gamma\n",
    "empty": b"",
    "newline": b"\n",
    "nonl": b"alpha\nbeta",
    "blanks": b"\n\n\n",
    "spaces": b"  ab  cd ef\nxy\tzw\nplain\n \t \n",
    "tabs": b"a\tb  \n  c\t\n\td  e\naaa\tbbb\tccc\n",
    "crlf": b"one\r\ntwo\r\n\r\nthree\r\n",
    "fields": b"one:two:three\nfour:five:six\nnodelim\nseven::nine\n:lead\ntrail:\n",
    "numbers": b"10\n9\n100\n2\n-3\n2.5\n0\n1e3\n0x10\n+7\n 4\n007\n",
    "repeats": b"apple\napple\nbanana\ncherry\ncherry\ncherry\ndate\nApple\n",
    "mixed_case": b"Hello World\nHELLO world\nhello WORLD\nhElLo\n",
    "words": b"foo123bar\nbaz456qux\nnothing here\nFOO789BAR\nfoo\n",
    "sorted": b"a 1\nb 2\nc 3\nd 4\n",
    "unsorted": b"b 2 x\na 10 y\nc 1 z\na 3 w\nb 2 x\n",
    "high": b"caf\xc3\xa9\n\xff\xfe\n\x80\x81 tail\n",
    "nul": b"xa\x00yb\x00zc\x00",
    "nul_lines": b"a\x00b\nc\x00\n\x00\n",
    "controls": b"a\x01b\x7f\x1b[1mbold\x1b[0m\n\x08back\n\x0cff\n",
    "long": _boundary(70000),
    "wide_words": b" ".join(b"w%d" % n for n in range(3000)) + b"\n",
    "edge_65535": _boundary(65535),
    "edge_65536": _boundary(65536),
    "edge_65537": _boundary(65537),
    "edge_131072": _boundary(131072),
    "many_lines": b"".join(b"line %06d\n" % n for n in range(5000)),
    "blank_runs": b"x\n\n\ny\n\n\n\nz\n\n",
}

_BASIC = {
    "a.txt": b"alpha\nbeta\ngamma\n",
    "b.txt": b"two\nbeta\n",
    "c.txt": b"10\n9\n100\n2\n",
    "two words": b"spaced\n",
    ".hidden": b"hidden\n",
    "empty": b"",
    "nonl": b"no newline at the end",
    "dir/inside": b"nested\n",
    "dir/sub/deep": b"deeper\n",
    "input": b"one two\nthree\\four\nlast",
    "fields": INPUTS["fields"],
    "numbers": INPUTS["numbers"],
    "repeats": INPUTS["repeats"],
    "binary": bytes(range(256)) * 3,
    "link": ("link", "a.txt"),
    "dirlink": ("link", "dir"),
    "dangling": ("link", "nowhere"),
    "exe": ("mode", b"#!/bin/sh\nprintf 'ran:%s\\n' \"$*\"\n", 0o755),
    "unreadable": ("mode", b"secret\n", 0o000),
}

FIXTURES = {
    "basic": _BASIC,
    "none": {},
    "boundary": {**_BASIC,
                 "edge_65535": INPUTS["edge_65535"],
                 "edge_65536": INPUTS["edge_65536"],
                 "edge_65537": INPUTS["edge_65537"],
                 "long": INPUTS["long"],
                 "many": INPUTS["many_lines"]},
    "shell": {"a.txt": b"alpha\nbeta\n", "b.txt": b"two\n",
              "two words": b"spaced\n", ".hidden": b"hidden\n",
              "empty": b"", "dir/inside": b"nested\n",
              "input": b"one two\nthree\\four\nlast"},
}


# ----------------------------------------------------------------------------
#       The grammar a spec writes.
# ----------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Option:
    """One option. values=None is a flag; otherwise each value is tried.

    attached says how a value is written: False for "-n 2", True for "-n2"
    (or "--width=2"), None for both shapes in turn. repeat allows the option
    more than once in a random case, the way -e PATTERN is repeated."""
    spell: str
    values: tuple = None
    attached: bool = False
    repeat: bool = False
    weight: int = 1

    def forms(self):
        if self.values is None:
            return ((self.spell,),)
        out = []
        for value in self.values:
            shapes = (self.attached,) if self.attached is not None else (False, True)
            for attached in shapes:
                if attached:
                    glue = "=" if self.spell.startswith("--") else ""
                    out.append((self.spell + glue + value,))
                else:
                    out.append((self.spell, value))
        return tuple(out)


@dataclasses.dataclass(frozen=True)
class Utility:
    """The surface of one program.

    operands is a tuple of alternatives; each alternative is the tuple of
    words placed after the options. Words name files in the fixture set by
    their fixture path. stdin is the tuple of INPUTS names fed in turn.
    stderr is "loose" (both said something or neither), "exact" (byte for
    byte after sandbox paths are replaced) or "ignore". valid prunes an argv
    that cannot mean anything, so the budget is not spent on both tools
    rejecting the same nonsense. normalize(channel, data) rewrites both sides
    before comparison, for a column that is legitimately unstable.
    """
    name: str
    options: tuple = ()
    operands: tuple = ((),)
    stdin: tuple = ("text",)
    fixture: str = "basic"
    stderr: str = "loose"
    normalize: object = None
    valid: object = None
    timeout: float = 5.0
    reference: str = None
    env: tuple = ()
    max_flags: int = 4
    # For a program reached through a shell: modes to run in, and script(argv,
    # stdin_name) building the program text around the generated words.
    modes: tuple = ()
    script: object = None
    # Extra deterministic cases beyond the grammar: a tuple of argv tuples.
    extra: tuple = ()

    def key(self):
        return self.name


@dataclasses.dataclass
class Case:
    domain: str
    utility: str
    argv: list
    stdin: str = "text"
    fixture: str = "basic"
    mode: str = None
    family: str = None
    input_kind: str = "command"
    tier: str = "grammar"

    def identity(self):
        material = json.dumps([self.domain, self.utility, self.argv, self.stdin,
                               self.fixture, self.mode, self.input_kind],
                              separators=(",", ":"), ensure_ascii=False)
        return hashlib.sha256(material.encode()).hexdigest()[:16]

    def as_dict(self):
        return dataclasses.asdict(self)

    def words(self):
        return " ".join(shlex.quote(word) for word in self.argv)


# ----------------------------------------------------------------------------
#       Walking the grammar.
# ----------------------------------------------------------------------------

def covering_array(parameters, strength, rng):
    """Rows over parameter value indexes so every strength-tuple appears.

    Greedy: each new row starts as the best of a handful of seeded candidates
    by the number of still-uncovered tuples it hits, then is improved one
    column at a time, re-scoring only the column combinations that column
    takes part in. Deterministic for a seed, and small -- a dozen options
    with two values each cover pairwise in about fifteen rows, thirty
    three-valued options cover three-wise in about a hundred and twenty,
    where the products are thousands and millions."""
    sizes = [len(values) for values in parameters]
    if not sizes or strength < 1:
        return []
    strength = min(strength, len(sizes))
    # A program whose widest three parameters multiply into the thousands
    # has a three-wise cover of tens of thousands of tuples, and the greedy
    # builder spends hours on it. Those programs are covered pairwise; the
    # random tier is what reaches deeper into them.
    if strength > 2:
        widest = sorted(sizes, reverse=True)[:3]
        if widest[0] * widest[1] * widest[2] > 4000:
            strength = 2
    combos = list(itertools.combinations(range(len(sizes)), strength))
    uncovered = {c: set(itertools.product(*(range(sizes[i]) for i in c))) for c in combos}
    by_column = {i: [c for c in combos if i in c] for i in range(len(sizes))}
    remaining = sum(len(tuples) for tuples in uncovered.values())

    def hits(row, chosen):
        return sum(1 for c in chosen if tuple(row[i] for i in c) in uncovered[c])

    rows = []
    while remaining:
        best, best_hits = None, -1
        for _ in range(8):
            row = [rng.randrange(size) for size in sizes]
            score = hits(row, combos)
            if score > best_hits:
                best, best_hits = row, score
        for column in rng.sample(range(len(sizes)), len(sizes)):
            chosen = by_column[column]
            current = hits(best, chosen)
            for value in range(sizes[column]):
                if value == best[column]:
                    continue
                trial = list(best)
                trial[column] = value
                score = hits(trial, chosen)
                if score > current:
                    best, current = trial, score
        if hits(best, combos) == 0:
            # Nothing random reached the last tuples: take one directly.
            for c in combos:
                if uncovered[c]:
                    values = next(iter(sorted(uncovered[c])))
                    for i, v in zip(c, values):
                        best[i] = v
                    break
        for c in combos:
            key = tuple(best[i] for i in c)
            if key in uncovered[c]:
                uncovered[c].discard(key)
                remaining -= 1
        rows.append(best)
    return rows


def parameters_of(utility):
    """Each option becomes a parameter whose first value is 'absent'."""
    parameters = []
    for option in utility.options:
        parameters.append(((),) + option.forms())
    parameters.append(tuple(utility.operands))
    parameters.append(tuple(utility.stdin))
    return parameters


def assemble(utility, row, parameters):
    words = []
    count = len(utility.options)
    for index in range(count):
        words.extend(parameters[index][row[index]])
    operands = parameters[count][row[count]]
    stdin = parameters[count + 1][row[count + 1]]
    return list(words) + list(operands), stdin


def grammar_cases(domain, utility, budget, rng):
    """Singles, pairwise, the extra list, then the seeded deeper tier."""
    parameters = parameters_of(utility)
    count = len(utility.options)
    seen = set()

    def emit(argv, stdin, tier):
        if utility.valid and not utility.valid(argv):
            return
        case = Case(domain, utility.name, argv, stdin, utility.fixture, tier=tier)
        key = case.identity()
        if key in seen:
            return
        seen.add(key)
        yield case

    # Nothing at all, then every operand and input shape with no options.
    for operands in utility.operands:
        for stdin in utility.stdin:
            yield from emit(list(operands), stdin, "singles")
    # Every option form alone, on the first operand shape and input.
    for index in range(count):
        for form in parameters[index][1:]:
            yield from emit(list(form) + list(utility.operands[0]), utility.stdin[0], "singles")
    for argv in utility.extra:
        yield from emit(list(argv), utility.stdin[0], "extra")
    if budget == "singles":
        return
    # Triples only where the covering array stays tractable: past two dozen
    # parameters, or where the three largest of them multiply past a few
    # thousand, choosing strength-3 rows costs minutes each. The full
    # budget's deeper random tier already reaches those grammars.
    widest = sorted((len(values) for values in parameters), reverse=True)[:3]
    product = 1
    for size in widest:
        product *= size
    strength = 3 if budget == "full" and len(parameters) <= 24 and product <= 4000 else 2
    for row in covering_array(parameters, strength, random.Random(rng.random())):
        argv, stdin = assemble(utility, row, parameters)
        yield from emit(argv, stdin, "pairs" if strength == 2 else "triples")
    if budget == "full" and count and count <= 10:
        flags = [i for i, option in enumerate(utility.options) if option.values is None]
        for mask in range(1 << len(flags)):
            argv = []
            for bit, index in enumerate(flags):
                if mask >> bit & 1:
                    argv.extend(parameters[index][1])
            for operands in utility.operands:
                yield from emit(argv + list(operands), utility.stdin[0], "powerset")
    depth = {"quick": 2, "default": 4, "full": 12}[budget]
    for _ in range(depth * max(4, count * 2)):
        chosen = []
        for index in range(count):
            option = utility.options[index]
            if rng.random() < 0.5 * option.weight:
                chosen.append(rng.choice(parameters[index][1:]))
                if option.repeat and rng.random() < 0.3:
                    chosen.append(rng.choice(parameters[index][1:]))
        rng.shuffle(chosen)
        chosen = chosen[:utility.max_flags + 2]
        argv = [word for form in chosen for word in form]
        argv += list(rng.choice(utility.operands))
        yield from emit(argv, rng.choice(utility.stdin), "random")


def family_cases(domain, families, budget, seed):
    """rng-driven generators: each yields (family, modes, script)."""
    counts = {"singles": 24, "quick": 48, "default": 96, "full": 512}
    per = counts[budget]
    for name, generator in families:
        rng = random.Random(int.from_bytes(hashlib.sha256(
            f"{seed}:{name}".encode()).digest()[:8], "little"))
        seen = set()
        made = 0
        attempts = 0
        while made < per and attempts < per * 4:
            attempts += 1
            family, modes, script = generator(rng)
            for mode in modes:
                case = Case(domain, "shell", ["-c", script], "empty", "shell",
                            mode=mode, family=family, tier="family")
                key = case.identity()
                if key in seen:
                    continue
                seen.add(key)
                made += 1
                yield case


def shell_grammar_cases(domain, utility, budget, rng):
    """A builtin's grammar, wrapped into a script by the spec, per mode."""
    for case in grammar_cases(domain, utility, budget, rng):
        script = utility.script(case.argv, case.stdin)
        for mode in utility.modes:
            yield Case(domain, "shell", ["-c", script], case.stdin, "shell",
                       mode=mode, family=utility.name, tier=case.tier)


# ----------------------------------------------------------------------------
#       Running one case both ways.
# ----------------------------------------------------------------------------

def _limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (OUTPUT_LIMIT * 8, OUTPUT_LIMIT * 8))
    resource.setrlimit(resource.RLIMIT_CPU, (4, 4))
    resource.setrlimit(resource.RLIMIT_NOFILE, (256, 256))


def write_fixture(directory, fixture):
    """Entries: bytes, ("link", target[, stamp]), ("dir"[, mode[, stamp]]),
    ("mode", bytes, mode[, stamp]) or ("hard", source). A stamp is an epoch
    or an (atime, mtime) pair, so a listing's dates and a walk's ages are the
    same on both runs; "." names the directory itself. Stamps and directory
    modes are applied last, deepest first: making an entry moves its
    directory's time, and a directory without search permission hides what
    is below it."""
    later = []
    for name, contents in FIXTURES[fixture].items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(contents, tuple):
            kind = contents[0]
            if kind == "link":
                path.symlink_to(contents[1])
                later.append((path, None, contents[2] if len(contents) > 2 else None))
            elif kind == "dir":
                path.mkdir(exist_ok=True)
                later.append((path, contents[1] if len(contents) > 1 else None,
                              contents[2] if len(contents) > 2 else None))
            elif kind == "mode":
                path.write_bytes(contents[1])
                path.chmod(contents[2])
                later.append((path, None, contents[3] if len(contents) > 3 else None))
            elif kind == "hard":
                os.link(directory / contents[1], path)
        else:
            path.write_bytes(contents)
            path.chmod(0o644)
    for path, mode, stamp in sorted(later, key=lambda item: -len(item[0].parts)):
        if stamp is not None:
            times = (stamp, stamp) if isinstance(stamp, int) else tuple(stamp)
            os.utime(path, times, follow_symlinks=False)
        if mode is not None:
            path.chmod(mode)


def effects(directory):
    result = {}
    now = time.time()
    for path in sorted(directory.rglob("*")):
        name = str(path.relative_to(directory))
        try:
            info = path.lstat()
        except OSError:
            continue
        mode = stat.S_IMODE(info.st_mode)
        # A modification time far from now was set on purpose (touch -d,
        # cp -p, a stamped fixture kept or moved) and is part of the effect;
        # one near now is only when the run happened.
        stamp = int(info.st_mtime)
        deliberate = [stamp] if abs(stamp - now) > 60 else []
        if stat.S_ISLNK(info.st_mode):
            result[name] = ["link", os.readlink(path)] + deliberate
        elif stat.S_ISDIR(info.st_mode):
            result[name] = ["directory", mode] + deliberate
        elif stat.S_ISREG(info.st_mode):
            try:
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError:
                digest = "unreadable"
            result[name] = ["file", mode, info.st_size, digest] + deliberate
        else:
            result[name] = ["special", info.st_mode] + deliberate
    return result


class Runner:
    """Both programs in the same recreated directory, one after the other."""

    def __init__(self, farm, root, emulator=None, locale="C"):
        self.farm = Path(farm).resolve() if farm else None
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.emulator = emulator
        self.locale = locale
        self.names = self.root / "names"
        self.shell = None
        if self.farm:
            self.shell = self.resolve_shell()
        self.path = [p for p in os.environ.get("PATH", "/usr/bin:/bin").split(":")
                     if p and (not self.farm or Path(p).resolve() != self.farm)]

    def resolve_shell(self):
        for probe in ("cat", "echo", "ls", "sh"):
            link = self.farm / probe
            if link.exists():
                target = Path(os.path.realpath(link))
                break
        else:
            target = self.farm.parent / "shell"
        if not target.exists():
            return None
        self.names.mkdir(exist_ok=True)
        for name in ("bash", "sh", "dash"):
            link = self.names / name
            if not link.is_symlink():
                link.symlink_to(target)
        return target

    def reference_for(self, utility, spec):
        if spec is not None and spec.reference:
            return spec.reference
        for directory in self.path:
            candidate = Path(directory) / utility
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return str(candidate)
        return None

    def programs(self, case, spec):
        """(reference argv prefix, candidate argv prefix) or None if absent."""
        if case.mode:
            reference, flags, name = SHELL_MODES[case.mode]
            if not self.shell:
                return None
            return ([reference] + flags, [str(self.names / name)] + flags, name)
        reference = self.reference_for(case.utility, spec)
        candidate = self.farm / case.utility if self.farm else None
        if not reference or not candidate or not candidate.exists():
            return None
        return ([reference], [str(candidate)], case.utility)

    def run_one(self, prefix, argv0, case, spec, worker):
        directory = self.root / f"w{worker}"
        if directory.exists():
            shutil.rmtree(directory, ignore_errors=True)
            if directory.exists():
                # An unreadable directory left behind: make it removable.
                for path in directory.rglob("*"):
                    try:
                        path.chmod(0o700)
                    except OSError:
                        pass
                shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir()
        write_fixture(directory, case.fixture)
        argv = list(prefix) + list(case.argv)
        executable = argv[0]
        argv[0] = argv0
        if case.mode and case.input_kind != "command":
            script = case.argv[1]
            if case.input_kind == "file":
                source = self.root / f"script{worker}"
                source.write_text(script)
                argv = list(prefix) + [str(source)] + list(case.argv[2:])
                argv[0] = argv0
        if self.emulator and prefix[0].startswith(str(self.root)) or \
                (self.emulator and self.farm and prefix[0].startswith(str(self.farm))):
            argv = [self.emulator, "-0", argv0, executable] + argv[1:]
            executable = self.emulator
        environment = {"PATH": ":".join(self.path), "LC_ALL": self.locale,
                       "LANG": self.locale, "TZ": "UTC0", "HOME": str(directory),
                       "TMPDIR": str(directory), "COLUMNS": "80", "LINES": "24",
                       "TERM": "dumb"}
        if spec is not None:
            environment.update(dict(spec.env))
        timeout = spec.timeout if spec is not None else 5.0
        feed = INPUTS[case.stdin] if case.stdin in INPUTS else b""
        if case.mode and case.input_kind == "stdin":
            feed = case.argv[1].encode()
            argv = list(prefix) + list(case.argv[2:])
            argv[0] = argv0
        with tempfile.TemporaryFile(dir=self.root) as out, \
                tempfile.TemporaryFile(dir=self.root) as err, \
                tempfile.TemporaryFile(dir=self.root) as source:
            source.write(feed)
            source.seek(0)
            try:
                process = subprocess.Popen(argv, executable=executable, cwd=directory,
                                           env=environment, stdin=source, stdout=out,
                                           stderr=err, start_new_session=True,
                                           preexec_fn=_limits)
            except OSError as error:
                return {"status": -256, "timeout": False, "stdout": b"",
                        "stderr": str(error).encode(), "effects": {}}
            timed_out = False
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            out.seek(0)
            err.seek(0)
            result = {"status": process.returncode, "timeout": timed_out,
                      "stdout": out.read(OUTPUT_LIMIT + 1),
                      "stderr": err.read(OUTPUT_LIMIT + 1),
                      "effects": effects(directory)}
        result["stderr"] = result["stderr"].replace(str(directory).encode(), b"<DIR>")
        result["stdout"] = result["stdout"].replace(str(directory).encode(), b"<DIR>")
        if spec is not None and spec.normalize:
            for channel in ("stdout", "stderr"):
                result[channel] = spec.normalize(channel, result[channel])
        return result

    def pair(self, case, spec, worker=0):
        programs = self.programs(case, spec)
        if programs is None:
            return None, None
        reference, candidate, name = programs
        want = self.run_one(reference, name, case, spec, worker)
        got = self.run_one(candidate, name, case, spec, worker)
        return want, got


def loose_stderr(data):
    return b"said" if data.strip() else b""


def differences(want, got, policy="loose"):
    keys = []
    for key in ("status", "stdout", "effects", "timeout"):
        if want[key] != got[key]:
            keys.append(key)
    if policy == "exact":
        if _stderr_exact(want["stderr"]) != _stderr_exact(got["stderr"]):
            keys.append("stderr")
    elif policy == "loose":
        if loose_stderr(want["stderr"]) != loose_stderr(got["stderr"]):
            keys.append("stderr")
    return tuple(keys)


def _stderr_exact(data):
    # Both tools name themselves; GNU's spelling of its own path is not the
    # utility's behavior. Reduce "/usr/bin/grep: x" and "grep: x" to "x".
    return re.sub(rb"(?m)^(?:/[^\s:]*/)?([A-Za-z0-9_.+-]+): ", rb"\1: ", data)


def signature(want, got, policy):
    return (want["status"], got["status"], differences(want, got, policy))


def shrink(runner, case, spec, want, got, policy):
    """Drop words (or script lines) while the divergence signature holds."""
    target = signature(want, got, policy)
    if case.mode:
        lines = case.argv[1].splitlines(keepends=True)
        attempts = 0
        chunk = max(1, len(lines) // 2)
        while len(lines) > 1 and attempts < 40:
            changed = False
            for start in range(0, len(lines), chunk):
                smaller = lines[:start] + lines[start + chunk:]
                if not smaller:
                    continue
                attempts += 1
                trial = dataclasses.replace(case, argv=["-c", "".join(smaller)] + case.argv[2:])
                left, right = runner.pair(trial, spec)
                if left and not left["timeout"] and not right["timeout"] and \
                        signature(left, right, policy) == target:
                    lines, case, want, got = smaller, trial, left, right
                    changed = True
                    break
            if not changed:
                if chunk == 1:
                    break
                chunk = max(1, chunk // 2)
        return case, want, got
    words = list(case.argv)
    index = 0
    while index < len(words) and len(words) > 1:
        trial_words = words[:index] + words[index + 1:]
        trial = dataclasses.replace(case, argv=trial_words)
        left, right = runner.pair(trial, spec)
        if left and not left["timeout"] and not right["timeout"] and \
                signature(left, right, policy) == target:
            words, want, got = trial_words, left, right
            continue
        index += 1
    return dataclasses.replace(case, argv=words), want, got


# ----------------------------------------------------------------------------
#       The pinned lists.
# ----------------------------------------------------------------------------

def _pins_are_json():
    return PIN_FILE.suffix == ".json"


def _pin_span(text):
    """Where the pinned block sits: the markers on lines of their own, not
    the two constants above that spell them."""
    begin = text.index("\n" + PIN_BEGIN + "\n") + len(PIN_BEGIN) + 2
    end = text.index("\n" + PIN_END, begin)
    return begin, end


def load_rows(which):
    """The pinned rows of one list ("ledger" or "regression") from this file."""
    if _pins_are_json():
        rows = json.loads(PIN_FILE.read_text() or "[]") if PIN_FILE.exists() else []
        return [row for row in rows if row.get("list") == which]
    text = PIN_FILE.read_text()
    begin, end = _pin_span(text)
    block = text[begin:end].strip()
    block = block[len('PINNED = r"""'):] if block.startswith('PINNED = r"""') else block
    block = block[:-3] if block.endswith('"""') else block
    rows = json.loads(block or "[]")
    if not isinstance(rows, list):
        raise SystemExit(f"{PIN_FILE}: pinned rows must be a list")
    return [row for row in rows if row.get("list") == which]


def save_rows(which, rows):
    """Rewrite the block between the markers with these rows for one list."""
    text = PIN_FILE.read_text() if not _pins_are_json() else ""
    begin, end = _pin_span(text) if not _pins_are_json() else (0, 0)
    others = [row for row in load_rows("ledger") + load_rows("regression")
              if row.get("list") != which]
    for row in rows:
        row["list"] = which
    merged = sorted(others + rows,
                    key=lambda row: (row.get("list", ""), row.get("domain", ""),
                                     row.get("utility", ""), row.get("option", ""),
                                     row.get("id", "")))
    body = json.dumps(merged, indent=1, ensure_ascii=False, sort_keys=True)
    if _pins_are_json():
        PIN_FILE.write_text(body + "\n")
        return
    text = text[:begin] + 'PINNED = r"""\n' + body + '\n"""' + text[end:]
    PIN_FILE.write_text(text)


def row_case(row):
    case = row["case"]
    return Case(case["domain"], case["utility"], list(case["argv"]), case.get("stdin", "text"),
                case.get("fixture", "basic"), case.get("mode"), case.get("family"),
                case.get("input_kind", "command"), tier="pinned")


def digest(result):
    return {"status": result["status"],
            "stdout": hashlib.sha256(result["stdout"]).hexdigest(),
            "effects": hashlib.sha256(json.dumps(result["effects"], sort_keys=True).encode()).hexdigest()}


def option_words(utility, argv):
    """The words of an argv that are options rather than another option's value.

    A valued option written apart from its value -- ptx -M -O -- makes the
    next word that option's value, and a ledger row keyed on an option must
    not match it there. Without a spec every word is an option, which is what
    the matcher did before."""
    valued = set()
    if utility is not None:
        for option in utility.options:
            if option.values is not None:
                valued.add(option.spell)
    words = []
    skip = False
    for index, word in enumerate(argv):
        if skip:
            skip = False
            continue
        words.append(word)
        if word in valued:
            skip = True
    return words


def option_ledger(rows, case, utility=None):
    """A row keyed by option rather than by case: the whole option is refused."""
    words = option_words(utility, case.argv)
    for row in rows:
        if "option" not in row or row.get("utility") != case.utility:
            continue
        if row.get("domain") not in (None, case.domain):
            continue
        spell = row["option"]
        for word in words:
            if word == spell or (spell.startswith("--") and word.startswith(spell + "=")) \
                    or (not spell.startswith("--") and len(spell) == 2 and word.startswith(spell) and not word.startswith("--")):
                return row
    return None


# ----------------------------------------------------------------------------
#       Loading specs and building the case list.
# ----------------------------------------------------------------------------

def load_spec(domain):
    """The grammar of one domain: inlined below when SPECS exists, else a
    spec_<domain>.py module beside this file while a grammar is being written."""
    specs = globals().get("SPECS")
    if specs is not None and domain in specs:
        utilities, families = specs[domain]
        namespace = type("Spec", (), {})()
        namespace.UTILITIES = utilities
        namespace.FAMILIES = families
        return namespace
    if str(HERE) not in sys.path:
        sys.path.append(str(HERE))
    # A spec imports this module by its file name. Run as the program (or
    # as a pool worker's __mp_main__) that name would be a second copy with
    # its own INPUTS and FIXTURES, and what a spec adds to them would be
    # lost. Point the name at the module that is running.
    sys.modules.setdefault("differential", sys.modules[__name__])
    try:
        return importlib.import_module(f"spec_{domain}")
    except ModuleNotFoundError as error:
        if error.name == f"spec_{domain}":
            return None
        raise


def spec_utilities(spec):
    return {utility.name: utility for utility in getattr(spec, "UTILITIES", ())} if spec else {}


def build_cases(domain, spec, budget, seed, selected, modes, families):
    utilities = spec_utilities(spec)
    cases = []
    for name, utility in sorted(utilities.items()):
        if selected and name not in selected:
            continue
        # An input or fixture the spec misspelled would otherwise feed nothing
        # and pass, since both programs would agree about an empty input.
        for stdin in utility.stdin:
            if stdin not in INPUTS:
                raise SystemExit(f"spec_{domain}: {name} names unknown input {stdin!r}")
        if utility.fixture not in FIXTURES:
            raise SystemExit(f"spec_{domain}: {name} names unknown fixture {utility.fixture!r}")
        rng = random.Random(int.from_bytes(hashlib.sha256(
            f"{seed}:{domain}:{name}".encode()).digest()[:8], "little"))
        if utility.modes:
            for case in shell_grammar_cases(domain, utility, budget, rng):
                if not modes or case.mode in modes:
                    cases.append(case)
        else:
            cases.extend(grammar_cases(domain, utility, budget, rng))
    if spec and getattr(spec, "FAMILIES", None) and (not selected or "shell" in selected):
        chosen = [(f.__name__, f) for f in spec.FAMILIES
                  if not families or f.__name__ in families]
        for case in family_cases(domain, chosen, budget, seed):
            if not modes or case.mode in modes:
                cases.append(case)
    return cases, utilities


# ----------------------------------------------------------------------------
#       Parallel execution.
# ----------------------------------------------------------------------------

_worker_runner = None
_worker_specs = None


def _worker_init(farm, root, emulator, locale, domains):
    global _worker_runner, _worker_specs
    _worker_runner = Runner(farm, Path(root) / f"p{os.getpid()}", emulator, locale)
    _worker_specs = {domain: spec_utilities(load_spec(domain)) for domain in domains}


def _worker_run(payload):
    case = Case(**payload)
    spec = _worker_specs.get(case.domain, {}).get(case.family if case.mode else case.utility)
    want, got = _worker_runner.pair(case, spec)
    if want is None:
        return payload, None, None
    return payload, want, got


def serializable(result):
    return {key: (value.hex() if isinstance(value, bytes) else value)
            for key, value in result.items()}


def show(data, limit=160):
    text = data[:limit].decode("utf-8", "backslashreplace").replace("\n", "|")
    return text + ("..." if len(data) > limit else "")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("farm", nargs="?", type=Path,
                        help="directory of the names our shell answers to")
    parser.add_argument("--domain", action="append", choices=DOMAINS)
    parser.add_argument("--utility", action="append", help="only these programs (or 'shell')")
    parser.add_argument("--family", action="append", help="only these shell families")
    parser.add_argument("--mode", action="append", choices=tuple(SHELL_MODES))
    parser.add_argument("--budget", default=os.environ.get("MW_BUDGET", "default"),
                        choices=("singles", "quick", "default", "full"))
    parser.add_argument("--seed", type=lambda v: int(v, 0),
                        default=int(os.environ.get("MW_SEED", "0x4d574253"), 0))
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--locale", default="C")
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--input", dest="input_kind", choices=("command", "file", "stdin"),
                        default="command", help="how a shell case is handed its program")
    parser.add_argument("--replay", type=Path, action="append",
                        help="run the case saved in an artifact JSON")
    parser.add_argument("--artifacts", type=Path, default=os.environ.get("MW_ARTIFACTS"))
    parser.add_argument("--shrink", action="store_true")
    parser.add_argument("--max-failures", type=int, default=12,
                        help="distinct failure classes shown per program")
    parser.add_argument("--record", choices=("ledger", "regression"),
                        help="write every divergence (ledger) or every agreement of the "
                             "cases run (regression) into the pinned list")
    parser.add_argument("--reason", help="the reason written with --record ledger")
    parser.add_argument("--kind", default="deliberate", choices=("deliberate", "bug"))
    parser.add_argument("--no-pinned", action="store_true", help="skip ledger and regressions")
    parser.add_argument("--pins", type=Path, default=os.environ.get("MW_PINS"),
                        help="read and record pinned rows in this JSON file instead of "
                             "the block at the end of this program")
    parser.add_argument("--list", action="store_true", help="print the cases and stop")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--harness", nargs=argparse.REMAINDER,
                        help="run one of the folded standalone checks: --harness NAME [ARGS]")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.harness is not None:
        entry = globals().get("harness_entry")
        if entry is None:
            parser.error("no harness is folded into this file")
        sys.argv = [sys.argv[0]] + args.harness
        return entry()
    if args.pins:
        globals()["PIN_FILE"] = args.pins.resolve()
    if not args.farm and not args.list:
        parser.error("a farm directory is required")
    if args.record == "ledger" and not args.reason:
        parser.error("--record ledger needs --reason")

    domains = args.domain or list(DOMAINS)
    specs = {domain: load_spec(domain) for domain in domains}
    selected = set(args.utility or ())
    modes = set(args.mode or ())
    families = set(args.family or ())

    cases = []
    utilities = {}
    if args.replay:
        for path in args.replay:
            saved = json.loads(path.read_text())
            cases.append(row_case(saved))
        domains = sorted({case.domain for case in cases})
        specs = {domain: load_spec(domain) for domain in domains}
        for domain in domains:
            utilities.update(spec_utilities(specs[domain]))
    else:
        for domain in domains:
            made, found = build_cases(domain, specs[domain], args.budget, args.seed,
                                      selected, modes, families)
            cases.extend(made)
            utilities.update(found)
    for case in cases:
        if case.mode:
            case.input_kind = args.input_kind

    ledger = [] if args.no_pinned else load_rows("ledger")
    regressions = [] if args.no_pinned else load_rows("regression")
    pinned = {}
    if not args.no_pinned and not args.replay:
        for row in ledger + regressions:
            if "case" not in row or row["case"]["domain"] not in domains:
                continue
            case = row_case(row)
            if selected and (case.family if case.mode else case.utility) not in selected \
                    and case.utility not in selected:
                continue
            if modes and case.mode and case.mode not in modes:
                continue
            pinned[case.identity()] = (row, case)
    generated_ids = {case.identity() for case in cases}
    cases = [case for case in cases if case.identity() not in pinned]
    cases.extend(case for _, case in pinned.values())

    if args.list:
        for case in cases:
            label = f"{case.domain}/{case.family or case.utility}"
            if case.mode:
                label += f"[{case.mode}]"
            print(f"{case.identity()} {label} {case.tier:8} {case.words()}  <{case.stdin}")
        print(f"{len(cases)} cases")
        return 0

    farm = args.farm.resolve(strict=True)
    emulator = str(args.emulator.resolve(strict=True)) if args.emulator else None
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)

    print(f"  differential seed={hex(args.seed)} budget={args.budget} cases={len(cases)} "
          f"domains={','.join(domains)} jobs={args.jobs}; compares status, stdout, effects and diagnostics")

    passed = collections.Counter()
    total = collections.Counter()
    absent = collections.Counter()
    invalid = 0
    tiers = collections.Counter()
    failures = collections.defaultdict(collections.Counter)
    shown = collections.Counter()
    to_record = []
    ledger_rows_by_id = {row["id"]: row for row in ledger if "id" in row}
    regression_ids = {row["id"] for row in regressions if "id" in row}

    def label_of(case):
        name = case.family or case.utility
        return f"{case.domain}/{name}" + (f"[{case.mode}]" if case.mode else "")

    with tempfile.TemporaryDirectory(prefix="differential-") as temporary:
        runner = Runner(farm, Path(temporary) / "main", emulator, args.locale)
        payloads = [case.as_dict() for case in cases]
        if args.jobs > 1 and len(payloads) > 8:
            executor = concurrent.futures.ProcessPoolExecutor(
                max_workers=args.jobs, initializer=_worker_init,
                initargs=(str(farm), temporary, emulator, args.locale, domains))
            results = executor.map(_worker_run, payloads, chunksize=4)
        else:
            _worker_init(str(farm), temporary, emulator, args.locale, domains)
            executor = None
            results = map(_worker_run, payloads)
        for payload, want, got in results:
            case = Case(**payload)
            key = case.identity()
            name = case.family or case.utility
            spec = utilities.get(name)
            policy = spec.stderr if spec else "loose"
            tally_key = f"{case.domain}/{name}"
            if want is None:
                absent[tally_key] += 1
                continue
            total[tally_key] += 1
            tiers[case.tier] += 1
            diff = differences(want, got, policy)
            row = ledger_rows_by_id.get(key) or option_ledger(ledger, case, spec)
            if want["timeout"] or want["status"] < 0:
                invalid += 1
                print(f"  INVALID ORACLE {key} {label_of(case)} status={want['status']} "
                      f"timeout={want['timeout']}: {case.words()}")
                continue
            if row is not None and "case" in row:
                # A pinned deliberate difference: the answer must hold, and
                # must still differ from the reference.
                if not diff:
                    failures[tally_key][("ledger-agrees",)] += 1
                    print(f"  FAIL {key} {label_of(case)} agrees with the reference now -- "
                          f"remove its ledger row: {case.words()}")
                elif digest(got) != row["candidate"]:
                    failures[tally_key][("ledger-drift",)] += 1
                    print(f"  FAIL {key} {label_of(case)} ledger row drifted: {case.words()}")
                    print(f"       pinned {row['candidate']} now {digest(got)}")
                else:
                    passed[tally_key] += 1
                continue
            if row is not None and "option" in row:
                # A refused option: ours must refuse it (nonzero, silent).
                if got["status"] != 0 and not got["stdout"]:
                    passed[tally_key] += 1
                    continue
                if not diff:
                    failures[tally_key][("ledger-option-agrees",)] += 1
                    print(f"  FAIL {key} {label_of(case)} agrees with the reference although "
                          f"the ledger refuses {row['option']}: {case.words()}")
                    continue
            if not diff:
                passed[tally_key] += 1
                if args.record == "regression" and key not in regression_ids:
                    to_record.append((case, want, got))
                continue
            if key in regression_ids:
                diff = diff + ("regression",)
            sig = (want["status"], got["status"], diff)
            failures[tally_key][sig] += 1
            first = failures[tally_key][sig] == 1
            original = case
            if args.shrink and first and not want["timeout"] and not got["timeout"]:
                case, want, got = shrink(runner, case, spec, want, got, policy)
            if first and shown[tally_key] < args.max_failures:
                shown[tally_key] += 1
                print(f"  FAIL {key} {label_of(case)} tier={case.tier} differences={diff}")
                print(f"       {case.utility if not case.mode else case.mode} {case.words()}  <{case.stdin}  fixture={case.fixture}")
                print(f"       reference status={want['status']} stdout={show(want['stdout'])!r} stderr={show(want['stderr'], 100)!r}")
                print(f"       candidate status={got['status']} stdout={show(got['stdout'])!r} stderr={show(got['stderr'], 100)!r}")
                if "effects" in diff:
                    changed = sorted(set(want["effects"]) ^ set(got["effects"]) |
                                     {k for k in want["effects"] if k in got["effects"]
                                      and want["effects"][k] != got["effects"][k]})
                    print(f"       effects differ at {changed[:8]}")
            elif shown[tally_key] >= args.max_failures and first:
                shown[tally_key] += 1
                if shown[tally_key] == args.max_failures + 1:
                    print(f"  ... more distinct failures in {tally_key} not shown")
            if args.artifacts:
                artifact = {"id": key, "case": original.as_dict(), "shrunk": case.as_dict(),
                            "reference": serializable(want), "candidate": serializable(got),
                            "differences": diff}
                (args.artifacts / f"{key}.json").write_text(json.dumps(artifact, indent=1) + "\n")
            if args.record == "ledger":
                to_record.append((original, want, got))
        if executor:
            executor.shutdown()

    if args.record and to_record:
        rows = ledger if args.record == "ledger" else regressions
        known = {row.get("id") for row in rows}
        added = 0
        for case, want, got in to_record:
            key = case.identity()
            if key in known:
                continue
            row = {"id": key, "domain": case.domain, "utility": case.family or case.utility,
                   "case": case.as_dict()}
            if args.record == "ledger":
                row["reason"] = args.reason
                row["kind"] = args.kind
                row["candidate"] = digest(got)
                row["reference"] = digest(want)
            rows.append(row)
            known.add(key)
            added += 1
        save_rows(args.record, rows)
        print(f"  recorded {added} rows into {args.record}")

    if not args.replay:
        for domain in domains:
            for check in getattr(specs.get(domain), "CHECKS", ()):
                if selected and check.__name__ not in selected:
                    continue
                key = f"{domain}/{check.__name__}"
                won, count, notes = check(str(farm))
                passed[key] += won
                total[key] += count
                for note in notes:
                    failures[key][("check",)] += 1
                    print(f"  FAIL {key}: {note}")

    for key in sorted(set(total) | set(absent)):
        line = f"  {key:28} {passed[key]} of {total[key]}"
        if absent[key]:
            line += f"  ({absent[key]} NOT RUN -- no reference or candidate program)"
        print(line)
        if os.environ.get("TEST_TALLY") and total[key]:
            with open(os.environ["TEST_TALLY"], "a") as tally:
                tally.write(f"{key.replace('/', '-')} {passed[key]} {total[key]}\n")
    all_passed = sum(passed.values())
    all_total = sum(total.values())
    distinct = sum(len(v) for v in failures.values())
    print(f"  tiers: " + " ".join(f"{k}={v}" for k, v in sorted(tiers.items())))
    print(f"  differential {all_passed} of {all_total}; failure classes={distinct}, "
          f"invalid oracles={invalid}, not run={sum(absent.values())}")
    if not all_total:
        print("  differential NOT RUN -- no case had both programs")
        return 2
    return 1 if all_passed != all_total or invalid else 0


# ----------------------------------------------------------------------------
#       The oracle checked against deliberately wrong subjects.
# ----------------------------------------------------------------------------

def self_test():
    import unittest

    class Oracle(unittest.TestCase):
        def setUp(self):
            self.temporary = tempfile.TemporaryDirectory(prefix="differential-self-")
            self.root = Path(self.temporary.name)
            self.system = self.root / "system"
            self.farm = self.root / "farm"
            self.system.mkdir()
            self.farm.mkdir()
            self.script(self.system / "echoer", "#!/bin/sh\nprintf '%s\\n' \"$@\"; cat\n")
            self.script(self.farm / "echoer", "#!/bin/sh\nprintf '%s\\n' \"$@\"; cat\n")
            self.script(self.system / "maker", "#!/bin/sh\nprintf made > out; exit 3\n")
            self.script(self.farm / "maker", "#!/bin/sh\nprintf made > out; exit 3\n")
            self.script(self.farm / "wrong", "#!/bin/sh\nprintf wrong\n")
            self.script(self.system / "wrong", "#!/bin/sh\nprintf right\n")
            self.script(self.farm / "quiet", "#!/bin/sh\nexit 0\n")
            self.script(self.system / "quiet", "#!/bin/sh\nexit 1\n")
            self.script(self.farm / "chatty", "#!/bin/sh\necho oops >&2\n")
            self.script(self.system / "chatty", "#!/bin/sh\nexit 0\n")
            self.script(self.farm / "sleeper", "#!/bin/sh\nsleep 5\n")
            self.script(self.system / "sleeper", "#!/bin/sh\nexit 0\n")
            self.script(self.farm / "flagged", "#!/bin/sh\nfor a; do case $a in -z) exit 2;; esac; done; printf ok\n")
            self.script(self.system / "flagged", "#!/bin/sh\nprintf ok\n")
            self.script(self.farm / "effect", "#!/bin/sh\nprintf x > a.txt\n")
            self.script(self.system / "effect", "#!/bin/sh\nprintf y > a.txt\n")
            self.old_path = os.environ.get("PATH")
            os.environ["PATH"] = f"{self.system}:/usr/bin:/bin"
            # The inner runs must not write their rows into the suite's tally,
            # nor read the caller's pinned rows in place of their own fixtures.
            self.old_tally = os.environ.pop("TEST_TALLY", None)
            self.old_pins = os.environ.pop("MW_PINS", None)
            self.runner = Runner(self.farm, self.root / "run")

        def tearDown(self):
            os.environ["PATH"] = self.old_path
            if self.old_tally is not None:
                os.environ["TEST_TALLY"] = self.old_tally
            if self.old_pins is not None:
                os.environ["MW_PINS"] = self.old_pins
            self.temporary.cleanup()

        def script(self, path, text):
            path.write_text(text)
            path.chmod(0o755)

        def pair(self, utility, argv, stdin="text", fixture="basic"):
            case = Case("text", utility, argv, stdin, fixture)
            return case, self.runner.pair(case, None)

        def test_same_programs_agree_including_effects_and_input(self):
            _, (want, got) = self.pair("echoer", ["-a", "b c"], "fields")
            self.assertEqual(differences(want, got), ())
            self.assertEqual(want["stdout"], b"-a\nb c\n" + INPUTS["fields"])
            _, (want, got) = self.pair("maker", [])
            self.assertEqual(differences(want, got), ())
            self.assertEqual(want["status"], 3)
            self.assertEqual(got["effects"]["out"][2], 4)

        def test_directory_is_recreated_between_cases(self):
            self.pair("maker", [])
            _, (want, got) = self.pair("echoer", [])
            self.assertNotIn("out", got["effects"])
            self.assertIn("dir/sub/deep", got["effects"])
            self.assertEqual(got["effects"]["link"], ["link", "a.txt"])

        def test_wrong_subject_cannot_pass_each_channel(self):
            _, (want, got) = self.pair("wrong", [])
            self.assertEqual(differences(want, got), ("stdout",))
            _, (want, got) = self.pair("quiet", [])
            self.assertEqual(differences(want, got), ("status",))
            _, (want, got) = self.pair("effect", [])
            self.assertEqual(differences(want, got), ("effects",))
            _, (want, got) = self.pair("chatty", [])
            self.assertEqual(differences(want, got, "loose"), ("stderr",))
            self.assertEqual(differences(want, got, "ignore"), ())
            self.assertIn("stderr", differences(want, got, "exact"))

        def test_exact_stderr_drops_only_the_program_path(self):
            self.assertEqual(_stderr_exact(b"/usr/bin/grep: bad\ngrep: x\n"),
                             b"grep: bad\ngrep: x\n")
            self.assertNotEqual(_stderr_exact(b"grep: bad\n"), _stderr_exact(b"grep: worse\n"))

        def test_candidate_timeout_is_a_divergence(self):
            spec = Utility("sleeper", timeout=1.0)
            case = Case("text", "sleeper", [])
            want, got = self.runner.pair(case, spec)
            self.assertTrue(got["timeout"])
            self.assertIn("timeout", differences(want, got))

        def test_missing_reference_is_reported_not_passed(self):
            case = Case("text", "nowhere-such", [])
            self.assertEqual(self.runner.pair(case, None), (None, None))

        def test_covering_array_covers_every_pair(self):
            parameters = [(0, 1), (0, 1, 2), (0, 1), (0, 1, 2, 3), (0, 1)]
            rows = covering_array([tuple(range(len(p))) for p in parameters], 2, random.Random(7))
            for a, b in itertools.combinations(range(len(parameters)), 2):
                for va in range(len(parameters[a])):
                    for vb in range(len(parameters[b])):
                        self.assertTrue(any(row[a] == va and row[b] == vb for row in rows),
                                        (a, b, va, vb))
            self.assertLess(len(rows), 12 * 4)
            again = covering_array([tuple(range(len(p))) for p in parameters], 2, random.Random(7))
            self.assertEqual(rows, again)

        def test_grammar_walk_is_deterministic_and_complete_for_singles(self):
            spec = Utility("flagged", options=(Option("-a"), Option("-z"), Option("-n", ("1", "2"), None)),
                           operands=((), ("a.txt",)), stdin=("text", "empty"))
            first = list(grammar_cases("text", spec, "default", random.Random(1)))
            second = list(grammar_cases("text", spec, "default", random.Random(1)))
            self.assertEqual([c.identity() for c in first], [c.identity() for c in second])
            words = {tuple(c.argv) for c in first}
            for single in (("-a",), ("-z",), ("-n", "1"), ("-n2",), ("a.txt",), ()):
                self.assertIn(single, words)
            self.assertTrue(any("-a" in c.argv and "-z" in c.argv for c in first))
            self.assertEqual(len(first), len({c.identity() for c in first}))

        def test_option_ledger_matches_spellings_and_refusal(self):
            rows = [{"utility": "flagged", "option": "-z", "reason": "deliberate"}]
            case = Case("text", "flagged", ["-a", "-z"])
            self.assertIsNotNone(option_ledger(rows, case))
            self.assertIsNone(option_ledger(rows, Case("text", "flagged", ["-a"])))
            self.assertIsNotNone(option_ledger([{"utility": "x", "option": "--w"}],
                                               Case("text", "x", ["--w=3"])))
            _, (want, got) = self.pair("flagged", ["-z"])
            self.assertNotEqual(differences(want, got), ())
            self.assertTrue(got["status"] != 0 and not got["stdout"])

        def test_shrink_keeps_the_signature(self):
            case, (want, got) = self.pair("flagged", ["-a", "-b", "-z", "-c"])
            smaller, want2, got2 = shrink(self.runner, case, None, want, got, "loose")
            self.assertEqual(signature(want2, got2, "loose"), signature(want, got, "loose"))
            self.assertIn("-z", smaller.argv)
            self.assertLess(len(smaller.argv), 4)

        def test_main_end_to_end_with_recording(self):
            spec_dir = self.root / "specs"
            spec_dir.mkdir()
            (spec_dir / "spec_text.py").write_text(
                "from differential import Utility, Option\n"
                "UTILITIES = (Utility('flagged', options=(Option('-a'), Option('-z')), stdin=('text',)),\n"
                "             Utility('wrong'),)\n")
            pins = self.root / "pins.py"
            pins.write_text("\n" + PIN_BEGIN + '\nPINNED = r"""\n[]\n"""\n' + PIN_END + "\n")
            saved = PIN_FILE
            globals_ = globals()
            globals_["PIN_FILE"] = pins
            sys.path.insert(0, str(spec_dir))
            sys.modules.pop("spec_text", None)
            saved_specs = globals_.get("SPECS")
            globals_["SPECS"] = None
            try:
                import io
                import contextlib
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    status = main([str(self.farm), "--domain", "text", "--budget", "singles",
                                   "--jobs", "1", "--record", "ledger", "--reason", "self-test",
                                   "--kind", "bug"])
                self.assertEqual(status, 1)
                text = out.getvalue()
                self.assertIn("FAIL", text)
                self.assertIn("text/wrong", text)
                rows = load_rows("ledger")
                self.assertTrue(rows)
                self.assertTrue(all(row["reason"] == "self-test" for row in rows))
                # With the divergences pinned, the same run passes.
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    status = main([str(self.farm), "--domain", "text", "--budget", "singles", "--jobs", "1"])
                self.assertEqual(status, 0, out.getvalue())
                # A pinned row that starts agreeing fails and says so.
                self.script(self.farm / "wrong", "#!/bin/sh\nprintf right\n")
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    status = main([str(self.farm), "--domain", "text", "--budget", "singles", "--jobs", "1"])
                self.assertEqual(status, 1)
                self.assertIn("agrees with the reference now", out.getvalue())
                # Regressions are recorded from agreements and rerun.
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    main([str(self.farm), "--domain", "text", "--budget", "singles", "--jobs", "1",
                          "--utility", "flagged", "--record", "regression", "--no-pinned"])
                self.assertTrue(load_rows("regression"))
                self.assertTrue(load_rows("ledger"))
                self.assertIn("recorded", out.getvalue())
            finally:
                globals_["PIN_FILE"] = saved
                globals_["SPECS"] = saved_specs
                sys.path.remove(str(spec_dir))
                sys.modules.pop("spec_text", None)

        def test_tally_is_written_per_program(self):
            tally = self.root / "tally"
            spec_dir = self.root / "specs2"
            spec_dir.mkdir()
            (spec_dir / "spec_text.py").write_text(
                "from differential import Utility\nUTILITIES = (Utility('echoer'),)\n")
            sys.path.insert(0, str(spec_dir))
            sys.modules.pop("spec_text", None)
            old = os.environ.get("TEST_TALLY")
            os.environ["TEST_TALLY"] = str(tally)
            saved_specs = globals().get("SPECS")
            globals()["SPECS"] = None
            try:
                import io
                import contextlib
                with contextlib.redirect_stdout(io.StringIO()):
                    status = main([str(self.farm), "--domain", "text", "--budget", "singles",
                                   "--jobs", "1", "--no-pinned"])
                self.assertEqual(status, 0)
                self.assertRegex(tally.read_text(), r"text-echoer (\d+) \1\n")
            finally:
                globals()["SPECS"] = saved_specs
                if old is None:
                    del os.environ["TEST_TALLY"]
                else:
                    os.environ["TEST_TALLY"] = old
                sys.path.remove(str(spec_dir))
                sys.modules.pop("spec_text", None)

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Oracle)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    if os.environ.get("TEST_TALLY"):
        with open(os.environ["TEST_TALLY"], "a") as tally:
            passed = result.testsRun - len(result.failures) - len(result.errors)
            tally.write(f"differential-oracle {passed} {result.testsRun}\n")
    return 0 if result.wasSuccessful() else 1



# ---- pinned rows begin (written by --record; never by hand) ----
PINNED = r"""
[]
"""
# ---- pinned rows end ----


if __name__ == "__main__":
    sys.exit(main())
