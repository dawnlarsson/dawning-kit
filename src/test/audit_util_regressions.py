"""C01 regression: exact summary and arena bodies against synthetic topology. No applet/device/sysfs operations.

Copies exact current-source bodies into a hosted fixture. The production
192 MiB bump arena and full summary builder are retained; I/O/rendering and
primitive library functions are substituted explicitly below.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
import sys
workspace = None if len(sys.argv) > 1 else tempfile.TemporaryDirectory(prefix="audit-util-results-")
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(workspace.name)
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'src/sh/util_linux.c').read_text()
text_source = (ROOT / 'src/sh/text.c').read_text()

def function(s, name):
    m = re.search(r'^static [^\n]*\b' + re.escape(name) + r'\(', s, re.M)
    if not m:
        raise ValueError(name)
    a = s.index('{', m.start())
    d, i = 1, a + 1
    while d:
        d += (s[i] == '{') - (s[i] == '}')
        i += 1
    return s[m.start():i]

def span(s, begin, finish):
    return s[s.index(begin):s.index(finish, s.index(begin))]

prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
typedef uint64_t positive;
typedef int64_t bipolar;
typedef int32_t b32;
typedef uint32_t p32;
typedef unsigned char p8;
typedef const char *string_address;
typedef void *address_any;
typedef void fn;
#define address_to *
#define address_of &
#define null NULL
#define end 0
#define positive_bits 64
#define positive_max UINT64_MAX
#define UL_CPU_WORDS 1024
#define FILE_PATH_MAX 4096
#define TEXT_ARENA_BYTES (192u << 20)
#define X64 1
#define X86 0
#define TEXT_ARENA_GROW
#define memory_copy_apart memcpy
#define memory_growth(room,wanted,first) ((wanted) > (room)*2 ? ((wanted) > (first) ? (wanted) : (first)) : (room)*2)
#define memory_fill memset
#define memory_copy memcpy
#define memory_copy_end(d,s,n) ((p8 *)memcpy((d),(s),(n))+(n))
#define string_length(s) strlen((const char *)(s))
#define string_get(s) (*(s))
#define string_is(s,c) (*(s)==(c))
#define string_search(s,n) strstr((const char *)(s),(n))
#define string_compare_max(s,t,n) strncmp((const char *)(s),(t),(n))
#define byte_to_upper(c) toupper((unsigned char)(c))
#define bits_counted(n) __builtin_popcountll(n)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define array_count(a) (sizeof(a)/sizeof((a)[0]))
typedef void (*writer)(address_any,positive);
typedef struct { p8 system[65],node[65],release[65],version[65],machine[65],domain[65]; } file_machine;
static p8 fixture_arena[TEXT_ARENA_BYTES];
static p8 *text_arena;
static positive text_arena_used;
static bool ul_lscpu_failed;
static positive fixture_errors;
static void text_error(void *unused, const char *message) { (void)unused; (void)message; fixture_errors++; }
static void *memory(positive bytes) { (void)bytes; abort(); }
static bool system_failed(positive value) { return value >= positive_max-4095; }
static positive positive_into_string(p8 *p,positive n) { return (positive)sprintf((char *)p,"%llu",(unsigned long long)n); }
static void positive_to_string(writer w,positive n) { p8 b[24]; positive k=positive_into_string(b,n); w(b,k); }
static positive storage_hex_padded(p8 *p,p32 n,positive width,bool upper) { (void)upper; return (positive)sprintf((char *)p,"%0*x",(int)width,n); }
typedef struct { const char *key,*heading; int width; bool numeric; int kind; } ul_table_column;
#define UL_TABLE_STRING 0
static positive captured_count;
static void *captured_items;
static bool captured_json;
#define ul_table(name,items,count,definitions,columns,n,headings,raw,field) do { captured_count=(count); captured_items=(items); captured_json=((name)!=NULL); } while(0)
typedef struct {int unused;} file_walk;
struct linux_dirent64 {char d_name[256];};
#define AT_FDCWD -100
static positive mock_next_calls;
static bool file_walk_open(file_walk *w,int fd,const char *path) { (void)w;(void)fd; if(strcmp(path,"/sys/devices/system/cpu/vulnerabilities"))abort(); return true; }
static struct linux_dirent64 *file_walk_next(file_walk *w) { (void)w; mock_next_calls++; return NULL; }
static void file_walk_close(file_walk *w) { (void)w; }
static void path_join(p8 *p,positive n,const char *base,const char *name) { (void)p;(void)n;(void)base;(void)name;abort(); }
static bipolar ul_slurp_word(p8 *path,p8 *value,positive size) { (void)path;(void)value;(void)size;abort(); }
'''

definitions = span(source, '#define UL_LSCPU_CACHE_MAX 8', 'static positive ul_lscpu_set_count(')
summary_types = span(source, 'typedef struct\n{\n        string_address field;\n        string_address data;', 'static fn ul_lscpu_summary_add(')
set_globals = span(source, 'static p8 address_to ul_lscpu_set_into;', 'static fn ul_lscpu_set_write(')
names = ['ul_lscpu_set_count','ul_lscpu_keep','ul_lscpu_number','ul_cpu_has',
         'ul_cpu_list_write','ul_cpu_mask_write','ul_lscpu_set_write',
         'ul_lscpu_set_text','ul_lscpu_summary_add','ul_lscpu_cache_size',
         'ul_lscpu_cache_summary','ul_lscpu_summary']

main = r'''
static void run_case(positive n,bool duplicate,bool negative,bool json,bool exhaust) {
  text_arena=fixture_arena; text_arena_used=0; ul_lscpu_failed=false;
  captured_count=0; captured_items=NULL; fixture_errors=0;
  memset(&ul_lscpu,0,sizeof ul_lscpu);
  memset(ul_lscpu_present,0,sizeof ul_lscpu_present);
  memset(ul_lscpu_online,0,sizeof ul_lscpu_online);
  ul_lscpu.cpus=text_arena_take(n*sizeof(*ul_lscpu.cpus));
  ul_lscpu.cpu_count=ul_lscpu.present_count=ul_lscpu.online_count=ul_lscpu.core_count=n;
  ul_lscpu.node_count=duplicate?1:n;
  memcpy(ul_lscpu.machine.machine,"fixture64",10);
  for(positive i=0;i<n;i++) {
    ul_lscpu.cpus[i].id=i;
    ul_lscpu.cpus[i].node=negative?-1:duplicate?0:(bipolar)i;
    ul_lscpu_present[i/64]|=(positive)1<<(i%64);
    ul_lscpu_online[i/64]|=(positive)1<<(i%64);
  }
  if(exhaust) text_arena_used=TEXT_ARENA_BYTES;
  mock_next_calls=0;
  ul_lscpu_summary(json,false,false);
  if(exhaust) {
    if(!ul_lscpu_failed || !fixture_errors || captured_items) abort();
  } else {
    ul_lscpu_summary_item *items=captured_items;
    char expected[24]; sprintf(expected,"%llu",(unsigned long long)n);
    positive expected_count=10+(negative?0:duplicate?1:n);
    if(ul_lscpu_failed || captured_count!=expected_count || captured_json!=json ||
       strcmp(items[3].data,expected)) abort();
    positive nodes=negative?0:duplicate?1:n;
    for(positive i=0;i<nodes;i++) {
      char field[64]; sprintf(field,"NUMA node%llu CPU(s):",(unsigned long long)i);
      if(strcmp(items[10+i].field,field)) abort();
      if(!duplicate) {
        sprintf(expected,"%llu",(unsigned long long)i);
        if(strcmp(items[10+i].data,expected)) abort();
      }
    }
  }
  printf("summary cpus=%llu duplicate=%d negative=%d json=%d exhausted=%d rows=%llu OK\n",
         (unsigned long long)n,duplicate,negative,json,exhaust,(unsigned long long)captured_count);
}
int main(void) {
  for(positive json=0;json<2;json++) {
    run_case(1,false,false,json,false);
    run_case(86,false,false,json,false);
    run_case(87,false,false,json,false);
    run_case(97,false,false,json,false);
    run_case(257,false,false,json,false);
    run_case(97,true,false,json,false);
    run_case(97,false,true,json,false);
    run_case(97,false,false,json,true);
  }
  ul_lscpu_summary_rows rows={0}; ul_lscpu_failed=false;
  text_arena_used=0;
  for(positive i=0;i<96;i++) ul_lscpu_summary_add(&rows,"field",ul_lscpu_number(i));
  void *before=rows.items; text_arena_used=TEXT_ARENA_BYTES;
  ul_lscpu_summary_add(&rows,"field","97");
  if(!ul_lscpu_failed || rows.count!=96 || rows.items!=before || strcmp(rows.items[95].data,"95")) abort();
  puts("summary growth allocation failure keeps prior rows OK");
  return 0;
}
'''

common=(ROOT/'src/library.common.c').read_text()
arena_macro=span(common,'#define array_arena_reserve(', '#define byte_store_reserve(')
file_source=(ROOT/'src/sh/file.c').read_text()
c='\n'.join([prefix, arena_macro, function(text_source,'text_arena_take'),
              function(file_source,'text_arena_grow'),definitions,summary_types,set_globals]+
             [function(source,n) for n in names]+[main])
(OUT/'lscpu-summary.c').write_text(c)
command=['clang','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
         '-Wno-pointer-sign',str(OUT/'lscpu-summary.c'),'-o',str(OUT/'lscpu-summary')]
build=subprocess.run(command,capture_output=True,text=True)
(OUT/'lscpu-build.log').write_text(build.stdout+build.stderr)
if build.returncode: raise RuntimeError(build.stderr)
run=subprocess.run([str(OUT/'lscpu-summary')],capture_output=True,text=True,timeout=30)
(OUT/'lscpu-run.log').write_text(run.stdout+run.stderr)
assert run.returncode==0,run.stderr
print(run.stdout,end='')
(OUT/'lscpu-results.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
    'compile':command,'cases':17,'status':run.returncode,
    'limits':'Exact summary, retention, reserve and arena bodies; filesystem, table writer and primitive library operations mocked.'},indent=2)+'\n')
