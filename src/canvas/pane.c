/*
        Canvas -- windows

        One file per window. The ioctl sizes it, the mapping is at offset zero,
        and closing the file destroys it, so there is no identifier for either
        side to get wrong and no lifetime to track separately from the fd.

        The shared page is writable by the program at any instant, so nothing
        below composes from it. pane_refresh takes a copy and checks it; the
        buffer's extent is the compositor's and is never read back from the
        page at all.
*/

static struct output *output_by_index(unsigned int index)
{
        struct output *output;
        unsigned int i = 0;

        list_for_each_entry(output, &desktop.outputs, link)
                if (i++ == index)
                        return output;

        return list_first_entry_or_null(&desktop.outputs, struct output, link);
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

        title = pane->style & WINDOW_FRAME ? canvas_title : 0;

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

        title = pane->style & WINDOW_FRAME ? canvas_title : 0;

        pane->width = (int)min(output->width, pane->max_width);
        pane->height = (int)min(output->height - (unsigned int)title, pane->max_height);
}

static void pane_raise(struct pane *raised)
{
        struct pane *pane;
        int top = 0;

        list_for_each_entry(pane, &desktop.windows, link)
                top = max(top, pane->z);

        raised->z = top + 1;

        if (raised->shared)
                WRITE_ONCE(raised->shared->z, raised->z);
}

static int pane_by_z(void *unused, const struct list_head *a, const struct list_head *b)
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

static void pane_free(struct pane *pane)
{
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
static void desktop_grid(unsigned int *columns, unsigned int *rows)
{
        int width = desktop.width - canvas_border * 2;
        int height = desktop.height - (canvas_title + canvas_border * 3);

        *columns = (unsigned int)(max(width, 0) / canvas_cell_w);
        *rows = (unsigned int)(max(height, 0) / canvas_cell_h);
}

static struct pane *pane_create(unsigned int width, unsigned int height,
                                unsigned int columns, unsigned int rows)
{
        unsigned int max_columns, max_rows;
        unsigned long cell_bytes;
        struct pane *pane;
        unsigned long bytes;

        desktop_grid(&max_columns, &max_rows);
        cell_bytes = (unsigned long)max_columns * max_rows *
                     sizeof(struct window_cell);

        /*
                A window of cells is allocated for as many as the desktop
                could hold, not for as many as it asked for, because its size
                is its grid: resizing it changes how many cells there are, and
                a program cannot be handed a larger mapping than the one it
                already has.
        */
        if (columns)
        {
                columns = min(columns, max_columns);
                rows = min(rows, max_rows);
                width = columns * (unsigned int)canvas_cell_w;
                height = rows * (unsigned int)canvas_cell_h;
        }

        // A window is allowed to be as large as the desktop and no larger.
        // Not the kind of ceiling the rest of this refuses: it is what stops
        // one program asking for every page in the machine.
        if (!width || !height ||
            width > (unsigned int)desktop.width || height > (unsigned int)desktop.height)
                return NULL;

        pane = kzalloc(sizeof(*pane), GFP_KERNEL);
        if (!pane)
                return NULL;

        bytes = PAGE_ALIGN(WINDOW_PIXELS +
                           (columns ? cell_bytes : (unsigned long)width * height * 4));

        if (canvas_pane_bytes + bytes > canvas_pane_budget())
        {
                kfree(pane);
                return NULL;
        }

        pane->mapping = vmalloc_user(bytes);
        if (!pane->mapping)
        {
                kfree(pane);
                return NULL;
        }

        canvas_pane_bytes += bytes;
        pane->bytes = bytes;
        pane->shared = pane->mapping;
        pane->pitch = width;

        if (columns)
        {
                pane->cells = pane->mapping + WINDOW_PIXELS;
                pane->columns = columns;
                pane->rows = rows;
                pane->max_columns = max_columns;
                pane->max_rows = max_rows;
                pane->shared->max_columns = max_columns;
                pane->shared->max_rows = max_rows;
                pane->max_width = max_columns * (unsigned int)canvas_cell_w;
                pane->max_height = max_rows * (unsigned int)canvas_cell_h;
                pane->shared->columns = columns;
                pane->shared->rows = rows;
                pane->grid_columns = columns;
                pane->grid_rows = rows;
                pane->shared->grid_columns = columns;
                pane->shared->grid_rows = rows;
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
        pane->shared->style = WINDOW_FRAME;

        list_add_tail(&pane->link, &desktop.windows);
        pane_raise(pane);

        pane->shared->x = pane->x;
        pane->shared->y = pane->y;
        pane->shared->z = pane->z;
        pane->shared->width = width;
        pane->shared->height = height;
        pane->shared->pitch = pane->pitch;
        pane->shared->max_width = (unsigned int)pane->max_width;
        pane->shared->max_height = (unsigned int)pane->max_height;
        pane->shared->mapping = (unsigned int)bytes;

        return pane;
}

/*
        A window of cells the compositor draws into itself: no shared page, and
        the cells are its own rather than a program's mapping. Everything that
        reads a page checks for one first, so one of these is placed, moved and
        composed like any other window.
*/
static __maybe_unused struct pane *pane_create_owned(unsigned int columns,
                                                     unsigned int rows)
{
        unsigned int max_columns, max_rows;
        unsigned long bytes;
        struct pane *pane;

        desktop_grid(&max_columns, &max_rows);
        bytes = PAGE_ALIGN((unsigned long)max_columns * max_rows *
                           sizeof(struct window_cell));

        if (!columns || !rows || !max_columns || !max_rows)
                return NULL;

        if (canvas_pane_bytes + bytes > canvas_pane_budget())
                return NULL;

        pane = kzalloc(sizeof(*pane), GFP_KERNEL);
        if (!pane)
                return NULL;

        // Not vmalloc_user: nothing maps this one.
        pane->mapping = vzalloc(bytes);
        if (!pane->mapping)
        {
                kfree(pane);
                return NULL;
        }

        canvas_pane_bytes += bytes;
        pane->bytes = bytes;
        pane->cells = pane->mapping;
        pane->columns = min(columns, max_columns);
        pane->rows = min(rows, max_rows);
        pane->grid_columns = pane->columns;
        pane->grid_rows = pane->rows;
        pane->max_columns = max_columns;
        pane->max_rows = max_rows;
        pane->max_width = max_columns * (unsigned int)canvas_cell_w;
        pane->max_height = max_rows * (unsigned int)canvas_cell_h;
        pane->width = (int)pane->columns * canvas_cell_w;
        pane->height = (int)pane->rows * canvas_cell_h;
        pane->pitch = (unsigned int)pane->width;
        pane->x = 80;
        pane->y = 80;
        pane->style = WINDOW_FRAME;

        list_add_tail(&pane->link, &desktop.windows);
        pane_raise(pane);

        return pane;
}

/*
        A grid follows the window it is in.

        The size of a window of cells is a whole number of them, so a resize
        is rounded down to one and the program is told how many it now has --
        it is the program that has to lay its text out again.
*/
static void pane_regrid(struct pane *pane)
{
        if (!pane->cells)
                return;

        pane->columns = min((unsigned int)(pane->width / canvas_cell_w), pane->max_columns);
        pane->rows = min((unsigned int)(pane->height / canvas_cell_h), pane->max_rows);

        if (!pane->columns)
                pane->columns = 1;

        if (!pane->rows)
                pane->rows = 1;

        pane->width = (int)pane->columns * canvas_cell_w;
        pane->height = (int)pane->rows * canvas_cell_h;

        /*
                A resize reaches here from drag.c without going through
                pane_refresh, so a pane the compositor owns has no page to be
                told. Its grid is left alone for the same reason a program's
                is: the cells are still in the shape they were laid out in
                until whoever owns them says otherwise.
        */
        if (!pane->shared)
                return;

        WRITE_ONCE(pane->shared->columns, pane->columns);
        WRITE_ONCE(pane->shared->rows, pane->rows);
        WRITE_ONCE(pane->shared->width, (unsigned int)pane->width);
        WRITE_ONCE(pane->shared->height, (unsigned int)pane->height);
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
                pane->x = READ_ONCE(shared->x);
                pane->y = READ_ONCE(shared->y);
        }
        pane->z = READ_ONCE(shared->z);
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

                /*
                        The shape the program says its cells are in, which is
                        not the shape they were asked to be in until it has
                        caught up. Composing from the requested one mid-resize
                        reads every row at the wrong stride.
                */
                pane->grid_columns = min(READ_ONCE(shared->grid_columns),
                                         pane->max_columns);
                pane->grid_rows = min(READ_ONCE(shared->grid_rows), pane->max_rows);

                pane->damage_row = min(row, pane->grid_rows);
                pane->damage_rows = min(count, pane->grid_rows - pane->damage_row);

                WRITE_ONCE(shared->damage_row, 0);
                WRITE_ONCE(shared->damage_rows, 0);
        }

        /*
                Copied, not pointed at. Composing walks this string every time
                it draws a titlebar, and the program can be rewriting the page
                underneath at any instant.
        */
        memcpy(pane->title, shared->title, WINDOW_TITLE_MAX);
        pane->title[WINDOW_TITLE_MAX - 1] = 0;
        pane->title_length = strnlen(pane->title, WINDOW_TITLE_MAX);

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
        struct pane *other;

        if (desktop.focused == pane)
                return;

        desktop.focused = pane;

        list_for_each_entry(other, &desktop.windows, link)
        {
                other->state = other == pane ? WINDOW_FOCUSED : 0;

                if (other->shared)
                        WRITE_ONCE(other->shared->state, other->state);
        }
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

static void desktop_damage(int x, int y, int w, int h)
{
        if (desktop.damage_all)
                return;

        if (desktop.damage_count == ARRAY_SIZE(desktop.damage))
        {
                desktop.damage_all = true;
                return;
        }

        rect_set(&desktop.damage[desktop.damage_count++], x, y, w, h);
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
                int fx, fy, fw, fh;

                /*
                        The compositor's own. There is no page to read back, so
                        what changed is already recorded on the pane; taking it
                        clears it, the way pane_refresh clears a program's.
                */
                if (!pane->shared)
                {
                        if (pane->cells && pane->damage_rows)
                        {
                                unsigned int row = min(pane->damage_row, pane->grid_rows);
                                unsigned int count = min(pane->damage_rows,
                                                         pane->grid_rows - row);

                                if (count)
                                        desktop_damage(pane->x,
                                                       pane->y +
                                                           (pane->style & WINDOW_FRAME ? canvas_title : 0) +
                                                           (int)row * canvas_cell_h,
                                                       pane->width,
                                                       (int)count * canvas_cell_h);

                                pane->damage_row = 0;
                                pane->damage_rows = 0;
                        }
                        continue;
                }

                pane_frame(pane, &fx, &fy, &fw, &fh);
                pane_refresh(pane);

                if (pane->sequence == was_sequence && pane->x == was_x &&
                    pane->y == was_y && pane->width == was_w &&
                    pane->height == was_h && (unsigned int)pane->z == was_z &&
                    pane->style == was_style)
                        continue;

                // Anything but text changing in place is easier to repaint
                // whole than to reason about.
                if (pane->x != was_x || pane->y != was_y || pane->width != was_w ||
                    pane->height != was_h || (unsigned int)pane->z != was_z ||
                    pane->style != was_style || !pane->cells || !pane->damage_rows)
                {
                        desktop_damage(fx, fy, fw, fh);
                        pane_frame(pane, &fx, &fy, &fw, &fh);
                        desktop_damage(fx, fy, fw, fh);
                        continue;
                }

                desktop_damage(pane->x,
                               pane->y + (pane->style & WINDOW_FRAME ? canvas_title : 0) +
                                   (int)pane->damage_row * canvas_cell_h,
                               pane->width, (int)pane->damage_rows * canvas_cell_h);
        }

        list_sort(NULL, &desktop.windows, pane_by_z);
}

static long window_ioctl_create(struct file *file, unsigned long argument)
{
        struct window_request request;
        struct pane *pane;
        unsigned long bytes;

        if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
                return -EFAULT;

        mutex_lock(&desktop.lock);

        // Tested and stored under the one lock: two threads on one file used
        // to be able to both create, and the window one of them made was then
        // reachable from nothing and freed by nothing.
        if (file->private_data)
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
                           request.columns, request.rows);
        if (!pane)
        {
                mutex_unlock(&desktop.lock);
                return -EINVAL;
        }

        bytes = pane->bytes;
        file->private_data = pane;
        pane_focus(pane);
        desktop_redraw();

        mutex_unlock(&desktop.lock);

        // How much to map, which the program cannot work out for itself.
        return (long)bytes;
}

static long window_ioctl_commit(struct file *file)
{
        if (!file->private_data)
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
        struct pane *pane = file->private_data;

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
        struct pane *pane = file->private_data;

        if (!pane)
                return;

        mutex_lock(&desktop.lock);

        if (desktop.dragging == pane)
                desktop.dragging = NULL;

        if (desktop.resizing == pane)
                desktop.resizing = NULL;

        if (desktop.focused == pane)
                desktop.focused = NULL;

        pane_free(pane);

        if (!list_empty(&desktop.outputs))
                desktop_redraw();

        mutex_unlock(&desktop.lock);

        file->private_data = NULL;
}

/*
        Watching the shared pages.

        A program changes a window by storing into its page and bumping a
        sequence, which costs it nothing. Something has to notice, so this
        ticks while there is something to notice and stops once nothing has
        changed for a couple of seconds. An idle desktop takes no wakeups, and
        a program drawing every frame makes no calls; the one call is the first
        change after the compositor went to sleep.
*/
#define CANVAS_FRAME_MS 16
#define CANVAS_IDLE_FRAMES 120

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

        hrtimer_forward_now(timer, ms_to_ktime(CANVAS_FRAME_MS));
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
        hrtimer_start(&desktop.frame, ms_to_ktime(CANVAS_FRAME_MS), HRTIMER_MODE_REL);
}

static _Bool desktop_sequence_changed(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
        {
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
        else if (++desktop.idle_frames > CANVAS_IDLE_FRAMES)
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
