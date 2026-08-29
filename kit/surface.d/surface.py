#!/usr/bin/env python3
"""
Every flag the system's tool has, against ours.

Mutation testing asks whether the tests can see a change to code that is
here. This asks the other question: what is not here at all. A flag never
implemented, an exit status never emitted and a divergence nobody wrote down
are all invisible to a differential suite, because nothing ever calls them.

So the flags are taken from the system tool's own --help rather than from a
list somebody maintains, and each one is run through both.

    python3 kit/surface.d/surface.py --bin <dir-of-ours>
    python3 kit/surface.d/surface.py --bin <dir> --tool grep

Four verdicts:

    agrees    same bytes and same status
    differs   both ran, and disagreed
    absent    ours refused the flag
    crashed   ours died on it

differs and absent are not automatically wrong. A tool that deliberately
does less should say so once, in ledger.json, with the reason -- the way
src/test/shell.sh keeps its own list of what it does differently from dash
and why. What this refuses to allow is a divergence nobody has looked at.
"""
import argparse, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = os.path.join(HERE, 'ledger.json')

#
#       A small input each tool can say something about, and any argument it
#       needs before it will run at all. Kept deliberately plain: the point
#       is to reach the flag, not to exercise the tool.
#
FEED = "banana\napple\ncherry\napple\n\tdate  \n42\n"

NEEDS = {
    'grep': ['a'],
    'sed':  ['s/a/A/'],
    'cut':  ['-c1-3'],
    'tr':   ['a', 'A'],
    'ln':   [],
    'seq':  ['3'],
}

# Flags that say something about the machine or the clock rather than about
# the input, so two runs never agree and the answer means nothing.
UNSTABLE = {'--help', '--version', '--random-sort', '--parallel',
            '--temporary-directory', '-T', '--compress-program',
            '--time-style', '--full-time'}

# Short options are not globally meaningful: grep -R is recursive while sort
# -R is random.  Keep aliases whose output is inherently unstable scoped to
# the tool which defines them, so they cannot hide another tool's real flag.
UNSTABLE_BY_TOOL = {
    'diff': {'-v'},
    'grep': {'-V'},
    'rev': {'-h', '-V'},
    'sort': {'-R'},
}

# GNU documents these as --name[=VALUE]. Passing VALUE as the next word does
# not exercise the option; it becomes an operand, and both tools can then fail
# for an extra-operand error while the option remains completely unsupported.
OPTIONAL_EQUALS = {
    ('diff', '--context'), ('diff', '--unified'), ('diff', '--color'),
}


#
#       A plausible value for each shape of argument --help names, so that a
#       flag which takes one is reached rather than rejected for the wrong
#       reason. Without this every argument-taking flag reads as a difference
#       and the report is mostly noise about the probe.
#
SAMPLES = {
    'NUM': '2', 'N': '2', 'COUNT': '2', 'SIZE': '2', 'WIDTH': '4',
    'NUMBER': '2', 'K': '1', 'BYTES': '2', 'LINES': '2', 'DIGITS': '2',
    'CHAR': 'a', 'CC': 'a', 'C': 'a', 'SEP': ',', 'STRING': 'a',
    'STYLE': 'a', 'FORMAT': 'rn', 'WORD': 'always', 'TYPE': 'a',
    'WHEN': 'always',
    'LIST': '1', 'FIELDS': '1', 'RANGE': '1', 'PATTERN': 'a',
    'PATTERNS': 'a', 'FILE': '/dev/null', 'SUFFIX': '.bak',
    'EXPR': 'a', 'SCRIPT': 's/a/A/', 'PROGRAM': 'true', 'NAME': 'a',
    'OPT': 'a', 'KEYDEF': '1', 'PREFIX': 'x', 'DIR': '/tmp',
}

def sample_for(placeholder):
    key = placeholder.strip('[]<>=').upper()
    if key in SAMPLES:
        return SAMPLES[key]
    for k, v in SAMPLES.items():
        if k in key:
            return v
    return '1'


def flags_of(tool):
    """
    What the system's own tool says it takes, and whether each takes a value.

    Read out of --help rather than kept in a list here, so a tool that grows
    a flag is noticed without anybody remembering to come back and add it.
    """
    try:
        r = subprocess.run([tool, '--help'], capture_output=True, text=True,
                           timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return []
    text = r.stdout + r.stderr

    found, seen = [], set()
    for line in text.split('\n'):
        # the shapes GNU writes: "-w, --width=COLS", "--long[=WHEN]", "-n NUM"
        for m in re.finditer(
                r'(?<![\w-])(-[a-zA-Z]|--[a-z][a-z0-9-]*)'
                r'(?:(=|\[=|\s)([A-Z][A-Z_]*)\]?)?', line):
            flag, sep, arg = m.group(1), m.group(2), m.group(3)
            if (flag in UNSTABLE or flag in UNSTABLE_BY_TOOL.get(tool, set())
                    or flag in seen):
                continue
            # a bare "-x" followed by a space and a capitalised word is only an
            # argument when the line is the flag's own description line
            if sep and sep.strip() == '' and not line.lstrip().startswith('-'):
                arg = None
            seen.add(flag)
            found.append((flag, sample_for(arg) if arg else None))

    # a short flag paired with a long one on the same line shares its argument
    by_line = {}
    for line in text.split('\n'):
        pair = re.match(r'\s*(-[a-zA-Z]), (--[a-z][a-z0-9-]*)(=|\[=)?([A-Z][A-Z_]*)?',
                        line)
        if pair and pair.group(4):
            by_line[pair.group(1)] = sample_for(pair.group(4))
    found = [(f, by_line.get(f, a)) for f, a in found]
    return found


def run(argv, feed, timeout=10, cwd=None):
    try:
        # Tool output is an arbitrary byte stream.  In particular, flags such
        # as grep's binary/text controls can make a perfectly valid probe emit
        # bytes which are not UTF-8.  Decode only --help above; compare command
        # output exactly as bytes here.
        environment = os.environ.copy()
        environment['LC_ALL'] = 'C'
        environment['LANG'] = 'C'
        environment['TZ'] = 'UTC0'
        r = subprocess.run(argv, input=feed.encode(), capture_output=True,
                           timeout=timeout, cwd=cwd, env=environment)
        return r.stdout, r.returncode, None
    except subprocess.TimeoutExpired:
        return b'', -1, 'did not stop'
    except OSError as e:
        return b'', -1, str(e)


def looks_unsupported(out, status, tool):
    return status != 0 and not out


def probe(tool, ours, flag, value):
    base = NEEDS.get(tool, [])
    scratch = None
    cwd = None
    if tool == 'grep' and flag in {
            '-r', '-R', '--recursive', '--dereference-recursive'}:
        # Without a path, recursive grep walks the auditor's source checkout.
        # Besides being huge and nondeterministic, that made system and local
        # results depend on unrelated build artifacts.  Give both commands the
        # same small tree and a relative path so their filenames also agree.
        scratch = tempfile.TemporaryDirectory(prefix='surface-grep-')
        cwd = scratch.name
        os.mkdir(os.path.join(cwd, 'sub'))
        with open(os.path.join(cwd, 'root'), 'wb') as f:
            f.write(b'banana\npear\n')
        with open(os.path.join(cwd, 'sub', 'leaf'), 'wb') as f:
            f.write(b'apple\nplum\n')
        os.symlink('sub', os.path.join(cwd, 'linked-sub'))
        base = ['a', '.']
    elif tool == 'diff':
        # diff without its pair of operands rejects every option for the same
        # later reason, which made unsupported formats look implemented. Use
        # a deterministic changed pair so output-style and ignore flags reach
        # the code they name.
        scratch = tempfile.TemporaryDirectory(prefix='surface-diff-')
        cwd = scratch.name
        with open(os.path.join(cwd, 'left'), 'wb') as f:
            f.write(b'alpha\nbeta  \ngamma\n')
        with open(os.path.join(cwd, 'right'), 'wb') as f:
            f.write(b'alpha\nBETA\ngamma\n')
        base = ['left', 'right']
    if value is None:
        with_flag = [flag]
    elif (tool, flag) in OPTIONAL_EQUALS:
        with_flag = [f'{flag}={value}']
    else:
        with_flag = [flag, value]
    argv_sys = [tool] + with_flag + base
    argv_our = [ours] + with_flag + base

    try:
        want, want_status, want_err = run(argv_sys, FEED, cwd=cwd)
        got, got_status, got_err = run(argv_our, FEED, cwd=cwd)
    finally:
        if scratch:
            scratch.cleanup()

    if got_err == 'did not stop':
        return 'crashed', 'did not stop'
    if got_err:
        return 'crashed', got_err
    if want_err:
        return 'agrees', ''            # the system tool refused it too

    if want == got and want_status == got_status:
        return 'agrees', ''

    if looks_unsupported(got, got_status, tool) and not looks_unsupported(want, want_status, tool):
        return 'absent', f'ours exits {got_status} with nothing to say'

    return 'differs', (f'status {want_status} vs {got_status}'
                       if want_status != got_status else 'output')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bin', required=True, help='directory holding our tools')
    p.add_argument('--tool', action='append')
    p.add_argument('--all', action='store_true', help='list agreements too')
    args = p.parse_args()

    try:
        ledger = json.load(open(LEDGER))
    except (OSError, ValueError):
        ledger = {}

    tools = args.tool or sorted(
        n for n in os.listdir(args.bin)
        if os.path.exists(f'/usr/bin/{n}') or os.path.exists(f'/bin/{n}'))

    totals = {'agrees': 0, 'differs': 0, 'absent': 0, 'crashed': 0}
    unjudged = []

    for tool in tools:
        ours = os.path.join(args.bin, tool)
        if not os.path.exists(ours):
            continue
        flags = flags_of(tool)
        if not flags:
            continue
        rows = []
        for flag, value in flags:
            verdict, why = probe(tool, ours, flag, value)
            totals[verdict] += 1
            if verdict == 'agrees':
                if args.all:
                    rows.append((verdict, flag, ''))
                continue
            key = f'{tool} {flag}'
            if key in ledger:
                rows.append((verdict, flag, 'known: ' + ledger[key]))
            else:
                rows.append((verdict, flag, why))
                unjudged.append((tool, flag, verdict, why))
        if rows:
            print(f'\n{tool}  ({len(flags)} flags the system tool takes)')
            for verdict, flag, why in rows:
                print(f'    {verdict:8} {flag:22} {why}')

    print(f"\n  agrees  {totals['agrees']}")
    print(f"  differs {totals['differs']}")
    print(f"  absent  {totals['absent']}")
    print(f"  crashed {totals['crashed']}")
    print(f"\n  {len(unjudged)} nobody has written down")
    return 1 if unjudged else 0


sys.exit(main())
