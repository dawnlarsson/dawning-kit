from pathlib import Path
import re, subprocess
root=Path('artifacts/codec-floor-native');root.mkdir(parents=True,exist_ok=True)
lib=Path('src/library.c').read_text(); checks=Path('test/checks.c').read_text();gz=Path('src/sh/gzip.c').read_text();xz=Path('src/sh/xz.c').read_text()
head='''#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
typedef uint8_t p8; typedef uint16_t p16; typedef uint32_t p32; typedef uint64_t p64;
typedef uint64_t positive; typedef int64_t bipolar; typedef int32_t b32;
#define address_to *
#define address_of &
#define fn void
#define null NULL
#define array_count(x) (sizeof(x)/sizeof((x)[0]))
#define memory_fill memset
#define memory_compare memcmp
#define memory_copy memmove
#define memory_copy_apart memcpy
#define memory_free munmap
#define system_failed(p) ((void*)(p)==MAP_FAILED)
static void *memory(size_t n) { return mmap(0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); }
#define syscall(n) 0
#define system_call_3(no,p,n,prot) mprotect((void*)(p),n,prot)
static int failed, total;
static void check(const char *what, int ok) { total++; if(!ok) { failed++; fprintf(stderr,"FAIL %s\\n",what); } }
static int test_report(void *unused) { printf("%d/%d checks passed on native arm64\\n",total-failed,total); return !!failed; }
extern p32 hash_crc32(p32,void*,positive);
extern p64 hash_crc64(p64,void*,positive);
extern void lzma_range_shift(void*);
extern void lzma_range_encode(void*,void*,positive,positive);
extern bipolar lzma_range_decode(void*,void*,positive);
extern positive huffman_encode_back(void*,void*,positive,void*);
extern void deflate_decode_span(void*);
extern void lzma_decode_span(void*);
extern positive memory_common_prefix(void*,void*,positive);
extern void memory_copy_match(void*,positive,positive);
extern const p32 hash_crc32_tab[2048];
extern const p64 hash_crc64_tab[2048];
static p8 cpu_has_pclmul;
void *floor_copy(void *d,void *s,positive n) __asm__("_memory_copy_apart");
void *floor_copy(void *d,void *s,positive n) { return memcpy(d,s,n); }
void *floor_fill(void *d,p8 v,positive n) __asm__("_memory_fill");
void *floor_fill(void *d,p8 v,positive n) { return memset(d,v,n); }

typedef const char *string_address;
static bipolar system_read_retry(positive fd,void *p,positive n) { return read((int)fd,p,n); }
static bipolar system_write_all(positive fd,void *p,positive n) { return write((int)fd,p,n); }

'''
for name,source in [('gzip_len_base',gz),('gzip_dist_base',gz)]:
 head+=re.search(r'static const p16 '+name+r'\[.*?};',source,re.S).group(0)+'\n'
a=gz.index('static const p32 gzip_length_info');b=gz.index('} gzip_decode_job;',a)+len('} gzip_decode_job;');head+=gz[a:b]+'\n'
head+='#define XZ_CORE_ONLY\n'+xz+'\n'+Path('src/sh/compression_huffman.c').read_text()+'\n'
for name in ('hash_crc32_tab','hash_crc64_tab'):
 a=lib.index('ASM_RODATA_OBJECT_BEGIN('+name);a=lib.index('\n',a)+1;b=lib.index('    ASM_OBJECT_END('+name,a)
 head+='__asm__(".section __TEXT,__const\\n.globl _'+name+'\\n.p2align 4\\n_'+name+':\\n"\n'+lib[a:b]+'".text\\n");\n'
head+=subprocess.check_output(['python3','test/differential.py','--harness','native_extract','src/library.c','hash_crc32','hash_crc64','lzma_range_shift','lzma_range_encode','lzma_range_decode','huffman_encode_back','deflate_decode_span','lzma_decode_span','memory_common_prefix','memory_copy_match'],text=True)
a=checks.index('static p64 floor_crc(');b=checks.index('#endif\n#ifdef BENCH_compression_floor',a)
body=checks[a:b].replace('#ifdef CHECK_compression_floor','')
# Actual Darwin pages are 16 KiB. Keep the LZMA dictionary 4096 bytes,
# ending exactly at its output guard; only mapping geometry changes.
a=body.index('static fn floor_lzma_span(');b=body.index('static fn floor_codebook(',a)
span=body[a:b].replace('input + 5 * 4096','input + 5 * (positive)getpagesize()').replace('output + 4096','output + 2 * (positive)getpagesize() - 4096').replace('6 * 4096','6 * (positive)getpagesize()').replace('3 * 4096','3 * (positive)getpagesize()')
body=body[:a]+'/* SPAN_PLACEHOLDER */\n'+body[b:]
a=body.index('static p8 address_to floor_pages(');b=body.index('static fn floor_checksums',a)
body=body[:a]+body[a:b].replace('4096','(positive)getpagesize()')+body[b:]
body=body.replace('[8192]', '[8 * 1024]')
body=body.replace('p[4096 + i]','p[getpagesize() + i]').replace('8192','(2 * (positive)getpagesize())')
body=body.replace(': 4096)',': (positive)getpagesize())')
body=body.replace('input + 4096, 0, 4096','input + getpagesize(), 0, getpagesize()')
body=body.replace('positive i = 4096; i < 10 * 4096','positive i = getpagesize(); i < 10 * (positive)getpagesize()')
body=body.replace('10 * 4096','10 * (positive)getpagesize()').replace('3 * 4096','3 * (positive)getpagesize()').replace('3*4096','3*(positive)getpagesize()').replace('11*4096','11*(positive)getpagesize()')
body=body.replace('for (positive i = 0; i < 4096; i++) p[getpagesize() + i]', 'for (positive i = 0; i < (positive)getpagesize(); i++) p[getpagesize() + i]')
body=body.replace('/* SPAN_PLACEHOLDER */',span)
(root/'native-arm64.c').write_text(head+body)
subprocess.run(['clang','-O2',str(root/'native-arm64.c'),'-o',str(root/'native-arm64')],check=True)
subprocess.run([str(root/'native-arm64')],check=True)
