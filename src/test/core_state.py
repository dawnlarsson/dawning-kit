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
console = (root / "src/canvas/console.c").read_text()
drag = (root / "src/canvas/drag.c").read_text()
output = (root / "src/canvas/output.c").read_text()
pointer = (root / "src/canvas/pointer.c").read_text()
keys = (root / "src/canvas/keys.c").read_text()
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
    cells += section(canvas, "enum\n{", "/*\n        A pane")
    cells += section(paint, "static const u32 canvas_ink", "/*\n        A bitmap,")
    cells += section(paint, "static void bits_draw", "// xrgb8888")
    cells += section(text, "static const struct font_desc", "/*\n        Where one line ends.")
    cells += section(compose, "static void cell_draw", "/*\n        A window made of text.\n\n        The rows")
    (work / "canvas-cells.inc").write_text(cells)
    (work / "canvas-ring.inc").write_text(section(compose, "static void compose_cells", "/*\n        A pane, in target coordinates."))
    geometry = section(canvas, "static CONST _Bool rects_overlap", "// The border and titlebar")
    geometry += section(canvas, "static void pane_frame", "// Which edges")
    geometry += "#define compose_cells compose_cells_pixels\n"
    geometry += section(compose, "static void compose_cells", "/*\n        A pane, in target coordinates.")
    geometry += section(compose, "struct pane_bar_geometry", "/*\n        The desktop, everywhere")
    geometry += "#undef compose_cells\n"
    geometry += section(drag, "static void bar_move", "/*\n        Filling the screen")
    (work / "canvas-pane.inc").write_text(geometry)
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
#include <stddef.h>
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
#define string_length_max strnlen
#define memory_fill memset
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
#define clamp_t(t,a,b,c) clamp((t)(a),(t)(b),(t)(c))
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
static u64 mock_time=123;
static unsigned long ktime_get_ns(void) { return mock_time; }
static unsigned long ktime_get_real_seconds(void) { return 456; }
static unsigned long ktime_get_boottime_ns(void) { return 789; }
static void check(int okay, const char *name) {
    checks++;
    if (!okay && ++failures <= 12) fprintf(stderr, "FAIL %s\n", name);
}
'''
source += spark.replace('#include "platform/spark.inc"',
                        (root / "src/platform/spark.inc").read_text())
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
static int canvas_title=24, canvas_border=2, canvas_cell_w=8, canvas_cell_h=16, canvas_bar=10;
struct pane { unsigned display,style,max_width,max_height; int width,height,x,y,edge;
    int saved_x,saved_y,saved_w,saved_h; unsigned saved_display; _Bool maximized;
    unsigned columns,rows,max_columns,max_rows,grid_columns,grid_rows,damage_row,damage_rows;
    void *cells; int wait;
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
static unsigned regrids,wakes;
static void console_regrid(struct pane *p) { (void)p;regrids++; }
static void wake_up_interruptible(int *wait) { (void)wait;wakes++; }
'''
source += section(pane, "static void pane_regrid", "static void pane_refresh")
source += r'''
static void pane_limits(struct pane *p,int *w,int *h) { *w=p->max_width; *h=p->max_height; }
static void pane_reshape(struct pane *p,int x,int y,int w,int h) {
    p->x=x; p->y=y; p->width=w; p->height=h;
    pane_regrid(p);
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
source += section(compose, "static void target_rectangle", "/*\n        The run of one row")
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
struct file { int unused; };
static struct spawn *spawn_request;
static const char *spawn_path;
static int spawn_descriptors[3], spawn_shell, spawn_calls;
static long do_spawn(struct file *file, struct spawn *request, _Bool shell,
                     const char *path, int input, int output, int error) {
    (void)file; spawn_calls++; spawn_request=request; spawn_shell=shell; spawn_path=path;
    spawn_descriptors[0]=input; spawn_descriptors[1]=output; spawn_descriptors[2]=error;
    return 321;
}
static long report_stats(struct stats *out) { (void)out; return 322; }
'''
source += section(core, "static long device_ioctl", "/*\n        misc_open")
source += r'''
static void check_spawn_dispatch(void) {
    _Static_assert(sizeof(struct spawn)==48 && sizeof(struct spawn_to)==56 &&
                   sizeof(struct spawn_into)==64, "spawn ioctl encoded sizes");
    _Static_assert(offsetof(struct spawn_to,output)==sizeof(struct spawn) &&
                   offsetof(struct spawn_into,input)==sizeof(struct spawn), "spawn descriptor prefix");
    const unsigned commands[]={SPARK_IOCTL_SPAWN,SPARK_IOCTL_SPAWN_SHELL,
        SPARK_IOCTL_SPAWN_TOOL,SPARK_IOCTL_SPAWN_SHELL_INTO,SPARK_IOCTL_SPAWN_TOOL_TO};
    struct spawn_into into={.input=0,.output=INT_MAX,.error=-9};
    struct spawn_to to={.output=-9,.error=0};
    for (unsigned i=0;i<5;i++) for (unsigned fail=0;fail<2;fail++) {
        void *request=i==4?(void *)&to:(void *)&into;
        int rejected=i>=3 && fail;
        spawn_calls=0; copies=0; fail_copy=fail;
        check(device_ioctl(NULL,commands[i],(unsigned long)request)==(rejected?-EFAULT:321),
              "spawn dispatch and descriptor-copy failure");
        check(spawn_calls==!rejected && copies==(i>=3),"spawn copies only its descriptor forms");
        if (!rejected) {
            check(spawn_request==request && spawn_shell==(i==1 || i==3) &&
                  (i==2 || i==4 ? spawn_path && !strcmp(spawn_path,"/shell") : !spawn_path),
                  "spawn entry prefix and interpretation policy");
            check(spawn_descriptors[0]==(i==3?0:-1) &&
                  spawn_descriptors[1]==(i==3?INT_MAX:i==4?-9:-1) &&
                  spawn_descriptors[2]==(i==3?-9:i==4?0:-1),"spawn keeps descriptor order and bits");
        }
    }
    check(device_ioctl(NULL,0,0)==-ENOTTY,"unknown device ioctl remains rejected");
    fail_copy=0;
}
'''
source += r'''
#define COLD
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTCTRL 29
#define KEY_RIGHTCTRL 97
#define KEY_LEFTALT 56
#define KEY_RIGHTALT 100
#define KEY_TAB 15
#define KEY_F9 67
#define WINDOW_KEYS 64
#define WINDOW_KEY_DOWN 1u
#define WINDOW_KEY_SHIFT 2u
#define WINDOW_KEY_CONTROL 4u
#define WINDOW_KEY_ALT 8u
struct window_key { unsigned code,character,flags,reserved; };
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define SYN_REPORT 0
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define REL_WHEEL_HI_RES 11
#define BTN_LEFT 272
#define BTN_TOUCH 330
#define ABS_X 0
#define ABS_Y 1
#define NSEC_PER_MSEC 1000000ull
typedef int atomic_t;
struct input_absinfo { int minimum,maximum; };
struct input_dev { struct input_absinfo absinfo[2]; unsigned long relbit[1]; };
struct input_handle { struct input_dev *dev; };
static struct {
    int width,height,abs_x,abs_y,raw_x,raw_y,accel_x,accel_y;
    unsigned abs_have;
    atomic_t pending_x,pending_y,motion_pending,shake_dir,shake_count,magnify,wheel;
    atomic_t button_x,button_y,button_down,button_changed;
    atomic_t modifiers,key_head,key_tail,focus_steps,focus_commit,focus_cycling,minimize;
    struct window_key key_ring[WINDOW_KEYS];
    int input_lock;
    u64 accel_stamp,motion_stamp,shake_window;
} desktop;
static unsigned long pointer_counts,pointer_moved;
static unsigned wakes,wheel_cas,drain_race;
static int atomic_read(const atomic_t *p) { return *p; }
static void atomic_set(atomic_t *p,int value) { *p=value; }
static int atomic_xchg(atomic_t *p,int value) { int old=*p;*p=value;return old; }
static int atomic_inc_return(atomic_t *p) { return ++*p; }
static _Bool atomic_try_cmpxchg(atomic_t *p,int *old,int value) {
    wheel_cas++;
    if (drain_race) { drain_race=0;*p=0; }
    if (*p!=*old) { *old=*p;return 0; }
    *p=value;return 1;
}
static int test_bit(unsigned bit,const unsigned long *bits) { return (bits[bit/64]>>(bit%64))&1; }
static void canvas_thread_wake(void) { wakes++; }
static void atomic_fetch_add(int value,atomic_t *at) { *at+=value; }
#define smp_wmb() ((void)0)
#define spin_lock_irqsave(lock,flags) do { (flags)=0;mutex_lock(lock); } while(0)
#define spin_unlock_irqrestore(lock,flags) do { (void)(flags);mutex_unlock(lock); } while(0)
struct pointer_handle;
struct key_link { struct pointer_handle *owner; };
struct pointer_handle {
    struct input_handle handle;
    struct pointer_handle *next;
    struct key_link link;
    int reopen,opened;
    unsigned modifiers;
};
static struct pointer_handle *pointer_handles;
static unsigned closed;
#define container_of(p,type,member) ((type *)(p))
#undef list_for_each_entry
#define list_for_each_entry(p,list,link) for(p=pointer_handles;p;p=p->next)
static void list_del(struct key_link *link) {
    struct pointer_handle **at=&pointer_handles;
    while(*at!=link->owner) { assert(*at);at=&(*at)->next; }
    *at=(*at)->next;
}
static void cancel_delayed_work_sync(int *work) { (void)work; }
static void input_close_device(struct input_handle *handle) {
    (void)handle;assert(!desktop.input_lock);closed++;
}
static void input_unregister_handle(struct input_handle *handle) { (void)handle; }
#define kfree free
static u64 div_u64(u64 a,u32 b) { return a/b; }
static u64 div64_u64(u64 a,u64 b) { assert(b);return a/b; }
static unsigned long int_sqrt(unsigned long value) {
    unsigned long root=0,bit=1ul<<62;
    while (bit>value) bit>>=2;
    while (bit) {
        if (value>=root+bit) {value-=root+bit;root=(root>>1)+bit;}
        else root>>=1;
        bit>>=2;
    }
    return root;
}
'''
source += section(keys, "#define KEY_TABLE", "// Under desktop.lock")
source += section(pointer, "static inline struct pointer_handle *pointer_handle_of", "static HOT void pointer_event")
source += section(pointer, "static COLD void pointer_disconnect", "static void canvas_input_devices")
source += section(drag, "#define WHEEL_LINES", "static void wheel_deliver")
source += section(pointer, "#define ACCEL_ONE", "static void desktop_confine_cursor")
source += section(pointer, "static void pointer_commit", "#define POINTER_OPEN_TRIES")
# Run the real owned-pane release after printk has drained its callbacks.
source += r'''
struct console_test_pane { int link; unsigned long bytes; void *mapping; };
static struct console_test_pane *console_pane;
static struct { int lock; } console_desktop;
static int console_registered,canvas_console,console_callbacks,console_listed,console_frees;
static unsigned long canvas_pane_bytes;
static void unregister_console(int *console) {
    (void)console;assert(console_pane && console_registered);console_callbacks=0;
}
static void console_list_del(int *link) {
    assert(*link && console_desktop.lock);*link=0;console_listed--;
}
static void console_vfree(void *mapping) {
    assert(!console_callbacks && !console_registered && !console_pane &&
           !console_listed && console_desktop.lock);
    console_frees++;free(mapping);
}
#define pane console_test_pane
#define desktop console_desktop
#define list_del console_list_del
#define vfree console_vfree
'''
source += section(pane, "static void pane_free", "static void desktop_grid(")
source += console[console.index("static void console_stop(void)"): ]
source += r'''
#undef pane
#undef desktop
#undef list_del
#undef vfree
static void check_console_teardown(void) {
    for (unsigned run=0;run<2;run++) {
        console_pane=malloc(sizeof(*console_pane));assert(console_pane);
        *console_pane=(struct console_test_pane){1,4096,malloc(4096)};
        assert(console_pane->mapping);
        console_registered=console_callbacks=console_listed=1;canvas_pane_bytes=4096;
        console_stop();
        check(!console_pane && !canvas_pane_bytes && console_frees==(int)run+1,
              "console release returns its owned pane and ring budget");
        console_stop();
        check(console_frees==(int)run+1 && !console_desktop.lock,
              "console release is idempotent");
    }
}
'''
source += r'''
static struct pointer_handle *keyboard_attach(int opened) {
    struct pointer_handle *p=calloc(1,sizeof(*p));assert(p);
    p->link.owner=p;p->opened=opened;p->next=pointer_handles;pointer_handles=p;
    return p;
}
static void keyboard_send(struct pointer_handle *p,unsigned code,int value) {
    mutex_lock(&desktop.input_lock);
    pointer_event_locked(&p->handle,EV_KEY,code,value);
    mutex_unlock(&desktop.input_lock);
}
static void check_keyboard_state(void) {
    const unsigned pairs[][2]={{KEY_LEFTSHIFT,KEY_RIGHTSHIFT},
                              {KEY_LEFTCTRL,KEY_RIGHTCTRL},{KEY_LEFTALT,KEY_RIGHTALT}};
    for(unsigned family=0;family<3;family++)for(unsigned order=0;order<2;order++)
    for(unsigned release=0;release<2;release++)for(unsigned split=0;split<2;split++) {
        memset(&desktop,0,sizeof(desktop));closed=0;
        struct pointer_handle *a=keyboard_attach(0),*b=keyboard_attach(-EIO);
        struct pointer_handle *devices[]={a,split?b:a};
        unsigned codes[]={pairs[family][order],pairs[family][!order]},flag=2u<<family;
        keyboard_send(devices[0],codes[0],1);
        keyboard_send(devices[1],codes[1],1);
        for(unsigned repeat=0;repeat<4;repeat++)keyboard_send(devices[repeat%2],codes[repeat%2],2);
        if(family==2)keyboard_send(a,KEY_TAB,1);
        check((unsigned)desktop.modifiers==flag,"both modifier sides and repeats");
        keyboard_send(devices[release],codes[release],0);
        keyboard_send(devices[release],codes[release],0);
        check((unsigned)desktop.modifiers==flag,"one side and duplicate release preserve other");
        check(family!=2 || (desktop.focus_cycling && !desktop.focus_commit),
              "first Alt release preserves traversal");
        keyboard_send(a,30,1);
        if(family!=2) {
            struct window_key *key=&desktop.key_ring[(desktop.key_head-1)%WINDOW_KEYS];
            check(key->flags==(flag|WINDOW_KEY_DOWN) && key->character==(family?1u:'A'),
                  "surviving modifier changes delivered character and flags");
        }
        keyboard_send(devices[!release],codes[!release],0);
        check(!desktop.modifiers,"last modifier side releases");
        check(family!=2 || (!desktop.focus_cycling && desktop.focus_commit),
              "last Alt release commits traversal");
        pointer_disconnect(&a->handle);pointer_disconnect(&b->handle);
        check(!pointer_handles && closed==1,"disconnect closes only opened devices");
    }
    for(unsigned family=0;family<3;family++) {
        memset(&desktop,0,sizeof(desktop));
        struct pointer_handle *a=keyboard_attach(0),*b=keyboard_attach(0);
        keyboard_send(a,pairs[family][0],2); // A repeat also establishes held state.
        keyboard_send(b,pairs[family][0],1);
        if(family==2)keyboard_send(a,KEY_TAB,1);
        pointer_disconnect(&a->handle);
        check((unsigned)desktop.modifiers==(2u<<family),"disconnect preserves same key on another device");
        check(family!=2 || (desktop.focus_cycling && !desktop.focus_commit),
              "disconnect preserves another keyboard's Alt traversal");
        pointer_disconnect(&b->handle);
        check(!desktop.modifiers && !pointer_handles,"last disconnect releases held modifiers");
        check(family!=2 || (!desktop.focus_cycling && desktop.focus_commit),
              "last keyboard disconnect commits Alt traversal");
        a=keyboard_attach(0);keyboard_send(a,30,1);
        struct window_key *key=&desktop.key_ring[(desktop.key_head-1)%WINDOW_KEYS];
        check(key->character=='a' && key->flags==WINDOW_KEY_DOWN,"reconnected keyboard has no stale modifiers");
        pointer_disconnect(&a->handle);
    }
}
static int reference_int(s64 value) {
    return value<INT_MIN?INT_MIN:value>INT_MAX?INT_MAX:(int)value;
}
static u64 reference_sqrt(u64 value) {
    u64 low=0,high=1ull<<32;
    while (low+1<high) {
        u64 middle=low+(high-low)/2;
        if (middle*middle>value) high=middle;else low=middle;
    }
    return low;
}
static int reference_gain(u64 speed) {
    return speed<=1?1024:speed>=8?2560:1024+(2560-1024)*(int)(speed-1)/7;
}
static int reference_accel(int delta,int *remainder,int gain) {
    s64 scaled=(s64)delta*gain+*remainder;
    *remainder=(int)(scaled%1024);
    return reference_int(scaled/1024);
}
static void check_pointer_state(struct input_handle *handle) {
    static const int values[]={INT_MIN,INT_MIN+1,-1048576,-32768,-1025,-1,0,
                               1,1023,32767,1048576,INT_MAX-1,INT_MAX};
    static const int fractions[]={-1023,-513,-1,0,1,511,1023};
    static const u64 intervals[]={0,1,999999,1000000,2000000,4294967295ull,
                                  4294967296ull,4294967297ull,5000000000ull,
                                  60000000000ull};
    static const int dimensions[]={1,3840,INT_MAX};
    for(unsigned d=0;d<13;d++)for(unsigned speed=0;speed<=8;speed++)
    for(unsigned r=0;r<7;r++) {
        int have=fractions[r],want=have,gain=reference_gain(speed);
        int expected=reference_accel(values[d],&want,gain);
        pointer_counts=pointer_moved=0;
        check(accel_apply(values[d],&have,gain)==expected && have==want,
              "relative acceleration and signed remainder");
        check(pointer_counts==(u64)(values[d]<0?-(s64)values[d]:values[d]) &&
              pointer_moved==(u64)(expected<0?-(s64)expected:expected),
              "relative INT_MIN counter magnitude");
    }
    for(unsigned a=0;a<2;a++)for(unsigned initial=0;initial<13;initial++)
    for(unsigned event=0;event<13;event++) {
        memset(&desktop,0,sizeof(desktop));
        int expected=values[initial];
        int *raw=a?&desktop.raw_y:&desktop.raw_x;
        *raw=expected;
        for(unsigned repeat=0;repeat<16;repeat++) {
            expected=reference_int((s64)expected+values[event]);
            pointer_event_locked(handle,EV_REL,a?REL_Y:REL_X,values[event]);
            check(*raw==expected && !(a?desktop.raw_x:desktop.raw_y),
                  "relative repeated extreme accumulation and axis ownership");
        }
    }
    for(unsigned initial=0;initial<13;initial++)for(unsigned event=0;event<13;event++)
    for(unsigned fine=0;fine<2;fine++)for(unsigned capable=0;capable<2;capable++)
    for(unsigned race=0;race<2;race++) {
        desktop.wheel=values[initial];wakes=wheel_cas=0;drain_race=race;
        handle->dev->relbit[0]=(unsigned long)capable<<REL_WHEEL_HI_RES;
        int expected=values[initial];
        for(unsigned repeat=0;repeat<8;repeat++) {
            unsigned calls=wheel_cas;
            if (fine || !capable) {
                if (!repeat && race) expected=0;
                expected=reference_int((s64)expected+(s64)values[event]*(fine?1:120));
            }
            pointer_event_locked(handle,EV_REL,fine?REL_WHEEL_HI_RES:REL_WHEEL,values[event]);
            check(desktop.wheel==expected,"wheel saturated repeated distance");
            check(fine || !capable ? wheel_cas>calls && wakes==repeat+1 && !drain_race
                                   : wheel_cas==calls && !wakes,
                  "wheel concurrent drain retry and coarse duplicate suppression");
        }
        int remainder=fractions[event%7]%120;
        s64 scaled=(s64)expected*3+remainder;
        check(wheel_lines(expected,&remainder)==scaled/120 && remainder==scaled%120,
              "wheel saturated distance delivery keeps fraction");
    }
    handle->dev->relbit[0]=0;drain_race=0;
    for(unsigned x=0;x<13;x++)for(unsigned y=0;y<13;y++)
    for(unsigned delay=0;delay<10;delay++)for(unsigned geometry=0;geometry<3;geometry++)
    for(unsigned position=0;position<3;position++) {
        memset(&desktop,0,sizeof(desktop));
        desktop.width=desktop.height=dimensions[geometry];
        int px=position==0?0:position==1?desktop.width/2:desktop.width-1;
        int py=desktop.height-1-px;
        desktop.pending_x=px;desktop.pending_y=py;
        desktop.raw_x=values[x];desktop.raw_y=values[y];
        int rx=desktop.accel_x=fractions[x%7],ry=desktop.accel_y=fractions[y%7];
        desktop.accel_stamp=123;mock_time=123+intervals[delay];
        u64 squares=(u64)((s64)values[x]*values[x])+(u64)((s64)values[y]*values[y]);
        u64 interval=intervals[delay]<1000000?1000000:intervals[delay];
        int gain=reference_gain(reference_sqrt(squares)*1000000/interval);
        int dx=reference_accel(values[x],&rx,gain),dy=reference_accel(values[y],&ry,gain);
        s64 nx=(s64)px+dx,ny=(s64)py+dy;
        int ex=nx<0?0:nx>=desktop.width?desktop.width-1:(int)nx;
        int ey=ny<0?0:ny>=desktop.height?desktop.height-1:(int)ny;
        wakes=0;pointer_counts=pointer_moved=0;
        pointer_event_locked(handle,EV_SYN,SYN_REPORT,0);
        check(desktop.pending_x==ex && desktop.pending_y==ey &&
              desktop.accel_x==rx && desktop.accel_y==ry,
              "relative frame wide square, elapsed interval and cursor bounds");
        check(!desktop.raw_x && !desktop.raw_y &&
              wakes==(unsigned)(values[x]!=0 || values[y]!=0),
              "relative frame consumes one report and preserves empty frames");
    }
    // The old int arithmetic is defined for these small reports. Keep it as
    // a differential oracle across repeated reports and signed remainders.
    unsigned random=0x12345678;
    for(unsigned trial=0;trial<128;trial++) {
        memset(&desktop,0,sizeof(desktop));
        desktop.width=3840;desktop.height=2160;
        int px=desktop.pending_x=1920,py=desktop.pending_y=1080,rx=0,ry=0;
        mock_time=123;desktop.accel_stamp=mock_time;
        for(unsigned frame=0;frame<32;frame++) {
            int dx=0,dy=0;
            for(unsigned event=0;event<8;event++) {
                random=random*1664525+1013904223;
                int x=(int)((random>>16)%63)-31;
                int y=(int)((random>>24)%63)-31;
                dx+=x;dy+=y;
                pointer_event_locked(handle,EV_REL,REL_X,x);
                pointer_event_locked(handle,EV_REL,REL_Y,y);
            }
            u64 interval=(1+frame%17)*1000000ull;
            mock_time+=interval;
            int gain=reference_gain(reference_sqrt((unsigned)(dx*dx+dy*dy))*1000000/interval);
            rx+=dx*gain;ry+=dy*gain;
            int mx=rx/1024,my=ry/1024;rx-=mx*1024;ry-=my*1024;
            px=clamp(px+mx,0,3839);py=clamp(py+my,0,2159);
            pointer_event_locked(handle,EV_SYN,SYN_REPORT,0);
            check(desktop.pending_x==px && desktop.pending_y==py &&
                  desktop.accel_x==rx && desktop.accel_y==ry,
                  "relative normal bursts match former integer math");
        }
    }
    memset(&desktop,0,sizeof(desktop));mock_time=123;
    pointer_shake(INT_MIN);
    check(desktop.shake_dir==-1 && desktop.shake_count==1,"INT_MIN remains a negative shake");
    for(int small=-5;small<=5;small++)pointer_shake(small);
    check(desktop.shake_count==1,"shake ignores only subthreshold motion");
}

static void reset(void) {
    free(snapshot); snapshot=NULL; snapshot_room=0;
    allocations=fail_allocation=copies=fail_copy=0;
    cpu_records=network_records=growing=captures=0;
    assert(!snapshot_lock);
}
int main(void) {
    check_spawn_dispatch();
    check_console_teardown();
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
    for (unsigned w=0;w<80;w++)
    for (unsigned framed=0;framed<2;framed++) {
        canvas_title=20*scale; canvas_border=2*scale;
        canvas_cell_w=8*scale; canvas_cell_h=16*scale; canvas_bar=10*scale;
        screen.width=w; screen.height=h;
        unsigned columns,rows;
        desktop_grid(w,h,&columns,&rows);
        check(columns==(unsigned)max((int)w-14*(int)scale,0)/(8*scale) &&
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
    const int widths[]={0,1,9,10,17,18,19,25,26,27,639,640,641,3840};
    for (unsigned scale=1;scale<=3;scale++)
    for (unsigned shared=0;shared<2;shared++)
    for (unsigned w=0;w<sizeof(widths)/sizeof(*widths);w++) {
        canvas_cell_w=8*scale;canvas_cell_h=16*scale;canvas_bar=10*scale;
        struct pane page={0},p={.width=widths[w],.height=99,.cells=&page,
            .max_columns=80,.max_rows=24,.shared=shared?&page:NULL};
        unsigned columns=max(min(max(widths[w]-canvas_bar,0)/canvas_cell_w,80),1);
        regrids=wakes=0;pane_regrid(&p);
        check(p.columns==columns && p.width==(int)columns*canvas_cell_w+canvas_bar,
              "grid reserves gutter before rounding and clamps tiny widths");
        check(shared ? page.columns==columns && page.width==p.width && wakes==1 :
            p.grid_columns==columns && p.grid_rows==p.rows && regrids==1,
            "resized grid reaches client or owned console");
        int width=p.width,height=p.height;pane_regrid(&p);
        check(p.width==width && p.height==height && p.columns==columns,
              "gutter rounding is idempotent");
    }
    for (unsigned scale=1;scale<=3;scale++)for(unsigned framed=0;framed<2;framed++) {
        canvas_title=20*scale;canvas_border=2*scale;canvas_bar=10*scale;
        canvas_cell_w=8*scale;canvas_cell_h=16*scale;
        screen.width=1024;screen.height=768;
        struct pane p={.x=17,.y=19,.width=5*canvas_cell_w+canvas_bar,
            .height=3*canvas_cell_h,.cells=&screen,.max_columns=80,.max_rows=24,
            .max_width=80*canvas_cell_w+canvas_bar,.max_height=24*canvas_cell_h,
            .style=framed?WINDOW_FRAME:0};
        pane_maximize(&p,0,0);
        check(p.width<=(int)screen.width-(framed?2*canvas_border:0) &&
            p.width==(int)p.columns*canvas_cell_w+canvas_bar,
            "maximized grid and gutter fit within output borders");
        pane_maximize(&p,0,0);
        check(p.width==5*canvas_cell_w+canvas_bar && p.columns==5 && p.x==17 && p.y==19,
            "maximize restores the requested cells and gutter");
        p.style|=WINDOW_FULLSCREEN;pane_size(&p);pane_regrid(&p);
        check(p.width<=(int)screen.width && p.width==(int)p.columns*canvas_cell_w+canvas_bar,
            "fullscreen includes the gutter without covering a cell");
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
        pointer_event_locked(&handle,EV_ABS,axis,coordinates[value]);
        int expected=axis?456:123;
        if (high>low) expected=(int)(((uint64_t)((int64_t)clamp(coordinates[value],low,high)-low)*
                                      (axis?2160:3840))/((int64_t)high-low));
        check((axis?desktop.abs_y:desktop.abs_x)==expected &&
              (axis?desktop.abs_x:desktop.abs_y)==(axis?123:456) &&
              desktop.abs_have==(high>low?(1u<<axis):0),"absolute axis range and ownership");
    }
    check_pointer_state(&handle);
    check_keyboard_state();
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
