#!/usr/bin/env python3
"""Mutation tests for the raw zero-C/parity inventory gate."""

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
INVENTORY = ROOT / 'kit/compact/inventory.py'

BASE = '''\
/* deliberately tiny input for the inventory generator */
#ifndef STANDARD_MODERN_C
#define STANDARD_MODERN_C
#if X64
ASM_FUNC(probe)
ASM_END(probe)
#elif ARM64
ASM_FUNC(probe)
ASM_END(probe)
#elif RISCV64
ASM_FUNC(probe)
ASM_END(probe)
#endif
#endif
'''

checks = 0


def invoke(path, *options):
    return subprocess.run(
        [sys.executable, str(INVENTORY), *options, str(path)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False)


def expect(condition, message, result=None):
    global checks
    checks += 1
    if condition:
        return
    detail = '' if result is None else '\nstdout:\n%s\nstderr:\n%s' % (
        result.stdout, result.stderr)
    raise AssertionError(message + detail)


def fresh(directory, name='library.c', source=BASE):
    path = directory / name
    path.write_text(source, encoding='utf-8')
    result = invoke(path, '--target', 'direct')
    expect(result.returncode == 0, 'clean write mode failed', result)
    return path


def body_mutation(directory, label, source, expected):
    path = fresh(directory, label + '.c')
    path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                    encoding='utf-8')
    result = invoke(path, '--check', '--target', 'direct')
    expect(result.returncode != 0 and
           ('forbidden C body %s ' % expected) in result.stderr,
           '%s body evaded the lexical scan' % label, result)


with tempfile.TemporaryDirectory(prefix='inventory-test-') as temporary:
    directory = pathlib.Path(temporary)

    clean = fresh(directory)
    result = invoke(clean, '--check', '--target', 'direct')
    expect(result.returncode == 0, 'freshly generated inventory is stale', result)

    stale = directory / 'stale.c'
    stale.write_text(BASE, encoding='utf-8')
    result = invoke(stale, '--check', '--target', 'direct')
    expect(result.returncode != 0 and 'generated block is stale' in result.stderr,
           'check mode accepted a stale/missing generated block', result)

    body_mutation(directory, 'same-line',
                  'static int same_line(void) { return 1; }\n', 'same_line')
    body_mutation(directory, 'indented',
                  '    static int indented(void)\n    {\n        return 1;\n    }\n',
                  'indented')
    body_mutation(directory, 'prefix-attribute',
                  '__attribute__((used))\nstatic int prefix_attribute(void) { return 1; }\n',
                  'prefix_attribute')
    body_mutation(directory, 'suffix-attribute',
                  'static int suffix_attribute(void) __attribute__((used))\n'
                  '{\n    return 1;\n}\n',
                  'suffix_attribute')
    body_mutation(directory, 'suffix-attribute-macro',
                  '#define NOINLINE __attribute__((noinline))\n'
                  'static int suffix_attribute_macro(void) NOINLINE\n'
                  '{\n    return 1;\n}\n',
                  'suffix_attribute_macro')
    body_mutation(directory, 'c23-attribute',
                  '[[gnu::used]] static int c23_attribute(void) { return 1; }\n',
                  'c23_attribute')
    body_mutation(directory, 'multiline',
                  'static int\nmultiline(\n    int value\n)\n{\n    return value;\n}\n',
                  'multiline')
    body_mutation(directory, 'inactive',
                  '#if 0\nstatic int dormant(void) { return 1; }\n#endif\n',
                  'dormant')
    body_mutation(directory, 'conditional-attribute',
                  'static int conditional_attribute(void)\n'
                  '#if 0\n__attribute__((used))\n#endif\n'
                  '{\n    return 1;\n}\n',
                  'conditional_attribute')
    body_mutation(directory, 'pointer-return',
                  'static int (*pointer_return(void))(int)\n{\n    return 0;\n}\n',
                  'pointer_return')
    body_mutation(directory, 'old-style',
                  'int old_style(value)\nint value;\n'
                  '{\n    return value;\n}\n',
                  'old_style')
    body_mutation(directory, 'digraph-braces',
                  'static int digraph_braces(void) <% return 1; %>\n',
                  'digraph_braces')

    ignored = fresh(directory, 'ignored.c')
    ignored.write_text(ignored.read_text(encoding='utf-8') + r'''
/* static int comment_body(void) { return 1; } */
static const char body_text[] = "static int string_body(void) { return 1; }";
struct aggregate { int (*callback)(void); };
struct __attribute__((packed)) attributed_aggregate { int value; };
static struct aggregate data = { 0 };
static struct aggregate compound = (struct aggregate) { 0 };
__asm__(".text\nnot_c: # { this is assembly text }\n");
#define ASM_BODY_TEXT "static int macro_string(void) { return 1; }"
''' , encoding='utf-8')
    result = invoke(ignored, '--check', '--target', 'direct')
    expect(result.returncode == 0,
           'comments, literals, data, aggregates, or global asm looked like C bodies',
           result)

    write_body = directory / 'write-body.c'
    write_body.write_text(BASE + 'static int write_body(void) { return 1; }\n',
                         encoding='utf-8')
    result = invoke(write_body, '--target', 'direct')
    expect(result.returncode != 0 and 'forbidden C body write_body' in result.stderr,
           'write mode silently accepted a C body', result)

    macro_body = fresh(directory, 'macro-body.c')
    macro_body.write_text(macro_body.read_text(encoding='utf-8') + '''\
#define BODY_MACRO(name) \\
        static int name(void) { return 1; }
BODY_MACRO(generated)
''', encoding='utf-8')
    result = invoke(macro_body, '--check', '--target', 'direct')
    expect(result.returncode != 0 and
           'body-generating macro BODY_MACRO' in result.stderr,
           'an invoked body-generating macro evaded the raw zero-C gate', result)

    gap = directory / 'gap.c'
    gap.write_text(BASE.replace(
        '#elif RISCV64\nASM_FUNC(probe)\nASM_END(probe)\n', ''),
                   encoding='utf-8')
    result = invoke(gap, '--target', 'direct')
    expect(result.returncode != 0 and '1 not at parity' in result.stderr,
           'write mode silently accepted an architecture gap', result)

    duplicate = directory / 'duplicate.c'
    duplicate.write_text(BASE.replace(
        '#elif ARM64', 'ASM_FUNC(probe)\nASM_END(probe)\n#elif ARM64'),
                         encoding='utf-8')
    result = invoke(duplicate, '--target', 'direct')
    expect(result.returncode != 0 and
           'duplicate assembly function probe on X64' in result.stderr,
           'duplicate architecture body was hidden by the set inventory', result)

    unscoped = directory / 'unscoped.c'
    unscoped.write_text(BASE + 'ASM_FUNC(outside)\n', encoding='utf-8')
    result = invoke(unscoped, '--target', 'direct')
    expect(result.returncode != 0 and 'unscoped ASM_FUNC outside' in result.stderr,
           'unscoped assembly routine disappeared from parity accounting', result)

    kind_mismatch = directory / 'kind-mismatch.c'
    kind_mismatch.write_text(BASE.replace(
        '#elif ARM64\nASM_FUNC(probe)\nASM_END(probe)',
        '#elif ARM64\nASM_LOCAL_FUNC(probe)\nASM_LOCAL_END(probe)'),
        encoding='utf-8')
    result = invoke(kind_mismatch, '--target', 'direct')
    expect(result.returncode != 0 and
           'assembly function scope mismatch probe' in result.stderr,
           'public/local architecture mismatch evaded the audit', result)

    missing_end = directory / 'missing-end.c'
    missing_end.write_text(BASE.replace('ASM_END(probe)\n', '', 1),
                           encoding='utf-8')
    result = invoke(missing_end, '--target', 'direct')
    expect(result.returncode != 0 and 'has no matching end marker' in result.stderr,
           'missing assembly end marker evaded the audit', result)

    wrong_name = directory / 'wrong-end-name.c'
    wrong_name.write_text(BASE.replace('ASM_END(probe)', 'ASM_END(other)', 1),
                          encoding='utf-8')
    result = invoke(wrong_name, '--target', 'direct')
    expect(result.returncode != 0 and
           'ASM_END(other) does not close ASM_FUNC(probe)' in result.stderr,
           'wrong assembly end name evaded the audit', result)

    wrong_kind = directory / 'wrong-end-kind.c'
    wrong_kind.write_text(BASE.replace(
        'ASM_END(probe)', 'ASM_LOCAL_END(probe)', 1), encoding='utf-8')
    result = invoke(wrong_kind, '--target', 'direct')
    expect(result.returncode != 0 and
           'ASM_LOCAL_END(probe) does not close ASM_FUNC(probe)' in result.stderr,
           'wrong public/local end marker evaded the audit', result)

    orphan_end = directory / 'orphan-end.c'
    orphan_end.write_text(BASE.replace(
        'ASM_END(probe)\n#elif ARM64',
        'ASM_END(probe)\nASM_END(orphan)\n#elif ARM64'), encoding='utf-8')
    result = invoke(orphan_end, '--target', 'direct')
    expect(result.returncode != 0 and 'ASM_END(orphan) has no opener' in result.stderr,
           'orphan assembly end marker evaded the audit', result)

    overlap = directory / 'overlapping-functions.c'
    overlap.write_text(BASE.replace(
        'ASM_END(probe)\n#elif ARM64',
        'ASM_FUNC(second)\nASM_END(second)\n#elif ARM64'), encoding='utf-8')
    result = invoke(overlap, '--target', 'direct')
    expect(result.returncode != 0 and
           'ASM_FUNC(second) opens before ASM_FUNC(probe)' in result.stderr,
           'overlapping assembly function extents evaded the audit', result)

    directive_names = fresh(
        directory, 'directive-names.c',
        '#define ASM_LOCAL_FUNC(name) ignored_begin(name)\n'
        '#define ASM_LOCAL_END(name) ignored_end(name)\n' + BASE)
    result = invoke(directive_names, '--check', '--target', 'direct')
    expect(result.returncode == 0,
           'macro definitions themselves became assembly inventory entries', result)

    c_include = directory / 'c-include.c'
    c_include.write_text(BASE + '''\
#if 0
/* comments count as directive whitespace */ # inc\\
lude /* and here */ "renamed.C"
# include <directory//angle.c>
%:include "digraph.c"
#endif
''', encoding='utf-8')
    result = invoke(c_include, '--target', 'direct')
    expect(result.returncode != 0 and
           'forbidden .c include renamed.C' in result.stderr and
           'forbidden .c include directory//angle.c' in result.stderr and
           'forbidden .c include digraph.c' in result.stderr,
           'inactive/commented/spliced textual .c include evaded hygiene', result)

    runtime_root = directory / 'runtime-root.c'
    runtime_root.write_text(BASE + '#include "runtime.inc"\n', encoding='utf-8')
    runtime = directory / 'runtime.inc'
    runtime.write_text('''\
#if X64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#elif ARM64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#elif RISCV64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#endif
''', encoding='utf-8')
    result = invoke(runtime_root, '--target', 'direct')
    expect(result.returncode == 0 and '2 routines, 0 not at parity' in result.stderr,
           'Linux include-graph assembly did not join the generated inventory', result)
    runtime.write_text(runtime.read_text(encoding='utf-8').replace(
        '#elif RISCV64\nASM_LOCAL_FUNC(runtime_probe)\n'
        'ASM_LOCAL_END(runtime_probe)\n', ''), encoding='utf-8')
    result = invoke(runtime_root, '--check', '--target', 'linux')
    expect(result.returncode != 0 and
           'assembly function runtime_probe missing on RISCV64' in result.stderr,
           'included runtime architecture gap evaded parity accounting', result)

    active_root = directory / 'active-root.c'
    active_root.write_text(BASE + '''\
#if defined(LINUX)
#include "first.inc"
#endif
''', encoding='utf-8')
    (directory / 'first.inc').write_text('#include "second.inc"\n', encoding='utf-8')
    (directory / 'second.inc').write_text(
        'static int nested_body(void) { return 1; }\n', encoding='utf-8')
    result = invoke(active_root, '--target', 'direct')
    expect(result.returncode == 0, 'could not generate active-include fixture', result)
    result = invoke(active_root, '--check', '--target', 'linux')
    expect(result.returncode != 0 and 'forbidden C body nested_body' in result.stderr,
           'body in a recursively included renamed .inc evaded the Linux gate', result)
    result = invoke(active_root, '--target', 'linux')
    expect(result.returncode != 0 and 'forbidden C body nested_body' in result.stderr,
           'write/rebuild mode silently accepted an included C body', result)

    inactive_root = fresh(directory, 'inactive-root.c', BASE + '''\
#if defined(WINDOWS)
#include "windows.inc"
#endif
''')
    (directory / 'windows.inc').write_text(
        'static int windows_body(void) { return 1; }\n', encoding='utf-8')
    result = invoke(inactive_root, '--check', '--target', 'linux')
    expect(result.returncode == 0,
           'Linux-active gate followed a Windows-inactive include', result)
    result = invoke(inactive_root, '--check', '--target', 'all')
    expect(result.returncode != 0 and 'forbidden C body windows_body' in result.stderr,
           'all-platform include audit missed an inactive platform body', result)

    mac_root = directory / 'mac-root.c'
    mac_root.write_text(BASE + '''\
#if defined(MACOS)
#include "platform/macos.inc"
#endif
''', encoding='utf-8')
    platform = directory / 'platform'
    platform.mkdir()
    mac_runtime = platform / 'macos.inc'
    mac_runtime.write_text(r'''
#if defined(X64)
__asm__(
    ".globl _sleep\n" "_sleep:\n"
    ".globl _exit\n" "_exit:\n"
    ".globl _start\n" "_start:\n"
    ".globl __start\n" "__start:\n");
#elif defined(ARM64)
__asm__(
    ".globl _sleep\n" "_sleep:\n"
    ".globl _exit\n" "_exit:\n"
    ".globl _start\n" "_start:\n"
    ".globl __start\n" "__start:\n");
#endif
''', encoding='utf-8')
    result = invoke(mac_root, '--target', 'direct')
    expect(result.returncode == 0,
           'complete x64/ARM64 Mach-O runtime parity was rejected', result)
    mac_text = mac_runtime.read_text(encoding='utf-8')
    last_global = mac_text.rfind('".globl _exit\\n"')
    expect(last_global >= 0, 'Mach-O fixture did not contain its ARM64 exit global')
    mac_runtime.write_text(mac_text[:last_global] +
                           '".hidden _exit\\n"' +
                           mac_text[last_global + len('".globl _exit\\n"'):],
                           encoding='utf-8')
    result = invoke(mac_root, '--check', '--target', 'all')
    expect(result.returncode != 0 and
           'macOS ARM64 global _exit expected once, found 0' in result.stderr,
           'raw Mach-O ARM64 global parity gap evaded the audit', result)

    missing = fresh(directory, 'missing-root.c')
    missing.write_text(missing.read_text(encoding='utf-8') +
                       '#include "missing.inc"\n', encoding='utf-8')
    result = invoke(missing, '--check', '--target', 'linux')
    expect(result.returncode != 0 and 'missing quoted include missing.inc' in result.stderr,
           'missing local include silently weakened the recursive audit', result)

print('inventory mutation audit: %d checks' % checks)
