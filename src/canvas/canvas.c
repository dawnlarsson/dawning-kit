/*
        Moonwater Canvas

        An in-kernel compositor, on the DRM client API the framebuffer console
        and drm_log already use. Attaches without patching the kernel: a DRM
        device can be opened like any other file, and struct drm_file leads
        back to the drm_device behind it.

            ../window.c the page a program shares with this, and the whole
                        interface it needs -- see there first
            pane.c      windows: creating, destroying, and reading the shared
                        page without trusting it
            paint.c     pixels: a pointer, a pitch, a rectangle
            compose.c   what a window looks like and in what order
            plane.c     the cursor, on a hardware plane or in the framebuffer
            drag.c      moving a window, across a seam if it comes to that
            output.c    outputs, their placement, and starting and stopping
            client.c    attaching to DRM, and finding cards to attach to
            pointer.c   input

        Included rather than compiled apart, so that order is a dependency
        order. Included by core.c in turn, which is where the headers are, and
        before library.c, which redefines bool and defines "end" as a macro.

        There is one desktop across every card, and one cursor on it. Windows
        and the cursor are in desktop coordinates; an output is a rectangle of
        the desktop that some crtc scans out. Nothing has a fixed maximum.
*/

#include "../window.c"

#define log_canvas(fmt, ...) pr_info("[moonwater canvas] " fmt, ##__VA_ARGS__)

#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

/*
        A pane is the compositor's side of a window: where it is, and the
        pixels behind it. struct window, in ../window.c, is the page the
        program that owns it has mapped. Panes without one are the compositor's
        own.

        The geometry here is a checked copy of the geometry there. Nothing
        composes from the shared page directly: a program can store into it
        between a bounds check and a read.
*/
struct pane
{
        struct list_head link;
        int x, y, z;
        int width, height;
        unsigned int region;
        unsigned int display;

        struct window *shared;
        u32 *pixels;
        unsigned int pitch;
        unsigned int max_width, max_height;
        unsigned long bytes;
        void *mapping;
};

struct output
{
        struct list_head link;
        struct canvas *canvas;
        struct drm_client_buffer *buffer;
        struct drm_mode_set *mode_set;

        int x, y;
        unsigned int width, height;
        u32 format;

        // Null means the cursor is drawn into this output's framebuffer.
        struct drm_plane *cursor_plane;
        struct drm_client_buffer *cursor_buffer;
        unsigned int cursor_w, cursor_h;
        _Bool cursor_shown;
};

struct canvas
{
        struct list_head link;
        struct drm_client_dev client;
        _Bool started;
};

static struct desktop
{
        struct mutex lock;
        struct list_head outputs;
        struct list_head windows;

        int cursor_x, cursor_y;
        int drawn_x, drawn_y;

        // The pane being dragged, and where inside it the cursor took hold.
        struct pane *dragging;
        int grab_x, grab_y;

        // The bounding box of every output. Read by the input handler in
        // atomic context, where it cannot walk the list.
        int width, height;

        atomic_t pending_x;
        atomic_t pending_y;
        atomic_t motion_pending;
        u64 motion_stamp;

        // The button, and where it went down. Picking a window needs the list,
        // which the input handler cannot walk, so it records and the thread
        // picks.
        atomic_t button_down;
        atomic_t button_changed;
        atomic_t button_x;
        atomic_t button_y;
} desktop = {
    .lock = __MUTEX_INITIALIZER(desktop.lock),
    .outputs = LIST_HEAD_INIT(desktop.outputs),
    .windows = LIST_HEAD_INIT(desktop.windows),
};

static DEFINE_MUTEX(canvas_list_lock);
static LIST_HEAD(canvas_list);

static void pointer_start(void);
static void pointer_stop(void);
static void desktop_redraw(void);

// Nanoseconds from an event arriving to the cursor being on screen.
static u64 pointer_latency_total;
static u64 pointer_latency_worst;
static unsigned long pointer_events;
static u64 pointer_queue_total;
static u64 pointer_draw_total;
static u64 pointer_flush_total;

static struct canvas *canvas_from_client(struct drm_client_dev *client)
{
        return container_of(client, struct canvas, client);
}

static _Bool rects_overlap(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh)
{
        return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static _Bool pane_titlebar_holds(struct pane *pane, int x, int y)
{
        return x >= pane->x && x < pane->x + pane->width &&
               y >= pane->y && y < pane->y + WINDOW_TITLE;
}

static _Bool output_holds(struct output *output, int x, int y)
{
        return x >= output->x && x < output->x + (int)output->width &&
               y >= output->y && y < output->y + (int)output->height;
}

#include "paint.c"
#include "pane.c"
#include "compose.c"
#include "plane.c"
#include "drag.c"
#include "output.c"
#include "client.c"
#include "pointer.c"
