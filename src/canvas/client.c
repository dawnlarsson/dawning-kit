/*
        Canvas -- attaching to DRM

        Taking the device, the client callbacks DRM calls back on, and the
        poll that waits for /dev/dri/card0 to exist.

        The retry loop is here because the device node appears when devtmpfs
        is mounted, which is after the initcalls that could otherwise have
        started this.
*/

static void client_unregister(struct drm_client_dev *client)
{
        struct canvas *canvas = canvas_from_client(client);

        /*
                Order matters. The input handler and its worker reach the
                display through canvas_active, so that has to stop
                pointing at it before anything it owns is freed, and the
                worker has to be known finished rather than merely asked to
                stop. Getting this wrong is a use after free on every mouse
                move after a device goes away.
        */
        if (canvas_active == canvas) {
                pointer_stop();
                canvas_active = NULL;
        }

        mutex_lock(&canvas->lock);
        canvas->started = 0;
        canvas_release(canvas);
        mutex_unlock(&canvas->lock);

        drm_client_release(client);
}

static void client_free(struct drm_client_dev *client)
{
        struct canvas *canvas = canvas_from_client(client);

        mutex_destroy(&canvas->lock);
        kfree(canvas);
}

static int client_hotplug(struct drm_client_dev *client)
{
        struct canvas *canvas = canvas_from_client(client);
        int ret = 0;

        mutex_lock(&canvas->lock);

        if (!canvas->started) {
                ret = canvas_start(canvas);
                canvas->started = (ret == 0);
        } else {
                canvas_redraw(canvas);
        }

        mutex_unlock(&canvas->lock);
        return ret;
}

// The bool argument is whether the restore happens from an atomic context.
static int client_restore(struct drm_client_dev *client, _Bool in_atomic)
{
        struct canvas *canvas = canvas_from_client(client);

        // Nothing here is safe to do without sleeping, so an atomic restore
        // is declined rather than half performed.
        if (in_atomic)
                return -EBUSY;

        mutex_lock(&canvas->lock);
        if (canvas->started)
                canvas_redraw(canvas);
        mutex_unlock(&canvas->lock);

        return 0;
}

static const struct drm_client_funcs client_funcs = {
    .owner = THIS_MODULE,
    .unregister = client_unregister,
    .free = client_free,
    .restore = client_restore,
    .hotplug = client_hotplug,
};

/*
        Claims a DRM device. Reports whether it was taken, so the replacement
        for drm_client_setup can fall through to the original for anything we
        decline -- a device with no modesetting, or an allocation that failed.
*/
static int canvas_take_over(struct drm_device *dev)
{
        struct canvas *canvas;

        if (!drm_core_check_feature(dev, DRIVER_MODESET))
                return 0;

        /*
                One display, once. A machine can easily present two cards --
                the first boot on real hardware had virtio-gpu and a standard
                VGA both probing -- and taking the second would register the
                input handler twice, leak the first workqueue, and leave
                canvas_active pointing at whichever won the race.
        */
        if (canvas_active)
                return 0;

        canvas = kzalloc(sizeof(*canvas), GFP_KERNEL);
        if (!canvas)
                return 0;

        mutex_init(&canvas->lock);

        if (drm_client_init(dev, &canvas->client, "moonwater", &client_funcs)) {
                mutex_destroy(&canvas->lock);
                kfree(canvas);
                return 0;
        }

        drm_client_register(&canvas->client);
        log_canvas("attached to %s\n", dev->driver->name);

        pointer_start();
        return 1;
}

/*
        Finding a device to draw on.

        Opening the node runs the driver's open path and hands back a
        drm_file, which knows its minor, which knows its device. The file is
        closed immediately: drm_client_init takes its own reference, so the
        device outlives our brief handle on it.
*/
#define CANVAS_NODE "/dev/dri/card0"
// Retried fast: the node appears the moment devtmpfs is mounted, and every
// millisecond spent waiting after that is a millisecond of black screen.
#define CANVAS_RETRY_MS 5
#define CANVAS_ATTEMPTS 1000

static struct delayed_work canvas_probe_work;
static unsigned int canvas_attempts;

static int canvas_claim(const char *path)
{
        struct file *filp;
        struct drm_file *file_priv;
        struct drm_device *dev;
        int taken;

        filp = filp_open(path, O_RDWR, 0);
        if (IS_ERR(filp))
                return PTR_ERR(filp);

        file_priv = filp->private_data;

        if (!file_priv || !file_priv->minor || !file_priv->minor->dev) {
                filp_close(filp, NULL);
                return -ENODEV;
        }

        dev = file_priv->minor->dev;
        taken = canvas_take_over(dev);

        filp_close(filp, NULL);

        return taken ? 0 : -EBUSY;
}

static void canvas_probe(struct work_struct *work)
{
        int ret = canvas_claim(CANVAS_NODE);

        if (ret == 0)
                return;

        if (++canvas_attempts >= CANVAS_ATTEMPTS) {
                log_canvas("gave up waiting for %s (%d)\n", CANVAS_NODE, ret);
                return;
        }

        schedule_delayed_work(&canvas_probe_work,
                              msecs_to_jiffies(CANVAS_RETRY_MS));
}

static void canvas_start_probing(void)
{
        INIT_DELAYED_WORK(&canvas_probe_work, canvas_probe);

        // No initial delay. The display driver has already probed by the time
        // this runs, so the first attempt usually succeeds outright.
        schedule_delayed_work(&canvas_probe_work, 0);
}
