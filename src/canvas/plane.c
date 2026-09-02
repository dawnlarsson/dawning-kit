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

        Every commit has to re-arm the plane, because drm_client_modeset_commit
        disables every non-primary plane on its device first.
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
        int hot = (int)output->cursor_scale;
        struct drm_modeset_acquire_ctx ctx;
        int ret;

        drm_modeset_acquire_init(&ctx, 0);
retry:
        /* Match drm_mode_cursor_common's global modeset lock order. */
        ret = drm_modeset_lock(&crtc->mutex, &ctx);

        if (!ret)
                ret = drm_modeset_lock(&plane->mutex, &ctx);

        if (!ret)
                ret = show ? plane->funcs->update_plane(
                                 plane, crtc, output->cursor_buffer->fb,
                                 x - canvas_cursor_hot[output->cursor_shape][0] * hot,
                                 y - canvas_cursor_hot[output->cursor_shape][1] * hot,
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

        // Never delete a buffer that a failed update may still be scanning.
        // A full client commit disables every non-primary plane; until that
        // recovery has happened the buffer stays alive but is no longer used.
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

        if (!ret && !output->cursor_recovery && output->cursor_buffer)
        {
                drm_client_buffer_delete(output->cursor_buffer);
                output->cursor_buffer = NULL;
        }
}

// The largest whole scale of a shape that fits the plane's buffer.
static PURE unsigned int plane_scale(struct output *output, unsigned int scale)
{
        while (scale > 1 && (CURSOR_W * scale > output->cursor_w ||
                             CURSOR_H * scale > output->cursor_h))
                scale--;

        return scale;
}

/*
        Paints one shape into the plane's buffer.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer elsewhere only uploads it when the plane's
        framebuffer changes -- virtio-gpu transfers the image on that edge and
        sends nothing but a position afterwards, which is what makes this
        cheap. Changing shape is therefore a repaint here, not per move.
*/
static int plane_paint(struct output *output, unsigned int shape,
                       unsigned int fitted_scale)
{
        u32 opaque_ink[INK_COUNT];
        struct iosys_map map;
        struct target t;

        canvas_palette(opaque_ink, DRM_FORMAT_ARGB8888);

        if (drm_client_buffer_vmap_local(output->cursor_buffer, &map))
                return -EIO;

        // The plane's own buffer is a target like any other: its own size,
        // no clip beyond itself, and a palette that is always opaque.
        t.pixels = map.vaddr;
        t.pitch = output->cursor_buffer->fb->pitches[0] / sizeof(u32);
        t.width = (int)output->cursor_w;
        t.height = (int)output->cursor_h;
        t.x = 0;
        t.y = 0;
        t.opaque = 0xff000000;
        t.ink = opaque_ink;
        t.clip.x1 = 0;
        t.clip.y1 = 0;
        t.clip.x2 = t.width;
        t.clip.y2 = t.height;

        // Transparent everywhere the shape does not cover, or it wears a box
        // of whatever the buffer was allocated holding.
        canvas_painted += (unsigned long)t.width * t.height;
        canvas_runs++;
        canvas_rect_fill(t.pixels, t.pitch, t.width, t.height, 0x00000000);

        canvas_draw_cursor(&t, canvas_cursor_hot[shape][0] * (int)fitted_scale,
                           canvas_cursor_hot[shape][1] * (int)fitted_scale,
                           shape, fitted_scale);

        drm_client_buffer_vunmap_local(output->cursor_buffer);
        drm_client_buffer_flush(output->cursor_buffer, NULL);

        output->cursor_shape = shape;
        output->cursor_scale = fitted_scale;
        return 0;
}

static void plane_claim(struct drm_client_dev *client, struct output *output)
{
        struct drm_plane *plane = output->mode_set->crtc->cursor;

        if (!plane || !plane->funcs->update_plane || !plane->funcs->disable_plane ||
            canvas_plane_pick_format(plane, DRM_FORMAT_ARGB8888,
                                     DRM_FORMAT_ARGB8888) == DRM_FORMAT_INVALID)
                return;

        output->cursor_w = client->dev->mode_config.cursor_width ?: CURSOR_W;
        output->cursor_h = client->dev->mode_config.cursor_height ?: CURSOR_H;

        if (output->cursor_w < CURSOR_W || output->cursor_h < CURSOR_H)
                return;

        output->cursor_buffer = drm_client_buffer_create_dumb(
            client, output->cursor_w, output->cursor_h, DRM_FORMAT_ARGB8888);
        if (IS_ERR(output->cursor_buffer))
        {
                output->cursor_buffer = NULL;
                return;
        }

        if (plane_paint(output, CURSOR_ARROW, 1))
        {
                drm_client_buffer_delete(output->cursor_buffer);
                output->cursor_buffer = NULL;
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
        log_canvas("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
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
