/*
        Canvas -- windows

        One file per window. The ioctl sizes it, the mapping is at offset zero,
        and closing the file destroys it, so there is no identifier for either
        side to get wrong and no lifetime to track separately from the fd.

        The shared page is writable by the program at any instant, so nothing
        below composes from it. pane_refresh takes a copy and checks it; the
        buffer's extent is the compositor's and is never read back from the
        page at all.

        A window of cells is a ring of lines. The compositor allocates it and
        both sides address it the same way, so the scrollback, the wheel and
        the bar down the side are here once rather than once per window that
        wanted them.
*/

/*
        How many lines a window of cells keeps.

        Enough to hold a boot, and never fewer than a couple of screens of
        whatever the display can show, because the arithmetic below counts
        backwards from the newest line and must not run past the oldest.
*/
#define PANE_HISTORY 512

// The view follows the end rather than sitting on a line.
#define PANE_LIVE ((unsigned int)-1)

static PURE struct output *output_by_index(unsigned int index)
{
        struct output *output;
        unsigned int i = 0;

        list_for_each_entry(output, &desktop.outputs, link)
                if (i++ == index)
                        return output;

        return list_first_entry_or_null(&desktop.outputs, struct output, link);
}

/*
        The first step down the cascade that no other window is sitting on.

        Bounded, and not only because an unbounded search would be a loop over
        the window list per step: eight titlebars down is already most of the
        way across a small screen, and past that the honest answer is to start
        again at the centre and let them overlap. A step that would put the
        window off the bottom or the right of its own output stops the search
        for the same reason -- a window placed where it cannot be seen is
        worse than one placed on top of another.
*/
static PURE int pane_cascade_free(struct pane *pane, struct output *output,
                                  int title)
{
        int step = canvas_title;
        int at;

        for (at = 0; at < 8; at++)
        {
                struct pane *other;
                int x = pane->x + at * step;
                int y = pane->y + at * step;
                _Bool taken = false;

                if (x + pane->width > output->x + (int)output->width ||
                    y + pane->height + title > output->y + (int)output->height)
                        break;

                list_for_each_entry(other, &desktop.windows, link)
                        if (other != pane && other->x == x && other->y == y)
                        {
                                taken = true;
                                break;
                        }

                if (!taken)
                        return at;
        }

        return 0;
}

// Where a region or a style puts a window. Free floating and not fullscreen
// is the program's own x and y.
static void pane_place(struct pane *pane)
{
        struct output *output = output_by_index(pane->display);
        int title;

        if (!output)
                return;

        if (pane->shared)
        {
                WRITE_ONCE(pane->shared->display_width, output->width);
                WRITE_ONCE(pane->shared->display_height, output->height);
        }

        title = pane_title(pane);

        if (pane->style & WINDOW_FULLSCREEN)
        {
                pane->x = output->x;
                pane->y = output->y;
                return;
        }

        if (pane->region != WINDOW_CENTRED)
                return;

        pane->x = output->x + ((int)output->width - pane->width) / 2;
        pane->y = output->y + ((int)output->height - (pane->height + title)) / 2;

        /*
                Down and across, when the centre is already taken.

                Every terminal asks to be centred, so a second one landed on
                the first exactly -- same size, same place, pixel for pixel --
                and opening one looked like nothing had happened at all. The
                window was there, with another behind it.

                Worked out once, the first time this window is placed, and
                kept: it is the window's position in a stack, not a function
                of where the other windows are this instant, or closing the
                one underneath would slide this one up the screen.
        */
        if (!pane->cascaded)
        {
                pane->cascaded = true;
                pane->cascade = pane_cascade_free(pane, output, title);
        }

        pane->x += pane->cascade * canvas_title;
        pane->y += pane->cascade * canvas_title;
}

/*
        What a style says the size should be, before the grid rounds it.

        Fullscreen used to move a window to the corner of its output and leave
        it whatever size it was, which is not what the flag says. A window of
        cells grows to the whole screen; one with a buffer covers as much of it
        as the buffer it asked for will reach.
*/
static void pane_size(struct pane *pane)
{
        struct output *output = output_by_index(pane->display);
        int title;

        if (!output || !(pane->style & WINDOW_FULLSCREEN))
                return;

        title = pane_title(pane);

        pane->width = (int)min(output->width, pane->max_width);
        pane->height = (int)min(output->height - min(output->height, (unsigned int)title),
                                pane->max_height);
}

static void pane_raise(struct pane *raised)
{
        int top = 0;

        if (!list_empty(&desktop.windows))
                top = max(top, list_last_entry(&desktop.windows,
                                                struct pane, link)->z);

        raised->z = top + 1;

        if (raised->shared)
                WRITE_ONCE(raised->shared->z, raised->z);
}

static PURE int pane_by_z(void *unused, const struct list_head *a, const struct list_head *b)
{
        int za = list_entry(a, struct pane, link)->z;
        int zb = list_entry(b, struct pane, link)->z;

        return za < zb ? -1 : za > zb;
}

/*
        How much of the machine every window may hold between them.

        There is no cap on how many windows there are and there must not be,
        but /dev/spark is open to anything, so without a ceiling on the total
        one program can ask until there are no pages left. A quarter of memory,
        which no honest desktop comes near.
*/
static unsigned long canvas_pane_bytes;

static unsigned long canvas_pane_budget(void)
{
        return (totalram_pages() / 4) * PAGE_SIZE;
}

/*
        A pane going away, and everything that was still pointing at it.

        The desktop remembers a particular window between events -- what a
        hand has hold of, what it last pressed, where the keys go -- and every
        one of those outlives the window unless it is told otherwise. This was
        written at window_release's call site, which left the other caller
        freeing a pane the desktop could still be holding: console_stop runs
        on the way out of the module, and a console that had been clicked was
        freed with desktop.focused still naming it.

        Here rather than there, so that freeing a pane is the whole of
        forgetting one and a third caller cannot be written that forgets to.
        Who gets the keys instead is the caller's: window_release hands them
        to the window underneath, and nothing is left to hand them to when
        the module is going away.
*/
static void pane_free(struct pane *pane)
{
        struct pane **held[] = {
                &desktop.dragging, &desktop.resizing, &desktop.barring,
                &desktop.press_pane, &desktop.focused,
        };
        unsigned int i;

        for (i = 0; i < ARRAY_SIZE(held); i++)
                if (*held[i] == pane)
                        *held[i] = NULL;

        list_del(&pane->link);
        canvas_pane_bytes -= pane->bytes;
        vfree(pane->mapping);
        kfree(pane);
}

/*
        How many cells the desktop has room for, the frame counted.

        A window wears a titlebar and a border, so a grid measured against the
        bare screen is a window larger than the screen it has to fit on. At a
        scale of one the terminal never asked for enough cells to notice; at
        two it did, and came out taller than the display, centred to a negative
        y with its titlebar off the top of it.
*/
static void desktop_grid(int width, int height,
                          unsigned int *columns, unsigned int *rows)
{
        width -= canvas_border * 2 + canvas_bar;
        height -= canvas_title + canvas_border * 3;

        *columns = (unsigned int)(max(width, 0) / canvas_cell_w);
        *rows = (unsigned int)(max(height, 0) / canvas_cell_h);
}

/*
        The grid a window is BUILT for, which is not the grid the desktop
        happens to have when it is built.

        A window of cells keeps its lines in a ring whose stride is fixed for
        the window's life, because a program cannot be handed a larger
        mapping than the one it already mapped. Sizing that ring from the
        desktop of the moment therefore freezes the window's largest possible
        size at whatever the screen was when it opened -- and the screen is
        not a constant. desktop.width is the sum of the outputs, recomputed
        every time a connector is probed, a mode is chosen, or a monitor
        arrives, and on real hardware those happen after the first terminal
        already exists. The terminal that opened first then refuses to grow
        past a screen nobody has any more, which is a bug that comes and goes
        with the boot's timing and never appears under an emulator whose
        display is ready before anything asks.

        So the ring is built for a ceiling instead: a size no ordinary display
        exceeds, taken as the larger of that and the desktop as it stands, so
        a screen bigger than the ceiling is still served. The window's visible
        size is clamped to the output it sits on, exactly as before -- this
        changes only what it is ALLOWED to become, never what it is.

        The cost is the difference between a ring cut for the screen and one
        cut for the ceiling. At the eight by sixteen cell that is about two
        megabytes for a terminal against one, inside a budget that is a
        quarter of memory.
*/
#define PANE_CEILING_W 3840
#define PANE_CEILING_H 2160

/*
        How long a ring is, and how far apart its lines are.

        A line is as wide as the desktop could ever make this window, so the
        stride never changes and a resize moves nothing. The history is at
        least two screens of the tallest window there could be, because
        everything below counts backwards from the newest line.
*/
static void pane_ring(unsigned int max_columns, unsigned int max_rows,
                      unsigned int *stride, unsigned int *history,
                      unsigned long *bytes)
{
        *stride = max_columns;
        *history = max_t(unsigned int, PANE_HISTORY, max_rows * 2 + 1);
        *bytes = (unsigned long)*history *
                 ((unsigned long)*stride * sizeof(struct window_cell) +
                  sizeof(unsigned int));
}

// One line of the ring, and how much of it was written. Any index: it is
// taken modulo the history, which is what makes head a count and not a cursor.
static unsigned int pane_length(struct pane *pane, unsigned int index)
{
        return min(pane->lengths[index % pane->history], pane->stride);
}

/*
        How many rows are actually drawn.

        The room the window has now and the shape whoever owns the cells last
        laid them out in are the same at rest and not during a resize.
*/
static PURE unsigned int pane_rows(struct pane *pane)
{
        return max(min(pane->grid_rows, pane->rows), 1u);
}

/*
        The oldest line still held.

        Counting back from head rather than from a line number that was kept,
        so a ring that has wrapped stops at the oldest line it still has
        instead of scrolling into ones it gave away. Never fewer than the rows
        on the screen, so a window taller than what has been written to it
        still has the newest line at the bottom of it.
*/
static PURE unsigned int pane_oldest(struct pane *pane)
{
        unsigned int filled = clamp(pane->head - pane->history, pane_rows(pane),
                                    pane->history);

        return pane->head - filled;
}

/*
        How many rows a line takes at the width it is being drawn at.

        This is the whole of the wrapping. A line is stored as long as it was
        written and folded when it is drawn, so a window made wider re-wraps
        everything in it -- including what has already scrolled past -- without
        anything being moved or rewritten.
*/
static PURE unsigned int pane_line_rows(struct pane *pane, unsigned int index)
{
        unsigned int width = max(pane->grid_columns, 1u);
        unsigned int length = pane_length(pane, index);

        return length ? (length + width - 1) / width : 1;
}

/*
        The line the top row of the window is on, and how many rows of that
        line are above the window.

        Following the end means counting rows backwards from the newest line
        until the window is full, which can stop in the middle of a line that
        is too long to fit -- so the top row of the screen is a fold of one,
        the way the bottom of a terminal has always been.
*/
static unsigned int pane_view_at(struct pane *pane, unsigned int view,
                                 unsigned int *skip)
{
        unsigned int oldest = pane_oldest(pane);
        unsigned int rows = pane_rows(pane);
        unsigned int at = pane->head - 1;
        unsigned int count = 0;

        *skip = 0;

        if (view != PANE_LIVE)
        {
                at = clamp(view, oldest, pane->head - 1);
                *skip = min(pane->view_skip, pane_line_rows(pane, at) - 1);
                return at;
        }

        while (count < rows && at > oldest)
        {
                count += pane_line_rows(pane, at);

                if (count >= rows)
                        break;

                at--;
        }

        if (count < rows)
                count += pane_line_rows(pane, at);

        *skip = count > rows ? count - rows : 0;

        return at;
}

/*
        Where the view sits in what there is, for something to draw a bar with.

        In rows rather than pixels, and rows rather than lines: what the bar is
        made of is the drawing code's business, and a bar measured in lines is
        one that jumps whenever a line folds. False when there is nothing to
        scroll, which is also when a bar would say nothing worth the pixels.
*/
static _Bool pane_extent(struct pane *pane, unsigned int *first,
                         unsigned int *shown, unsigned int *total)
{
        unsigned int skip, top, at;

        if (!pane->cells)
                return false;

        top = pane_view_at(pane, pane->view, &skip);

        *shown = pane_rows(pane);
        *total = 0;
        *first = 0;

        for (at = pane_oldest(pane); at != pane->head; at++)
        {
                if (at == top)
                        *first = *total + skip;

                *total += pane_line_rows(pane, at);
        }

        return *total > *shown;
}

/*
        Put the view where a count of rows says, rather than a number of lines.

        The bar is measured in drawn rows, because that is what it is a picture
        of: a line too long for the window is several of them. So the walk is
        the same one pane_extent does, stopping at the line the row asked for.
*/
static _Bool pane_view_set(struct pane *pane, unsigned int above)
{
        unsigned int was = pane->view;
        unsigned int was_skip = pane->view_skip;
        unsigned int at, run = 0, live, live_skip, skip = 0;

        if (!pane->cells)
                return false;

        live = pane_view_at(pane, PANE_LIVE, &live_skip);

        for (at = pane_oldest(pane); at != pane->head; at++)
        {
                unsigned int past = run + pane_line_rows(pane, at);

                if (above < past)
                {
                        skip = above - run;
                        break;
                }

                run = past;
        }

        if (at > live || (at == live && skip >= live_skip))
        {
                pane->view = PANE_LIVE;
                pane->view_skip = 0;
        }
        else
        {
                pane->view = at;
                pane->view_skip = skip;
        }

        if (pane->view == was && pane->view_skip == was_skip)
                return false;

        pane->view_moved = true;

        return true;
}

/*
        The wheel, in lines. Positive is away from the hand, which is back
        through what has already been said.

        Back at the end is following again rather than sitting on the last
        line: what arrives next should appear.
*/
static _Bool pane_scroll(struct pane *pane, int lines)
{
        unsigned int above, shown, total, live;
        long target;

        if (!pane->cells)
                return false;

        pane_extent(pane, &above, &shown, &total);
        live = total > shown ? total - shown : 0;
        target = (long)above - lines;

        if (target < 0)
                target = 0;
        else if ((unsigned long)target > live)
                target = live;

        return pane_view_set(pane, (unsigned int)target);
}

/*
        Back to following the end, wherever the view had been left.

        Separate from pane_scroll because it is not a distance: a keystroke
        does not move the view by so many lines, it says the end is where the
        interesting part is again. Answers whether anything moved, so a window
        already at the end costs no frame.
*/
static _Bool pane_view_live(struct pane *pane)
{
        if (!pane->cells || pane->view == PANE_LIVE)
                return false;

        pane->view = PANE_LIVE;
        pane->view_skip = 0;
        pane->view_moved = true;

        return true;
}

/*
        The view is never later in the ring than following the end would be.

        pane_view_set refuses to place the view past the last screenful, so
        that there is always a full window of lines below it. What it cannot
        do is keep that true afterwards: the rule is about where the end is,
        and the end moves. A window made taller shows more rows, and a window
        made wider folds its lines into fewer -- both pull the top of the last
        screenful EARLIER in the ring, and a view left where it was is then
        past it. compose_cells draws what there is and fills the rest with
        blank rows, so a scrolled-back window dragged taller grew a band of
        empty rows under its last line and kept them until the wheel moved.

        The same test pane_view_set makes, made again against the size the
        window is now.
*/
static _Bool pane_view_clamp(struct pane *pane)
{
        unsigned int live, live_skip;

        if (!pane->cells || pane->view == PANE_LIVE)
                return false;

        live = pane_view_at(pane, PANE_LIVE, &live_skip);

        if (pane->view < live ||
            (pane->view == live && pane->view_skip < live_skip))
                return false;

        return pane_view_live(pane);
}

/*
        A window, of pixels or of cells.

        An owned one is the compositor's own: nothing maps it, so there is no
        shared page and none of what a program would have read from one is
        written. Everything that reads a page checks for one first, so an owned
        window is placed, moved and composed like any other.
*/
static COLD struct pane *pane_create(unsigned int width, unsigned int height,
                                     unsigned int columns, unsigned int rows,
                                     _Bool owned)
{
        unsigned int max_columns, max_rows;
        unsigned int fit_columns, fit_rows;
        unsigned int stride = 0, history = 0;
        unsigned long ring_bytes = 0;
        struct window *page;
        struct pane *pane;
        unsigned long bytes;

        // What it may be built for, and what there is room to show right now.
        desktop_grid(max(desktop.width, PANE_CEILING_W),
                     max(desktop.height, PANE_CEILING_H), &max_columns, &max_rows);
        desktop_grid(desktop.width, desktop.height, &fit_columns, &fit_rows);

        // A screen with no room for one cell has no room for a window of
        // cells, whatever the ring could hold.
        if (columns && (!fit_columns || !fit_rows))
                return NULL;

        /*
                A window of cells is allocated for as many as the desktop
                could hold, not for as many as it asked for, because its size
                is its grid: resizing it changes how many cells there are, and
                a program cannot be handed a larger mapping than the one it
                already has.
        */
        if (columns)
        {
                pane_ring(max_columns, max_rows, &stride, &history, &ring_bytes);

                // The ring is cut for the ceiling; what opens is what fits.
                columns = min(columns, fit_columns);
                rows = min(rows, fit_rows);
                width = columns * (unsigned int)canvas_cell_w + canvas_bar;
                height = rows * (unsigned int)canvas_cell_h;
        }

        // A window is allowed to be as large as the desktop and no larger.
        // Not the kind of ceiling the rest of this refuses: it is what stops
        // one program asking for every page in the machine.
        if (!width || !height ||
            width > (unsigned int)desktop.width || height > (unsigned int)desktop.height)
                return NULL;

        bytes = PAGE_ALIGN(WINDOW_PIXELS +
                           (columns ? ring_bytes : (unsigned long)width * height * 4));

        if (canvas_pane_bytes + bytes > canvas_pane_budget())
                return NULL;

        pane = kzalloc(sizeof(*pane), GFP_KERNEL);
        if (!pane)
                return NULL;

        init_waitqueue_head(&pane->wait);

        // Not vmalloc_user when it is the compositor's own: nothing maps it.
        pane->mapping = owned ? vzalloc(bytes) : vmalloc_user(bytes);
        if (!pane->mapping)
        {
                kfree(pane);
                return NULL;
        }

        canvas_pane_bytes += bytes;
        pane->bytes = bytes;
        pane->pitch = width;
        page = pane->mapping;

        // The page is written either way -- the compositor's own window reads
        // its geometry out of one too. What an owned window has not got is a
        // program behind it, and pane->shared is what says so.
        if (!owned)
                pane->shared = pane->mapping;

        if (columns)
        {
                unsigned long lines = WINDOW_PIXELS + (unsigned long)history *
                                                          stride *
                                                          sizeof(struct window_cell);

                pane->cells = pane->mapping + WINDOW_PIXELS;
                pane->lengths = pane->mapping + lines;
                pane->stride = stride;
                pane->history = history;
                pane->head = history + rows;
                pane->view = PANE_LIVE;
                pane->columns = columns;
                pane->rows = rows;
                pane->max_columns = max_columns;
                pane->max_rows = max_rows;
                pane->max_width = max_columns * (unsigned int)canvas_cell_w + canvas_bar;
                pane->max_height = max_rows * (unsigned int)canvas_cell_h;
                pane->grid_columns = columns;
                pane->grid_rows = rows;
                /*
                        Say what was allocated, not only the tallest visible
                        grid.  history is deliberately at least PANE_HISTORY,
                        so logging max_rows here understated each boot pane by
                        almost four times and hid about two MiB of live RAM.
                */
                pr_info("[moonwater canvas] " "window grid %ux%u, ring holds %ux%u (%lu KiB)\n", columns, rows, stride, history, bytes >> 10);

                page->max_columns = max_columns;
                page->max_rows = max_rows;
                page->columns = columns;
                page->rows = rows;
                page->grid_columns = columns;
                page->grid_rows = rows;
                page->stride = stride;
                page->history = history;
                page->head = pane->head;
                page->lines = (unsigned int)lines;
        }
        else
        {
                pane->pixels = pane->mapping + WINDOW_PIXELS;

                // A window of pixels can never outgrow the buffer it asked
                // for. A window of cells has room for the whole desktop, and
                // setting this from the requested size here is what clamped
                // every attempt to resize one back to the size it started at.
                pane->max_width = width;
                pane->max_height = height;
        }

        pane->width = (int)width;
        pane->height = (int)height;
        pane->x = 80;
        pane->y = 80;
        pane->style = WINDOW_FRAME;

        pane_raise(pane);
        list_add_tail(&pane->link, &desktop.windows);

        page->style = WINDOW_FRAME;
        page->x = pane->x;
        page->y = pane->y;
        page->z = pane->z;
        page->width = width;
        page->height = height;
        page->pitch = pane->pitch;
        page->max_width = (unsigned int)pane->max_width;
        page->max_height = (unsigned int)pane->max_height;
        page->mapping = (unsigned int)bytes;

        return pane;
}

/*
        A grid follows the window it is in.

        The size of a window of cells is a whole number of them, so a resize
        is rounded down to one and the program is told how many it now has --
        it is the program that has to lay its text out again.
*/
/*
        The size a window of cells will actually be given, which is a whole
        number of them and never more than the ring was cut for.

        Separate from pane_regrid because a resize has to know the answer
        before it can decide where to put the window. The edge a hand is not
        holding stays where it was only if the rounding is taken off the edge
        it IS holding, and that cannot be arranged after the fact -- so the
        one rule lives here and both of them ask it.
*/
static void pane_grid_fit(struct pane *pane, int *width, int *height)
{
        unsigned int columns, rows;

        // A window of pixels is whatever size it was asked for; it has no
        // cells to be a whole number of, and no ceiling but its own buffer.
        if (!pane->cells)
                return;

        columns =
            min((unsigned int)(max(*width - canvas_bar, 0) / canvas_cell_w),
                pane->max_columns);
        rows = min((unsigned int)(*height / canvas_cell_h), pane->max_rows);

        *width = (int)max(columns, 1u) * canvas_cell_w + canvas_bar;
        *height = (int)max(rows, 1u) * canvas_cell_h;
}

static void pane_regrid(struct pane *pane)
{
        if (!pane->cells)
                return;

        pane_grid_fit(pane, &pane->width, &pane->height);

        // Exact, because the fit is what made these a whole number of cells.
        pane->columns = (unsigned int)(pane->width - canvas_bar) / canvas_cell_w;
        pane->rows = (unsigned int)pane->height / canvas_cell_h;

        // Against the shape just worked out, not the one it replaced: whether
        // the view still has a full window of lines below it is a question
        // about the rows there are now.
        pane_view_clamp(pane);

        /*
                A resize reaches here from drag.c without going through
                pane_refresh, so a pane the compositor owns has no page to be
                told and nobody to tell. Its cells are the compositor's, and
                the shape they are drawn in is this one -- which is what makes
                a window it owns grow into the room it was given instead of
                leaving the desktop showing through the part nothing wrote.
        */
        if (!pane->shared)
        {
                pane->grid_columns = pane->columns;
                pane->grid_rows = pane->rows;
                pane->damage_row = 0;
                pane->damage_rows = pane->rows;
                console_regrid(pane);
                return;
        }

        WRITE_ONCE(pane->shared->columns, pane->columns);
        WRITE_ONCE(pane->shared->rows, pane->rows);
        WRITE_ONCE(pane->shared->width, (unsigned int)pane->width);
        WRITE_ONCE(pane->shared->height, (unsigned int)pane->height);

        // A program asleep on its file lays out to the new grid now, not
        // when its next key arrives.
        wake_up_interruptible(&pane->wait);
}

static void pane_refresh(struct pane *pane)
{
        struct window *shared = pane->shared;
        unsigned int width, height;

        if (!shared)
                return;

        /*
                Not while it is being dragged or resized. The compositor owns
                the rectangle for as long as a hand is on it, and reading the
                program's copy back mid-drag is two writers fighting over one
                number -- which looks like a window that will not stay where
                it is put.
        */
        if (desktop.dragging != pane && desktop.resizing != pane)
        {
                width = READ_ONCE(shared->width);
                height = READ_ONCE(shared->height);

                pane->width = (int)min(width, pane->max_width);
                pane->height = (int)min(height, pane->max_height);
                // Clamped the way z is, and for the same reason: these
                // are a program's numbers and everything drawn is measured
                // from them.
                pane->x = clamp(READ_ONCE(shared->x),
                                -WINDOW_COORD_MAX, WINDOW_COORD_MAX);
                pane->y = clamp(READ_ONCE(shared->y),
                                -WINDOW_COORD_MAX, WINDOW_COORD_MAX);
        }
        pane->z = clamp(READ_ONCE(shared->z), -WINDOW_Z_MAX, WINDOW_Z_MAX);
        pane->region = READ_ONCE(shared->region);
        pane->display = READ_ONCE(shared->display);
        pane->style = READ_ONCE(shared->style);
        pane->edge = (int)min(READ_ONCE(shared->edge), 256u);
        pane->sequence = READ_ONCE(shared->sequence);

        if (pane->cells)
        {
                // What the program says it changed, clamped to what it has.
                unsigned int row = READ_ONCE(shared->damage_row);
                unsigned int count = READ_ONCE(shared->damage_rows);
                unsigned int head = READ_ONCE(shared->head);

                /*
                        The shape the program says its cells are in, which is
                        not the shape they were asked to be in until it has
                        caught up. Composing from the requested one mid-resize
                        wraps every line at a width nothing was written at.
                */
                pane->grid_columns = min(READ_ONCE(shared->grid_columns),
                                         pane->max_columns);
                pane->grid_rows = min(READ_ONCE(shared->grid_rows), pane->max_rows);

                // A program's number, so it can be anything at all. Before the
                // ring began there is nothing to count backwards through.
                pane->head = max(head, pane->history);

                pane->damage_row = min(row, pane->grid_rows);
                pane->damage_rows = min(count, pane->grid_rows - pane->damage_row);

                // Scrolled back, what the program drew is not what is shown.
                if (pane->view != PANE_LIVE)
                        pane->damage_rows = 0;

                WRITE_ONCE(shared->damage_row, 0);
                WRITE_ONCE(shared->damage_rows, 0);
        }

        /*
                Copied, not pointed at. Composing walks this string every time
                it draws a titlebar, and the program can be rewriting the page
                underneath at any instant.
        */
        memory_copy_apart(pane->title, shared->title, WINDOW_TITLE_MAX);
        pane->title[WINDOW_TITLE_MAX - 1] = 0;
        pane->title_length = (unsigned int)string_length_max(
            pane->title, WINDOW_TITLE_MAX);

        pane_size(pane);
        pane_regrid(pane);
        pane_place(pane);

        // Where a region put it, so the program can read where it ended up.
        if (pane->region != WINDOW_FREE)
        {
                WRITE_ONCE(shared->x, pane->x);
                WRITE_ONCE(shared->y, pane->y);
        }
}

/*
        Focus is a pointer here and a bit in every shared page, so a program
        can see it without being told and the compositor can colour a titlebar
        without a lookup.
*/
static void pane_focus(struct pane *pane)
{
        struct pane *old = desktop.focused;

        if (old == pane)
                return;

        /*
                One bit each way, not the whole word.

                state was only ever WINDOW_FOCUSED, so assigning it was the
                same as setting the bit -- and stopped being the same the
                moment WINDOW_CLOSING joined it: clicking another window
                after pressing the X would have withdrawn the request the
                program had not read yet.
        */
        if (old)
        {
                old->state &= ~WINDOW_FOCUSED;
                if (old->shared)
                        WRITE_ONCE(old->shared->state, old->state);
        }

        desktop.focused = pane;

        if (pane)
        {
                pane->state |= WINDOW_FOCUSED;
                if (pane->shared)
                        WRITE_ONCE(pane->shared->state, pane->state);
        }
}

/*
        Ask the program to close its window.

        Nothing here frees anything. The pane goes when the program closes the
        file it was made from, which is window_release, and a program that
        never does keeps its window on the screen -- visibly, which is the
        honest outcome and not a leak: the alternative is the compositor
        taking pages out from under a task that is still writing to them.
*/
static void pane_close_request(struct pane *pane)
{
        if (!pane->shared || (pane->state & WINDOW_CLOSING))
                return;

        pane->state |= WINDOW_CLOSING;
        WRITE_ONCE(pane->shared->state, pane->state);
        atomic_set(&pane->closing_wake, 1);
        wake_up_interruptible(&pane->wait);
}

static PURE _Bool pane_focusable(struct pane *pane, _Bool include_minimized)
{
        return pane->shared && !(pane->style & WINDOW_PASSTHROUGH) &&
               (include_minimized || !(pane->style & WINDOW_MINIMIZED));
}

/*
        The highest z that can take focus, skipping one on its way out.

        Closing and minimizing both hand focus to the window underneath the
        one leaving, and Alt-Tab starts from the top of the whole stack; this
        walk was written once per caller before it was written here.
*/
static PURE struct pane *pane_topmost(struct pane *except,
                                      _Bool include_minimized)
{
        struct pane *pane;
        struct pane *top = NULL;

        list_for_each_entry(pane, &desktop.windows, link)
                if (pane != except && pane_focusable(pane, include_minimized) &&
                    (!top || pane->z > top->z))
                        top = pane;

        return top;
}

/*
        One Alt-Tab step, without changing z yet.

        Focus is allowed to preview a minimized pane; releasing Alt restores
        and raises it in pane_focus_commit. When focus is already the highest
        eligible z, choose the one immediately behind it. When focus came from
        minimizing that top window, choose the highest one instead, which is
        the window the user just put away and makes Alt-Tab a restore path.
*/
static _Bool pane_focus_step(void)
{
        struct pane *pane;
        struct pane *top = pane_topmost(NULL, true);
        struct pane *focused = desktop.focused;
        struct pane *next = NULL;
        int below;

        /*
                Start behind the active top window. If focus is not top -- it
                moved when that top window was minimized -- start from top so
                one Alt-Tab restores the thing just put away. Further steps
                descend the unchanged z order and wrap after the bottom.
        */
        if (desktop.focus_cycle_z)
                below = desktop.focus_cycle_z;
        else
                below = focused == top && focused ? focused->z : INT_MAX;

        list_for_each_entry(pane, &desktop.windows, link)
                if (pane_focusable(pane, true) && pane->z < below &&
                    (!next || pane->z > next->z))
                        next = pane;

        if (!next)
                next = top;

        if (!next || next == focused)
                return false;

        desktop.focus_cycle_z = next->z;
        pane_focus(next);
        return true;
}

static _Bool pane_focus_commit(void)
{
        struct pane *pane = desktop.focused;

        desktop.focus_cycle_z = 0;

        if (!pane || !pane_focusable(pane, true))
                return false;

        if (pane->style & WINDOW_MINIMIZED)
        {
                pane->style &= ~WINDOW_MINIMIZED;
                WRITE_ONCE(pane->shared->style, pane->style);
        }

        pane_raise(pane);
        list_move_tail(&pane->link, &desktop.windows);
        return true;
}

static _Bool pane_minimize_focused(void)
{
        struct pane *pane = desktop.focused;

        if (!pane || !pane_focusable(pane, false))
                return false;

        pane->style |= WINDOW_MINIMIZED;
        WRITE_ONCE(pane->shared->style, pane->style);

        pane_focus(pane_topmost(pane, false));
        return true;
}

/*
        Windows the desktop no longer reaches.

        Unplugging a screen shrinks the desktop, and anything that was on it
        is then outside every output -- drawn nowhere and impossible to take
        hold of, because the pointer is confined to the outputs. Each one is
        pulled back far enough that its titlebar is on a screen.
*/
static void desktop_gather_panes(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
        {
                int x = clamp(pane->x, 0, max(desktop.width - WINDOW_MIN_WIDTH, 0));
                int y = clamp(pane->y, 0, max(desktop.height - canvas_title, 0));

                if (x == pane->x && y == pane->y)
                        continue;

                pane->x = x;
                pane->y = y;

                if (pane->shared)
                {
                        WRITE_ONCE(pane->shared->x, x);
                        WRITE_ONCE(pane->shared->y, y);
                }
        }
}

static void desktop_damage_rect(const struct drm_rect *r)
{
        if (desktop.damage_all)
                return;

        if (desktop.damage_count == ARRAY_SIZE(desktop.damage))
        {
                desktop.damage_all = true;
                return;
        }

        desktop.damage[desktop.damage_count++] = *r;
}

static void desktop_damage(int x, int y, int w, int h)
{
        struct drm_rect r;

        drm_rect_init(&r, x, y, w, h);
        desktop_damage_rect(&r);
}

static void pane_damage_frame(struct pane *pane)
{
        struct drm_rect frame;

        pane_frame(pane, &frame);
        desktop_damage_rect(&frame);
}

static void pane_damage_was_and_now(struct pane *pane, const struct drm_rect *was)
{
        desktop_damage_rect(was);
        pane_damage_frame(pane);
}

// The rectangle a run of changed cell rows covers, in desktop coordinates.
// Owned panes and shared ones both report damage this way, and the titlebar
// offset was the same arithmetic written in each.
static void pane_damage_rows(struct pane *pane, unsigned int row, unsigned int count)
{
        desktop_damage(pane->x,
                       pane->y + pane_title(pane) + (int)row * canvas_cell_h,
                       pane->width, (int)count * canvas_cell_h);
}

/*
        The strip the scrollbar is drawn in, in desktop coordinates.

        The bounding box, worked out arithmetically, and never pane_bar: that
        walks all five hundred lines of the ring twice over to place the thumb
        exactly, which is the right price to pay once inside compose_bar --
        which rejects by this same box first -- and the wrong one to pay for
        every window on every pass of the refresh loop.
*/
static void pane_damage_bar(struct pane *pane)
{
        struct drm_rect gutter;

        pane_gutter_rect(pane, &gutter);
        desktop_damage_rect(&gutter);
}

/*
        Reads every shared page and records what moved.

        A program that changed a few rows of text should not cost a repaint of
        every screen, which is what committing used to mean -- and resizing a
        window of cells relaid every row, so a drag was a full recompose per
        step of it.
*/
static void desktop_refresh_panes(void)
{
        struct pane *pane;

        desktop.damage_count = 0;
        desktop.damage_all = false;

        list_for_each_entry(pane, &desktop.windows, link)
        {
                int was_x = pane->x, was_y = pane->y;
                int was_w = pane->width, was_h = pane->height;
                unsigned int was_z = (unsigned int)pane->z;
                unsigned int was_style = pane->style;
                unsigned int was_sequence = pane->sequence;
                struct drm_rect was;

                /*
                        The compositor's own. There is no page to read back, so
                        what changed is already recorded on the pane; taking it
                        clears it, the way pane_refresh clears a program's.
                */
                if (!pane->shared)
                {
                        if (pane->view_moved)
                        {
                                pane->view_moved = false;
                                pane->damage_rows = 0;
                                pane_damage_frame(pane);
                                continue;
                        }

                        /*
                                Scrolled back, the same as a program's window
                                below: the rows are not what is being shown,
                                so only the bar is redrawn -- and the report
                                is taken either way, or the desktop would be
                                told for ever that something had changed.
                        */
                        if (pane->cells && pane->view != PANE_LIVE)
                        {
                                pane_damage_bar(pane);
                                pane->damage_row = 0;
                                pane->damage_rows = 0;
                                continue;
                        }

                        if (pane->cells && pane->damage_rows)
                        {
                                unsigned int row = min(pane->damage_row, pane->grid_rows);
                                unsigned int count = min(pane->damage_rows,
                                                         pane->grid_rows - row);

                                if (count)
                                        pane_damage_rows(pane, row, count);

                                pane->damage_row = 0;
                                pane->damage_rows = 0;
                        }
                        continue;
                }

                pane_frame(pane, &was);
                pane_refresh(pane);

                /*
                        The wheel or a keystroke moved the view, which changes
                        every row of it.

                        Both rectangles, because pane_refresh has run since the
                        old frame was taken and the program may have moved the
                        window in the same pass. Damaging only where it was
                        left the rows it moved to undrawn, which is the trail
                        the reshaped path below damages twice to avoid.
                */
                if (pane->view_moved)
                {
                        pane->view_moved = false;
                        pane_damage_was_and_now(pane, &was);
                        continue;
                }

                _Bool reshaped = pane->x != was_x || pane->y != was_y ||
                                 pane->width != was_w || pane->height != was_h ||
                                 (unsigned int)pane->z != was_z ||
                                 pane->style != was_style;

                // Nothing that reaches the screen changed.
                if (!reshaped && pane->sequence == was_sequence)
                        continue;

                /*
                        Scrolled back: what the program drew is not what is
                        being shown, so none of its rows are repainted.

                        The bar is the exception, and used to be skipped with
                        them. It is a picture of how much there is and how far
                        down it you are looking, and both of those change with
                        every line that arrives -- so a window left scrolled
                        back while its program kept writing showed a thumb
                        that grew steadily more wrong about a ring that had
                        moved on underneath it, and only told the truth again
                        once the wheel was touched.
                */
                if (!reshaped && pane->view != PANE_LIVE)
                {
                        if (pane->cells)
                                pane_damage_bar(pane);

                        continue;
                }

                // Anything but text changing in place is easier to repaint
                // whole than to reason about.
                if (reshaped || !pane->cells || !pane->damage_rows)
                {
                        pane_damage_was_and_now(pane, &was);
                        continue;
                }

                pane_damage_rows(pane, pane->damage_row, pane->damage_rows);
        }

        /*
                The keyboard follows what the user can actually see.

                A program owns its style word, and pane_refresh above has just
                read it back: it can set MINIMIZED, which compose draws
                nowhere, and PASSTHROUGH, which drag lets the pointer fall
                straight through. Neither bit moved focus, so a window could
                take the keyboard on creation and then make itself invisible
                and untouchable while every keystroke kept arriving in the
                ring it shares. That is a keylogger built entirely out of the
                documented interface, with no bug to find.

                pane_minimize_focused already holds this invariant for the
                compositor's own minimize. This is the same rule applied to
                the path the program drives.

                Only a pane with a shared page. An owned one -- the console
                -- has no program behind it and no page to set a style
                through, so it cannot be the thing this guards against, and
                pane_focusable rejects it for the unrelated reason that it
                has no page at all. pane_under does not skip it, so a click
                can focus it, and without this test that focus would be taken
                straight back on the next pass.

                Mid Alt-Tab a minimized window is legitimately focused, so
                cycling passes include_minimized and only PASSTHROUGH revokes.
                Both flags are read, not just focus_cycling: releasing Alt
                clears cycling and sets commit in one step, and until the
                canvas thread runs pane_focus_commit the window it is about to
                restore is still minimized. A refresh in that gap -- any
                program can drive one through WINDOW_IOCTL_COMMIT -- would
                otherwise revoke the focus commit was on its way to keep.
        */
        if (desktop.focused && desktop.focused->shared &&
            !pane_focusable(desktop.focused,
                            atomic_read(&desktop.focus_cycling) ||
                            atomic_read(&desktop.focus_commit)))
        {
                pane_focus(pane_topmost(desktop.focused, false));

                // pane_focus restyles every titlebar, and the damage loop
                // above has already run. Every other caller pairs it with a
                // redraw; this is that, without recursing into one.
                desktop.damage_all = true;
        }

        list_sort(NULL, &desktop.windows, pane_by_z);
}

static long window_ioctl_create(struct file *file, unsigned long argument)
{
        struct device_context *context = file->private_data;
        struct window_request request;
        struct pane *pane;
        unsigned long bytes;

        if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
                return -EFAULT;

        mutex_lock(&desktop.lock);

        // Tested and stored under the one lock: two threads on one file used
        // to be able to both create, and the window one of them made was then
        // reachable from nothing and freed by nothing.
        if (context->pane)
        {
                mutex_unlock(&desktop.lock);
                return -EBUSY;
        }

        if (list_empty(&desktop.outputs))
        {
                mutex_unlock(&desktop.lock);
                return -ENODEV;
        }

        pane = pane_create(request.width, request.height,
                           request.columns, request.rows, false);
        if (!pane)
        {
                mutex_unlock(&desktop.lock);
                return -EINVAL;
        }

        bytes = pane->bytes;

        /*
                Released, because window_mmap reads this pointer without the
                desktop lock and must not see it before the pane it points at.
                Two threads on one descriptor -- create here, mmap there -- is
                allowed, and on a weakly ordered machine the store of the
                pointer can otherwise land ahead of the stores that filled in
                bytes and mapping. Free on x86, a compiler barrier only.
        */
        smp_store_release(&context->pane, pane);
        pane_focus(pane);
        desktop_redraw();

        mutex_unlock(&desktop.lock);

        // How much to map, which the program cannot work out for itself.
        return (long)bytes;
}

/*
        Whether the program has anything to wake up for: a key it has not
        read, or a grid the compositor changed that it has not laid out to.
        The wait queue is the pane's, and the pane outlives every poll on its
        file, since only the file's own release frees it.
*/
static __poll_t window_poll(struct file *file, poll_table *wait)
{
        struct device_context *context = file->private_data;
        struct pane *pane = smp_load_acquire(&context->pane);
        struct window *shared;

        if (!pane || !pane->shared)
                return 0;

        shared = pane->shared;
        poll_wait(file, &pane->wait, wait);

        /*
                The close request wakes the program and has to be something it
                can wake up to. pane_close_request already wakes this queue;
                without a reason here the poll went straight back to sleep and
                the X in the titlebar did nothing until the next keystroke.

                Taken rather than read, so it is delivered once. The request
                itself stays set in the shared page for the program to find at
                its own pace; answering from that instead would make every
                later poll return immediately and turn a program that ignores
                the X into one that spins.
        */
        if (atomic_xchg(&pane->closing_wake, 0))
                return EPOLLIN | EPOLLRDNORM;

        // The grid the program laid out to is its own record in the page;
        // the compositor's columns and rows moving away from it is a resize
        // it has not seen yet, the same test window_regrid makes.
        if (READ_ONCE(shared->key_head) != READ_ONCE(shared->key_tail) ||
            READ_ONCE(shared->columns) != READ_ONCE(shared->grid_columns) ||
            READ_ONCE(shared->rows) != READ_ONCE(shared->grid_rows))
                return EPOLLIN | EPOLLRDNORM;

        return 0;
}

static long window_ioctl_commit(struct file *file)
{
        struct device_context *context = file->private_data;

        if (!context->pane)
                return -EINVAL;

        mutex_lock(&desktop.lock);
        desktop_watch();
        desktop_refresh_panes();
        desktop_repaint();
        mutex_unlock(&desktop.lock);

        return 0;
}

static int window_mmap(struct file *file, struct vm_area_struct *vma)
{
        struct device_context *context = file->private_data;
        struct pane *pane = smp_load_acquire(&context->pane);

        if (!pane)
                return -EINVAL;

        if (vma->vm_pgoff)
                return -EINVAL;

        if (vma->vm_end - vma->vm_start > pane->bytes)
                return -EINVAL;

        return remap_vmalloc_range(vma, pane->mapping, 0);
}

static void window_release(struct file *file)
{
        struct device_context *context = file->private_data;
        struct pane *pane = context->pane;
        _Bool refocus;

        if (!pane)
                return;

        mutex_lock(&desktop.lock);

        // Asked before the free, which is what clears it.
        refocus = desktop.focused == pane;

        pane_free(pane);

        // Closing the active window hands focus to what is now on top,
        // instead of leaving a live desktop with nowhere for keys to go.
        if (refocus)
                pane_focus(pane_topmost(NULL, false));

        if (!list_empty(&desktop.outputs))
                desktop_redraw();

        mutex_unlock(&desktop.lock);

        context->pane = NULL;
}

/*
        Watching the shared pages.

        A program changes a window by storing into its page and bumping a
        sequence, which costs it nothing. Something has to notice, so this
        ticks while there is something to notice and stops once nothing has
        changed for a couple of seconds. An idle desktop takes no wakeups, and
        a program drawing every frame makes no calls; the one call is the first
        change after the compositor went to sleep.

        The period is the fastest output's refresh. Sixteen milliseconds is
        60 Hz, which is the wrong answer on a 120 Hz panel and used to be
        the only answer this had.
*/
#define CANVAS_FRAME_FALLBACK_NS (NSEC_PER_SEC / 60)
#define CANVAS_IDLE_NS (2ULL * NSEC_PER_SEC)

static void desktop_sync_frame_ns(void)
{
        struct output *output;
        unsigned int hz = 0;
        u64 ns;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                unsigned int refresh;

                if (!output->mode_set || !output->mode_set->mode)
                        continue;

                refresh = drm_mode_vrefresh(output->mode_set->mode);
                if (refresh > hz)
                        hz = refresh;
        }

        ns = hz ? NSEC_PER_SEC / hz : CANVAS_FRAME_FALLBACK_NS;
        if (!ns)
                ns = CANVAS_FRAME_FALLBACK_NS;

        WRITE_ONCE(desktop.frame_ns, ns);
}

static u64 canvas_frame_ns(void)
{
        u64 ns = READ_ONCE(desktop.frame_ns);

        return ns ? ns : CANVAS_FRAME_FALLBACK_NS;
}

static void desktop_set_awake(_Bool awake)
{
        struct pane *pane;

        desktop.awake = awake;

        list_for_each_entry(pane, &desktop.windows, link)
                if (pane->shared)
                        WRITE_ONCE(pane->shared->awake, awake);
}

static enum hrtimer_restart desktop_frame(struct hrtimer *timer)
{
        if (!READ_ONCE(desktop.awake))
                return HRTIMER_NORESTART;

        atomic_set(&desktop.frame_pending, 1);
        canvas_thread_wake();

        hrtimer_forward_now(timer, ns_to_ktime(canvas_frame_ns()));
        return HRTIMER_RESTART;
}

static _Bool canvas_thread_running(void);

static void desktop_watch(void)
{
        if (desktop.awake)
        {
                desktop.idle_frames = 0;
                return;
        }

        // A frame with nothing to service it wakes nobody and re-arms itself
        // forever, and every program would sit waiting on an awake that no
        // longer means anything.
        if (!canvas_thread_running())
                return;

        desktop_set_awake(true);
        desktop.idle_frames = 0;
        hrtimer_start(&desktop.frame, ns_to_ktime(canvas_frame_ns()),
                      HRTIMER_MODE_REL);
}

static _Bool desktop_sequence_changed(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
        {
                if (pane->view_moved)
                        return true;

                if (!pane->shared)
                {
                        if (pane->damage_rows)
                                return true;

                        continue;
                }

                if (READ_ONCE(pane->shared->sequence) != pane->sequence)
                        return true;
        }

        return false;
}

/*
        A magnified cursor goes back on its own. Nothing else is watching the
        clock, so the frame the shake armed is what notices.
*/
static void cursor_settle(void)
{
        if (desktop.cursor_scale <= desktop.scale)
                return;

        if (ktime_get_ns() < desktop.magnified_until)
                return;

        desktop.cursor_scale = desktop.scale;
        cursor_move(desktop.cursor_x, desktop.cursor_y);
}

static void desktop_frame_pass(void)
{
        mutex_lock(&desktop.lock);

        cursor_settle();

        if (desktop_sequence_changed())
        {
                desktop_refresh_panes();
                desktop_repaint();
                desktop.idle_frames = 0;
        }
        else if (desktop.cursor_scale > desktop.scale)
        {
                desktop.idle_frames = 0;
        }
        else if ((u64)++desktop.idle_frames * canvas_frame_ns() >= CANVAS_IDLE_NS)
        {
                desktop_set_awake(false);

                /*
                        One more look after clearing it. A program that read
                        awake just before this and so skipped its call would
                        otherwise be left on screen a frame behind until the
                        next thing it did.
                */
                smp_mb();

                if (desktop_sequence_changed())
                {
                        desktop_refresh_panes();
                        desktop_repaint();
                }
        }

        mutex_unlock(&desktop.lock);
}
