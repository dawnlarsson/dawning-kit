/*
        Moonwater Canvas

        An in-kernel compositor. The kernel owns the screen directly rather
        than handing a device node to a userspace server, which is closer to
        how Windows draws than to how X11 or Wayland do.

        The substrate is the in-kernel DRM client API -- the same one the
        framebuffer console and drm_log use -- so this is a supported way to
        own a display from kernel space, not a detour around the graphics
        stack. drm_client_buffer_create_dumb gives a scanout buffer, vmap
        gives a pointer to draw through, and modeset_commit puts it on screen.

        It attaches without modifying the kernel at all. drm_client_setup
        dispatches to a fixed set of clients and there is no exported way to
        enumerate DRM devices, but a DRM device can be opened like any other
        file, and struct drm_file leads back to the drm_device behind it. So
        Canvas opens /dev/dri/card0, takes the device, and closes the file
        again -- the client it registered keeps its own reference.

        This file is the shared part: the types, the one live instance, and
        the order the rest goes in. The rest is separate files because they
        are separate problems, not because one file was long:

            paint.c     pixels and nothing above them -- filling a rectangle,
                        converting a colour, the shape of the arrow. Takes a
                        pointer and a pitch and knows no structure here.
            compose.c   what a window looks like and what order they are
                        drawn in. One pass over the scanout buffer.
            plane.c     the hardware cursor plane, and moving the cursor
                        either through it or by recomposing underneath it.
            output.c    one screen: its format, its scanout buffer, its
                        modeset. Starting and stopping all of it, since both
                        are really "for each output".
            client.c    attaching to DRM: taking the device, the callbacks
                        DRM calls back on, the poll that waits for card0.
            pointer.c   input: the handler the input core calls, the thread
                        that applies what it saw, and what that cost.

        They are included rather than compiled apart, so this stays one
        translation unit and the order at the bottom is a real dependency
        order: paint knows nothing, compose uses paint, plane uses compose,
        output uses both, client drives output, pointer drives plane.

        Included by core.c in turn, so the headers all of this needs are
        pulled in there. _Bool is spelled out where the kernel expects it,
        because library.c redefines bool as an 8 bit integer, and all of this
        has to come before library.c: that file defines "end" as a macro, and
        asm/io.h uses the same word as a variable name.
*/

#define log_canvas(fmt, ...) pr_info("[moonwater/canvas] " fmt, ##__VA_ARGS__)

#define MAX_WINDOWS 8

// Where in the cursor bitmap the pointer position actually is. Top left,
// which is what the arrow in paint.c is drawn for.
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

struct window {
        int x, y;
        int width, height;
        _Bool present;
};

struct surface {
        struct drm_client_buffer *buffer;
        struct drm_mode_set *mode_set;
        unsigned int width, height;
        u32 format;

        /*
                The hardware cursor, when the display has one. Null means the
                cursor is drawn into the framebuffer like anything else, which
                is what every path below falls back to.
        */
        struct drm_plane *cursor_plane;
        struct drm_client_buffer *cursor_buffer;
};

struct canvas {
        struct drm_client_dev client;
        struct mutex lock;

        struct surface *surfaces;
        unsigned int surface_count;

        struct window windows[MAX_WINDOWS];
        int cursor_x, cursor_y;

        _Bool started;

        // Written from the input handler in atomic context, read by the
        // worker. Only ever one writer and one reader.
        atomic_t pending_x;
        atomic_t pending_y;
        atomic_t motion_pending;
        u64 motion_stamp;

        int drawn_x, drawn_y;

        /*
                The bounds the input handler clamps against.

                It runs in the input core's context, where it cannot take the
                mutex, so it must not follow the surfaces pointer -- that array
                is freed when a device goes away. These are plain scalars that
                live as long as the display does; reading a stale one clamps
                to the wrong edge for an instant, which is nothing.
        */
        int screen_w, screen_h;
};

// The compositor owns one display at a time. The input handler reaches it
// through this rather than being handed it, since the input core calls us
// with no notion of which screen a pointer belongs to.
static struct canvas *canvas_active;

static void pointer_start(void);
static void pointer_stop(void);

/*
        Timing. Nanoseconds from a pointer event arriving to the cursor being
        on screen, split so the handoff can be told apart from the drawing.
*/
static u64 pointer_latency_total;
static u64 pointer_latency_worst;
static unsigned long pointer_events;
static u64 pointer_queue_total;  // event to the worker starting
static u64 pointer_draw_total;   // composing the two damage rects
static u64 pointer_flush_total;  // handing the damage to the driver

static struct canvas *canvas_from_client(struct drm_client_dev *client)
{
        return container_of(client, struct canvas, client);
}

#include "paint.c"
#include "compose.c"
#include "plane.c"
#include "output.c"
#include "client.c"
#include "pointer.c"
