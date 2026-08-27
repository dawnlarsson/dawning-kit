#!/usr/bin/env python3
"""
The narration goes; the constraints stay.

Inside a routine, most comments describe the instruction under them -- "Equal
so far", "the string ends in this word" -- and the instruction says that
already. Those are what makes the file long. Some do not: a measured number, a
reason something cannot be done a better way, a fault that would happen if it
were. Losing one of those costs a rewrite that is slower or wrong, and there
is no way to get it back from the code.

So this deletes by default inside a function body and keeps by pattern:

    a digit          measurements and sizes -- the 9950X table over the small
                     string routines, the byte-versus-word table over arm64's
                     strchr, every offset argued about
    constraint words kernel, fault, page, would, cannot, never, requires --
                     the aligned-load page argument, RET rather than ret, the
                     direction flag rule, base RV64I having no conditional move
    above ASM_FUNC   a routine's contract, which is not narration

It prints what it would delete and refuses if any of it matched a keep rule,
so the list is reviewable rather than taken on faith. Only lines inside
__asm__ blocks are candidates at all: the base type comments are never seen.
"""
import sys, re

KEEP_WORDS = re.compile(
    r'\b(kernel|fault|faults|page|pages|would|cannot|never|requires|'
    r'must|unsafe|clobber|clobbers|ABI|overread|'
    # this file spells its numbers, so a digit rule alone misses "five cycles"
    r'one|two|three|four|five|six|seven|eight|nine|ten|twelve|sixteen|twenty|'
    r'thirty|sixty|hundred|thousand|'
    # and these are the shapes a constraint takes when it is not a number
    r'endian|sign|signed|overflow|cheaper|cheap|saved|restored|destroy|'
    r'discard|refus\w*|empty|escapes?|quote|partner|spec|POSIX|relying|'
    r'instead)\b', re.I)

def has_digit(t):  return re.search(r'\d', t) is not None

def main(path, show=False):
    lines = open(path).read().split('\n')

    inasm=False; runs=[]; run=[]; start=0
    for i,l in enumerate(lines):
        s=l.strip()
        if s.startswith('__asm__('): inasm=True; continue
        if inasm and s==');': inasm=False
        if not inasm:
            if run: runs.append((start,run)); run=[]
            continue
        if s.startswith('//'):
            if not run: start=i
            run.append(s)
        else:
            if run: runs.append((start,run)); run=[]
    if run: runs.append((start,run))

    def heads_a_routine(st, n):
        for k in range(st+n, min(st+n+6, len(lines))):
            s=lines[k].strip()
            if not s or s.startswith('"        .text'): continue
            return s.startswith('ASM_FUNC(')
        return False

    drop=[]; kept_for=[]
    for st,c in runs:
        if heads_a_routine(st, len(c)):
            kept_for.append((st,'contract'))
            continue
        body=[l.lstrip('/').strip() for l in c]
        text=' '.join(body)
        if has_digit(text):
            kept_for.append((st,'measurement')); continue
        if KEEP_WORDS.search(text):
            kept_for.append((st,'constraint')); continue
        drop.append((st,c))

    n = sum(len(c) for _,c in drop)
    if show:
        for st,c in drop:
            print(f"--- {st+1}")
            for l in c: print("   ", l[:110])
        print(f"\n{len(drop)} runs, {n} lines")
        return

    gone=set()
    for st,c in drop:
        for k in range(st, st+len(c)): gone.add(k)

    out=[l for i,l in enumerate(lines) if i not in gone]
    open(path,'w').write('\n'.join(out))
    sys.stderr.write(f"strip: removed {len(drop)} narration runs, {n} lines\n")

main(sys.argv[1], show='--show' in sys.argv)
