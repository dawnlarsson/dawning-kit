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

static _Bool canvas_plane_takes_argb(struct drm_plane *plane)
{
        unsigned int i;

        for (i = 0; i < plane->format_count; i++)
                if (plane->format_types[i] == DRM_FORMAT_ARGB8888)
                        return true;

        return false;
}

/*
        The lock dance is the one drm_mode_cursor_common does: take the crtc
        and the plane, and back off and retry the whole thing on -EDEADLK. The
        retry has to cover update_plane too, which takes more locks of its own.

        Uninterruptible, unlike the ioctl: this is a thread with no signals to
        take, where -ERESTARTSYS would only mean a dropped mouse move.
*/
static int plane_place(struct output *output, int x, int y)
{
        struct drm_plane *plane = output->cursor_plane;
        struct drm_crtc *crtc = output->mode_set->crtc;
        struct drm_modeset_acquire_ctx ctx;
        int ret;

        drm_modeset_acquire_init(&ctx, 0);
retry:
        ret = drm_modeset_lock(&crtc->mutex, &ctx);
        if (ret)
                goto out;

        ret = drm_modeset_lock(&plane->mutex, &ctx);
        if (ret)
                goto out;

        ret = plane->funcs->update_plane(plane, crtc, output->cursor_buffer->fb,
                                         x - cursor_hot_x(output->cursor_shape),
                                         y - cursor_hot_y(output->cursor_shape),
                                         output->cursor_w, output->cursor_h,
                                         0, 0,
                                         output->cursor_w << 16,
                                         output->cursor_h << 16,
                                         &ctx);
out:
        if (ret == -EDEADLK)
        {
                drm_modeset_backoff(&ctx);
                goto retry;
        }

        drm_modeset_drop_locks(&ctx);
        drm_modeset_acquire_fini(&ctx);

        return ret;
}

static int plane_hide(struct output *output)
{
        struct drm_plane *plane = output->cursor_plane;
        struct drm_modeset_acquire_ctx ctx;
        int ret;

        drm_modeset_acquire_init(&ctx, 0);
retry:
        ret = drm_modeset_lock(&plane->mutex, &ctx);
        if (!ret)
                ret = plane->funcs->disable_plane(plane, &ctx);

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
        output->cursor_plane = NULL;
        output->cursor_shown = false;

        if (output->cursor_buffer)
        {
                drm_client_buffer_delete(output->cursor_buffer);
                output->cursor_buffer = NULL;
        }
}

/*
        Claims the crtc's cursor plane and paints the arrow into it once.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer elsewhere only uploads it when the plane's
        framebuffer changes -- virtio-gpu transfers the image on that edge and
        sends nothing but a position afterwards, which is what makes this
        cheap. An image drawn after arming would never be sent.
*/
/*
        Paints one shape into the plane's buffer.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer elsewhere only uploads it when the plane's
        framebuffer changes -- virtio-gpu transfers the image on that edge and
        sends nothing but a position afterwards, which is what makes this
        cheap. Changing shape is therefore a repaint here, not per move.
*/
static int plane_paint(struct output *output, unsigned int shape)
{
        struct iosys_map map;
        unsigned int pitch_pixels;

        if (drm_client_buffer_vmap_local(output->cursor_buffer, &map))
                return -EIO;

        pitch_pixels = output->cursor_buffer->fb->pitches[0] / sizeof(u32);

        // Transparent everywhere the shape does not cover, or it wears a box
        // of whatever the buffer was allocated holding.
        canvas_fill_rect(map.vaddr, pitch_pixels,
                         output->cursor_w, output->cursor_h,
                         0, 0, output->cursor_w, output->cursor_h,
                         0x00000000);

        canvas_draw_cursor(map.vaddr, pitch_pixels,
                           output->cursor_w, output->cursor_h,
                           cursor_hot_x(shape), cursor_hot_y(shape), shape,
                           canvas_colour(COLOUR_CURSOR, DRM_FORMAT_ARGB8888),
                           canvas_colour(COLOUR_CURSOR_EDGE, DRM_FORMAT_ARGB8888));

        drm_client_buffer_vunmap_local(output->cursor_buffer);

        output->cursor_shape = shape;
        return 0;
}

static int plane_claim(struct drm_client_dev *client, struct output *output)
{
        struct drm_plane *plane = output->mode_set->crtc->cursor;

        if (!plane || !plane->funcs->disable_plane || !canvas_plane_takes_argb(plane))
                return -ENODEV;

        output->cursor_w = client->dev->mode_config.cursor_width ?: CURSOR_W;
        output->cursor_h = client->dev->mode_config.cursor_height ?: CURSOR_H;

        if (output->cursor_w < CURSOR_W || output->cursor_h < CURSOR_H)
                return -ENODEV;

        output->cursor_buffer = drm_client_buffer_create_dumb(
            client, output->cursor_w, output->cursor_h, DRM_FORMAT_ARGB8888);
        if (IS_ERR(output->cursor_buffer))
        {
                output->cursor_buffer = NULL;
                return -ENOMEM;
        }

        if (plane_paint(output, CURSOR_ARROW))
        {
                drm_client_buffer_delete(output->cursor_buffer);
                output->cursor_buffer = NULL;
                return -EIO;
        }

        output->cursor_plane = plane;
        return 0;
}

static void cursor_arm_output(struct output *output)
{
        _Bool wanted = output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);
        int ret;

        if (!output->cursor_plane)
                return;

        if (wanted && output->cursor_shape != desktop.cursor_shape &&
            plane_paint(output, desktop.cursor_shape))
        {
                plane_drop(output);
                return;
        }

        ret = wanted ? plane_place(output, desktop.cursor_x - output->x,
                                   desktop.cursor_y - output->y)
                     : plane_hide(output);

        if (!ret)
        {
                output->cursor_shown = wanted;
                return;
        }

        // Give it up rather than leave a cursor that cannot move. The next
        // event repaints through the software path.
        log_canvas("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
        plane_drop(output);
}

static void cursor_paint(struct output *output, int old_x, int old_y,
                         unsigned int old_shape, int new_x, int new_y)
{
        struct drm_rect damage[2];

        cursor_cell(&damage[0], old_x, old_y, old_shape);
        cursor_cell(&damage[1], new_x, new_y, desktop.cursor_shape);

        output_repaint(output, damage, 2);
}

/*
        Moves the one cursor, and changes its shape where that is what changed.
        Only the outputs it left and the outputs it arrived on are touched.
*/
static void cursor_move(int new_x, int new_y)
{
        int old_x = desktop.drawn_x;
        int old_y = desktop.drawn_y;
        unsigned int old_shape = desktop.drawn_shape;
        struct output *output;

        if (old_x == new_x && old_y == new_y && old_shape == desktop.cursor_shape)
                return;

        desktop.cursor_x = new_x;
        desktop.cursor_y = new_y;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (!output_shows_cursor_shape(output, old_x, old_y, old_shape) &&
                    !output_shows_cursor(output, new_x, new_y))
                        continue;

                if (output->cursor_plane)
                {
                        u64 started = ktime_get_ns();

                        cursor_arm_output(output);
                        pointer_flush_total += ktime_get_ns() - started;

                        if (output->cursor_plane)
                                continue;
                }

                cursor_paint(output, old_x, old_y, old_shape, new_x, new_y);
        }

        desktop.drawn_x = new_x;
        desktop.drawn_y = new_y;
        desktop.drawn_shape = desktop.cursor_shape;
}
