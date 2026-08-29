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
        int max_w, max_h, min_w, min_h;

        pane_limits(pane, &max_w, &max_h);
        min_w = min(WINDOW_MIN_WIDTH, max_w);
        min_h = min(WINDOW_MIN_HEIGHT, max_h);

        if (edges & EDGE_LEFT)
                nw = desktop.resize_w - dx;
        else if (edges & EDGE_RIGHT)
                nw = desktop.resize_w + dx;

        if (edges & EDGE_TOP)
                nh = desktop.resize_h - dy;
        else if (edges & EDGE_BOTTOM)
                nh = desktop.resize_h + dy;

        nw = clamp(nw, min_w, max_w);
        nh = clamp(nh, min_h, max_h);

        // The edge that was not grabbed stays where it was.
        if (edges & EDGE_LEFT)
                nx = desktop.resize_x + desktop.resize_w - nw;

        if (edges & EDGE_TOP)
                ny = desktop.resize_y + desktop.resize_h - nh;

        pane_reshape(pane, nx, ny, nw, nh);
}

static _Bool pane_bar_holds(struct pane *pane, int x, int y)
{
        int bx, by, bw, bh, at, span;

        if (!pane->cells || !pane_bar(pane, &bx, &by, &bw, &bh, &at, &span))
                return false;

        return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/*
        The bar under the hand, in rows.

        The thumb is a picture of how much of the whole is showing, so where it
        sits is where the view is: the top of the thumb over the height of the
        bar is the same fraction as the rows above the view over all of them.
        Grabbed anywhere inside the thumb it stays under the finger, and
        pressed outside it the thumb comes to the finger's middle.
*/
static void bar_move(int y)
{
        struct pane *pane = desktop.barring;
        int bx, by, bw, bh, at, span, top;
        unsigned int first, shown, total, above;

        if (!pane || !pane_bar(pane, &bx, &by, &bw, &bh, &at, &span))
                return;

        if (!pane_extent(pane, &first, &shown, &total))
                return;

        top = clamp(y - desktop.bar_grab - by, 0, max(bh - span, 0));
        above = (unsigned int)((unsigned long)top * total / (unsigned long)max(bh, 1));

        if (!pane_view_set(pane, above))
                return;

        atomic_set(&desktop.frame_pending, 1);
        canvas_thread_wake();
}

/*
        Filling the screen, and going back.

        The rectangle it had is kept on the window itself, so a second pair of
        clicks puts it back where it was rather than somewhere a rule decided.
*/
static void pane_maximize(struct pane *pane, int at_x, int at_y)
{
        struct output *output = NULL;
        struct output *candidate;
        int title = pane->style & WINDOW_FRAME ? canvas_title : 0;
        int border = pane->style & WINDOW_FRAME ? canvas_border : 0;
        int max_w, max_h, width, height;
        unsigned int display = pane->display, index = 0;

        if (pane->maximized)
        {
                pane->maximized = false;
                pane->display = pane->saved_display;
                if (pane->shared)
                        WRITE_ONCE(pane->shared->display, pane->display);
                pane_reshape(pane, pane->saved_x, pane->saved_y,
                             pane->saved_w, pane->saved_h);
                return;
        }

        pane->saved_x = pane->x;
        pane->saved_y = pane->y;
        pane->saved_w = pane->width;
        pane->saved_h = pane->height;
        pane->saved_display = pane->display;

        // The click is on an output because the pointer is confined to one.
        // Maximize on that output, not across the bounding box of every
        // monitor. Keep the index in sync so fullscreen requests agree later.
        list_for_each_entry(candidate, &desktop.outputs, link)
        {
                if (output_holds(candidate, at_x, at_y))
                {
                        output = candidate;
                        display = index;
                        break;
                }

                index++;
        }

        if (!output)
                output = output_by_index(pane->display);

        if (!output)
                return;

        pane->maximized = true;
        pane->display = display;
        if (pane->shared)
                WRITE_ONCE(pane->shared->display, display);

        pane_limits(pane, &max_w, &max_h);
        width = min((int)output->width - border * 2, max_w);
        height = min((int)output->height - title - border * 3, max_h);

        pane_reshape(pane, output->x + border, output->y + border, width, height);
}

/*
        Taking a maximized titlebar restores the saved window under the hand.

        Leaving it maximized while ordinary drag_move changed x and y produced
        a full-screen-sized loose window, and the next double-click restored
        geometry from before the maximize. Keep the horizontal fraction that
        was grabbed, so taking the right side does not make the restored
        window jump until its left side is under the pointer.
*/
static void pane_restore_for_drag(struct pane *pane, int x, int y)
{
        int max_w, max_h;
        int old_w = max(pane->width, 1);
        int title_at = clamp(y - pane->y, 0, max(canvas_title - 1, 0));
        int width, height, grab;

        pane_limits(pane, &max_w, &max_h);
        width = clamp(pane->saved_w, min(WINDOW_MIN_WIDTH, max_w), max_w);
        height = clamp(pane->saved_h, min(WINDOW_MIN_HEIGHT, max_h), max_h);
        grab = (int)((long)(x - pane->x) * width / old_w);

        pane->maximized = false;
        pane_reshape(pane, x - grab, y - title_at, width, height);
}

// Two clicks are a pair when the second lands on the same window soon enough
// after the first. A quarter of a second is what a hand does without meaning
// to say two separate things.
#define PRESS_AGAIN_NS 400000000ull

static void drag_press(int x, int y)
{
        unsigned int edges;
        struct pane *pane = pane_under(x, y, &edges);
        struct pane *was = desktop.focused;
        int fx, fy, fw, fh;

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

        {
                u64 now = ktime_get_ns();
                _Bool again = pane == desktop.press_pane &&
                              now - desktop.press_ns <= PRESS_AGAIN_NS;

                desktop.press_pane = pane;
                desktop.press_ns = now;

                if (again && (pane->style & WINDOW_FRAME) &&
                    pane_titlebar_holds(pane, x, y))
                {
                        // Cleared, so a third click is a first one again
                        // rather than the window flickering under a hand that
                        // is still clicking.
                        desktop.press_pane = NULL;
                        pane_maximize(pane, x, y);
                        return;
                }
        }

        /*
                An edge first, and the bar after it.

                The grip reaches six pixels in from the frame and the bar is
                ten wide, so the two overlap by four and one of them has to
                give. The edge wins: a window that cannot be resized from its
                own corner is worse than a bar that has to be grabbed a few
                pixels further in, and there is bar left over on the inside to
                grab. That is why the bar is ten and not six.
        */
        if (!edges && pane_bar_holds(pane, x, y))
        {
                int bx, by, bw, bh, at, span;

                pane_bar(pane, &bx, &by, &bw, &bh, &at, &span);

                desktop.bar_grab = y >= by + at && y < by + at + span
                                       ? y - (by + at)
                                       : span / 2;
                desktop.barring = pane;
                bar_move(y);
        }
        else if (edges)
        {
                // An edge resize makes this ordinary geometry; a later
                // double-click must maximize it, not restore stale pre-max
                // coordinates.
                pane->maximized = false;
                desktop.resizing = pane;
                desktop.resize_edges = edges;
                desktop.resize_x = pane->x;
                desktop.resize_y = pane->y;
                desktop.resize_w = pane->width;
                desktop.resize_h = pane->height;
        }
        else if ((pane->style & WINDOW_FRAME) && pane_titlebar_holds(pane, x, y))
        {
                if (pane->maximized)
                        pane_restore_for_drag(pane, x, y);

                desktop.dragging = pane;
                desktop.grab_x = x - pane->x;
                desktop.grab_y = y - pane->y;
        }

        // The titlebar that lost focus and the window that came to the front.
        desktop.damage_count = 0;
        desktop.damage_all = false;

        if (was && was != pane)
        {
                pane_frame(was, &fx, &fy, &fw, &fh);
                desktop_damage(fx, fy, fw, fh);
        }

        pane_frame(pane, &fx, &fy, &fw, &fh);
        desktop_damage(fx, fy, fw, fh);
        desktop_repaint();
}

static void drag_release(void)
{
        desktop.dragging = NULL;
        desktop.resizing = NULL;
        desktop.barring = NULL;
}

/*
        The wheel goes to whatever is under the pointer.

        Under, not focused: a wheel is aimed with the hand rather than chosen,
        and every desktop since the wheel existed has read it that way. Nothing
        takes focus for it either, so reading one window while typing into
        another works the way it looks like it should.

        Every window of cells answers, its own or a program's, because the
        lines a program wrote are in a ring the compositor allocated and the
        view onto that ring is the compositor's. The turn never reaches the
        program: there is nothing for it to do about one.
*/
/*
        Linux calls one legacy REL_WHEEL unit a physical detent and calls 120
        REL_WHEEL_HI_RES units the same distance. Three text lines per detent
        is the conventional desktop step.

        Do not accelerate here. The old curve made four ordinary notches move
        3 + 6 + 9 + 12 = 30 lines, so a wheel became ten times faster merely
        by being used continuously. Multiple input events are already
        coalesced in desktop.wheel; this conversion preserves their distance.
        A high-resolution wheel keeps the fraction until it amounts to a line.
*/
#define WHEEL_LINES 3
#define WHEEL_V120 120

static int wheel_lines(int v120, int *remainder)
{
        long scaled = (long)v120 * WHEEL_LINES + *remainder;
        int lines = (int)(scaled / WHEEL_V120);

        *remainder = (int)(scaled % WHEEL_V120);
        return lines;
}

static void wheel_deliver(void)
{
        int v120 = atomic_xchg(&desktop.wheel, 0);
        int lines;
        unsigned int edges;
        struct pane *pane;

        if (!v120)
                return;

        pane = pane_under(desktop.cursor_x, desktop.cursor_y, &edges);

        if (!pane)
                return;

        lines = wheel_lines(v120, &desktop.wheel_remainder);
        if (!lines)
                return;

        // The same way a console write asks for a frame. Damaging and
        // repainting from here draws before the cells are looked at again,
        // and the view lands a frame later or not at all.
        if (pane_scroll(pane, lines))
        {
                atomic_set(&desktop.frame_pending, 1);
                canvas_thread_wake();
        }
}
