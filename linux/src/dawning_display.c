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
};

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

static void dawning_display_register(struct drm_device *dev)
{
        struct dawn_display *display;

        display = kzalloc(sizeof(*display), GFP_KERNEL);
        if (!display)
                return;

        mutex_init(&display->lock);

        if (drm_client_init(dev, &display->client, "dawning", &dawn_client_funcs)) {
                mutex_destroy(&display->lock);
                kfree(display);
                return;
        }

        drm_client_register(&display->client);
        log_d("attached to %s\n", dev->driver->name);
}
