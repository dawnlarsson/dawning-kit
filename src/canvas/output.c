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


/*
        Whether this display is a window on somebody else's screen.

        The driver's own name, which is the only thing here that actually
        knows. Working it out from what the display reports does not succeed:
        a virtual one answers a physical size anyway, bochs says 320 by 200
        millimetres whatever mode it is in, and QEMU synthesises an EDID too,
        so neither the size nor the presence of one separates them.
*/
static _Bool canvas_is_virtual(struct drm_device *dev)
{
        static const char *const guests[] = {
            "bochs-drm", "virtio_gpu", "qxl", "vmwgfx", "cirrus-qemu",
            "hyperv_drm", "vkms", NULL};
        unsigned int i;

        if (!dev->driver || !dev->driver->name)
                return false;

        for (i = 0; guests[i]; i++)
                if (!strcmp(dev->driver->name, guests[i]))
                        return true;

        return false;
}

static unsigned int output_mode_count(struct drm_connector *connector)
{
        struct drm_display_mode *mode;
        unsigned int count = 0;

        list_for_each_entry(mode, &connector->modes, head)
                count++;

        return count;
}

static struct drm_display_mode *output_best_mode(struct drm_connector *connector,
                                                 unsigned int want_width)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_area = 0, best_refresh = 0;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int area, refresh;

                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);

                /*
                        Nearest to the width asked for rather than the largest
                        under it: a mode list with a hole in it would otherwise
                        drop a long way below half, and half is the point.
                */
                if (want_width)
                {
                        int off = mode->hdisplay > (int)want_width
                                      ? mode->hdisplay - (int)want_width
                                      : (int)want_width - mode->hdisplay;

                        if (best && (off > best_area ||
                                     (off == best_area && refresh <= best_refresh)))
                                continue;

                        best = mode;
                        best_area = off;
                        best_refresh = refresh;
                        continue;
                }

                area = mode->hdisplay * mode->vdisplay;

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

                want = output_best_mode(mode_set->connectors[0], 0);

                /*
                        Half the width inside a guest, so the window this is
                        drawn in leaves room for the machine around it. The
                        largest mode is the right answer on a real panel and
                        the wrong one when the panel belongs to a host that is
                        still using it.
                */
                if (want && canvas_is_virtual(dev))
                {
                        struct drm_display_mode *smaller =
                            output_best_mode(mode_set->connectors[0],
                                             (unsigned int)want->hdisplay / 2);

                        if (smaller)
                                want = smaller;
                }

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
                log_canvas_error("no 32 bit format on this plane, skipping output\n");
                return NULL;
        }

        output = kzalloc(sizeof(*output), GFP_KERNEL);
        if (!output)
                return NULL;

        output->buffer = drm_client_buffer_create_dumb(&canvas->client, width, height, format);
        if (IS_ERR(output->buffer))
        {
                log_canvas_error("could not create a %ux%u scanout buffer\n", width, height);
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

        if (!output_describe(output))
        {
                drm_client_buffer_delete(output->buffer);
                mode_set->fb = NULL;
                kfree(output);
                return NULL;
        }

        if (plane_claim(&canvas->client, output))
                output->cursor_plane = NULL;

        return output;
}

/*
        A modeset nothing will be drawn on, left the way a probe leaves one.

        A screen that could not be given a buffer -- no 32 bit format on its
        plane, no memory for one -- still carries the mode the probe chose and
        no framebuffer to go with it, and that combination is not one screen
        missing: the commit carrying it is refused whole, so one connector
        nobody can draw on takes every other screen on the card down with it.

        Clearing it is what says "this crtc is off", and then the screens that
        did build commit without it.
*/
static void output_disable_modeset(struct drm_device *dev,
                                   struct drm_mode_set *mode_set)
{
        unsigned int i;

        if (mode_set->mode)
        {
                drm_mode_destroy(dev, mode_set->mode);
                mode_set->mode = NULL;
        }

        mode_set->fb = NULL;

        for (i = 0; mode_set->connectors && i < (unsigned int)mode_set->num_connectors; i++)
        {
                drm_connector_put(mode_set->connectors[i]);
                mode_set->connectors[i] = NULL;
        }

        mode_set->num_connectors = 0;
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
        Puts every output's buffer back on the modeset that scans it out.

        A probe releases every modeset, and releasing one takes its framebuffer
        away -- and canvas_modes_changed probes to answer a question, so the
        answer alone is enough to lose it. A modeset carrying a mode and no
        framebuffer is refused, the whole commit with it, and what stays on the
        screen is whatever was there before this ever ran: on a machine that
        inherits the firmware's picture that is a cursor moving over it and
        nothing else.

        So it is set before every commit rather than once when the output was
        made. Whoever cleared it, and for whatever reason, it is right again by
        the time it matters.
*/
static void desktop_attach_buffers(void)
{
        struct output *output;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                struct drm_client_dev *client = &output->canvas->client;

                if (!output->buffer)
                        continue;

                mutex_lock(&client->modeset_mutex);
                output->mode_set->fb = output->buffer->fb;
                mutex_unlock(&client->modeset_mutex);
        }
}

/*
        A card's outputs are added together, so they are consecutive here and
        remembering the last one is enough to commit each card once.

        The answer used to be thrown away. A commit is the only thing that puts
        a mode on a screen, and one that refuses says so in the one place that
        could have noticed.
*/
static void desktop_commit(void)
{
        struct canvas *committed = NULL;
        struct output *output;

        desktop_attach_buffers();

        list_for_each_entry(output, &desktop.outputs, link)
        {
                int set;

                if (output->canvas == committed)
                        continue;

                committed = output->canvas;
                set = drm_client_modeset_commit(&committed->client);

                // EBUSY is not now: something else is the device's master and
                // the next commit is the one that lands.
                if (set == -EBUSY || set == committed->set_result)
                        continue;

                committed->set_result = set;

                if (set)
                        log_canvas("%ux%u would not go on the screen (%d)\n",
                                   output->width, output->height, set);
                else
                        log_canvas("%ux%u is on the screen\n",
                                   output->width, output->height);
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
        struct drm_connector *connector;
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

                /*
                        Which screen, and how much of a choice there was. A
                        mode that turns out to be wrong on a machine that is
                        not here is answered by what its connector offered,
                        not by what was picked out of it.
                */
                connector = mode_set->num_connectors ? mode_set->connectors[0] : NULL;

                log_canvas("screen %s %ux%u at %u Hz, drawn %ux, %u mode(s) offered\n",
                           connector && connector->name ? connector->name : "?",
                           mode_set->mode->hdisplay, mode_set->mode->vdisplay,
                           drm_mode_vrefresh(mode_set->mode), desktop.scale,
                           connector ? output_mode_count(connector) : 0);

                output = output_add(canvas, mode_set);
                if (!output)
                {
                        output_disable_modeset(client->dev, mode_set);
                        continue;
                }

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

        /*
                A refused commit means the mode, not the moment.

                This runs from the hotplug that drm_client_register fires, and
                the file canvas_claim opened to find the card is still open at
                that point -- so it is still the device's master, and a commit
                answers EBUSY whatever mode it was handed. Falling back on that
                threw away every mode this ever chose and quietly took the
                probe's, which is the opposite of the point.
        */
        int set;

        if (!ret)
                desktop_attach_buffers();

        set = ret ? 0 : drm_client_modeset_commit(&canvas->client);

        if (!ret && set && set != -EBUSY)
        {
                log_canvas("that mode would not set (%d), taking the offered one\n", set);
                canvas_release(canvas);
                ret = canvas_build(canvas, false);

                if (!ret)
                {
                        desktop_attach_buffers();
                        set = drm_client_modeset_commit(&canvas->client);
                }
        }

        /*
                Standing aside.

                Nothing below will put a picture on this screen, and holding it
                anyway is a black display and no way to find out why: the
                kernel log reaches a machine with no serial port through fbcon,
                and fbcon only has something to bind to while the fbdev client
                still owns the display. So the outputs go back and started
                stays false, which leaves whatever was on the screen on it and
                the reason above readable there.

                Boot the image without drm_client_lib.active= and this is the
                difference between a log on the monitor and a guess.
        */
        if (!ret && set && set != -EBUSY)
        {
                log_canvas_error("no mode would set (%d), leaving the display alone\n",
                                 set);
                canvas_release(canvas);
                return set;
        }

        if (ret)
        {
                log_canvas_error("no screen to draw on (%d), leaving the display alone\n",
                                 ret);
                return ret;
        }

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
