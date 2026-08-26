/*
        Moonwater Canvas

        An in-kernel compositor, on the DRM client API the framebuffer console
        and drm_log already use. Attaches without patching the kernel: a DRM
        device can be opened like any other file, and struct drm_file leads
        back to the drm_device behind it.

            paint.c     pixels: a pointer, a pitch, a rectangle
            compose.c   what a window looks like and in what order
            plane.c     the hardware cursor plane
            output.c    one screen, and starting and stopping all of them
            client.c    attaching to DRM, and finding cards to attach to
            pointer.c   input

        Included rather than compiled apart, so that order is a dependency
        order. Included by core.c in turn, which is where the headers are, and
        before library.c, which redefines bool and defines "end" as a macro.

        Nothing here has a fixed maximum. Windows, outputs and cards are lists,
        and the cursor plane is sized from what the device reports.
*/

#define log_canvas(fmt, ...) pr_info("[moonwater canvas] " fmt, ##__VA_ARGS__)

#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

struct window
{
        struct list_head link;
        int x, y;
        int width, height;
};

struct surface
{
        struct list_head link;
        struct drm_client_buffer *buffer;
        struct drm_mode_set *mode_set;
        unsigned int width, height;
        u32 format;

        // Null means the cursor is drawn into the framebuffer instead.
        struct drm_plane *cursor_plane;
        struct drm_client_buffer *cursor_buffer;
        unsigned int cursor_w, cursor_h;
};

struct canvas
{
        struct list_head link;
        struct kref ref;
        struct drm_client_dev client;
        struct mutex lock;

        struct list_head outputs;
        struct list_head windows;

        int cursor_x, cursor_y;
        _Bool started;

        // Written by the input handler in atomic context, read by the thread.
        atomic_t pending_x;
        atomic_t pending_y;
        atomic_t motion_pending;
        u64 motion_stamp;

        int drawn_x, drawn_y;

        // What the input handler clamps against. It cannot take the mutex, so
        // it cannot walk the output list; a stale value clamps to the wrong
        // edge for an instant.
        int screen_w, screen_h;
};

static DEFINE_MUTEX(canvas_list_lock);
static LIST_HEAD(canvas_list);

/*
        The pointer is single, so it is on one canvas at a time. The input
        handler reads this in atomic context and outlives any one canvas, so it
        is RCU published and the readers take a reference before sleeping.
*/
static struct canvas __rcu *pointer_canvas;

static void pointer_start(void);
static void pointer_stop(void);

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

static struct surface *canvas_first_output(struct canvas *canvas)
{
        return list_first_entry_or_null(&canvas->outputs, struct surface, link);
}

static void canvas_destroy(struct kref *ref)
{
        struct canvas *canvas = container_of(ref, struct canvas, ref);

        mutex_destroy(&canvas->lock);
        kfree(canvas);
}

static void canvas_put(struct canvas *canvas)
{
        kref_put(&canvas->ref, canvas_destroy);
}

// Null once the canvas is on its way out, so callers that can sleep must check.
static struct canvas *pointer_canvas_get(void)
{
        struct canvas *canvas;

        rcu_read_lock();
        canvas = rcu_dereference(pointer_canvas);
        if (canvas && !kref_get_unless_zero(&canvas->ref))
                canvas = NULL;
        rcu_read_unlock();

        return canvas;
}

#include "paint.c"
#include "compose.c"
#include "plane.c"
#include "output.c"
#include "client.c"
#include "pointer.c"
