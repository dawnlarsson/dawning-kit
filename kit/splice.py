#!/usr/bin/env python3
"""
Take one routine's assembly out of somebody's copy and put it in this one.

Four people cannot edit one file at once, and library.c is one file. So each
works in a whole copy of the tree and their routines come back one at a time,
found by name rather than by a text merge: ASM_FUNC(name) opens a routine and
ASM_END(name) closes it, and everything between belongs to whoever owns that
name. Nothing outside those markers is taken, so two agents cannot disagree
about a line neither of them owns.

    python3 kit/splice.py <their-copy> <routine> [routine ...]
    python3 kit/splice.py <their-copy> --list        what differs
    python3 kit/splice.py <their-copy> --outside     what they changed that
                                                     is not inside a routine

--outside matters because a new routine needs more than its own body: a
prototype, sometimes an entry in the libc alias block, and always cases in
verify.c. None of that is between ASM_FUNC and ASM_END, so none of it splices,
and it has to be read and merged by hand. Anything this prints is work still
to do after the routines are in.

A routine that appears once per architecture is spliced once per
architecture, in order, and the count has to match on both sides -- if theirs
has three and ours has two, something is wrong with the assumption and it
stops rather than guessing which two.
"""
import re, shutil, sys, os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OURS = os.path.join(HERE, 'src', 'library.c')


def blocks(text, name):
    """Every ASM_FUNC(name) .. ASM_END(name) span, in file order."""
    out = []
    start = None
    for m in re.finditer(r'^[ \t]*ASM_(FUNC|END)\((%s)\)[ \t]*$' % re.escape(name),
                         text, re.M):
        if m.group(1) == 'FUNC':
            start = m.start()
        elif start is not None:
            out.append((start, m.end()))
            start = None
    return out


def preceding(text, spans):
    """The routine that closes last before each of these begins."""
    names = set()
    for start, _ in spans:
        best, who = -1, None
        for m in re.finditer(r'^[ \t]*ASM_END\(([A-Za-z0-9_]+)\)[ \t]*$',
                             text[:start], re.M):
            if m.start() > best:
                best, who = m.start(), m.group(1)
        names.add(who)
    return names.pop() if len(names) == 1 else None


def routines(text):
    return sorted(set(re.findall(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', text)))


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2

    theirs_root = argv[0]
    theirs_path = os.path.join(theirs_root, 'src', 'library.c')
    if not os.path.exists(theirs_path):
        sys.stderr.write(f'splice: no library.c under {theirs_root}\n')
        return 1

    theirs = open(theirs_path).read()
    ours = open(OURS).read()

    if argv[1] == '--outside':
        # blank out every routine body on both sides, then diff what is left
        import difflib
        def hollow(text):
            spans = []
            for n in routines(text):
                spans += blocks(text, n)
            out, at = [], 0
            for s, e in sorted(spans):
                out.append(text[at:s]); at = e
            out.append(text[at:])
            return ''.join(out).split('\n')
        a, b = hollow(ours), hollow(theirs)
        shown = 0
        for line in difflib.unified_diff(a, b, 'ours', 'theirs', lineterm='', n=1):
            if line.startswith(('+++', '---', '@@')) or line[:1] in '+-':
                print(line[:150]); shown += 1
            if shown > 200:
                print('  ... more, look at the file directly'); break
        if not shown:
            print('  nothing outside the routine bodies differs')
        return 0

    if argv[1] == '--list':
        mine, yours = set(routines(ours)), set(routines(theirs))
        for n in sorted(yours - mine):
            print(f'  new       {n}')
        for n in sorted(mine - yours):
            print(f'  gone      {n}')
        for n in sorted(mine & yours):
            a = [ours[s:e] for s, e in blocks(ours, n)]
            b = [theirs[s:e] for s, e in blocks(theirs, n)]
            if a != b:
                print(f'  changed   {n}  ({len(a)} here, {len(b)} there)')
        return 0

    shutil.copyfile(OURS, OURS + '.before-splice')
    taken = []

    for name in argv[1:]:
        mine = blocks(ours, name)
        yours = blocks(theirs, name)

        if not yours:
            sys.stderr.write(f'splice: {name} is not in {theirs_root}\n')
            return 1

        # A routine that is new here has nowhere to be put back into, so it is
        # placed after whatever routine precedes it over there. That keeps it
        # in the same architecture block and beside the same neighbours,
        # which is where its author decided it belonged.
        if not mine:
            after = preceding(theirs, yours)
            if after is None:
                sys.stderr.write(f'splice: {name} is new and nothing precedes '
                                 f'it, so there is nowhere obvious to put it\n')
                return 1
            here = blocks(ours, after)
            if len(here) != len(yours):
                sys.stderr.write(f'splice: {name} is new and follows {after}, '
                                 f'which is here {len(here)} times and there '
                                 f'{len(yours)} -- refusing to guess\n')
                return 1
            for (hs, he), (ys, ye) in zip(reversed(here), reversed(yours)):
                ours = ours[:he] + '\n' + theirs[ys:ye] + ours[he:]
            taken.append(f'{name} x{len(yours)} (new, after {after})')
            continue

        if len(mine) != len(yours):
            sys.stderr.write(
                f'splice: {name} appears {len(mine)} times here and '
                f'{len(yours)} times there -- refusing to guess which\n')
            return 1

        # back to front, so the earlier offsets stay valid
        for (ms, me), (ys, ye) in zip(reversed(mine), reversed(yours)):
            ours = ours[:ms] + theirs[ys:ye] + ours[me:]
        taken.append(f'{name} x{len(mine)}')

    open(OURS, 'w').write(ours)
    sys.stderr.write('splice: took ' + ', '.join(taken) + '\n')
    sys.stderr.write(f'splice: the file before this is at {OURS}.before-splice\n')
    return 0


sys.exit(main(sys.argv[1:]))
