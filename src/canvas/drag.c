/*
        Canvas -- moving and resizing

        Both are the same operation underneath: give a window a new rectangle
        and repaint the desktop where it was and where it is, on every output
        either rectangle touches. Because panes are in desktop coordinates,
        crossing to another screen is not a case.

        A window with no pixels behind it can be any size the desktop allows,
        and its text rewraps to whatever that turns out to be. One with a
        buffer cannot outgrow the buffer its program asked for.
*/

static unsigned int pane_edges_at(struct pane *pane, int x, int y)
{
        unsigned int edges = 0;
        int fx, fy, fw, fh;

        if (!(pane->style & WINDOW_FRAME) || (pane->style & WINDOW_MINIMIZED))
                return 0;

        pane_frame(pane, &fx, &fy, &fw, &fh);

        if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
                return 0;

        if (x < fx + EDGE_GRIP)
                edges |= EDGE_LEFT;
        else if (x >= fx + fw - EDGE_GRIP)
                edges |= EDGE_RIGHT;

        if (y < fy + EDGE_GRIP)
                edges |= EDGE_TOP;
        else if (y >= fy + fh - EDGE_GRIP)
                edges |= EDGE_BOTTOM;

        return edges;
}

// Front to back is the reverse of the drawing order.
static struct pane *pane_under(int x, int y, unsigned int *edges)
{
        struct pane *pane, *found = NULL;
        unsigned int found_edges = 0;

        list_for_each_entry(pane, &desktop.windows, link)
        {
                unsigned int e = pane_edges_at(pane, x, y);

                if (e)
                {
                        found = pane;
                        found_edges = e;
                }
                else if (pane_titlebar_holds(pane, x, y) &&
                         (pane->style & WINDOW_FRAME) &&
                         !(pane->style & WINDOW_MINIMIZED))
                {
                        found = pane;
                        found_edges = 0;
                }
        }

        *edges = found_edges;
        return found;
}

static unsigned int cursor_for_edges(unsigned int edges)
{
        switch (edges)
        {
        case EDGE_LEFT:
        case EDGE_RIGHT:
                return CURSOR_RESIZE_H;
        case EDGE_TOP:
        case EDGE_BOTTOM:
                return CURSOR_RESIZE_V;
        case EDGE_TOP | EDGE_LEFT:
        case EDGE_BOTTOM | EDGE_RIGHT:
                return CURSOR_RESIZE_NWSE;
        case EDGE_TOP | EDGE_RIGHT:
        case EDGE_BOTTOM | EDGE_LEFT:
                return CURSOR_RESIZE_NESW;
        default:
                return CURSOR_ARROW;
        }
}

static unsigned int cursor_shape_at(int x, int y)
{
        unsigned int edges;

        if (desktop.resizing)
                return cursor_for_edges(desktop.resize_edges);

        if (desktop.dragging)
                return CURSOR_ARROW;

        pane_under(x, y, &edges);
        return cursor_for_edges(edges);
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

        output->cursor_shown =
            !output->cursor_plane &&
            output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);

        if (output->cursor_shown)
                canvas_draw_cursor(pixels, pitch_pixels, output->width, output->height,
                                   desktop.cursor_x - output->x,
                                   desktop.cursor_y - output->y,
                                   desktop.cursor_shape,
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

/*
        The one way a window's rectangle changes. Everything else -- a drag, a
        resize, a region -- decides what the new rectangle is and comes here.
*/
static void pane_reshape(struct pane *pane, int x, int y, int w, int h)
{
        struct output *output;
        int ax, ay, aw, ah, bx, by, bw, bh;

        pane_frame(pane, &ax, &ay, &aw, &ah);

        pane->x = x;
        pane->y = y;
        pane->width = w;
        pane->height = h;

        pane_frame(pane, &bx, &by, &bw, &bh);

        if (pane->shared)
        {
                WRITE_ONCE(pane->shared->x, pane->x);
                WRITE_ONCE(pane->shared->y, pane->y);
                WRITE_ONCE(pane->shared->width, (unsigned int)pane->width);
                WRITE_ONCE(pane->shared->height, (unsigned int)pane->height);
        }

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
        desktop.drawn_shape = desktop.cursor_shape;
}

static void drag_move(int x, int y)
{
        struct pane *pane = desktop.dragging;

        pane_reshape(pane, x - desktop.grab_x, y - desktop.grab_y,
                     pane->width, pane->height);
}

// A window with a buffer cannot outgrow it; one without can fill the desktop.
static void pane_limits(struct pane *pane, int *max_w, int *max_h)
{
        if (pane->pixels)
        {
                *max_w = (int)pane->max_width;
                *max_h = (int)pane->max_height;
                return;
        }

        *max_w = desktop.width;
        *max_h = desktop.height;
}

static void resize_move(int x, int y)
{
        struct pane *pane = desktop.resizing;
        unsigned int edges = desktop.resize_edges;
        int dx = x - desktop.press_x;
        int dy = y - desktop.press_y;
        int nx = desktop.resize_x, ny = desktop.resize_y;
        int nw = desktop.resize_w, nh = desktop.resize_h;
        int max_w, max_h;

        pane_limits(pane, &max_w, &max_h);

        if (edges & EDGE_LEFT)
                nw = desktop.resize_w - dx;
        else if (edges & EDGE_RIGHT)
                nw = desktop.resize_w + dx;

        if (edges & EDGE_TOP)
                nh = desktop.resize_h - dy;
        else if (edges & EDGE_BOTTOM)
                nh = desktop.resize_h + dy;

        nw = clamp(nw, WINDOW_MIN_WIDTH, max_w);
        nh = clamp(nh, WINDOW_MIN_HEIGHT, max_h);

        // The edge that was not grabbed stays where it was.
        if (edges & EDGE_LEFT)
                nx = desktop.resize_x + desktop.resize_w - nw;

        if (edges & EDGE_TOP)
                ny = desktop.resize_y + desktop.resize_h - nh;

        pane_reshape(pane, nx, ny, nw, nh);
}

static void drag_press(int x, int y)
{
        unsigned int edges;
        struct pane *pane = pane_under(x, y, &edges);

        if (!pane)
                return;

        pane_focus(pane);
        pane_raise(pane);
        list_sort(NULL, &desktop.windows, pane_by_z);

        // Taking hold of a window is what free floating means, so a window the
        // compositor placed is handed back to its program.
        pane->region = WINDOW_FREE;
        if (pane->shared)
                WRITE_ONCE(pane->shared->region, WINDOW_FREE);

        desktop.press_x = x;
        desktop.press_y = y;

        if (edges)
        {
                desktop.resizing = pane;
                desktop.resize_edges = edges;
                desktop.resize_x = pane->x;
                desktop.resize_y = pane->y;
                desktop.resize_w = pane->width;
                desktop.resize_h = pane->height;
        }
        else
        {
                desktop.dragging = pane;
                desktop.grab_x = x - pane->x;
                desktop.grab_y = y - pane->y;
        }

        desktop_redraw();
}

static void drag_release(void)
{
        desktop.dragging = NULL;
        desktop.resizing = NULL;
}
