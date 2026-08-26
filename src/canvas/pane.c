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

        pane->shared->x = pane->x;
        pane->shared->y = pane->y;
        pane->shared->width = width;
        pane->shared->height = height;
        pane->shared->pitch = pane->pitch;
        pane->shared->max_width = width;
        pane->shared->max_height = height;

        list_add_tail(&pane->link, &desktop.windows);

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
}

static void desktop_refresh_panes(void)
{
        struct pane *pane;

        list_for_each_entry(pane, &desktop.windows, link)
                pane_refresh(pane);
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
                desktop_redraw();

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

        pane_free(pane);

        if (!list_empty(&desktop.outputs))
                desktop_redraw();

        mutex_unlock(&desktop.lock);

        file->private_data = NULL;
}
