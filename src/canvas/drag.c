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

/*
        The window the pointer is over, and which of its edges it has hold of.

        A window blocks over all of itself, not only over the parts that
        answer: its middle is not a hole through to whatever is behind. The
        list is walked back to front and the last match wins, which is the
        topmost, and a window can opt out with WINDOW_PASSTHROUGH.
*/
static struct pane *pane_under(int x, int y, unsigned int *edges)
{
        struct pane *pane, *found = NULL;
        unsigned int found_edges = 0;

        list_for_each_entry(pane, &desktop.windows, link)
        {
                int fx, fy, fw, fh;

                if (pane->style & (WINDOW_MINIMIZED | WINDOW_PASSTHROUGH))
                        continue;

                pane_frame(pane, &fx, &fy, &fw, &fh);

                if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
                        continue;

                found = pane;
                found_edges = pane_edges_at(pane, x, y);
        }

        *edges = found_edges;
        return found;
}

/*
        Which shape an edge mask wears. Sixteen entries because the mask is
        four bits; the impossible ones -- left and right at once -- read as an
        arrow, which is what a mask of no edges means too.
*/
static const unsigned char cursor_by_edges[16] = {
    [EDGE_LEFT] = CURSOR_RESIZE_H,
    [EDGE_RIGHT] = CURSOR_RESIZE_H,
    [EDGE_TOP] = CURSOR_RESIZE_V,
    [EDGE_BOTTOM] = CURSOR_RESIZE_V,
    [EDGE_TOP | EDGE_LEFT] = CURSOR_RESIZE_NWSE,
    [EDGE_BOTTOM | EDGE_RIGHT] = CURSOR_RESIZE_NWSE,
    [EDGE_TOP | EDGE_RIGHT] = CURSOR_RESIZE_NESW,
    [EDGE_BOTTOM | EDGE_LEFT] = CURSOR_RESIZE_NESW,
};

static unsigned int cursor_for_edges(unsigned int edges)
{
        return cursor_by_edges[edges & 15];
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

/*
        The one way a window's rectangle changes. Everything else -- a drag, a
        resize, a region -- decides what the new rectangle is and comes here.
*/
static void pane_reshape(struct pane *pane, int x, int y, int w, int h)
{
        struct drm_rect damage[4];
        struct output *output;
        int fx, fy, fw, fh;

        pane_frame(pane, &fx, &fy, &fw, &fh);
        rect_set(&damage[0], fx, fy, fw, fh);

        pane->x = x;
        pane->y = y;
        pane->width = w;
        pane->height = h;

        pane_regrid(pane);
        pane_frame(pane, &fx, &fy, &fw, &fh);
        rect_set(&damage[1], fx, fy, fw, fh);

        // The cursor is dragging this, so where it was and where it is are
        // damaged too, and its cell reaches outside the frame.
        cursor_cell(&damage[2], desktop.drawn_x, desktop.drawn_y,
                    desktop.drawn_shape, desktop.drawn_scale);
        cursor_cell(&damage[3], desktop.cursor_x, desktop.cursor_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        if (pane->shared)
        {
                WRITE_ONCE(pane->shared->x, pane->x);
                WRITE_ONCE(pane->shared->y, pane->y);
                WRITE_ONCE(pane->shared->width, (unsigned int)pane->width);
                WRITE_ONCE(pane->shared->height, (unsigned int)pane->height);
        }

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (!output_touched(output, damage, 4))
                        continue;

                if (output->cursor_plane)
                        cursor_arm_output(output);

                output_repaint(output, damage, 4);
        }

        desktop.drawn_x = desktop.cursor_x;
        desktop.drawn_y = desktop.cursor_y;
        desktop.drawn_shape = desktop.cursor_shape;
        desktop.drawn_scale = desktop.cursor_scale;
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
        else if ((pane->style & WINDOW_FRAME) && pane_titlebar_holds(pane, x, y))
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
