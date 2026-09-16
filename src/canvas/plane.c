/*
        Canvas -- the cursor

        One cursor, on whichever outputs it currently overlaps.

        A hardware cursor plane is composited by the display engine during
        scanout, so moving it touches no pixels: the commit carries a position
        and nothing else. drm_atomic_helper_update_plane sets
        legacy_cursor_update whenever the plane is the crtc's cursor, and the
        commit helper then completes without waiting for vblank. Drawing into
        the framebuffer instead means a damage rectangle, and on an atomic
        driver that is a commit that does wait -- a cursor move cannot land in
        less than a frame however little is drawn.

        Every commit has to re-arm the plane: on the atomic drivers used here,
        drm_client_modeset_commit disables every non-primary plane first.
*/

/*
        The lock dance is the one drm_mode_cursor_common does: take the crtc
        and the plane, and back off and retry the whole thing on -EDEADLK. The
        retry has to cover update_plane too, which takes more locks of its own.

        Uninterruptible, unlike the ioctl: this is a thread with no signals to
        take, where -ERESTARTSYS would only mean a dropped mouse move.
*/
static int plane_update(struct output *output, _Bool show, int x, int y)
{
        struct drm_plane *plane = output->cursor_plane;
        struct drm_crtc *crtc = output->mode_set->crtc;
        struct drm_rect cell;
        struct drm_modeset_acquire_ctx ctx;
        int ret;

        cursor_cell(&cell, x, y, output->cursor_shape, output->cursor_scale);
        drm_modeset_acquire_init(&ctx, 0);
retry:
        /* Match drm_mode_cursor_common's global modeset lock order. */
        ret = drm_modeset_lock(&crtc->mutex, &ctx);

        if (!ret)
                ret = drm_modeset_lock(&plane->mutex, &ctx);

        if (!ret)
                ret = show ? plane->funcs->update_plane(
                                 plane, crtc, output->cursor_buffer->fb,
                                 cell.x1, cell.y1,
                                 output->cursor_w, output->cursor_h, 0, 0,
                                 output->cursor_w << 16, output->cursor_h << 16, &ctx)
                           : plane->funcs->disable_plane(plane, &ctx);

        if (ret == -EDEADLK)
        {
                drm_modeset_backoff(&ctx);
                goto retry;
        }

        drm_modeset_drop_locks(&ctx);
        drm_modeset_acquire_fini(&ctx);

        return ret;
}

static void plane_drop(struct output *output)
{
        int ret = 0;

        // Keep a failed cursor for this output's next full client commit,
        // which disables non-primary planes. Output destruction instead
        // releases the client buffer through DRM's framebuffer removal.
        if (output->cursor_plane)
                ret = plane_update(output, false, 0, 0);

        if (ret)
        {
                atomic_long_inc(&cursor_plane_failures);
                output->cursor_recovery = 1;
                cursor_plane_recovery = true;
        }

        output->cursor_plane = NULL;
        output->cursor_shown = false;

        if (!ret && !output->cursor_recovery)
        {
                drm_client_buffer_delete(output->cursor_buffer);
                drm_client_buffer_delete(output->cursor_back);
                output->cursor_buffer = NULL;
                output->cursor_back = NULL;
        }
}

// The largest whole scale of a shape that fits the plane's buffer.
static PURE unsigned int plane_scale(struct output *output, unsigned int scale)
{
        return min(scale, max(1u, min(output->cursor_w / CURSOR_W,
                                      output->cursor_h / CURSOR_H)));
}

/*
        Paints one shape into the plane's buffer.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer elsewhere only uploads it when the plane's
        framebuffer changes -- virtio-gpu transfers the image on that edge and
        sends nothing but a position afterwards, which is what makes this
        cheap. Changing shape is therefore a repaint here, not per move, and
        the repaint has to land in a different framebuffer object than the
        one already on the plane: flushing new pixels into the same object
        is not that edge, so the arrow from the first paint stayed up for
        every later shape.
*/
static int plane_paint(struct output *output, unsigned int shape,
                       unsigned int fitted_scale)
{
        struct drm_client_buffer *into = output->cursor_buffer;
        u32 opaque_ink[INK_COUNT];
        struct iosys_map map;
        struct target t;

        canvas_palette(opaque_ink, DRM_FORMAT_ARGB8888);

        if (output->cursor_plane)
        {
                if (!output->cursor_back)
                {
                        output->cursor_back = drm_client_buffer_create_dumb(
                            &output->canvas->client, output->cursor_w,
                            output->cursor_h, DRM_FORMAT_ARGB8888);
                        if (IS_ERR(output->cursor_back))
                                output->cursor_back = NULL;
                }

                if (output->cursor_back)
                        into = output->cursor_back;
        }

        if (drm_client_buffer_vmap_local(into, &map))
                return -EIO;

        t.pixels = map.vaddr;
        t.pitch = into->fb->pitches[0] / sizeof(u32);
        t.width = (int)output->cursor_w;
        t.height = (int)output->cursor_h;
        t.x = 0;
        t.y = 0;
        t.opaque = 0xff000000;
        t.ink = opaque_ink;
        drm_rect_init(&t.clip, 0, 0, t.width, t.height);

        // Transparent everywhere the shape does not cover, or it wears a box
        // of whatever the buffer was allocated holding.
        target_rectangle(&t, 0, 0, t.width, t.height, 0x00000000);
        canvas_draw_cursor(&t,
                           canvas_cursor_hot[shape][0] * (int)fitted_scale,
                           canvas_cursor_hot[shape][1] * (int)fitted_scale,
                           shape, fitted_scale);

        drm_client_buffer_vunmap_local(into);
        drm_client_buffer_flush(into, NULL);

        if (into != output->cursor_buffer)
        {
                output->cursor_back = output->cursor_buffer;
                output->cursor_buffer = into;
        }

        output->cursor_shape = shape;
        output->cursor_scale = fitted_scale;
        return 0;
}

/*
        moonwater.cursor_plane=0 turns the plane off: every output draws the
        cursor into its framebuffer, as one with no cursor plane does.

        The escape hatch for a machine whose cursor plane is broken --
        misplaced, stale, or never shown -- on a card where the software cursor
        still works, slower on every move and otherwise the same. The canvas
        lane boots with it too, because QEMU's screendump holds the primary
        plane only and its pixel checks are the software cursor's. Read back
        under /sys/module/moonwater/parameters, and by the pointer applet.
*/
static bool canvas_cursor_plane = true;
module_param_named(cursor_plane, canvas_cursor_plane, bool, 0444);

/*
        Said, once for the output it happened to, because what it leaves is
        the software cursor: identical on screen and slower on every move, so
        nothing else would ever show that a plane was there to be had. It went
        unseen that way on virtio-gpu for as long as the fallback below was the
        arrow's own size.
*/
static COLD void plane_lost(struct drm_client_dev *client, struct output *output,
                            const char *why, long error)
{
        pr_info("[moonwater canvas] " "%s cursor plane %ux%u %s (%ld), drawing the cursor instead\n",
                client->dev->driver->name, output->cursor_w, output->cursor_h, why, error);
}

static void plane_claim(struct drm_client_dev *client, struct output *output)
{
        struct drm_plane *plane = output->mode_set->crtc->cursor;
        const struct drm_mode_config *config = &client->dev->mode_config;
        int ret;

        if (!canvas_cursor_plane)
        {
                pr_info_once("[moonwater canvas] " "cursor plane turned off (moonwater.cursor_plane=0), drawing the cursor instead\n");
                return;
        }

        /*
                i915's cursor plane does not scan a dumb buffer.

                Haswell will take the object and then stop the pipe the
                moment the plane is armed: the first picture (kernel log)
                stays, the pointer thread waits out a cursor update that
                never completes, and there is no terminal. virtio-gpu is
                why the plane exists; i915's dirtyfb is a frontbuffer
                flush, so drawing the arrow into the scanout is cheap.
                moonwater.cursor_plane=0 is the same drawing everywhere.
        */
        if (client->dev->driver && client->dev->driver->name &&
            (!strcmp(client->dev->driver->name, "i915") ||
             !strcmp(client->dev->driver->name, "xe")))
        {
                pr_info_once("[moonwater canvas] " "%s cursor plane skipped, drawing the cursor instead\n",
                             client->dev->driver->name);
                return;
        }

        // Direct callbacks rely on atomic state owning framebuffer references;
        // legacy callbacks need core bookkeeping and a different recovery path.
        if (!drm_drv_uses_atomic_modeset(client->dev) || !plane ||
            !plane->funcs->update_plane || !plane->funcs->disable_plane ||
            canvas_plane_pick_format(plane, DRM_FORMAT_ARGB8888,
                                     DRM_FORMAT_ARGB8888) == DRM_FORMAT_INVALID)
                return;

        /*
                The size the driver asks for, and 64 when it asks for none --
                what DRM_CAP_CURSOR_WIDTH and _HEIGHT answer a compositor in
                userspace for the same silence -- then raised to the smallest
                framebuffer the driver makes at all.

                The arrow's own 16x20 was the fallback. virtio-gpu sets no
                cursor size and refuses a framebuffer under 32x32, so this
                buffer failed with EINVAL and every move was drawn into the
                screen instead: a blocking commit in the canvas thread, a
                display period long, beside a plane nothing used. The arrow is
                drawn at its hotspot inside whatever buffer this is, and
                plane_scale fits the shape to it, so a larger buffer moves
                nothing but where the transparent part ends.
        */
        output->cursor_w = max_t(unsigned int, config->cursor_width ?: 64,
                                 config->min_width);
        output->cursor_h = max_t(unsigned int, config->cursor_height ?: 64,
                                 config->min_height);

        if (config->max_width)
                output->cursor_w = min_t(unsigned int, output->cursor_w,
                                         config->max_width);
        if (config->max_height)
                output->cursor_h = min_t(unsigned int, output->cursor_h,
                                         config->max_height);

        if (output->cursor_w < CURSOR_W || output->cursor_h < CURSOR_H)
        {
                plane_lost(client, output, "is smaller than the arrow", 0);
                return;
        }

        output->cursor_buffer = drm_client_buffer_create_dumb(
            client, output->cursor_w, output->cursor_h, DRM_FORMAT_ARGB8888);
        if (IS_ERR(output->cursor_buffer))
        {
                plane_lost(client, output, "has no buffer",
                           PTR_ERR(output->cursor_buffer));
                output->cursor_buffer = NULL;
                return;
        }

        ret = plane_paint(output, CURSOR_ARROW, 1);
        if (ret)
        {
                plane_lost(client, output, "would not take the arrow", ret);
                drm_client_buffer_delete(output->cursor_buffer);
                drm_client_buffer_delete(output->cursor_back);
                output->cursor_buffer = NULL;
                output->cursor_back = NULL;
                return;
        }

        output->cursor_plane = plane;
}

static void cursor_arm_output(struct output *output, _Bool wanted)
{
        unsigned int scale;
        int ret;

        if (!output->cursor_plane)
                return;

        scale = wanted ? plane_scale(output, desktop.cursor_scale) : 0;

        if (wanted &&
            (output->cursor_shape != desktop.cursor_shape ||
             output->cursor_scale != scale) &&
            plane_paint(output, desktop.cursor_shape, scale))
        {
                atomic_long_inc(&cursor_plane_failures);
                plane_drop(output);
                return;
        }

        ret = plane_update(output, wanted, desktop.cursor_x - output->x,
                           desktop.cursor_y - output->y);

        if (!ret)
        {
                if (wanted)
                        atomic_long_inc(&cursor_plane_updates);
                output->cursor_shown = wanted;
                return;
        }

        // Give it up rather than leave a cursor that cannot move. The next
        // event repaints through the software path.
        atomic_long_inc(&cursor_plane_failures);
        pr_info("[moonwater canvas] " "cursor plane refused an update (%d), drawing the cursor instead\n", ret);
        plane_drop(output);
}

/*
        Moves the one cursor, and changes its shape where that is what changed.
        Only the outputs it left and the outputs it arrived on are touched.

        During a window drag the window repaint carries a software cursor with
        it, but a hardware cursor is a different plane and that repaint cannot
        move it. planes_only arms those planes immediately, before the more
        expensive window compose, and leaves the software damage and drawn
        coordinates for pane_reshape to finish in the same pass.
*/
static _Bool cursor_move_core(int new_x, int new_y, _Bool planes_only)
{
        int old_x = desktop.drawn_x;
        int old_y = desktop.drawn_y;
        unsigned int old_shape = desktop.drawn_shape;
        unsigned int old_scale = desktop.drawn_scale;
        struct drm_rect damage[2];
        struct output *output;
        _Bool plane_presented = false;
        _Bool plane_complete = true;

        if (old_x == new_x && old_y == new_y &&
            old_shape == desktop.cursor_shape && old_scale == desktop.cursor_scale)
                return false;

        desktop.cursor_x = new_x;
        desktop.cursor_y = new_y;

        /*
                Cursor geometry is desktop geometry.  Computing both cells
                once keeps the output walk to two overlap checks; formerly it
                rebuilt the new cell twice and the old cell once per output,
                including on the urgent plane-only resize path.
        */
        cursor_cell(&damage[0], old_x, old_y, old_shape, old_scale);
        cursor_cell(&damage[1], new_x, new_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        list_for_each_entry(output, &desktop.outputs, link)
        {
                _Bool wanted = output_touched(output, &damage[1], 1);

                if (!output_touched(output, &damage[0], 1) && !wanted)
                        continue;

                if (output->cursor_plane)
                {
                        u64 started = ktime_get_ns();

                        cursor_arm_output(output, wanted);
                        pointer_flush_total += ktime_get_ns() - started;

                        if (output->cursor_plane)
                        {
                                if (wanted)
                                        plane_presented = true;
                                continue;
                        }
                }

                // A failed hide matters too: the old cursor may still be on
                // screen, so this request was not an all-plane completion.
                plane_complete = false;

                if (!planes_only)
                        output_repaint(output, damage, 2);
        }

        if (!planes_only)
        {
                desktop.drawn_x = new_x;
                desktop.drawn_y = new_y;
                desktop.drawn_shape = desktop.cursor_shape;
                desktop.drawn_scale = desktop.cursor_scale;
        }

        return plane_presented && plane_complete;
}

static _Bool cursor_move_planes(int new_x, int new_y)
{
        _Bool complete;

        cursor_plane_requested_generation++;
        cursor_plane_requested_x = new_x;
        cursor_plane_requested_y = new_y;
        complete = cursor_move_core(new_x, new_y, true);

        if (complete)
        {
                cursor_plane_armed_generation = cursor_plane_requested_generation;
                cursor_plane_armed_x = new_x;
                cursor_plane_armed_y = new_y;
        }

        return complete;
}

static void cursor_move(int new_x, int new_y)
{
        cursor_move_core(new_x, new_y, false);
}
