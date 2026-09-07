#!/usr/bin/env python3
"""Kernel snapshot allocation and Canvas geometry, with syscall/DRM-free mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
core = (root / "src/core.c").read_text()
pane = (root / "src/canvas/pane.c").read_text()
canvas = (root / "src/canvas/canvas.c").read_text()
compose = (root / "src/canvas/compose.c").read_text()
drag = (root / "src/canvas/drag.c").read_text()
output = (root / "src/canvas/output.c").read_text()
pointer = (root / "src/canvas/pointer.c").read_text()
spark = (root / "src/spark.c").read_text()


def section(source, first, following):
    return source[source.index(first):source.index(following)]


def canvas_sources(work, arch):
    paint = (root / "src/canvas/paint.c").read_text()
    text = (root / "src/canvas/text.c").read_text()
    cells = section(canvas, "struct target\n", "static void target_row")
    cells += section(compose, "struct shape\n", "static _Bool shape_span")
    cells += paint[paint.index("static CONST int round_inset"):]
    cells += section(compose, "static _Bool shape_span", "static void shape_blit")
    cells += section(paint, "static const u32 canvas_terminal", "static void canvas_palette")
    cells += section(paint, "static void bits_draw", "/*\n        Cursors.")
    cells += section(text, "static const struct font_desc", "/*\n        Where one line ends.")
    cells += section(compose, "static void cell_draw", "/*\n        A window made of text.\n\n        The rows")
    (work / "canvas-cells.inc").write_text(cells)
    (work / "canvas-ring.inc").write_text(section(compose, "static void compose_cells", "/*\n        A pane, in target coordinates."))
    # kit/asm supplies the function macros; these empty include files replace
    # only the kernel declarations, never the renderer's assembly bodies.
    (work / "linux").mkdir(exist_ok=True)
    for name in ("export.h", "linkage.h"):
        (work / "linux" / name).write_text("")
    inputs = [str(root / "src/test/canvas_cells.c")]
    for name in ("glyph", "fill"):
        target = work / f"{name}.S"
        subprocess.run(["sh", str(root / "kit/asm"), arch,
                        str(root / f"src/canvas/{name}.asm"), str(target)], check=True)
        inputs.append(str(target))
    return inputs


source = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
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
#define clamp(a,b,c) min(max(a,b),c)
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
#define WINDOW_MINIMIZED 4u
#define true 1
#define false 0
#define CONST
#define INK_DESKTOP 0
#define WRITE_ONCE(a,b) ((a)=(b))
static int canvas_title=24, canvas_border=2, canvas_cell_w=8, canvas_cell_h=16;
struct pane { unsigned display,style,max_width,max_height; int width,height,x,y,edge;
    int saved_x,saved_y,saved_w,saved_h; unsigned saved_display; _Bool maximized;
    struct pane *shared;
};
struct output { unsigned width,height; int x,y; };
static struct output screen={.width=800,.height=600};
static struct pane panes[16];
static unsigned pane_count;
#define list_for_each_entry(p,list,link) for (p=&screen;p==&screen;p=NULL)
#define list_for_each_entry_reverse(p,list,link) \
    for (unsigned pane_i=pane_count;pane_i && ((p)=&panes[--pane_i],1);)
static struct output *output_by_index(unsigned index) { return index ? NULL : &screen; }
static int point_in_rect(int x,int y,int w,int h,int px,int py) {
    return px>=x && px<x+w && py>=y && py<y+h;
}
static void pane_limits(struct pane *p,int *w,int *h) { *w=p->max_width; *h=p->max_height; }
static void pane_reshape(struct pane *p,int x,int y,int w,int h) {
    p->x=x; p->y=y; p->width=w; p->height=h;
}
'''
source += section(pane, "static void pane_size", "static void pane_raise")
source += section(pane, "static void desktop_grid(", "/*\n        The grid a window")
source += section(drag, "static void pane_maximize", "/*\n        Taking a maximized")
source += section(canvas, "static void pane_frame", "// Which edges")
source += r'''
struct drm_rect { int x1,y1,x2,y2; };
struct target { u32 *pixels; unsigned pitch; int x,y; const u32 *ink; };
static void drm_rect_init(struct drm_rect *r,int x,int y,int w,int h) {
    *r=(struct drm_rect){x,y,x+w,y+h};
}
static unsigned long canvas_painted,canvas_runs;
#define memory_copy_apart memcpy
static void canvas_rect_fill(u32 *at,unsigned long pitch,unsigned long w,
                             unsigned long h,u32 colour) {
    for (unsigned long y=0;y<h;y++) for (unsigned long x=0;x<w;x++) at[y*pitch+x]=colour;
}
'''
source += section(compose, "#define DESKTOP_PIECES", "static HOT void compose_clip")
source += r'''
#define IS_ENABLED(x) largest
#define log_canvas(...) ((void)0)
#define log_canvas_error(...) ((void)0)
struct canvas { int client; };
static int largest,build_results[2],commit_results[2];
static unsigned builds,commits,releases,attaches,mode_bits;
static int canvas_build(struct canvas *c,int big) {
    (void)c; mode_bits|=(unsigned)big<<builds; return build_results[builds++];
}
static int drm_client_modeset_commit(int *client) { (void)client; return commit_results[commits++]; }
static void desktop_attach_buffers(void) { attaches++; }
static void canvas_release(struct canvas *c) { (void)c; releases++; }
'''
source += section(output, "static int canvas_start(struct canvas *canvas)\n{",
                  "        // The cursor is drawn from a bitmap") + "return 0;\n}\n"
source += (root / "src/platform/spark.inc").read_text()
source += r'''
typedef unsigned refcount_t;
#define refcount_set(p,n) (*(p)=(n))
#define GFP_KERNEL_ACCOUNT 0
#define kvmalloc(n,flags) kvrealloc(NULL,n,flags)
#define kvfree free
#define memory_first_of memchr
'''
source += section(core, "struct spawn_strings", "struct pane;")
source += section(core, "static int copy_strings", "static long do_spawn")
source += r'''
#define EV_ABS 3
#define ABS_X 0
#define ABS_Y 1
struct input_absinfo { int minimum,maximum; };
struct input_dev { struct input_absinfo absinfo[2]; };
struct input_handle { struct input_dev *dev; };
static struct { int width,height,abs_x,abs_y; unsigned abs_have; } desktop;
static u64 div_u64(u64 a,u32 b) { return a/b; }
static void absolute_event(struct input_handle *handle,unsigned type,unsigned code,int value) {
'''
source += section(pointer, "        if (type == EV_ABS)", "        if (type == EV_KEY)") + "}\n"
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
    for (unsigned scale=1;scale<=3;scale++)
    for (unsigned h=0;h<80;h++)
    for (unsigned w=0;w<20;w++)
    for (unsigned framed=0;framed<2;framed++) {
        canvas_title=20*scale; canvas_border=2*scale;
        canvas_cell_w=8*scale; canvas_cell_h=16*scale;
        screen.width=w; screen.height=h;
        unsigned columns,rows;
        desktop_grid(w,h,&columns,&rows);
        check(columns==(unsigned)max((int)w-4*(int)scale,0)/(8*scale) &&
              rows==(unsigned)max((int)h-26*(int)scale,0)/(16*scale),"framed grid bounds");
        struct pane p={.style=framed,.max_width=640,.max_height=480,
                       .width=123,.height=99,.x=17,.y=19};
        pane_maximize(&p,0,0);
        check(p.width==max((int)w-(framed?4*(int)scale:0),0) &&
              p.height==max((int)h-(framed?26*(int)scale:0),0),"maximize body bounds");
        pane_maximize(&p,0,0);
        check(!p.maximized && p.width==123 && p.height==99 && p.x==17 && p.y==19,
              "maximize restoration");
    }
    const int answers[]={0,-EBUSY,-EINVAL,-ENOMEM};
    for (largest=0;largest<=1;largest++)
    for (unsigned a=0;a<4;a++) for (unsigned b=0;b<4;b++)
    for (unsigned c=0;c<4;c++) for (unsigned d=0;d<4;d++) {
        build_results[0]=answers[a]; build_results[1]=answers[b];
        commit_results[0]=answers[c]; commit_results[1]=answers[d];
        builds=commits=releases=attaches=mode_bits=0;
        int retry=!answers[a] && answers[c] && answers[c]!=-EBUSY;
        int expected=answers[a]?answers[a]:retry?answers[b]?answers[b]:
            answers[d]==-EBUSY?0:answers[d]:0;
        struct canvas card={0};
        check(canvas_start(&card)==expected,"mode fallback status");
        check(builds==1u+retry && commits==(unsigned)((!answers[a])+(retry&&!answers[b])) &&
              attaches==commits && releases==(unsigned)(retry+(retry&&!answers[b]&&answers[d]&&answers[d]!=-EBUSY)) &&
              mode_bits==(unsigned)largest,"mode fallback ownership and bound");
    }
    unsigned random=0x12345678;
    for (unsigned scene=0;scene<2000;scene++) {
        u32 pixels[32*32]={0},ink[]={0x12345678};
        struct target t={.pixels=pixels,.pitch=32,.ink=ink};
        pane_count=scene%17;
        for (unsigned p=0;p<pane_count;p++) {
            random=random*1664525+1013904223;
            panes[p]=(struct pane){.x=(int)(random%40)-4,.y=(int)((random>>8)%40)-4,
                .width=(random>>16)%24,.height=(random>>24)%24,
                .style=scene%7==0?WINDOW_MINIMIZED:0,.edge=scene%3};
        }
        desktop_fill(&t,2,3,30,29);
        for (int y=0;y<32;y++) for (int x=0;x<32;x++) {
            int outside=x<2||x>=30||y<3||y>=29,covered=0;
            for (unsigned p=0;p<pane_count;p++) {
                int radius=min(panes[p].edge,min(panes[p].width,panes[p].height)/2);
                covered|=!(panes[p].style&WINDOW_MINIMIZED) &&
                    point_in_rect(panes[p].x+radius,panes[p].y+radius,
                        panes[p].width-2*radius,panes[p].height-2*radius,x,y);
            }
            if (outside || !covered)
                check(pixels[y*32+x]==(outside?0:ink[0]),"desktop clip and uncovered pixels");
        }
    }
    for (unsigned bytes=1;bytes<=64;bytes++)
    for (unsigned spacing=1;spacing<=17;spacing++)
    for (unsigned count=1;count<=24;count++) {
        char block[64]; unsigned available=0;
        for (unsigned at=0;at<bytes;at++) {
            block[at]=(at%spacing==spacing-1)?0:'a';
            available+=!block[at];
        }
        struct spawn_strings *copied=NULL;
        allocations=fail_allocation=copies=fail_copy=0;
        int result=copy_strings((unsigned long)block,bytes,count,&copied);
        check(result==(count<=available?0:-EINVAL),"flat argument delimiter bounds");
        if (copied) {
            char *at=block;
            for (unsigned i=0;i<count;i++) {
                check(!strcmp(copied->vector[i],at),"flat argument contents");
                at+=strlen(at)+1;
            }
            check(!copied->vector[count],"flat argument sentinel");
            free(copied);
        }
    }
    struct spawn_strings *copied=NULL;
    allocations=0; fail_allocation=1;
    check(copy_strings((unsigned long)"",1,1,&copied)==-ENOMEM,"argument allocation failure");
    fail_allocation=0; copies=0; fail_copy=1;
    check(copy_strings((unsigned long)"",1,1,&copied)==-EFAULT,"argument usercopy failure");
    fail_copy=0;
    const int coordinates[]={INT_MIN,-32768,-1,0,1,32767,INT_MAX};
    struct input_dev input={0}; struct input_handle handle={&input};
    desktop.width=3840; desktop.height=2160;
    for (unsigned lo=0;lo<7;lo++) for (unsigned hi=0;hi<7;hi++)
    for (unsigned value=0;value<7;value++) for (unsigned axis=0;axis<2;axis++) {
        int low=coordinates[lo],high=coordinates[hi];
        input.absinfo[axis]=(struct input_absinfo){low,high};
        desktop.abs_x=123; desktop.abs_y=456; desktop.abs_have=0;
        absolute_event(&handle,EV_ABS,axis,coordinates[value]);
        int expected=axis?456:123;
        if (high>low) expected=(int)(((uint64_t)((int64_t)clamp(coordinates[value],low,high)-low)*
                                      (axis?2160:3840))/((int64_t)high-low));
        check((axis?desktop.abs_y:desktop.abs_x)==expected &&
              (axis?desktop.abs_x:desktop.abs_y)==(axis?123:456) &&
              desktop.abs_have==(high>low?(1u<<axis):0),"absolute axis range and ownership");
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
    for line in (root / "kernel/profile/arch/arm.pi").read_text().splitlines():
        if line.startswith("#> post sh "):
            assert (root / line.removeprefix("#> post sh ")).is_file()
    # Exercise the real patch script against disposable files, never a kernel
    # tree or elevated sudo. A stock fresh tree must not acquire our fold, and
    # link repair must leave an unexpected directory/file in place.
    patch = root / "kernel/patch/apply"
    for stock in (False, True):
        for kind in ("absent", "symlink", "directory", "file"):
            tree = Path(work) / f"patch-{stock}-{kind}"
            for directory in ("kit", "src", "linux/kernel", "linux/arch/x86/include/asm",
                              "linux/arch/x86/lib", "kernel/patch"):
                (tree / directory).mkdir(parents=True, exist_ok=True)
            (tree / "kit/common").write_text(r'''
key_one() { printf '%s\n' x86_64; }
die() { printf '%s\n' "$*" >&2; exit 1; }
sudo() { "$@"; }
line_add() { grep -qxF "$2" "$1" || printf '%s\n' "$2" >> "$1"; }
line_add_padded() { line_add "$@"; }
''')
            header = tree / "linux/arch/x86/include/asm/string_64.h"
            header.write_text("#ifdef __KERNEL__\n#endif /* __KERNEL__ */\n")
            (tree / "linux/arch/x86/lib/Makefile").write_text("obj-y += memcpy_$(BITS).o\n")
            for target in ("linux/Kconfig", "linux/kernel/Makefile"):
                (tree / target).write_text("")
            shutil.copy(root / "kernel/patch/fold-x86.h", tree / "kernel/patch/fold-x86.h")
            link = tree / "linux/kernel/moonwater"
            if kind == "symlink":
                link.symlink_to(tree / "missing-source")
            elif kind == "directory":
                link.mkdir()
                (link / "precious").write_text("preserve me\n")
            elif kind == "file":
                link.write_text("preserve me\n")
            env = {**os.environ, "MOONWATER_STOCK": "1" if stock else ""}
            result = subprocess.run(["sh", str(patch)], cwd=tree, env=env,
                                    text=True, capture_output=True)
            assert (result.returncode == 0) == (kind in ("absent", "symlink")), result.stderr
            assert ("MOONWATER_FOLD_X86" in header.read_text()) != stock
            if kind in ("directory", "file"):
                preserved = link / "precious" if kind == "directory" else link
                assert preserved.read_text() == "preserve me\n"
                assert not link.is_symlink()
            else:
                assert link.resolve() == (tree / "src").resolve()
                # Reapplying retains the same source and header contents.
                before = header.read_bytes()
                subprocess.run(["sh", str(patch)], cwd=tree, env=env,
                               capture_output=True, check=True)
                assert header.read_bytes() == before
    print("  kernel-glue 8 of 8", flush=True)
    if os.environ.get("TEST_TALLY"):
        with open(os.environ["TEST_TALLY"], "a") as tally:
            tally.write("kernel-glue 8 8\n")
    executable = str(Path(work) / "core-state")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                    "-x", "c", "-", "-o", executable], input=source, text=True, check=True)
    subprocess.run([executable], check=True)
    # The existing kit lane now also checks real pixel stores without a GPU,
    # DRM device, module load, or writable prepared kernel tree.
    if os.uname().sysname == "Linux":
        inputs = canvas_sources(Path(work), os.uname().machine)
        executable = str(Path(work) / "canvas-cells")
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-fno-builtin",
                        "-DMOONWATER_FREESTANDING_ASM", "-I", work,
                        *inputs, "-o", executable], check=True)
        subprocess.run([executable], check=True)
