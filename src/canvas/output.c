/*
        Canvas -- outputs

        An output is a rectangle of the desktop that one crtc scans out. They
        are laid left to right in the order their cards attached, which is the
        placement; the desktop is their bounding box.
*/

/*
        Whether this display is a window on somebody else's screen.

        The driver's own name, which is the only thing here that actually
        knows. Working it out from what the display reports does not succeed:
        a virtual one answers a physical size anyway, bochs says 320 by 200
        millimetres whatever mode it is in, and QEMU synthesises an EDID too,
        so neither the size nor the presence of one separates them.
*/
static PURE _Bool canvas_is_virtual(struct drm_device *dev)
{
        static const char *const guests[] = {
            "bochs-drm", "virtio_gpu", "qxl", "vmwgfx", "cirrus-qemu",
            "hyperv_drm", "vkms"};

        if (!dev->driver || !dev->driver->name)
                return false;

        return string_table_find((string_address)dev->driver->name, guests,
                                 sizeof(guests[0]), array_count(guests)) <
               array_count(guests);
}

static PURE unsigned int output_mode_count(struct drm_connector *connector)
{
        struct drm_display_mode *mode;
        unsigned int count = 0;

        list_for_each_entry(mode, &connector->modes, head)
                count++;

        return count;
}

/*
        Highest resolution, and at that size the highest refresh.

        Same size at 60 Hz and 120 Hz is the 120 Hz entry: that is what a
        Mac's virtio EDID actually offers, and what the cursor needs.
*/
static struct drm_display_mode *output_best_mode(struct drm_connector *connector)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_score = 0, best_refresh = 0;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int score, refresh;

                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);
                score = mode->hdisplay * mode->vdisplay;
                if (best && (score < best_score ||
                             (score == best_score && refresh <= best_refresh)))
                        continue;

                best = mode;
                best_score = score;
                best_refresh = refresh;
        }

        return best;
}

/*
        The host's idea of the screen: the preferred mode, or the largest
        if the connector did not mark one.
*/
static struct drm_display_mode *output_screen_mode(struct drm_connector *connector)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_score = 0, best_refresh = 0;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int score, refresh;

                if (!(mode->type & DRM_MODE_TYPE_PREFERRED))
                        continue;
                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);
                score = mode->hdisplay * mode->vdisplay;
                if (best && (score < best_score ||
                             (score == best_score && refresh <= best_refresh)))
                        continue;

                best = mode;
                best_score = score;
                best_refresh = refresh;
        }

        return best ? best : output_best_mode(connector);
}

/*
        The same size, at the highest refresh that size lists.

        Restoring a running size used to take the first match, which on a
        120 Hz EDID is often the 60 Hz established timing of the same
        width and height.
*/
static struct drm_display_mode *output_mode_wh(struct drm_connector *connector,
                                               unsigned int width,
                                               unsigned int height)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_refresh = -1;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int refresh;

                if (mode->hdisplay != (int)width || mode->vdisplay != (int)height)
                        continue;
                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);
                if (best && refresh <= best_refresh)
                        continue;

                best = mode;
                best_refresh = refresh;
        }

        return best;
}

/*
        Largest mode that still fits, preferring prefer_refresh when it is
        positive.

        A guest cap of seventy percent of a 120 Hz panel often still lists a
        larger 60 Hz established timing under that cap. Pixels first would
        take it and throw the refresh away; matching the screen's rate first
        keeps the cursor on the panel's clock.
*/
static struct drm_display_mode *output_mode_under(struct drm_connector *connector,
                                                  int width, int height,
                                                  int prefer_refresh)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_score = 0, best_refresh = 0;
        _Bool best_preferred = false;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int score, refresh;
                _Bool preferred;

                if (mode->hdisplay > width || mode->vdisplay > height)
                        continue;
                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);
                score = mode->hdisplay * mode->vdisplay;
                preferred = prefer_refresh > 0 && refresh == prefer_refresh;
                if (best && preferred == best_preferred &&
                    (score < best_score ||
                     (score == best_score && refresh <= best_refresh)))
                        continue;
                if (best && preferred != best_preferred && !preferred)
                        continue;

                best = mode;
                best_score = score;
                best_refresh = refresh;
                best_preferred = preferred;
        }

        return best;
}

/*
        A copy of src with its CRTC timings filled, or nothing.

        Connector modes leave crtc_clock at 0 until a modeset fills it.
        virtio-gpu's vblank timer reads that field; 0 means no vblank, and
        the next flip waits ten seconds then warns.
*/
static struct drm_display_mode *output_mode_take(struct drm_device *dev,
                                                 const struct drm_display_mode *src)
{
        struct drm_display_mode *taken;

        if (!src || src->clock <= 0)
                return NULL;

        taken = drm_mode_duplicate(dev, src);
        if (!taken)
                return NULL;

        drm_mode_set_crtcinfo(taken, CRTC_INTERLACE_HALVE_V);
        if (taken->crtc_clock <= 0)
        {
                drm_mode_destroy(dev, taken);
                return NULL;
        }

        return taken;
}

/*
        A guest is a window on somebody else's screen, so it cannot take the
        whole of that screen. Seventy percent of the host, in the pixels the
        host already scaled: cocoa then divides by the backing factor and
        centres the window, which is seventy percent of the DIP screen on a
        Retina panel.

        The mode has to be one the connector already listed. A CVT line for
        this size is a size virtio-gpu will scan out, but it arrives with
        crtc_clock still 0, the CRTC never generates vblank, and the first
        flip after the picture is on screen waits forever. The 120 Hz entry
        of a listed size is still preferred over a 60 Hz established timing
        of the same width and height. When seventy percent itself is not
        listed, the under-cap pick prefers the screen's refresh before it
        prefers more pixels.
*/
static struct drm_display_mode *output_guest_mode(struct drm_device *dev,
                                                  struct drm_connector *connector)
{
        struct drm_display_mode *screen, *listed;
        int w, h;

        screen = output_screen_mode(connector);
        if (!screen)
                return NULL;

        w = screen->hdisplay * 7 / 10;
        h = screen->vdisplay * 7 / 10;
        w &= ~1;
        h &= ~1;
        if (w < 2)
                w = 2;
        if (h < 2)
                h = 2;

        listed = output_mode_wh(connector, (unsigned int)w, (unsigned int)h);
        if (listed)
                return output_mode_take(dev, listed);

        listed = output_mode_under(connector, w, h, drm_mode_vrefresh(screen));
        if (listed)
                return output_mode_take(dev, listed);

        return output_mode_take(dev, screen);
}

/*
        Probes every connector and puts the best mode on each modeset.

        The lock order is the one drm_client_modeset_probe uses: the client's
        modesets, then the device's mode configuration, which is what guards a
        connector's list of modes.
*/
static COLD int canvas_probe_modes(struct canvas *canvas, _Bool biggest)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_device *dev = client->dev;
        struct drm_mode_set *mode_set;

        if (drm_client_modeset_probe(client, 0, 0))
                return -ENODEV;

        mutex_lock(&client->modeset_mutex);
        mutex_lock(&dev->mode_config.mutex);

        drm_client_for_each_modeset(mode_set, client)
        {
                struct drm_connector *connector;
                struct drm_display_mode *want, *taken;

                if (!mode_set->mode || !mode_set->num_connectors ||
                    !mode_set->connectors || !mode_set->connectors[0])
                        continue;

                connector = mode_set->connectors[0];
                /*
                        A guest stays a window even when the first modeset is
                        refused. The retry used to take the probe's preferred
                        size, which is the host's whole screen, and that is
                        the blow-out the seventy percent cap exists to stop.
                */
                if (canvas_is_virtual(dev))
                {
                        taken = output_guest_mode(dev, connector);
                        if (!taken)
                                continue;
                        if (drm_mode_equal(taken, mode_set->mode))
                        {
                                drm_mode_destroy(dev, taken);
                                continue;
                        }
                }
                else
                {
                        want = biggest ? output_best_mode(connector)
                                       : output_mode_wh(connector,
                                                        (unsigned int)mode_set->mode->hdisplay,
                                                        (unsigned int)mode_set->mode->vdisplay);
                        if (!want || drm_mode_equal(want, mode_set->mode))
                                continue;
                        taken = output_mode_take(dev, want);
                        if (!taken)
                                continue;
                }

                drm_mode_destroy(dev, mode_set->mode);
                mode_set->mode = taken;
        }

        mutex_unlock(&dev->mode_config.mutex);
        mutex_unlock(&client->modeset_mutex);

        desktop_sync_frame_ns();
        return 0;
}

static struct output *output_for_modeset(struct canvas *canvas,
                                         struct drm_mode_set *mode_set)
{
        struct output *output;

        list_for_each_entry(output, &desktop.outputs, link)
                if (output->canvas == canvas && output->mode_set == mode_set)
                        return output;

        return NULL;
}

/*
        Puts this output's buffer back on the modeset that is scanning it.

        A probe may have replaced the mode with a different size. The buffer
        we already have is the one on the CRTC; a modeset of a new size and
        the old framebuffer is refused, and destroying that framebuffer while
        the CRTC still holds it blanks the scanout. So the running size is
        restored, and a guest window that grew can wait.
*/
static void output_attach(struct output *output)
{
        struct drm_mode_set *mode_set = output->mode_set;
        struct drm_device *dev;
        struct drm_connector *connector;
        struct drm_display_mode *want, *taken;

        if (!mode_set || !output->buffer)
                return;

        if (!mode_set->mode)
                return;

        if (mode_set->mode->hdisplay == (int)output->width &&
            mode_set->mode->vdisplay == (int)output->height)
        {
                mode_set->fb = output->buffer->fb;
                return;
        }

        dev = output->canvas->client.dev;
        connector = (mode_set->connectors && mode_set->num_connectors)
                        ? mode_set->connectors[0]
                        : NULL;
        if (!connector)
                return;

        mutex_lock(&dev->mode_config.mutex);
        want = output_mode_wh(connector, output->width, output->height);
        taken = output_mode_take(dev, want);
        mutex_unlock(&dev->mode_config.mutex);

        if (!taken)
                return;

        drm_mode_destroy(dev, mode_set->mode);
        mode_set->mode = taken;
        mode_set->fb = output->buffer->fb;
        desktop_sync_frame_ns();
}

static void desktop_place_outputs(void);
static struct output *output_add(struct canvas *canvas, struct drm_mode_set *mode_set);
static void output_disable_modeset(struct drm_device *dev,
                                   struct drm_mode_set *mode_set);

/*
        A hotplug after the first picture.

        drm_client_modeset_probe drops every modeset's framebuffer pointer.
        The old path treated a different mode -- or a different count of them
        -- as a reason to destroy the buffers still on the CRTCs. That is
        SET_SCANOUT 0. virtio-gpu then replaces the host window with
        "Display output is not active", and a later commit does not always
        get that window back.

        Guest displays also fire a hotplug about a second after the first
        scanout, when the host window is up. That callback runs on the DRM
        helper workqueue. A commit from here disables every cursor plane
        (drm_client_modeset_commit does) and can wait on that same queue,
        so the pointer thread never runs again and the plane is left off.
        The mode already scanning is the one the window has; leave it.
*/
static int canvas_rebind(struct canvas *canvas)
{
        struct drm_client_dev *client = &canvas->client;
        struct drm_mode_set *mode_set;
        struct output *output;
        _Bool placed = false;

        if (canvas_is_virtual(client->dev))
                return 0;

        if (canvas_probe_modes(canvas,
                               IS_ENABLED(CONFIG_MOONWATER_CANVAS_LARGEST_MODE)))
        {
                desktop_redraw();
                return 0;
        }

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
        {
                struct drm_connector *connector;

                if (!mode_set->mode)
                        continue;

                output = output_for_modeset(canvas, mode_set);
                if (output)
                {
                        output_attach(output);
                        continue;
                }

                connector = mode_set->num_connectors ? mode_set->connectors[0]
                                                     : NULL;
                pr_info("[moonwater canvas] " "screen %s %ux%u at %u Hz, drawn %ux, %u mode(s) offered\n", connector && connector->name ? connector->name : "?", mode_set->mode->hdisplay, mode_set->mode->vdisplay, drm_mode_vrefresh(mode_set->mode), desktop.scale, connector ? output_mode_count(connector) : 0);

                output = output_add(canvas, mode_set);
                if (!output)
                {
                        output_disable_modeset(client->dev, mode_set);
                        continue;
                }

                list_add_tail(&output->link, &desktop.outputs);
                placed = true;
        }
        mutex_unlock(&client->modeset_mutex);

        if (placed)
                desktop_place_outputs();

        desktop_redraw();
        return 0;
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
        desktop_sync_frame_ns();
}

static struct output *output_add(struct canvas *canvas, struct drm_mode_set *mode_set)
{
        unsigned int width = mode_set->mode->hdisplay;
        unsigned int height = mode_set->mode->vdisplay;
        u32 format = canvas_plane_pick_format(mode_set->crtc->primary,
                                              DRM_FORMAT_XRGB8888,
                                              DRM_FORMAT_ARGB8888);
        struct output *output;

        if (format == DRM_FORMAT_INVALID)
        {
                pr_err("[moonwater canvas] " "no 32 bit format on this plane, skipping output\n");
                return NULL;
        }

        output = kzalloc(sizeof(*output), GFP_KERNEL);
        if (!output)
                return NULL;

        output->buffer = drm_client_buffer_create_dumb(&canvas->client, width, height, format);
        if (IS_ERR(output->buffer))
        {
                pr_err("[moonwater canvas] " "could not create a %ux%u scanout buffer\n", width, height);
                kfree(output);
                return NULL;
        }

        output->canvas = canvas;
        output->mode_set = mode_set;
        output->width = width;
        output->height = height;
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

        plane_claim(&canvas->client, output);

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

        // A failed disable leaves a client buffer that recovery can no longer
        // reach after this output is gone. RMFB drops the client ownership;
        // atomic plane state keeps scanout alive even if removal also fails.
        drm_client_buffer_delete(output->cursor_buffer);
        drm_client_buffer_delete(output->buffer);

        list_del(&output->link);
        kfree(output);
}

/*
        Puts every output's buffer back on the modeset that scans it out.

        A probe releases every modeset, and releasing one takes its framebuffer
        away. A modeset carrying a mode and no framebuffer is refused, the
        whole commit with it, and what stays on the screen is whatever was
        there before this ever ran: on a machine that inherits the firmware's
        picture that is a cursor moving over it and nothing else.

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
static _Bool desktop_commit(void)
{
        struct canvas *committed = NULL;
        struct drm_rect cursor;
        struct output *output;
        _Bool complete = true;

        desktop_attach_buffers();
        cursor_cell(&cursor, desktop.cursor_x, desktop.cursor_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        list_for_each_entry(output, &desktop.outputs, link)
        {
                int set;

                if (output->canvas == committed)
                        continue;

                committed = output->canvas;
                set = drm_client_modeset_commit(&committed->client);

                if (set)
                        complete = false;

                // EBUSY is not now: something else is the device's master and
                // the next commit is the one that lands.
                if (set == -EBUSY || set == committed->set_result)
                        continue;

                committed->set_result = set;

                if (set)
                        pr_info("[moonwater canvas] " "%ux%u would not go on the screen (%d)\n", output->width, output->height, set);
                else
                        pr_info("[moonwater canvas] " "%ux%u is on the screen\n", output->width, output->height);
        }

        list_for_each_entry(output, &desktop.outputs, link)
                cursor_arm_output(output,
                                  output_touched(output, &cursor, 1));

        return complete;
}

// A failed cursor-plane disable may leave the old image live over the
// software fallback. A full client commit disables all non-primary planes.
static void cursor_plane_recover(void)
{
        struct output *output;
        _Bool complete;

        if (!cursor_plane_recovery)
                return;

        list_for_each_entry(output, &desktop.outputs, link)
                if (output->cursor_recovery == 1)
                        output->cursor_recovery = 2;

        cursor_plane_recovery = false;
        complete = desktop_commit();

        // The successful full commit covered state 2. A failure while its
        // post-commit cursor arms ran is new state 1 and needs the next pass.
        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (output->cursor_recovery != 2)
                        continue;

                if (!complete)
                {
                        output->cursor_recovery = 1;
                        continue;
                }

                if (output->cursor_buffer)
                {
                        drm_client_buffer_delete(output->cursor_buffer);
                        output->cursor_buffer = NULL;
                }

                output->cursor_recovery = 0;
        }

        if (!complete)
                cursor_plane_recovery = true;
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

                output_repaint(output, desktop.damage, desktop.damage_count);
        }

        desktop.damage_count = 0;
}

static COLD void canvas_release(struct canvas *canvas);

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

                pr_info("[moonwater canvas] " "screen %s %ux%u at %u Hz, drawn %ux, %u mode(s) offered\n", connector && connector->name ? connector->name : "?", mode_set->mode->hdisplay, mode_set->mode->vdisplay, drm_mode_vrefresh(mode_set->mode), desktop.scale, connector ? output_mode_count(connector) : 0);

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
        which is the one that used to be taken unconditionally. A guest is
        already capped: retrying still asks for seventy percent of the host,
        not the probe's native size.
*/
static int canvas_start(struct canvas *canvas)
{
        /*
                A refused commit means the mode, not the moment.

                This runs from the hotplug that drm_client_register fires, and
                the file canvas_claim opened to find the card is still open at
                that point -- so it is still the device's master, and a commit
                answers EBUSY whatever mode it was handed. Falling back on that
                threw away every mode this ever chose and quietly took the
                probe's, which is the opposite of the point.
        */
        for (unsigned int attempt = 0; ; attempt++)
        {
                int ret = canvas_build(canvas, !attempt &&
                    IS_ENABLED(CONFIG_MOONWATER_CANVAS_LARGEST_MODE));

                if (ret)
                {
                        pr_err("[moonwater canvas] " "no screen to draw on (%d), leaving the display alone\n", ret);
                        return ret;
                }

                desktop_attach_buffers();
                ret = drm_client_modeset_commit(&canvas->client);
                if (!ret || ret == -EBUSY)
                        break;

                // A rejected mode owns no useful picture. Release its outputs
                // before retrying, or standing aside for the console client.
                if (attempt)
                        pr_err("[moonwater canvas] " "no mode would set (%d), leaving the display alone\n", ret);
                else
                        pr_info("[moonwater canvas] " "that mode would not set (%d), taking the offered one\n", ret);
                canvas_release(canvas);
                if (attempt)
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

        // Before the redraw, so the first frame already carries it, and before
        // the terminal below, which is userspace and may never arrive.
        console_start();

        desktop_redraw();

        {
                struct output *output;
                unsigned int count = 0;

                list_for_each_entry(output, &desktop.outputs, link)
                        count++;

                pr_info("[moonwater canvas] " "desktop %dx%d, %u output(s)\n", desktop.width, desktop.height, count);
        }

        // Something to use it with. A desktop with nothing on it is not a
        // desktop, and this is the first program a screen is worth having.
        if (!desktop.terminal)
        {
                desktop.terminal = true;
                pr_info("[moonwater canvas] " "terminal: %d\n", spawn_terminal());
        }

        return 0;
}

static COLD void canvas_release(struct canvas *canvas)
{
        struct output *output, *next;

        list_for_each_entry_safe(output, next, &desktop.outputs, link)
                if (output->canvas == canvas)
                        output_drop(output);

        desktop_place_outputs();

        if (!list_empty(&desktop.outputs))
                desktop_redraw();
}
