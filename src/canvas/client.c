/*
        Canvas -- attaching to DRM

        Opening the node runs the driver's open path and hands back a
        drm_file, which knows its minor, which knows its device. The file is
        closed immediately; drm_client_init takes its own reference.

        Every card is taken, not just the first. The poll is here because the
        nodes appear when devtmpfs is mounted, after the initcalls that could
        otherwise have started this.
*/

/*
        The outputs come off the desktop before the card they belong to is
        released, or output->canvas dangles for anything still composing.
        canvas_thread_stop joins a thread that takes desktop.lock, so it runs under
        canvas_list_lock and never under desktop.lock.
*/
static COLD void client_unregister(struct drm_client_dev *client)
{
        struct canvas *canvas = canvas_from_client(client);

        mutex_lock(&canvas_list_lock);
        list_del(&canvas->link);
        if (list_empty(&canvas_list))
                canvas_thread_stop();
        mutex_unlock(&canvas_list_lock);

        mutex_lock(&desktop.lock);
        canvas->started = 0;
        canvas_release(canvas);
        mutex_unlock(&desktop.lock);

        drm_client_release(client);
}

static COLD void client_free(struct drm_client_dev *client)
{
        kfree(canvas_from_client(client));
}

static int client_hotplug(struct drm_client_dev *client)
{
        struct canvas *canvas = canvas_from_client(client);
        int ret = 0;

        mutex_lock(&desktop.lock);

        if (!canvas->started || canvas_modes_changed(canvas))
        {
                if (canvas->started)
                {
                        // A screen arrived, left, or only now said what it can
                        // do. Rebuild the card from the freshly probed modes.
                        canvas_release(canvas);
                }

                ret = canvas_start(canvas);
                canvas->started = (ret == 0);
        }
        else
        {
                desktop_redraw();
        }

        mutex_unlock(&desktop.lock);
        return ret;
}

// The bool argument is whether the restore happens from an atomic context.
static int client_restore(struct drm_client_dev *client, _Bool in_atomic)
{
        struct canvas *canvas = canvas_from_client(client);

        if (in_atomic)
                return -EBUSY;

        mutex_lock(&desktop.lock);
        if (canvas->started)
                desktop_redraw();
        mutex_unlock(&desktop.lock);

        return 0;
}

static const struct drm_client_funcs client_funcs = {
    .owner = THIS_MODULE,
    .unregister = client_unregister,
    .free = client_free,
    .restore = client_restore,
    .hotplug = client_hotplug,
};

static _Bool canvas_holds(struct drm_device *dev)
{
        struct canvas *canvas;
        _Bool held = false;

        mutex_lock(&canvas_list_lock);
        list_for_each_entry(canvas, &canvas_list, link)
        {
                if (canvas->client.dev == dev)
                {
                        held = true;
                        break;
                }
        }
        mutex_unlock(&canvas_list_lock);

        return held;
}

/*
        Everything but the registering, which is deliberately not done here:
        registering fires the hotplug that chooses a mode and commits it, and
        that has to happen with no file open on the card. See canvas_claim.
*/
static struct canvas *canvas_take_over(struct drm_device *dev)
{
        struct canvas *canvas;
        _Bool first;

        if (!drm_core_check_feature(dev, DRIVER_MODESET) || canvas_holds(dev))
                return NULL;

        // The face the whole machine draws with. The kernel already carries
        // it for its own console.
        if (!canvas_font)
        {
                canvas_font = find_font("VGA8x16");

                if (!canvas_font)
                {
                        log_canvas("no console font to draw with\n");
                        return NULL;
                }

                canvas_cursor_bits();
        }

        canvas = kzalloc(sizeof(*canvas), GFP_KERNEL);
        if (!canvas)
                return NULL;

        canvas->set_result = 1;

        if (drm_client_init(dev, &canvas->client, "moonwater", &client_funcs))
        {
                kfree(canvas);
                return NULL;
        }

        mutex_lock(&canvas_list_lock);
        first = list_empty(&canvas_list);
        list_add_tail(&canvas->link, &canvas_list);
        if (first)
                canvas_thread_start();
        mutex_unlock(&canvas_list_lock);

        return canvas;
}

// Retried fast: the node appears the moment devtmpfs is mounted, and every
// millisecond spent waiting after that is a millisecond of black screen.
#define CANVAS_RETRY_MS 5
#define CANVAS_ATTEMPTS 1000

// Rounds to keep looking after the first card, so a sibling that probes late
// is found too.
//
// A hundred rounds at CANVAS_RETRY_MS is half a second of polling after the
// screen is already up, which reads like an obvious thing to cut. It is not:
// measured at 10 rounds against 100, moonwater starts, the canvas is drawn
// and the shell runs at the same times to within the noise of five boots
// each. The poll is a delayed work item that sleeps between rounds, so what
// it costs the boot is not the half second it spans. Cutting it would buy
// nothing and lose the late sibling it is here for.
#define CANVAS_SETTLE 100

static struct delayed_work canvas_probe_work;
static unsigned int canvas_attempts;
static unsigned int canvas_settled_at;

/*
        Which primary nodes are already ours.

        Not an optimisation. Opening a node we already hold and closing it
        again is a client releasing the device as far as DRM is concerned, and
        it answers by telling every client to restore -- a full repaint of
        every screen. The poll below runs for a hundred rounds, so booting
        cost a hundred and one full composes, about two hundred milliseconds
        of drawing nobody asked for.

        One bit per minor, which is the whole of DRM's minor space.
*/
static u64 canvas_claimed;

static int canvas_claim(const char *path, unsigned int minor)
{
        struct file *filp;
        struct drm_file *file_priv;
        struct canvas *canvas;

        if (canvas_claimed & BIT_ULL(minor))
                return -EBUSY;

        filp = filp_open(path, O_RDWR, 0);
        if (IS_ERR(filp))
                return PTR_ERR(filp);

        file_priv = filp->private_data;

        if (!file_priv || !file_priv->minor || !file_priv->minor->dev)
        {
                filp_close(filp, NULL);
                return -ENODEV;
        }

        canvas = canvas_take_over(file_priv->minor->dev);

        if (canvas)
                canvas_claimed |= BIT_ULL(minor);

        /*
                Closed before the client is registered, and drm_client_init has
                taken its own reference to the device by now.

                Registering fires the hotplug that picks a mode and commits it,
                and a commit is refused out of hand while anything else is the
                device's master -- which this file is until it is closed. With
                it still open the first commit always answered EBUSY, so
                "that mode would not set, take the offered one" could never
                run: the one answer it was written to read was the one answer
                it could never get.
        */
        filp_close(filp, NULL);

        if (!canvas)
                return -EBUSY;

        drm_client_register(&canvas->client);
        log_canvas("attached to %s, %s display\n", canvas->client.dev->driver->name,
                   canvas_is_virtual(canvas->client.dev) ? "a guest's" : "a real");

        return 0;
}

/*
        Every primary node, every round. DRM allocates card minors out of an
        idr with no promise they are contiguous, and a card that probes late
        would be missed by a scan that stopped at the first gap. The whole
        minor space is cheap to try: an absent node fails in filp_open.
*/
static unsigned int canvas_claim_all(void)
{
        char path[24];
        unsigned int minor, taken = 0;

        for (minor = 0; minor < 64; minor++)
        {
                if (canvas_claimed & BIT_ULL(minor))
                        continue;

                positive_into_string(
                    memory_copy_apart_end(path, "/dev/dri/card",
                                         sizeof("/dev/dri/card") - 1),
                    minor);

                if (canvas_claim(path, minor) == 0)
                        taken++;
        }

        return taken;
}

static COLD void canvas_probe(struct work_struct *work)
{
        canvas_claim_all();
        canvas_attempts++;

        if (!canvas_settled_at && !list_empty(&canvas_list))
                canvas_settled_at = canvas_attempts + CANVAS_SETTLE;

        if (canvas_settled_at && canvas_attempts >= canvas_settled_at)
                return;

        if (canvas_attempts >= CANVAS_ATTEMPTS)
        {
                log_canvas("gave up waiting for a card\n");
                return;
        }

        schedule_delayed_work(&canvas_probe_work, msecs_to_jiffies(CANVAS_RETRY_MS));
}

static void __maybe_unused canvas_start_probing(void)
{
        INIT_DELAYED_WORK(&canvas_probe_work, canvas_probe);
        schedule_delayed_work(&canvas_probe_work, 0);
}
