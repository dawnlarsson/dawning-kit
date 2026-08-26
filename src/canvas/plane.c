/*
        Canvas -- the cursor plane

        Hardware first, software second. A DRM cursor plane moves with an
        atomic commit and no drawing at all; where there is none, the arrow is
        composited into the scanout buffer like anything else and the two
        rectangles it left and arrived in are redrawn.

        The plane has to be re-armed after every modeset commit, because
        drm_client_modeset_commit disables every plane that is not the
        primary. That is the least obvious thing in this directory.
*/

/*
        The hardware cursor.

        A cursor plane is composited by the display engine during scanout, so
        moving it touches no pixels at all: the commit carries a position and
        nothing else. That is the whole point. Drawing the cursor into the
        framebuffer means handing the driver a damage rectangle, and on an
        atomic driver that is a full commit that waits for vblank -- a cursor
        move cannot land in less than a frame however little is drawn.

        The path out is one the kernel already has. drm_atomic_helper_update_plane
        sets legacy_cursor_update on the state whenever the plane being updated
        is the crtc's cursor, and the commit helper then completes the flip
        immediately rather than waiting: "Legacy cursor updates are fully
        unsynced". So going through plane->funcs->update_plane on crtc->cursor
        is both the hardware plane and the unsynced commit, without asking for
        either by name.
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
        Places the cursor plane at a position, creating no damage and drawing
        nothing.

        The lock dance is the one drm_mode_cursor_common does: take the crtc
        and the plane, and back off and retry the whole thing on -EDEADLK. The
        retry has to cover update_plane as well as the two locks above it,
        because it takes more locks of its own on the way to the commit.

        Uninterruptible, unlike the ioctl. That path is a syscall and wants to
        return to a signal handler; this one is a worker with no signals to
        take, where -ERESTARTSYS would only mean a dropped mouse move.
*/
static int canvas_place_cursor_plane(struct surface *surface, int x, int y)
{
        struct drm_plane *plane = surface->cursor_plane;
        struct drm_crtc *crtc = surface->mode_set->crtc;
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

        ret = plane->funcs->update_plane(plane, crtc, surface->cursor_buffer->fb,
                                         x - CURSOR_HOTSPOT_X,
                                         y - CURSOR_HOTSPOT_Y,
                                         CURSOR_PLANE_W, CURSOR_PLANE_H,
                                         0, 0,
                                         CURSOR_PLANE_W << 16,
                                         CURSOR_PLANE_H << 16,
                                         &ctx);
out:
        if (ret == -EDEADLK) {
                drm_modeset_backoff(&ctx);
                goto retry;
        }

        drm_modeset_drop_locks(&ctx);
        drm_modeset_acquire_fini(&ctx);

        return ret;
}

static void canvas_drop_cursor_plane(struct surface *surface)
{
        surface->cursor_plane = NULL;

        if (surface->cursor_buffer) {
                drm_client_buffer_delete(surface->cursor_buffer);
                surface->cursor_buffer = NULL;
        }
}

/*
        Claims the crtc's cursor plane and paints the arrow into it once.

        The image is written before the plane is ever armed, because a driver
        that keeps its framebuffer somewhere else only uploads it when the
        plane's framebuffer changes -- virtio-gpu transfers the image on that
        edge and sends nothing but a position afterwards, which is exactly the
        behaviour that makes this cheap. An image drawn after arming would
        never be sent.
*/
static int canvas_setup_cursor_plane(struct drm_client_dev *client,
                                           struct surface *surface)
{
        struct drm_plane *plane = surface->mode_set->crtc->cursor;
        struct iosys_map map;
        unsigned int pitch_pixels;

        if (!plane || !canvas_plane_takes_argb(plane))
                return -ENODEV;

        surface->cursor_buffer = drm_client_buffer_create_dumb(
            client, CURSOR_PLANE_W, CURSOR_PLANE_H, DRM_FORMAT_ARGB8888);
        if (IS_ERR(surface->cursor_buffer)) {
                surface->cursor_buffer = NULL;
                return -ENOMEM;
        }

        if (drm_client_buffer_vmap_local(surface->cursor_buffer, &map)) {
                drm_client_buffer_delete(surface->cursor_buffer);
                surface->cursor_buffer = NULL;
                return -EIO;
        }

        pitch_pixels = surface->cursor_buffer->fb->pitches[0] / sizeof(u32);

        // Transparent everywhere the arrow does not cover. draw_cursor skips
        // the blank cells of its bitmap, so without this the buffer keeps
        // whatever it was allocated holding and the arrow wears a black box.
        canvas_fill_rect(map.vaddr, pitch_pixels,
                               CURSOR_PLANE_W, CURSOR_PLANE_H,
                               0, 0, CURSOR_PLANE_W, CURSOR_PLANE_H,
                               0x00000000);

        canvas_draw_cursor(map.vaddr, pitch_pixels,
                                 CURSOR_PLANE_W, CURSOR_PLANE_H, 0, 0,
                                 canvas_colour(COLOUR_CURSOR, DRM_FORMAT_ARGB8888),
                                 canvas_colour(COLOUR_CURSOR_EDGE, DRM_FORMAT_ARGB8888));

        drm_client_buffer_vunmap_local(surface->cursor_buffer);

        surface->cursor_plane = plane;
        return 0;
}

/*
        Puts the cursor plane back after a modeset.

        Not optional and not a formality: drm_client_modeset_commit disables
        every non-primary plane on the device before it commits, so that a
        client inheriting the display does not also inherit an overlay left
        behind by the last one. Our cursor is one of those planes. Every
        commit turns it off, and this is what turns it back on.
*/
static void canvas_arm_cursor(struct canvas *canvas)
{
        struct surface *surface;
        int ret;

        /*
                cursor_x is the position to arm at, and it is written by the
                worker just after it moves the plane. Both run under
                canvas->lock, so this cannot read a position the plane has
                already moved past -- a second commit site that did not hold
                the lock would make the pointer snap back on every redraw.
        */

        if (!canvas->surface_count)
                return;

        surface = &canvas->surfaces[0];
        if (!surface->cursor_plane)
                return;

        ret = canvas_place_cursor_plane(surface, canvas->cursor_x, canvas->cursor_y);
        if (!ret)
                return;

        // Give it up rather than leave a cursor that cannot move. The next
        // pointer event repaints through the software path, which needs
        // nothing from the display beyond a framebuffer.
        log_canvas("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
        canvas_drop_cursor_plane(surface);
}

/*
        Moves the cursor without repainting the screen.

        Two damage rectangles -- the old position and the new -- are recomposed
        and the cursor is drawn at the new one. drm_client_buffer_flush tells
        the driver which region changed, so a device that uploads its
        framebuffer only sends those bytes.
*/
static void canvas_move_cursor(struct canvas *canvas,
                                     struct surface *surface,
                                     int new_x, int new_y)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        struct drm_rect damage;
        int old_x = canvas->drawn_x;
        int old_y = canvas->drawn_y;

        /*
                On a display with a cursor plane none of the rest of this
                happens. No pixels are read, none are written, and no damage
                is handed to the driver -- the commit carries two coordinates
                and returns without waiting for vblank.
        */
        if (surface->cursor_plane) {
                u64 flush_started = ktime_get_ns();
                int ret = canvas_place_cursor_plane(surface, new_x, new_y);

                pointer_flush_total += ktime_get_ns() - flush_started;

                if (!ret)
                        return;

                log_canvas("cursor plane refused an update (%d), drawing the cursor instead\n", ret);
                canvas_drop_cursor_plane(surface);
        }

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        {
        u64 draw_started = ktime_get_ns();

        canvas_compose_rect(canvas, surface, pixels, pitch_pixels,
                                  old_x, old_y, CURSOR_W, CURSOR_H);
        canvas_compose_rect(canvas, surface, pixels, pitch_pixels,
                                  new_x, new_y, CURSOR_W, CURSOR_H);

        canvas_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                 new_x, new_y,
                                 canvas_colour(COLOUR_CURSOR, surface->format),
                                 canvas_colour(COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);

        pointer_draw_total += ktime_get_ns() - draw_started;
        }

        // Clamped: the cursor is allowed to sit against the right or bottom
        // edge, so the rectangle around it would otherwise reach past the
        // surface and be handed to the driver that way.
        damage.x1 = max(min(old_x, new_x), 0);
        damage.y1 = max(min(old_y, new_y), 0);
        damage.x2 = min(max(old_x, new_x) + CURSOR_W, (int)surface->width);
        damage.y2 = min(max(old_y, new_y) + CURSOR_H, (int)surface->height);

        if (damage.x2 <= damage.x1 || damage.y2 <= damage.y1)
                return;

        {
        u64 flush_started = ktime_get_ns();

        /*
                Required, not optional. Skipping it was tried: the cursor's
                position updated internally but the screen kept showing it
                where it was, because this driver shadows the framebuffer
                rather than scanning out what we wrote to.

                It is also the whole cost. The driver implements dirty as
                drm_atomic_helper_dirtyfb, a full atomic commit that waits for
                vblank, so a cursor move cannot land in less than a frame. A
                hardware cursor plane is what avoids this -- moving one does
                not touch the framebuffer at all -- but this device does not
                have one.
        */
        drm_client_buffer_flush(surface->buffer, &damage);

        pointer_flush_total += ktime_get_ns() - flush_started;
        }
}
