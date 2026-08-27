#!/usr/bin/env python3
"""
The corrections, applied to library.c as it stands.

Kept apart from the reformatting passes and anchored on the file's own
wording, so re-running the pipeline over a library.c somebody else has moved
on either applies cleanly or names the anchor it could not find. A missing
anchor usually means the bug was fixed upstream; look before deleting the
entry.

    python3 kit/compact/fixes.py <file.c>

Both were measured on arm64 by src/test/native/run. See src/test/native/FINDING.
"""
import sys

RESTART_NOTE = '''    //
    //       A candidate that fails restarts one byte past where it began, not
    //       where it stopped. Carrying on from the mismatch -- which is what
    //       the C this replaces did, and what the comment here used to excuse
    //       -- steps over every start inside the part that matched, so "aab"
    //       is not found in "aaab": the candidate at zero eats two bytes
    //       before failing and the scan resumes past the one at one.
    //
    //       Nothing unusual is needed to reach it. Over strings drawn from a
    //       four letter alphabet it missed 1000 of 200000 searches, and
    //       string_find is what grep and sed use for fixed string search.
    //
'''

EXACT_NOTE = '''    //
    //       The zero test here is the five instruction one, not the three
    //       instruction one the rest of this file uses, and the difference
    //       only shows where the highest flag in a word is wanted.
    //
    //       (v - 0x0101..) & ~v & 0x8080.. lets a borrow out of a zero byte
    //       set the flag on the byte above it. The flags are exact at the
    //       lowest and can lie above it, which no other hunt here notices
    //       because they all take the lowest. bsr takes the highest, and so
    //       took the lie: a byte equal to the one hunted followed by the next
    //       value up answered one byte late.
    //
    //       (v & 0x7f7f..) + 0x7f7f.. cannot carry between bytes -- neither
    //       half reaches past 0xfe -- so or v, or 0x7f7f.. and invert leaves
    //       bit seven of a byte set exactly when that byte was zero.
    //
    //       Measured on arm64, where the same mistake could be run: 774 wrong
    //       in 300000 over a five letter alphabet, and none after.
    //
'''

EDITS = [('x86_64 string_find restart', '    "        cmp     (%rdi), %al\\n"\n    "        jne     1b                      # carry on from here, as the C did\\n"\n    "        inc     %rdi\\n"\n', '    "        cmp     (%rdi), %al\\n"\n    "        jne     9f\\n"\n    "        inc     %rdi\\n"\n'),
         ('x86_64 string_find restart target', '    "7:      mov     %rdx, %rax\\n"\n', '    "9:      lea     1(%rdx), %rdi           # one past where this candidate began\\n"\n    "        jmp     1b\\n"\n    "7:      mov     %rdx, %rax\\n"\n'),
         ('arm64 string_find restart', '    "        cmp     w9, w8\\n"\n    "        b.ne    1b                      // carry on from here, as the C did\\n"\n    "        add     x0, x0, #1\\n"\n    "        add     x15, x15, #1\\n"\n    "        b       4b\\n"\n', '    "        cmp     w9, w8\\n"\n    "        b.ne    8f\\n"\n    "        add     x0, x0, #1\\n"\n    "        add     x15, x15, #1\\n"\n    "        b       4b\\n"\n    "8:      add     x0, x14, #1             // one past where this candidate began\\n"\n    "        b       1b\\n"\n'),
         ('riscv64 string_find restart', '    "        bne     t3, t2, 1b              # carry on from here, as the C did\\n"\n    "        addi    a0, a0, 1\\n"\n    "        addi    t6, t6, 1\\n"\n    "        j       4b\\n"\n', '    "        bne     t3, t2, 8f\\n"\n    "        addi    a0, a0, 1\\n"\n    "        addi    t6, t6, 1\\n"\n    "        j       4b\\n"\n    "8:      addi    a0, a4, 1               # one past where this candidate began\\n"\n    "        j       1b\\n"\n'),
         ('x86_64 strrchr constant', '    "        xor     %ebx, %ebx              # best so far: none\\n"\n    "        movabs  $0x0101010101010101, %r10\\n"\n    "        mov     %rcx, %rsi\\n"\n    "        imul    %r10, %rsi\\n"\n    "        movabs  $0x8080808080808080, %r11\\n"\n', '    "        xor     %ebx, %ebx              # best so far: none\\n"\n    "        movabs  $0x0101010101010101, %r10\\n"\n    "        mov     %rcx, %rsi\\n"\n    "        imul    %r10, %rsi\\n"\n    "        movabs  $0x7f7f7f7f7f7f7f7f, %r11\\n"\n'),
         ('x86_64 strrchr hunts', '    "5:      mov     %rdx, %rax\\n"\n    "        xor     %rsi, %rax\\n"\n    "        mov     %rax, %rcx\\n"\n    "        not     %rcx\\n"\n    "        sub     %r10, %rax\\n"\n    "        and     %rcx, %rax\\n"\n    "        and     %r11, %rax\\n"\n    "        mov     %rdx, %r8\\n"\n    "        sub     %r10, %r8\\n"\n    "        mov     %rdx, %rcx\\n"\n    "        not     %rcx\\n"\n    "        and     %rcx, %r8\\n"\n    "        and     %r11, %r8\\n"\n', '    "5:      mov     %rdx, %rax\\n"\n    "        xor     %rsi, %rax\\n"\n    "        mov     %rax, %rcx\\n"\n    "        and     %r11, %rcx\\n"\n    "        add     %r11, %rcx\\n"\n    "        or      %rax, %rcx\\n"\n    "        or      %r11, %rcx\\n"\n    "        not     %rcx\\n"\n    "        mov     %rcx, %rax              # matches, and none a borrow invented\\n"\n    "        mov     %rdx, %r8\\n"\n    "        and     %r11, %r8\\n"\n    "        add     %r11, %r8\\n"\n    "        or      %rdx, %r8\\n"\n    "        or      %r11, %r8\\n"\n    "        not     %r8\\n"\n')]

def main(path):
    s = open(path).read()
    done, missing = [], []
    for name, old, new in EDITS:
        if s.count(old) == 1:
            s = s.replace(old, new, 1); done.append(name)
        else:
            missing.append(f'{name} (anchor matched {s.count(old)} times)')

    if any('string_find' in d for d in done):
        s = s.replace('    ASM_FUNC(moonwater_find)\n',
                      '    ASM_FUNC(moonwater_find)\n' + RESTART_NOTE)
    if any('strrchr' in d for d in done):
        s = s.replace('    ASM_FUNC(strrchr)\n',
                      '    ASM_FUNC(strrchr)\n' + EXACT_NOTE, 1)

    open(path, 'w').write(s)

    # Every anchor gone means the file already has these -- landed, or fixed
    # upstream -- and there is nothing to do. Some gone and some not is the
    # state worth stopping on: the file has moved under one edit and not the
    # others, and applying half of a correction is worse than none of it.
    if not done:
        sys.stderr.write('fixes: already applied, nothing to do\n')
        return 0
    for d in done:    sys.stderr.write(f'fixes: applied {d}\n')
    for m in missing: sys.stderr.write(f'fixes: SKIPPED {m}\n')
    if missing:
        sys.stderr.write('fixes: applied some and not others -- look before '
                         'trusting the result\n')
    return 1 if missing else 0

sys.exit(main(sys.argv[1]))
