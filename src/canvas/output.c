/*
        Canvas -- outputs

        An output is a rectangle of the desktop that one crtc scans out. They
        are laid left to right in the order their cards attached, which is the
        placement; the desktop is their bounding box.
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


static struct drm_display_mode *output_best_mode(struct drm_connector *connector)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_area = 0, best_refresh = 0;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int area, refresh;

                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                area = mode->hdisplay * mode->vdisplay;
                refresh = drm_mode_vrefresh(mode);

                if (area < best_area || (area == best_area && refresh <= best_refresh))
                        continue;

                best = mode;
                best_area = area;
                best_refresh = refresh;
        }

        return best;
}

/*
        Probes every connector and puts the best mode on each modeset.

        The lock order is the one drm_client_modeset_probe uses: the client's
        modesets, then the device's mode configuration, which is what guards a
        connector's list of modes.
*/
static int canvas_probe_modes(struct canvas *canvas, _Bool biggest)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_device *dev = client->dev;
        struct drm_mode_set *mode_set;

        if (drm_client_modeset_probe(client, 0, 0))
                return -ENODEV;

        if (!biggest)
                return 0;

        mutex_lock(&client->modeset_mutex);
        mutex_lock(&dev->mode_config.mutex);

        drm_client_for_each_modeset(mode_set, client)
        {
                struct drm_display_mode *want, *taken;

                if (!mode_set->mode || !mode_set->num_connectors ||
                    !mode_set->connectors || !mode_set->connectors[0])
                        continue;

                want = output_best_mode(mode_set->connectors[0]);

                if (!want || drm_mode_equal(want, mode_set->mode))
                        continue;

                // The modeset owns its mode, so this is a copy, not the
                // connector's own entry.
                taken = drm_mode_duplicate(dev, want);

                if (!taken)
                        continue;

                drm_mode_destroy(dev, mode_set->mode);
                mode_set->mode = taken;
        }

        mutex_unlock(&dev->mode_config.mutex);
        mutex_unlock(&client->modeset_mutex);

        return 0;
}

// Whether a fresh probe would give any screen a different mode than the one it
// is running, which is what a monitor arriving or its EDID landing late looks
// like.
static _Bool canvas_modes_changed(struct canvas *canvas)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_mode_set *mode_set;
        struct output *output;
        unsigned int active = 0, mine = 0;
        _Bool changed = false;

        if (canvas_probe_modes(canvas, true))
                return false;

        mutex_lock(&client->modeset_mutex);

        drm_client_for_each_modeset(mode_set, client)
        {
                if (!mode_set->mode)
                        continue;

                active++;

                list_for_each_entry(output, &desktop.outputs, link)
                {
                        if (output->mode_set != mode_set)
                                continue;

                        if (output->width != mode_set->mode->hdisplay ||
                            output->height != mode_set->mode->vdisplay)
                                changed = true;
                }
        }

        mutex_unlock(&client->modeset_mutex);

        list_for_each_entry(output, &desktop.outputs, link)
                if (output->canvas == canvas)
                        mine++;

        return changed || active != mine;
}

static void desktop_place_outputs(void)
{
        struct output *output;
        int x = 0, height = 0;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                output->x = x;
                output->y = 0;

                x += (int)output->width;
                height = max(height, (int)output->height);
        }

        desktop.width = x;
        desktop.height = height;

        desktop_gather_panes();
}

static struct output *output_add(struct canvas *canvas, struct drm_mode_set *mode_set)
{
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = canvas_pick_format(mode_set->crtc->primary);
        struct output *output;

        if (format == DRM_FORMAT_INVALID)
        {
                log_canvas("no 32 bit format on this plane, skipping output\n");
                return NULL;
        }

        output = kzalloc(sizeof(*output), GFP_KERNEL);
        if (!output)
                return NULL;

        output->buffer = drm_client_buffer_create_dumb(&canvas->client, width, height, format);
        if (IS_ERR(output->buffer))
        {
                log_canvas("could not create a %ux%u scanout buffer\n", width, height);
                kfree(output);
                return NULL;
        }

        output->canvas = canvas;
        output->mode_set = mode_set;
        output->width = width;
        output->height = height;
        output->format = format;
        output->opaque = format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0;
        canvas_palette(output->palette, format);
        mode_set->fb = output->buffer->fb;

        if (plane_claim(&canvas->client, output))
                output->cursor_plane = NULL;

        return output;
}

static void output_drop(struct output *output)
{
        plane_drop(output);

        if (output->buffer)
                drm_client_buffer_delete(output->buffer);

        list_del(&output->link);
        kfree(output);
}

/*
        A card's outputs are added together, so they are consecutive here and
        remembering the last one is enough to commit each card once.
*/
static void desktop_commit(void)
{
        struct canvas *committed = NULL;
        struct output *output;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (output->canvas == committed)
                        continue;

                committed = output->canvas;
                drm_client_modeset_commit(&committed->client);
        }

        list_for_each_entry(output, &desktop.outputs, link)
                cursor_arm_output(output);
}

static void desktop_redraw(void)
{
        u64 started = ktime_get_ns();
        struct output *output;

        list_for_each_entry(output, &desktop.outputs, link)
                compose_output(output);

        canvas_composes++;
        canvas_compose_ns += ktime_get_ns() - started;

        desktop_commit();
}

// Whatever desktop_refresh_panes recorded, or the whole thing when it gave up
// counting.
static void desktop_repaint(void)
{
        struct output *output;

        // Nothing recorded means nothing changed. It used to mean repaint
        // every screen, which is the opposite.
        if (!desktop.damage_count && !desktop.damage_all)
                return;

        if (desktop.damage_all)
        {
                desktop.damage_count = 0;
                desktop.damage_all = false;
                desktop_redraw();
                return;
        }

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (!output_touched(output, desktop.damage, desktop.damage_count))
                        continue;

                if (output->cursor_plane)
                        cursor_arm_output(output);

                output_repaint(output, desktop.damage, desktop.damage_count);
        }

        desktop.damage_count = 0;
}

static void canvas_release(struct canvas *canvas);

static int canvas_build(struct canvas *canvas, _Bool biggest)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_mode_set *mode_set;
        unsigned int count = 0;

        if (canvas_probe_modes(canvas, biggest))
                return -ENODEV;

#ifdef CONFIG_MOONWATER_CANVAS_SCALE
        desktop.scale = CONFIG_MOONWATER_CANVAS_SCALE;
#endif
        if (desktop.scale < 1)
                desktop.scale = 1;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
        {
                struct output *output;

                if (!mode_set->mode)
                        continue;

                log_canvas("screen %ux%u at %u Hz, drawn %ux\n",
                           mode_set->mode->hdisplay, mode_set->mode->vdisplay,
                           drm_mode_vrefresh(mode_set->mode), desktop.scale);

                output = output_add(canvas, mode_set);
                if (!output)
                        continue;

                list_add_tail(&output->link, &desktop.outputs);
                count++;
        }
        mutex_unlock(&client->modeset_mutex);

        if (!count)
                return -ENODEV;

        desktop_place_outputs();

        return 0;
}

/*
        The biggest mode every screen offers, and what to do when it will not
        set.

        A monitor listing a mode is not a promise the link can carry it,
        especially with more than one screen sharing the bandwidth, so a
        refused commit falls back to the mode the probe would have chosen --
        which is the one that used to be taken unconditionally.
*/
static int canvas_start(struct canvas *canvas)
{
        int ret = canvas_build(canvas, IS_ENABLED(CONFIG_MOONWATER_CANVAS_LARGEST_MODE));

        if (!ret && drm_client_modeset_commit(&canvas->client))
        {
                log_canvas("the largest mode would not set, taking the offered one\n");
                canvas_release(canvas);
                ret = canvas_build(canvas, false);
        }

        if (ret)
                return ret;

        // The cursor is drawn from a bitmap like everything else, so it is the
        // same sixteen pixels and the same too small without this.
        desktop.cursor_scale = desktop.scale;
        desktop.drawn_scale = desktop.scale;

        if (!desktop.started)
        {
                desktop.started = true;
                desktop.cursor_x = desktop.width / 2;
                desktop.cursor_y = desktop.height / 2;
                desktop.drawn_x = desktop.cursor_x;
                desktop.drawn_y = desktop.cursor_y;
                atomic_set(&desktop.pending_x, desktop.cursor_x);
                atomic_set(&desktop.pending_y, desktop.cursor_y);
        }

        desktop_redraw();

        {
                struct output *output;
                unsigned int count = 0;

                list_for_each_entry(output, &desktop.outputs, link)
                        count++;

                log_canvas("desktop %dx%d, %u output(s)\n",
                           desktop.width, desktop.height, count);
        }

        // Something to use it with. A desktop with nothing on it is not a
        // desktop, and this is the first program a screen is worth having.
        if (!desktop.terminal)
        {
                desktop.terminal = true;
                log_canvas("terminal: %d\n", spawn_program("/term"));
        }

        return 0;
}

static void canvas_release(struct canvas *canvas)
{
        struct output *output, *next;

        list_for_each_entry_safe(output, next, &desktop.outputs, link)
                if (output->canvas == canvas)
                        output_drop(output);

        desktop_place_outputs();

        if (!list_empty(&desktop.outputs))
                desktop_redraw();
}
