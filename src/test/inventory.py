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


def object_mutation(directory, label, source, expected):
    path = fresh(directory, label + '.c')
    path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                    encoding='utf-8')
    result = invoke(path, '--check', '--target', 'direct')
    expect(result.returncode != 0 and
           ('forbidden C object %s ' % expected) in result.stderr,
           '%s object evaded the lexical scan' % label, result)


def object_macro_mutation(directory, label, source, expected):
    path = fresh(directory, label + '.c')
    path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                    encoding='utf-8')
    result = invoke(path, '--check', '--target', 'direct')
    expect(result.returncode != 0 and
           ('object-generating macro %s ' % expected) in result.stderr,
           '%s object macro evaded the lexical scan' % label, result)


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
extern const char body_text[];
struct aggregate { int (*callback)(void); };
struct __attribute__((packed)) attributed_aggregate { int value; };
union forward_union;
enum forward_enum;
extern struct aggregate data, *data_pointer;
extern struct aggregate data_array[];
extern int (*external_callback)(void);
extern _Thread_local int external_tls;
extern int external_one, external_two[2], *external_three,
           (*external_four)(void), external_function(void);
extern int external_attribute __attribute__((visibility("hidden")));
extern int external_asm asm("renamed_external");
int prototype(void);
int *pointer_result(void);
int (*function_result(void))(int);
KEEP b32 retained_main();
typedef int integer_alias;
typedef int (*callback_alias)(void);
typedef struct aggregate aggregate_alias;
typedef struct local_tag { int member; } local_tag;
typedef enum { KIND_ZERO, KIND_ONE } kind_alias;
_Static_assert(sizeof(int) >= 2, "int width");
static_assert(1, "accepted spelling");
__asm__(".text\nnot_c: # { this is assembly text }\n");
#define ASM_BODY_TEXT "static int macro_string(void) { return 1; }"
#define ir(asm_args...) asm volatile(asm_args)
#define ASM(name) asm_x64_##name
#define str(string) (string), (sizeof(string) - 1)
#define WIDE_LENGTH(W, ZEROED, ZEROS, LEAVE) \
        "assembly" W ZEROED ZEROS LEAVE ASM_RET
#define syscall(name) syscall_linux_x64_##name
#define MUL(a, b) a * b
#define address_to *
#define TYPE_ALIAS file address_to
#define FUNCTION_DECL(name) int name(void)
#define EXTERN_DECL(name) extern int name
''' , encoding='utf-8')
    result = invoke(ignored, '--check', '--target', 'direct')
    expect(result.returncode == 0,
           'declarations, type/tag definitions, expressions, or global asm '
           'looked like C storage',
           result)

    object_mutation(directory, 'initialized-object',
                    'int initialized_object = 1;\n', 'initialized_object')
    object_mutation(directory, 'tentative-object',
                    'positive tentative_object;\n', 'tentative_object')
    object_mutation(directory, 'static-object',
                    'static int static_object;\n', 'static_object')
    object_mutation(directory, 'const-array',
                    'const char const_array[] = { 1, 2 };\n', 'const_array')
    object_mutation(directory, 'tentative-array',
                    'int tentative_array[];\n', 'tentative_array')
    object_mutation(directory, 'pointer-object',
                    'object_type *pointer_object;\n', 'pointer_object')
    object_mutation(directory, 'function-pointer',
                    'int (*function_pointer)(void);\n', 'function_pointer')
    object_mutation(directory, 'function-pointer-array',
                    'int (*function_pointer_array[2])(void);\n',
                    'function_pointer_array')
    object_mutation(directory, 'thread-local',
                    '_Thread_local int thread_local_object;\n',
                    'thread_local_object')
    object_mutation(directory, 'c23-thread-local',
                    'thread_local int c23_thread_local;\n', 'c23_thread_local')
    object_mutation(directory, 'extern-initializer',
                    'extern int extern_definition = 1;\n', 'extern_definition')
    object_mutation(directory, 'extern-comma-mix',
                    'extern int declaration_only, comma_definition = 1, '
                    'also_declaration_only;\n', 'comma_definition')
    object_mutation(directory, 'function-object-comma-mix',
                    'static int function_declaration(void), mixed_object;\n',
                    'mixed_object')
    object_mutation(directory, 'tagged-aggregate-object',
                    'struct record { int value; } aggregate_object;\n',
                    'aggregate_object')
    object_mutation(directory, 'anonymous-enum-object',
                    'enum { STATE_ZERO, STATE_ONE } enum_object;\n',
                    'enum_object')
    object_mutation(directory, 'compound-literal-initializer',
                    'struct point compound_object = (struct point) { 0 };\n',
                    'compound_object')
    object_mutation(directory, 'attributed-object',
                    '[[gnu::used]] static int attributed_object;\n',
                    'attributed_object')
    object_mutation(directory, 'suffix-attributed-object',
                    'static int suffix_attributed_object '
                    '__attribute__((used));\n', 'suffix_attributed_object')
    object_mutation(directory, 'typeof-object',
                    'typeof(0) typeof_object;\n', 'typeof_object')
    object_mutation(directory, 'declspec-object',
                    '__declspec(allocate("named")) int declspec_object;\n',
                    'declspec_object')
    object_mutation(directory, 'aligned-object',
                    '_Alignas(64) static int aligned_object;\n',
                    'aligned_object')
    object_mutation(directory, 'inactive-object',
                    '#if 0\nstatic int dormant_object;\n#endif\n',
                    'dormant_object')
    object_mutation(directory, 'digraph-object',
                    'static int digraph_object[] = <% 1, 2 %>;\n',
                    'digraph_object')

    object_macro_mutation(
        directory, 'local-var-macro',
        '#define local_var(name) static int name\nlocal_var(local_storage);\n',
        'local_var')
    object_macro_mutation(
        directory, 'conversion-constants-macro',
        '#define CONVERSION_CONSTANTS \\\n'
        '        static const unsigned conversion_constants[] = { 1, 2 }\n'
        'CONVERSION_CONSTANTS;\n', 'CONVERSION_CONSTANTS')
    object_macro_mutation(
        directory, 'callback-object-macro',
        '#define CALLBACK_OBJECT(name) static int (*name)(void)\n'
        'CALLBACK_OBJECT(callback_storage);\n', 'CALLBACK_OBJECT')
    object_macro_mutation(
        directory, 'custom-type-object-macro',
        '#define CUSTOM_OBJECT(name) object_type name\n'
        'CUSTOM_OBJECT(custom_storage);\n', 'CUSTOM_OBJECT')
    object_macro_mutation(
        directory, 'fixed-object-macro',
        '#define FIXED_OBJECT object_type fixed_storage\nFIXED_OBJECT;\n',
        'FIXED_OBJECT')
    object_macro_mutation(
        directory, 'extern-definition-macro',
        '#define EXTERN_DEFINITION(name) extern int name = 1\n'
        'EXTERN_DEFINITION(extern_storage);\n', 'EXTERN_DEFINITION')

    object_report = directory / 'object-report.c'
    object_report.write_text(BASE + 'static int reported_object;\n',
                             encoding='utf-8')
    result = invoke(object_report, '--target', 'direct')
    report_text = object_report.read_text(encoding='utf-8')
    expect(result.returncode != 0 and
           'Raw C purity: 0 function bodies, 1 object definitions' in report_text and
           'C object definitions still present (forbidden):' in report_text and
           'reported_object' in report_text,
           'generated report did not account for forbidden C storage', result)

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

    object_graph_root = directory / 'object-graph-root.c'
    object_graph_root.write_text(BASE + '''\
#if defined(LINUX)
#include "object-first.inc"
#endif
''', encoding='utf-8')
    (directory / 'object-first.inc').write_text(
        '#include "object-second.inc"\n', encoding='utf-8')
    (directory / 'object-second.inc').write_text(
        'extern int declaration_only;\n'
        'static int nested_object;\n'
        '#define NESTED_OBJECT(name) static int name\n', encoding='utf-8')
    result = invoke(object_graph_root, '--target', 'direct')
    expect(result.returncode == 0,
           'could not generate recursive object fixture', result)
    result = invoke(object_graph_root, '--check', '--target', 'linux')
    expect(result.returncode != 0 and
           'forbidden C object nested_object' in result.stderr and
           'object-generating macro NESTED_OBJECT' in result.stderr,
           'object or object macro in a recursive .inc graph evaded the gate',
           result)
    result = invoke(object_graph_root, '--target', 'linux')
    expect(result.returncode != 0 and
           'forbidden C object nested_object' in result.stderr,
           'write/rebuild mode silently accepted included C storage', result)

    inactive_root = fresh(directory, 'inactive-root.c', BASE + '''\
#if defined(WINDOWS)
#include "windows.inc"
#endif
''')
    (directory / 'windows.inc').write_text(
        'static int windows_body(void) { return 1; }\n'
        'static int windows_object;\n', encoding='utf-8')
    result = invoke(inactive_root, '--check', '--target', 'linux')
    expect(result.returncode == 0,
           'Linux-active gate followed a Windows-inactive include', result)
    result = invoke(inactive_root, '--check', '--target', 'all')
    expect(result.returncode != 0 and
           'forbidden C body windows_body' in result.stderr and
           'forbidden C object windows_object' in result.stderr,
           'all-platform include audit missed inactive platform C', result)

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
