#!/usr/bin/env python3
# Run from any directory: python3 test/audit_text_cut.py [output-directory].
# With no output directory, generated sources, binaries and reports are temporary.
# Uses current production source and hosted adapters; no full applet is executed.
"""Fixed-production cut endpoint regressions with exact checked parser and consumers."""
import ast
import hashlib
import json
import pathlib
import platform
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
import sys, tempfile
_owned_output = None if len(sys.argv) > 1 else tempfile.TemporaryDirectory(prefix='audit-text-cut-')
OUT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(_owned_output.name)
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "src/sh/text.c").read_text()
library = (ROOT / "src/library.common.c").read_text()
sha = lambda data: hashlib.sha256(data).hexdigest()
spans = []

def capture(text, begin, end, path, label, start=0):
    a = text.index(begin, start)
    b = text.index(end, a)
    block = text[a:b]
    spans.append(dict(file=path, label=label, start=text[:a].count("\n") + 1,
                      end=text[:b].count("\n") + 1, sha256=sha(block.encode()),
                      exact_source_bytes=True))
    return block

parser = capture(source, "#define TEXT_LIST_MAX", "/*\n        The long spellings cut", "src/sh/text.c", "list state, parser and membership")
inside = capture(source, "static b8 text_set_inside", "/*\n        Coreutils' numeric", "src/sh/text.c", "whitespace membership set")
consumer = capture(source, "                        if (by_character)\n", "\n                }\n\n                text_close();", "src/sh/text.c", "complete byte and field consumer blocks", source.index("static b32 text_cut()"))
digits = capture(library, "static inline INLINE positive digit_known", "/* GNU ld repairs", "src/library.common.c", "checked digit parser")
preamble = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef unsigned long long positive;
typedef unsigned char p8;
typedef signed char b8;
typedef uint32_t p32;
typedef char *string_address;
#define INLINE
#define string_get(p) (*(p))
#define address_to *
#define address_of &
#define null NULL
#define min(a,b) ((a)<(b)?(a):(b))
#define TEXT_LINE_MAX (1 << 20)
#define TEXT_OUT_MAX (1 << 21)
#define TEXT_UNSET ((positive)-1)
#define STRING_SET_BYTES 256
_Static_assert(sizeof(positive) == 8, "source's 64-bit positive width");
static bool byte_is_digit(p8 c) { return c>='0' && c<='9'; }
static void memory_fill(void *p, int v, positive n) { memset(p, v, n); }
static void memory_copy(void *a, const void *b, positive n) { memcpy(a, b, n); }
static void *memory_first_of(const void *a, b8 b, positive n) { return memchr(a, (unsigned char)b, n); }
static positive string_span_max(const void *v, positive n, const b8 *set) {
    const p8 *p=v; positive i=0; while (i<n && set[p[i]]) i++; return i;
}
static b8 string_set_blanks[256] = { [' ']=1, ['\t']=1 };
static p8 output[TEXT_OUT_MAX];
static positive text_out_used;
static p8 text_delimiter='\n';
static p8 *text_reserve(positive n) {
    assert(n <= TEXT_OUT_MAX-text_out_used);
    p8 *result=output+text_out_used; text_out_used+=n; return result;
}
static void text_put(const void *p, positive n) { memcpy(text_reserve(n),p,n); }
static void text_put_character(p8 c) { *text_reserve(1)=c; }
'''
driver_begin = r'''
int main(int argc, char **argv) {
    if (argc != 5) return 64;
    bool by_character = argv[2][0]=='b' || argv[2][0]=='c';
    bool by_field = !by_character;
    bool whitespace = argv[2][0]=='w';
    bool complement = argv[3][0]=='1';
    bool only_delimited = false;
    string_address separator = argv[4][0]=='1' ? "|" : NULL;
    positive separator_length = separator ? 1 : 0;
    if (whitespace && !separator) { separator="\t"; separator_length=1; }
    p8 delimiter = ':';
    p8 *line = (p8 *)(by_character ? "abcdefghi" : whitespace ? "a b c d e f g h i" : "a:b:c:d:e:f:g:h:i");
    positive line_length = strlen((char *)line);
    bool parsed = text_list_parse(argv[1]);
    if (parsed) {
        for (int record=0; record<1; record++) {
'''
driver_end = r'''
        }
    }
    printf("{\"parsed\":%s,\"first\":\"%llu\",\"last\":\"%llu\",\"open\":\"%llu\",\"single\":%s,\"members\":\"",
           parsed?"true":"false",text_list_single_first,text_list_single_last,text_list_open,text_list_single?"true":"false");
    for (positive n=1;n<=9;n++) putchar(text_list_has(n)?'1':'0');
    printf("\",\"stdout_hex\":\"");
    for (positive n=0;n<text_out_used;n++) printf("%02x",output[n]);
    puts("\"}");
    return 0;
}
'''
c_path = OUT / "C06-source-fixture.c"
c_path.write_text(preamble + digits + inside + parser + driver_begin + consumer + driver_end)
binary = OUT / "C06-source-fixture"
build_args = ["clang", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", str(c_path), "-o", str(binary)]
build = subprocess.run(build_args, capture_output=True, text=True)
(OUT / "C06-source-fixture-build.log").write_text(build.stdout + build.stderr)
assert build.returncode == 0, build.stderr

U = 1 << 64
specs = ["1", "2-4", "3-", "-3", "1,3-", str(U-1), str(U-1)+"-", "1-"+str(U-1),
         str(U), str(U+1), str(U+1)+"-2", str(U+2)+"-", "-"+str(U+2), "1-"+str(U+2),
         "2-"+str(U+1), "1-"+str(U), str(2*U+1), "000"+str(U+1),
         "999999999999999999999999999999999999999999999999999999999999999999",
         "0", "0-", "-0", "2-1", "", "1,", "1x", "+1", "-", "1,"+str(U+2)+"-", "0000000000000000000000000000001", "000"+str(U-1), "1,"+str(U), "1-"+str(U)+",3", "999999", "2,4-6", "4-6,2", "1-3,5-", "-3,5-7"]
results = []
for spec in specs:
    for mode, complement, separator in [("b",False,False),("c",False,False),("b",True,False),("b",True,True),("f",False,False),("f",False,True),("f",True,False),("w",False,False)]:
        args = [str(binary),spec,mode,str(int(complement)),str(int(separator))]
        got = subprocess.run(args, capture_output=True, text=True)
        assert got.returncode == 0, (args,got.returncode,got.stderr)
        value = json.loads(got.stdout)
        value.update(spec=spec,mode=mode,complement=complement,custom_separator=separator,
                     fixture_exit_status=got.returncode,stderr=got.stderr,
                     stdout=bytes.fromhex(value["stdout_hex"]).decode())
        results.append(value)

def row(spec, mode="b", complement=False, separator=False):
    return next(r for r in results if (r["spec"],r["mode"],r["complement"],r["custom_separator"]) == (spec,mode,complement,separator))

checks = []
def check(label, condition):
    checks.append(dict(label=label,passed=bool(condition)))
    assert condition, label

def oracle(spec, mode, complement, separator):
    import re
    positions = set()
    if not spec: return None
    for part in spec.split(','):
        if not re.fullmatch(r'(?:[0-9]+|[0-9]+-[0-9]*|-[0-9]+)', part): return None
        if '-' in part:
            first, last = part.split('-')
            first = int(first) if first else 1
            last = int(last) if last else U-1
        else:
            first = last = int(part)
        if not 0 < first < U or not 0 < last < U or last < first: return None
        positions.update(n for n in range(1,10) if first<=n<=last)
    selected = [n for n in range(1,10) if (n in positions)!=complement]
    if mode in ('f','w'):
        glue = '|' if separator else '\t' if mode=='w' else ':'
        return glue.join(chr(96+n) for n in selected)+'\n'
    if not separator:
        return ''.join(chr(96+n) for n in selected)+'\n'
    # Custom byte output separators divide distinct selected runs.  These
    # cases use disjoint input ranges; overlapping delimiter policy is separate.
    output=''
    prior=0
    for n in selected:
        if output and n!=prior+1: output+='|'
        output+=chr(96+n); prior=n
    return output+'\n'

for r in results:
    want=oracle(r['spec'],r['mode'],r['complement'],r['custom_separator'])
    check('acceptance '+repr((r['spec'],r['mode'],r['complement'],r['custom_separator'])), r['parsed']==(want is not None))
    check('output '+repr((r['spec'],r['mode'],r['complement'],r['custom_separator'])), r['stdout']==('' if want is None else want))
check('all execution stderr empty',all(not r['stderr'] for r in results))

record = dict(kind="fresh exact current-source hosted fixture; not full shell execution",
              host=platform.platform(),positive_bits=64,extracted_spans=spans,
              checked_parser="Exact current digit_known and string_digits_checked helpers; no wrapping assembly digit parser.",
              harness_adaptations="64-bit positive typedef, libc memory helpers, in-memory output/input and options supplied directly; one fresh process per case; exact byte/field consumer blocks run for one record.",
              build_command=build_args,build_status=build.returncode,
              files={str(p):sha(p.read_bytes()) for p in [pathlib.Path(__file__),c_path,binary]},
              cases_run=len(results),checks=checks,results=results)
(OUT / "C06-source-fixture-results.json").write_text(json.dumps(record,indent=2)+"\n")
print(json.dumps(dict(cases_run=len(results),checks_passed=len(checks))))
