#!/usr/bin/env python3
# Run from any directory: python3 test/audit_text_sort.py [output-directory].
# With no output directory, generated sources, binaries and reports are temporary.
# Uses current production source and hosted adapters; no full applet is executed.
"""C05 fixed-production regression and unguarded negative control."""
import hashlib, json, pathlib, subprocess, platform, sys, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
_owned_output = None if len(sys.argv) > 1 else tempfile.TemporaryDirectory(prefix='audit-text-sort-')
OUT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(_owned_output.name)
OUT.mkdir(parents=True, exist_ok=True)
src = (ROOT / 'src/sh/text.c').read_text()
common = (ROOT / 'src/library.common.c').read_text()
def h(data): return hashlib.sha256(data).hexdigest()
def span(text, start, end):
    a = text.index(start); b = text.index(end, a)
    return text[a:b], text[:a].count('\n') + 1, text[:b].count('\n')
body, first, last = span(src, '#define SORT_KEYS_MAX 8', 'static positive sort_key_flags')
merge, mf, ml = span(common, '#define array_merge_sort', '/* Linux raw errors')
plan, pf, pl = span(src, '        // Every comparison uses this effective key plan,', '        if (null_data)')
dispatch, df, dl = span(src, '                if (byte_order)', '        // -o is opened')
# Strip only the containing text_sort else block's closing brace.
dispatch = dispatch[:dispatch.rfind('        }')]
pre = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t positive;
typedef intptr_t bipolar;
typedef int32_t b32;
typedef uint8_t p8;
typedef uint8_t b8;
typedef p8 *string_address;
#define address_to *
#define address_of &
#define fn void
#define INLINE
#define PURE
#define HOT
#define null NULL
#define min(a,b) ((a)<(b)?(a):(b))
#define memory_fill memset
#define memory_copy_apart memcpy
#define memory_compare memcmp
#define memory_first_of memchr
#define STRING_SET_BYTES 256
static const b8 string_set_blanks[256] = {['\t']=1, [' ']=1};
static const b8 string_set_digits[256] = {['0']=1,['1']=1,['2']=1,['3']=1,['4']=1,['5']=1,['6']=1,['7']=1,['8']=1,['9']=1};
static positive string_span_max(const p8 *s, positive n, const b8 *set) { positive i=0; while(i<n && set[s[i]]) ++i; return i; }
static positive memory_span_byte(const p8 *s, p8 b, positive n) { positive i=0; while(i<n && s[i]==b) ++i; return i; }
static bool byte_is_blank(p8 c) { return c==' ' || c=='\t'; }
static bool byte_is_digit(p8 c) { return c>='0' && c<='9'; }
static bool byte_is_alnum(p8 c) { return byte_is_digit(c) || (c>='A' && c<='Z') || (c>='a' && c<='z'); }
static bool text_word(p8 c) { return byte_is_alnum(c) || c=='_'; }
static p8 upper(p8 c) { return c>='a' && c<='z' ? c-32 : c; }
static b32 memory_compare_ascii_case(const p8 *a, const p8 *b, positive n) { for(positive i=0;i<n;i++) if(upper(a[i])!=upper(b[i])) return (int)upper(a[i])-(int)upper(b[i]); return 0; }
static const b8 *text_inside(void) { static b8 s[256]; for(int i=0;i<256;i++) s[i]=!byte_is_blank(i); return s; }
static const char *file_month_names[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
typedef struct { p8 *at; positive length; bool ended; } text_slice;
static text_slice *text_lines;
static positive text_lines_count;
'''
post = r'''
int main(int argc, char **argv) {
    p8 delimiter='\n';
    sort_ordering defaults={0};
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i], "b")) defaults.blanks[0]=defaults.blanks[1]=true;
        else if(!strcmp(argv[i], "s")) sort_stable=true;
        else if(!strcmp(argv[i], "u")) sort_unique=true;
        else if(!strcmp(argv[i], "r")) sort_reverse=defaults.reverse=true;
        else if(!strcmp(argv[i], "f")) defaults.how=SORT_FOLD;
        else if(!strcmp(argv[i], "z")) delimiter=0;
        else if(!strcmp(argv[i], "k1")) { sort_key_count=1; sort_keys[0]=(sort_key){.first_field=1}; }
        else if(!strcmp(argv[i], "k1b")) { sort_key_count=1; sort_keys[0]=(sort_key){.first_field=1,.order.blanks={true,false},.ordered=true}; }
        else return 3;
    }
    p8 *input=malloc(1048576); if(!input) return 4;
    positive size=fread(input,1,1048576,stdin); if(size==1048576) return 5;
    text_lines=calloc(size+1,sizeof(*text_lines));
    positive start=0;
    for(positive i=0;i<size;i++) if(input[i]==delimiter) {
        text_lines[text_lines_count++]=(text_slice){input+start,i-start,true}; start=i+1;
    }
    if(start<size) text_lines[text_lines_count++]=(text_slice){input+start,size-start,false};
    sort_order=calloc(text_lines_count+1,sizeof(*sort_order));
    sort_spare=calloc(text_lines_count+1,sizeof(*sort_spare));
    for(positive i=0;i<text_lines_count;i++) sort_order[i]=i;
    c05_dispatch(defaults);
    positive disorder=0;
    for(positive i=1;i<text_lines_count;i++) if(sort_compare(sort_order[i-1],sort_order[i])>0) disorder++;
    fprintf(stderr,"{\"adjacent_comparator_inversions\":%lu}\n",(unsigned long)disorder);
    for(positive i=0;i<text_lines_count;i++) {
        if(sort_unique && i && !sort_compare_keys(sort_order[i-1],sort_order[i])) continue;
        text_slice *line=text_lines+sort_order[i];
        fwrite(line->at,1,line->length,stdout); putchar(delimiter);
    }
    return 0;
}
'''
checks=[]
for label, fix in [('fixed',False),('unguarded_control',True)]:
    # Break accelerator eligibility alone; the comparator keeps the true plan.
    chosen=plan.replace('bool byte_order = first->whole',
                        'bool byte_order = (first->first_field == 1 && first->first_char <= 1 && !first->second_field)') if fix else plan
    code=pre+'\n'+merge+'\n'+body+'\nstatic void c05_dispatch(sort_ordering defaults) {\n'+chosen+dispatch+'}\n'+post
    path=OUT/f'C05-sort-blanks-{label}.c'; path.write_text(code)
    exe=OUT/f'C05-sort-blanks-{label}'
    build=subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-Wno-pointer-sign',str(path),'-o',str(exe)],capture_output=True,text=True)
    (OUT/f'C05-sort-blanks-{label}-build.log').write_text(build.stdout+build.stderr)
    if build.returncode: raise RuntimeError(build.stderr)
    checks.append({'name':label,'source':str(path),'source_sha256':h(path.read_bytes()),'binary_sha256':h(exe.read_bytes()),'build_status':build.returncode})
cases=[]
def add(name,records,flags=('b',),delimiter=b'\n'):
    cases.append({'name':name,'records':records,'flags':list(flags),'delimiter':delimiter})
for n in [0,1,2,8,15,16,17,31,32,33,64]:
    add(f'space_threshold_{n}',[(b' ' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(n))])
for n in [15,16,17,32]:
    add(f'tab_threshold_{n}',[(b'\t' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(n))])
    add(f'common_blank_prefix_{n}',[b' \t'+(b' ' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(n))])
for n in [15,16,17,32]:
    add(f'blank_empty_equal_keys_{n}',([b'\t',b' ',b'',b' a',b'a',b'\ta',b'  a',b' b']*4)[:n])
records=[(b' \t' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(32))]
for flags in [(),('b','s'),('b','u'),('b','r'),('b','f'),('k1b',)]: add('mode_'+'_'.join(flags or ('raw',)),records,flags)
for n in [15,16,17]:
    for flags in [('s',),('u',),('k1',),('k1','s'),('k1','u')]:
        add('whole_key_'+'_'.join(flags)+f'_{n}', (records[:8]*3)[:n], flags)
add('zero_delimited_16',records[:16],('b','z'),b'\0')
add('embedded_nul_16',[(b' ' if i%2 else b'')+b'a\0'+f'{i:02}'.encode() for i in reversed(range(16))])
add('long_common_prefix_16',[b' '*2048+(b'\t' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(16))])
add('internal_blank_not_leading_16',[b'x'+(b' ' if i%2 else b'')+f'{i:02}'.encode() for i in reversed(range(16))])
for flags in [('b','s'),('b','u')]:
    add('equal_normalized_'+'_'.join(flags),[b'a',b' a',b'\ta',b'b',b' b',b'']*5,flags)
cases.append({'name':'unterminated_17','records':records[:17],'flags':['b'],'delimiter':b'\n','unterminated':True})
def oracle(records,flags):
    skip='b' in flags or 'k1b' in flags
    def key(r):
        k=r.lstrip(b' \t') if skip else r
        return k.upper() if 'f' in flags else k
    tie='u' not in flags and 's' not in flags
    ans=sorted(records,key=lambda r:(key(r),r) if tie else key(r),reverse='r' in flags)
    if 'u' in flags:
        unique=[]; prior=None
        for r in ans:
            if prior is None or prior!=key(r): unique.append(r)
            prior=key(r)
        ans=unique
    return ans
results=[]
for c in cases:
    data=c['delimiter'].join(c['records'])+(c['delimiter'] if c['records'] and not c.get('unterminated') else b'')
    expected=c['delimiter'].join(oracle(c['records'],c['flags']))+(c['delimiter'] if c['records'] else b'')
    row={'name':c['name'],'count':len(c['records']),'flags':c['flags'],'input_sha256':h(data),'expected_sha256':h(expected),'input_hex':data.hex() if len(data)<2000 else None,'expected_hex':expected.hex() if len(expected)<2000 else None,'runs':[]}
    for b in checks:
        r=subprocess.run([str(OUT/f'C05-sort-blanks-{b["name"]}'),*c['flags']],input=data,capture_output=True)
        row['runs'].append({'engine':b['name'],'status':r.returncode,'matches_oracle':r.stdout==expected,'stdout_sha256':h(r.stdout),'stdout_hex':r.stdout.hex() if len(r.stdout)<2000 else None,**json.loads(r.stderr)})
    results.append(row)
report={'text_source_sha256':h(src.encode()),'host':platform.platform(),'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0],'extracted_spans':[{'file':'src/sh/text.c','first':first,'last':last,'sha256':h(body.encode())},{'file':'src/library.common.c','first':mf,'last':ml,'sha256':h(merge.encode())},{'file':'src/sh/text.c','first':pf,'last':pl,'sha256':h(plan.encode())},{'file':'src/sh/text.c','first':df,'last':dl,'sha256':h(dispatch.encode()),'note':'Containing text_sort else closing brace removed, statements unchanged.'}],'fixture_scope':'Exact current sort comparator/key/radix/merge code with scalar host adapters and production effective-key normalization; not full applet option parsing, input arena, architecture assembly, or output-error path. The fixed engine uses the exact production normalization and dispatch. The negative control omits leading-blank handling from the whole-record eligibility condition.','builds':checks,'results':results,'summary':{b['name']:{'cases':len(results),'matches':sum(r['runs'][i]['matches_oracle'] for r in results),'mismatches':[r['name'] for r in results if not r['runs'][i]['matches_oracle']]} for i,b in enumerate(checks)}}
(OUT/'C05-sort-blanks-fixture-results.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report['summary'],indent=2))

assert report['summary']['fixed']['matches']==len(results), report['summary']['fixed']
assert 'space_threshold_16' in report['summary']['unguarded_control']['mismatches']
