#!/usr/bin/env python3
"""Fault-inject Canvas output retirement against a reference-counted DRM model.

Runs extracted production ownership transitions, not a GPU driver. Atomic plane
state owns a framebuffer reference independently of the client buffer wrapper;
failed atomic framebuffer removal leaves that state reference until a later
commit/device shutdown. Early RMFB errors retain file ownership until close.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-root", type=Path, default=ROOT)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
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
    raise SystemExit(run(args.output.resolve()))
with tempfile.TemporaryDirectory(prefix="canvas-lifetime-") as work:
    raise SystemExit(run(Path(work)))
