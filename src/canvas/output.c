/*
        Canvas -- outputs

        One screen: the format it can scan out, the dumb buffer behind it, and
        the modeset that puts the buffer on it. Starting and stopping the
        compositor lives here too, because both are really "for each output".
*/

/*
        Only 32 bit little endian xrgb/argb is handled for now. Every device
        this targets offers one of them for a dumb buffer, and pretending to
        support formats that are not tested would be worse than refusing them.
*/
static _Bool canvas_format_supported(u32 format)
{
        return format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888;
}

static u32 canvas_pick_format(struct drm_plane *plane)
{
        unsigned int i;

        for (i = 0; i < plane->format_count; i++)
                if (canvas_format_supported(plane->format_types[i]))
                        return plane->format_types[i];

        return DRM_FORMAT_INVALID;
}

static void canvas_redraw(struct canvas *canvas)
{
        unsigned int i;

        for (i = 0; i < canvas->surface_count; i++)
                canvas_compose(canvas, &canvas->surfaces[i]);

        drm_client_modeset_commit(&canvas->client);
        canvas_arm_cursor(canvas);
}

static int canvas_setup_surface(struct drm_client_dev *client,
                              struct drm_mode_set *mode_set,
                              struct surface *surface)
{
        struct drm_crtc *crtc = mode_set->crtc;
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = canvas_pick_format(crtc->primary);

        if (format == DRM_FORMAT_INVALID) {
                log_canvas("no 32 bit format on this plane, skipping output\n");
                return -EINVAL;
        }

        surface->buffer = drm_client_buffer_create_dumb(client, width, height, format);
        if (IS_ERR(surface->buffer)) {
                log_canvas("could not create a %ux%u scanout buffer\n", width, height);
                surface->buffer = NULL;
                return -ENOMEM;
        }

        surface->mode_set = mode_set;
        surface->width = width;
        surface->height = height;
        surface->format = format;
        mode_set->fb = surface->buffer->fb;

        log_canvas("output %ux%u ready\n", width, height);
        return 0;
}

static unsigned int canvas_count_modesets(struct drm_client_dev *client)
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
static void canvas_seed_windows(struct canvas *canvas,
                              unsigned int width, unsigned int height)
{
        canvas->windows[0] = (struct window){
            .x = width / 10, .y = height / 8,
            .width = width / 3, .height = height / 3, .present = true};

        canvas->windows[1] = (struct window){
            .x = width / 3, .y = height / 3,
            .width = width / 3, .height = height / 3, .present = true};

        canvas->cursor_x = width / 2;
        canvas->cursor_y = height / 2;
}

static int canvas_start(struct canvas *canvas)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_mode_set *mode_set;
        unsigned int max_surfaces;
        unsigned int count = 0;

        if (drm_client_modeset_probe(client, 0, 0))
                return -ENODEV;

        max_surfaces = canvas_count_modesets(client);
        if (!max_surfaces)
                return -ENODEV;

        canvas->surfaces = kcalloc(max_surfaces, sizeof(*canvas->surfaces), GFP_KERNEL);
        if (!canvas->surfaces)
                return -ENOMEM;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client) {
                if (!mode_set->mode)
                        continue;

                if (canvas_setup_surface(client, mode_set, &canvas->surfaces[count]))
                        continue;

                count++;
        }
        mutex_unlock(&client->modeset_mutex);

        if (!count) {
                kfree(canvas->surfaces);
                canvas->surfaces = NULL;
                return -ENODEV;
        }

        canvas->surface_count = count;
        canvas_seed_windows(canvas, canvas->surfaces[0].width,
                          canvas->surfaces[0].height);

        canvas->screen_w = (int)canvas->surfaces[0].width;
        canvas->screen_h = (int)canvas->surfaces[0].height;

        atomic_set(&canvas->pending_x, canvas->cursor_x);
        atomic_set(&canvas->pending_y, canvas->cursor_y);
        canvas->drawn_x = canvas->cursor_x;
        canvas->drawn_y = canvas->cursor_y;

        if (canvas_setup_cursor_plane(client, &canvas->surfaces[0]))
                log_canvas("no hardware cursor here, drawing the cursor into the framebuffer\n");
        else
                log_canvas("cursor on hardware plane %u\n",
                      canvas->surfaces[0].cursor_plane->base.id);

        canvas_active = canvas;
        canvas_redraw(canvas);

        log_canvas("compositing on %u output(s)\n", count);
        return 0;
}

static void canvas_release(struct canvas *canvas)
{
        unsigned int i;

        for (i = 0; i < canvas->surface_count; i++) {
                canvas_drop_cursor_plane(&canvas->surfaces[i]);

                if (canvas->surfaces[i].buffer)
                        drm_client_buffer_delete(canvas->surfaces[i].buffer);
        }

        kfree(canvas->surfaces);
        canvas->surfaces = NULL;
        canvas->surface_count = 0;
}
