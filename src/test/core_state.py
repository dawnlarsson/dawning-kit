#!/usr/bin/env python3
"""Kernel snapshot allocation and Canvas geometry, with syscall/DRM-free mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / "src/core.c").read_text()
pane = (root / "src/canvas/pane.c").read_text()
spark = (root / "src/spark.c").read_text()


def section(source, first, following):
    return source[source.index(first):source.index(following)]


source = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define HOT
#define PURE
#define __user
#define PAGE_SIZE 4096u
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define DEFINE_MUTEX(name) int name
#define likely(x) (x)
#define unlikely(x) (x)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define max_t(t,a,b) max((t)(a),(t)(b))
static unsigned allocations, fail_allocation, copies, fail_copy, failures, checks;
static unsigned cpu_records, network_records, growing, captures;
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static void *kvrealloc(void *old, size_t bytes, int flags) {
    (void)flags;
    return ++allocations == fail_allocation ? NULL : realloc(old, bytes);
}
static int user_copy(void *to, const void *from, size_t bytes) {
    if (++copies == fail_copy) return 1;
    memcpy(to, from, bytes); return 0;
}
#define copy_from_user user_copy
#define copy_to_user user_copy
static unsigned long ktime_get_ns(void) { return 123; }
static unsigned long ktime_get_real_seconds(void) { return 456; }
static unsigned long ktime_get_boottime_ns(void) { return 789; }
static void check(int okay, const char *name) {
    checks++;
    if (!okay && ++failures <= 12) fprintf(stderr, "FAIL %s\n", name);
}
'''
source += section(spark, "#define SPARK_SNAPSHOT_VERSION", "// _IOWR('s', 9")
source += section(core, "struct snapshot_builder", "static HOT void snapshot_system")
source += r'''
static void snapshot_system(struct snapshot_header *header) { header->memory_total=42; }
static void snapshot_cpus(struct snapshot_builder *build, struct snapshot_header *header) {
    if (++captures <= growing)
        cpu_records=(build->capacity-sizeof(*header))/sizeof(struct snapshot_cpu)+1;
    header->cpu_offset=build->required;
    header->cpu_count=cpu_records;
    for (unsigned i=0; i<cpu_records; i++) {
        struct snapshot_cpu *p=snapshot_append(build, sizeof(*p));
        if (p) *p=(struct snapshot_cpu){.id=i,.total_ns=100+i,.idle_ns=20+i};
    }
}
static void snapshot_networks(struct snapshot_builder *build, struct snapshot_header *header) {
    header->network_offset=build->required;
    header->network_count=network_records;
    for (unsigned i=0; i<network_records; i++) {
        struct snapshot_network *p=snapshot_append(build, sizeof(*p));
        if (p) *p=(struct snapshot_network){.name="test",.received=i,.transmitted=7+i};
    }
}
'''
source += section(core, "static HOT long report_snapshot", "static long device_ioctl")
source += r'''
#define WINDOW_FRAME 1u
#define WINDOW_FULLSCREEN 2u
static unsigned canvas_title=24;
struct pane { unsigned display,style,max_width,max_height; int width,height; };
struct output { unsigned width,height; };
static struct output screen={800,600};
static struct output *output_by_index(unsigned index) { return index ? NULL : &screen; }
'''
source += section(pane, "static void pane_size", "static void pane_raise")
source += r'''
static void reset(void) {
    free(snapshot); snapshot=NULL; snapshot_room=0;
    allocations=fail_allocation=copies=fail_copy=0;
    cpu_records=network_records=growing=captures=0;
    assert(!snapshot_lock);
}
int main(void) {
    const unsigned capacities[]={0,111,112,113,4095,4096,4097,8192,SPARK_SNAPSHOT_MAX_BYTES};
    const unsigned records[]={0,1,32,171};
    unsigned char *output=malloc(SPARK_SNAPSHOT_MAX_BYTES+1);
    assert(output);
    for (unsigned shape=0; shape<4; shape++)
    for (unsigned flags=0; flags<=SPARK_SNAPSHOT_KERNEL; flags++)
    for (unsigned c=0; c<sizeof(capacities)/sizeof(*capacities); c++) {
        reset(); cpu_records=records[shape]; network_records=records[3-shape];
        unsigned required=sizeof(struct snapshot_header);
        if (flags & SPARK_SNAPSHOT_CPU) required+=cpu_records*sizeof(struct snapshot_cpu);
        if (flags & SPARK_SNAPSHOT_NETWORK) required+=network_records*sizeof(struct snapshot_network);
        unsigned reported=capacities[c]<sizeof(struct snapshot_header)?sizeof(struct snapshot_header):required;
        struct snapshot_request request={.buffer=(unsigned long)output,
            .capacity=capacities[c],.flags=flags,.version=SPARK_SNAPSHOT_VERSION};
        memset(output,0xa5,capacities[c]+1);
        long result=report_snapshot(&request);
        check(result==(capacities[c]<required?-ENOSPC:0),"snapshot status");
        check(request.required==reported && request.used<=request.capacity,"snapshot extent");
        check(output[request.used]==0xa5,"snapshot output boundary");
        check(snapshot_room<=max(PAGE_SIZE,required*2),"allocation follows data not caller capacity");
        if (request.used) {
            struct snapshot_header *header=(void *)output;
            check(header->bytes==request.used && header->flags==flags &&
                  header->monotonic_ns==123 && header->uptime_ns==789,"snapshot header");
            unsigned at=sizeof(*header);
            if (flags & SPARK_SNAPSHOT_CPU)
                for (unsigned i=0; i<cpu_records && at+sizeof(struct snapshot_cpu)<=request.used; i++,at+=sizeof(struct snapshot_cpu)) {
                    struct snapshot_cpu *p=(void *)(output+at);
                    check(p->id==i && p->total_ns==100+i,"CPU retry contents");
                }
        }
        check(!snapshot_lock,"snapshot lock released");
    }
    reset();
    struct snapshot_request request={.buffer=(unsigned long)output,
        .capacity=SPARK_SNAPSHOT_MAX_BYTES,.version=SPARK_SNAPSHOT_VERSION};
    check(report_snapshot(&request)==0,"metadata only");
    unsigned held=allocations;
    for (unsigned i=0; i<8; i++) check(report_snapshot(&request)==0,"cached metadata");
    check(allocations==held && snapshot_room<=PAGE_SIZE,"no oversized capacity retention");
    cpu_records=300; request.flags=SPARK_SNAPSHOT_CPU; fail_allocation=allocations+1;
    check(report_snapshot(&request)==-ENOMEM && !snapshot_lock,"growth failure");
    fail_allocation=0;
    check(report_snapshot(&request)==0 && request.required==112+300*24,"growth retry");
    check(snapshot_room>=request.required && snapshot_room<request.required*2,"bounded growth slack");
    for (unsigned fault=1; fault<=3; fault++) {
        copies=0; fail_copy=fault;
        check(report_snapshot(&request)==-EFAULT && !snapshot_lock,"user copy failure");
    }
    reset(); fail_allocation=1;
    check(report_snapshot(&request)==-ENOMEM && !snapshot_lock,"initial allocation failure");
    const unsigned changes[]={1,2,UINT_MAX};
    for (unsigned i=0; i<3; i++) {
        reset(); growing=changes[i];
        long result=report_snapshot(&request);
        check(result==(i==2?-ENOSPC:0),"changing inventory status");
        check(captures<=13 && allocations<=13 && !snapshot_lock,"bounded cold captures");
        check(result!=-ENOSPC || request.required>request.capacity,"ENOSPC caller retry protocol");
        check(result || request.required<=request.used,"successful final inventory complete");
    }
    reset();
    const unsigned heights[]={0,1,23,24,25,47,48,49,600,2160};
    for (unsigned scale=1; scale<=2; scale++)
    for (unsigned h=0; h<sizeof(heights)/sizeof(*heights); h++)
    for (unsigned style=0; style<4; style++) {
        canvas_title=24*scale; screen.height=heights[h];
        struct pane p={.style=style,.max_width=640,.max_height=480,.width=123,.height=99};
        unsigned title=(style & WINDOW_FRAME)?canvas_title:0;
        unsigned expected=screen.height>title?screen.height-title:0;
        pane_size(&p);
        check(p.height==((style & WINDOW_FULLSCREEN)?(int)min(expected,480u):99),"fullscreen title subtraction");
        check(p.width==((style & WINDOW_FULLSCREEN)?640:123),"fullscreen width cap");
    }
    free(output);
    printf("  core-state %u of %u\n",checks-failures,checks);
    const char *tally=getenv("TEST_TALLY");
    if (tally) {
        FILE *stream=fopen(tally,"a");
        assert(stream);
        fprintf(stream,"core-state %u %u\n",checks-failures,checks);
        assert(!fclose(stream));
    }
    return failures!=0;
}
'''
with tempfile.TemporaryDirectory(prefix="moonwater-core-state.") as work:
    executable = str(Path(work) / "core-state")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                    "-x", "c", "-", "-o", executable], input=source, text=True, check=True)
    raise SystemExit(subprocess.run([executable]).returncode)
