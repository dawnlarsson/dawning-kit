#!/usr/bin/env python3
"""
Flags together, and inputs that are not the easy one.

surface.py asks whether each flag on its own is understood. That is the first
question and not the whole one: -i and -v are each fine in grep and the pair
of them is a different code path, and a tool that is right on a tidy input can
still be wrong on an empty one or on a file whose last line has no newline.

So this runs combinations, and runs each over a set of inputs chosen to be
awkward rather than representative:

    empty          nothing at all, where a count is 0 and not blank
    no newline     a last line the file never terminated
    only newlines  where a line is empty rather than absent
    one long line  longer than any buffer a tool is likely to have
    tabs and space trailing blanks, which several tools treat specially
    high bytes     not text, which is what a tool does when handed a binary

    python3 kit/surface.d/pairs.py --bin <dir-of-ours>
    python3 kit/surface.d/pairs.py --bin <dir> --tool grep

Same four verdicts as surface.py, same meaning. Where a difference is only
reachable with two flags together, this is what says so.
"""
import argparse, itertools, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = os.path.join(HERE, 'pairs-ledger.json')

INPUTS = {
    'empty':         '',
    'no newline':    'alpha\nbeta',
    'only newlines': '\n\n\n',
    'one long line': 'x' * 5000 + '\n',
    'tabs and space': 'a\tb  \n  c\t\n\td  e\n',
    'high bytes':    'café\nÿþ\n',
    'repeats':       'a\na\nb\na\nb\nb\n',
    'numbers':       '10\n9\n100\n-3\n0\n',
}

#
#       Pairs worth trying, per tool. Not every combination -- that is a
#       product nobody can read -- but the ones where two flags meet in the
#       same piece of code and one can undo the other.
#
PAIRS = {
    'grep': [['-i', '-v'], ['-c', '-v'], ['-n', '-i'], ['-o', '-i'],
             ['-w', '-i'], ['-x', '-v'], ['-c', '-l'], ['-n', '-o'],
             ['-E', '-i'], ['-F', '-x'], ['-q', '-c'], ['-b', '-o']],
    'sort': [['-r', '-n'], ['-u', '-r'], ['-f', '-u'], ['-b', '-n'],
             ['-k', '-r'], ['-n', '-u'], ['-r', '-f']],
    'uniq': [['-c', '-d'], ['-c', '-u'], ['-d', '-i'], ['-u', '-i'],
             ['-c', '-i']],
    'cut':  [['-d', '-f'], ['-c', '-s'], ['-f', '-s']],
    'wc':   [['-l', '-w'], ['-c', '-l'], ['-w', '-L'], ['-l', '-c', '-w']],
    'head': [['-n', '-q'], ['-c', '-q'], ['-n', '-v']],
    'tail': [['-n', '-q'], ['-c', '-q'], ['-n', '-v']],
    'nl':   [['-b', '-n'], ['-w', '-s'], ['-b', '-w']],
    'cat':  [['-n', '-b'], ['-A', '-n'], ['-s', '-n'], ['-E', '-T']],
    'tr':   [['-d', '-c'], ['-s', '-c'], ['-d', '-s']],
    'sed':  [['-n', '-e'], ['-E', '-n']],
    'fold': [['-w', '-s'], ['-b', '-w']],
}

VALUES = {'-n': '2', '-c': '2', '-w': '4', '-k': '1', '-d': ':', '-s': ';',
          '-f': '1', '-b': 'a', '-e': 's/a/A/'}

NEEDS = {'grep': ['a'], 'sed': [], 'tr': ['a', 'A'], 'cut': []}


def expand(tool, combo):
    argv = []
    for flag in combo:
        argv.append(flag)
        # -c means a count in grep and a byte range in cut; only some take a value
        if flag in VALUES and tool in ('sort', 'cut', 'wc', 'head', 'tail',
                                       'nl', 'fold', 'sed', 'tr'):
            if not (tool == 'wc' and flag in ('-c', '-l', '-w')):
                argv.append(VALUES[flag])
        elif flag in ('-n', '-c') and tool in ('head', 'tail'):
            argv.append('2')
    return argv + NEEDS.get(tool, [])


def run(argv, feed):
    try:
        r = subprocess.run(argv, input=feed, capture_output=True, text=True,
                           timeout=10)
        return r.stdout, r.returncode, None
    except subprocess.TimeoutExpired:
        return '', -1, 'did not stop'
    except OSError as e:
        return '', -1, str(e)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--bin', required=True)
    p.add_argument('--tool', action='append')
    args = p.parse_args()

    try:
        ledger = json.load(open(LEDGER))
    except (OSError, ValueError):
        ledger = {}

    tools = args.tool or sorted(PAIRS)
    totals = {'agrees': 0, 'differs': 0, 'absent': 0, 'crashed': 0}
    unjudged = []

    for tool in tools:
        ours = os.path.join(args.bin, tool)
        if not os.path.exists(ours) or tool not in PAIRS:
            continue
        rows = []
        for combo in PAIRS[tool]:
            argv = expand(tool, combo)
            for label, feed in INPUTS.items():
                want, want_status, want_err = run([tool] + argv, feed)
                got, got_status, got_err = run([ours] + argv, feed)
                if want_err:
                    continue
                if got_err:
                    totals['crashed'] += 1
                    rows.append(('crashed', combo, label, got_err))
                    continue
                if want == got and want_status == got_status:
                    totals['agrees'] += 1
                    continue
                verdict = ('absent' if got_status != 0 and not got
                           else 'differs')
                totals[verdict] += 1
                key = f"{tool} {' '.join(combo)} on {label}"
                why = (f'status {want_status} vs {got_status}'
                       if want_status != got_status else 'output')
                if key in ledger:
                    rows.append((verdict, combo, label, 'known: ' + ledger[key]))
                else:
                    rows.append((verdict, combo, label, why))
                    unjudged.append(key)
        if rows:
            print(f'\n{tool}')
            for verdict, combo, label, why in rows:
                print(f"    {verdict:8} {' '.join(combo):14} on {label:15} {why}")

    for k, v in totals.items():
        print(f'  {k:8} {v}')
    print(f'\n  {len(unjudged)} nobody has written down')
    return 1 if unjudged else 0


sys.exit(main())
