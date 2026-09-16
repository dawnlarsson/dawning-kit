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

/*
        The firmware's leftover framebuffer, not a GPU.

        simpledrm (and efidrm) bind to whatever GOP left in sysfb. On a
        Dell with i915 that is card0 at the firmware's size -- often
        1024x768 -- and Canvas starting there paints the kernel log into
        that buffer. i915 then takes the same pipe; the firmware client is
        kicked with a picture on it, and the machine freezes. It is a last
        resort when no real card appears.
*/
static PURE _Bool canvas_is_firmware(struct drm_device *dev)
{
        static const char *const firmware[] = {"simpledrm", "efidrm"};

        if (!dev->driver || !dev->driver->name)
                return false;

        return string_table_find((string_address)dev->driver->name, firmware,
                                 sizeof(firmware[0]), array_count(firmware)) <
               array_count(firmware);
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
        The best mode a connector lists among those that pass: every bit of
        type set, a type of 0 being any, and when width is not 0 a size equal
        to width by height or, unless exact, one that fits under it. Of those:
        a refresh equal to prefer_refresh first when it is positive, then the
        most pixels, then the highest refresh.

        Same size at 60 Hz and 120 Hz is the 120 Hz entry: that is what a
        Mac's virtio EDID actually offers, and what the cursor needs. A guest
        cap of seventy percent of a 120 Hz panel often still lists a larger
        60 Hz established timing under that cap; pixels first would take it
        and throw the refresh away, so the screen's rate comes first.
*/
static struct drm_display_mode *output_pick_mode(struct drm_connector *connector,
                                                 unsigned int type, int width,
                                                 int height, _Bool exact,
                                                 int prefer_refresh)
{
        struct drm_display_mode *mode, *best = NULL;
        int best_score = 0, best_refresh = 0;
        _Bool best_preferred = false;

        list_for_each_entry(mode, &connector->modes, head)
        {
                int score, refresh;
                _Bool preferred;

                if ((mode->type & type) != type)
                        continue;
                if (width && (exact ? mode->hdisplay != width ||
                                          mode->vdisplay != height
                                    : mode->hdisplay > width ||
                                          mode->vdisplay > height))
                        continue;
                if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
                        continue;

                refresh = drm_mode_vrefresh(mode);
                score = mode->hdisplay * mode->vdisplay;
                preferred = prefer_refresh > 0 && refresh == prefer_refresh;
                if (best && (preferred < best_preferred ||
                             (preferred == best_preferred &&
                              (score < best_score ||
                               (score == best_score && refresh <= best_refresh)))))
                        continue;

                best = mode;
                best_score = score;
                best_refresh = refresh;
                best_preferred = preferred;
        }

        return best;
}

/*
        The mode the first successful start put on a connector.

        Off and on probes again. A guest's preferred size is then the window
        already committed -- seventy percent of that, not of the host -- and
        a real screen's probe is the preferred mode, not the largest one
        startup took. Remembering the committed size keeps restart on the
        same picture.
*/
#define CANVAS_SAVED_MODES 8

struct canvas_saved_mode
{
        char name[32];
        int hdisplay;
        int vdisplay;
        int vrefresh;
};

static struct canvas_saved_mode canvas_saved_mode[CANVAS_SAVED_MODES];
static unsigned int canvas_saved_modes;

static const struct canvas_saved_mode *canvas_mode_saved(const char *name)
{
        unsigned int i;

        if (!name)
                return NULL;

        for (i = 0; i < canvas_saved_modes; i++)
                if (!strcmp(canvas_saved_mode[i].name, name))
                        return &canvas_saved_mode[i];

        return NULL;
}

static void canvas_mode_keep(const char *name, const struct drm_display_mode *mode)
{
        unsigned int i;

        if (!name || !mode)
                return;

        for (i = 0; i < canvas_saved_modes; i++)
                if (!strcmp(canvas_saved_mode[i].name, name))
                {
                        canvas_saved_mode[i].hdisplay = mode->hdisplay;
                        canvas_saved_mode[i].vdisplay = mode->vdisplay;
                        canvas_saved_mode[i].vrefresh = drm_mode_vrefresh(mode);
                        return;
                }

        if (canvas_saved_modes >= CANVAS_SAVED_MODES)
                return;

        strscpy(canvas_saved_mode[canvas_saved_modes].name, name,
                sizeof(canvas_saved_mode[0].name));
        canvas_saved_mode[canvas_saved_modes].hdisplay = mode->hdisplay;
        canvas_saved_mode[canvas_saved_modes].vdisplay = mode->vdisplay;
        canvas_saved_mode[canvas_saved_modes].vrefresh = drm_mode_vrefresh(mode);
        canvas_saved_modes++;
}

static void canvas_modes_keep(void)
{
        struct output *output;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                struct drm_mode_set *set = output->mode_set;

                if (!set || !set->mode || !set->num_connectors ||
                    !set->connectors || !set->connectors[0] ||
                    !set->connectors[0]->name)
                        continue;

                canvas_mode_keep(set->connectors[0]->name, set->mode);
        }
}

/*
        The host's idea of the screen: the preferred mode, or the largest
        if the connector did not mark one.
*/
static struct drm_display_mode *output_screen_mode(struct drm_connector *connector)
{
        struct drm_display_mode *preferred = output_pick_mode(
            connector, DRM_MODE_TYPE_PREFERRED, 0, 0, false, 0);

        return preferred ? preferred
                         : output_pick_mode(connector, 0, 0, 0, false, 0);
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

        listed = output_pick_mode(connector, 0, w, h, true, 0);
        if (listed)
                return output_mode_take(dev, listed);

        listed = output_pick_mode(connector, 0, w, h, false, drm_mode_vrefresh(screen));
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
static COLD int canvas_probe_modes(struct canvas *canvas, _Bool biggest,
                                   _Bool keep_saved)
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
                const struct canvas_saved_mode *saved;

                if (!mode_set->mode || !mode_set->num_connectors ||
                    !mode_set->connectors || !mode_set->connectors[0])
                        continue;

                connector = mode_set->connectors[0];
                saved = keep_saved && connector->name
                                ? canvas_mode_saved(connector->name)
                                : NULL;
                /*
                        A guest stays a window even when the first modeset is
                        refused. The retry used to take the probe's preferred
                        size, which is the host's whole screen, and that is
                        the blow-out the seventy percent cap exists to stop.

                        A later start has already committed once: take that
                        size rather than seventy percent of the window, or
                        the probe's preferred instead of the largest.

                        A hotplug on a running card must not: the first
                        picture is often a fallback written before EDID
                        finished, and restoring that size leaves a real
                        screen at 1024x768 for the rest of the boot.
                */
                if (saved)
                {
                        want = output_pick_mode(connector, 0, saved->hdisplay,
                                                saved->vdisplay, true,
                                                saved->vrefresh);
                        if (!want || drm_mode_equal(want, mode_set->mode))
                                continue;
                        taken = output_mode_take(dev, want);
                        if (!taken)
                                continue;
                }
                else if (canvas_is_virtual(dev))
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
                        want = biggest ? output_pick_mode(connector, 0, 0, 0, false, 0)
                                       : output_pick_mode(connector, 0,
                                                          mode_set->mode->hdisplay,
                                                          mode_set->mode->vdisplay,
                                                          true, 0);
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
        want = output_pick_mode(connector, 0, (int)output->width,
                                (int)output->height, true, 0);
        taken = output_mode_take(dev, want);
        mutex_unlock(&dev->mode_config.mutex);

        if (!taken)
                return;

        drm_mode_destroy(dev, mode_set->mode);
        mode_set->mode = taken;
        mode_set->fb = output->buffer->fb;
        desktop_sync_frame_ns();
}

/*
        A later probe listed a larger mode than the one already scanning.

        The first picture is often a fallback: i915 writes 1024x768 before
        EDID finishes, then hotplugs with the panel's real list. The buffer
        on the CRTC is the old size, so a modeset of the new size with that
        framebuffer is refused. A new buffer is made first; the old one is
        kept until the commit that points the pipe at the new one has
        landed, because freeing it sooner blanks the scanout.
*/
static _Bool output_grow(struct output *output, struct drm_mode_set *mode_set)
{
        unsigned int width, height;
        u32 format;
        struct drm_client_buffer *fresh;

        if (!mode_set || !mode_set->mode || !mode_set->crtc ||
            !mode_set->crtc->primary)
                return false;

        width = mode_set->mode->hdisplay;
        height = mode_set->mode->vdisplay;
        if ((unsigned long)width * height <=
            (unsigned long)output->width * output->height)
                return false;

        format = canvas_plane_pick_format(mode_set->crtc->primary,
                                          DRM_FORMAT_XRGB8888,
                                          DRM_FORMAT_ARGB8888);
        if (format == DRM_FORMAT_INVALID)
                return false;

        fresh = drm_client_buffer_create_dumb(&output->canvas->client, width,
                                              height, format);
        if (IS_ERR(fresh))
                return false;

        if (output->replaced)
                drm_client_buffer_delete(output->buffer);
        else
                output->replaced = output->buffer;

        output->buffer = fresh;
        output->width = width;
        output->height = height;
        output->opaque = format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0;
        canvas_palette(output->palette, format);
        mode_set->fb = fresh->fb;
        desktop_sync_frame_ns();
        return true;
}

static void desktop_place_outputs(void);
static struct output *output_add(struct canvas *canvas, struct drm_mode_set *mode_set);
static void output_disable_modeset(struct drm_device *dev,
                                   struct drm_mode_set *mode_set);

/*
        A screen with a mode: say which, and how much of a choice there was,
        then put an output on it. A mode that turns out to be wrong on a
        machine that is not here is answered by what its connector offered,
        not by what was picked out of it. A screen that cannot take an output
        has its mode set turned off.
*/
static _Bool output_bring_up(struct canvas *canvas, struct drm_mode_set *mode_set)
{
        struct drm_connector *connector =
            mode_set->num_connectors ? mode_set->connectors[0] : NULL;
        struct output *output;

        pr_info("[moonwater canvas] " "screen %s %ux%u at %u Hz, drawn %ux, %u mode(s) offered\n", connector && connector->name ? connector->name : "?", mode_set->mode->hdisplay, mode_set->mode->vdisplay, drm_mode_vrefresh(mode_set->mode), desktop.scale, connector ? output_mode_count(connector) : 0);

        output = output_add(canvas, mode_set);
        if (!output)
        {
                output_disable_modeset(canvas->client.dev, mode_set);
                return false;
        }

        list_add_tail(&output->link, &desktop.outputs);
        return true;
}

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

        A real card is the same queue. i915 hotplugs after EDID, and with a
        cursor plane the commit from here is the virtio lockup: low-res
        kernel log, no pointer, no terminal. The callback only queues; this
        runs on moonwater/plug. A guest still returns above. A real screen
        may grow if the connector now lists more pixels than the fallback
        that was committed first; a hotplug that does not grow does not
        commit, so the cursor plane stays up.
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
                               IS_ENABLED(CONFIG_MOONWATER_CANVAS_LARGEST_MODE),
                               false))
        {
                desktop_redraw();
                return 0;
        }

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
        {
                if (!mode_set->mode)
                        continue;

                output = output_for_modeset(canvas, mode_set);
                if (output)
                {
                        if (output_grow(output, mode_set))
                                placed = true;
                        else
                                output_attach(output);
                        continue;
                }

                if (output_bring_up(canvas, mode_set))
                        placed = true;
        }
        mutex_unlock(&client->modeset_mutex);

        if (placed)
        {
                desktop_place_outputs();
                canvas_modes_keep();
                desktop_redraw();
        }

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

static void output_free(struct output *output)
{
        drm_client_buffer_delete(output->buffer);
        drm_client_buffer_delete(output->replaced);
        kfree(output);
}

static void output_drop(struct output *output)
{
        _Bool flushing;

        plane_drop(output);

        // A failed disable leaves a client buffer that recovery can no longer
        // reach after this output is gone. RMFB drops the client ownership;
        // atomic plane state keeps scanout alive even if removal also fails.
        drm_client_buffer_delete(output->cursor_buffer);
        drm_client_buffer_delete(output->cursor_back);
        output->cursor_buffer = NULL;
        output->cursor_back = NULL;

        list_del(&output->link);

        /*
                Off the flusher's queue, and left to the flusher if its buffer
                is with the driver right now: deleting it under a dirtyfb in
                flight frees what the driver is using. output_flush_done frees
                it instead, and client_unregister waits for that.
        */
        spin_lock(&desktop.flush_lock);
        if (output->flush_queued)
        {
                list_del_init(&output->flush_link);
                output->flush_queued = false;
        }
        flushing = output->flushing;
        output->retired = flushing;
        if (flushing)
                atomic_fetch_add(1, &output->canvas->retiring);
        spin_unlock(&desktop.flush_lock);

        if (!flushing)
                output_free(output);
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
                if (!set)
                {
                        struct output *grown;

                        list_for_each_entry(grown, &desktop.outputs, link)
                                if (grown->canvas == committed && grown->replaced)
                                {
                                        drm_client_buffer_delete(grown->replaced);
                                        grown->replaced = NULL;
                                }
                }

                // Somebody else is master, and the card is not ours to draw
                // on until they let go. The loop is woken so it watches for that.
                if (set == -EBUSY || set == -EACCES)
                {
                        desktop.suspended = true;
                        canvas_thread_wake();
                }

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

                drm_client_buffer_delete(output->cursor_buffer);
                drm_client_buffer_delete(output->cursor_back);
                output->cursor_buffer = NULL;
                output->cursor_back = NULL;

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

/*
        Whether a program other than this one is master of a card Canvas draws
        on: Weston, a game, anything that opened the card for itself.

        In-kernel clients never become master, so a device with one has a
        program in front of it. Tested under the device's master_mutex, taken
        inside desktop.lock the way drm_client_modeset_commit already takes
        it, and never dereferenced: the master can go the moment the lock is
        dropped, and all this answers is whether there was one.
*/
static _Bool desktop_taken(void)
{
        struct drm_device *checked = NULL;
        struct output *output;
        _Bool taken = false;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                struct drm_device *dev = output->canvas->client.dev;

                if (dev == checked)
                        continue;

                checked = dev;
                mutex_lock(&dev->master_mutex);
                taken = dev->master != NULL;
                mutex_unlock(&dev->master_mutex);

                if (taken)
                        break;
        }

        return taken;
}

/*
        The card is Canvas's again.

        Nothing Canvas remembers about the screen can be trusted after another
        program had it: the mode on each crtc is theirs, and so may be the
        image, the position and whether there is anything at all on the cursor
        plane. So the windows are read again, everything is drawn and
        committed, which puts Canvas's modes back and disables every plane
        but the primary, and each cursor plane is painted and armed afresh
        under a new request generation that only counts as armed once every
        output showing the cursor has it back: on its plane where it has one,
        and drawn by the redraw where it has none.
*/
static void desktop_resume(void)
{
        struct drm_rect cursor;
        struct output *output;
        _Bool presented = false;
        _Bool complete = true;

        desktop.suspended = false;

        cursor_plane_requested_generation++;
        cursor_plane_requested_x = desktop.cursor_x;
        cursor_plane_requested_y = desktop.cursor_y;

        list_for_each_entry(output, &desktop.outputs, link)
                output->cursor_shape = ~0u;

        desktop_refresh_panes();
        desktop_redraw();
        cursor_plane_recover();

        cursor_cell(&cursor, desktop.cursor_x, desktop.cursor_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (!output_touched(output, &cursor, 1))
                        continue;

                // An output with no cursor plane had its cursor drawn by the
                // redraw above; one with a plane has it only if it is shown.
                if (!output->cursor_plane || output->cursor_shown)
                        presented = true;
                else
                        complete = false;
        }

        if (presented && complete && !desktop.suspended)
        {
                cursor_plane_armed_generation = cursor_plane_requested_generation;
                cursor_plane_armed_x = desktop.cursor_x;
                cursor_plane_armed_y = desktop.cursor_y;
        }
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
        struct drm_mode_set *mode_set;
        unsigned int count = 0;

        if (canvas_probe_modes(canvas, biggest, true))
                return -ENODEV;

#ifdef CONFIG_MOONWATER_CANVAS_SCALE
        desktop.scale = CONFIG_MOONWATER_CANVAS_SCALE;
#endif
        if (desktop.scale < 1)
                desktop.scale = 1;

        mutex_lock(&client->modeset_mutex);
        drm_client_for_each_modeset(mode_set, client)
        {
                if (!mode_set->mode)
                        continue;

                if (output_bring_up(canvas, mode_set))
                        count++;
        }
        mutex_unlock(&client->modeset_mutex);

        if (!count)
                return -ENODEV;

        desktop_place_outputs();

        return 0;
}

/*
        The first terminal waits for the initcalls.

        A built-in canvas has a screen at device_initcall, but nothing in
        userspace is meant to run before every initcall has -- floodlight, for
        one, registers its device at late_initcall on exactly that promise. A
        terminal started into that gap exited without opening a window, on
        every boot, and left the log alone on the screen; the same /term run
        once init had started opened one. So the first is held until the
        initcalls are done, and a canvas that comes up after that -- a late
        card, or the module loaded by init -- starts it at once.

        Both sides under desktop.lock, which canvas_start already holds; the
        spawn itself happens on the canvas thread, outside that lock.
*/
#ifdef MODULE
static _Bool canvas_initcalls_done = true;
#else
static _Bool canvas_initcalls_done;
#endif
static _Bool canvas_terminal_waiting;

static void canvas_terminal_first(void)
{
        if (!canvas_initcalls_done)
        {
                canvas_terminal_waiting = true;
                return;
        }

        /*
                Outside desktop.lock, on the canvas thread.

                canvas_start holds that lock for the first picture. The
                terminal's first WINDOW ioctl takes it too, so spawning
                from here would wait out the child's open. The thread
                already starts Control-Shift-T that way.
        */
        atomic_set(&desktop.spawn, 1);
        canvas_thread_wake();
}

#ifndef MODULE
static int __init canvas_initcalls_finished(void)
{
        _Bool waiting;

        rt_mutex_lock(&desktop.lock);
        canvas_initcalls_done = true;
        waiting = canvas_terminal_waiting;
        canvas_terminal_waiting = false;
        rt_mutex_unlock(&desktop.lock);

        if (waiting)
        {
                atomic_set(&desktop.spawn, 1);
                canvas_thread_wake();
        }

        return 0;
}
late_initcall_sync(canvas_initcalls_finished);
#endif

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

                The claim file is closed before drm_client_register, and this
                runs from moonwater/plug after that, so a commit is not EBUSY
                because we still hold master. EBUSY here is another program.
                Falling back on that threw away every mode this ever chose and
                quietly took the probe's, which is the opposite of the point.
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
        canvas_modes_keep();

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
                canvas_terminal_first();
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
