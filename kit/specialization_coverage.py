#!/usr/bin/env python3
"""Audit known-argument specialization coverage for library.c's inventory.

src/compiler_memory.c replaces calls whose decisive argument the compiler
already knows with the narrower operation that argument permits. Forty-five
routines have that today. This file classifies the complete inventory so that
the question is asked once per routine and not once per reader.

A row says which parameter a call site might hand the compiler as a literal,
and what that literal is worth:

``specialized`` already ships a known-argument path. The row must name the
static inline that expands it, that name must be in src/compiler_memory.c, and
the routine must be exercised by literal in src/test/exact.c -- a specializer
without a written-out-by-literal test is a specializer nothing reaches.

``worth_it`` has a foldable parameter that changes the shape of the work: it
redirects onto a narrower routine that already exists, or it replaces a loop
with a straight line at small literals. A row whose note begins "Measured" has
been timed and carries the numbers; the harness goes in the tree with the
specializer, and the row names it then, which turns the anchor check on. A row
that says "expected" says at what sizes it expects to win, so that a later
measurement can contradict it rather than quietly agree.

``folds_already`` has a foldable parameter and nothing worth removing. Either
the body is no longer than the call that reaches it, or the only foldable
parameter is a value no reachable call site hands over as a literal. Both are
a decision not to build, and both are recorded rather than left implicit.

``nothing_to_fold`` takes no parameter the compiler could know: handles,
pointers into memory the caller owns, and the arguments of a syscall whose
work happens on the far side of the trap.

The check is coupled to the generated assembly inventory, the same way
kit/performance_coverage.py is. A new, removed or renamed routine fails this
until somebody classifies it, and a named parameter that is not in the
routine's declaration fails it too.

    python3 kit/specialization_coverage.py
    python3 kit/specialization_coverage.py --gaps
    python3 kit/specialization_coverage.py --all
"""

import argparse
import importlib.util
import pathlib
import re
import sys
from collections import Counter, namedtuple


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIBRARY = ROOT / 'src/library.c'
COMPILER_MEMORY = ROOT / 'src/compiler_memory.c'
INVENTORY_TOOL = ROOT / 'kit/compact/inventory.py'

#       Where a declaration may live. library.c holds most of them; the
#       platform includes hold the rest, and a parameter named in a row has to
#       be findable in one of these.
DECLARATION_SOURCES = (
    'src/library.c',
    'src/platform/any.inc',
    'src/platform/linux.inc',
    'src/platform/socket.inc',
    'src/platform/standard.inc',
    'src/platform/syscall.inc',
)

Coverage = namedtuple('Coverage',
                      'routine category parameter expansion evidence anchor note')
ROWS = []

CATEGORY_DESCRIPTION = {
    'specialized': 'a known-argument path ships today',
    'worth_it': 'a foldable parameter that changes the shape of the work',
    'folds_already': 'foldable, but the expansion would remove nothing',
    'nothing_to_fold': 'no parameter the compiler could know',
}

#       Only these carry a measured claim, and only these are counted as
#       evidence in the summary.
MEASURED = ('specialized', 'worth_it')


def cover(category, parameter, names, note, expansion=None, evidence=None,
          anchors=None):
    """Add a compact group to the manifest below."""
    anchors = anchors or {}
    for routine in names.split():
        ROWS.append(Coverage(routine, category, parameter,
                             expansion.get(routine) if isinstance(expansion, dict)
                             else expansion,
                             evidence, anchors.get(routine, routine), note))


# ----------------------------------------------------------------------
#       specialized
# ----------------------------------------------------------------------
# The first family. Each is a function-like macro naming itself, so a
# folded size expands and anything else is the ordinary call.
cover('specialized', 'size', 'memory_copy', 'memmove, overlap safe, expanded '
      'to loads before stores up to KNOWN_SIZE_MAX',
      expansion='copy_known', evidence='src/test/exact.c')
cover('specialized', 'size', 'memory_copy_apart', 'memcpy, the halves are '
      'apart, expanded up to KNOWN_SIZE_MAX',
      expansion='copy_apart_known', evidence='src/test/exact.c')
cover('specialized', 'size', 'memory_fill', 'memset, expanded up to '
      'KNOWN_SIZE_MAX with a broadcast when the byte is not folded',
      expansion='fill_known', evidence='src/test/exact.c')


# ----------------------------------------------------------------------
#       worth_it -- measured
# ----------------------------------------------------------------------
# The set routines take the members as a string, so the compiler has the whole
# set. gcc folds the build loop over a literal into constants: the bitmap the
# routine assembles at run time is already there, and for a set of three the
# probe is three compares and no memory at all.
cover('specialized', 'accept', 'string_span_of_set',
      'Measured, and the win is tiered by how many members the literal has. '
      'One to three members expand to a compare chain and win everywhere '
      'timed: 25% of the routine on an empty run, 39% at eight bytes, 69% at '
      'two hundred and fifty six. Four or more members all under sixty four '
      'expand to a register mask with a range guard, which wins only while '
      'the run is short -- 25% empty, 73% at eight, level by sixteen and 136% '
      'at sixty four. The whole 256 bit bitmap, which gcc does fold from a '
      'literal, is NOT a win at any run length measured (87% empty, 103-114% '
      'after) and is not proposed: it does the same probe the routine does '
      'and only saves the build',
      expansion='set_known_table', evidence='src/test/exact_set.c')
cover('specialized', 'accept', 'string_first_of_set',
      'Measured with string_span_of_set: one to three literal members become '
      'a compare chain, and short larger sets become a register mask',
      expansion='first_of_set_known', evidence='src/test/exact_set.c')
cover('specialized', 'reject', 'string_span_without_set',
      'Measured with the pair above and tiered the same way, with the '
      'terminator put into the stopping set so the end of the source stops '
      'the run too', expansion='span_without_byte_known',
      evidence='src/test/exact_set.c')

# Three siblings of the routines that already have a path. Each is a
# trampoline or a wrapper onto one of them, so the expansion is that
# specializer and one argument, and the cutoff is inherited rather than found.
cover('specialized', 'size', 'memory_zero',
      'Measured. bzero is "mov, xor, jmp memory_fill" and the expansion is '
      'fill_known with the byte already chosen: 19-48% of the routine from 1 to 128 '
      'bytes, tracking the fill it reuses',
      expansion='zero_known', evidence='src/test/exact_family.c')
cover('worth_it', 'size', 'memory_copy_end memory_copy_apart_end',
      'Measured. A lea, a push, a call to the copy, a pop and one terminator; '
      'expanded, the call goes and the lea folds into the answer: 16-40% of '
      'the routine from 1 to 128 bytes',
      expansion='copy_end_known')
cover('worth_it', 'size', 'memory_copy_source_first',
      'bcopy is three register moves and a jump into memory_copy; the same '
      'argument as memory_zero and not separately timed',
      expansion='copy_known', evidence=None)

# memcmp's contract here is the magnitude of the byte difference and not its
# sign, which src/test/verify.c checks, so __builtin_memcmp cannot be the
# expansion: a hand written word walk has to keep the subtraction.
cover('specialized', 'size', 'memory_compare',
      'Measured. Word loads, the first differing byte from three masked tests '
      'rather than a count-trailing-zeros riscv has no instruction for, and the '
      'unsigned difference kept. Measured on equal blocks: 18-30% of the '
      'routine to 20 bytes, 60-80% at 32-64, level at 80-96, and 200% at 128 '
      'where the routine reaches its four-block round. So this family needs '
      'its own KNOWN_COMPARE_MAX of 96 and must NOT inherit KNOWN_SIZE_MAX, '
      'which is 128 and would expand into the two-to-one loss',
      expansion='compare_known', evidence='src/test/exact_scan.c')

# ----------------------------------------------------------------------
#       worth_it -- expected, from the shape of the body
# ----------------------------------------------------------------------
cover('specialized', 'size', 'memory_common_prefix',
      'Measured. The bounded word walk returns the first differing byte '
      'directly. Equal-span traffic wins through twenty bytes on native '
      'x86-64 and under the ARM64/RV64 instruction-set runners. Emulated RV64 '
      'is the first reversal: 95% of the routine at twenty and 115% at '
      'twenty-one, so KNOWN_PREFIX_MAX is conservatively twenty rather than '
      'inheriting the wider memcmp cutoff. Mismatch positions are guarded for '
      'correctness but are not claimed as native floor measurements',
      expansion='common_prefix_known',
      evidence='src/test/exact_prefix.c')

cover('specialized', 'size', 'memory_compare_ascii_case',
      'Measured at caller-shaped protocol-token lengths. The straight folded '
      'byte line is 65-92% of the routine through twelve bytes on x86-64 and '
      'ARM64 and 59-83% under the RV64 floor runner. ARM64 turns to 160% at '
      'sixteen and x86-64 loses from twenty four, so the shared cutoff is '
      'twelve, not memory_compare\'s much larger cutoff',
      expansion='compare_ascii_case_known',
      evidence='src/test/exact_ascii_case.c')

cover('specialized', 'needle_size', 'memory_search',
      'Measured. A one byte needle is already redirected inside the routine, '
      'so folding it removes five instructions and a jump: 84-99% of the '
      'routine, a small and uniform win. Two and four byte needles were built '
      'and timed and are NOT a uniform win -- 65% of the routine at a 16 byte '
      'haystack, 158% at 64, 137% at 256, 52% at 4096 -- so only the one byte '
      'redirect is proposed', expansion='search_known',
      evidence='src/test/needle.c')
cover('specialized', 'needle_size', 'memory_search_ascii_case',
      'expected: the same one byte redirect, onto memory_first_of_ascii_case, '
      'from the shape of the body rather than from measurement',
      expansion='search_case_known',
      evidence='src/test/needle.c')
cover('worth_it', 'input', 'string_find',
      'a literal needle gives its length; one byte is string_first_of and the '
      'empty needle is the front of the string, both without the general '
      'hunt', expansion='find_known', evidence=None)
cover('worth_it', 'needle', 'string_search',
      'strstr with a literal needle: the same one byte and empty cases, and '
      'the length no longer measured at run time',
      expansion='find_known', evidence=None)

cover('specialized', 'base', 'positive_into_base',
      'base ten is positive_into and base sixteen is a shift and a nibble '
      'table; the general path divides by a register',
      expansion='into_base_known', evidence='src/test/exact_base.c')
cover('worth_it', 'base', 'string_digits_base_max',
      'base ten is positive_into and base sixteen is a shift and a nibble '
      'table; the general path divides by a register',
      expansion='into_base_known', evidence=None)
cover('specialized', 'base', 'string_to_number string_to_number_unsigned',
      'base ten and sixteen are the two a caller ever writes down, and each '
      'drops the general digit-value path and the base range check',
      expansion='number_known', evidence='src/test/exact_base.c')

cover('specialized', 'bound', 'string_length_max',
      'Measured literal bounds become a straight bounded word/byte scan',
      expansion='length_max_known', evidence='src/test/bounded.c')
cover('specialized', 'bound', 'string_compare_max',
      'Measured literal bounds become a straight bounded word/byte compare',
      expansion='compare_max_known', evidence='src/test/bounded.c')
cover('specialized', 'bound', 'string_append_max',
      'Measured literal bounds reuse the bounded length and copy expansion',
      expansion='append_max_known', evidence='src/test/bounded.c')
cover('specialized', 'bound', 'string_copy_max_end',
      'Measured literal bounds copy the short known pair directly',
      expansion='copy_max_end_known', evidence='src/test/bounded.c')
cover('specialized', 'bound', 'string_copy_max_endptr',
      'Measured literal bounds copy the short known pair directly',
      expansion='copy_max_endptr_known', evidence='src/test/bounded.c')
cover('worth_it', 'bound', '''
string_compare_folded_max string_first_of_max string_span_max
''', 'a literal bound under about thirty two turns a bounded walk into one or '
     'two word loads and a SWAR test; expected to track memory_compare\'s '
     'measured table, since it is the same shape of body',
      expansion='bounded_known', evidence=None)
cover('worth_it', 'length', 'string_copy_max',
      'strncpy pads the whole length with zeros, so a literal length is a '
      'copy and a fill both of known size -- two expansions that already '
      'exist; the padding is what makes this one worth more than the rest of '
      'the bounded family', expansion='copy_max_known', evidence=None)

cover('specialized', 'size', 'memory_first_of memory_last_of',
      'Measured short literal spans skip the width dispatch and tail '
      'arithmetic', expansion={'memory_first_of': 'first_of_known',
                               'memory_last_of': 'last_of_known'},
      evidence='src/test/exact_scan.c')
cover('specialized', 'size', 'memory_copy_until',
      'Measured short literal spans expand the stop scan and bounded copy',
      expansion='copy_until_known', evidence='src/test/exact_family.c')
cover('specialized', 'source', 'string_copy_end',
      'A literal source has a folded length; its bytes and terminator use the '
      'known-size copy and the answer is the terminator address',
      expansion='copy_end_known', evidence='src/test/exact_family.c')
cover('worth_it', 'size', '''
memory_first_of_ascii_case memory_span_byte memory_count memory_count_words
memory_hash_33 memory_translate memory_to_lower_ascii memory_to_upper_ascii
memory_reverse
''', 'a literal size under a block skips the width dispatch and the tail '
     'arithmetic that is most of the work there; expected to win under '
     'thirty two and to be level by a block, unmeasured',
      expansion='block_known', evidence=None)

cover('worth_it', 'count', 'writer_fill',
      'writer_fill makes exactly count calls; a literal count is that many '
      'calls written out, with no counter and no branch',
      expansion='fill_writer_known', evidence=None)
cover('worth_it', 'width', 'positive_into_padded positive_to_padded '
      'writer_field string_to_field positive_to_base_field',
      'width and pad together decide the shape of the field, and a folded '
      'zero pad removes the padding path altogether',
      expansion='field_known', evidence=None)
cover('worth_it', 'stride', 'string_table_find',
      'stride and count are a sizeof and an array length at every call site '
      'in the tree; folding both turns the walk into a fixed sequence of '
      'compares', expansion='table_known', evidence=None)
cover('worth_it', 'which', 'byte_class_holds',
      'the class number is a literal wherever a lexer asks, and each class is '
      'the branchless range test byte_is_* already holds',
      expansion='class_known', evidence=None)


# ----------------------------------------------------------------------
#       folds_already
# ----------------------------------------------------------------------
cover('specialized', 'value', '''
bits_counted bits_first_set bits_first_set_wide bits_leading_zeros
bits_trailing_zeros byte_is_alnum byte_is_alpha byte_is_ascii byte_is_blank
byte_is_control byte_is_digit byte_is_graphic byte_is_hexadecimal byte_is_lower
byte_is_printable byte_is_punctuation byte_is_space byte_is_upper byte_to_ascii
byte_to_lower byte_to_upper
''', 'A literal value expands through the shared KNOWN_SINGLE shape into the '
     'branchless range test or hardware bit instruction',
      expansion='KNOWN_SINGLE', evidence='src/test/single.c')

# Arithmetic leaves. Each body is between one and ten instructions, which is
# what the call sequence that reaches it costs, so expanding one replaces a
# call with the instructions the call was going to run anyway.
cover('folds_already', 'value', '''
absolute absolute_whole absolute_wide bytes_reverse_16 bytes_reverse_32
decimal_ceiling decimal_floor
decimal_nearest decimal_rounded decimal_truncated narrow_absolute
narrow_ceiling narrow_floor narrow_rounded narrow_square_root narrow_truncated
square_root
''', 'a branchless range test, a sign fold or the instruction the hardware '
     'already has: the body is no longer than the call that reaches it')
cover('folds_already', 'first', '''
decimal_difference decimal_larger decimal_smaller decimal_multiply_add
''', 'one or two floating point instructions; same argument as the leaves above')
cover('folds_already', 'value', 'narrow_larger narrow_smaller',
      'a min or a max the hardware does in one instruction')
cover('folds_already', 'magnitude', 'narrow_with_sign decimal_with_sign',
      'a sign copy: one and or one bit insert')

# Converters whose only foldable parameter is the number being converted. No
# call site in the tree hands one a literal -- they all convert something read
# or counted at run time -- so the expansion would be unreachable code.
cover('folds_already', 'value', '''
positive_into positive_into_string positive_into_pair positive_digits
positive_into_human_1024_string positive_to_human_1024
positive_into_human_nearest_string
''', 'the foldable parameter is the number itself, and every call site in the '
     'tree converts a runtime value; the expansion would never be reached')
cover('folds_already', 'value', 'bipolar_into bipolar_into_string',
      'the signed forms of the same argument')
cover('folds_already', 'number', 'positive_to_string bipolar_to_string',
      'the same argument under the name the writer forms give it')
cover('folds_already', 'host', 'host_into',
      'an address the program was handed, spelled into dotted decimal')
cover('folds_already', 'value', 'decimal_to_string',
      'a runtime measurement in every caller; the writer walk is the work')
cover('folds_already', 'input', 'path_basename',
      'a literal path folds the answer to a constant string, and no caller '
      'has one; the routine exists for a path read at run time')
cover('folds_already', 'format', 'string_format',
      'the format is a literal at nearly every call site and this is the '
      'largest thing here that cannot be built: expanding it means turning '
      'one variadic call into a sequence of per-conversion calls, and C has '
      'no construct that walks a literal and emits calls against __VA_ARGS__')
cover('folds_already', 'x', 'fast_sin',
      'a polynomial with no branch in it; folding the angle folds the answer, '
      'and no caller has a literal angle')
cover('folds_already', 'raw', 'wait_status_code wait_status_code_base',
      'a shift, a mask and a select on a status the kernel wrote; the base is '
      'foldable and removes one compare')
cover('folds_already', 'at', '''
network_load_16 network_load_32 network_store_16 network_store_32
''', 'a load or a store and a byte reversal; the value is foldable and the '
     'body is two instructions')
cover('folds_already', 'input', '''
string_to_positive string_to_bipolar string_to_whole string_to_whole_wide
string_to_host
''', 'a literal string would fold the whole answer, and no caller parses a '
     'literal; the routines exist for text that arrived at run time')
cover('folds_already', 'source', '''
string_length string_compare string_copy string_append
string_first_of string_first_of_or_end string_last_of string_last_of_or_end
string_compare_folded string_digits
string_digits_exact string_bipolar string_span string_lex_word
''', 'the string is foldable only as a literal, and a literal argument to an '
     'unbounded walk folds the answer rather than shortening the walk; the '
     'bounded forms above are where the bound is the useful literal')
cover('folds_already', 'string', 'string_cut string_replace_all',
      'the byte cut at or replaced is foldable and the walk is not; a literal '
      'string would be written into by these, which no caller does')
cover('folds_already', 'set', 'string_set_add',
      'a literal member string folds this to a fixed bitmap, but the routine '
      'exists so that a set can be built once and reused; a caller with a '
      'literal set wants string_span_of_set above instead')
cover('folds_already', 'name', 'byte_class_index string_get_environment',
      'a literal name folds the length; the body is a length and a small '
      'compare chain either way')
cover('folds_already', 'capacity', 'path_join path_tail_copy path_head_copy',
      'the capacity is a literal at every call site and bounds the writes, '
      'but the work is the scan for the last separator, which the capacity '
      'says nothing about')
cover('folds_already', 'x', 'shell_set_cursor',
      'two numbers into an escape sequence; folding them folds the sequence, '
      'and no caller has a literal cursor position')
cover('folds_already', 'size', 'memory memory_free',
      'the size is a literal often enough, and the work is an mmap or an '
      'munmap: nothing on this side of the trap gets shorter')
cover('folds_already', 'count', 'memory_fill_u32 memory_fill_u64_aligned',
      'the count is a runtime row or block length at every caller; when it is '
      'small the assembly routine already selects its scalar tail, and when '
      'it is large expanding stores would only duplicate its vector loop')
cover('folds_already', 'want', 'memory_growth memory_reserve',
      'growth policy arithmetic, a handful of instructions with an overflow '
      'check that a literal does not remove')
cover('folds_already', 'unit', 'memory_release',
      'a free and three stores; the unit is foldable and removes nothing')
cover('folds_already', 'length', '''
buffered_write buffered_write_deferred_equal log log_direct log_error
system_write_all
''', 'a literal length is common and the work is the copy into the buffer, '
     'which memory_copy_apart already expands from inside these')
cover('folds_already', 'length', 'buffered_reserve',
      'callers commonly know the requested span, but the public hot path is '
      'already only two capacity checks, one count update and the returned '
      'address; the direct floor harness measures that residual')
cover('folds_already', 'capacity', 'buffered_write_byte',
      'a compare against the capacity and a byte store; the capacity is a '
      'literal and the body is already that short')
cover('folds_already', 'code', 'exit',
      'a syscall number and a trap, with the code in a register either way')
cover('folds_already', 'flags', 'file_new file_new_lazy',
      'the flags are a literal at every call site and are moved into a '
      'register for the open either way')
cover('folds_already', 'size', 'file_read file_write',
      'the size is a literal and the work is the syscall')
cover('folds_already', 'capacity', 'file_slurp',
      'a literal capacity bounds the read; the work is the open, the read and '
      'the close')
cover('folds_already', 'index', 'program_argument program_environment',
      'a bounds check and an indexed load; a literal index removes the '
      'multiply and nothing else')
cover('folds_already', 'count', 'program_arguments_use',
      'a count and a pointer stored into two globals')
cover('folds_already', 'syscall', '''
system_call system_call_1 system_call_2 system_call_3 system_call_4
system_call_5 system_call_6
''', 'the syscall number is always a literal and is loaded into one register; '
     'that is the whole body besides the trap')
cover('folds_already', 'handle', 'system_read_retry',
      'a retry loop around a trap; the handle is foldable and the loop is not')
cover('folds_already', 'options', 'system_wait4_retry',
      'the options are a literal at every call site and are moved into a '
      'register for the trap either way')

cover('folds_already', 'bound', '''
string_digits_max string_digits_octal_max string_digits_octal_escape_max
string_digits_hexadecimal_max string_digits_hexadecimal_escape_max
''', 'the bound is a literal, but each of these is already the base-folded '
     'form of string_digits_base_max: the specialization this family has is '
     'the one that shipped as five separate routines')
cover('folds_already', 'needle_size', '''
memory_search_prepared memory_search_ascii_case_prepared
memory_count_records_with_prepared
''', 'prepared searches are reached with compiled run-time patterns and already '
     'redirect empty and one-byte needles inside their assembly entry')
cover('folds_already', 'ascii_case', 'memory_search_prepare',
      'the case choice is made once while preparing a reusable needle; splitting '
      'the cold pass would not shorten any repeated search')


# ----------------------------------------------------------------------
#       nothing_to_fold
# ----------------------------------------------------------------------
cover('nothing_to_fold', None, '''
_start moonwater_cpu_detect program_initial_identity get_cpu_time term_size working_directory_get
working_directory_set program_argument_count program_arguments_own
program_environment_list log_failed log_failure_reset log_flush sleep buffered_flush
string_hash_33_length
''', 'no argument, or an argument that is a pointer into memory the caller '
     'owns; nothing the compiler could know shortens the body')
cover('nothing_to_fold', None, '''
file_close file_get_status file_load file_unload file_valid
library_close library_get library_open
''', 'a file or library handle the caller opened at run time')
cover('nothing_to_fold', None, '''
socket_accept socket_bind socket_close socket_connect socket_listen
socket_name socket_new socket_option_get socket_option_set socket_receive
socket_send socket_shutdown
''', 'a descriptor and an address structure; the literals among the flags are '
     'moved into a register for the trap either way')
cover('nothing_to_fold', None, '''
bipolar_into_core buffered_write_core path_split_core positive_digits_core
positive_into_core string_to_number_core writer_field_core
memory_search_prepared_core memory_search_ascii_case_prepared_core
''', 'a private core with no declaration, so C cannot name it and no call '
     'site can hand it a literal; its wrappers carry the classification')


def load_inventory():
    spec = importlib.util.spec_from_file_location(
        'moonwater_inventory_for_specialization_coverage', INVENTORY_TOOL)
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


def load_declarations():
    """Every routine's C declaration, so a named parameter can be checked.

    A declaration here is a statement ending in ');' that is not inside an
    __asm__ string and not a macro line. Routines with no declaration -- the
    private cores and _start -- simply do not appear, which is what makes
    naming a parameter for one of them a failure.
    """
    found = {}
    for relative in DECLARATION_SOURCES:
        path = ROOT / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        for match in re.finditer(r'(?<![A-Za-z0-9_])([a-z_][a-z_0-9]*)\s*\(', text):
            name = match.group(1)
            if name in found:
                continue
            begin = text.rfind('\n', 0, match.start()) + 1
            close = text.find(';', match.start())
            if close == -1:
                continue
            statement = text[begin:close + 1]
            if '"' in statement or '#' in statement or 'ASM_' in statement:
                continue
            if statement.lstrip().startswith('//'):
                continue
            if not statement.rstrip().endswith(');'):
                continue
            found[name] = ' '.join(statement.split())
    return found


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

    declarations = load_declarations()
    specializers = COMPILER_MEMORY.read_text(encoding='utf-8', errors='replace')
    cache = {}

    # A public routine spelled as a function-like macro in the compiler
    # umbrella is a shipped call-site specializer. Keep this structural fact
    # tied to the manifest: the old check only proved that a row's helper
    # existed, so thirty five live macros remained labelled as future work
    # without failing the audit.
    macro_specialized = set(re.findall(
        r'^#define\s+([a-z_][a-z_0-9]*)\s*\(', specializers, re.MULTILINE)) & inventory
    manifested_specialized = {
        row.routine for row in ROWS if row.category == 'specialized'
    }
    if macro_specialized != manifested_specialized:
        if macro_specialized - manifested_specialized:
            errors.append('live specializers not labelled specialized: ' +
                          ', '.join(sorted(macro_specialized - manifested_specialized)))
        if manifested_specialized - macro_specialized:
            errors.append('specialized rows without public macros: ' +
                          ', '.join(sorted(manifested_specialized - macro_specialized)))

    for row in ROWS:
        if row.category not in CATEGORY_DESCRIPTION:
            errors.append('%s: unknown category %s' % (row.routine, row.category))
            continue

        if row.category == 'nothing_to_fold':
            if row.parameter is not None:
                errors.append('%s: nothing_to_fold row names a parameter' %
                              row.routine)
            if row.evidence is not None:
                errors.append('%s: nothing_to_fold row carries evidence' %
                              row.routine)
            continue

        #      Everything else claims a foldable parameter, and the claim is
        #      checked against the routine's own declaration rather than left
        #      as prose nobody reads.
        if not row.parameter:
            errors.append('%s: %s row names no foldable parameter' %
                          (row.routine, row.category))
        elif row.routine not in declarations:
            errors.append('%s: %s row names parameter %s but the routine has '
                          'no C declaration' %
                          (row.routine, row.category, row.parameter))
        elif not token_present(declarations[row.routine], row.parameter):
            errors.append('%s: parameter %s is not in its declaration -- %s' %
                          (row.routine, row.parameter, declarations[row.routine]))

        if row.category == 'folds_already':
            if row.expansion is not None:
                errors.append('%s: folds_already row names an expansion' %
                              row.routine)
            if row.evidence is not None:
                errors.append('%s: folds_already row carries evidence' %
                              row.routine)
            continue

        #      specialized and worth_it both name the expansion they mean.
        if not row.expansion:
            errors.append('%s: %s row names no expansion' %
                          (row.routine, row.category))
        elif row.category == 'specialized' and \
                not token_present(specializers, row.expansion):
            errors.append('%s: specializer %s is absent from '
                          'src/compiler_memory.c' % (row.routine, row.expansion))

        if row.category == 'specialized' and not row.evidence:
            errors.append('%s: a shipped specializer needs correctness '
                          'evidence' % row.routine)

        if row.evidence is None:
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
    return errors


def print_report(mode):
    counts = Counter(row.category for row in ROWS)
    total = len(ROWS)
    timed = sum(1 for row in ROWS
                if row.category in MEASURED and row.evidence is not None)

    print('specialization coverage: %d routines classified' % total)
    for category in CATEGORY_DESCRIPTION:
        print('  %-18s %3d  %s' %
              (category, counts[category], CATEGORY_DESCRIPTION[category]))
    print('  candidates whose harness is in the tree %d/%d' %
          (timed, counts['specialized'] + counts['worth_it']))

    if mode == 'summary':
        pending = sorted(row.routine for row in ROWS
                         if row.category == 'worth_it'
                         and not row.note.startswith('Measured'))
        print('  worth_it and not yet timed: ' + ', '.join(pending))
        return

    if mode == 'gaps':
        selected = [row for row in ROWS if row.category == 'worth_it']
    else:
        selected = list(ROWS)
    for row in sorted(selected, key=lambda item: item.routine):
        parameter = row.parameter or '-'
        evidence = row.evidence or 'not timed'
        print('  %-34s %-16s %-12s %-28s %s' %
              (row.routine, row.category, parameter, evidence, row.note))


def main():
    parser = argparse.ArgumentParser(
        description='check every assembly routine for a specialization decision')
    output = parser.add_mutually_exclusive_group()
    output.add_argument('--gaps', action='store_true',
                        help='list every worth_it row')
    output.add_argument('--all', action='store_true',
                        help='list every manifest row')
    arguments = parser.parse_args()

    try:
        errors = validate()
    except Exception as error:  # keep CI diagnostics short and actionable
        sys.stderr.write('specialization coverage: %s\n' % error)
        return 1
    if errors:
        for error in errors:
            sys.stderr.write('specialization coverage: %s\n' % error)
        return 1

    mode = 'all' if arguments.all else ('gaps' if arguments.gaps else 'summary')
    print_report(mode)
    return 0


if __name__ == '__main__':
    sys.exit(main())
