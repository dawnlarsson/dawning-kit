/*
        Canvas -- outputs

        One screen: its format, its scanout buffer, its modeset. Starting and
        stopping the compositor is here too, since both are per output.
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
        struct surface *surface;

        list_for_each_entry(surface, &canvas->outputs, link)
                canvas_compose(canvas, surface);

        drm_client_modeset_commit(&canvas->client);
        canvas_arm_cursor(canvas);
}

static struct surface *canvas_add_output(struct drm_client_dev *client,
                                         struct drm_mode_set *mode_set)
{
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = canvas_pick_format(mode_set->crtc->primary);
        struct surface *surface;

        if (format == DRM_FORMAT_INVALID)
        {
                log_canvas("no 32 bit format on this plane, skipping output\n");
                return NULL;
        }

        surface = kzalloc(sizeof(*surface), GFP_KERNEL);
        if (!surface)
                return NULL;

        surface->buffer = drm_client_buffer_create_dumb(client, width, height, format);
        if (IS_ERR(surface->buffer))
        {
                log_canvas("could not create a %ux%u scanout buffer\n", width, height);
                kfree(surface);
                return NULL;
        }

        surface->mode_set = mode_set;
        surface->width = width;
        surface->height = height;
        surface->format = format;
        mode_set->fb = surface->buffer->fb;

        log_canvas("output %ux%u ready\n", width, height);
        return surface;
}

static void canvas_drop_output(struct surface *surface)
{
        canvas_drop_cursor_plane(surface);

        if (surface->buffer)
                drm_client_buffer_delete(surface->buffer);

        list_del(&surface->link);
        kfree(surface);
}

/*
        A first arrangement, so there is something on screen before anything
        can create a window. Sized to the screen rather than counted, so it
        also stands in for a window list of no particular length.
*/
static unsigned int canvas_seed_windows(struct canvas *canvas, unsigned int width, unsigned int height)
{
        unsigned int columns = max(width / 320u, 1u);
        unsigned int rows = max(height / 260u, 1u);
        unsigned int column, row, seeded = 0;

        for (row = 0; row < rows; row++)
        {
                for (column = 0; column < columns; column++)
                {
                        struct window *window = kzalloc(sizeof(*window), GFP_KERNEL);

                        if (!window)
                                goto done;

                        window->x = 40 + column * 300;
                        window->y = 40 + row * 240;
                        window->width = 240;
                        window->height = 170;

                        list_add_tail(&window->link, &canvas->windows);
                        seeded++;
                }
        }

done:
        canvas->cursor_x = width / 2;
        canvas->cursor_y = height / 2;

        return seeded;
}

static void canvas_drop_windows(struct canvas *canvas)
{
        struct window *window, *next;

        list_for_each_entry_safe(window, next, &canvas->windows, link)
        {
                list_del(&window->link);
                kfree(window);
        }
}

static unsigned int canvas_probe_outputs(struct canvas *canvas)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_mode_set *mode_set;
        unsigned int count = 0;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
        {
                struct surface *surface;

                if (!mode_set->mode)
                        continue;

                surface = canvas_add_output(client, mode_set);
                if (!surface)
                        continue;

                list_add_tail(&surface->link, &canvas->outputs);
                count++;
        }
        mutex_unlock(&client->modeset_mutex);

        return count;
}

static int canvas_start(struct canvas *canvas)
{
        struct surface *first;
        unsigned int count, windows;

        if (drm_client_modeset_probe(&canvas->client, 0, 0))
                return -ENODEV;

        count = canvas_probe_outputs(canvas);
        if (!count)
                return -ENODEV;

        first = canvas_first_output(canvas);

        windows = canvas_seed_windows(canvas, first->width, first->height);

        canvas->screen_w = (int)first->width;
        canvas->screen_h = (int)first->height;

        atomic_set(&canvas->pending_x, canvas->cursor_x);
        atomic_set(&canvas->pending_y, canvas->cursor_y);
        canvas->drawn_x = canvas->cursor_x;
        canvas->drawn_y = canvas->cursor_y;

        if (canvas_setup_cursor_plane(&canvas->client, first))
                log_canvas("no hardware cursor here, drawing the cursor into the framebuffer\n");
        else
                log_canvas("cursor on hardware plane %u, %ux%u\n",
                           first->cursor_plane->base.id, first->cursor_w, first->cursor_h);

        canvas_redraw(canvas);

        log_canvas("compositing on %u output(s), %u window(s)\n", count, windows);
        return 0;
}

static void canvas_release(struct canvas *canvas)
{
        struct surface *surface, *next;

        list_for_each_entry_safe(surface, next, &canvas->outputs, link)
                canvas_drop_output(surface);

        canvas_drop_windows(canvas);
}
