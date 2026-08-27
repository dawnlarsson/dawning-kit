#!/usr/bin/env python3
"""
Does anything outside library.c still call a name that moved?

The rename is invisible to the compiler in every other file: a caller that
says moonwater_ticks() still declares it, still compiles, and fails at the
link, which is the last place anybody looks. So the tree is read for names
the restructure pass takes away, and the answer is a list rather than a
surprise later.

    python3 kit/compact/callers.py <root>

Exit is non-zero when something is left behind.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from restructure import FROM_MOONWATER, FROM_LIBC

def main(root):
    gone = dict(FROM_MOONWATER)
    # the libc names survive as aliases, so calling them is still fine
    stale = []

    for base in ('src', 'programs', 'kit'):
        top = os.path.join(root, base)
        for dirpath, _, names in os.walk(top):
            for n in names:
                if not n.endswith(('.c', '.h')):
                    continue
                path = os.path.join(dirpath, n)
                if os.path.basename(path).startswith('library'):
                    continue
                try:
                    text = open(path, errors='replace').read()
                except OSError:
                    continue
                for line_no, line in enumerate(text.split('\n'), 1):
                    for old, new in gone.items():
                        # a name in a string is a filename, not a call
                        if re.search(r'\b' + old + r'\b\s*[(;]', line):
                            rel = os.path.relpath(path, root)
                            stale.append((rel, line_no, old, new))

    for rel, line_no, old, new in stale:
        sys.stderr.write(f'callers: {rel}:{line_no} still names {old} '
                         f'-- it is {new} now\n')
    if not stale:
        sys.stderr.write('callers: nothing outside library.c names a moved symbol\n')
    return 1 if stale else 0

sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else '.'))
