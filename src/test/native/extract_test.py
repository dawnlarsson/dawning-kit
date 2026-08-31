#!/usr/bin/env python3
"""Contract and corruption checks for the native assembly-object lifter."""
import pathlib
import re
import subprocess
import sys
import tempfile

extract = pathlib.Path(sys.argv[1])
library = pathlib.Path(sys.argv[2])
source = library.read_text()

begin = 'NATIVE_BYTE_COMMONNESS_BEGIN'
end = 'NATIVE_BYTE_COMMONNESS_END'
start_macro = 'ASM_RODATA_OBJECT_BEGIN(byte_commonness, 16)'
stop_macro = 'ASM_OBJECT_END(byte_commonness)'

for required in (begin, end, start_macro, stop_macro):
    if source.count(required) != 1:
        sys.exit('native extract test: expected one %s' % required)

def extract_from(text, directory, case):
    candidate = pathlib.Path(directory) / (case + '.c')
    candidate.write_text(text)
    return subprocess.run(
        [sys.executable, str(extract), str(candidate), 'memory_search',
         'memory_search_prepare', 'memory_search_prepared_core'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )

def mutation_block(text):
    first = text.index(begin)
    last = text.index(end, first)
    return first, last, text[first:last]

def replace_block(text, block):
    first, last, _ = mutation_block(text)
    return text[:first] + block + text[last:]

def reject(text, directory, case, diagnostic):
    result = extract_from(text, directory, case)
    if result.returncode == 0:
        sys.exit('native extract test: %s mutation was accepted' % case)
    if diagnostic not in result.stderr:
        sys.exit('native extract test: %s lacked %r diagnostic: %s'
                 % (case, diagnostic, result.stderr.strip()))

with tempfile.TemporaryDirectory(prefix='native-extract.') as directory:
    result = extract_from(source, directory, 'good')
    if result.returncode:
        sys.exit('native extract test: good object rejected: %s'
                 % result.stderr.strip())
    lifted = result.stdout
    for required in (
        '.section __TEXT,__const',
        '.globl _byte_commonness',
        '_byte_commonness:',
        'adrp x3, _byte_commonness@PAGE',
        'add x3, x3, _byte_commonness@PAGEOFF',
        'extern const unsigned char byte_commonness[256];',
    ):
        if required not in lifted:
            sys.exit('native extract test: converted output lacks %s' % required)
    for forbidden in ('.type byte_commonness', '.size byte_commonness',
                      '.pushsection', '.popsection', '%object', '%progbits'):
        if forbidden in lifted:
            sys.exit('native extract test: converted output retained %s' % forbidden)

    reject(source.replace(begin, 'NATIVE_BYTE_COMMONNESS_GONE', 1), directory,
           'missing-marker', 'need one ordered')
    reject(source.replace(begin, begin + '\n// ' + begin, 1), directory,
           'duplicate-marker', 'need one ordered')

    first, last, block = mutation_block(source)
    reject(replace_block(source, block.replace(
               stop_macro, 'ASM_OBJECT_END(not_byte_commonness)', 1)),
           directory, 'mismatched-wrapper', 'object begin/end macro pair')
    reject(replace_block(source, block.replace(
               start_macro, 'ASM_RODATA_OBJECT_BEGIN(byte_commonness, 8)', 1)),
           directory, 'bad-alignment', 'alignment 8, expected 16')

    short_block, changed = re.subn(r'(\.byte\s+)\d+\s*,\s*', r'\1', block,
                                   count=1)
    if changed != 1:
        sys.exit('native extract test: could not shorten byte payload')
    reject(replace_block(source, short_block), directory,
           'bad-size', 'has 255 bytes, expected 256')

    swapped_block, changed = re.subn(
        r'(\.byte\s+)(\d+)(\s*,\s*)(\d+)', r'\1\4\3\2', block, count=1)
    if changed != 1:
        sys.exit('native extract test: could not reorder byte payload')
    reject(replace_block(source, swapped_block), directory,
           'bad-checksum', 'checksum')

print('arm64 native extractor: object contract and corruption checks passed')
