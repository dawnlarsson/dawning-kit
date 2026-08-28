#!/usr/bin/env python3
"""
One name per routine, and no C where assembly already does the work.

Four things, in this order, because each depends on the one before:

    forwarders      a C function whose whole body calls a moonwater_ one is
                    the assembly under another name. The assembly takes the
                    name and the function goes.

    dispatchers     the file_ and library_ and memory_ routines choose
                    between real Windows C and a forward. The Windows arm
                    keeps the function, guarded; the forward goes.

    the table       MOONWATER_HAVE_* existed to say which of three
                    implementations a name got. With assembly on every
                    architecture there is one, so the table and every C
                    fallback under it go with it.

    the names       moonwater_ drops off, and the libc-shaped names become
                    the library's own: strlen is string_length, strnchr is
                    string_first_of_max, and so on down the list below.

What replaces the compatibility layer is aliases -- .set strlen,
string_length -- added by this pass, above the line where the file stops
being compiled into a kernel. No code: a second label on the same address.
Which names get one depends on who is linking, which is the comment that
goes with them.

    python3 kit/compact/restructure.py <file.c>
"""
import re, sys

FROM_MOONWATER = {
    'moonwater_basename':              'path_basename',
    'moonwater_bipolar_to_string':     'bipolar_to_string',
    'moonwater_copy':                  'memory_copy_fast',
    'moonwater_decimal_to_string':     'decimal_to_string',
    'moonwater_fast_sin':              'fast_sin',
    'moonwater_file_close':            'file_close',
    'moonwater_file_load':             'file_load',
    'moonwater_file_new':              'file_new',
    'moonwater_file_new_lazy':         'file_new_lazy',
    'moonwater_file_read':             'file_read',
    'moonwater_file_status':           'file_get_status',
    'moonwater_file_unload':           'file_unload',
    'moonwater_file_valid':            'file_valid',
    'moonwater_file_write':            'file_write',
    'moonwater_fill':                  'memory_fill',
    'moonwater_find':                  'string_find',
    'moonwater_format':                'string_format',
    'moonwater_get_environment':       'string_get_environment',
    'moonwater_lex_word':              'string_lex_word',
    'moonwater_library_close':         'library_close',
    'moonwater_library_get':           'library_get',
    'moonwater_library_open':          'library_open',
    'moonwater_memory':                'memory',
    'moonwater_memory_free':           'memory_free',
    'moonwater_move':                  'memory_copy',
    'moonwater_positive_to_string':    'positive_to_string',
    'moonwater_set_cursor':            'shell_set_cursor',
    'moonwater_span':                  'string_span',
    'moonwater_strchr':                'string_first_of',
    'moonwater_strcmp':                'string_compare',
    'moonwater_string_copy':           'string_copy',
    'moonwater_string_copy_max':       'string_copy_max',
    'moonwater_string_cut':            'string_cut',
    'moonwater_string_last_of':        'string_last_of',
    'moonwater_string_replace_all':    'string_replace_all',
    'moonwater_string_to_bipolar':     'string_to_bipolar',
    'moonwater_string_to_positive':    'string_to_positive',
    'moonwater_strlen':                'string_length',
    'moonwater_table_find':            'string_table_find',
    'moonwater_ticks':                 'get_cpu_time',
    'moonwater_working_directory_get': 'working_directory_get',
    'moonwater_working_directory_set': 'working_directory_set',
    'moonwater_digit_pairs':           'digit_pairs',
    'moonwater_decimal_constants':     'decimal_constants',
    'moonwater_decimal_marks':         'decimal_marks',
}

FROM_LIBC = {
    'strlen':    'string_length',
    'strcmp':    'string_compare',
    'strchr':    'string_first_of',
    'memchr':    'memory_first_of',
    'strrchr':   'string_last_of_or_end',
    'strncmp':   'string_compare_max',
    'strnlen':   'string_length_max',
    'strchrnul': 'string_first_of_or_end',
    'strnchr':   'string_first_of_max',
}

ALIASES = r'''
/*
        The libc names, as second labels rather than as code.

        A .set gives a routine a second name: strlen and string_length are one
        address, and a call through either is the same call. The forwarders
        that used to be here cost a jump to say that.

        Which names are ours depends on who is linking. A freestanding program
        gets all of them, the three the compiler emits calls to included -- a
        struct assignment becomes a memcpy and a zero fill a memset whether or
        not anyone wrote one, and the link fails on the undefined symbol when
        they are missing.

        A kernel gets exactly the ones build.sh handed over in that
        architecture's own string.h, and nothing else. Everything else it
        already has, and a second definition is a link that fails the other
        way round. That is nine names on x86_64, whose asm/string_64.h claims
        only the three memory ones, and the two nobody had written on arm64
        and riscv64.

        These sit above the userspace half of the file because the kernel is
        the reader that cannot see anything below it.
*/
#define ASM_ALIAS(name, target)         \
    ".globl " #name "\n"                \
    ".type " #name ", " ASM_TYPE "\n"   \
    ".set " #name ", " #target "\n"

__asm__(
    ASM_ALIAS(strchrnul, string_first_of_or_end)
    ASM_ALIAS(strnchr,   string_first_of_max)
#if !defined(KERNEL_MODE) || X64
    ASM_ALIAS(memchr,    memory_first_of)
    ASM_ALIAS(strlen,    string_length)
    ASM_ALIAS(strnlen,   string_length_max)
    ASM_ALIAS(strcmp,    string_compare)
    ASM_ALIAS(strncmp,   string_compare_max)
    ASM_ALIAS(strchr,    string_first_of)
    ASM_ALIAS(strrchr,   string_last_of_or_end)
#endif
#ifndef KERNEL_MODE
    ASM_ALIAS(memcpy,    memory_copy)
    ASM_ALIAS(memmove,   memory_copy)
    ASM_ALIAS(memset,    memory_fill)
    ASM_ALIAS(strcpy,    string_copy)
    ASM_ALIAS(strncpy,   string_copy_max)
    /*
            strncpy is the one name here that does not mean what libc means
            by it: string_copy_max pads nothing and writes no terminator when
            the source filled the bound. The alias keeps the behaviour the
            wrapper before it had rather than quietly changing it, and this is
            the warning that went with it.
    */
    ASM_ALIAS(strstr,    string_find)
#endif
);

'''

ANCHOR = '''// Userspace land
#ifndef KERNEL_MODE
'''

note = lambda m: sys.stderr.write('restructure: ' + m + '\n')


def drop_forwarders(lines):
    out, i, n = [], 0, 0
    while i < len(lines):
        l = lines[i]
        m = re.match(r'^[A-Za-z_][A-Za-z0-9_ *]*?\b([a-z_][a-z0-9_]*)\s*\(', l)
        if m and not l.rstrip().endswith(';') and i+1 < len(lines) \
           and lines[i+1].strip() == '{':
            j = i+2; body = []
            while j < len(lines) and lines[j].strip() != '}':
                if lines[j].strip(): body.append(lines[j].strip())
                j += 1
            if j < len(lines) and len(body) == 1:
                call = re.search(r'\b(moonwater_[a-z0-9_]+)\s*\(', body[0])
                if call and FROM_MOONWATER.get(call.group(1)) == m.group(1):
                    n += 1; i = j+1
                    while i < len(lines) and lines[i].strip() == '': i += 1
                    continue
        out.append(l); i += 1
    note(f'{n} forwarding functions removed')
    return out


def guard_windows(lines, asm):
    """Keep the Windows arm of a dispatcher; the other arm was a forward."""
    out, i, moved = [], 0, []
    while i < len(lines):
        l = lines[i]
        m = re.match(r'^[A-Za-z_][A-Za-z0-9_ *]*?\b([a-z_][a-z0-9_]*)\s*\(', l)
        if not (m and m.group(1) in asm and not l.rstrip().endswith(';')):
            out.append(l); i += 1; continue
        ob = None
        for k in range(i, min(i+4, len(lines))):
            if lines[k].strip() == '{': ob = k; break
            if lines[k].strip().endswith(';'): break
        if ob is None: out.append(l); i += 1; continue
        cb = next((k for k in range(ob+1, len(lines)) if lines[k] == '}'), None)
        if cb is None: out.append(l); i += 1; continue
        body = lines[ob+1:cb]
        if not (body and body[0].strip() in ('#if defined(WINDOWS)', '#ifdef WINDOWS')):
            out.append(l); i += 1; continue
        depth, els = 0, None
        for k, b in enumerate(body):
            t = b.strip()
            if t.startswith('#if'): depth += 1
            elif t.startswith('#endif'): depth -= 1
            elif t.startswith('#else') and depth == 1: els = k
        if els is None or body[-1].strip() != '#endif':
            out.append(l); i += 1; continue
        tail = [b for b in body[els+1:-1] if b.strip()]
        # The forward still calls the prefixed name, because the rename runs
        # later, and the two are not always the same word: file_get_status
        # forwards to moonwater_file_status. The map is what says they match.
        call = re.match(r'^\s*(?:return\s+)?([a-z_][a-z0-9_]*)\s*\(', tail[0]) \
               if len(tail) == 1 else None
        if not (call and (call.group(1) == m.group(1) or
                          FROM_MOONWATER.get(call.group(1)) == m.group(1))):
            out.append(l); i += 1; continue
        out.append('#if defined(WINDOWS)')
        out.extend(lines[i:ob+1]); out.extend(body[1:els])
        out.append('}'); out.append('#endif')
        moved.append(m.group(1)); i = cb+1
        while i < len(lines) and lines[i].strip() == '': i += 1
    note(f'{len(moved)} dispatchers now Windows only')
    return out


def kill_block(lines, pat):
    out, i, n = [], 0, 0
    while i < len(lines):
        if re.match(pat, lines[i]):
            depth, j = 0, i
            while j < len(lines):
                t = lines[j].strip()
                if t.startswith('#if'): depth += 1
                elif t.startswith('#endif'):
                    depth -= 1
                    if depth == 0: break
                j += 1
            i = j+1; n += 1
            while i < len(lines) and lines[i].strip() == '': i += 1
            continue
        out.append(lines[i]); i += 1
    return out, n


def main(path):
    s = open(path).read()
    asm = set(re.findall(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', s))
    lines = drop_forwarders(s.split('\n'))
    lines = guard_windows(lines, {FROM_MOONWATER.get(a, a) for a in asm} | asm)

    killed = 0
    for pat in (r'^#ifdef MOONWATER_HAVE_MOONWATER_STRLEN$',
                r'^#ifdef MOONWATER_HAVE_MOONWATER_STRCMP$',
                r'^#ifdef MOONWATER_HAVE_MOONWATER_GET_ENVIRONMENT$',
                r'^#ifndef MOONWATER_HAVE_STRLEN$',
                r'^#ifndef MOONWATER_HAVE_STRCMP$',
                r'^#ifndef MOONWATER_HAVE_STRCHR$',
                r'^#ifndef MOONWATER_HAVE_STRRCHR$'):
        lines, n = kill_block(lines, pat); killed += n
    note(f'{killed} HAVE blocks removed')

    s = '\n'.join(lines)
    s = re.sub(r'^#define MOONWATER_HAVE_[A-Z_]+ 1\n', '', s, flags=re.M)
    s = re.sub(r'^.*\.set\s+string_format, moonwater_format.*\n', '', s, flags=re.M)

    for old, new in sorted(FROM_MOONWATER.items(), key=lambda kv: -len(kv[0])):
        s = re.sub(r'\b' + old + r'\b', new, s)
        s = s.replace('.L' + old, '.L' + new)

    # Both held out of the renaming below, because both already mean the libc
    # name. ASM_EXPORT is EXPORT_SYMBOL, and what the rest of the kernel asks
    # the linker for is strlen, not string_length. The alias block is the same
    # thing said to the assembler, and it goes out whole, comment included:
    # renamed with everything else it would alias a name onto itself. That
    # only comes up when this pass runs on its own output, which is what
    # happens once the result is landed.
    #
    # Do not recognize that block by comparing it with this pass's original
    # template. The live block grows whenever another libc routine is claimed
    # and its kernel guards change with the claim list. An exact comparison
    # silently stopped protecting it, then the rename below produced
    # `.set string_length, string_length` and eight more self aliases. Find
    # the block by its structural bounds and put back the exact bytes read
    # from the input instead.
    s = re.sub(r'ASM_EXPORT\(([a-z_0-9]+)\)', r'ASM_KEEP_EXPORT<\1>', s)
    alias_block = re.search(
        r'/\*\n        The libc names, as second labels rather than as code\..*?'
        r'^__asm__\(\n.*?^\);\n\n', s, flags=re.S | re.M)
    aliases = alias_block.group(0) if alias_block else ALIASES
    if alias_block:
        s = s[:alias_block.start()] + 'ASM_KEEP_ALIASES\n' + s[alias_block.end():]

    for old, new in sorted(FROM_LIBC.items(), key=lambda kv: -len(kv[0])):
        for form in (f'ASM_FUNC({old})', f'ASM_END({old})'):
            s = s.replace(form, form.replace(old, new))
        s = re.sub(r'(?<=[\s,("])' + old + r'(?=[\s,:)("])', new, s)
        s = s.replace('.L' + old, '.L' + new)
    s = re.sub(r'ASM_KEEP_EXPORT<([a-z_0-9]+)>', r'ASM_EXPORT(\1)', s)
    s = s.replace('ASM_KEEP_ALIASES\n', aliases)
    note(f'{len(FROM_MOONWATER)} prefixed and {len(FROM_LIBC)} libc names renamed')

    # the libc-shaped prototypes said the same thing in another shape
    s = re.sub(r'/\*\n\s*Declared here only where nothing else has\.\n.*?\*/\n\n?',
               '', s, flags=re.S, count=1)
    s = s.replace('''#ifndef KERNEL_MODE
unsigned long string_length(const char address_to source);
int string_compare(const char address_to source, const char address_to input);
char address_to string_first_of(const char address_to source, int character);
#endif
''', '')


    # string_first_of is assembly everywhere now
    s = re.sub(r'string_address string_first_of\(string_address source, p8 character\)\n\{\n'
               r'#ifdef MOONWATER_HAVE_MOONWATER_STRCHR.*?\n#endif\n\}\n',
               '// Assembly on every architecture; the prototype above is all C needs.\n'
               '// It answers the same at the terminator as the byte loop it replaced:\n'
               '// string_first_of(s, 0) is the terminator, not nothing.\n',
               s, flags=re.S, count=1)

    # string_table_find is not assembly everywhere yet
    s = s.replace('''#ifdef MOONWATER_HAVE_TABLE_FIND
positive string_table_find(string_address name, address_any table,
                              positive stride, positive count);
#endif''', '''positive string_table_find(string_address name, address_any table,
                           positive stride, positive count);''')
    s = s.replace('''positive string_table_find(string_address name, address_any table,
                           positive stride, positive count)
{
#ifdef MOONWATER_HAVE_TABLE_FIND
        return string_table_find(name, table, stride, count);
#else
''', '''// The last routine still written in C, and only where assembly has not
// reached it. The inventory says which architectures those are.
#if !X64
positive string_table_find(string_address name, address_any table,
                           positive stride, positive count)
{
''')
    # the #endif that closed the old three way choice now closes the guard,
    # and belongs after the function rather than inside it
    s = s.replace('''        return count;
#endif
}
''', '''        return count;
}
#endif
''', 1)

    for old in ('''#undef memset
// for compatibility, makes the linker happy
address_any memset(address_any destination, int value, long unsigned int size)
{
        return memory_fill(destination, value, size);
}
''', '''#undef memcpy
// use memory_copy instead, this is for compatibility
address_any memcpy(address_any destination, address_any source, long unsigned int size)
{
        return memory_copy(destination, source, size);
}
'''):
        s = s.replace(old, '', 1)
    s = re.sub(r'#undef strcpy\n// use string_copy instead.*?\n\}\n', '', s, flags=re.S, count=1)
    s = re.sub(r'#undef strncpy\n// use string_copy_max instead.*?\n\}\n', '', s, flags=re.S, count=1)
    # and the comment that stood over all of them, with nothing left under it
    s = re.sub(r'\n+/\*\n\s+Where the assembly above defines one of these.*?\*/\n+(?=#endif)',
               '\n\n', s, flags=re.S, count=1)

    # Above the line, not below it. The anchor is where the file stops being
    # compiled into a kernel, and everything after it is inside a #ifndef
    # KERNEL_MODE that the preprocessor throws away before the compiler ever
    # reads it. Put here, the aliases were userspace-only and vmlinux had no
    # strlen. A guard of their own inside a block that is already gone says
    # nothing, which is why this was silent.
    #
    # Fatal rather than skipped: an alias emits no bytes, so a build without
    # one is a link error in another directory and nothing closer.
    if 'ASM_ALIAS(strlen,' not in s:
        if ANCHOR not in s:
            note('the alias anchor is gone -- see ANCHOR in this file')
            sys.exit(1)
        s = s.replace(ANCHOR, aliases + ANCHOR, 1)

    open(path, 'w').write(s)

# guarded so callers.py can import the two maps without running the pass
if __name__ == '__main__':
    main(sys.argv[1])
