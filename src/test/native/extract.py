#!/usr/bin/env python3
"""
Lift an arm64 routine out of library.c so it can be run here.

    python3 src/test/native/extract.py <lib.c> <routine> [more...]

Writes a C file to standard output holding each routine's body under a
_<name> symbol, with the three prologue lines Darwin needs instead of
ASM_FUNC. The body between the label and the ret is copied unchanged, so
what runs here is what is in the file and not a transcription of it.

Three things are spelled differently on Darwin and are rewritten rather
than left to fail at assembly time. A call to another routine in the
library names it without the leading underscore Mach-O gives every C
symbol; the address of a global is built with @PAGE and @PAGEOFF rather
than with :lo12:. Both appeared the first time a routine here called
another one -- memory_search calls memory_compare, string_find calls
three of them -- and without the rewrite the case does not link rather
than failing a check, which is a confusing way to find out.

A routine that names a table gets the table too: byte_commonness is
copied out of the library as C, so what the lifted code indexes here is
the same two hundred and fifty six numbers it indexes there.
"""
import re, sys

lib, names = sys.argv[1], sys.argv[2:]
lines = open(lib).read().split('\n')

arch=[None]*len(lines); cur=None
for i,l in enumerate(lines):
    m=re.match(r'^#(el)?if (X64|ARM64|RISCV64)\b', l.strip())
    if m: cur=m.group(2)
    arch[i]=cur

#
#       Mach-O spells a C symbol with a leading underscore and builds the
#       address of a global out of @PAGE and @PAGEOFF. Branch targets that are
#       numbers -- 1b, 4f -- are local labels and must be left alone, which is
#       why the name has to start with a letter to be rewritten.
#
def darwin(line):
    # A table embedded inside a routine stays inside the extracted body. ELF
    # and Mach-O name the read-only section differently, and ASM_SECTION is a
    # C macro that does not come along with the literal lines being lifted.
    line = line.replace('.section .rodata', '.section __TEXT,__const')
    if line.strip() == 'ASM_SECTION':
        return '    ".text\\n"'

    # ELF temporary symbols begin .L; Mach-O's assembler-local spelling is L.
    # AArch64 conditional branches cannot carry an external relocation, so a
    # forward .L name left unchanged is rejected even though its definition is
    # present later in the same inline assembly block.
    line = line.replace('.L', 'L')

    line = re.sub(r'\b(bl|b) ([a-z_][a-z0-9_]*)\b', r'\1 _\2', line)
    line = re.sub(r'\badrp (x[0-9]+), ([a-z_][a-z0-9_]*)\b', r'adrp \1, _\2@PAGE', line)
    line = re.sub(r':lo12:([a-z_][a-z0-9_]*)\b', r'_\1@PAGEOFF', line)

    # Labels defined by the extracted inline assembly need the same Mach-O
    # spelling as the references above. Numeric and .L labels are local and
    # deliberately do not match this form.
    line = re.sub(r'^(\s*")([a-z_][a-z0-9_]*):', r'\1_\2:', line)
    return line

def table(name):
    """A global the lifted code indexes, copied out of the library as C."""
    text = '\n'.join(lines)
    at = text.find('const p8 %s[256] = {' % name)
    if at < 0:
        sys.exit('extract: no %s in %s' % (name, lib))
    stop = text.index('};', at)
    return ('const unsigned char %s[256] = {%s};'
            % (name, text[text.index('{', at) + 1:stop]))

def digit_pair_table():
    """The assembler digit table as a C symbol for lifted formatter leaves."""
    return ('const unsigned char digit_pairs[] = '
            '"00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";')

def body(name):
    for i,l in enumerate(lines):
        if l.strip()==f'ASM_FUNC({name})' and arch[i]=='ARM64':
            j=next(k for k in range(i,len(lines))
                   if lines[k].strip().startswith(f'ASM_END({name})'))
            out=[]
            for x in lines[i+1:j]:
                if x.strip().startswith('//'): continue
                indirect = re.fullmatch(r'\s*ASM_CALL\("(x[0-9]+)"\)', x)
                if indirect:
                    out.append('    "blr %s\\n"' % indirect.group(1))
                    continue
                out.append(darwin(x.replace('    ASM_RET', '    "ret\\n"')))
            return out
    sys.exit(f"extract: no arm64 {name} in {lib}")

bodies = {n: body(n) for n in names}

print(f'// Lifted from {lib} by src/test/native/extract.py -- do not edit.')

if any('byte_commonness' in l for b in bodies.values() for l in b):
    print(table('byte_commonness'))

if any('digit_pairs' in l for b in bodies.values() for l in b):
    print(digit_pair_table())


# The arm64 bodies are written over macros -- one wide loop shared by every
# hunt that has one -- so the macros come across too, or what is lifted does
# not compile. A definition runs until a line that does not end in a backslash.
i = 0
while i < len(lines):
    if lines[i].startswith('#define NEON_') or lines[i].startswith('#define WIDE_'):
        while True:
            print(lines[i])
            if not lines[i].rstrip().endswith('\\'):
                break
            i += 1
    i += 1

for n in names:
    print(f'__asm__(\n    ".globl _{n}\\n"\n    ".p2align 4\\n"\n    "_{n}:\\n"')
    for l in bodies[n]: print(l)
    print(');')
