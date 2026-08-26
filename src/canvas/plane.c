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
                                         x - CURSOR_HOTSPOT_X,
                                         y - CURSOR_HOTSPOT_Y,
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
static int plane_claim(struct drm_client_dev *client, struct output *output)
{
        struct drm_plane *plane = output->mode_set->crtc->cursor;
        struct iosys_map map;
        unsigned int pitch_pixels;

        if (!plane || !plane->funcs->disable_plane || !canvas_plane_takes_argb(plane))
                return -ENODEV;

        // What the device reports through DRM_CAP_CURSOR_WIDTH, which is what
        // userspace would be told. Zero means the driver never set one.
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

        if (drm_client_buffer_vmap_local(output->cursor_buffer, &map))
        {
                drm_client_buffer_delete(output->cursor_buffer);
                output->cursor_buffer = NULL;
                return -EIO;
        }

        pitch_pixels = output->cursor_buffer->fb->pitches[0] / sizeof(u32);

        // Transparent everywhere the arrow does not cover, or it wears a box
        // of whatever the buffer was allocated holding.
        canvas_fill_rect(map.vaddr, pitch_pixels,
                         output->cursor_w, output->cursor_h,
                         0, 0, output->cursor_w, output->cursor_h,
                         0x00000000);

        canvas_draw_cursor(map.vaddr, pitch_pixels,
                           output->cursor_w, output->cursor_h, 0, 0,
                           canvas_colour(COLOUR_CURSOR, DRM_FORMAT_ARGB8888),
                           canvas_colour(COLOUR_CURSOR_EDGE, DRM_FORMAT_ARGB8888));

        drm_client_buffer_vunmap_local(output->cursor_buffer);

        output->cursor_plane = plane;
        return 0;
}

static void cursor_arm_output(struct output *output)
{
        _Bool wanted = output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);
        int ret;

        if (!output->cursor_plane)
                return;

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

static void cursor_paint(struct output *output, int old_x, int old_y, int new_x, int new_y)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        struct drm_rect damage;
        u64 started;

        if (drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);
        started = ktime_get_ns();

        compose_rect(output, pixels, pitch_pixels, old_x, old_y, CURSOR_W, CURSOR_H);
        compose_rect(output, pixels, pitch_pixels, new_x, new_y, CURSOR_W, CURSOR_H);

        output->cursor_shown = output_shows_cursor(output, new_x, new_y);

        if (output->cursor_shown)
                canvas_draw_cursor(pixels, pitch_pixels, output->width, output->height,
                                   new_x - output->x, new_y - output->y,
                                   canvas_colour(COLOUR_CURSOR, output->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, output->format));

        drm_client_buffer_vunmap_local(output->buffer);
        pointer_draw_total += ktime_get_ns() - started;

        // Clipped: the cursor may sit against an edge, or half of it may be on
        // the next screen along.
        damage.x1 = max(min(old_x, new_x) - output->x, 0);
        damage.y1 = max(min(old_y, new_y) - output->y, 0);
        damage.x2 = min(max(old_x, new_x) + CURSOR_W - output->x, (int)output->width);
        damage.y2 = min(max(old_y, new_y) + CURSOR_H - output->y, (int)output->height);

        if (damage.x2 <= damage.x1 || damage.y2 <= damage.y1)
                return;

        started = ktime_get_ns();
        drm_client_buffer_flush(output->buffer, &damage);
        pointer_flush_total += ktime_get_ns() - started;
}

/*
        Moves the one cursor. Only the outputs it left and the outputs it
        arrived on are touched, which for a move inside one screen is one
        output and for a move across a seam is two.
*/
static void cursor_move(int new_x, int new_y)
{
        int old_x = desktop.drawn_x;
        int old_y = desktop.drawn_y;
        struct output *output;

        desktop.cursor_x = new_x;
        desktop.cursor_y = new_y;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (!output_shows_cursor(output, old_x, old_y) &&
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

                cursor_paint(output, old_x, old_y, new_x, new_y);
        }

        desktop.drawn_x = new_x;
        desktop.drawn_y = new_y;
}
