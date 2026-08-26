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

        title = pane->style & WINDOW_FRAME ? WINDOW_TITLE : 0;

        if (pane->style & WINDOW_FULLSCREEN)
        {
                // As much of the output as the buffer the program asked for
                // will cover, from its corner.
                pane->x = output->x;
                pane->y = output->y;
                return;
        }

        if (pane->region != WINDOW_CENTRED)
                return;

        pane->x = output->x + ((int)output->width - pane->width) / 2;
        pane->y = output->y + ((int)output->height - (pane->height + title)) / 2;
}

static int pane_top_z(void)
{
        struct pane *pane;
        int top = 0;

        list_for_each_entry(pane, &desktop.windows, link)
                top = max(top, pane->z);

        return top;
}

static void pane_raise(struct pane *pane)
{
        pane->z = pane_top_z() + 1;

        if (pane->shared)
                WRITE_ONCE(pane->shared->z, pane->z);
}

static int pane_by_z(void *unused, const struct list_head *a, const struct list_head *b)
{
        int za = list_entry(a, struct pane, link)->z;
        int zb = list_entry(b, struct pane, link)->z;

        return za < zb ? -1 : za > zb;
}

static void pane_free(struct pane *pane)
{
        list_del(&pane->link);
        vfree(pane->mapping);
        kfree(pane);
}

static struct pane *pane_create(unsigned int width, unsigned int height)
{
        struct pane *pane;
        unsigned long bytes;

        // A window is allowed to be as large as the desktop and no larger.
        // Not the kind of ceiling the rest of this refuses: it is what stops
        // one program asking for every page in the machine.
        if (!width || !height ||
            width > (unsigned int)desktop.width || height > (unsigned int)desktop.height)
                return NULL;

        pane = kzalloc(sizeof(*pane), GFP_KERNEL);
        if (!pane)
                return NULL;

        bytes = PAGE_ALIGN(WINDOW_PIXELS + (unsigned long)width * height * 4);

        pane->mapping = vmalloc_user(bytes);
        if (!pane->mapping)
        {
                kfree(pane);
                return NULL;
        }

        pane->bytes = bytes;
        pane->shared = pane->mapping;
        pane->pixels = pane->mapping + WINDOW_PIXELS;
        pane->pitch = width;
        pane->max_width = width;
        pane->max_height = height;

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
        pane->shared->max_width = width;
        pane->shared->max_height = height;

        return pane;
}

/*
        The only place a program's numbers get believed, and only after they are
        clamped to the buffer that was actually allocated for it.
*/
static void pane_refresh(struct pane *pane)
{
        struct window *shared = pane->shared;
        unsigned int width, height;

        if (!shared)
                return;

        width = READ_ONCE(shared->width);
        height = READ_ONCE(shared->height);

        pane->width = (int)min(width, pane->max_width);
        pane->height = (int)min(height, pane->max_height);
        pane->x = READ_ONCE(shared->x);
        pane->y = READ_ONCE(shared->y);
        pane->z = READ_ONCE(shared->z);
        pane->region = READ_ONCE(shared->region);
        pane->display = READ_ONCE(shared->display);
        pane->style = READ_ONCE(shared->style);
        pane->edge = (int)min(READ_ONCE(shared->edge), 256u);
        pane->sequence = READ_ONCE(shared->sequence);

        pane_place(pane);

        // Where a region put it, so the program can read where it ended up.
        if (pane->region != WINDOW_FREE)
        {
                WRITE_ONCE(shared->x, pane->x);
                WRITE_ONCE(shared->y, pane->y);
        }
}

// Drawing is the list order, so the list is kept in z order. list_sort is
// stable, which is what keeps windows that share a z where they were.
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

static void desktop_refresh_panes(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
                pane_refresh(pane);

        list_sort(NULL, &desktop.windows, pane_by_z);
}

static long window_ioctl_create(struct file *file, unsigned long argument)
{
        struct window_request request;
        struct pane *pane;

        if (file->private_data)
                return -EBUSY;

        if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
                return -EFAULT;

        mutex_lock(&desktop.lock);

        if (list_empty(&desktop.outputs))
        {
                mutex_unlock(&desktop.lock);
                return -ENODEV;
        }

        pane = pane_create(request.width, request.height);
        if (pane)
        {
                pane_focus(pane);
                desktop_redraw();
        }

        mutex_unlock(&desktop.lock);

        if (!pane)
                return -EINVAL;

        file->private_data = pane;
        return 0;
}

static long window_ioctl_commit(struct file *file)
{
        if (!file->private_data)
                return -EINVAL;

        mutex_lock(&desktop.lock);
        desktop_watch();
        desktop_refresh_panes();
        desktop_redraw();
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

static void desktop_watch(void)
{
        if (desktop.awake)
        {
                desktop.idle_frames = 0;
                return;
        }

        desktop_set_awake(true);
        desktop.idle_frames = 0;
        hrtimer_start(&desktop.frame, ms_to_ktime(CANVAS_FRAME_MS), HRTIMER_MODE_REL);
}

static _Bool desktop_sequence_changed(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
                if (pane->shared && READ_ONCE(pane->shared->sequence) != pane->sequence)
                        return true;

        return false;
}

static void desktop_frame_pass(void)
{
        mutex_lock(&desktop.lock);

        if (desktop_sequence_changed())
        {
                desktop_refresh_panes();
                desktop_redraw();
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
                        desktop_redraw();
                }
        }

        mutex_unlock(&desktop.lock);
}
