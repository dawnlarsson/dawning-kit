#!/usr/bin/env python3
"""
The same contract, written once -- and nothing describing code that is not here.

What is left is three quarters architecture independent -- what a
routine computes, why the SWAR identity holds, which overread is safe -- and
was written out once per block. The first copy stays and the later ones keep
a line saying where it went, so a wrong explanation is one edit and not three.

A block sitting between instructions rather than above a routine is narration
of the step below it; where that is word for word what another architecture
already says, it goes the same way. The ones that carry a constraint rather
than a description differ between architectures, so they are never duplicates
and are never touched here.

Comments do not reach the assembler. check.sh proves the disassembly is
unchanged on all three rather than asking anyone to trust that.
"""
import sys, collections, re

path = sys.argv[1]
lines = open(path).read().split('\n')

arch=[None]*len(lines); cur=None
for i,l in enumerate(lines):
    m=re.match(r'^#(el)?if (X64|ARM64|RISCV64)\b', l.strip())
    if m: cur=m.group(2)
    arch[i]=cur
NAME={'X64':'x86_64','ARM64':'arm64','RISCV64':'riscv64'}

# what each architecture block actually defines
defined=collections.defaultdict(set)
for i,l in enumerate(lines):
    m=re.match(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', l.strip())
    if m and arch[i]: defined[arch[i]].add(m.group(1))

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

def subject(run):
    """The routine the prose is about, taken from a prototype it quotes."""
    for l in run:
        t=l.lstrip('/').strip()
        m=re.match(r'^(?:[A-Za-z_][A-Za-z0-9_ ]*[ *]+)?([a-z_][a-z0-9_]*)\s*\(', t)
        if m and ('*' in t or 'const' in t or t.endswith(')')):
            return m.group(1)
    return None

def opens(st, n):
    """The routine defined immediately below this block, if any."""
    for k in range(st+n, min(st+n+6, len(lines))):
        s=lines[k].strip()
        if not s or s.startswith('"        .text'): continue
        m=re.match(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', s)
        return m.group(1) if m else None
    return None

drop=set(); replace={}

# Dedup only. A rule that dropped prose naming a routine the block does not
# define was tried and taken out again: it cannot tell strrchr copied into
# arm64 by accident from memchr explained as deliberately absent in riscv,
# and the second is the more valuable comment in the file. Those few stale
# blocks are a hand edit, and the invariant below is what makes this pass safe
# to run without reading all six thousand lines again.
alive=list(runs)

seen=collections.defaultdict(list)
for st,c in alive: seen[tuple(c)].append(st)

for c,locs in seen.items():
    if len(locs)<2 or len(c)<3: continue
    first=locs[0]
    name=opens(first, len(c))
    for st in locs[1:]:
        for k in range(st, st+len(c)): drop.add(k)
        if name and opens(st, len(c))==name:
            replace[st]=f"    //       {name}: the {NAME[arch[first]]} block carries the reasoning."

# Nothing may vanish from the file that is not a repeat of something still in
# it. A dropped explanation that was the only copy is the failure mode this
# whole pass has, so it is checked rather than hoped for.
kept=set()
for i,l in enumerate(lines):
    if i in drop: continue
    t=l.strip()
    if t.startswith('//') and len(t)>6: kept.add(t)
lost=[]
for i,l in enumerate(lines):
    if i not in drop: continue
    t=l.strip()
    if t.startswith('//') and len(t)>6 and t not in kept: lost.append((i+1,t))
if lost:
    sys.stderr.write(f"dedup: {len(lost)} comment lines would vanish entirely:\n")
    for n,t in lost: sys.stderr.write(f"    {n}: {t[:100]}\n")
    sys.exit(1)

out=[]
for i,l in enumerate(lines):
    if i in replace: out.append(replace[i])
    if i in drop: continue
    out.append(l)
open(path,'w').write('\n'.join(out))
