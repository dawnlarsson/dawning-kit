/*
        Canvas -- dragging a window

        A pane is moved by repainting the desktop where it was and where it
        is, on every output either rectangle touches. Because panes are in
        desktop coordinates that is the same code whether the window stays on
        one screen or crosses to the next.
*/

static struct pane *pane_at(int x, int y)
{
        struct pane *pane, *found = NULL;

        // Front to back is the reverse of the drawing order.
        list_for_each_entry(pane, &desktop.windows, link)
                if (pane_titlebar_holds(pane, x, y))
                        found = pane;

        return found;
}

static void output_repaint(struct output *output,
                           int ax, int ay, int aw, int ah,
                           int bx, int by, int bw, int bh)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        struct drm_rect damage;

        if (drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);

        compose_rect(output, pixels, pitch_pixels, ax, ay, aw, ah);
        compose_rect(output, pixels, pitch_pixels, bx, by, bw, bh);

        output->cursor_shown = !output->cursor_plane &&
                               output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);

        if (output->cursor_shown)
                canvas_draw_cursor(pixels, pitch_pixels, output->width, output->height,
                                   desktop.cursor_x - output->x,
                                   desktop.cursor_y - output->y,
                                   canvas_colour(COLOUR_CURSOR, output->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, output->format));

        drm_client_buffer_vunmap_local(output->buffer);

        damage.x1 = max(min(ax, bx) - output->x, 0);
        damage.y1 = max(min(ay, by) - output->y, 0);
        damage.x2 = min(max(ax + aw, bx + bw) - output->x, (int)output->width);
        damage.y2 = min(max(ay + ah, by + bh) - output->y, (int)output->height);

        if (damage.x2 > damage.x1 && damage.y2 > damage.y1)
                drm_client_buffer_flush(output->buffer, &damage);
}

static void drag_move(int x, int y)
{
        struct pane *pane = desktop.dragging;
        struct output *output;
        int ax, ay, aw, ah, bx, by, bw, bh;

        pane_frame(pane, &ax, &ay, &aw, &ah);

        pane->x = x - desktop.grab_x;
        pane->y = y - desktop.grab_y;

        // The program sees where its window ended up without asking.
        if (pane->shared)
        {
                WRITE_ONCE(pane->shared->x, pane->x);
                WRITE_ONCE(pane->shared->y, pane->y);
        }

        pane_frame(pane, &bx, &by, &bw, &bh);

        list_for_each_entry(output, &desktop.outputs, link)
        {
                _Bool touched =
                    rects_overlap(ax, ay, aw, ah, output->x, output->y,
                                  (int)output->width, (int)output->height) ||
                    rects_overlap(bx, by, bw, bh, output->x, output->y,
                                  (int)output->width, (int)output->height);

                if (!touched)
                        continue;

                if (output->cursor_plane)
                        cursor_arm_output(output);

                output_repaint(output, ax, ay, aw, ah, bx, by, bw, bh);
        }

        desktop.drawn_x = desktop.cursor_x;
        desktop.drawn_y = desktop.cursor_y;
}

static void drag_press(int x, int y)
{
        struct pane *pane = pane_at(x, y);

        if (!pane)
                return;

        desktop.dragging = pane;
        desktop.grab_x = x - pane->x;
        desktop.grab_y = y - pane->y;

        // Raise it. Drawing is back to front, so the tail is the front.
        list_move_tail(&pane->link, &desktop.windows);

        desktop_redraw();
}

static void drag_release(void)
{
        desktop.dragging = NULL;
}
