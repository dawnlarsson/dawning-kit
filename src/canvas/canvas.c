/*
        Moonwater Canvas

        An in-kernel compositor, on the DRM client API the framebuffer console
        and drm_log already use. Attaches without patching the kernel: a DRM
        device can be opened like any other file, and struct drm_file leads
        back to the drm_device behind it.

            window.c    the page a program shares with this, and the whole
                        interface it needs -- see there first
            pane.c      windows: creating, destroying, and reading the shared
                        page without trusting it
            fill.asm    one run of pixels, per architecture. Everything
                        Canvas draws goes through it.
            glyph.asm   one glyph, for the same reason
            paint.c     pixels: a pointer, a pitch, a rectangle
            text.c      words: a box, where the lines break, where they sit
            compose.c   what a window looks like and in what order
            plane.c     the cursor, on a hardware plane or in the framebuffer
            drag.c      moving a window, across a seam if it comes to that
            output.c    outputs, their placement, and starting and stopping
            keys.c      what a keyboard means, and which window hears it
            client.c    attaching to DRM, and finding cards to attach to
            pointer.c   input

        Included rather than compiled apart, so that order is a dependency
        order. Included by core.c in turn, which is where the headers are, and
        before library.c, which redefines bool and defines "end" as a macro.

        There is one desktop across every card, and one cursor on it. Windows
        and the cursor are in desktop coordinates; an output is a rectangle of
        the desktop that some crtc scans out. Nothing has a fixed maximum.
*/

#include "window.c"

#define log_canvas(fmt, ...) pr_info("[moonwater canvas] " fmt, ##__VA_ARGS__)

/*
        Colours are an index, not a value.

        Every one has to be converted for the format of the screen it lands
        on, and that used to happen at each of the hundreds of thousands of
        draw calls a compose makes. An output converts the whole palette once
        when it learns its format, and the drawing code indexes what it was
        handed without knowing what a format is.
*/
enum
{
        INK_DESKTOP,
        INK_FRAME,
        INK_TITLE,
        INK_TITLE_LIT,
        INK_BODY,
        INK_TEXT,
        INK_CURSOR,
        INK_CURSOR_EDGE,
        INK_COUNT,
};

/*
        A pane is the compositor's side of a window: where it is, and the
        pixels behind it. struct window, in window.c, is the page the
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
        unsigned int style;
        unsigned int state;
        int edge;
        unsigned int sequence;
        char title[WINDOW_TITLE_MAX];
        unsigned int title_length;

        struct window *shared;
        u32 *pixels;

        // A window of text instead: cells the compositor draws the glyphs for.
        struct window_cell *cells;
        unsigned int columns, rows;
        unsigned int max_columns, max_rows;
        unsigned int damage_row, damage_rows;

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

        // Every colour, converted once for this output's format.
        u32 palette[INK_COUNT];
        u32 opaque;

        // Null means the cursor is drawn into this output's framebuffer.
        struct drm_plane *cursor_plane;
        struct drm_client_buffer *cursor_buffer;
        unsigned int cursor_w, cursor_h;
        unsigned int cursor_shape;
        unsigned int cursor_scale;
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
        struct pane *focused;

        // Resizing: which edges are held, and the rectangle and pointer
        // position they were held at, so every step measures from the grab
        // rather than accumulating.
        struct pane *resizing;
        unsigned int resize_edges;
        int resize_x, resize_y, resize_w, resize_h;
        int press_x, press_y;

        unsigned int cursor_shape;
        unsigned int drawn_shape;

        /*
                Shake to find it, the way a desktop does: reverse direction
                enough times in a short enough window and the cursor grows,
                then goes back on its own.
        */
        unsigned int cursor_scale;
        unsigned int drawn_scale;
        u64 magnified_until;
        atomic_t magnify;
        atomic_t shake_dir;
        atomic_t shake_count;
        u64 shake_window;

        /*
                Acceleration. The remainder is what stops a gain that is not a
                whole number from quietly dropping the fraction of every
                movement: a slow drag would come up short of where it was
                aimed, which is the thing people notice.
        */
        u64 accel_stamp;
        int accel_x, accel_y;

        // What a device has reported so far this frame. A mouse sends each
        // axis as its own event and then says it is done, and a movement is
        // the whole of what arrived between those.
        int raw_x, raw_y;

        /*
                A program changes a window by storing into its shared page and
                bumping a sequence, which costs no call. Something has to look,
                so this ticks while there is anything to look at and stops
                itself once nothing has changed for a while. awake is what the
                programs read to know whether it is still looking.
        */
        /*
                What changed since the last commit, so a program saying so
                repaints its own window rather than every screen. Four is a
                working set, not a limit: past it the whole desktop is cheaper
                than tracking the pieces.
        */
        struct drm_rect damage[4];
        unsigned int damage_count;
        _Bool damage_all;

        struct hrtimer frame;
        atomic_t frame_pending;
        unsigned int idle_frames;
        _Bool awake;
        _Bool started;
        _Bool terminal;

        // The bounding box of every output. Read by the input handler in
        // atomic context, where it cannot walk the list.
        int width, height;

        atomic_t pending_x;
        atomic_t pending_y;
        atomic_t motion_pending;
        u64 motion_stamp;

        /*
                Keys as they arrive, before anything knows which window they
                belong to. The handler cannot take the lock that says which
                window has focus, and cannot follow a pointer to one that may
                be freed underneath it, so it records here and the thread
                delivers.
        */
        struct window_key key_ring[WINDOW_KEYS];
        atomic_t key_head;
        atomic_t key_tail;
        atomic_t modifiers;

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

static void canvas_thread_start(void);
static void canvas_thread_stop(void);
static void canvas_thread_wake(void);
static void desktop_redraw(void);
static void desktop_repaint(void);
static void rect_set(struct drm_rect *rect, int x, int y, int w, int h);
static void desktop_watch(void);
static void cursor_move(int x, int y);

// Nanoseconds from an event arriving to the cursor being on screen.
static u64 pointer_latency_total;
static u64 pointer_latency_worst;
static unsigned long pointer_events;
static u64 pointer_queue_total;
static u64 pointer_draw_total;
static u64 pointer_flush_total;
static unsigned long pointer_counts;
static unsigned long pointer_moved;

// What drawing costs: passes over every output, the time in them, and the
// pixels written, which against the size of the desktop is the overdraw.
static unsigned long canvas_composes;
static u64 canvas_compose_ns;
static unsigned long canvas_painted;
static unsigned long canvas_runs;
static u64 canvas_flush_ns;
static u64 canvas_text_ns;

/*
        The two loops every pixel goes through, in fill.asm. They are
        assembly because a full compose is four megabytes of stores and the
        kernel is built with no vector instructions on x86, so what the C
        turned into was two four byte stores an iteration.
*/
void canvas_row_fill(u32 *at, unsigned long count, u32 colour);
void canvas_rect_fill(u32 *at, unsigned long pitch, unsigned long width,
                      unsigned long height, u32 colour);
void canvas_glyph(u32 *at, unsigned long pitch, const u8 *bits,
                  unsigned long stride, unsigned long rows, u32 colour);
void canvas_row_blit(u32 *at, const u32 *from, unsigned long count, u32 opaque);

/*
        Somewhere to draw, and the only thing the drawing code is given.

        It used to take a pixel pointer, a pitch, an output and a clip, four
        arguments threaded through every function down to the innermost loop,
        and the innermost loop was the only place all four were wanted. Now
        the caller assembles one of these and the drawing knows nothing about
        outputs, formats or windows.

        The origin is where the target sits on the desktop, so drawing can be
        in desktop coordinates and land in target ones. The clip is the damage,
        in target coordinates.
*/
struct target
{
        u32 *pixels;
        unsigned int pitch;
        int width, height;
        int x, y;
        u32 opaque;
        const u32 *ink;
        struct drm_rect clip;
};

static void target_row(const struct target *t, int y, int x1, int x2, u32 colour);

static struct canvas *canvas_from_client(struct drm_client_dev *client)
{
        return container_of(client, struct canvas, client);
}

static _Bool rects_overlap(int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh)
{
        return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

// The border and titlebar a framed window wears, and nothing when it does not.
static void pane_frame(struct pane *pane, int *x, int *y, int *w, int *h)
{
        int title = pane->style & WINDOW_FRAME ? WINDOW_TITLE : 0;
        int border = pane->style & WINDOW_FRAME ? 2 : 0;

        *x = pane->x - border;
        *y = pane->y - border;
        *w = pane->width + border * 2;
        *h = pane->height + title + border * 3;
}

static _Bool pane_titlebar_holds(struct pane *pane, int x, int y)
{
        return x >= pane->x && x < pane->x + pane->width &&
               y >= pane->y && y < pane->y + WINDOW_TITLE;
}

// Which edges of a window's frame a point is close enough to take hold of.
#define EDGE_LEFT 1u
#define EDGE_RIGHT 2u
#define EDGE_TOP 4u
#define EDGE_BOTTOM 8u
#define EDGE_GRIP 6

static _Bool output_holds(struct output *output, int x, int y)
{
        return x >= output->x && x < output->x + (int)output->width &&
               y >= output->y && y < output->y + (int)output->height;
}

#include "paint.c"
#include "text.c"
#include "pane.c"
#include "compose.c"
#include "plane.c"
#include "drag.c"
#include "output.c"
#include "keys.c"
#include "client.c"
#include "pointer.c"
