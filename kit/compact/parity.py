#!/usr/bin/env python3
"""
The routines an architecture was missing.

string_last_of_or_end, string_compare_max, string_length_max and
memory_first_of existed only on x86_64. arm64 and riscv64 carried a comment
where they should have been, saying the kernel ships its own and freestanding
code can fall back to the generic C. True, and it left the library uneven: a
program calling string_length_max linked on one machine and not the others.

This adds them where they are absent. The arm64 four are written out here and
have been run -- src/test/native/run puts 1.2 million calls through them
against a C reference. riscv64 is still outstanding and the inventory says so.

    python3 kit/compact/parity.py <file.c>
"""
import re, sys

ARM64 = open(__file__.replace('parity.py', 'arm64_parity.asm')).read()

def block_bounds(lines, which):
    """The __asm__ block belonging to one architecture."""
    arch, cur = [None]*len(lines), None
    for i, l in enumerate(lines):
        m = re.match(r'^#(el)?if (X64|ARM64|RISCV64)$', l.strip())
        if m: cur = m.group(2)
        arch[i] = cur
    return arch

def main(path):
    lines = open(path).read().split('\n')
    arch = block_bounds(lines, 'ARM64')

    have = set()
    for i, l in enumerate(lines):
        m = re.match(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', l.strip())
        if m and arch[i] == 'ARM64': have.add(m.group(1))

    wanted = {'string_last_of_or_end', 'string_compare_max',
              'string_length_max', 'memory_first_of'}
    if wanted & have:
        sys.stderr.write('parity: arm64 already has them, nothing to do\n')
        return

    # after string_first_of_max, replacing the prose that stood in their place
    # This runs before the rename, so the routine it follows may still be
    # called strnchr; it runs after it too, when the result is fed back in.
    anchor = None
    for i, l in enumerate(lines):
        if arch[i] == 'ARM64' and l.strip() in ('ASM_END(strnchr)',
                                                'ASM_END(string_first_of_max)'):
            anchor = i+1; break
    if anchor is None:
        sys.stderr.write('parity: SKIPPED, no arm64 bounded byte hunt to follow\n')
        return

    stop = anchor
    while stop < len(lines) and not lines[stop].lstrip().startswith('ASM_FUNC('):
        stop += 1
    # back up over the comment block that heads whatever comes next
    while stop > anchor and (lines[stop-1].strip().startswith('//')
                             or lines[stop-1].strip() == ''):
        stop -= 1

    out = lines[:anchor] + ARM64.rstrip('\n').split('\n') + lines[stop:]
    open(path, 'w').write('\n'.join(out))
    sys.stderr.write(f'parity: arm64 gained {len(wanted)} routines\n')

main(sys.argv[1])
