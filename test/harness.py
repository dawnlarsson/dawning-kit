#!/usr/bin/env python3
"""The standalone checks test/run calls by name.

    python3 test/harness.py core_state
    python3 test/harness.py spark_entry
    python3 test/harness.py build_tools
    python3 test/harness.py inventory
    python3 test/harness.py edit_driver SHELL
    python3 test/harness.py native_extract LIBRARY ROUTINE...
    python3 test/harness.py native_extract_test LIBRARY
    python3 test/harness.py surface_coreutils_gap
    python3 test/harness.py shell_functions --shell SHELL
    python3 test/harness.py audit_shell_functions
    python3 test/harness.py canvas_lifetime
    python3 test/harness.py code_map [-v]

Each was a file of its own in test/ -- core_state.py, spark_entry.py,
build_tools.py, inventory.py, edit_driver.py, native_extract.py with
native_extract_test.py as its self-check, surface_coreutils_gap.py with the
coreutils 9.11 program list it pinned, shell_functions.py,
audit_shell_functions.py, canvas_lifetime.py and code_map.py -- and each is
one function here, harness_<name>(argv), with the body it had. Nothing runs at import: the name
on the command line goes to harness_main, and that is the only thing
__main__ does.
"""

import argparse
import ast
import contextlib
import copy
import csv
import fcntl
import hashlib
import importlib.util
import io
import json
import os
import pathlib
from pathlib import Path
import platform
import pty
import re
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import textwrap
import time
import types
import unittest
from unittest.mock import patch

HARNESS_ROOT = Path(__file__).resolve().parents[1]

#       GNU coreutils 9.11's installed program list, which is what
#       test/surface_coreutils-9.11.json held: the denominator the shell's
#       dispatch tables are measured against.
HARNESS_COREUTILS_9_11 = {
    "package": "GNU coreutils",
    "version": "9.11",
    "source": {
        "url": "https://ftp.gnu.org/gnu/coreutils/coreutils-9.11.tar.xz",
        "file": "src/cu-progs.mk",
        "git_commit": "c01fd163a47468a8296fb369f5233853bb551bb6",
        "policy": "default__progs plus build_if_possible__progs; ginstall is install; libstdbuf.so, no_install__progs, and build_if_appropriate__progs are not installed applets"
    },
    "boundaries": {
        "factor": "native unsigned word: 0..18446744073709551615 on 64-bit builds; larger GNU bignum inputs are rejected without truncation"
    },
    "commands": [
        "[",
        "b2sum",
        "base32",
        "base64",
        "basename",
        "basenc",
        "cat",
        "chgrp",
        "chmod",
        "chown",
        "chroot",
        "cksum",
        "comm",
        "cp",
        "csplit",
        "cut",
        "date",
        "dd",
        "df",
        "dir",
        "dircolors",
        "dirname",
        "du",
        "echo",
        "env",
        "expand",
        "expr",
        "factor",
        "false",
        "fmt",
        "fold",
        "groups",
        "head",
        "hostid",
        "id",
        "install",
        "join",
        "link",
        "ln",
        "logname",
        "ls",
        "md5sum",
        "mkdir",
        "mkfifo",
        "mknod",
        "mktemp",
        "mv",
        "nice",
        "nl",
        "nohup",
        "nproc",
        "numfmt",
        "od",
        "paste",
        "pathchk",
        "pinky",
        "pr",
        "printenv",
        "printf",
        "ptx",
        "pwd",
        "readlink",
        "realpath",
        "rm",
        "rmdir",
        "seq",
        "sha1sum",
        "sha224sum",
        "sha256sum",
        "sha384sum",
        "sha512sum",
        "shred",
        "shuf",
        "sleep",
        "sort",
        "split",
        "stat",
        "stdbuf",
        "stty",
        "sum",
        "sync",
        "tac",
        "tail",
        "tee",
        "test",
        "timeout",
        "touch",
        "tr",
        "true",
        "truncate",
        "tsort",
        "tty",
        "uname",
        "unexpand",
        "uniq",
        "unlink",
        "users",
        "vdir",
        "wc",
        "who",
        "whoami",
        "yes"
    ],
    "absent": []
}


def harness_core_state(argv):
    """Kernel snapshot allocation and Canvas geometry, with syscall/DRM-free mocks."""
    root = HARNESS_ROOT
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
        inputs = ["-DCHECK_canvas_cells", str(root / "test/checks.c")]
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
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
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
    return 0


def harness_spark_entry(argv):
    """Actual Spark entry publication plus exhaustive kernel capability/state gates."""
    root = HARNESS_ROOT
    core = (root / "src/core.c").read_text()
    start = core.index("static unsigned long __ro_after_init spark_cpu_features;")
    features = core[start:core.index("#endif", start)]
    source = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define __ro_after_init
#define __init
#define XCR_XFEATURE_ENABLED_MASK 0
enum { X86_FEATURE_OSXSAVE, X86_FEATURE_AVX, X86_FEATURE_AVX2,
       X86_FEATURE_AVX512F, X86_FEATURE_AVX512BW, X86_FEATURE_AVX512VL,
       X86_FEATURE_AVX512VBMI, X86_FEATURE_FMA };
static unsigned capabilities, reads;
static u64 enabled;
#define cpu_feature_enabled(feature) ((capabilities >> (feature)) & 1)
static u64 xgetbv(unsigned index) { reads++; return enabled; }
'''
    source += '#include "' + str(root / "src/platform/spark.inc") + '"\n'
    source += features
    source += r'''
int main(void) {
    unsigned count=0;
    for (capabilities=0; capabilities<256; capabilities++)
    for (enabled=0; enabled<256; enabled++) {
        unsigned long expected=0;
        reads=0;
        spark_cpu_features=0;
        spark_cpu_features_start();
        if ((capabilities & 3) == 3 && (enabled & 6) == 6) {
            if (capabilities & 128) expected |= 0x1000000;
            if (capabilities & 4) {
                expected |= 1;
                if ((enabled & 0xe6) == 0xe6 && (capabilities & 56) == 56) {
                    expected |= 256;
                    if (capabilities & 64) expected |= 65536;
                }
            }
        }
        if (spark_cpu_features != expected || reads != ((capabilities & 3)==3))
            return 1;
        count++;
    }
    printf("spark kernel capability/state: %u of %u\n",count,count);
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="moonwater-spark-entry-") as temporary:
        work = Path(temporary)
        kernel = work / "kernel.c"
        kernel.write_text(source)
        compiler = os.environ.get("CC", "gcc")
        subprocess.run([compiler, "-O2", str(kernel), "-o", str(work / "kernel")], check=True)
        subprocess.run([str(work / "kernel")], check=True)
        if os.environ.get("TEST_TALLY"):
            with open(os.environ["TEST_TALLY"], "a") as tally:
                tally.write("spark-kernel-features 65536 65536\n")
        if platform.system() == "Linux" and platform.machine() in ("x86_64", "amd64"):
            binary = work / "entry"
            subprocess.run([compiler, "-O2", "-static", "-nostdlib", "-nostartfiles",
                            "-fno-stack-protector", "-fno-builtin", "-w",
                            "-T", str(root / "kit/spark.ld"), "-Wl,-e,spark_entry_probe",
                            "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments",
                            "-DCHECK_spark_entry", str(root / "test/checks.c"),
                            "-o", str(binary)], check=True)
            for mode in range(9):
                subprocess.run([str(binary), str(mode)], check=True)
            print("spark old/new/fallback entry: 9 of 9")
            if os.environ.get("TEST_TALLY"):
                with open(os.environ["TEST_TALLY"], "a") as tally:
                    tally.write("spark-entry 9 9\n")
        else:
            print("spark x86 entry: not run (requires native Linux x86-64)")
    return 0


class HarnessBuildTools(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="moonwater-build-")
        self.work = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        BUILD_FIXTURE=str(self.work), TERM="dumb")
        self.executable("cc", '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
if sys.argv[1:] == ["--version"]:
    print("gcc (GCC) fixture")
    raise SystemExit
with (root / "compiled").open("a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
out = pathlib.Path(sys.argv[sys.argv.index("-o") + 1])
out.write_text("#!/usr/bin/env python3\\n"
    "import os, pathlib, signal, time\\n"
    "root = pathlib.Path(os.environ['BUILD_FIXTURE'])\\n"
    "if 'BUILD_FIXTURE_EXIT' in os.environ: raise SystemExit(int(os.environ['BUILD_FIXTURE_EXIT']))\\n"
    "with (root / 'started').open('a') as f: f.write(str(os.getpid()) + '\\\\n')\\n"
    "signal.signal(signal.SIGTERM, lambda *_: exit(0))\\n"
    "while True: time.sleep(0.05)\\n")
''')
        self.env["CC"] = str(self.bin / "cc")

    def executable(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def invoke(self, *args):
        return subprocess.run(["sh", str(HARNESS_ROOT / "kit/build"), *map(str, args)],
                              cwd=self.work, env=self.env, capture_output=True,
                              text=True, timeout=10)

    def test_paths_preserve_spaces_globs_and_option_terminator(self):
        source = self.work / "source [literal] space.c"
        source.touch()
        output = self.work / "output [literal] space"
        result = self.invoke("--", source, output)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads((self.work / "compiled").read_text())
        self.assertEqual(args[0], str(source))
        self.assertEqual(args[args.index("-o") + 1], str(output))
        self.assertTrue(output.exists())

    def test_extra_operand_is_rejected_before_compiling(self):
        source = self.work / "source.c"
        source.touch()
        result = self.invoke(source, self.work / "out", "unexpected")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "compiled").exists())

    def test_run_uses_local_output_and_preserves_status(self):
        source = self.work / "source.c"
        source.touch()
        self.env["BUILD_FIXTURE_EXIT"] = "7"
        result = self.invoke("--run", source, "out")
        self.assertEqual(result.returncode, 7, result.stderr)

    def test_watch_reaps_latest_app_and_watcher_on_termination(self):
        source = self.work / "source.c"
        source.touch()
        self.executable("clear", "#!/bin/sh\nexit 0\n")
        self.executable("inotifywait", '''#!/usr/bin/env python3
import os, pathlib, time
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
(root / "watcher").write_text(str(os.getpid()))
time.sleep(0.1)
print("changed", flush=True)
while True: time.sleep(0.05)
''')
        process = subprocess.Popen(["sh", str(HARNESS_ROOT / "kit/build"), "--watch",
                                    str(source), str(self.work / "out")],
                                   cwd=self.work, env=self.env, start_new_session=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pids = []
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                started = self.work / "started"
                pids = list(map(int, started.read_text().split())) if started.exists() else []
                if len(pids) >= 2:
                    break
                time.sleep(0.02)
            self.assertGreaterEqual(len(pids), 2, "watch did not launch a replacement")
            process.terminate()
            process.wait(timeout=5)
            watcher = int((self.work / "watcher").read_text())
            for pid in [*pids, watcher]:
                with self.assertRaises(ProcessLookupError, msg=f"process {pid} survived build"):
                    os.kill(pid, 0)
        finally:
            # This disposable process group is ours, including the unfixed
            # pipeline watcher that would otherwise survive the test.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)

    def test_image_build_preserves_remote_paths_and_profile_arguments(self):
        remote = self.work / "remote ' $(printf injected) [glob]"
        (remote / "kit").mkdir(parents=True)
        (remote / "kit/common").write_text((HARNESS_ROOT / "kit/common").read_text())
        (remote / "artifacts").mkdir()
        (remote / "artifacts/.config").write_text("#> kernel_export dist/image space.efi\n")
        (remote / "dist").mkdir()
        (remote / "dist/image space.efi").write_bytes(b"image\x00bytes")
        for name in ("src", "kit"):
            (self.work / name).mkdir()
        (self.work / "build.sh").touch()
        self.env["MOONWATER_BUILD_DIR"] = str(remote)
        self.executable("ssh", '''#!/usr/bin/env python3
import os, subprocess, sys
raise SystemExit(subprocess.run(["/bin/sh", "-c", sys.argv[-1]], env=os.environ).returncode)
''')
        self.executable("rsync", '''#!/usr/bin/env python3
import os, subprocess, sys
for arg in sys.argv[1:]:
    if arg.startswith("--rsync-path="):
        raise SystemExit(subprocess.run(["/bin/sh", "-c", arg.split("=", 1)[1]], env=os.environ).returncode)
''')
        self.executable("sudo", '''#!/usr/bin/env python3
import json, os, pathlib, sys
(pathlib.Path(os.environ["BUILD_FIXTURE"]) / "remote-args").write_text(json.dumps(sys.argv[1:]))
''')
        profiles = ["arch/x64", "quote ' ; printf injected", "", "line\nend\n", "[glob]"]
        result = subprocess.run(["/bin/sh", str(HARNESS_ROOT / "build.sh"), profiles[0],
                                 "--host", "fixture", *profiles[1:]],
                                cwd=self.work, env=self.env, capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.work / "remote-args").read_text()),
                         ["env", "sh", "build.sh", *profiles])
        self.assertEqual((self.work / "dist/image space.efi").read_bytes(), b"image\x00bytes")

    def test_onbox_rejects_remote_path_code_before_contact(self):
        for name in ("rsync", "ssh"):
            self.executable(name, '#!/bin/sh\nprintf contacted >>"$BUILD_FIXTURE/contacted"\nexit 0\n')
        result = subprocess.run(["sh", str(HARNESS_ROOT / "kit/onbox"), "run; exit 0 #", "kit"],
                                cwd=HARNESS_ROOT, env=self.env, capture_output=True, text=True,
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.work / "contacted").exists())

    def test_onbox_quotes_remote_arguments_and_omits_agent_worktrees(self):
        for name in ("rsync", "ssh"):
            self.executable(name, '''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ["BUILD_FIXTURE"])
(root / pathlib.Path(sys.argv[0]).name).write_text(json.dumps(sys.argv[1:]))
''')
        hostile = "kit; printf injected 'bad'"
        result = subprocess.run(["sh", str(HARNESS_ROOT / "kit/onbox"), "audit-fixture", hostile],
                                cwd=HARNESS_ROOT, env=self.env, capture_output=True, text=True,
                                timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        copy = json.loads((self.work / "rsync").read_text())
        self.assertIn(".claude", copy)
        remote = json.loads((self.work / "ssh").read_text())[-1]
        # Execute only the final command with a stub sh: the literal hostile
        # argument must arrive intact and must never become shell syntax.
        command = remote.split("&&")[-1].strip()
        self.executable("sh", '''#!/usr/bin/env python3
import json, os, pathlib, sys
(pathlib.Path(os.environ["BUILD_FIXTURE"]) / "remote-args").write_text(json.dumps(sys.argv[1:]))
''')
        ran = subprocess.run(["/bin/sh", "-c", command], env=self.env,
                             capture_output=True, text=True, timeout=5)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "")
        self.assertEqual(json.loads((self.work / "remote-args").read_text()),
                         ["test/run", hostile])


def harness_build_tools(argv):
    """Exercise build argument boundaries and watch-process ownership without GCC."""
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HarnessBuildTools)
    return 0 if unittest.TextTestRunner().run(suite).wasSuccessful() else 1


def harness_inventory(argv):
    """Mutation tests for the raw zero-C/parity inventory gate."""
    ROOT = HARNESS_ROOT
    INVENTORY = ROOT / 'kit/compact/inventory.py'

    BASE = '''\
/* deliberately tiny input for the inventory generator */
#ifndef STANDARD_MODERN_C
#define STANDARD_MODERN_C
#if X64
ASM_FUNC(probe)
ASM_END(probe)
#elif ARM64
ASM_FUNC(probe)
ASM_END(probe)
#elif RISCV64
ASM_FUNC(probe)
ASM_END(probe)
#endif
#endif
'''

    checks = 0


    def invoke(path, *options):
        return subprocess.run(
            [sys.executable, str(INVENTORY), *options, str(path)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False)


    def expect(condition, message, result=None):
        nonlocal checks
        checks += 1
        if condition:
            return
        detail = '' if result is None else '\nstdout:\n%s\nstderr:\n%s' % (
            result.stdout, result.stderr)
        raise AssertionError(message + detail)


    def fresh(directory, name='library.c', source=BASE):
        path = directory / name
        path.write_text(source, encoding='utf-8')
        result = invoke(path, '--target', 'direct')
        expect(result.returncode == 0, 'clean write mode failed', result)
        return path


    def body_mutation(directory, label, source, expected):
        path = fresh(directory, label + '.c')
        path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                        encoding='utf-8')
        result = invoke(path, '--check', '--target', 'direct')
        expect(result.returncode != 0 and
               ('forbidden C body %s ' % expected) in result.stderr,
               '%s body evaded the lexical scan' % label, result)


    def object_mutation(directory, label, source, expected):
        path = fresh(directory, label + '.c')
        path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                        encoding='utf-8')
        result = invoke(path, '--check', '--target', 'direct')
        expect(result.returncode != 0 and
               ('forbidden C object %s ' % expected) in result.stderr,
               '%s object evaded the lexical scan' % label, result)


    def object_macro_mutation(directory, label, source, expected):
        path = fresh(directory, label + '.c')
        path.write_text(path.read_text(encoding='utf-8') + '\n' + source,
                        encoding='utf-8')
        result = invoke(path, '--check', '--target', 'direct')
        expect(result.returncode != 0 and
               ('object-generating macro %s ' % expected) in result.stderr,
               '%s object macro evaded the lexical scan' % label, result)


    with tempfile.TemporaryDirectory(prefix='inventory-test-') as temporary:
        directory = pathlib.Path(temporary)

        clean = fresh(directory)
        result = invoke(clean, '--check', '--target', 'direct')
        expect(result.returncode == 0, 'freshly generated inventory is stale', result)

        stale = directory / 'stale.c'
        stale.write_text(BASE, encoding='utf-8')
        result = invoke(stale, '--check', '--target', 'direct')
        expect(result.returncode != 0 and 'generated block is stale' in result.stderr,
               'check mode accepted a stale/missing generated block', result)

        body_mutation(directory, 'same-line',
                      'static int same_line(void) { return 1; }\n', 'same_line')
        body_mutation(directory, 'indented',
                      '    static int indented(void)\n    {\n        return 1;\n    }\n',
                      'indented')
        body_mutation(directory, 'prefix-attribute',
                      '__attribute__((used))\nstatic int prefix_attribute(void) { return 1; }\n',
                      'prefix_attribute')
        body_mutation(directory, 'suffix-attribute',
                      'static int suffix_attribute(void) __attribute__((used))\n'
                      '{\n    return 1;\n}\n',
                      'suffix_attribute')
        body_mutation(directory, 'suffix-attribute-macro',
                      '#define NOINLINE __attribute__((noinline))\n'
                      'static int suffix_attribute_macro(void) NOINLINE\n'
                      '{\n    return 1;\n}\n',
                      'suffix_attribute_macro')
        body_mutation(directory, 'c23-attribute',
                      '[[gnu::used]] static int c23_attribute(void) { return 1; }\n',
                      'c23_attribute')
        body_mutation(directory, 'multiline',
                      'static int\nmultiline(\n    int value\n)\n{\n    return value;\n}\n',
                      'multiline')
        body_mutation(directory, 'inactive',
                      '#if 0\nstatic int dormant(void) { return 1; }\n#endif\n',
                      'dormant')
        body_mutation(directory, 'conditional-attribute',
                      'static int conditional_attribute(void)\n'
                      '#if 0\n__attribute__((used))\n#endif\n'
                      '{\n    return 1;\n}\n',
                      'conditional_attribute')
        body_mutation(directory, 'pointer-return',
                      'static int (*pointer_return(void))(int)\n{\n    return 0;\n}\n',
                      'pointer_return')
        body_mutation(directory, 'old-style',
                      'int old_style(value)\nint value;\n'
                      '{\n    return value;\n}\n',
                      'old_style')
        body_mutation(directory, 'digraph-braces',
                      'static int digraph_braces(void) <% return 1; %>\n',
                      'digraph_braces')

        ignored = fresh(directory, 'ignored.c')
        ignored.write_text(ignored.read_text(encoding='utf-8') + r'''
/* static int comment_body(void) { return 1; } */
extern const char body_text[];
struct aggregate { int (*callback)(void); };
struct __attribute__((packed)) attributed_aggregate { int value; };
union forward_union;
enum forward_enum;
extern struct aggregate data, *data_pointer;
extern struct aggregate data_array[];
extern int (*external_callback)(void);
extern _Thread_local int external_tls;
extern int external_one, external_two[2], *external_three,
           (*external_four)(void), external_function(void);
extern int external_attribute __attribute__((visibility("hidden")));
extern int external_asm asm("renamed_external");
int prototype(void);
int *pointer_result(void);
int (*function_result(void))(int);
KEEP b32 retained_main();
typedef int integer_alias;
typedef int (*callback_alias)(void);
typedef struct aggregate aggregate_alias;
typedef struct local_tag { int member; } local_tag;
typedef enum { KIND_ZERO, KIND_ONE } kind_alias;
_Static_assert(sizeof(int) >= 2, "int width");
static_assert(1, "accepted spelling");
__asm__(".text\nnot_c: # { this is assembly text }\n");
#define ASM_BODY_TEXT "static int macro_string(void) { return 1; }"
#define ir(asm_args...) asm volatile(asm_args)
#define ASM(name) asm_x64_##name
#define str(string) (string), (sizeof(string) - 1)
#define WIDE_LENGTH(W, ZEROED, ZEROS, LEAVE) \
        "assembly" W ZEROED ZEROS LEAVE ASM_RET
#define syscall(name) syscall_linux_x64_##name
#define MUL(a, b) a * b
#define address_to *
#define TYPE_ALIAS file address_to
#define FUNCTION_DECL(name) int name(void)
#define EXTERN_DECL(name) extern int name
''' , encoding='utf-8')
        result = invoke(ignored, '--check', '--target', 'direct')
        expect(result.returncode == 0,
               'declarations, type/tag definitions, expressions, or global asm '
               'looked like C storage',
               result)

        object_mutation(directory, 'initialized-object',
                        'int initialized_object = 1;\n', 'initialized_object')
        object_mutation(directory, 'tentative-object',
                        'positive tentative_object;\n', 'tentative_object')
        object_mutation(directory, 'static-object',
                        'static int static_object;\n', 'static_object')
        object_mutation(directory, 'const-array',
                        'const char const_array[] = { 1, 2 };\n', 'const_array')
        object_mutation(directory, 'tentative-array',
                        'int tentative_array[];\n', 'tentative_array')
        object_mutation(directory, 'pointer-object',
                        'object_type *pointer_object;\n', 'pointer_object')
        object_mutation(directory, 'function-pointer',
                        'int (*function_pointer)(void);\n', 'function_pointer')
        object_mutation(directory, 'function-pointer-array',
                        'int (*function_pointer_array[2])(void);\n',
                        'function_pointer_array')
        object_mutation(directory, 'thread-local',
                        '_Thread_local int thread_local_object;\n',
                        'thread_local_object')
        object_mutation(directory, 'c23-thread-local',
                        'thread_local int c23_thread_local;\n', 'c23_thread_local')
        object_mutation(directory, 'extern-initializer',
                        'extern int extern_definition = 1;\n', 'extern_definition')
        object_mutation(directory, 'extern-comma-mix',
                        'extern int declaration_only, comma_definition = 1, '
                        'also_declaration_only;\n', 'comma_definition')
        object_mutation(directory, 'function-object-comma-mix',
                        'static int function_declaration(void), mixed_object;\n',
                        'mixed_object')
        object_mutation(directory, 'tagged-aggregate-object',
                        'struct record { int value; } aggregate_object;\n',
                        'aggregate_object')
        object_mutation(directory, 'anonymous-enum-object',
                        'enum { STATE_ZERO, STATE_ONE } enum_object;\n',
                        'enum_object')
        object_mutation(directory, 'compound-literal-initializer',
                        'struct point compound_object = (struct point) { 0 };\n',
                        'compound_object')
        object_mutation(directory, 'attributed-object',
                        '[[gnu::used]] static int attributed_object;\n',
                        'attributed_object')
        object_mutation(directory, 'suffix-attributed-object',
                        'static int suffix_attributed_object '
                        '__attribute__((used));\n', 'suffix_attributed_object')
        object_mutation(directory, 'typeof-object',
                        'typeof(0) typeof_object;\n', 'typeof_object')
        object_mutation(directory, 'declspec-object',
                        '__declspec(allocate("named")) int declspec_object;\n',
                        'declspec_object')
        object_mutation(directory, 'aligned-object',
                        '_Alignas(64) static int aligned_object;\n',
                        'aligned_object')
        object_mutation(directory, 'inactive-object',
                        '#if 0\nstatic int dormant_object;\n#endif\n',
                        'dormant_object')
        object_mutation(directory, 'digraph-object',
                        'static int digraph_object[] = <% 1, 2 %>;\n',
                        'digraph_object')

        object_macro_mutation(
            directory, 'local-var-macro',
            '#define local_var(name) static int name\nlocal_var(local_storage);\n',
            'local_var')
        object_macro_mutation(
            directory, 'conversion-constants-macro',
            '#define CONVERSION_CONSTANTS \\\n'
            '        static const unsigned conversion_constants[] = { 1, 2 }\n'
            'CONVERSION_CONSTANTS;\n', 'CONVERSION_CONSTANTS')
        object_macro_mutation(
            directory, 'callback-object-macro',
            '#define CALLBACK_OBJECT(name) static int (*name)(void)\n'
            'CALLBACK_OBJECT(callback_storage);\n', 'CALLBACK_OBJECT')
        object_macro_mutation(
            directory, 'custom-type-object-macro',
            '#define CUSTOM_OBJECT(name) object_type name\n'
            'CUSTOM_OBJECT(custom_storage);\n', 'CUSTOM_OBJECT')
        object_macro_mutation(
            directory, 'fixed-object-macro',
            '#define FIXED_OBJECT object_type fixed_storage\nFIXED_OBJECT;\n',
            'FIXED_OBJECT')
        object_macro_mutation(
            directory, 'extern-definition-macro',
            '#define EXTERN_DEFINITION(name) extern int name = 1\n'
            'EXTERN_DEFINITION(extern_storage);\n', 'EXTERN_DEFINITION')

        object_report = directory / 'object-report.c'
        object_report.write_text(BASE + 'static int reported_object;\n',
                                 encoding='utf-8')
        result = invoke(object_report, '--target', 'direct')
        report_text = object_report.read_text(encoding='utf-8')
        expect(result.returncode != 0 and
               'Raw C purity: 0 function bodies, 1 object definitions' in report_text and
               'C object definitions still present (forbidden):' in report_text and
               'reported_object' in report_text,
               'generated report did not account for forbidden C storage', result)

        write_body = directory / 'write-body.c'
        write_body.write_text(BASE + 'static int write_body(void) { return 1; }\n',
                             encoding='utf-8')
        result = invoke(write_body, '--target', 'direct')
        expect(result.returncode != 0 and 'forbidden C body write_body' in result.stderr,
               'write mode silently accepted a C body', result)

        macro_body = fresh(directory, 'macro-body.c')
        macro_body.write_text(macro_body.read_text(encoding='utf-8') + '''\
#define BODY_MACRO(name) \\
        static int name(void) { return 1; }
BODY_MACRO(generated)
''', encoding='utf-8')
        result = invoke(macro_body, '--check', '--target', 'direct')
        expect(result.returncode != 0 and
               'body-generating macro BODY_MACRO' in result.stderr,
               'an invoked body-generating macro evaded the raw zero-C gate', result)

        gap = directory / 'gap.c'
        gap.write_text(BASE.replace(
            '#elif RISCV64\nASM_FUNC(probe)\nASM_END(probe)\n', ''),
                       encoding='utf-8')
        result = invoke(gap, '--target', 'direct')
        expect(result.returncode != 0 and '1 not at parity' in result.stderr,
               'write mode silently accepted an architecture gap', result)

        duplicate = directory / 'duplicate.c'
        duplicate.write_text(BASE.replace(
            '#elif ARM64', 'ASM_FUNC(probe)\nASM_END(probe)\n#elif ARM64'),
                             encoding='utf-8')
        result = invoke(duplicate, '--target', 'direct')
        expect(result.returncode != 0 and
               'duplicate assembly function probe on X64' in result.stderr,
               'duplicate architecture body was hidden by the set inventory', result)

        unscoped = directory / 'unscoped.c'
        unscoped.write_text(BASE + 'ASM_FUNC(outside)\n', encoding='utf-8')
        result = invoke(unscoped, '--target', 'direct')
        expect(result.returncode != 0 and 'unscoped ASM_FUNC outside' in result.stderr,
               'unscoped assembly routine disappeared from parity accounting', result)

        kind_mismatch = directory / 'kind-mismatch.c'
        kind_mismatch.write_text(BASE.replace(
            '#elif ARM64\nASM_FUNC(probe)\nASM_END(probe)',
            '#elif ARM64\nASM_LOCAL_FUNC(probe)\nASM_LOCAL_END(probe)'),
            encoding='utf-8')
        result = invoke(kind_mismatch, '--target', 'direct')
        expect(result.returncode != 0 and
               'assembly function scope mismatch probe' in result.stderr,
               'public/local architecture mismatch evaded the audit', result)

        missing_end = directory / 'missing-end.c'
        missing_end.write_text(BASE.replace('ASM_END(probe)\n', '', 1),
                               encoding='utf-8')
        result = invoke(missing_end, '--target', 'direct')
        expect(result.returncode != 0 and 'has no matching end marker' in result.stderr,
               'missing assembly end marker evaded the audit', result)

        wrong_name = directory / 'wrong-end-name.c'
        wrong_name.write_text(BASE.replace('ASM_END(probe)', 'ASM_END(other)', 1),
                              encoding='utf-8')
        result = invoke(wrong_name, '--target', 'direct')
        expect(result.returncode != 0 and
               'ASM_END(other) does not close ASM_FUNC(probe)' in result.stderr,
               'wrong assembly end name evaded the audit', result)

        wrong_kind = directory / 'wrong-end-kind.c'
        wrong_kind.write_text(BASE.replace(
            'ASM_END(probe)', 'ASM_LOCAL_END(probe)', 1), encoding='utf-8')
        result = invoke(wrong_kind, '--target', 'direct')
        expect(result.returncode != 0 and
               'ASM_LOCAL_END(probe) does not close ASM_FUNC(probe)' in result.stderr,
               'wrong public/local end marker evaded the audit', result)

        orphan_end = directory / 'orphan-end.c'
        orphan_end.write_text(BASE.replace(
            'ASM_END(probe)\n#elif ARM64',
            'ASM_END(probe)\nASM_END(orphan)\n#elif ARM64'), encoding='utf-8')
        result = invoke(orphan_end, '--target', 'direct')
        expect(result.returncode != 0 and 'ASM_END(orphan) has no opener' in result.stderr,
               'orphan assembly end marker evaded the audit', result)

        overlap = directory / 'overlapping-functions.c'
        overlap.write_text(BASE.replace(
            'ASM_END(probe)\n#elif ARM64',
            'ASM_FUNC(second)\nASM_END(second)\n#elif ARM64'), encoding='utf-8')
        result = invoke(overlap, '--target', 'direct')
        expect(result.returncode != 0 and
               'ASM_FUNC(second) opens before ASM_FUNC(probe)' in result.stderr,
               'overlapping assembly function extents evaded the audit', result)

        directive_names = fresh(
            directory, 'directive-names.c',
            '#define ASM_LOCAL_FUNC(name) ignored_begin(name)\n'
            '#define ASM_LOCAL_END(name) ignored_end(name)\n' + BASE)
        result = invoke(directive_names, '--check', '--target', 'direct')
        expect(result.returncode == 0,
               'macro definitions themselves became assembly inventory entries', result)

        c_include = directory / 'c-include.c'
        c_include.write_text(BASE + '''\
#if 0
/* comments count as directive whitespace */ # inc\\
lude /* and here */ "renamed.C"
# include <directory//angle.c>
%:include "digraph.c"
#endif
''', encoding='utf-8')
        result = invoke(c_include, '--target', 'direct')
        expect(result.returncode != 0 and
               'forbidden .c include renamed.C' in result.stderr and
               'forbidden .c include directory//angle.c' in result.stderr and
               'forbidden .c include digraph.c' in result.stderr,
               'inactive/commented/spliced textual .c include evaded hygiene', result)

        runtime_root = directory / 'runtime-root.c'
        runtime_root.write_text(BASE + '#include "runtime.inc"\n', encoding='utf-8')
        runtime = directory / 'runtime.inc'
        runtime.write_text('''\
#if X64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#elif ARM64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#elif RISCV64
ASM_LOCAL_FUNC(runtime_probe)
ASM_LOCAL_END(runtime_probe)
#endif
''', encoding='utf-8')
        result = invoke(runtime_root, '--target', 'direct')
        expect(result.returncode == 0 and '2 routines, 0 not at parity' in result.stderr,
               'Linux include-graph assembly did not join the generated inventory', result)
        runtime.write_text(runtime.read_text(encoding='utf-8').replace(
            '#elif RISCV64\nASM_LOCAL_FUNC(runtime_probe)\n'
            'ASM_LOCAL_END(runtime_probe)\n', ''), encoding='utf-8')
        result = invoke(runtime_root, '--check', '--target', 'linux')
        expect(result.returncode != 0 and
               'assembly function runtime_probe missing on RISCV64' in result.stderr,
               'included runtime architecture gap evaded parity accounting', result)

        active_root = directory / 'active-root.c'
        active_root.write_text(BASE + '''\
#if defined(LINUX)
#include "first.inc"
#endif
''', encoding='utf-8')
        (directory / 'first.inc').write_text('#include "second.inc"\n', encoding='utf-8')
        (directory / 'second.inc').write_text(
            'static int nested_body(void) { return 1; }\n', encoding='utf-8')
        result = invoke(active_root, '--target', 'direct')
        expect(result.returncode == 0, 'could not generate active-include fixture', result)
        result = invoke(active_root, '--check', '--target', 'linux')
        expect(result.returncode != 0 and 'forbidden C body nested_body' in result.stderr,
               'body in a recursively included renamed .inc evaded the Linux gate', result)
        result = invoke(active_root, '--target', 'linux')
        expect(result.returncode != 0 and 'forbidden C body nested_body' in result.stderr,
               'write/rebuild mode silently accepted an included C body', result)

        object_graph_root = directory / 'object-graph-root.c'
        object_graph_root.write_text(BASE + '''\
#if defined(LINUX)
#include "object-first.inc"
#endif
''', encoding='utf-8')
        (directory / 'object-first.inc').write_text(
            '#include "object-second.inc"\n', encoding='utf-8')
        (directory / 'object-second.inc').write_text(
            'extern int declaration_only;\n'
            'static int nested_object;\n'
            '#define NESTED_OBJECT(name) static int name\n', encoding='utf-8')
        result = invoke(object_graph_root, '--target', 'direct')
        expect(result.returncode == 0,
               'could not generate recursive object fixture', result)
        result = invoke(object_graph_root, '--check', '--target', 'linux')
        expect(result.returncode != 0 and
               'forbidden C object nested_object' in result.stderr and
               'object-generating macro NESTED_OBJECT' in result.stderr,
               'object or object macro in a recursive .inc graph evaded the gate',
               result)
        result = invoke(object_graph_root, '--target', 'linux')
        expect(result.returncode != 0 and
               'forbidden C object nested_object' in result.stderr,
               'write/rebuild mode silently accepted included C storage', result)

        inactive_root = fresh(directory, 'inactive-root.c', BASE + '''\
#if defined(WINDOWS)
#include "windows.inc"
#endif
''')
        (directory / 'windows.inc').write_text(
            'static int windows_body(void) { return 1; }\n'
            'static int windows_object;\n', encoding='utf-8')
        result = invoke(inactive_root, '--check', '--target', 'linux')
        expect(result.returncode == 0,
               'Linux-active gate followed a Windows-inactive include', result)
        result = invoke(inactive_root, '--check', '--target', 'all')
        expect(result.returncode != 0 and
               'forbidden C body windows_body' in result.stderr and
               'forbidden C object windows_object' in result.stderr,
               'all-platform include audit missed inactive platform C', result)

        mac_root = directory / 'mac-root.c'
        mac_root.write_text(BASE + '''\
#if defined(MACOS)
#include "platform/macos.inc"
#endif
''', encoding='utf-8')
        platform = directory / 'platform'
        platform.mkdir()
        mac_runtime = platform / 'macos.inc'
        mac_runtime.write_text(r'''
#if defined(X64)
__asm__(
    ".globl _sleep\n" "_sleep:\n"
    ".globl _exit\n" "_exit:\n"
    ".globl _start\n" "_start:\n"
    ".globl __start\n" "__start:\n");
#elif defined(ARM64)
__asm__(
    ".globl _sleep\n" "_sleep:\n"
    ".globl _exit\n" "_exit:\n"
    ".globl _start\n" "_start:\n"
    ".globl __start\n" "__start:\n");
#endif
''', encoding='utf-8')
        result = invoke(mac_root, '--target', 'direct')
        expect(result.returncode == 0,
               'complete x64/ARM64 Mach-O runtime parity was rejected', result)
        mac_text = mac_runtime.read_text(encoding='utf-8')
        last_global = mac_text.rfind('".globl _exit\\n"')
        expect(last_global >= 0, 'Mach-O fixture did not contain its ARM64 exit global')
        mac_runtime.write_text(mac_text[:last_global] +
                               '".hidden _exit\\n"' +
                               mac_text[last_global + len('".globl _exit\\n"'):],
                               encoding='utf-8')
        result = invoke(mac_root, '--check', '--target', 'all')
        expect(result.returncode != 0 and
               'macOS ARM64 global _exit expected once, found 0' in result.stderr,
               'raw Mach-O ARM64 global parity gap evaded the audit', result)

        missing = fresh(directory, 'missing-root.c')
        missing.write_text(missing.read_text(encoding='utf-8') +
                           '#include "missing.inc"\n', encoding='utf-8')
        result = invoke(missing, '--check', '--target', 'linux')
        expect(result.returncode != 0 and 'missing quoted include missing.inc' in result.stderr,
               'missing local include silently weakened the recursive audit', result)

    print('inventory mutation audit: %d checks' % checks)
    return 0


def harness_edit_driver(argv):
    """Exercise the editor's real pty, resize, and atomic-save driver."""
    def fail(message):
        print(f"  driver       {message}")
        raise SystemExit(1)


    def read_until(master, process, marker, timeout=3.0):
        seen = bytearray()
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            ready, _, _ = select.select([master], [], [], 0.05)
            if ready:
                try:
                    seen.extend(os.read(master, 65536))
                except OSError:
                    break
                if marker in seen:
                    return bytes(seen)
            if process.poll() is not None:
                break
        return bytes(seen)


    def start(shell, path, initial_flags=False, umask=0o022):
        master, slave = pty.openpty()
        fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 12, 60, 0, 0))
        original = termios.tcgetattr(slave)
        if initial_flags:
            unusual = termios.tcgetattr(slave)
            unusual[0] |= termios.ISTRIP | termios.INLCR
            unusual[2] &= ~termios.CSIZE
            unusual[2] |= termios.CS7
            termios.tcsetattr(slave, termios.TCSANOW, unusual)
            # Linux ptys may normalize unsupported character sizes immediately.
            original = termios.tcgetattr(slave)

        def child_setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            os.umask(umask)

        command = "edit " + shlex.quote(path)
        process = subprocess.Popen(
            [shell, "-c", command], stdin=slave, stdout=slave, stderr=slave,
            close_fds=True, preexec_fn=child_setup,
        )
        shown = read_until(master, process, b"\x1b[?2004h")
        if b"\x1b[?2004h" not in shown:
            process.kill()
            process.wait()
            os.close(master)
            os.close(slave)
            fail("editor did not enter its terminal screen")
        return master, slave, process, original


    def finish(master, slave, process, keys, original=None):
        os.write(master, keys)
        read_until(master, process, b"\x1b[?1049l")
        try:
            result = process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            fail("editor did not leave after Ctrl+Q")
        if original is not None:
            restored = termios.tcgetattr(slave)
            if restored != original:
                changed = [str(at) for at, pair in enumerate(zip(original, restored))
                           if pair[0] != pair[1]]
                fail("terminal modes were not restored exactly (fields " +
                     ",".join(changed) + f", cflag {original[2]:x}->{restored[2]:x})")
        os.close(master)
        os.close(slave)
        if result:
            fail(f"editor exited with status {result}")


    def replace(shell, path, text, original=None):
        master, slave, process, before = start(shell, path,
                                                initial_flags=original is not None)
        if original is not None:
            raw = termios.tcgetattr(slave)
            if raw[0] & (termios.ISTRIP | termios.INLCR):
                fail("raw mode still transforms input bytes")
            if (raw[2] & termios.CSIZE) != termios.CS8:
                fail("raw mode is not eight-bit clean")
        finish(master, slave, process, b"\x01" + text + b"\x13\x11", before)

    if len(argv) != 1:
        fail("usage: harness.py edit_driver SHELL")
    shell = os.path.abspath(argv[0])
    passed = 0

    with tempfile.TemporaryDirectory(prefix="moonwater-edit-") as directory:
        target = os.path.join(directory, "target")
        link = os.path.join(directory, "link")
        with open(target, "wb") as handle:
            handle.write(b"old\n")
        os.chmod(target, 0o755)
        os.symlink("target", link)
        replace(shell, link, b"new")
        if not os.path.islink(link) or open(target, "rb").read() != b"new\n":
            fail("saving through a symlink replaced the link or missed its target")
        if os.stat(target).st_mode & 0o7777 != 0o755:
            fail("saving changed the existing file mode")
        passed += 3

        created = os.path.join(directory, "created")
        replace(shell, created, b"fresh")
        if open(created, "rb").read() != b"fresh\n":
            fail("new file bytes differ")
        if os.stat(created).st_mode & 0o777 != 0o644:
            fail("new file ignored umask 022")
        passed += 2

        unusual = os.path.join(directory, "unusual")
        with open(unusual, "wb") as handle:
            handle.write(b"x\n")
        replace(shell, unusual, b"eight", original=True)
        passed += 2

        master, slave, process, before = start(shell, target)
        # No input follows this resize. SIGWINCH alone must wake ppoll and
        # cause a redraw, including the new 50-column status line.
        fcntl.ioctl(master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 10, 50, 0, 0))
        redrawn = read_until(master, process, b"\x1b[10;1H", 2.0)
        if b"\x1b[10;1H" not in redrawn:
            process.kill()
            process.wait()
            fail("SIGWINCH did not redraw without a keypress")
        finish(master, slave, process, b"\x11", before)
        passed += 1

        leftovers = [name for name in os.listdir(directory)
                     if name.startswith(".moonwater-edit-")]
        if leftovers:
            fail("atomic save left temporary files behind")
        passed += 1

    print(f"  driver       {passed} of {passed}")
    return 0


def harness_native_extract(argv):
    """Lift an arm64 routine out of library.c so it can be run here.

        python3 test/harness.py native_extract <lib.c> <routine> [more...]

    Writes a C file to standard output holding each routine's body under a
    _<name> symbol, with the three prologue lines Darwin needs instead of
    ASM_FUNC. The body between the label and the ret is copied unchanged, so
    what runs here is what is in the file and not a transcription of it.

    Three things are spelled differently on Darwin and are rewritten rather
    than left to fail at assembly time. A call to another routine in the
    library names it without the leading underscore Mach-O gives every C
    symbol; the address of a global is built with @PAGE and @PAGEOFF rather
    than with :lo12:. Both appeared the first time a routine here called
    another one -- memory_search calls memory_compare, string_find calls
    three of them -- and without the rewrite the case does not link rather
    than failing a check, which is a confusing way to find out.

    A routine that names a table gets the table too: byte_commonness is lifted
    from its marker-delimited assembly object, converted from ELF to Mach-O, and
    checked before it is emitted.  That keeps the native case on the exact bytes
    the three production architectures index without putting a second C form of
    the table here.
    """
    lib, names = argv[0], list(argv[1:])
    lines = open(lib).read().split('\n')

    arch=[None]*len(lines); cur=None
    for i,l in enumerate(lines):
        m=re.match(r'^#(el)?if (X64|ARM64|RISCV64)\b', l.strip())
        if m: cur=m.group(2)
        arch[i]=cur

    #
    #       Mach-O spells a C symbol with a leading underscore and builds the
    #       address of a global out of @PAGE and @PAGEOFF. Branch targets that are
    #       numbers -- 1b, 4f -- are local labels and must be left alone, which is
    #       why the name has to start with a letter to be rewritten.
    #
    def darwin(line):
        # A table embedded inside a routine stays inside the extracted body. ELF
        # and Mach-O name the read-only section differently, and ASM_SECTION is a
        # C macro that does not come along with the literal lines being lifted.
        line = line.replace('.section .rodata', '.section __TEXT,__const')
        if line.strip() == 'ASM_SECTION':
            return '    ".text\\n"'

        # ELF temporary symbols begin .L; Mach-O's assembler-local spelling is L.
        # AArch64 conditional branches cannot carry an external relocation, so a
        # forward .L name left unchanged is rejected even though its definition is
        # present later in the same inline assembly block.
        line = line.replace('.L', 'L')

        line = re.sub(r'\b(bl|b) ([a-z_][a-z0-9_]*)\b', r'\1 _\2', line)
        line = re.sub(r'\badrp (x[0-9]+), ([a-z_][a-z0-9_]*)\b', r'adrp \1, _\2@PAGE', line)
        line = re.sub(r':lo12:([a-z_][a-z0-9_]*)\b', r'_\1@PAGEOFF', line)

        # Labels defined by the extracted inline assembly need the same Mach-O
        # spelling as the references above. Numeric and .L labels are local and
        # deliberately do not match this form.
        line = re.sub(r'^(\s*")([a-z_][a-z0-9_]*):', r'\1_\2:', line)
        return line

    OBJECTS = {
        # A checksum pins the ordering as well as the size.  The table is a
        # permutation, which is checked separately so a diagnostic says what was
        # structurally wrong instead of reporting only an opaque digest mismatch.
        'byte_commonness': {
            'size': 256,
            'alignment': 16,
            'sha256': '470d515b123842faff312a364f32931ba37079f317e184420eb2b6bc93c30efc',
            'permutation': True,
        },
    }

    def marked_object(name):
        """Read, verify, and Mach-O-convert one inline-assembly data object."""
        tag = name.upper()
        begin_tag = 'NATIVE_%s_BEGIN' % tag
        end_tag = 'NATIVE_%s_END' % tag
        begins = [i for i, line in enumerate(lines) if begin_tag in line]
        ends = [i for i, line in enumerate(lines) if end_tag in line]
        if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
            sys.exit('extract: need one ordered %s/%s marker pair in %s'
                     % (begin_tag, end_tag, lib))

        # The marker encloses a complete __asm__ object.  Decode its C string
        # tokens instead of interpreting formatting in library.c; adjacent string
        # literals and any number of .byte rows consequently have the same result.
        source = '\n'.join(lines[begins[0] + 1:ends[0]])
        tokens = re.findall(r'"(?:\\.|[^"\\])*"', source)
        try:
            assembly = ''.join(ast.literal_eval(token) for token in tokens)
        except (SyntaxError, ValueError) as error:
            sys.exit('extract: malformed C string in %s object: %s' % (name, error))
        if not assembly:
            sys.exit('extract: empty %s object between native markers' % name)

        # library.c keeps the payload literal only once and lets these two macros
        # give normal builds their target object spelling.  Expand that wrapper to
        # its canonical ELF form here, then pass it through the same explicit
        # ELF-to-Mach conversion below.  An older fully literal marked object is
        # accepted too, which makes malformed/missing macro wrappers diagnosable.
        if not re.search(r'(?m)^\s*' + re.escape(name) + r':\s*$', assembly):
            starts = re.findall(r'ASM_RODATA_OBJECT_BEGIN\(\s*' + re.escape(name)
                                + r'\s*,\s*([0-9]+)\s*\)', source)
            stops = re.findall(r'ASM_OBJECT_END\(\s*' + re.escape(name) + r'\s*\)',
                               source)
            if len(starts) != 1 or len(stops) != 1:
                sys.exit('extract: %s markers need one object begin/end macro pair'
                         % name)
            alignment = int(starts[0])
            if alignment != OBJECTS[name]['alignment']:
                sys.exit('extract: %s alignment %d, expected %d'
                         % (name, alignment, OBJECTS[name]['alignment']))
            assembly = ('.pushsection .rodata.%s,"a",%%progbits\n'
                        '.balign %d\n'
                        '.globl %s\n'
                        '.type %s, %%object\n'
                        '%s:\n%s'
                        '.size %s, .-%s\n'
                        '.popsection\n'
                        % (name, alignment, name, name, name, assembly, name, name))

        # Verify the bytes between the label and ELF size directive.  Only .byte
        # contributes data there: accepting a .word or .zero without accounting
        # for it would make the declared 256-byte table check meaningless.
        seen_label = False
        values = []
        for line in assembly.splitlines():
            stripped = line.strip()
            if stripped == name + ':':
                seen_label = True
                continue
            if not seen_label:
                continue
            if stripped.startswith('.size '):
                break
            byte = re.match(r'^\.byte\s+(.+)$', stripped)
            if byte:
                for value in byte.group(1).split(','):
                    try:
                        number = int(value.strip(), 0)
                    except ValueError:
                        sys.exit('extract: non-integer byte in %s: %s'
                                 % (name, value.strip()))
                    if number < 0 or number > 255:
                        sys.exit('extract: byte outside 0..255 in %s: %d'
                                 % (name, number))
                    values.append(number)
                continue
            if stripped and not stripped.startswith(('.p2align ', '.balign ',
                                                      '.popsection')):
                sys.exit('extract: unsupported data directive in %s: %s'
                         % (name, stripped))
        if not seen_label:
            sys.exit('extract: no %s label inside native markers' % name)

        spec = OBJECTS[name]
        if len(values) != spec['size']:
            sys.exit('extract: %s has %d bytes, expected %d'
                     % (name, len(values), spec['size']))
        if spec.get('permutation') and sorted(values) != list(range(spec['size'])):
            sys.exit('extract: %s is not a permutation of 0..%d'
                     % (name, spec['size'] - 1))
        digest = hashlib.sha256(bytes(values)).hexdigest()
        if digest != spec['sha256']:
            sys.exit('extract: %s checksum %s, expected %s'
                     % (name, digest, spec['sha256']))

        out = []
        for line in assembly.splitlines():
            stripped = line.strip()
            if stripped.startswith(('.type ', '.size ')):
                continue                    # ELF symbol metadata has no Mach-O form
            if re.match(r'^\.(?:push)?section\s+\.rodata(?:\.[^,\s]+)?(?:\s|,|$)',
                        stripped):
                line = re.sub(r'^\s*\.(?:push)?section.*$',
                              '.section __TEXT,__const', line)
            elif stripped == '.popsection':
                line = '.text'              # restore the section for following bodies
            line = re.sub(r'^(\s*\.(?:globl|global)\s+)' + re.escape(name) + r'\b',
                          r'\1_' + name, line)
            line = re.sub(r'^(\s*)' + re.escape(name) + r':',
                          r'\1_' + name + ':', line)
            out.append(line)

        converted = '\n'.join(out) + '\n'
        required = ('.section __TEXT,__const', '.globl _' + name, '_' + name + ':')
        for spelling in required:
            if spelling not in converted:
                sys.exit('extract: converted %s object lacks %s' % (name, spelling))
        if re.search(r'(?m)^\s*\.(?:type|size|pushsection|popsection)\b', converted):
            sys.exit('extract: ELF-only metadata remains in converted %s object' % name)
        if '%object' in converted or '%progbits' in converted:
            sys.exit('extract: ELF-only type spelling remains in converted %s object'
                     % name)
        return converted

    def emit_asm(assembly):
        """Print decoded assembly as a C top-level __asm__ declaration."""
        print('__asm__(')
        for line in assembly.splitlines():
            print('    ' + json.dumps(line + '\n'))
        print(');')

    def digit_pair_table():
        """The assembler digit table as a C symbol for lifted formatter leaves."""
        return ('const unsigned char digit_pairs[] = '
                '"00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";')

    def body(name):
        for i,l in enumerate(lines):
            if l.strip() in (f'ASM_FUNC({name})', f'ASM_LOCAL_FUNC({name})') and arch[i]=='ARM64':
                j=next(k for k in range(i,len(lines))
                       if lines[k].strip().startswith((f'ASM_END({name})',
                                                       f'ASM_LOCAL_END({name})')))
                out=[]
                for x in lines[i+1:j]:
                    if x.strip().startswith('//'): continue
                    indirect = re.fullmatch(r'\s*ASM_CALL\("(x[0-9]+)"\)', x)
                    if indirect:
                        out.append('    "blr %s\\n"' % indirect.group(1))
                        continue
                    out.append(darwin(x.replace('ASM_RET', '"ret\\n"')))
                return out
        sys.exit(f"extract: no arm64 {name} in {lib}")

    #
    #       A routine whose arm64 form is ASM_ALIAS(name, target) has no body of
    #       its own: .set makes it a second label on the target's address. That
    #       is lifted the same way -- the target's body comes along, and the alias
    #       is emitted as a .set after every body, which is what the library does
    #       and what keeps the native case on the bytes the file actually holds.
    #
    def alias_target(name):
        for i, l in enumerate(lines):
            m = re.fullmatch(r'\s*ASM_ALIAS\((\w+),\s*(\w+)\)\s*', l)
            if m and m.group(1) == name and arch[i] == 'ARM64':
                return m.group(2)
        return None

    aliases = {}
    for n in list(names):
        target = alias_target(n)
        if not target:
            continue
        aliases[n] = target
        if target not in names:
            names.append(target)
    for n, target in aliases.items():
        if target in aliases:
            sys.exit(f"extract: arm64 {n} aliases {target}, itself an alias")

    bodies = {n: body(n) for n in names if n not in aliases}

    print(f'// Lifted from {lib} by test/harness.py native_extract -- do not edit.')

    if any('byte_commonness' in l for b in bodies.values() for l in b):
        emit_asm(marked_object('byte_commonness'))
        print('extern const unsigned char byte_commonness[256];')

    if any('digit_pairs' in l for b in bodies.values() for l in b):
        print(digit_pair_table())


    # The arm64 bodies are written over macros -- one wide loop shared by every
    # hunt that has one -- so the macros come across too, or what is lifted does
    # not compile. A definition runs until a line that does not end in a backslash.
    i = 0
    while i < len(lines):
        if lines[i].startswith('#define NEON_') or lines[i].startswith('#define WIDE_'):
            while True:
                print(lines[i])
                if not lines[i].rstrip().endswith('\\'):
                    break
                i += 1
        i += 1

    for n in names:
        if n in aliases:
            continue
        print(f'__asm__(\n    ".globl _{n}\\n"\n    ".p2align 4\\n"\n    "_{n}:\\n"')
        for l in bodies[n]: print(l)
        print(');')

    for n, target in aliases.items():
        print(f'__asm__(\n    ".globl _{n}\\n"\n    ".set _{n}, _{target}\\n"\n);')
    return 0


def harness_native_extract_captured(argv):
    """The extractor run in this process, reported the way subprocess.run reports a child.

    sys.exit with a message is how the extractor refuses; a child would have
    printed that message on stderr and left with 1, and its self-check below
    reads exactly those two things.
    """
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            status = harness_native_extract(argv) or 0
        except SystemExit as leave:
            if isinstance(leave.code, str):
                print(leave.code, file=sys.stderr)
                status = 1
            else:
                status = leave.code or 0
    return types.SimpleNamespace(returncode=status, stdout=out.getvalue(), stderr=err.getvalue())


def harness_native_extract_test(argv):
    """Contract and corruption checks for the native assembly-object lifter."""
    library = pathlib.Path(argv[0])
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
        return harness_native_extract_captured(
            [str(candidate), 'memory_search',
             'memory_search_prepare', 'memory_search_prepared_core'])

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

    #
    #       An ASM_ALIAS in the arm64 block is a routine with no body of its own.
    #       The lifter has to bring the target's body and spell the alias as the
    #       .set the library uses; this broke once without anything saying so.
    #       Which name is an alias is read off the library rather than written
    #       here, so turning an alias back into a body does not fail this.
    #
    arch = None
    alias = None
    for line in source.split('\n'):
        if_arch = re.match(r'^#(?:el)?if (X64|ARM64|RISCV64)\b', line.strip())
        if if_arch:
            arch = if_arch.group(1)
        is_alias = re.fullmatch(r'\s*ASM_ALIAS\((\w+),\s*(\w+)\)\s*', line)
        if is_alias and arch == 'ARM64':
            alias = is_alias.groups()
            break

    if alias:
        name, target = alias
        result = harness_native_extract_captured([str(library), name])
        if result.returncode:
            sys.exit('native extract test: alias %s rejected: %s'
                     % (name, result.stderr.strip()))
        for required in ('"_%s:\\n"' % target,
                         '.set _%s, _%s' % (name, target)):
            if required not in result.stdout:
                sys.exit('native extract test: lifted alias %s lacks %s'
                         % (name, required))

    print('arm64 native extractor: object contract and corruption checks passed')
    return 0


def harness_surface_coreutils_gap(argv):
    """Pin the GNU coreutils applet denominator against the shell dispatch."""
    ROOT = HARNESS_ROOT
    BUILTINS = ROOT / 'src/sh/builtin.c'
    TOOLS = ROOT / 'src/sh/tools.inc'


    def ordered_set(ledger, name):
        values = ledger.get(name)
        if not isinstance(values, list) or values != sorted(set(values)):
            raise ValueError('%s must be a sorted list without duplicates' % name)
        return set(values)


    def table_names(source, name):
        match = re.search(r'\b%s\[\]\s*=\s*\{(.*?)^\};' % re.escape(name),
                          source, re.MULTILINE | re.DOTALL)
        if not match:
            raise ValueError('cannot find dispatch table %s' % name)
        return set(re.findall(r'^\s*\{"([^"]+)",\s*[A-Za-z_]\w*\},',
                              match.group(1), re.MULTILINE))


    def tool_names(source):
        return set(re.findall(
            r'^SHELL_TOOL\((?:GENERAL|UTIL_(?:BIN|SBIN)),\s*([A-Za-z_]\w*),',
            source, re.MULTILINE))

    try:
        ledger = HARNESS_COREUTILS_9_11
        commands = ordered_set(ledger, 'commands')
        expected = ordered_set(ledger, 'absent')
        if not expected <= commands:
            raise ValueError('absent names must belong to commands')

        builtins = BUILTINS.read_text(encoding='utf-8')
        tools = TOOLS.read_text(encoding='utf-8')
        dispatched = table_names(builtins, 'shell_commands') | tool_names(tools)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print('coreutils gap: %s' % error, file=sys.stderr)
        return 1

    absent = commands - dispatched
    if absent != expected:
        added = sorted(absent - expected)
        filled = sorted(expected - absent)
        if added:
            print('coreutils gap: newly absent: %s' % ' '.join(added),
                  file=sys.stderr)
        if filled:
            print('coreutils gap: now dispatched: %s' % ' '.join(filled),
                  file=sys.stderr)
        return 1

    print('coreutils %s: %d dispatched, %d absent of %d' %
          (ledger['version'], len(commands - absent), len(absent),
           len(commands)))
    return 0


def harness_shell_functions(argv):
    """Check persistent function storage using the compiled shell and GNU Bash.

        python3 test/harness.py shell_functions --shell /path/to/shell [--out report.json]

    Capacity-error cases use explicit bounded-shell expectations; the remaining
    cases compare stdout, stderr and status with Bash. No source is extracted.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument('--shell', required=True)
    parser.add_argument('--out')
    parser.add_argument('--runner', default='')
    parser.add_argument('--observe', action='store_true',
                        help='record a baseline without requiring it to pass')
    args = parser.parse_args(argv)

    cases = {
        'alternating': '''i=0; while test "$i" -lt 10000; do
eval 'a(){ :; }; b(){ :; }' || break
i=$((i+1)); done; printf '%s\\n' "$i"; a; b''',
        'varying_sizes': '''i=0; while test "$i" -lt 2000; do
case $((i%3)) in
0) eval 'a(){ : A A A A; }; b(){ : B; }; c(){ : C C; }';;
1) eval 'a(){ : A; }; c(){ : C; }; b(){ : B B B B; }';;
2) eval 'b(){ : B B; }; a(){ : A A; }; c(){ : C C C C; }';;
esac
test "$?" = 0 || break
a; b; c; i=$((i+1)); done; printf '%s\\n' "$i"''',
        'active_redefinition': '''install(){ f(){ f(){ new=$((new+1)); }; old=$((old+1)); }; }
i=0; old=0; new=0
while test "$i" -lt 2000; do install && f && f || break; i=$((i+1)); done
printf '%s:%s:%s\\n' "$i" "$old" "$new"''',
        'active_unset': '''install(){ f(){ unset -f f; tail=$((tail+1)); }; }
i=0; tail=0
while test "$i" -lt 2000; do install && f || break; i=$((i+1)); done
printf '%s:%s\\n' "$i" "$tail"; command -v f; :''',
        'recursive_versions': '''install(){ f(){
if test "$1" -gt 0; then f "$(( $1-1 ))"; else
f(){ newer=$((newer+1)); }
fi
older=$((older+1))
}; }
i=0; older=0; newer=0
while test "$i" -lt 1000; do install && f 12 && f || break; i=$((i+1)); done
printf '%s:%s:%s\\n' "$i" "$older" "$newer"''',
        'nested_kept_heredoc': '''outer(){ inner(){ cat <<'BODY'
held $literal text
BODY
}; }
outer; unset -f outer
i=0; while test "$i" -lt 1200; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"; inner
text=$(declare -f inner); unset -f inner; eval "$text"; inner''',
        'return_trap': '''set -T
trap 'f(){ result=new; }' RETURN
f(){ result=old; }; f
trap - RETURN
printf '%s\\n' "$result"; f; printf '%s\\n' "$result"
i=0; while test "$i" -lt 1000; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"''',
        'alias_definition_time': '''shopt -s expand_aliases
alias saved='printf "defined\\n"'
eval 'f(){ saved; }'
unalias saved
i=0; while test "$i" -lt 1000; do eval 'a(){ :; }; b(){ :; }' || break; i=$((i+1)); done
printf '%s\\n' "$i"; f
text=$(declare -f f); unset -f f; eval "$text"; f''',
        'compound_word_reuse': '''i=0
while test "$i" -lt 1000; do
eval 'a(){ local x=(one two); test "${x[1]}" = two; }; b(){ : "x=(wrong)"; }' || break
a || break
unset -f a
eval 'a(){ local x="(one two)"; test "$x" = "(one two)"; }' || break
a || break
i=$((i+1)); done
printf '%s\\n' "$i"''',
        'frontier_reclaimed': '''i=0; while test "$i" -lt 1000; do
eval 'a(){ :; }; b(){ :; }; c(){ :; }' || break
unset -f b a c
i=$((i+1)); done
printf '%s\\n' "$i"
eval '''+shlex.quote('; '.join(': x' for _ in range(180)))+'''
printf 'parsed:%s\\n' "$?"''',
        'exported_redefinition': '''i=0; f(){ :; }; export -f f
while test "$i" -lt 1000; do
eval 'a(){ :; }; f(){ printf "exported\\n"; }; b(){ :; }' || break
i=$((i+1)); done
printf '%s\\n' "$i"; /bin/bash -c f''',
    }
    # An inactive large definition can contribute its own capacity to replacement.
    large = 'x' * 6200
    cases['large_same_size_replacement'] = (
        'i=0; while test "$i" -lt 100; do eval '
        + shlex.quote('a(){ : ' + large + '; }')
        + ' || break; i=$((i+1)); done; printf "%s\\n" "$i"; a')

    # Reservation can fail after earlier arenas were reserved. Measurement can
    # also reject the new body before reservation; both must preserve the old one.
    cases['failed_copy_preserves_definition'] = (
        'a(){ printf "old\\n"; }; b(){ : ' + 'x' * 4000 + '; }\n'
        + 'eval ' + shlex.quote('a(){ : ' + 'y' * 5000 + '; }')
        + ' 2>/dev/null\nprintf "reject:%s\\n" "$?"; a; b')
    cases['failed_measure_preserves_definition'] = (
        'a(){ printf "old\\n"; }\n'
        + 'eval ' + shlex.quote('a(){ : ' + 'z' * 8192 + '; }')
        + ' 2>/dev/null\nprintf "reject:%s\\n" "$?"; a')
    special_expected = {
        'failed_copy_preserves_definition': (0, 'reject:1\nold\n', ''),
        'failed_measure_preserves_definition': (0, 'reject:1\nold\n', ''),
    }

    results = []
    for name, script in cases.items():
        if args.runner:
            command = shlex.split(args.runner) + ['-0', 'bash', args.shell, '-c', script]
            executable = None
        else:
            command = ['bash', '-c', script]
            executable = args.shell
        run = subprocess.run(command, executable=executable, capture_output=True,
                             text=True, timeout=60)
        if name in special_expected:
            wanted = special_expected[name]
        else:
            reference = subprocess.run(['/bin/bash', '-c', script], capture_output=True,
                                       text=True, timeout=60)
            wanted = (reference.returncode, reference.stdout, reference.stderr)
        got = (run.returncode, run.stdout, run.stderr)
        results.append(dict(name=name, script=script, wanted=wanted, got=got,
                            passed=got == wanted))
        print(name, 'PASS' if got == wanted else 'FAIL', repr(run.stdout), flush=True)

    if args.out:
        report = dict(binary_sha256=hashlib.sha256(pathlib.Path(args.shell).read_bytes()).hexdigest(),
                      shell=args.shell, runner=args.runner, cases=results)
        pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + '\n')
    passed = sum(row['passed'] for row in results)
    print(f'function-storage {passed} of {len(results)}')
    if os.environ.get('TEST_TALLY'):
        with open(os.environ['TEST_TALLY'], 'a') as tally:
            tally.write(f'function-storage {passed} {len(results)}\n')
    if not args.observe:
        assert passed == len(results)
    return 0


def harness_audit_shell_functions(argv):
    """Check retained AST ownership and atomic allocation using a hosted extraction.

        python3 test/harness.py audit_shell_functions [--out output-directory]

    Project span/copy primitives are hosted adapters here; real-shell grammar and
    call-frame lifetime are covered separately by harness shell_functions.
    """
    ROOT = HARNESS_ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument('--out')
    args = parser.parse_args(argv)
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
static positive memory_span_without_byte(const void *p, int byte, positive n) { const unsigned char *s=p;positive i=0;while(i<n&&s[i]!=byte)i++;return i; }
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
int main(void) {
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
    return 0


def harness_canvas_lifetime(argv):
    """Fault-inject Canvas output retirement against a reference-counted DRM model.

    Runs extracted production ownership transitions, not a GPU driver. Atomic plane
    state owns a framebuffer reference independently of the client buffer wrapper;
    failed atomic framebuffer removal leaves that state reference until a later
    commit/device shutdown. Early RMFB errors retain file ownership until close.
    """
    ROOT = HARNESS_ROOT
    parser = argparse.ArgumentParser(description=harness_canvas_lifetime.__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    spans = []
    def function(file, name):
        text = (args.source_root / file).read_text()
        match = re.search(r"^static [^;{}]*\b" + name + r"\([^;{}]*\)\n\{", text, re.M)
        if not match:
            raise ValueError("missing function definition: " + name)
        start = match.start()
        brace = match.end() - 1
        end = text.index("\n}", brace) + 2
        body = text[start:end]
        spans.append(dict(file=file, name=name, line=text[:start].count("\n") + 1,
                          sha256=hashlib.sha256(body.encode()).hexdigest()))
        return body + "\n"

    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define COLD
#define GFP_KERNEL 0
#define CURSOR_W 16
#define CURSOR_H 20
#define CURSOR_ARROW 0
#define DRM_FORMAT_ARGB8888 1
#define DRM_FORMAT_INVALID 0
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct list_head { struct list_head *next, *prev; };
static void list_init(struct list_head *h) { h->next = h->prev = h; }
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h) {
    n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n;
}
static void list_del(struct list_head *n) { n->prev->next=n->next; n->next->prev=n->prev; }
#define list_for_each_entry(p,h,m) \
    for (p=container_of((h)->next,__typeof__(*p),m); &p->m!=(h); \
         p=container_of(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,n,h,m) \
    for (p=container_of((h)->next,__typeof__(*p),m), \
         n=container_of(p->m.next,__typeof__(*p),m); &p->m!=(h); \
         p=n,n=container_of(n->m.next,__typeof__(*n),m))
struct drm_device { bool atomic; struct { unsigned cursor_width,cursor_height; } mode_config; };
struct drm_client_dev { struct drm_device *dev; };
struct drm_client_buffer { unsigned resource; };
struct drm_plane_funcs { void (*update_plane)(void), (*disable_plane)(void); };
struct drm_plane { const struct drm_plane_funcs *funcs; };
struct drm_crtc { struct drm_plane *cursor; };
struct drm_mode_set { struct drm_crtc *crtc; };
struct canvas { struct list_head link; struct drm_client_dev client; bool started; };
struct output {
    struct list_head link; struct canvas *canvas;
    struct drm_client_buffer *buffer, *cursor_buffer;
    struct drm_mode_set *mode_set; struct drm_plane *cursor_plane;
    unsigned cursor_w,cursor_h,cursor_recovery; bool cursor_shown; int x,y;
};
static struct { struct list_head outputs; int lock; } desktop;
static struct list_head canvas_list;
static int canvas_list_lock, cursor_plane_failures;
static bool cursor_plane_recovery;
static void atomic_long_inc(int *n) { ++*n; }
static void mutex_lock(int *m) { (void)m; }
static void mutex_unlock(int *m) { (void)m; }
static void canvas_thread_stop(void) {}
static struct canvas *canvas_from_client(struct drm_client_dev *c) {
    return container_of(c,struct canvas,client);
}
static void kfree(void *p) { free(p); }

/* Separate client wrapper, file framebuffer, and plane-state ownership. */
struct resource { struct drm_client_buffer *wrapper; struct drm_client_dev *client;
                  bool file, plane, gem; };
static struct resource resources[64];
static unsigned allocated, deleted, paint_calls, release_calls, commit_calls;
static bool disable_fail, remove_fail, close_fail, commit_fail, create_fail, paint_fail;
static struct output *fail_during_commit;
static bool drm_drv_uses_atomic_modeset(struct drm_device *d) { return d->atomic; }
static unsigned canvas_plane_pick_format(struct drm_plane *p, unsigned a, unsigned b) {
    (void)p; (void)b; return a;
}
static void collect(struct resource *r) { if (!r->wrapper && !r->file && !r->plane) r->gem=false; }
static struct drm_client_buffer *drm_client_buffer_create_dumb(
    struct drm_client_dev *c,unsigned w,unsigned h,unsigned f) {
    (void)w; (void)h; (void)f;
    if (create_fail) return (void *)(intptr_t)-12;
    assert(allocated < 64);
    struct drm_client_buffer *b=malloc(sizeof(*b)); assert(b);
    b->resource=allocated++;
    resources[b->resource]=(struct resource){b,c,true,false,true};
    return b;
}
static void drm_client_buffer_delete(struct drm_client_buffer *b) {
    if (!b) return;
    struct resource *r=&resources[b->resource];
    assert(r->wrapper == b && r->gem);
    if (!close_fail) {
        if (!remove_fail) r->plane=false;
        r->file=false;
    }
    r->wrapper=NULL; deleted++; free(b); collect(r);
}
static int plane_update(struct output *o,bool show,int x,int y) {
    (void)x; (void)y; assert(!show);
    struct resource *r=&resources[o->cursor_buffer->resource];
    assert(r->gem);
    if (disable_fail) return -5;
    r->plane=false; collect(r); return 0;
}
static int plane_paint(struct output *o,unsigned shape,unsigned scale) {
    (void)o; (void)shape; (void)scale; paint_calls++; return paint_fail ? -5 : 0;
}
static void desktop_place_outputs(void) {}
static void desktop_redraw(void) {}
static bool desktop_commit(void);
static void drm_client_release(struct drm_client_dev *c) {
    /* Client close cannot discover an orphaned drm_client_buffer wrapper. */
    for (unsigned i=0;i<allocated;i++) if (resources[i].client==c) {
        if (resources[i].file && !remove_fail) resources[i].plane=false;
        resources[i].file=false; collect(&resources[i]);
    }
    release_calls++; free(canvas_from_client(c));
}
'''

    bodies = "".join(function(file, name) for file, name in [
        ("src/canvas/plane.c", "plane_drop"),
        ("src/canvas/plane.c", "plane_claim"),
        ("src/canvas/output.c", "output_drop"),
        ("src/canvas/output.c", "cursor_plane_recover"),
        ("src/canvas/output.c", "canvas_release"),
        ("src/canvas/client.c", "client_unregister"),
    ])

    runner = r'''
static bool desktop_commit(void) {
    commit_calls++;
    if (commit_fail) return false;
    /* Only cards still represented in desktop.outputs get a client commit. */
    struct output *o;
    list_for_each_entry(o,&desktop.outputs,link)
        for (unsigned i=0;i<allocated;i++) if (resources[i].client==&o->canvas->client) {
            resources[i].plane=false; collect(&resources[i]);
        }
    if (fail_during_commit) {
        struct output *failed=fail_during_commit; fail_during_commit=NULL;
        plane_drop(failed);
    }
    return true;
}
static void dummy(void) {}
static const struct drm_plane_funcs funcs={dummy,dummy};
static struct drm_plane plane={&funcs};
static struct drm_crtc crtc={&plane};
static struct drm_mode_set mode={&crtc};
static struct drm_device atomic_device={.atomic=true}, legacy_device={0};
static struct canvas *card(bool atomic) {
    struct canvas *c=calloc(1,sizeof(*c)); assert(c);
    c->client.dev=atomic?&atomic_device:&legacy_device; c->started=true;
    list_add_tail(&c->link,&canvas_list); return c;
}
static struct output *output(struct canvas *c) {
    struct output *o=calloc(1,sizeof(*o)); assert(o); o->canvas=c; o->mode_set=&mode;
    o->x=13; o->y=-27;
    o->buffer=drm_client_buffer_create_dumb(&c->client,640,480,1);
    plane_claim(&c->client,o);
    if (o->cursor_plane) { resources[o->cursor_buffer->resource].plane=true; o->cursor_shown=true; }
    list_add_tail(&o->link,&desktop.outputs); return o;
}
static unsigned wrappers(void) {
    unsigned n=0; for (unsigned i=0;i<allocated;i++) n+=resources[i].wrapper!=NULL; return n;
}
static unsigned scanouts(void) {
    unsigned n=0; for (unsigned i=0;i<allocated;i++) if(resources[i].plane) {
        assert(resources[i].gem); n++;
    }
    return n;
}
static unsigned gems(void) {
    unsigned n=0; for (unsigned i=0;i<allocated;i++) n+=resources[i].gem; return n;
}
static unsigned checks, failures;
static void check(const char *name,bool okay) {
    checks++; if (!okay) { failures++; printf("FAIL %s\n",name); }
}
static void reset(void) {
    /* Dispose failed-baseline orphans after recording them; keep cases isolated. */
    for(unsigned i=0;i<allocated;i++) free(resources[i].wrapper);
    for(unsigned i=0;i<allocated;i++) resources[i]=(struct resource){0};
    allocated=deleted=paint_calls=release_calls=commit_calls=0;
    disable_fail=remove_fail=close_fail=commit_fail=create_fail=paint_fail=false;
    fail_during_commit=NULL; cursor_plane_recovery=false; cursor_plane_failures=0;
    list_init(&desktop.outputs); list_init(&canvas_list);
}
int main(void) {
    for (unsigned pending=0;pending<2;pending++) for(unsigned failure=0;failure<2;failure++)
    for(unsigned rmfail=0;rmfail<3;rmfail++) {
        reset(); struct canvas *c=card(true); struct output *o=output(c);
        disable_fail=failure; remove_fail=rmfail==1; close_fail=rmfail==2;
        if (pending) plane_drop(o);
        output_drop(o);
        check("retire releases both client buffers",wrappers()==0 && deleted==2);
        check("retire preserves failed scanout's GEM",scanouts()==(failure&&rmfail));
        cursor_plane_recover();
        check("recovery without outputs cannot strand a client buffer",wrappers()==0);
        client_unregister(&c->client);
        check("unregister releases canvas after its outputs",release_calls==1 && list_empty(&desktop.outputs));
        for(unsigned i=0;i<allocated;i++) { resources[i].plane=false; collect(&resources[i]); }
        check("device shutdown releases final scanout references",gems()==0);
    }
    reset(); struct canvas *c=card(true); struct output *o=output(c);
    disable_fail=true; plane_drop(o); commit_fail=true; cursor_plane_recover();
    check("live failed commit keeps recovery ownership",wrappers()==2 && o->cursor_recovery==1);
    commit_fail=false; cursor_plane_recover();
    check("live recovery releases cursor once",wrappers()==1 && deleted==1 && !o->cursor_recovery);
    client_unregister(&c->client); check("recovered teardown has no remaining GEM",gems()==0);

    reset(); c=card(true); o=output(c); struct output *second=output(c);
    disable_fail=true; plane_drop(o); fail_during_commit=second; cursor_plane_recover();
    check("failure while rearming survives current recovery",second->cursor_recovery==1 && wrappers()==3);
    cursor_plane_recover(); check("next recovery covers rearm failure",wrappers()==2);
    client_unregister(&c->client); check("two-output teardown is balanced",gems()==0 && deleted==4);

    reset(); c=card(true); o=output(c); struct canvas *other=card(true); second=output(other);
    disable_fail=remove_fail=true; plane_drop(o); canvas_release(c); cursor_plane_recover();
    check("other card's recovery cannot own retired buffers",wrappers()==2 && scanouts()==1);
    client_unregister(&c->client); client_unregister(&other->client);
    check("multi-card unregister releases every wrapper",wrappers()==0);

    reset(); c=card(true); o=output(c); disable_fail=remove_fail=true;
    client_unregister(&c->client);
    check("unregister after failed disable releases client wrappers",wrappers()==0 && release_calls==1);
    check("unregister keeps scanout alive until device teardown",scanouts()==1);

    reset(); c=card(false); o=output(c);
    check("legacy modesetting uses software cursor",!o->cursor_plane && !o->cursor_buffer && !paint_calls);
    check("software cursor claim leaves placement and allocation unchanged",o->x==13 && o->y==-27 && allocated==1);
    client_unregister(&c->client); check("software-only teardown is balanced",!gems());

    reset(); c=card(true); o=calloc(1,sizeof(*o)); assert(o); o->canvas=c; o->mode_set=&mode;
    create_fail=true; plane_claim(&c->client,o);
    check("allocation failure leaves no cursor owner",!o->cursor_buffer && !o->cursor_plane);
    create_fail=false; paint_fail=true; plane_claim(&c->client,o);
    check("paint failure releases unsubmitted buffer",!o->cursor_buffer && !o->cursor_plane && !gems());
    free(o); client_unregister(&c->client); reset();
    printf("canvas-lifetime %u/%u\n",checks-failures,checks); return failures?1:0;
}
'''

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        unit = out / "canvas-lifetime.c"
        unit.write_text(prefix + bodies + runner)
        binary = out / "canvas-lifetime"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fsanitize=address,undefined", str(unit), "-o", str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        (out / "result.json").write_text(json.dumps(dict(source_root=str(args.source_root),
            spans=spans, command=command, returncode=result.returncode,
            stdout=result.stdout, stderr=result.stderr), indent=2) + "\n")
        tally = re.search(r"canvas-lifetime (\d+)/(\d+)", result.stdout)
        if tally and os.environ.get("TEST_TALLY"):
            with open(os.environ["TEST_TALLY"], "a") as stream:
                stream.write("canvas-lifetime " + " ".join(tally.groups()) + "\n")
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode

    if args.output:
        return run(args.output.resolve())
    with tempfile.TemporaryDirectory(prefix="canvas-lifetime-") as work:
        return run(Path(work))


def harness_code_map(argv):
    """Source-atlas regression fixtures; no compiler or production mutation needed.

        python3 test/harness.py code_map [-v]

    The examples assert source facts, rather than a second implementation of the
    atlas parser. These checks do not turn lexical edges into semantic call edges.
    The atlas builder, kit/code_map/build.py, is loaded here rather than at import.
    """
    SPEC = importlib.util.spec_from_file_location(
        'code_map_build', HARNESS_ROOT / 'kit/code_map/build.py')
    build = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(build)


    class AtlasFixture(unittest.TestCase):
        def setUp(self):
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            self.root = Path(temporary.name)
            self.here = self.root / 'kit/code_map'
            (self.here / 'annotations').mkdir(parents=True)
            for owner, attribute, value in (
                    (build, 'ROOT', self.root), (build.audit, 'ROOT', self.root),
                    (build, 'HERE', self.here)):
                replacement = patch.object(owner, attribute, value)
                replacement.start()
                self.addCleanup(replacement.stop)

        def source(self, text, path='src/fixture.c'):
            destination = self.root / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(textwrap.dedent(text).lstrip('\n'))
            return build.load_source(path)

        def rows(self, text, path='src/fixture.c', definitions=None):
            source = self.source(text, path)
            if definitions is None:
                definitions = (build.audit.ordinary_definitions(self.root / path) +
                               build.audit.alias_definitions(self.root / path))
            else:
                definitions = [build.audit.Definition(path, line, name, kind)
                               for name, line, kind in definitions]
            rows = build.c_rows(path, source, definitions)
            for index, row in enumerate(rows):
                row.update(index=index, production=path.startswith('src/'),
                           family='fixture.engine', review_basis='body_reviewed')
            return source, rows

        def family(self):
            return {'id': 'fixture.engine', 'title': 'Fixture engine',
                    'description': 'Small isolated source fixtures.',
                    'design_question': 'Are source facts represented faithfully?',
                    'constraints': ['No semantic-equivalence claim.']}

        def annotations(self, rows, name='fixture.json'):
            functions = [{
                'id': row['id'], 'family': 'fixture.engine', 'role': 'algorithm',
                'responsibility': 'Execute the fixture operation.',
                'review_basis': 'body_reviewed', 'confidence': 'high',
                'contract_notes': ['Fixture only.']}
                for row in rows]
            data = {'schema_version': 1, 'scope': sorted({r['file'] for r in rows}),
                    'families': [self.family()], 'functions': functions}
            path = self.here / 'annotations' / name
            path.write_text(json.dumps(data))
            return path, data

        def aggregate(self, source, rows, path='src/fixture.c'):
            files = [{'file': path, 'physical_loc': len(source['lines'])}]
            families = [self.family()]
            build.aggregate(rows, files, families, {path: source})
            return files[0], families[0]


    class SourceBoundaryTests(AtlasFixture):
        def test_braceless_loops_comments_and_literals_preserve_body_boundary(self):
            source, rows = self.rows('''
            static const char *text = "not_a_body() { while (;;) }";
            static int first(int n)
            {
                /* fake() { return; } */
                while (n > 2) n--;
                for (; n > 1; --n) consume(n);
                return n;
            }
            int second(void) { return 7; }
        ''')
            self.assertEqual([(r['name'], r['start'], r['end']) for r in rows],
                             [('first', 2, 8), ('second', 9, 9)])
            self.assertEqual(rows[0]['loops'], 2)
            self.assertEqual(rows[0]['call_names'], ['consume'])
            self.assertNotIn(4, source['code'])
            self.assertEqual(rows[0]['max_brace_depth'], 1)

        def test_loop_count_counts_do_while_once_including_nested_loops(self):
            _, rows = self.rows('''
            int count(int n) {
                do { do n--; while (n > 4); } while (n > 2);
                while (n > 1) n--;
                for (; n; --n) visit(n);
                return n;
            }
        ''')
            self.assertEqual(rows[0]['loops'], 4)

        def test_knr_header_retains_multiline_storage_and_return_type(self):
            _, rows = self.rows('''
            static
            int
            old(a, b)
            int a;
            char *b;
            {
                return a + b[0];
            }
        ''')
            self.assertEqual([(r['name'], r['line']) for r in rows], [('old', 3)])
            self.assertEqual(rows[0]['start'], 1)
            self.assertTrue(rows[0]['signature'].startswith('static int old'))
            self.assertEqual(rows[0]['loc'], 8)

        def test_separate_macro_invocation_does_not_join_function_header(self):
            _, rows = self.rows('''
            CONFIGURE(example)
            static int handler(int n)
            {
                return n;
            }
        ''')
            self.assertEqual([(r['name'], r['start'], r['end']) for r in rows],
                             [('handler', 2, 5)])
            self.assertNotIn('CONFIGURE', rows[0]['signature'])

        def test_preprocessor_variants_remain_distinct_with_exact_spans(self):
            source, rows = self.rows('''
            #if FAST
            int choose(void) { return 1; }
            #else
            int choose(void) { return 2; }
            #endif
        ''')
            self.assertEqual([(r['name'], r['line'], r['loc']) for r in rows],
                             [('choose', 2, 1), ('choose', 4, 1)])
            file, _ = self.aggregate(source, rows)
            self.assertEqual(file['function_attributed_loc'], 2)
            self.assertEqual(file['outside_function_loc'], 3)

        def test_multiline_directive_and_comments_have_distinct_code_accounting(self):
            source, rows = self.rows('''
            #define VALUE(x) \\
                ((x) + 1)

            /* retained explanation */
            int read_value(void)
            {
                // a body comment
                return VALUE(2);
            }
        ''')
            self.assertEqual(source['code'], {1, 2, 5, 6, 8, 9})
            self.assertEqual(rows[0]['loc'], 5)
            self.assertEqual(rows[0]['code_lines'], 4)
            file, _ = self.aggregate(source, rows)
            self.assertEqual(file['outside_function_loc'], 4)


    class AttributionTests(AtlasFixture):
        def test_aliases_on_one_line_keep_their_targets_and_tokens(self):
            source, rows = self.rows(
                'int first(void) __attribute__((alias("base_one"))); '
                'int second(void) __attribute__((alias("base_two")));\n')
            self.assertEqual({r['name']: r.get('alias_of') for r in rows},
                             {'first': 'base_one', 'second': 'base_two'})
            self.assertTrue(rows[0]['_token_positions'].isdisjoint(
                rows[1]['_token_positions']))
            file, family = self.aggregate(source, rows)
            self.assertEqual(file['function_attributed_loc'], 1)
            self.assertEqual(sum(r['attributed_loc'] for r in rows), 1)
            self.assertEqual(sum(r['attributed_code_lines'] for r in rows), 1)
            self.assertEqual(family['tokens'], len(source['tokens']))

        def test_multiline_alias_includes_full_declarator(self):
            _, rows = self.rows('''
            extern
            int
            public_name(void)
                __attribute__((alias("private_name")));
        ''')
            self.assertEqual(rows[0]['alias_of'], 'private_name')
            self.assertEqual((rows[0]['start'], rows[0]['end']), (1, 4))
            self.assertEqual(rows[0]['tokens'], 16)

        def test_shared_generator_invocation_is_charged_once(self):
            with patch.dict(build.audit.GENERATORS, {
                    ('src/fixture.c', 'MAKE_PAIR'): ('{}_read', '{}_write')}):
                source, rows = self.rows('''
                #define MAKE_PAIR(name) /* expansion intentionally outside symbols */
                MAKE_PAIR(
                    thing)
            ''', definitions=[('thing_read', 2, 'generated'),
                                  ('thing_write', 2, 'generated')])
            self.assertEqual(rows[0]['_token_positions'], rows[1]['_token_positions'])
            file, family = self.aggregate(source, rows)
            self.assertEqual([r['attributed_loc'] for r in rows], [2, 0])
            self.assertEqual(file['outside_function_loc'], 1)
            self.assertEqual(family['tokens'], 4)

        def test_distinct_same_line_generators_have_distinct_token_ownership(self):
            with patch.dict(build.audit.GENERATORS, {
                    ('src/fixture.c', 'MAKE'): ('{}',)}):
                source, rows = self.rows(
                    'MAKE(first) MAKE(second)\n',
                    definitions=[('first', 1, 'generated'), ('second', 1, 'generated')])
            self.assertTrue(rows[0]['_token_positions'].isdisjoint(
                rows[1]['_token_positions']))
            _, family = self.aggregate(source, rows)
            self.assertEqual(family['attributed_loc'], 1)
            self.assertEqual(family['tokens'], 8)
            self.assertEqual(sum(r['attributed_tokens'] for r in rows),
                             len(source['tokens']))

        def test_shared_body_line_deduplicates_loc_but_preserves_both_bodies(self):
            source, rows = self.rows(
                'int first(void) { return 1; } int second(void) { return 2; }\n')
            self.assertEqual(len(rows), 2)
            file, family = self.aggregate(source, rows)
            self.assertEqual([r['loc'] for r in rows], [1, 1])
            self.assertEqual(sum(r['attributed_loc'] for r in rows), 1)
            self.assertEqual(family['tokens'], len(source['tokens']))
            self.assertEqual(file['outside_function_loc'], 0)

        def test_architecture_bodies_and_api_alias_have_disjoint_source_costs(self):
            path = 'src/library.c'
            source = self.source('''
            #if X64
            ASM_FUNC(copy, void, (void))
                "ret"
            ASM_END(copy)
            #elif ARM64
            ASM_FUNC(copy, void, (void))
                "ret"
            ASM_END(copy)
            #endif
            ASM_ALIAS(public_copy, copy)
        ''', path)
            rows = build.assembly_rows({path: source})
            for i, row in enumerate(rows):
                row.update(index=i, family='fixture.engine', production=True,
                           review_basis='alias_or_generator')
            self.assertEqual({r['name'] for r in rows}, {'copy', 'public_copy'})
            routine = next(r for r in rows if r['name'] == 'copy')
            alias = next(r for r in rows if r['name'] == 'public_copy')
            self.assertEqual(routine['spans'], [[2, 4], [6, 8]])
            self.assertEqual(alias['alias_of'], 'copy')
            file, family = self.aggregate(source, rows, path)
            self.assertEqual(file['function_attributed_loc'], 7)
            self.assertEqual(file['outside_function_loc'], 3)
            self.assertEqual(family['tokens'], 0)


    class ConnectionTests(AtlasFixture):
        def connected(self, sources):
            all_sources, rows = {}, []
            for path, text in sources.items():
                source, added = self.rows(text, path)
                all_sources[path] = source
                rows.extend(added)
            build.connections(rows, all_sources)
            return rows

        @staticmethod
        def targets(rows, row, key):
            return {(rows[i]['file'], rows[i]['name'], rows[i]['line'])
                    for i in row[key]}

        def test_same_file_overrides_cross_file_and_retains_configuration_variants(self):
            rows = self.connected({
                'src/a.c': '#if FAST\nint pick(void) { return 1; }\n#else\n'
                           'int pick(void) { return 2; }\n#endif\n'
                           'int use(void) { return pick(); }\n',
                'src/b.c': 'int pick(void) { return 3; }\n'})
            caller = next(r for r in rows if r['name'] == 'use')
            self.assertEqual(self.targets(rows, caller, 'callees'),
                             {('src/a.c', 'pick', 2), ('src/a.c', 'pick', 4)})

        def test_cross_file_possible_calls_preserve_production_and_support_scope(self):
            rows = self.connected({
                'src/user.c': 'int use(void) { return shared(); }\n',
                'src/a.c': 'int shared(void) { return 1; }\n',
                'src/b.c': 'int shared(void) { return 2; }\n',
                'kit/test.c': 'int shared(void) { return 3; }\n'
                              'int verify(void) { return shared(); }\n'})
            caller = next(r for r in rows if r['name'] == 'use')
            self.assertEqual(self.targets(rows, caller, 'callees'),
                             {('src/a.c', 'shared', 1), ('src/b.c', 'shared', 1)})
            check = next(r for r in rows if r['name'] == 'verify')
            self.assertEqual(self.targets(rows, check, 'callees'),
                             {('kit/test.c', 'shared', 1)})

        def test_member_callbacks_are_indirect_not_global_calls_or_references(self):
            rows = self.connected({'src/fixture.c': '''
            int hook(void) { return 1; }
            int use(struct callbacks *pointer, struct callbacks value) {
                return pointer->hook() + value.hook();
            }
        '''})
            caller = next(r for r in rows if r['name'] == 'use')
            self.assertEqual(caller['indirect_call_names'], ['hook'])
            self.assertEqual(caller['callees'], [])
            self.assertEqual(caller['references'], [])
            self.assertEqual(caller['unresolved_calls'], [])

        def test_member_spelling_does_not_hide_a_separate_callback_reference(self):
            rows = self.connected({'src/fixture.c': '''
            int hook(void) { return 1; }
            int use(struct callbacks *pointer) {
                register_callback(hook);
                return pointer->hook();
            }
        '''})
            caller = next(r for r in rows if r['name'] == 'use')
            self.assertEqual(self.targets(rows, caller, 'references'),
                             {('src/fixture.c', 'hook', 1)})
            self.assertEqual(caller['callees'], [])
            self.assertEqual(caller['unresolved_calls'], ['register_callback'])

        def test_initializer_after_body_on_same_line_is_an_outside_reference(self):
            rows = self.connected({'src/fixture.c':
                'int target(void) { return 1; }\n'
                'int use(void) { return 0; } int (*selected)(void) = target;\n'})
            target = next(r for r in rows if r['name'] == 'target')
            self.assertIn('src/fixture.c:2', target['outside_body_references'])

        def test_alias_edge_resolves_target_without_inventing_an_extra_body(self):
            rows = self.connected({'src/fixture.c':
                'int target(void) { return 1; }\n'
                'int public_name(void) __attribute__((alias("target")));\n'})
            alias = next(r for r in rows if r['name'] == 'public_name')
            self.assertEqual(alias['kind'], 'alias')
            self.assertEqual(self.targets(rows, alias, 'callees'),
                             {('src/fixture.c', 'target', 1)})


    class ClassifierAndExportTests(AtlasFixture):
        def test_classifier_requires_exact_production_ids(self):
            _, rows = self.rows('int first(void) { return 1; }\n'
                                'int second(void) { return 2; }\n')
            path, data = self.annotations(rows)
            _, coverage = build.classify(copy.deepcopy(rows), False)
            self.assertEqual(coverage['missing'], [])
            self.assertEqual(coverage['stale'], [])
            data['functions'].pop()
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, '1 missing'):
                build.classify(copy.deepcopy(rows), False)
            data['functions'][0]['id'] = 'src/removed.c:stale:1'
            path.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, '1 stale'):
                build.classify(copy.deepcopy(rows), False)

        def test_classifier_rejects_duplicate_annotations_and_unknown_family(self):
            _, rows = self.rows('int first(void) { return 1; }\n')
            path, data = self.annotations(rows)
            data['functions'].append(copy.deepcopy(data['functions'][0]))
            path.write_text(json.dumps(data))
            with self.assertRaises(AssertionError):
                build.classify(copy.deepcopy(rows), False)
            data['functions'].pop()
            data['functions'][0]['family'] = 'missing.family'
            path.write_text(json.dumps(data))
            with self.assertRaises(AssertionError):
                build.classify(copy.deepcopy(rows), False)

        def test_assembly_symbols_require_annotations_too(self):
            _, rows = self.rows('int first(void) { return 1; }\n')
            rows[0].update(kind='assembly', id='src/library.c:first:asm')
            with self.assertRaisesRegex(ValueError, '1 missing'):
                build.classify(copy.deepcopy(rows), False)
            self.annotations(rows)
            _, coverage = build.classify(rows, False)
            self.assertEqual(coverage['missing'], [])

        def test_supplied_stale_source_digest_cannot_reuse_same_function_ids(self):
            _, rows = self.rows('int first(void) { return 1; }\n')
            path, data = self.annotations(rows)
            data['source_digest'] = 'old-source-digest'
            path.write_text(json.dumps(data))
            with patch.object(build.audit, 'source_digest', return_value='current-source-digest'):
                for allow_incomplete in (False, True):
                    with self.subTest(allow_incomplete=allow_incomplete):
                        with self.assertRaisesRegex(ValueError, 'Stale source digest'):
                            build.classify(copy.deepcopy(rows), allow_incomplete)
                data['source_digest'] = 'current-source-digest'
                path.write_text(json.dumps(data))
                _, coverage = build.classify(rows, False)
            self.assertEqual(coverage['annotation_pins'],
                             {'fixture.json': 'current-source-digest'})

        def test_incomplete_mode_exposes_missing_ids_and_keeps_review_basis_honest(self):
            _, rows = self.rows('int first(void) { return 1; }\n')
            _, coverage = build.classify(rows, True)
            self.assertEqual(coverage['missing'], [rows[0]['id']])
            self.assertEqual(rows[0]['family'], 'unclassified')
            self.assertEqual(rows[0]['review_basis'], 'family_rule')
            self.assertEqual(rows[0]['confidence'], 'low')

        def test_export_has_one_csv_header_and_conserved_physical_loc(self):
            source, rows = self.rows('''
            #define CONSTANT 3
            /* a file-level comment */
            int global = 3;
            int first(void) { return CONSTANT; }
            int second(void) { return first(); }
        ''')
            self.annotations(rows)
            definitions = build.audit.ordinary_definitions(self.root / 'src/fixture.c')
            output = self.root / 'out'
            with patch.object(build.audit, 'production_sources', return_value=[self.root / 'src/fixture.c']), \
                 patch.object(build.audit, 'audited_sources', return_value=[self.root / 'src/fixture.c']), \
                 patch.object(build.audit, 'inventory', return_value=definitions), \
                 patch.object(build.audit, 'library_routines', return_value=([], [], [])), \
                 patch.object(build.audit, 'source_digest', return_value='fixture-digest'), \
                 patch.object(build, 'git', side_effect=lambda *a: 'src/fixture.c' if a == ('ls-files',) else 'fixture-commit'), \
                 patch('sys.argv', ['build.py', '--out', str(output), '--no-similarity']), \
                 contextlib.redirect_stdout(io.StringIO()):
                build.main()
            atlas = json.loads((output / 'functions.json').read_text())
            with (output / 'functions.csv').open(newline='') as stream:
                exported = list(csv.DictReader(stream))
            self.assertEqual(len(exported), 2)
            self.assertEqual({row['id'] for row in exported}, {r['id'] for r in rows})
            self.assertEqual(atlas['summary']['production_entries'], 2)
            file = atlas['files'][0]
            self.assertEqual(file['physical_loc'],
                             file['function_attributed_loc'] + file['outside_function_loc'])
            self.assertEqual(sum(r['attributed_tokens'] for r in atlas['functions']),
                             len(source['tokens']) - 5)
            self.assertEqual(sum(r['attributed_code_lines'] for r in atlas['functions']), 2)
            self.assertEqual(file['code_bearing_loc'], 4)


    class SimilarityTests(AtlasFixture):
        def test_renamed_groups_are_syntactic_leads_and_keep_literals(self):
            def operation(name, call, literal):
                updates = '\n'.join(f'if (value > {n}) value += {call}(value);'
                                    for n in range(6))
                return f'int {name}(int value) {{ {updates} return value + {literal}; }}\n'
            _, rows = self.rows(operation('first', 'read', 1) +
                                operation('second', 'destroy', 1) +
                                operation('third', 'read', 2))
            result = build.similarities(rows)
            exact = [{rows[i]['name'] for i in g['functions']}
                     for g in result['exact_renamed_groups']]
            self.assertEqual(exact, [{'first', 'second'}])
            self.assertIn('leads', result['method'])
            self.assertIn('No NiCad run', result['method'])

        def test_small_or_support_bodies_are_outside_similarity_scope(self):
            _, rows = self.rows('int one(void) { return 1; }\n'
                                'int two(void) { return 1; }\n')
            result = build.similarities(rows)
            self.assertEqual(result['exact_renamed_groups'], [])
            for row in rows:
                row.update(production=False, tokens=1000)
            result = build.similarities(rows)
            self.assertEqual(result['exact_renamed_groups'], [])
            self.assertEqual(result['near_pairs'], [])

    suite = unittest.TestSuite(
        unittest.defaultTestLoader.loadTestsFromTestCase(case)
        for case in (SourceBoundaryTests, AttributionTests, ConnectionTests,
                     ClassifierAndExportTests, SimilarityTests))
    verbosity = 2 if '-v' in argv else 1
    return 0 if unittest.TextTestRunner(verbosity=verbosity).run(suite).wasSuccessful() else 1


HARNESS_CHECKS = {
    "core_state": harness_core_state,
    "spark_entry": harness_spark_entry,
    "build_tools": harness_build_tools,
    "inventory": harness_inventory,
    "edit_driver": harness_edit_driver,
    "native_extract": harness_native_extract,
    "native_extract_test": harness_native_extract_test,
    "surface_coreutils_gap": harness_surface_coreutils_gap,
    "shell_functions": harness_shell_functions,
    "audit_shell_functions": harness_audit_shell_functions,
    "canvas_lifetime": harness_canvas_lifetime,
    "code_map": harness_code_map,
}


def harness_main(argv):
    if not argv or argv[0] not in HARNESS_CHECKS:
        print("harness: one of " + " ".join(HARNESS_CHECKS), file=sys.stderr)
        return 2
    return HARNESS_CHECKS[argv[0]](argv[1:])


if __name__ == "__main__":
    sys.exit(harness_main(sys.argv[1:]))
