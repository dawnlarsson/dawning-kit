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

A routine that names a table gets the table too: byte_commonness is lifted
from its marker-delimited assembly object, converted from ELF to Mach-O, and
checked before it is emitted.  That keeps the native case on the exact bytes
the three production architectures index without putting a second C form of
the table here.
"""
import ast, hashlib, json, re, sys

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

OBJECTS = {
    # A checksum pins the ordering as well as the size.  The table is a
    # permutation, which is checked separately so a diagnostic says what was
    # structurally wrong instead of reporting only an opaque digest mismatch.
    'byte_commonness': {
        'size': 256,
        'alignment': 16,
        'sha256': '470d515b123842faff312a364f32931ba37079f317e184420eb2b6bc93c30efc',
        'permutation': True,
    },
}

def marked_object(name):
    """Read, verify, and Mach-O-convert one inline-assembly data object."""
    tag = name.upper()
    begin_tag = 'NATIVE_%s_BEGIN' % tag
    end_tag = 'NATIVE_%s_END' % tag
    begins = [i for i, line in enumerate(lines) if begin_tag in line]
    ends = [i for i, line in enumerate(lines) if end_tag in line]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        sys.exit('extract: need one ordered %s/%s marker pair in %s'
                 % (begin_tag, end_tag, lib))

    # The marker encloses a complete __asm__ object.  Decode its C string
    # tokens instead of interpreting formatting in library.c; adjacent string
    # literals and any number of .byte rows consequently have the same result.
    source = '\n'.join(lines[begins[0] + 1:ends[0]])
    tokens = re.findall(r'"(?:\\.|[^"\\])*"', source)
    try:
        assembly = ''.join(ast.literal_eval(token) for token in tokens)
    except (SyntaxError, ValueError) as error:
        sys.exit('extract: malformed C string in %s object: %s' % (name, error))
    if not assembly:
        sys.exit('extract: empty %s object between native markers' % name)

    # library.c keeps the payload literal only once and lets these two macros
    # give normal builds their target object spelling.  Expand that wrapper to
    # its canonical ELF form here, then pass it through the same explicit
    # ELF-to-Mach conversion below.  An older fully literal marked object is
    # accepted too, which makes malformed/missing macro wrappers diagnosable.
    if not re.search(r'(?m)^\s*' + re.escape(name) + r':\s*$', assembly):
        starts = re.findall(r'ASM_RODATA_OBJECT_BEGIN\(\s*' + re.escape(name)
                            + r'\s*,\s*([0-9]+)\s*\)', source)
        stops = re.findall(r'ASM_OBJECT_END\(\s*' + re.escape(name) + r'\s*\)',
                           source)
        if len(starts) != 1 or len(stops) != 1:
            sys.exit('extract: %s markers need one object begin/end macro pair'
                     % name)
        alignment = int(starts[0])
        if alignment != OBJECTS[name]['alignment']:
            sys.exit('extract: %s alignment %d, expected %d'
                     % (name, alignment, OBJECTS[name]['alignment']))
        assembly = ('.pushsection .rodata.%s,"a",%%progbits\n'
                    '.balign %d\n'
                    '.globl %s\n'
                    '.type %s, %%object\n'
                    '%s:\n%s'
                    '.size %s, .-%s\n'
                    '.popsection\n'
                    % (name, alignment, name, name, name, assembly, name, name))

    # Verify the bytes between the label and ELF size directive.  Only .byte
    # contributes data there: accepting a .word or .zero without accounting
    # for it would make the declared 256-byte table check meaningless.
    seen_label = False
    values = []
    for line in assembly.splitlines():
        stripped = line.strip()
        if stripped == name + ':':
            seen_label = True
            continue
        if not seen_label:
            continue
        if stripped.startswith('.size '):
            break
        byte = re.match(r'^\.byte\s+(.+)$', stripped)
        if byte:
            for value in byte.group(1).split(','):
                try:
                    number = int(value.strip(), 0)
                except ValueError:
                    sys.exit('extract: non-integer byte in %s: %s'
                             % (name, value.strip()))
                if number < 0 or number > 255:
                    sys.exit('extract: byte outside 0..255 in %s: %d'
                             % (name, number))
                values.append(number)
            continue
        if stripped and not stripped.startswith(('.p2align ', '.balign ',
                                                  '.popsection')):
            sys.exit('extract: unsupported data directive in %s: %s'
                     % (name, stripped))
    if not seen_label:
        sys.exit('extract: no %s label inside native markers' % name)

    spec = OBJECTS[name]
    if len(values) != spec['size']:
        sys.exit('extract: %s has %d bytes, expected %d'
                 % (name, len(values), spec['size']))
    if spec.get('permutation') and sorted(values) != list(range(spec['size'])):
        sys.exit('extract: %s is not a permutation of 0..%d'
                 % (name, spec['size'] - 1))
    digest = hashlib.sha256(bytes(values)).hexdigest()
    if digest != spec['sha256']:
        sys.exit('extract: %s checksum %s, expected %s'
                 % (name, digest, spec['sha256']))

    out = []
    for line in assembly.splitlines():
        stripped = line.strip()
        if stripped.startswith(('.type ', '.size ')):
            continue                    # ELF symbol metadata has no Mach-O form
        if re.match(r'^\.(?:push)?section\s+\.rodata(?:\.[^,\s]+)?(?:\s|,|$)',
                    stripped):
            line = re.sub(r'^\s*\.(?:push)?section.*$',
                          '.section __TEXT,__const', line)
        elif stripped == '.popsection':
            line = '.text'              # restore the section for following bodies
        line = re.sub(r'^(\s*\.(?:globl|global)\s+)' + re.escape(name) + r'\b',
                      r'\1_' + name, line)
        line = re.sub(r'^(\s*)' + re.escape(name) + r':',
                      r'\1_' + name + ':', line)
        out.append(line)

    converted = '\n'.join(out) + '\n'
    required = ('.section __TEXT,__const', '.globl _' + name, '_' + name + ':')
    for spelling in required:
        if spelling not in converted:
            sys.exit('extract: converted %s object lacks %s' % (name, spelling))
    if re.search(r'(?m)^\s*\.(?:type|size|pushsection|popsection)\b', converted):
        sys.exit('extract: ELF-only metadata remains in converted %s object' % name)
    if '%object' in converted or '%progbits' in converted:
        sys.exit('extract: ELF-only type spelling remains in converted %s object'
                 % name)
    return converted

def emit_asm(assembly):
    """Print decoded assembly as a C top-level __asm__ declaration."""
    print('__asm__(')
    for line in assembly.splitlines():
        print('    ' + json.dumps(line + '\n'))
    print(');')

def digit_pair_table():
    """The assembler digit table as a C symbol for lifted formatter leaves."""
    return ('const unsigned char digit_pairs[] = '
            '"00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";')

def body(name):
    for i,l in enumerate(lines):
        if l.strip() in (f'ASM_FUNC({name})', f'ASM_LOCAL_FUNC({name})') and arch[i]=='ARM64':
            j=next(k for k in range(i,len(lines))
                   if lines[k].strip().startswith((f'ASM_END({name})',
                                                   f'ASM_LOCAL_END({name})')))
            out=[]
            for x in lines[i+1:j]:
                if x.strip().startswith('//'): continue
                indirect = re.fullmatch(r'\s*ASM_CALL\("(x[0-9]+)"\)', x)
                if indirect:
                    out.append('    "blr %s\\n"' % indirect.group(1))
                    continue
                out.append(darwin(x.replace('ASM_RET', '"ret\\n"')))
            return out
    sys.exit(f"extract: no arm64 {name} in {lib}")

#
#       A routine whose arm64 form is ASM_ALIAS(name, target) has no body of
#       its own: .set makes it a second label on the target's address. That
#       is lifted the same way -- the target's body comes along, and the alias
#       is emitted as a .set after every body, which is what the library does
#       and what keeps the native case on the bytes the file actually holds.
#
def alias_target(name):
    for i, l in enumerate(lines):
        m = re.fullmatch(r'\s*ASM_ALIAS\((\w+),\s*(\w+)\)\s*', l)
        if m and m.group(1) == name and arch[i] == 'ARM64':
            return m.group(2)
    return None

aliases = {}
for n in list(names):
    target = alias_target(n)
    if not target:
        continue
    aliases[n] = target
    if target not in names:
        names.append(target)
for n, target in aliases.items():
    if target in aliases:
        sys.exit(f"extract: arm64 {n} aliases {target}, itself an alias")

bodies = {n: body(n) for n in names if n not in aliases}

print(f'// Lifted from {lib} by src/test/native/extract.py -- do not edit.')

if any('byte_commonness' in l for b in bodies.values() for l in b):
    emit_asm(marked_object('byte_commonness'))
    print('extern const unsigned char byte_commonness[256];')

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
    if n in aliases:
        continue
    print(f'__asm__(\n    ".globl _{n}\\n"\n    ".p2align 4\\n"\n    "_{n}:\\n"')
    for l in bodies[n]: print(l)
    print(');')

for n, target in aliases.items():
    print(f'__asm__(\n    ".globl _{n}\\n"\n    ".set _{n}, _{target}\\n"\n);')
