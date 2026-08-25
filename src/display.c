/*
        Moonwater display

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
        the compositor opens /dev/dri/card0, takes the device, and closes the
        file again -- the client it registered keeps its own reference.

        The retry loop is there because the device node appears when devtmpfs
        is mounted, which is after the initcalls that could otherwise have
        started this.

        Included by core.c rather than compiled on its own, so the
        headers it needs are pulled in there, and every symbol carries a
        display prefix to stay clear of the rest of that file. _Bool is
        spelled out where the kernel expects it, because library.c redefines
        bool as an 8 bit integer. They have to come before
        library.c: that file defines "end" as a macro, and asm/io.h uses the
        same word as a variable name.

        What is here so far is the surface and the composition pass: a
        background, a stack of windows drawn back to front, and a cursor. It
        composites in software into one dumb buffer, which is the simplest
        thing that can work everywhere. Moving windows and the cursor onto
        their own hardware planes comes later and is what makes it cheap: an
        atomic commit can move a cursor plane with no drawing at all.
*/

#define log_d(fmt, ...) pr_info("[moonwater/display] " fmt, ##__VA_ARGS__)

#define MAX_WINDOWS 8

/*
        Input

        This is the reason for putting the compositor in the kernel at all. A
        userspace display server sees a mouse move as: interrupt, input core,
        wake the server, the server reads the event, composites, and asks the
        kernel to move the cursor. Every one of those arrows is a context
        switch.

        An input handler registered here is called by the input core directly,
        in the same path that received the event. There is one handoff left
        and it is unavoidable: input events arrive in atomic context and a DRM
        commit can sleep, so the position is taken immediately and the update
        runs on a high priority worker.
*/
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

// Colours are written as plain xrgb8888 and converted once per surface.
#define COLOUR_DESKTOP 0x1b2733
#define COLOUR_FRAME 0x2f3f52
#define COLOUR_TITLE 0x4c6785
#define COLOUR_BODY 0x101820
#define COLOUR_CURSOR 0xffffff
#define COLOUR_CURSOR_EDGE 0x000000

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

struct display {
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
static struct display *display_active;

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

static struct display *display_from_client(struct drm_client_dev *client)
{
        return container_of(client, struct display, client);
}

/*
        Only 32 bit little endian xrgb/argb is handled for now. Every device
        this targets offers one of them for a dumb buffer, and pretending to
        support formats that are not tested would be worse than refusing them.
*/
static _Bool display_format_supported(u32 format)
{
        return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888;
}

static u32 display_pick_format(struct drm_plane *plane)
{
        unsigned int i;

        for (i = 0; i < plane->format_count; i++)
                if (display_format_supported(plane->format_types[i]))
                        return plane->format_types[i];

        return DRM_FORMAT_INVALID;
}

static void display_fill_rect(u32 *pixels, unsigned int pitch_pixels,
                           unsigned int surface_w, unsigned int surface_h,
                           int x, int y, int width, int height, u32 colour)
{
        int row, column;

        // Clip rather than trusting callers: a window dragged off the edge is
        // the normal case, not an error.
        if (x < 0) {
                width += x;
                x = 0;
        }

        if (y < 0) {
                height += y;
                y = 0;
        }

        if (x + width > (int)surface_w)
                width = (int)surface_w - x;

        if (y + height > (int)surface_h)
                height = (int)surface_h - y;

        if (width <= 0 || height <= 0)
                return;

        for (row = 0; row < height; row++) {
                u32 *line = pixels + (size_t)(y + row) * pitch_pixels + x;

                for (column = 0; column < width; column++)
                        line[column] = colour;
        }
}

/*
        An arrow, as a bitmap. Drawn in software for now; this is exactly what
        a hardware cursor plane exists to avoid, and moving it there is the
        next step.
*/
#define CURSOR_W 12
#define CURSOR_H 19

/*
        The hardware cursor buffer is square and larger than the arrow drawn
        into it. 64x64 is the one size every cursor plane accepts -- older
        display engines require a square power of two, and some accept nothing
        else -- so the arrow sits in the top left corner and the rest of the
        buffer is transparent.
*/
#define CURSOR_PLANE_W 64
#define CURSOR_PLANE_H 64

static const char display_cursor_bitmap[CURSOR_H][CURSOR_W + 1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X.X   ",
    "      XXX   ",
    "            ",
};

static void display_draw_cursor(u32 *pixels, unsigned int pitch_pixels,
                             unsigned int surface_w, unsigned int surface_h,
                             int x, int y, u32 fill, u32 edge)
{
        int row, column;

        for (row = 0; row < CURSOR_H; row++) {
                int py = y + row;

                if (py < 0 || py >= (int)surface_h)
                        continue;

                for (column = 0; column < CURSOR_W; column++) {
                        int px = x + column;
                        char pixel = display_cursor_bitmap[row][column];

                        if (pixel == ' ')
                                continue;

                        if (px < 0 || px >= (int)surface_w)
                                continue;

                        pixels[(size_t)py * pitch_pixels + px] =
                            (pixel == 'X') ? edge : fill;
                }
        }
}

// xrgb8888 is the source of truth; argb differs only in the alpha byte.
static u32 display_colour(u32 xrgb, u32 format)
{
        if (format == DRM_FORMAT_ARGB8888)
                return xrgb | 0xff000000;

        return xrgb;
}

/*
        Composes one rectangle rather than the whole screen.

        Moving the cursor dirties two small areas: where it was and where it
        is. Repainting a 1280x800 desktop for a mouse move would be about a
        megabyte of writes per event; this is a few kilobytes.
*/
static void display_compose_rect(struct display *display,
                                      struct surface *surface,
                                      u32 *pixels, unsigned int pitch_pixels,
                                      int rx, int ry, int rw, int rh)
{
        unsigned int i;

        display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               rx, ry, rw, rh,
                               display_colour(COLOUR_DESKTOP, surface->format));

        for (i = 0; i < MAX_WINDOWS; i++) {
                struct window *window = &display->windows[i];
                int fx, fy, fw, fh;

                if (!window->present)
                        continue;

                fx = window->x - 2;
                fy = window->y - 2;
                fw = window->width + 4;
                fh = window->height + 26;

                // Skip windows that cannot touch the damaged area at all.
                if (fx >= rx + rw || fx + fw <= rx || fy >= ry + rh || fy + fh <= ry)
                        continue;

                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       fx, fy, fw, fh,
                                       display_colour(COLOUR_FRAME, surface->format));
                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y, window->width, 20,
                                       display_colour(COLOUR_TITLE, surface->format));
                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y + 20, window->width, window->height,
                                       display_colour(COLOUR_BODY, surface->format));
        }
}

/*
        The hardware cursor.

        A cursor plane is composited by the display engine during scanout, so
        moving it touches no pixels at all: the commit carries a position and
        nothing else. That is the whole point. Drawing the cursor into the
        framebuffer means handing the driver a damage rectangle, and on an
        atomic driver that is a full commit that waits for vblank -- a cursor
        move cannot land in less than a frame however little is drawn.

        The path out is one the kernel already has. drm_atomic_helper_update_plane
        sets legacy_cursor_update on the state whenever the plane being updated
        is the crtc's cursor, and the commit helper then completes the flip
        immediately rather than waiting: "Legacy cursor updates are fully
        unsynced". So going through plane->funcs->update_plane on crtc->cursor
        is both the hardware plane and the unsynced commit, without asking for
        either by name.
*/
static _Bool display_plane_takes_argb(struct drm_plane *plane)
{
        unsigned int i;

        for (i = 0; i < plane->format_count; i++)
                if (plane->format_types[i] == DRM_FORMAT_ARGB8888)
                        return true;

        return false;
}

/*
        Places the cursor plane at a position, creating no damage and drawing
        nothing.

        The lock dance is the one drm_mode_cursor_common does: take the crtc
        and the plane, and back off and retry the whole thing on -EDEADLK. The
        retry has to cover update_plane as well as the two locks above it,
        because it takes more locks of its own on the way to the commit.

        Uninterruptible, unlike the ioctl. That path is a syscall and wants to
        return to a signal handler; this one is a worker with no signals to
        take, where -ERESTARTSYS would only mean a dropped mouse move.
*/
static int display_place_cursor_plane(struct surface *surface, int x, int y)
{
        struct drm_plane *plane = surface->cursor_plane;
        struct drm_crtc *crtc = surface->mode_set->crtc;
        struct drm_modeset_acquire_ctx ctx;
        int ret;

        drm_modeset_acquire_init(&ctx, 0);
retry:
        ret = drm_modeset_lock(&crtc->mutex, &ctx);
        if (ret)
                goto out;

        ret = drm_modeset_lock(&plane->mutex, &ctx);
        if (ret)
                goto out;

        ret = plane->funcs->update_plane(plane, crtc, surface->cursor_buffer->fb,
                                         x - CURSOR_HOTSPOT_X,
                                         y - CURSOR_HOTSPOT_Y,
                                         CURSOR_PLANE_W, CURSOR_PLANE_H,
                                         0, 0,
                                         CURSOR_PLANE_W << 16,
                                         CURSOR_PLANE_H << 16,
                                         &ctx);
out:
        if (ret == -EDEADLK) {
                drm_modeset_backoff(&ctx);
                goto retry;
        }

        drm_modeset_drop_locks(&ctx);
        drm_modeset_acquire_fini(&ctx);

        return ret;
}

static void display_drop_cursor_plane(struct surface *surface)
{
        surface->cursor_plane = NULL;

        if (surface->cursor_buffer) {
                drm_client_buffer_delete(surface->cursor_buffer);
                surface->cursor_buffer = NULL;
        }
}

/*
        Claims the crtc's cursor plane and paints the arrow into it once.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer somewhere else only uploads it when the
        plane's framebuffer changes -- virtio-gpu transfers the image on that
        edge and sends nothing but a position afterwards, which is exactly the
        behaviour that makes this cheap. An image drawn after arming would
        never be sent.
*/
static int display_setup_cursor_plane(struct drm_client_dev *client,
                                           struct surface *surface)
{
        struct drm_plane *plane = surface->mode_set->crtc->cursor;
        struct iosys_map map;
        unsigned int pitch_pixels;

        if (!plane || !display_plane_takes_argb(plane))
                return -ENODEV;

        surface->cursor_buffer = drm_client_buffer_create_dumb(
            client, CURSOR_PLANE_W, CURSOR_PLANE_H, DRM_FORMAT_ARGB8888);
        if (IS_ERR(surface->cursor_buffer)) {
                surface->cursor_buffer = NULL;
                return -ENOMEM;
        }

        if (drm_client_buffer_vmap_local(surface->cursor_buffer, &map)) {
                drm_client_buffer_delete(surface->cursor_buffer);
                surface->cursor_buffer = NULL;
                return -EIO;
        }

        pitch_pixels = surface->cursor_buffer->fb->pitches[0] / sizeof(u32);

        // Transparent everywhere the arrow does not cover. draw_cursor skips
        // the blank cells of its bitmap, so without this the buffer keeps
        // whatever it was allocated holding and the arrow wears a black box.
        display_fill_rect(map.vaddr, pitch_pixels,
                               CURSOR_PLANE_W, CURSOR_PLANE_H,
                               0, 0, CURSOR_PLANE_W, CURSOR_PLANE_H,
                               0x00000000);

        display_draw_cursor(map.vaddr, pitch_pixels,
                                 CURSOR_PLANE_W, CURSOR_PLANE_H, 0, 0,
                                 display_colour(COLOUR_CURSOR, DRM_FORMAT_ARGB8888),
                                 display_colour(COLOUR_CURSOR_EDGE, DRM_FORMAT_ARGB8888));

        drm_client_buffer_vunmap_local(surface->cursor_buffer);

        surface->cursor_plane = plane;
        return 0;
}

/*
        Puts the cursor plane back after a modeset.

        Not optional and not a formality: drm_client_modeset_commit disables
        every non-primary plane on the device before it commits, so that a
        client inheriting the display does not also inherit an overlay left
        behind by the last one. Our cursor is one of those planes. Every
        commit turns it off, and this is what turns it back on.
*/
static void display_arm_cursor(struct display *display)
{
        struct surface *surface;
        int ret;

        /*
                cursor_x is the position to arm at, and it is written by the
                worker just after it moves the plane. Both run under
                display->lock, so this cannot read a position the plane has
                already moved past -- a second commit site that did not hold
                the lock would make the pointer snap back on every redraw.
        */

        if (!display->surface_count)
                return;

        surface = &display->surfaces[0];
        if (!surface->cursor_plane)
                return;

        ret = display_place_cursor_plane(surface, display->cursor_x, display->cursor_y);
        if (!ret)
                return;

        // Give it up rather than leave a cursor that cannot move. The next
        // pointer event repaints through the software path, which needs
        // nothing from the display beyond a framebuffer.
        log_d("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
        display_drop_cursor_plane(surface);
}

/*
        Moves the cursor without repainting the screen.

        Two damage rectangles -- the old position and the new -- are recomposed
        and the cursor is drawn at the new one. drm_client_buffer_flush tells
        the driver which region changed, so a device that uploads its
        framebuffer only sends those bytes.
*/
static void display_move_cursor(struct display *display,
                                     struct surface *surface,
                                     int new_x, int new_y)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        struct drm_rect damage;
        int old_x = display->drawn_x;
        int old_y = display->drawn_y;

        /*
                On a display with a cursor plane none of the rest of this
                happens. No pixels are read, none are written, and no damage
                is handed to the driver -- the commit carries two coordinates
                and returns without waiting for vblank.
        */
        if (surface->cursor_plane) {
                u64 flush_started = ktime_get_ns();
                int ret = display_place_cursor_plane(surface, new_x, new_y);

                pointer_flush_total += ktime_get_ns() - flush_started;

                if (!ret)
                        return;

                log_d("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
                display_drop_cursor_plane(surface);
        }

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        {
        u64 draw_started = ktime_get_ns();

        display_compose_rect(display, surface, pixels, pitch_pixels,
                                  old_x, old_y, CURSOR_W, CURSOR_H);
        display_compose_rect(display, surface, pixels, pitch_pixels,
                                  new_x, new_y, CURSOR_W, CURSOR_H);

        display_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                 new_x, new_y,
                                 display_colour(COLOUR_CURSOR, surface->format),
                                 display_colour(COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);

        pointer_draw_total += ktime_get_ns() - draw_started;
        }

        // Clamped: the cursor is allowed to sit against the right or bottom
        // edge, so the rectangle around it would otherwise reach past the
        // surface and be handed to the driver that way.
        damage.x1 = max(min(old_x, new_x), 0);
        damage.y1 = max(min(old_y, new_y), 0);
        damage.x2 = min(max(old_x, new_x) + CURSOR_W, (int)surface->width);
        damage.y2 = min(max(old_y, new_y) + CURSOR_H, (int)surface->height);

        if (damage.x2 <= damage.x1 || damage.y2 <= damage.y1)
                return;

        {
        u64 flush_started = ktime_get_ns();

        /*
                Required, not optional. Skipping it was tried: the cursor's
                position updated internally but the screen kept showing it
                where it was, because this driver shadows the framebuffer
                rather than scanning out what we wrote to.

                It is also the whole cost. The driver implements dirty as
                drm_atomic_helper_dirtyfb, a full atomic commit that waits for
                vblank, so a cursor move cannot land in less than a frame. A
                hardware cursor plane is what avoids this -- moving one does
                not touch the framebuffer at all -- but this device does not
                have one.
        */
        drm_client_buffer_flush(surface->buffer, &damage);

        pointer_flush_total += ktime_get_ns() - flush_started;
        }
}

static void display_compose(struct display *display, struct surface *surface)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        unsigned int i;

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                       0, 0, surface->width, surface->height,
                       display_colour(COLOUR_DESKTOP, surface->format));

        // Back to front, so a later window overlaps an earlier one.
        for (i = 0; i < MAX_WINDOWS; i++) {
                struct window *window = &display->windows[i];

                if (!window->present)
                        continue;

                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x - 2, window->y - 2,
                               window->width + 4, window->height + 26,
                               display_colour(COLOUR_FRAME, surface->format));

                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y, window->width, 20,
                               display_colour(COLOUR_TITLE, surface->format));

                display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y + 20, window->width, window->height,
                               display_colour(COLOUR_BODY, surface->format));
        }

        // Skipped when the cursor lives on its own plane, or the arrow would
        // be baked into the desktop underneath the real one and left behind
        // wherever the pointer last was.
        if (!surface->cursor_plane)
                display_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                 display->cursor_x, display->cursor_y,
                                 display_colour(COLOUR_CURSOR, surface->format),
                                 display_colour(COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);
}

static void display_redraw(struct display *display)
{
        unsigned int i;

        for (i = 0; i < display->surface_count; i++)
                display_compose(display, &display->surfaces[i]);

        drm_client_modeset_commit(&display->client);
        display_arm_cursor(display);
}

static int display_setup_surface(struct drm_client_dev *client,
                              struct drm_mode_set *mode_set,
                              struct surface *surface)
{
        struct drm_crtc *crtc = mode_set->crtc;
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = display_pick_format(crtc->primary);

        if (format == DRM_FORMAT_INVALID) {
                log_d("no 32 bit format on this plane, skipping output\n");
                return -EINVAL;
        }

        surface->buffer = drm_client_buffer_create_dumb(client, width, height, format);
        if (IS_ERR(surface->buffer)) {
                log_d("could not create a %ux%u scanout buffer\n", width, height);
                surface->buffer = NULL;
                return -ENOMEM;
        }

        surface->mode_set = mode_set;
        surface->width = width;
        surface->height = height;
        surface->format = format;
        mode_set->fb = surface->buffer->fb;

        log_d("output %ux%u ready\n", width, height);
        return 0;
}

static unsigned int display_count_modesets(struct drm_client_dev *client)
{
        struct drm_mode_set *mode_set;
        unsigned int count = 0;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
                count++;
        mutex_unlock(&client->modeset_mutex);

        return count;
}

// A first arrangement, so there is something recognisable on screen before
// anything can create a window.
static void display_seed_windows(struct display *display,
                              unsigned int width, unsigned int height)
{
        display->windows[0] = (struct window){
            .x = width / 10, .y = height / 8,
            .width = width / 3, .height = height / 3, .present = true};

        display->windows[1] = (struct window){
            .x = width / 3, .y = height / 3,
            .width = width / 3, .height = height / 3, .present = true};

        display->cursor_x = width / 2;
        display->cursor_y = height / 2;
}

static int display_start(struct display *display)
{
        struct drm_client_dev *client = &display->client;
        struct drm_mode_set *mode_set;
        unsigned int max_surfaces;
        unsigned int count = 0;

        if (drm_client_modeset_probe(client, 0, 0))
                return -ENODEV;

        max_surfaces = display_count_modesets(client);
        if (!max_surfaces)
                return -ENODEV;

        display->surfaces = kcalloc(max_surfaces, sizeof(*display->surfaces), GFP_KERNEL);
        if (!display->surfaces)
                return -ENOMEM;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client) {
                if (!mode_set->mode)
                        continue;

                if (display_setup_surface(client, mode_set, &display->surfaces[count]))
                        continue;

                count++;
        }
        mutex_unlock(&client->modeset_mutex);

        if (!count) {
                kfree(display->surfaces);
                display->surfaces = NULL;
                return -ENODEV;
        }

        display->surface_count = count;
        display_seed_windows(display, display->surfaces[0].width,
                          display->surfaces[0].height);

        display->screen_w = (int)display->surfaces[0].width;
        display->screen_h = (int)display->surfaces[0].height;

        atomic_set(&display->pending_x, display->cursor_x);
        atomic_set(&display->pending_y, display->cursor_y);
        display->drawn_x = display->cursor_x;
        display->drawn_y = display->cursor_y;

        if (display_setup_cursor_plane(client, &display->surfaces[0]))
                log_d("no hardware cursor here, drawing the cursor into the framebuffer\n");
        else
                log_d("cursor on hardware plane %u\n",
                      display->surfaces[0].cursor_plane->base.id);

        display_active = display;
        display_redraw(display);

        log_d("compositing on %u output(s)\n", count);
        return 0;
}

static void display_release(struct display *display)
{
        unsigned int i;

        for (i = 0; i < display->surface_count; i++) {
                display_drop_cursor_plane(&display->surfaces[i]);

                if (display->surfaces[i].buffer)
                        drm_client_buffer_delete(display->surfaces[i].buffer);
        }

        kfree(display->surfaces);
        display->surfaces = NULL;
        display->surface_count = 0;
}

static void client_unregister(struct drm_client_dev *client)
{
        struct display *display = display_from_client(client);

        /*
                Order matters. The input handler and its worker reach the
                display through display_active, so that has to stop
                pointing at it before anything it owns is freed, and the
                worker has to be known finished rather than merely asked to
                stop. Getting this wrong is a use after free on every mouse
                move after a device goes away.
        */
        if (display_active == display) {
                pointer_stop();
                display_active = NULL;
        }

        mutex_lock(&display->lock);
        display->started = 0;
        display_release(display);
        mutex_unlock(&display->lock);

        drm_client_release(client);
}

static void client_free(struct drm_client_dev *client)
{
        struct display *display = display_from_client(client);

        mutex_destroy(&display->lock);
        kfree(display);
}

static int client_hotplug(struct drm_client_dev *client)
{
        struct display *display = display_from_client(client);
        int ret = 0;

        mutex_lock(&display->lock);

        if (!display->started) {
                ret = display_start(display);
                display->started = (ret == 0);
        } else {
                display_redraw(display);
        }

        mutex_unlock(&display->lock);
        return ret;
}

// The bool argument is whether the restore happens from an atomic context.
static int client_restore(struct drm_client_dev *client, _Bool in_atomic)
{
        struct display *display = display_from_client(client);

        // Nothing here is safe to do without sleeping, so an atomic restore
        // is declined rather than half performed.
        if (in_atomic)
                return -EBUSY;

        mutex_lock(&display->lock);
        if (display->started)
                display_redraw(display);
        mutex_unlock(&display->lock);

        return 0;
}

static const struct drm_client_funcs client_funcs = {
    .owner = THIS_MODULE,
    .unregister = client_unregister,
    .free = client_free,
    .restore = client_restore,
    .hotplug = client_hotplug,
};

/*
        Claims a DRM device. Reports whether it was taken, so the replacement
        for drm_client_setup can fall through to the original for anything we
        decline -- a device with no modesetting, or an allocation that failed.
*/
static int display_take_over(struct drm_device *dev)
{
        struct display *display;

        if (!drm_core_check_feature(dev, DRIVER_MODESET))
                return 0;

        /*
                One display, once. A machine can easily present two cards --
                the first boot on real hardware had virtio-gpu and a standard
                VGA both probing -- and taking the second would register the
                input handler twice, leak the first workqueue, and leave
                display_active pointing at whichever won the race.
        */
        if (display_active)
                return 0;

        display = kzalloc(sizeof(*display), GFP_KERNEL);
        if (!display)
                return 0;

        mutex_init(&display->lock);

        if (drm_client_init(dev, &display->client, "moonwater", &client_funcs)) {
                mutex_destroy(&display->lock);
                kfree(display);
                return 0;
        }

        drm_client_register(&display->client);
        log_d("attached to %s\n", dev->driver->name);

        pointer_start();
        return 1;
}

/*
        Finding a device to draw on.

        Opening the node runs the driver's open path and hands back a
        drm_file, which knows its minor, which knows its device. The file is
        closed immediately: drm_client_init takes its own reference, so the
        device outlives our brief handle on it.
*/
#define SPARK_DISPLAY_NODE "/dev/dri/card0"
// Retried fast: the node appears the moment devtmpfs is mounted, and every
// millisecond spent waiting after that is a millisecond of black screen.
#define SPARK_DISPLAY_RETRY_MS 5
#define SPARK_DISPLAY_ATTEMPTS 1000

static struct delayed_work display_probe_work;
static unsigned int display_attempts;

static int display_claim(const char *path)
{
        struct file *filp;
        struct drm_file *file_priv;
        struct drm_device *dev;
        int taken;

        filp = filp_open(path, O_RDWR, 0);
        if (IS_ERR(filp))
                return PTR_ERR(filp);

        file_priv = filp->private_data;

        if (!file_priv || !file_priv->minor || !file_priv->minor->dev) {
                filp_close(filp, NULL);
                return -ENODEV;
        }

        dev = file_priv->minor->dev;
        taken = display_take_over(dev);

        filp_close(filp, NULL);

        return taken ? 0 : -EBUSY;
}

static void display_probe(struct work_struct *work)
{
        int ret = display_claim(SPARK_DISPLAY_NODE);

        if (ret == 0)
                return;

        if (++display_attempts >= SPARK_DISPLAY_ATTEMPTS) {
                log_d("gave up waiting for %s (%d)\n", SPARK_DISPLAY_NODE, ret);
                return;
        }

        schedule_delayed_work(&display_probe_work,
                              msecs_to_jiffies(SPARK_DISPLAY_RETRY_MS));
}

static void display_start_probing(void)
{
        INIT_DELAYED_WORK(&display_probe_work, display_probe);

        // No initial delay. The display driver has already probed by the time
        // this runs, so the first attempt usually succeeds outright.
        schedule_delayed_work(&display_probe_work, 0);
}

/*
        Input to cursor

        input_handler.event is called by the input core in the path that
        received the event, so nothing is woken to notice a mouse move. The
        position is updated there and then; the pixels cannot be, because that
        context cannot sleep and a DRM commit can. A high priority worker does
        that part, and the gap between the two is what gets measured.
*/
static struct task_struct *pointer_thread;
static void pointer_apply(void);

/*
        How long the processor is allowed to take waking up.

        The largest cost in the path from a pointer moving to the thread that
        draws it is the processor coming back from an idle state, and the
        deeper the state the longer that takes -- a cursor moving after a pause
        is exactly the case that pays it.

        cpu_latency_qos is how a driver says so. It is the generic interface
        cpuidle governors already honour, so it holds on every architecture
        with cpuidle rather than only where a boot argument exists: x86 has
        intel_idle.max_cstate, arm64 has cpuidle-psci and no such argument,
        and this reaches both.

        Held for as long as there is a pointer, rather than taken around each
        movement. Taking it on the way in would be exactly backwards: the move
        that pays the wakeup is the first one after a pause, and at that moment
        the request has not been made yet.
*/
static struct pm_qos_request pointer_qos;

// Nanoseconds from the event arriving to the cursor being on screen.
// Declared with the rest of the timing counters near the top of the file.

static void pointer_apply(void)
{
        struct display *display = display_active;
        int x, y;
        u64 started;

        if (!display)
                return;

        if (!atomic_xchg(&display->motion_pending, 0))
                return;

        started = display->motion_stamp;
        x = atomic_read(&display->pending_x);
        y = atomic_read(&display->pending_y);

        if (started)
                pointer_queue_total += ktime_get_ns() - started;

        mutex_lock(&display->lock);

        if (display->started && display->surface_count) {
                display_move_cursor(display, &display->surfaces[0], x, y);
                display->drawn_x = x;
                display->drawn_y = y;
                display->cursor_x = x;
                display->cursor_y = y;
        }

        mutex_unlock(&display->lock);

        if (started) {
                u64 elapsed = ktime_get_ns() - started;

                pointer_latency_total += elapsed;
                pointer_events++;

                if (elapsed > pointer_latency_worst)
                        pointer_latency_worst = elapsed;
        }
}

static void pointer_event(struct input_handle *handle, unsigned int type,
                             unsigned int code, int value)
{
        struct display *display = display_active;
        int x, y, limit;

        if (!display || !display->started || !display->screen_w)
                return;

        x = atomic_read(&display->pending_x);
        y = atomic_read(&display->pending_y);

        if (type == EV_REL) {
                if (code == REL_X)
                        x += value;
                else if (code == REL_Y)
                        y += value;
                else
                        return;
        } else if (type == EV_ABS) {
                // Absolute devices report in their own range, so scale into
                // the screen. QEMU's tablet is one of these.
                struct input_absinfo *abs;

                if (code != ABS_X && code != ABS_Y)
                        return;

                abs = &handle->dev->absinfo[code];

                if (abs->maximum <= abs->minimum)
                        return;

                if (code == ABS_X)
                        x = (int)div_u64((u64)(value - abs->minimum) *
                                         (u32)display->screen_w,
                                         abs->maximum - abs->minimum);
                else
                        y = (int)div_u64((u64)(value - abs->minimum) *
                                         (u32)display->screen_h,
                                         abs->maximum - abs->minimum);
        } else {
                return;
        }

        limit = display->screen_w - 1;
        x = clamp(x, 0, limit);
        limit = display->screen_h - 1;
        y = clamp(y, 0, limit);

        atomic_set(&display->pending_x, x);
        atomic_set(&display->pending_y, y);

        // Stamp only the first event of a burst, so the measurement is the age
        // of the oldest movement not yet on screen.
        if (!atomic_xchg(&display->motion_pending, 1))
                display->motion_stamp = ktime_get_ns();

        /*
                A wake, not a queue. The work this does is one atomic commit
                that returns without waiting, so the machinery a workqueue
                brings -- a pool, a dispatch, a kworker that is still an
                ordinary task -- is all overhead around it. A thread of our
                own, at a real time priority, is woken and runs.
        */
        wake_up_process(pointer_thread);
}

static int pointer_connect(struct input_handler *handler, struct input_dev *dev,
                              const struct input_device_id *id)
{
        struct input_handle *handle;
        int ret;

        handle = kzalloc(sizeof(*handle), GFP_KERNEL);
        if (!handle)
                return -ENOMEM;

        handle->dev = dev;
        handle->handler = handler;
        handle->name = "moonwater";

        ret = input_register_handle(handle);
        if (ret)
                goto err_free;

        ret = input_open_device(handle);
        if (ret)
                goto err_unregister;

        log_d("pointer: %s\n", dev->name ? dev->name : "unnamed");
        return 0;

err_unregister:
        input_unregister_handle(handle);
err_free:
        kfree(handle);
        return ret;
}

static void pointer_disconnect(struct input_handle *handle)
{
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(handle);
}

// Anything that reports relative or absolute motion: mice, tablets, touchpads.
static const struct input_device_id pointer_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_REL)},
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_ABS)},
    },
    {},
};

static struct input_handler pointer_handler = {
    .event = pointer_event,
    .connect = pointer_connect,
    .disconnect = pointer_disconnect,
    .name = "moonwater",
    .id_table = pointer_ids,
};

/*
        Unregistering the handler stops new events; cancelling the work waits
        for the one that may already be running. Both have to happen before
        the display it draws through is freed.
*/
static void pointer_stop(void)
{
        if (!pointer_thread)
                return;

        input_unregister_handler(&pointer_handler);
        cpu_latency_qos_remove_request(&pointer_qos);
        kthread_stop(pointer_thread);
        pointer_thread = NULL;
}

/*
        Sleeps until something moves, then draws it.

        set_current_state before the flag is read, which is what makes the
        sleep safe: a wake arriving between the two finds the task already
        marked and schedule() returns at once rather than losing the event.
*/
static int pointer_loop(void *unused)
{
        while (!kthread_should_stop()) {
                set_current_state(TASK_IDLE);

                if (!display_active ||
                    !atomic_read(&display_active->motion_pending))
                        schedule();

                __set_current_state(TASK_RUNNING);
                pointer_apply();
        }

        return 0;
}

static void pointer_start(void)
{
        /*
                A thread rather than a workqueue.

                WQ_HIGHPRI raises a kworker's nice level and leaves it an
                ordinary task, so it is still scheduled against everything
                else running. SCHED_FIFO is a different queue entirely: the
                scheduler picks it before any normal task, which is the whole
                of what this thread is for.

                fifo_low rather than fifo: priority 1 is ahead of every
                SCHED_OTHER task and behind anything the machine considers
                more urgent than a cursor, which is the honest place for it.
        */
        pointer_thread = kthread_run(pointer_loop, NULL, "moonwater/pointer");

        if (IS_ERR(pointer_thread)) {
                log_d("no thread for input\n");
                pointer_thread = NULL;
                return;
        }

        sched_set_fifo_low(pointer_thread);

        // 0 microseconds: no idle state whose exit can be measured.
        cpu_latency_qos_add_request(&pointer_qos, 0);

        if (input_register_handler(&pointer_handler))
                log_d("could not register the input handler\n");
}

// Nanoseconds, for the stats ioctl.
static void display_input_stats(unsigned long *events, unsigned long *mean,
                                 unsigned long *worst, unsigned long *queue,
                                 unsigned long *draw, unsigned long *flush)
{
        unsigned long n = pointer_events ? pointer_events : 1;

        *events = pointer_events;
        *mean = pointer_latency_total / n;
        *worst = pointer_latency_worst;
        *queue = pointer_queue_total / n;
        *draw = pointer_draw_total / n;
        *flush = pointer_flush_total / n;
}
