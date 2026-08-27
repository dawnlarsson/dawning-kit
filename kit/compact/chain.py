#!/usr/bin/env python3
"""
Instructions read as instructions, not as a wall of quotes.

Each source line is one string literal holding several instructions, split by
the \\n that already ended each of them:

    "1:  mov %rdx, %rax\\n   sub %r10, %rax\\n   mov %rdx, %rsi\\n   not %rsi\\n"

rather than one literal per instruction with column padding inside each. The
padding was there to line up mnemonics when every instruction had its own
line, and once they share one it only pushes the operands apart.

Nothing here reaches the compiler: the \\n that ended each instruction is still
there, so the assembler reads exactly the same text, and whitespace between a
mnemonic and its operands was never significant anyway. That also keeps a
trailing # comment from swallowing the instruction after it -- the newline
inside the literal ends the comment, as it always did.

A label starts a fresh line and a line carrying a comment ends the run it is
in, so the two things a reader scans for stay at the left margin. A .ascii
keeps its own line and its own spacing: its operand is data.

    python3 kit/compact/chain.py <file.c>

Run kit/compact/prove afterwards. It is the only thing that says this was a
reformat and not an edit.
"""
import re, sys

PER_LINE = 4
COLUMNS  = 96
JOIN     = '\\n   '

def is_data(t):
    return '.ascii' in t or '.asciz' in t or '.string' in t

def tighten(insn):
    """One space between the parts, two after a label. Data is left alone."""
    if is_data(insn):
        return insn
    body, note = insn, ''
    m = re.search(r'\s+(#(?!\d)|//)\s?', insn)
    if m:
        body, note = insn[:m.start()], '  ' + insn[m.start():].strip()
    m = re.match(r'^(\S+:)\s*(.*)$', body)
    if m:
        label, rest = m.group(1), re.sub(r'\s+', ' ', m.group(2)).strip()
        return (f'{label}  {rest}' if rest else label) + note
    return re.sub(r'\s+', ' ', body).strip() + note

def explode(line):
    """Every instruction on this source line, whatever shape it is in now."""
    out = []
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
        for insn in lit.split('\\n'):
            if insn.strip() or insn == '':
                if insn.strip():
                    out.append(insn)
    return out

def main(path):
    lines = open(path).read().split('\n')
    out, run, inasm = [], [], False

    def flush():
        if run:
            out.append('    "' + JOIN.join(run) + '\\n"')
            run.clear()

    for raw in lines:
        s = raw.strip()

        if s.startswith('__asm__('):
            flush(); inasm = True; out.append(raw); continue
        if inasm and s == ');':
            flush(); inasm = False; out.append(raw); continue
        if not inasm:
            out.append(raw); continue

        # Only a line that is nothing but string literals is ours to reshape.
        # Some carry a macro between them -- ".type " #name ", " ASM_TYPE --
        # and taking the literals out of those would drop the macro on the
        # floor. Checked by removing every literal and seeing what is left.
        if not (s.startswith('"') and s.endswith('"')) or \
           re.sub(r'"(?:[^"\\]|\\.)*"', '', s).strip():
            flush(); out.append(raw); continue

        for insn in explode(s):
            t = tighten(insn)
            if is_data(t):
                flush(); out.append('    "' + t + '\\n"'); continue
            if re.match(r'^\S+:', t) and run:
                flush()
            run.append(t)
            if '#' in t or '//' in t or len(run) >= PER_LINE or \
               len(JOIN.join(run)) >= COLUMNS:
                flush()

    flush()
    open(path, 'w').write('\n'.join(out))

main(sys.argv[1])
