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

import pathlib
import sys
from collections import Counter, namedtuple

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'compact'))
import manifest

ROOT = manifest.ROOT

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
cover('direct_benchmark', 'test/checks.c#BENCH_floor', '''
memory_compare memory_copy memory_copy_apart memory_count memory_fill
memory_first_of string_compare string_first_of
string_last_of_or_end string_length
''', 'floor-relative rows over multiple sizes')

cover('direct_benchmark', 'test/checks.c#BENCH_numbers', '''
positive_into positive_to_string
''', 'paired former-C/assembly timing over numeric distributions')

cover('direct_benchmark', 'test/checks.c#BENCH_bases', '''
positive_into_base
''', 'paired scalar/assembly timing across bases and value widths')

cover('direct_benchmark', 'test/checks.c#BENCH_hex', 'memory_into_hex',
      'paired former-C/assembly timing over byte tails, dump rows and large spans')

cover('direct_benchmark', 'test/checks.c#BENCH_fixed', 'memory_decimal_series',
      'bounded decimal-record expansion against a C carry loop; native timing required')

cover('benchmark_context', 'test/checks.c#BENCH_escape', '''
memory_escape_index memory_into_escaped
''', 'shared primitives timed through sparse/dense hex and JSON writer workloads',
      anchors={'memory_escape_index': 'writer_hex_escaped',
               'memory_into_escaped': 'writer_hex_escaped'})

cover('direct_benchmark', 'test/checks.c#BENCH_codec', '''
memory_encode_power2 memory_decode_power2
''', 'bounded codec quanta against independent scalar bit loops; native timing required')

cover('correctness_only', 'test/checks.c#CHECK_codec', 'memory_into_hex_case',
      'case-selectable entry shares the existing hexadecimal assembly core')

cover('direct_benchmark', 'test/checks.c#BENCH_padded', '''
positive_to_padded
''', 'paired former-C/assembly timing across field shapes')

cover('direct_benchmark', 'test/checks.c#BENCH_input_bases', '''
string_digits_base_max string_digits_hexadecimal_escape_max
string_digits_hexadecimal_max string_digits_octal_escape_max
string_digits_octal_max
''', 'paired scalar/assembly timing with bounded parser inputs')

cover('direct_benchmark', 'test/checks.c#BENCH_reserve', 'memory_reserve',
      'fresh-process mapping growth timing and peak resident memory, with '
      'dense and sparse inputs; hardware/RSS claims require native execution')

# The text family: exported for programs that link this library, and called
# by nothing in the tree, so there is no workload here whose speed they could
# change and no timing row is claimed for them.
cover('correctness_only', 'test/checks.c#CHECK_declare', '''
string_append_bounded string_copy_bounded string_duplicate string_duplicate_max
string_search_folded string_split_next string_token string_token_next
''', 'the standard names, compiled twice from two files sharing nothing and '
     'diffed against the host headers; no caller in this tree, so no timing')

cover('correctness_only', 'test/checks.c#CHECK_number', 'string_to_decimal_short',
      'the short-decimal reader is exercised by every strtod case the ULP '
      'lane runs against glibc, 702,066 a machine; its speed was measured on '
      'an awk workload rather than in a dedicated harness, so no isolated '
      'timing row is claimed here')

cover('direct_benchmark', 'test/checks.c#BENCH_allocator', 'memory_take memory_give',
      'malloc/free pair timing against an empty ABI control and a free-list '
      'traffic floor that pops and pushes the same words; the shelf-hit path '
      'is what is timed, and refill/mapping stay in the C it jumps to')

cover('direct_benchmark', 'test/checks.c#BENCH_fields', '''
positive_into_padded positive_into_pair
''', 'paired former-C/assembly timing across padded converter shapes')

cover('direct_benchmark', 'test/checks.c#BENCH_human', '''
positive_into_human_1024_string positive_to_human_1024
''', 'paired former-C/assembly timing for buffer and writer forms')

cover('direct_benchmark', 'test/checks.c#BENCH_human_nearest', '''
positive_into_human_nearest_string
''', 'paired former-C/assembly timing for decimal and binary forms')

cover('direct_benchmark', 'test/checks.c#BENCH_startup', '''
moonwater_cpu_detect program_environment_list program_initial_identity
''', 'isolated runtime-entry components, including the Spark loader identity '
     'handoff and the stock-kernel fallback')

cover('direct_benchmark', 'test/checks.c#BENCH_paths', '''
path_head_copy path_join path_tail_copy
''', 'paired former-C/assembly timing over short, nested, and long paths')

cover('direct_benchmark', 'test/checks.c#BENCH_reverse', '''
memory_reverse
''', 'paired former-C/assembly timing over primitive and folded rev shapes')

cover('direct_benchmark', 'test/checks.c#BENCH_writer_field', '''
string_to_field writer_field
''', 'paired former-C/assembly timing over exact and padded fields')
cover('direct_benchmark', 'test/checks.c#BENCH_writer_text', '''
buffered_flush buffered_reserve buffered_write buffered_write_byte buffered_write_deferred_equal log
''', 'paired former-C/assembly timing over buffered and direct output shapes')
cover('direct_benchmark', 'test/checks.c#BENCH_prefix_known', 'memory_common_prefix',
      'literal-size expansion against the out-of-line hardware routine on '
      'equal spans: native x86-64 plus ARM64/RV64 instruction-set runners')
cover('direct_benchmark', 'test/checks.c#BENCH_compare_max', 'string_compare_max',
      'dynamic-bound first-mismatch semantic floor and equal/late traffic proxies')
cover('direct_benchmark', 'test/checks.c#BENCH_string_copy', 'string_copy',
      'caller-shaped sizes against copy-only traffic and exact scalar semantic proxies')
cover('direct_benchmark', 'test/checks.c#BENCH_hash_33',
      'memory_hash_33 string_hash_33_length',
      'bounded/string verifier and paired scalar/four-byte or one-pass/two-pass timing')
cover('direct_benchmark', 'test/checks.c#BENCH_span_byte', 'memory_span_byte',
      'page-edge verifier and paired scalar/vector equal-run timing')
cover('direct_benchmark', 'test/checks.c#BENCH_fill_u32', 'memory_fill_u32',
      '32-bit span fill against scalar and bulk-store traffic floors')
cover('direct_benchmark', 'test/checks.c#BENCH_fill_u64', 'memory_fill_u64_aligned',
      'aligned 64-bit pattern fill against scalar and bulk-store traffic floors')
cover('direct_benchmark', 'test/checks.c#BENCH_ascii_case', 'memory_compare_ascii_case',
      'exhaustive byte-pair validation and paired folded comparison timing')
cover('direct_benchmark', 'test/checks.c#BENCH_ascii_convert', '''
byte_to_lower byte_to_upper memory_to_lower_ascii memory_to_upper_ascii
''', 'exhaustive byte/page-edge validation and paired scalar-call/inlined-loop timing')
cover('direct_benchmark', 'test/checks.c#BENCH_ascii_search', 'memory_search_ascii_case',
      'bounded exhaustive verifier plus paired former-C/assembly fixed-search timing')
cover('direct_benchmark', 'test/checks.c#BENCH_grep_search', '''
memory_search_prepared memory_search_ascii_case_prepared
''', 'paired repeated-search timing over sparse, folded, false-candidate and dense matches')
cover('direct_benchmark', 'test/checks.c#BENCH_grep_count',
      'memory_count_records_with_prepared',
      'bounded record verifier and traffic/repeated/fused resident-input timing')
cover('direct_benchmark', 'test/checks.c#BENCH_last_of', 'memory_last_of',
      'guarded reverse-search validation and paired scalar/assembly timing')
cover('direct_benchmark', 'test/checks.c#BENCH_words', 'memory_count_words',
      'state/split validation and paired word-transition timing')
cover('direct_benchmark', 'test/checks.c#BENCH_translate', 'memory_translate',
      'paired former-C/assembly timing over byte-table translation sizes')


# Private cores are present in the timed call graph, but the harness cannot
# assign their cost independently from their public wrappers.
cover('benchmark_context', 'test/checks.c#BENCH_ascii_search',
      'memory_first_of_ascii_case',
      'bounded hunt reached by the directly timed folded search',
      {'memory_first_of_ascii_case': 'memory_search_ascii_case'})
cover('benchmark_context', 'test/checks.c#BENCH_grep_search', '''
memory_search_prepared_core memory_search_ascii_case_prepared_core
''', 'private cores reached by the directly timed prepared searches',
      {'memory_search_prepared_core': 'memory_search_prepared',
       'memory_search_ascii_case_prepared_core': 'memory_search_ascii_case_prepared'})
cover('benchmark_context', 'test/checks.c#BENCH_paths', 'path_split_core',
      'private core reached by all three directly timed path wrappers',
      {'path_split_core': 'path_head_copy'})
cover('benchmark_context', 'test/checks.c#BENCH_numbers', '''
positive_digits_core positive_into_core
''', 'private conversion core reached by a directly timed public converter',
      {'positive_digits_core': 'positive_to_string',
       'positive_into_core': 'positive_into'})
cover('benchmark_context', 'test/checks.c#BENCH_writer_field', 'writer_field_core',
      'private core reached by both directly timed field wrappers',
      {'writer_field_core': 'writer_field'})
cover('benchmark_context', 'test/checks.c#BENCH_writer_text', 'buffered_write_core',
      'private core reached by both directly timed buffer-policy wrappers',
      {'buffered_write_core': 'buffered_write'})
cover('benchmark_context', 'test/checks.c#BENCH_span_byte', 'memory_span_byte_wide',
      'the out-of-line SSE and AVX bulk of the directly timed span, which the '
      'same row reaches for every run past sixteen bytes',
      {'memory_span_byte_wide': 'memory_span_byte'})


# These are deliberately a static classification. Loading a syscall number and
# trapping, moving ABI arguments, reading the architectural counter, or doing a
# byte swap has no direct timing row here. Do not turn this category into a
# "hardware floor" assertion without measured evidence.
cover('static_leaf', 'src/platform/linux.inc', '_start exit sleep',
      'startup or direct Linux syscall ABI body; statically reviewed only')
cover('static_leaf', 'src/platform/standard.inc', '''
byte_is_alnum byte_is_alpha byte_is_digit byte_is_hexadecimal
byte_is_lower byte_is_space byte_is_upper absolute_whole absolute_wide
absolute square_root bits_counted bits_first_set bits_first_set_wide
bits_leading_zeros bits_trailing_zeros byte_is_ascii byte_is_blank
byte_is_control byte_is_graphic byte_is_printable byte_is_punctuation
byte_to_ascii decimal_ceiling decimal_difference decimal_floor
decimal_larger decimal_multiply_add decimal_nearest decimal_rounded
decimal_smaller decimal_truncated decimal_with_sign
memory_copy_source_first memory_copy_until memory_zero narrow_absolute
narrow_ceiling narrow_floor narrow_larger narrow_rounded narrow_smaller
narrow_square_root narrow_truncated narrow_with_sign string_append_max
string_compare_folded string_compare_folded_max string_copy_end
string_copy_max_endptr string_first_of_set string_span_of_set
string_span_without_set string_to_number string_to_number_core
string_to_number_unsigned string_to_whole string_to_whole_wide
''', 'branchless range test, register bitmap, sign fold, or the instruction '
     'the hardware already has; each measured against what gcc emits from '
     'the obvious C, on each architecture')

cover('static_leaf', 'src/platform/socket.inc', '''
bytes_reverse_16 bytes_reverse_32 network_load_16 network_load_32
network_store_16 network_store_32 socket_accept socket_bind socket_close
socket_connect socket_listen socket_name socket_new socket_option_get
socket_option_set socket_receive socket_send socket_shutdown
''', 'straight-line byte-order or socket syscall ABI body; statically reviewed only')
cover('static_leaf', 'src/library.c', '''
get_cpu_time system_call system_call_1 system_call_2 system_call_3
system_call_4 system_call_5 system_call_6
memory_sum_bytes memory_checksum_bsd16
''', 'counter read, syscall ABI, or checksum leaf loop; statically reviewed '
     'instruction by instruction on all architecture floors')


# Direct correctness references in the exhaustive assembly verifier. None of
# these references supplies isolated timing evidence.
cover('correctness_only', 'test/checks.c#CHECK_verify', '''
bipolar_into bipolar_into_string bipolar_to_string byte_class_holds
byte_class_index decimal_to_string fast_sin file_close file_get_status
file_load file_new file_read file_valid file_write memory memory_copy_end
memory_copy_apart_end memory_exchange_apart memory_frob memory_free memory_search
path_basename positive_digits
memory_fill_32 memory_fill_64
positive_into_string positive_to_base_field program_argument_list
program_arguments_own
program_arguments_use program_environment
string_append string_bipolar string_copy_max
string_copy_max_end string_cut string_digits string_digits_exact
string_digits_max string_find string_first_of_max string_first_of_or_end
string_format string_report string_diagnostic writer_stderr writer_stderr_once
string_get_environment string_last_of string_length_max
string_replace_all string_search string_set_add string_span
string_span_max string_table_find string_to_bipolar string_to_positive
wait_status_code_base working_directory_get working_directory_set writer_fill
''', 'direct correctness coverage in the assembly verifier')

cover('correctness_only', 'test/checks.c#CHECK_needle', 'memory_search_prepare',
      'prepared-anchor ABI and empty, one-byte, long, exact and folded needles on all architectures')

cover('correctness_only', 'test/checks.c#CHECK_utf8', 'memory_utf8_span',
      'bounded scalar reference, invalid sequences, alignment and guard pages '
      'on all architecture floors; no isolated timing claim')

cover('correctness_only', 'test/checks.c#CHECK_number', '''
string_to_number_checked string_to_number_unsigned_checked
''', 'single-scan overflow status, end pointers, and folded/dynamic parity; '
     'no shipped isolated timing harness')

cover('correctness_only', 'test/checks.c#CHECK_verify', '''
file_new_lazy library_close library_get library_open shell_set_cursor
''', 'focused ABI, error-path and writer-output coverage on all three architectures')

# Indirect correctness anchors and focused subsystem tests. The anchor is named
# separately whenever the private/helper routine itself is not in the test.
cover('correctness_only', 'test/checks.c#CHECK_verify', 'bipolar_into_core file_unload',
      'correctness exercised through a public wrapper, not timed',
      {'bipolar_into_core': 'bipolar_into', 'file_unload': 'file_close'})
cover('correctness_only', 'test/checks.c#CHECK_slurp', 'file_slurp',
      'focused file-slurp correctness and error-path test')
cover('correctness_only', 'test/checks.c#CHECK_socket', 'host_into string_to_host',
      'focused socket conversion correctness test')
cover('correctness_only', 'test/checks.c#CHECK_writer_buffer', '''
log_direct log_failed log_failure_reset log_flush
''', 'focused deferred, flush, direct, sticky and reset failure checks')
cover('correctness_only', 'test/checks.c#CHECK_probe', '''
log_error program_argument program_argument_count term_size
''', 'focused runtime/probe behavior checks; no isolated timing')
cover('correctness_only', 'test/checks.c#CHECK_wait_retry', '''
system_read_retry system_wait4_retry system_write_all wait_status_code
''', 'focused retry/status correctness and signal-interruption checks')
cover('correctness_only', 'test/checks.c#CHECK_stream', 'system_write_all_checked',
      'checked aggregate ABI, partial writes and errno on all three floors; '
      'no shipped isolated timing harness')
cover('correctness_only', 'test/checks.c#CHECK_native_reserve', '''
memory_growth memory_release
''', 'exact lifted ARM64 growth, overflow, failure and release checks')


def validate():
    errors = []
    manifest.reconcile(ROWS, errors)

    cache = {}
    benchmark_dispatch = (ROOT / 'test/run').read_text(encoding='utf-8')
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
        if not manifest.anchor(row, cache, errors):
            continue
        if row.category in ('direct_benchmark', 'benchmark_context'):
            # The evidence is a section of test/checks.c, and the claim that
            # it is a benchmark is only worth anything if something runs it:
            # the section name has to appear in test/run's bench catalogue.
            section = row.evidence.partition('#')[2]
            if not section or section not in benchmark_dispatch:
                errors.append('%s: benchmark %s is not dispatched by test/run bench' %
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
    print('  isolated timing coverage %d/%d; performance-unproven %d/%d' %
          (direct, total, total - direct, total))
    print('  isolated timing is only a measurement candidate; floor evidence '
          'also requires a defensible native lower bound and absolute gap')
    print('  benchmark context only %d/%d (not counted as direct)' %
          (context, total))

    if mode == 'summary':
        gaps = [row.routine for row in ROWS if row.category == 'unmeasured']
        if gaps:
            print('  wholly unmeasured manifest entries: ' +
                  ', '.join(sorted(gaps)))
        else:
            print('  wholly unmeasured manifest entries: none; non-direct '
                  'categories remain performance-unproven')
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


if __name__ == '__main__':
    sys.exit(manifest.run(
        'performance coverage',
        'check all assembly routines for explicit performance evidence',
        'list every routine without a direct benchmark row',
        validate, print_report))
