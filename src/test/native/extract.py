#!/usr/bin/env python3
"""
Lift an arm64 routine out of library.c so it can be run here.

    python3 src/test/native/extract.py <lib.c> <routine> [more...]

Writes a C file to standard output holding each routine's body under a
_<name> symbol, with the three prologue lines Darwin needs instead of
ASM_FUNC. The body between the label and the ret is copied unchanged, so
what runs here is what is in the file and not a transcription of it.
"""
import re, sys

lib, names = sys.argv[1], sys.argv[2:]
lines = open(lib).read().split('\n')

arch=[None]*len(lines); cur=None
for i,l in enumerate(lines):
    m=re.match(r'^#(el)?if (X64|ARM64|RISCV64)\b', l.strip())
    if m: cur=m.group(2)
    arch[i]=cur

def body(name):
    for i,l in enumerate(lines):
        if l.strip()==f'ASM_FUNC({name})' and arch[i]=='ARM64':
            j=next(k for k in range(i,len(lines))
                   if lines[k].strip().startswith(f'ASM_END({name})'))
            out=[]
            for x in lines[i+1:j]:
                if x.strip().startswith('//'): continue
                out.append(x.replace('    ASM_RET', '    "ret\\n"'))
            return out
    sys.exit(f"extract: no arm64 {name} in {lib}")

print(f'// Lifted from {lib} by src/test/native/extract.py -- do not edit.')
for n in names:
    print(f'__asm__(\n    ".globl _{n}\\n"\n    ".p2align 4\\n"\n    "_{n}:\\n"')
    for l in body(n): print(l)
    print(');')
