#!/usr/bin/env python3
"""Audit performance-evidence coverage for library.c's assembly inventory.

This is a coverage manifest, not a result database. ``direct_benchmark`` means
that a shipped harness times the named routine as the subject of a row. It does
not mean that a recent native result is stored in the repository, and a qemu
run is never treated as hardware timing. ``benchmark_context`` means a private
core runs inside such a row but is not isolated. ``static_leaf`` records a small
syscall/ABI/counter/byte-order leaf whose source shape can be reviewed; it is
explicitly neither a measurement nor a proof of optimality. The remaining two
classes carry correctness evidence only, or no focused evidence at all.

The check is intentionally coupled to the generated assembly inventory. A new,
removed, or renamed routine makes this fail until somebody classifies it and
names an evidence anchor that actually exists.

    python3 kit/performance_coverage.py
    python3 kit/performance_coverage.py --gaps
    python3 kit/performance_coverage.py --all
"""

import argparse
import importlib.util
import pathlib
import re
import sys
from collections import Counter, namedtuple


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIBRARY = ROOT / 'src/library.c'
INVENTORY_TOOL = ROOT / 'kit/compact/inventory.py'

Coverage = namedtuple('Coverage', 'routine category evidence anchor note')
ROWS = []

CATEGORY_DESCRIPTION = {
    'direct_benchmark': 'direct timing/floor harness',
    'benchmark_context': 'timed only inside a benchmarked wrapper',
    'static_leaf': 'static syscall/ABI/byte-order review; no timing claim',
    'correctness_only': 'correctness evidence only; no timing claim',
    'unmeasured': 'no focused correctness or performance evidence located',
}


def cover(category, evidence, names, note, anchors=None):
    """Add a compact evidence group to the manifest below."""
    anchors = anchors or {}
    for routine in names.split():
        ROWS.append(Coverage(routine, category, evidence,
                             anchors.get(routine, routine), note))


# Direct subjects: these names are passed to a timed runner or appear in a
# dedicated floor row, rather than merely helping the harness print or count.
cover('direct_benchmark', 'kit/floor.c', '''
memory_compare memory_copy memory_copy_fast memory_count memory_fill
memory_first_of string_compare string_copy string_first_of
string_last_of_or_end string_length
''', 'floor-relative rows over multiple sizes')

cover('direct_benchmark', 'kit/bench_numbers.c', '''
positive_into positive_to_string
''', 'paired former-C/assembly timing over numeric distributions')

cover('direct_benchmark', 'kit/bench_bases.c', '''
positive_into_base
''', 'paired scalar/assembly timing across bases and value widths')

cover('direct_benchmark', 'kit/bench_padded.c', '''
positive_to_padded
''', 'paired former-C/assembly timing across field shapes')

cover('direct_benchmark', 'kit/bench_parse_bases.c', '''
string_digits_base_max string_digits_hexadecimal_escape_max
string_digits_hexadecimal_max string_digits_octal_escape_max
string_digits_octal_max
''', 'paired scalar/assembly timing with bounded parser inputs')

cover('direct_benchmark', 'kit/bench_into_padded.c', '''
positive_into_padded positive_into_pair
''', 'paired former-C/assembly timing across padded converter shapes')

cover('direct_benchmark', 'kit/bench_human.c', '''
positive_into_human_1024_string positive_to_human_1024
''', 'paired former-C/assembly timing for buffer and writer forms')

cover('direct_benchmark', 'kit/bench_human_nearest.c', '''
positive_into_human_nearest_string
''', 'paired former-C/assembly timing for decimal and binary forms')

cover('direct_benchmark', 'kit/bench_paths.c', '''
path_head_copy path_join path_tail_copy
''', 'paired former-C/assembly timing over short, nested, and long paths')

cover('direct_benchmark', 'kit/bench_reverse.c', '''
memory_reverse
''', 'paired former-C/assembly timing over primitive and folded rev shapes')

cover('direct_benchmark', 'kit/bench_writer_field.c', '''
string_to_field writer_field
''', 'paired former-C/assembly timing over exact and padded fields')
cover('direct_benchmark', 'kit/bench_writer_text.c', '''
buffered_flush buffered_write buffered_write_byte buffered_write_deferred_equal
''', 'paired former-C/assembly timing over buffered and direct output shapes')
cover('direct_benchmark', 'kit/bench_text_hot.c', 'memory_common_prefix',
      'paired scalar/assembly timing over equal and late-difference text spans')
cover('direct_benchmark', 'kit/bench_ascii_case.c', 'memory_compare_ascii_case',
      'exhaustive byte-pair validation and paired folded comparison timing')
cover('direct_benchmark', 'kit/bench_translate.c', 'memory_translate',
      'paired former-C/assembly timing over byte-table translation sizes')


# Private cores are present in the timed call graph, but the harness cannot
# assign their cost independently from their public wrappers.
cover('benchmark_context', 'kit/bench_paths.c', 'path_split_core',
      'private core reached by all three directly timed path wrappers',
      {'path_split_core': 'path_head_copy'})
cover('benchmark_context', 'kit/bench_numbers.c', '''
positive_digits_core positive_into_core
''', 'private conversion core reached by a directly timed public converter',
      {'positive_digits_core': 'positive_to_string',
       'positive_into_core': 'positive_into'})
cover('benchmark_context', 'kit/bench_writer_field.c', 'writer_field_core',
      'private core reached by both directly timed field wrappers',
      {'writer_field_core': 'writer_field'})
cover('benchmark_context', 'kit/bench_writer_text.c', 'buffered_write_core',
      'private core reached by both directly timed buffer-policy wrappers',
      {'buffered_write_core': 'buffered_write'})


# These are deliberately a static classification. Loading a syscall number and
# trapping, moving ABI arguments, reading the architectural counter, or doing a
# byte swap has no direct timing row here. Do not turn this category into a
# "hardware floor" assertion without measured evidence.
cover('static_leaf', 'src/platform/linux.inc', '_start exit sleep',
      'startup or direct Linux syscall ABI body; statically reviewed only')
cover('static_leaf', 'src/platform/socket.inc', '''
bytes_reverse_16 bytes_reverse_32 network_load_16 network_load_32
network_store_16 network_store_32 socket_accept socket_bind socket_close
socket_connect socket_listen socket_name socket_new socket_option_get
socket_option_set socket_receive socket_send socket_shutdown
''', 'straight-line byte-order or socket syscall ABI body; statically reviewed only')
cover('static_leaf', 'src/library.c', '''
get_cpu_time system_call system_call_1 system_call_2 system_call_3
system_call_4 system_call_5 system_call_6
''', 'counter read or generic syscall ABI body; statically reviewed only')


# Direct correctness references in the exhaustive assembly verifier. None of
# these references supplies isolated timing evidence.
cover('correctness_only', 'src/test/verify.c', '''
bipolar_into bipolar_into_string bipolar_to_string byte_class_holds
byte_class_index decimal_to_string fast_sin file_close file_get_status
file_load file_new file_read file_valid file_write memory memory_copy_end
memory_copy_fast_end memory_free memory_search path_basename positive_digits
positive_into_string positive_to_base_field program_arguments_own
program_arguments_use program_environment program_environment_list
string_append string_bipolar string_compare_max string_copy_max
string_copy_max_end string_cut string_digits string_digits_exact
string_digits_max string_find string_first_of_max string_first_of_or_end
string_format string_get_environment string_last_of string_length_max
string_lex_word string_replace_all string_search string_set_add string_span
string_span_max string_table_find string_to_bipolar string_to_positive
wait_status_code_base working_directory_get working_directory_set writer_fill
''', 'direct correctness coverage in the assembly verifier')

# Indirect correctness anchors and focused subsystem tests. The anchor is named
# separately whenever the private/helper routine itself is not in the test.
cover('correctness_only', 'src/test/verify.c', 'bipolar_into_core file_unload',
      'correctness exercised through a public wrapper, not timed',
      {'bipolar_into_core': 'bipolar_into', 'file_unload': 'file_close'})
cover('correctness_only', 'src/test/slurp.c', 'file_slurp',
      'focused file-slurp correctness and error-path test')
cover('correctness_only', 'src/test/socket.c', 'host_into string_to_host',
      'focused socket conversion correctness test')
cover('correctness_only', 'src/test/standard.c', 'log log_direct log_flush',
      'exercised by standard output checks; no isolated timing')
cover('correctness_only', 'src/test/probe.c', '''
log_error program_argument program_argument_count term_size
''', 'focused runtime/probe behavior checks; no isolated timing')
cover('correctness_only', 'src/test/exact.c', 'moonwater_cpu_detect',
      'feature dispatch exercised by exact-size correctness tiers')
cover('correctness_only', 'src/test/wait_retry.c', '''
system_read_retry system_wait4_retry system_write_all wait_status_code
''', 'focused retry/status correctness and signal-interruption checks')
cover('correctness_only', 'src/test/native/reserve.c', '''
memory_growth memory_release memory_reserve
''', 'exact lifted ARM64 growth, overflow, failure and release checks')


# No focused artifact was found. These entries are intentional gaps, not an
# inference that the routines are wrong or slow.
cover('unmeasured', None, '''
file_new_lazy library_close library_get library_open shell_set_cursor
''', 'no focused correctness test or direct performance harness located')


def load_inventory():
    spec = importlib.util.spec_from_file_location(
        'moonwater_inventory_for_performance_coverage', INVENTORY_TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError('could not load %s' % INVENTORY_TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _, _, _, sources = module.included_audit(str(LIBRARY), 'linux')
    _, order, _, _, _, _, _ = module.graph_assembly_inventory(sources)
    return set(order)


def token_present(text, name):
    return re.search(r'(?<![A-Za-z0-9_])' + re.escape(name) +
                     r'(?![A-Za-z0-9_])', text) is not None


def validate():
    errors = []
    inventory = load_inventory()
    counts = Counter(row.routine for row in ROWS)
    duplicates = sorted(name for name, count in counts.items() if count != 1)
    manifested = set(counts)

    if duplicates:
        errors.append('duplicate manifest rows: ' + ', '.join(duplicates))
    if inventory - manifested:
        errors.append('unclassified inventory routines: ' +
                      ', '.join(sorted(inventory - manifested)))
    if manifested - inventory:
        errors.append('stale manifest routines: ' +
                      ', '.join(sorted(manifested - inventory)))

    cache = {}
    benchmark_dispatch = (ROOT / 'kit/bench').read_text(encoding='utf-8')
    for row in ROWS:
        if row.category not in CATEGORY_DESCRIPTION:
            errors.append('%s: unknown category %s' %
                          (row.routine, row.category))
            continue
        if row.category == 'unmeasured':
            if row.evidence is not None:
                errors.append('%s: unmeasured row has evidence' % row.routine)
            continue
        if not row.evidence:
            errors.append('%s: %s row lacks evidence' %
                          (row.routine, row.category))
            continue
        evidence_path = ROOT / row.evidence
        if not evidence_path.is_file():
            errors.append('%s: missing evidence file %s' %
                          (row.routine, row.evidence))
            continue
        if row.evidence not in cache:
            cache[row.evidence] = evidence_path.read_text(
                encoding='utf-8', errors='replace')
        if not token_present(cache[row.evidence], row.anchor):
            errors.append('%s: anchor %s absent from %s' %
                          (row.routine, row.anchor, row.evidence))
        if row.category in ('direct_benchmark', 'benchmark_context'):
            basename = evidence_path.name
            if basename not in benchmark_dispatch:
                errors.append('%s: benchmark %s is not dispatched by kit/bench' %
                              (row.routine, row.evidence))
    return errors


def print_report(mode):
    counts = Counter(row.category for row in ROWS)
    total = len(ROWS)
    direct = counts['direct_benchmark']
    context = counts['benchmark_context']

    print('performance evidence coverage: %d routines classified' % total)
    for category in CATEGORY_DESCRIPTION:
        print('  %-20s %3d  %s' %
              (category, counts[category], CATEGORY_DESCRIPTION[category]))
    print('  direct isolated timing %d/%d; no direct timing row %d/%d' %
          (direct, total, total - direct, total))
    print('  benchmark context only %d/%d (not counted as direct)' %
          (context, total))

    if mode == 'summary':
        gaps = [row.routine for row in ROWS if row.category == 'unmeasured']
        print('  focused-evidence gaps: ' + ', '.join(sorted(gaps)))
        return

    if mode == 'gaps':
        selected = [row for row in ROWS if row.category != 'direct_benchmark']
    else:
        selected = list(ROWS)
    for row in sorted(selected, key=lambda item: item.routine):
        evidence = row.evidence or '-'
        anchor = '' if row.anchor == row.routine else ' via ' + row.anchor
        print('  %-30s %-18s %s%s -- %s' %
              (row.routine, row.category, evidence, anchor, row.note))


def main():
    parser = argparse.ArgumentParser(
        description='check all assembly routines for explicit performance evidence')
    output = parser.add_mutually_exclusive_group()
    output.add_argument('--gaps', action='store_true',
                        help='list every routine without a direct benchmark row')
    output.add_argument('--all', action='store_true',
                        help='list every manifest row')
    arguments = parser.parse_args()

    try:
        errors = validate()
    except Exception as error:  # keep CI diagnostics short and actionable
        sys.stderr.write('performance coverage: %s\n' % error)
        return 1
    if errors:
        for error in errors:
            sys.stderr.write('performance coverage: %s\n' % error)
        return 1

    mode = 'all' if arguments.all else ('gaps' if arguments.gaps else 'summary')
    print_report(mode)
    return 0


if __name__ == '__main__':
    sys.exit(main())
