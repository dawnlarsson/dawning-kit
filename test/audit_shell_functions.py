#!/usr/bin/env python3
"""Check retained AST ownership and atomic allocation using a hosted extraction.

Run: python3 test/audit_shell_functions.py [--out output-directory]
Project span/copy primitives are hosted adapters here; real-shell grammar and
call-frame lifetime are covered separately by test/shell_functions.py.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--out')
args = parser.parse_args()
owned_output = None if args.out else tempfile.TemporaryDirectory(prefix='shell-functions-')
out = pathlib.Path(args.out or owned_output.name)
out.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'src/sh/parse.c').read_text()
start = source.index('typedef struct\n{\n        b32 kind;', source.index('One node shape'))
types = source[start:source.index('#define PARSE_WORD_LITERAL')]
engine = source[source.index('/* Retained bodies own independent ranges'):]
reserve = 'static b32 parse_keep_reserve(positive arena, b32 count, b32 floor)\n{'
assert engine.count(reserve) == 1
engine = engine.replace(reserve, reserve + '\n        if ((b32)arena == injected_failure) return -1;')

prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef int32_t b32;
typedef uint8_t b8;
typedef char p8;
typedef uintptr_t positive;
typedef char *string_address;
#define fn void
#define address_to *
#define array_count(x) (sizeof(x)/sizeof((x)[0]))
static void memory_fill(void *p, int value, positive n) { memset(p,value,n); }
static void memory_copy(void *to, const void *from, positive n) { memcpy(to,from,n); }
static void memory_copy_end(char *to, const char *from, positive n) { memcpy(to,from,n);to[n]=0; }
static positive memory_span_byte(const void *p, int byte, positive n) { const unsigned char *s=p;positive i=0;while(i<n&&s[i]==byte)i++;return i; }
static void *memory_first_of(const void *p, int byte, positive n) { return memchr(p,byte,n); }
static void *memory_last_of(const void *p, int byte, positive n) { const unsigned char *s=p;while(n) { n--;if(s[n]==byte)return (void *)(s+n); }return NULL; }
static b32 injected_failure=-1;
'''
state=r'''
static b32 parse_node_used,parse_word_used,parse_redirect_used;
static b32 parse_node_top=PARSE_NODES,parse_word_top=PARSE_WORDS,parse_redirect_top=PARSE_REDIRECTS;
static char parse_kept_text[PARSE_KEPT_TEXT],here_text[PARSE_KEPT_TEXT];
'''
main=r'''
static positive checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
static char words[64][128];
static void prepare(int serial,int count,int redirected) {
    parse_node_used=3;parse_word_used=count;parse_redirect_used=redirected;
    parse_nodes[1]=(parse_node){.kind=12,.left=2};
    parse_nodes[2]=(parse_node){.kind=1,.word=0,.word_count=count,.redirect=0,.redirect_count=redirected};
    for(int i=0;i<count;i++) {
        snprintf(words[i],sizeof(words[i]),"word-%d-%d",serial,i);
        parse_words[i]=words[i];parse_word_lengths[i]=strlen(words[i]);
        parse_word_name_lengths[i]=i;parse_word_name_hashes[i]=serial+i;parse_word_flags[i]=(unsigned char)i;
    }
    if(redirected) {
        snprintf(here_text,sizeof(here_text),"body-%d",serial);
        parse_redirects[0]=(parse_redirect){.op=3,.fd=2,.text=words[0],.text_length=strlen(words[0]),.body=0,.body_length=strlen(here_text)};
    }
}
static void check_body(int body,int serial,int count,int redirected) {
    CHECK(body>0&&parse_kept_bodies[body].references>0);
    parse_node *node=parse_nodes+parse_nodes[body].left;
    CHECK(node->word_count==count&&node->redirect_count==redirected);
    for(int i=0;i<count;i++) {
        char wanted[128];snprintf(wanted,sizeof(wanted),"word-%d-%d",serial,i);
        int w=node->word+i;
        CHECK(!strcmp(parse_words[w],wanted));CHECK(parse_word_lengths[w]==strlen(wanted));
        CHECK(parse_word_name_lengths[w]==(positive)i&&parse_word_name_hashes[w]==(positive)(serial+i)&&parse_word_flags[w]==i);
    }
    if(redirected) {
        char wanted[128];snprintf(wanted,sizeof(wanted),"body-%d",serial);
        parse_redirect *r=parse_redirects+node->redirect;
        CHECK(r->kept&&r->body_length==strlen(wanted));CHECK(!strcmp(parse_kept_text+r->body,wanted));
    }
}
static uint64_t hash_bytes(const void *p,size_t n,uint64_t h) { const unsigned char*s=p;for(size_t i=0;i<n;i++)h=(h^s[i])*1099511628211ULL;return h; }
static uint64_t snapshot(void) {
    uint64_t h=1469598103934665603ULL;
#define HASH(x) h=hash_bytes(x,sizeof(x),h)
    HASH(parse_nodes);HASH(parse_words);HASH(parse_word_lengths);HASH(parse_word_name_lengths);HASH(parse_word_name_hashes);HASH(parse_word_flags);HASH(parse_redirects);HASH(parse_kept_text);HASH(parse_kept_bodies);HASH(parse_node_kept);HASH(parse_word_kept);HASH(parse_redirect_kept);HASH(parse_text_kept);
#undef HASH
    return h;
}
static void empty(void) {
    CHECK(parse_node_top==PARSE_NODES&&parse_word_top==PARSE_WORDS&&parse_redirect_top==PARSE_REDIRECTS);
    for(size_t a=0;a<array_count(parse_kept_arenas);a++)CHECK(memory_span_byte(parse_kept_arenas[a].occupied,0,parse_kept_arenas[a].room)==(positive)parse_kept_arenas[a].room);
    for(int i=0;i<PARSE_NODES;i++)CHECK(!parse_kept_bodies[i].references);
}
static void reserve_bitmap_cases(void) {
    unsigned char expected[PARSE_NODES];
    for (int n=0;n<=10;n++)
        for (unsigned bits=0;bits<(1u<<n);bits++)
            for (int low=0;low<=n;low++)
                for (int count=0;count<=n+1;count++) {
                    memset(parse_node_kept,1,sizeof(parse_node_kept));
                    for (int j=0;j<n;j++)parse_node_kept[PARSE_NODES-n+j]=(bits>>j)&1;
                    memcpy(expected,parse_node_kept,sizeof expected);
                    int floor=PARSE_NODES-n+low,wanted=count? -1:0;
                    for (int at=floor;count&&at<=PARSE_NODES-count;at++) {
                        int free=1;
                        for (int j=0;j<count;j++)if(expected[at+j])free=0;
                        if(free)wanted=at;
                    }
                    if(count&&wanted>=0)memset(expected+wanted,1,count);
                    CHECK(parse_keep_reserve(0,count,floor)==wanted);
                    CHECK(!memcmp(expected,parse_node_kept,sizeof expected));
                }
    memset(parse_node_kept,0,sizeof(parse_node_kept));
}
int main(void) {
    reserve_bitmap_cases();
    prepare(1,3,1);int old=parse_keep(1,0);CHECK(old);check_body(old,1,3,1);
    for(injected_failure=0;injected_failure<4;injected_failure++) {
        prepare(2,4,1);uint64_t held=snapshot();int n=parse_keep(1,old);
        CHECK(!n);CHECK(snapshot()==held);check_body(old,1,3,1);
    }
    injected_failure=-1;
    prepare(3,1,0);parse_word_lengths[0]=UINTPTR_MAX;uint64_t held=snapshot();
    CHECK(!parse_keep(1,old));CHECK(snapshot()==held);check_body(old,1,3,1);
    parse_release(old);empty();
    // Active versions retain their bytes despite later definitions and return in arbitrary order.
    int versions[40];
    for(int i=0;i<40;i++) {
        prepare(i,2,1);versions[i]=parse_keep(1,i?versions[i-1]:0);CHECK(versions[i]);parse_kept_bodies[versions[i]].references++;
    }
    for(int i=0;i<40;i++)check_body(versions[i],i,2,1);
    for(int i=0;i<40;i+=2)parse_release(versions[i]);
    for(int i=1;i<40;i+=2)parse_release(versions[i]);
    parse_release(versions[39]);empty();
    // Nested definitions copy owned heredoc text, then survive the original owner.
    prepare(91,2,1);old=parse_keep(1,0);parse_kept_bodies[old].references++;
    int nested=parse_keep(old,0);CHECK(nested);parse_release(old);parse_release(old);check_body(nested,91,2,1);parse_release(nested);empty();
    // Random replacement/deletion varies all independent arena extents.
    int live[12]={0},serial[12]={0},counts[12]={0},redirected[12]={0};srand(7);
    for(int step=0;step<10000;step++) {
        int slot=rand()%12;
        if(live[slot]&&rand()%5==0) {parse_release(live[slot]);live[slot]=0;}
        else {
            int count=1+rand()%9,red=rand()%2;prepare(step,count,red);
            int made=parse_keep(1,live[slot]);CHECK(made);live[slot]=made;serial[slot]=step;counts[slot]=count;redirected[slot]=red;
        }
        for(int i=0;i<12;i++)if(live[i])check_body(live[i],serial[i],counts[i],redirected[i]);
    }
    for(int i=0;i<12;i++)
        parse_release(live[i]);
    empty();
    // No words, redirects or text: zero extents must not pin unrelated frontiers.
    prepare(0,0,0);old=parse_keep(1,0);CHECK(old);CHECK(parse_word_top==PARSE_WORDS&&parse_redirect_top==PARSE_REDIRECTS);parse_release(old);empty();
    printf("{\"checks\":%lu,\"body_table_bytes\":%lu,\"occupancy_bytes\":%lu}\n",(unsigned long)checks,(unsigned long)sizeof(parse_kept_bodies),(unsigned long)(sizeof(parse_node_kept)+sizeof(parse_word_kept)+sizeof(parse_redirect_kept)+sizeof(parse_text_kept)));
}
'''
code = prefix + types + state + engine + main
unit = out / 'retention.c'
binary = out / 'retention'
unit.write_text(code)
command = shlex.split(os.environ.get('CC', 'cc')) + [
    '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
    '-fsanitize=address,undefined', str(unit), '-o', str(binary)]
build = subprocess.run(command, capture_output=True, text=True)
(out / 'build.log').write_text(build.stdout + build.stderr)
assert build.returncode == 0, build.stderr
run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
(out / 'run.log').write_text(run.stdout + run.stderr)
assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
result = json.loads(run.stdout)
report = dict(
    source_sha256=hashlib.sha256(source.encode()).hexdigest(),
    extracted_sha256=hashlib.sha256(code.encode()).hexdigest(),
    compiler_command=command,
    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
    result=result,
    limitations='Hosted retention engine; libc adapters replace project span/copy helpers. '
                'Failure injection occurs only at arena reservation entry. Runtime grammar '
                'and function-call reference routing are validated separately with real shells.')
(out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
print(f"function-storage-sanitizer {result['checks']} of {result['checks']}")
if os.environ.get('TEST_TALLY'):
    with open(os.environ['TEST_TALLY'], 'a') as tally:
        tally.write(f"function-storage-sanitizer {result['checks']} {result['checks']}\n")
