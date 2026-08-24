/*
        Dawning display

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

        Included by dawning_core.c rather than compiled on its own, so the
        headers it needs are pulled in there, and every symbol carries a
        dawn_display prefix to stay clear of the rest of that file. _Bool is
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

#define log_d(fmt, ...) pr_info("[Dawning display] " fmt, ##__VA_ARGS__)

#define DAWN_MAX_WINDOWS 8

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
#define DAWN_CURSOR_HOTSPOT_X 0
#define DAWN_CURSOR_HOTSPOT_Y 0

// Colours are written as plain xrgb8888 and converted once per surface.
#define DAWN_COLOUR_DESKTOP 0x1b2733
#define DAWN_COLOUR_FRAME 0x2f3f52
#define DAWN_COLOUR_TITLE 0x4c6785
#define DAWN_COLOUR_BODY 0x101820
#define DAWN_COLOUR_CURSOR 0xffffff
#define DAWN_COLOUR_CURSOR_EDGE 0x000000

struct dawn_window {
        int x, y;
        int width, height;
        _Bool present;
};

struct dawn_surface {
        struct drm_client_buffer *buffer;
        struct drm_mode_set *mode_set;
        unsigned int width, height;
        u32 format;
};

struct dawn_display {
        struct drm_client_dev client;
        struct mutex lock;

        struct dawn_surface *surfaces;
        unsigned int surface_count;

        struct dawn_window windows[DAWN_MAX_WINDOWS];
        int cursor_x, cursor_y;

        _Bool started;

        // Written from the input handler in atomic context, read by the
        // worker. Only ever one writer and one reader.
        atomic_t pending_x;
        atomic_t pending_y;
        atomic_t motion_pending;
        u64 motion_stamp;

        int drawn_x, drawn_y;
};

// The compositor owns one display at a time. The input handler reaches it
// through this rather than being handed it, since the input core calls us
// with no notion of which screen a pointer belongs to.
static struct dawn_display *dawn_display_active;

static void dawning_input_start(void);

/*
        Timing. Nanoseconds from a pointer event arriving to the cursor being
        on screen, split so the handoff can be told apart from the drawing.
*/
static u64 dawn_input_latency_total;
static u64 dawn_input_latency_worst;
static unsigned long dawn_input_events;
static u64 dawn_input_queue_total;  // event to the worker starting
static u64 dawn_input_draw_total;   // composing the two damage rects
static u64 dawn_input_flush_total;  // handing the damage to the driver

static struct dawn_display *dawn_display_from_client(struct drm_client_dev *client)
{
        return container_of(client, struct dawn_display, client);
}

/*
        Only 32 bit little endian xrgb/argb is handled for now. Every device
        this targets offers one of them for a dumb buffer, and pretending to
        support formats that are not tested would be worse than refusing them.
*/
static _Bool dawn_display_format_supported(u32 format)
{
        return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888;
}

static u32 dawn_display_pick_format(struct drm_plane *plane)
{
        unsigned int i;

        for (i = 0; i < plane->format_count; i++)
                if (dawn_display_format_supported(plane->format_types[i]))
                        return plane->format_types[i];

        return DRM_FORMAT_INVALID;
}

static void dawn_display_fill_rect(u32 *pixels, unsigned int pitch_pixels,
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
#define DAWN_CURSOR_W 12
#define DAWN_CURSOR_H 19

static const char dawn_display_cursor_bitmap[DAWN_CURSOR_H][DAWN_CURSOR_W + 1] = {
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

static void dawn_display_draw_cursor(u32 *pixels, unsigned int pitch_pixels,
                             unsigned int surface_w, unsigned int surface_h,
                             int x, int y, u32 fill, u32 edge)
{
        int row, column;

        for (row = 0; row < DAWN_CURSOR_H; row++) {
                int py = y + row;

                if (py < 0 || py >= (int)surface_h)
                        continue;

                for (column = 0; column < DAWN_CURSOR_W; column++) {
                        int px = x + column;
                        char pixel = dawn_display_cursor_bitmap[row][column];

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
static u32 dawn_display_colour(u32 xrgb, u32 format)
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
static void dawn_display_compose_rect(struct dawn_display *display,
                                      struct dawn_surface *surface,
                                      u32 *pixels, unsigned int pitch_pixels,
                                      int rx, int ry, int rw, int rh)
{
        unsigned int i;

        dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               rx, ry, rw, rh,
                               dawn_display_colour(DAWN_COLOUR_DESKTOP, surface->format));

        for (i = 0; i < DAWN_MAX_WINDOWS; i++) {
                struct dawn_window *window = &display->windows[i];
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

                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       fx, fy, fw, fh,
                                       dawn_display_colour(DAWN_COLOUR_FRAME, surface->format));
                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y, window->width, 20,
                                       dawn_display_colour(DAWN_COLOUR_TITLE, surface->format));
                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y + 20, window->width, window->height,
                                       dawn_display_colour(DAWN_COLOUR_BODY, surface->format));
        }
}

/*
        Moves the cursor without repainting the screen.

        Two damage rectangles -- the old position and the new -- are recomposed
        and the cursor is drawn at the new one. drm_client_buffer_flush tells
        the driver which region changed, so a device that uploads its
        framebuffer only sends those bytes.
*/
static void dawn_display_move_cursor(struct dawn_display *display,
                                     struct dawn_surface *surface,
                                     int new_x, int new_y)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        struct drm_rect damage;
        int old_x = display->drawn_x;
        int old_y = display->drawn_y;

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        {
        u64 draw_started = ktime_get_ns();

        dawn_display_compose_rect(display, surface, pixels, pitch_pixels,
                                  old_x, old_y, DAWN_CURSOR_W, DAWN_CURSOR_H);
        dawn_display_compose_rect(display, surface, pixels, pitch_pixels,
                                  new_x, new_y, DAWN_CURSOR_W, DAWN_CURSOR_H);

        dawn_display_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                 new_x, new_y,
                                 dawn_display_colour(DAWN_COLOUR_CURSOR, surface->format),
                                 dawn_display_colour(DAWN_COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);

        dawn_input_draw_total += ktime_get_ns() - draw_started;
        }

        damage.x1 = min(old_x, new_x);
        damage.y1 = min(old_y, new_y);
        damage.x2 = max(old_x, new_x) + DAWN_CURSOR_W;
        damage.y2 = max(old_y, new_y) + DAWN_CURSOR_H;

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

        dawn_input_flush_total += ktime_get_ns() - flush_started;
        }
}

static void dawn_display_compose(struct dawn_display *display, struct dawn_surface *surface)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        unsigned int i;

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                       0, 0, surface->width, surface->height,
                       dawn_display_colour(DAWN_COLOUR_DESKTOP, surface->format));

        // Back to front, so a later window overlaps an earlier one.
        for (i = 0; i < DAWN_MAX_WINDOWS; i++) {
                struct dawn_window *window = &display->windows[i];

                if (!window->present)
                        continue;

                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x - 2, window->y - 2,
                               window->width + 4, window->height + 26,
                               dawn_display_colour(DAWN_COLOUR_FRAME, surface->format));

                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y, window->width, 20,
                               dawn_display_colour(DAWN_COLOUR_TITLE, surface->format));

                dawn_display_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y + 20, window->width, window->height,
                               dawn_display_colour(DAWN_COLOUR_BODY, surface->format));
        }

        dawn_display_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                         display->cursor_x, display->cursor_y,
                         dawn_display_colour(DAWN_COLOUR_CURSOR, surface->format),
                         dawn_display_colour(DAWN_COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);
}

static void dawn_display_redraw(struct dawn_display *display)
{
        unsigned int i;

        for (i = 0; i < display->surface_count; i++)
                dawn_display_compose(display, &display->surfaces[i]);

        drm_client_modeset_commit(&display->client);
}

static int dawn_display_setup_surface(struct drm_client_dev *client,
                              struct drm_mode_set *mode_set,
                              struct dawn_surface *surface)
{
        struct drm_crtc *crtc = mode_set->crtc;
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = dawn_display_pick_format(crtc->primary);

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

static unsigned int dawn_display_count_modesets(struct drm_client_dev *client)
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
static void dawn_display_seed_windows(struct dawn_display *display,
                              unsigned int width, unsigned int height)
{
        display->windows[0] = (struct dawn_window){
            .x = width / 10, .y = height / 8,
            .width = width / 3, .height = height / 3, .present = true};

        display->windows[1] = (struct dawn_window){
            .x = width / 3, .y = height / 3,
            .width = width / 3, .height = height / 3, .present = true};

        display->cursor_x = width / 2;
        display->cursor_y = height / 2;
}

static int dawn_display_start(struct dawn_display *display)
{
        struct drm_client_dev *client = &display->client;
        struct drm_mode_set *mode_set;
        unsigned int max_surfaces;
        unsigned int count = 0;

        if (drm_client_modeset_probe(client, 0, 0))
                return -ENODEV;

        max_surfaces = dawn_display_count_modesets(client);
        if (!max_surfaces)
                return -ENODEV;

        display->surfaces = kcalloc(max_surfaces, sizeof(*display->surfaces), GFP_KERNEL);
        if (!display->surfaces)
                return -ENOMEM;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client) {
                if (!mode_set->mode)
                        continue;

                if (dawn_display_setup_surface(client, mode_set, &display->surfaces[count]))
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
        dawn_display_seed_windows(display, display->surfaces[0].width,
                          display->surfaces[0].height);

        atomic_set(&display->pending_x, display->cursor_x);
        atomic_set(&display->pending_y, display->cursor_y);
        display->drawn_x = display->cursor_x;
        display->drawn_y = display->cursor_y;

        dawn_display_active = display;
        dawn_display_redraw(display);

        log_d("compositing on %u output(s)\n", count);
        return 0;
}

static void dawn_display_release(struct dawn_display *display)
{
        unsigned int i;

        for (i = 0; i < display->surface_count; i++)
                if (display->surfaces[i].buffer)
                        drm_client_buffer_delete(display->surfaces[i].buffer);

        kfree(display->surfaces);
        display->surfaces = NULL;
        display->surface_count = 0;
}

static void dawn_client_unregister(struct drm_client_dev *client)
{
        struct dawn_display *display = dawn_display_from_client(client);

        mutex_lock(&display->lock);
        dawn_display_release(display);
        mutex_unlock(&display->lock);

        drm_client_release(client);
}

static void dawn_client_free(struct drm_client_dev *client)
{
        struct dawn_display *display = dawn_display_from_client(client);

        mutex_destroy(&display->lock);
        kfree(display);
}

static int dawn_client_hotplug(struct drm_client_dev *client)
{
        struct dawn_display *display = dawn_display_from_client(client);
        int ret = 0;

        mutex_lock(&display->lock);

        if (!display->started) {
                ret = dawn_display_start(display);
                display->started = (ret == 0);
        } else {
                dawn_display_redraw(display);
        }

        mutex_unlock(&display->lock);
        return ret;
}

// The bool argument is whether the restore happens from an atomic context.
static int dawn_client_restore(struct drm_client_dev *client, _Bool in_atomic)
{
        struct dawn_display *display = dawn_display_from_client(client);

        // Nothing here is safe to do without sleeping, so an atomic restore
        // is declined rather than half performed.
        if (in_atomic)
                return -EBUSY;

        mutex_lock(&display->lock);
        if (display->started)
                dawn_display_redraw(display);
        mutex_unlock(&display->lock);

        return 0;
}

static const struct drm_client_funcs dawn_client_funcs = {
    .owner = THIS_MODULE,
    .unregister = dawn_client_unregister,
    .free = dawn_client_free,
    .restore = dawn_client_restore,
    .hotplug = dawn_client_hotplug,
};

/*
        Claims a DRM device. Reports whether it was taken, so the replacement
        for drm_client_setup can fall through to the original for anything we
        decline -- a device with no modesetting, or an allocation that failed.
*/
static int dawning_display_take_over(struct drm_device *dev)
{
        struct dawn_display *display;

        if (!drm_core_check_feature(dev, DRIVER_MODESET))
                return 0;

        display = kzalloc(sizeof(*display), GFP_KERNEL);
        if (!display)
                return 0;

        mutex_init(&display->lock);

        if (drm_client_init(dev, &display->client, "dawning", &dawn_client_funcs)) {
                mutex_destroy(&display->lock);
                kfree(display);
                return 0;
        }

        drm_client_register(&display->client);
        log_d("attached to %s\n", dev->driver->name);

        dawning_input_start();
        return 1;
}

/*
        Finding a device to draw on.

        Opening the node runs the driver's open path and hands back a
        drm_file, which knows its minor, which knows its device. The file is
        closed immediately: drm_client_init takes its own reference, so the
        device outlives our brief handle on it.
*/
#define DAWN_DISPLAY_NODE "/dev/dri/card0"
// Retried fast: the node appears the moment devtmpfs is mounted, and every
// millisecond spent waiting after that is a millisecond of black screen.
#define DAWN_DISPLAY_RETRY_MS 5
#define DAWN_DISPLAY_ATTEMPTS 1000

static struct delayed_work dawn_display_probe_work;
static unsigned int dawn_display_attempts;

static int dawning_display_claim(const char *path)
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
        taken = dawning_display_take_over(dev);

        filp_close(filp, NULL);

        return taken ? 0 : -EBUSY;
}

static void dawning_display_probe(struct work_struct *work)
{
        int ret = dawning_display_claim(DAWN_DISPLAY_NODE);

        if (ret == 0)
                return;

        if (++dawn_display_attempts >= DAWN_DISPLAY_ATTEMPTS) {
                log_d("gave up waiting for %s (%d)\n", DAWN_DISPLAY_NODE, ret);
                return;
        }

        schedule_delayed_work(&dawn_display_probe_work,
                              msecs_to_jiffies(DAWN_DISPLAY_RETRY_MS));
}

static void dawning_display_start_probing(void)
{
        INIT_DELAYED_WORK(&dawn_display_probe_work, dawning_display_probe);

        // No initial delay. The display driver has already probed by the time
        // this runs, so the first attempt usually succeeds outright.
        schedule_delayed_work(&dawn_display_probe_work, 0);
}

/*
        Input to cursor

        input_handler.event is called by the input core in the path that
        received the event, so nothing is woken to notice a mouse move. The
        position is updated there and then; the pixels cannot be, because that
        context cannot sleep and a DRM commit can. A high priority worker does
        that part, and the gap between the two is what gets measured.
*/
static struct workqueue_struct *dawn_input_wq;
static void dawn_input_apply(struct work_struct *work);
static DECLARE_WORK(dawn_input_work, dawn_input_apply);

// Nanoseconds from the event arriving to the cursor being on screen.
// Declared with the rest of the timing counters near the top of the file.

static void dawn_input_apply(struct work_struct *work)
{
        struct dawn_display *display = dawn_display_active;
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
                dawn_input_queue_total += ktime_get_ns() - started;

        mutex_lock(&display->lock);

        if (display->started && display->surface_count) {
                dawn_display_move_cursor(display, &display->surfaces[0], x, y);
                display->drawn_x = x;
                display->drawn_y = y;
                display->cursor_x = x;
                display->cursor_y = y;
        }

        mutex_unlock(&display->lock);

        if (started) {
                u64 elapsed = ktime_get_ns() - started;

                dawn_input_latency_total += elapsed;
                dawn_input_events++;

                if (elapsed > dawn_input_latency_worst)
                        dawn_input_latency_worst = elapsed;
        }
}

static void dawn_input_event(struct input_handle *handle, unsigned int type,
                             unsigned int code, int value)
{
        struct dawn_display *display = dawn_display_active;
        int x, y, limit;

        if (!display || !display->started || !display->surface_count)
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
                                         display->surfaces[0].width,
                                         abs->maximum - abs->minimum);
                else
                        y = (int)div_u64((u64)(value - abs->minimum) *
                                         display->surfaces[0].height,
                                         abs->maximum - abs->minimum);
        } else {
                return;
        }

        limit = (int)display->surfaces[0].width - 1;
        x = clamp(x, 0, limit);
        limit = (int)display->surfaces[0].height - 1;
        y = clamp(y, 0, limit);

        atomic_set(&display->pending_x, x);
        atomic_set(&display->pending_y, y);

        // Stamp only the first event of a burst, so the measurement is the age
        // of the oldest movement not yet on screen.
        if (!atomic_xchg(&display->motion_pending, 1))
                display->motion_stamp = ktime_get_ns();

        queue_work(dawn_input_wq, &dawn_input_work);
}

static int dawn_input_connect(struct input_handler *handler, struct input_dev *dev,
                              const struct input_device_id *id)
{
        struct input_handle *handle;
        int ret;

        handle = kzalloc(sizeof(*handle), GFP_KERNEL);
        if (!handle)
                return -ENOMEM;

        handle->dev = dev;
        handle->handler = handler;
        handle->name = "dawning";

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

static void dawn_input_disconnect(struct input_handle *handle)
{
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(handle);
}

// Anything that reports relative or absolute motion: mice, tablets, touchpads.
static const struct input_device_id dawn_input_ids[] = {
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

static struct input_handler dawn_input_handler = {
    .event = dawn_input_event,
    .connect = dawn_input_connect,
    .disconnect = dawn_input_disconnect,
    .name = "dawning",
    .id_table = dawn_input_ids,
};

static void dawning_input_start(void)
{
        // WQ_HIGHPRI so the cursor is not queued behind ordinary work.
        dawn_input_wq = alloc_workqueue("dawning_input", WQ_HIGHPRI | WQ_UNBOUND, 1);

        if (!dawn_input_wq) {
                log_d("no workqueue for input\n");
                return;
        }

        if (input_register_handler(&dawn_input_handler))
                log_d("could not register the input handler\n");
}

// Nanoseconds, for the stats ioctl.
void dawning_display_input_stats(unsigned long *events, unsigned long *mean,
                                 unsigned long *worst, unsigned long *queue,
                                 unsigned long *draw, unsigned long *flush)
{
        unsigned long n = dawn_input_events ? dawn_input_events : 1;

        *events = dawn_input_events;
        *mean = dawn_input_latency_total / n;
        *worst = dawn_input_latency_worst;
        *queue = dawn_input_queue_total / n;
        *draw = dawn_input_draw_total / n;
        *flush = dawn_input_flush_total / n;
}
