#!/bin/sh
#
#       Random patterns, ours against the system's.
#
#           sh programs/text_fuzz.sh [directory of our binaries] [rounds]
#
#       programs/text_check.sh is a list somebody wrote down, which means it
#       tests what that somebody thought of. This generates patterns instead,
#       and it is what found the one that mattered: a jump target that pointed
#       at the instruction a quantifier was about to be inserted in front of,
#       which broke \(a\)*\(a\)* and nothing simpler.
#
#       Needs python3, which is why it is separate from text_check.sh.

LC_ALL=C
export LC_ALL

bin=${1:-/tmp/sh-text/bin}
rounds=${2:-600}

command -v python3 > /dev/null 2>&1 || { echo "text_fuzz: needs python3"; exit 0; }

BIN=$bin ROUNDS=$rounds python3 - <<'PYTHON'
import os, random, subprocess

ours = os.environ["BIN"]
rounds = int(os.environ["ROUNDS"])
total = 0
bad = 0
shown = 0


def report(what, script, want, got, data):
    global shown
    shown += 1
    if shown <= 8:
        print("  %-6s %-28s want %r got %r" % (what, script, want, got))
        print("         on %r" % data)


def lines(count, longest, alphabet="abc"):
    return "\n".join(
        "".join(random.choice(alphabet) for _ in range(random.randint(0, longest)))
        for _ in range(count)
    ) + "\n"


def run(program, arguments, data):
    got = subprocess.run([program] + arguments, input=data,
                         capture_output=True, text=True)
    return got.stdout, got.returncode


def both(name, arguments, data):
    global total, bad
    total += 1
    want, want_status = run(name, arguments, data)
    mine, mine_status = run(ours + "/" + name, arguments, data)

    if want != mine or want_status != mine_status:
        bad += 1
        report(name, " ".join(arguments), want, mine, data)


basic = ["a", "b", "c", ".", "[ab]", "[^a]", "\\(a\\)", "\\(ab\\)", "[a-c]",
         "a*", "b*", ".*", "\\(a\\)*", "ab", "a\\|b", "\\(a\\|b\\)",
         "a\\{1,2\\}", "[abc]\\{2\\}", "a\\+", "b\\?", "\\(ab\\)*",
         "\\(a\\|bc\\)", "\\(ab\\|a\\)"]

extended = ["a", "b", "c", ".", "[ab]", "[^a]", "(a)", "(ab)", "[a-c]", "a*",
            "b*", ".*", "(a)*", "ab", "a|b", "(a|b)", "a{1,2}", "[abc]{2}",
            "a+", "b?", "(ab)+", "(a|b)*", "(a|bc)"]

random.seed(20260827)

for _ in range(rounds):
    pattern = "".join(random.choice(basic) for _ in range(random.randint(1, 3)))

    if random.random() < 0.25:
        pattern = "^" + pattern

    if random.random() < 0.25:
        pattern = pattern + "$"

    data = lines(8, 6)

    for flags in (["-c"], ["-n"], ["-ci"], ["-cv"]):
        both("grep", flags + [pattern], data)

for _ in range(rounds):
    pattern = "".join(random.choice(extended) for _ in range(random.randint(1, 3)))

    if random.random() < 0.25:
        pattern = "^" + pattern

    if random.random() < 0.25:
        pattern = pattern + "$"

    data = lines(8, 6)

    for flags in (["-cE"], ["-nE"], ["-cEi"]):
        both("grep", flags + [pattern], data)

for _ in range(rounds):
    pattern = "".join(random.choice(basic) for _ in range(random.randint(1, 3)))
    replacement = random.choice(["X", "[&]", "<\\1>", "", "Y&Y"])

    if "\\1" in replacement and "\\(" not in pattern:
        replacement = "X"

    data = lines(6, 6)

    for tail in ("", "g", "2", "gp"):
        both("sed", ["-n" if tail == "gp" else "-e",
                     "s/" + pattern + "/" + replacement + "/" + tail], data)

for _ in range(rounds // 2):
    data = lines(6, 6)

    for flags in (["-n"], ["-c"], ["-u"], ["-d"], ["-i"]):
        both("uniq", flags if flags != ["-n"] else [], data)

    for flags in (["-r"], ["-u"], ["-f"], ["-n"]):
        both("sort", flags, data)

    for width in ("1", "3", "5"):
        both("fold", ["-w", width], data)
        both("cut", ["-c" + width + "-"], data)

    both("rev", [], data)
    both("wc", [], data)
    both("nl", [], data)
    both("tr", ["a-c", "x-z"], data)
    both("tr", ["-d", "ab"], data)
    both("tr", ["-s", "abc"], data)

print("\n  %s of %s" % (total - bad, total))
PYTHON
