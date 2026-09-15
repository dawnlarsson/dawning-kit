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

        rt_mutex_lock(&desktop.lock);
        canvas->started = 0;
        canvas_release(canvas);
        rt_mutex_unlock(&desktop.lock);

        // An output dropped while its buffer was with the flusher is freed by
        // the flusher, and that buffer is this client's.
        wait_event(desktop.flush_idle, !atomic_read(&canvas->retiring));

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

        rt_mutex_lock(&desktop.lock);

        if (!canvas->started)
        {
                ret = canvas_start(canvas);
                canvas->started = (ret == 0);
        }
        else
        {
                ret = canvas_rebind(canvas);
        }

        rt_mutex_unlock(&desktop.lock);
        return ret;
}

// The bool argument is whether the restore happens from an atomic context.
static int client_restore(struct drm_client_dev *client, _Bool in_atomic)
{
        struct canvas *canvas = canvas_from_client(client);

        if (in_atomic)
                return -EBUSY;

        rt_mutex_lock(&desktop.lock);
        if (canvas->started)
        {
                // The last program that held the card has closed it.
                if (desktop.suspended)
                        desktop_resume();
                else
                        desktop_redraw();
        }
        rt_mutex_unlock(&desktop.lock);

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
                        pr_info("[moonwater canvas] " "no console font to draw with\n");
                        return NULL;
                }

                canvas_terminal_prepare();

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

/*
        What turning Canvas on needs from a card before it is claimed.
*/
#include <drm/drm_auth.h>
#include <drm/drm_file.h>

/* Every in-kernel client on a card but Canvas's, as drm_client_dev_unregister takes them. */
static void canvas_clients_clear(struct drm_device *dev)
{
        struct drm_client_dev *client, *next;

        mutex_lock(&dev->clientlist_mutex);
        list_for_each_entry_safe(client, next, &dev->clientlist, list)
        {
                if (client->funcs == &client_funcs)
                        continue;

                // Unregistering consumes and frees the client.
                list_del(&client->list);
                if (client->funcs && client->funcs->unregister)
                        client->funcs->unregister(client);
                else
                        drm_client_release(client);
        }
        mutex_unlock(&dev->clientlist_mutex);
}

/* Which program is master of a card, for a refusal to say. */
static int canvas_master_holder(struct drm_device *dev, char *command, size_t room)
{
        struct drm_file *file;
        int holder = 0;

        mutex_lock(&dev->filelist_mutex);
        list_for_each_entry(file, &dev->filelist, lhead)
        {
                struct task_struct *task;

                if (!drm_is_current_master(file))
                        continue;

                rcu_read_lock();
                task = pid_task(rcu_dereference(file->pid), PIDTYPE_TGID);
                if (task)
                {
                        holder = task_tgid_nr(task);
                        strscpy(command, task->comm, room);
                }
                rcu_read_unlock();
                break;
        }
        mutex_unlock(&dev->filelist_mutex);

        return holder;
}

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

static int canvas_claim(const char *path, unsigned int minor,
                        struct canvas_control *on)
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

        /*
                Turned on from userspace, a card is taken only from nobody.

                This open is the card's master unless another program already
                is, and a Canvas started behind that program would only sit
                suspended, so the refusal names it instead. The kernel
                console's client, which off left on the card, goes while this
                file is still the master, so the close below restores nothing.
        */
        if (on && drm_core_check_feature(file_priv->minor->dev, DRIVER_MODESET))
        {
                if (!drm_is_current_master(file_priv))
                {
                        if (!on->master_pid && !on->master_command[0])
                                on->master_pid = canvas_master_holder(
                                    file_priv->minor->dev, on->master_command,
                                    sizeof(on->master_command));
                        filp_close(filp, NULL);
                        return -EACCES;
                }

                canvas_clients_clear(file_priv->minor->dev);
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
        pr_info("[moonwater canvas] " "attached to %s, %s display\n", canvas->client.dev->driver->name, canvas_is_virtual(canvas->client.dev) ? "a guest's" : "a real");

        return 0;
}

/*
        Every primary node, every round. DRM allocates card minors out of an
        idr with no promise they are contiguous, and a card that probes late
        would be missed by a scan that stopped at the first gap. The whole
        minor space is cheap to try: an absent node fails in filp_open.
*/
static unsigned int canvas_claim_all(struct canvas_control *on,
                                     unsigned int *refused)
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

                switch (canvas_claim(path, minor, on))
                {
                case 0:
                        taken++;
                        break;
                case -EACCES:
                        if (refused)
                                (*refused)++;
                        break;
                }
        }

        return taken;
}

static COLD void canvas_probe(struct work_struct *work)
{
        canvas_claim_all(NULL, NULL);
        canvas_attempts++;

        if (!canvas_settled_at && !list_empty(&canvas_list))
                canvas_settled_at = canvas_attempts + CANVAS_SETTLE;

        if (canvas_settled_at && canvas_attempts >= canvas_settled_at)
                return;

        if (canvas_attempts >= CANVAS_ATTEMPTS)
        {
                pr_info("[moonwater canvas] " "gave up waiting for a card\n");
                return;
        }

        schedule_delayed_work(&canvas_probe_work, msecs_to_jiffies(CANVAS_RETRY_MS));
}

static void __maybe_unused canvas_start_probing(void)
{
        INIT_DELAYED_WORK(&canvas_probe_work, canvas_probe);
        schedule_delayed_work(&canvas_probe_work, 0);
}

/*
        Canvas, off and on, from userspace.

        Off undoes what attaching did, in this order: printk stops reaching
        cells; the input handler and the thread go, which gives every console
        its keyboard back; every program's window is asked to close and taken
        off the desktop; each card's client is released; and the kernel's own
        framebuffer console is set up on each card, which
        drm_client_lib.active= kept from ever having one. On claims the cards
        again the way the boot does, and canvas_start opens the kernel log
        and a terminal as it does at boot.

        Lock order: canvas_control_lock, then a card's clientlist_mutex, then
        canvas_list_lock, then desktop.lock, then a card's master_mutex.
        canvas_thread_stop joins a thread that takes desktop.lock, so it is
        called under canvas_list_lock and never under desktop.lock.
*/
#ifndef MODULE
#include <../drivers/gpu/drm/clients/drm_client_internal.h>
#endif

static DEFINE_MUTEX(canvas_control_lock);

static _Bool canvas_is_on(void)
{
        _Bool on;

        mutex_lock(&canvas_list_lock);
        on = !list_empty(&canvas_list);
        mutex_unlock(&canvas_list_lock);

        return on;
}

/* The desktop's own pointers into a pane, as pane_free clears them. */
static void pane_forget(struct pane *pane)
{
        struct pane **held[] = {
                &desktop.dragging, &desktop.resizing, &desktop.barring,
                &desktop.press_pane, &desktop.focused,
        };

        for (unsigned int i = 0; i < ARRAY_SIZE(held); i++)
                if (*held[i] == pane)
                        *held[i] = NULL;
}

/*
        Every program's window asked to close and taken off the desktop.

        Nothing is freed: a pane goes when its program closes the file, and
        a program still writing to its pages keeps them. One that never
        closes stays on the detached list, drawn by nothing. Under
        desktop.lock.
*/
static void desktop_detach_windows(void)
{
        struct pane *pane, *next;

        list_for_each_entry_safe(pane, next, &desktop.windows, link)
        {
                if (!pane->shared)
                        continue;

                pane_close_request(pane);
                pane_forget(pane);
                list_move_tail(&pane->link, &desktop.detached);
        }
}

/*
        One card's client off DRM's list and released, unless the card is
        going away at this moment and its own unregister already has it.
*/
static void canvas_client_drop(struct canvas *canvas, struct drm_device *dev)
{
        struct drm_client_dev *client;

        mutex_lock(&dev->clientlist_mutex);
        list_for_each_entry(client, &dev->clientlist, list)
        {
                if (client == &canvas->client)
                {
                        list_del(&client->list);
                        client_unregister(client);
                        break;
                }
        }
        mutex_unlock(&dev->clientlist_mutex);
}

static long canvas_turn_off(void)
{
        struct drm_device *released[8];
        unsigned int count = 0;

        mutex_lock(&canvas_control_lock);

        if (!canvas_is_on())
        {
                mutex_unlock(&canvas_control_lock);
                return -EALREADY;
        }

#ifdef CONFIG_MOONWATER_CANVAS_AUTOSTART
        cancel_delayed_work_sync(&canvas_probe_work);
#endif

        // printk first: nothing may write to the log's cells once they go.
        console_stop();

        // Input and the thread before any window or output goes, so no key
        // lands in a pane being detached and nothing composes against an
        // output being released.
        mutex_lock(&canvas_list_lock);
        canvas_thread_stop();
        mutex_unlock(&canvas_list_lock);

        rt_mutex_lock(&desktop.lock);
        desktop.off = true;
        desktop_detach_windows();
        desktop.terminal = false;
        desktop.suspended = false;
        rt_mutex_unlock(&desktop.lock);

        put_pid(xchg(&canvas_spawned, NULL));

        for (;;)
        {
                struct canvas *canvas;
                struct drm_device *dev = NULL;

                mutex_lock(&canvas_list_lock);
                canvas = list_first_entry_or_null(&canvas_list, struct canvas, link);
                if (canvas)
                {
                        dev = canvas->client.dev;
                        drm_dev_get(dev);
                }
                mutex_unlock(&canvas_list_lock);

                if (!canvas)
                        break;

                canvas_client_drop(canvas, dev);

                if (count < ARRAY_SIZE(released))
                        released[count++] = dev;
                else
                        drm_dev_put(dev);
        }

        canvas_claimed = 0;

        // The kernel's console takes each screen back.
        for (unsigned int i = 0; i < count; i++)
        {
#ifndef MODULE
                if (released[i]->registered && !released[i]->fb_helper)
                        drm_fbdev_client_setup(released[i], NULL);
#endif
                drm_dev_put(released[i]);
        }

        pr_info("[moonwater canvas] " "off: %u card(s) given back to the console\n", count);

        mutex_unlock(&canvas_control_lock);
        return 0;
}

static long canvas_turn_on(struct canvas_control *answer)
{
        unsigned int taken, refused = 0;

        mutex_lock(&canvas_control_lock);

        if (canvas_is_on())
        {
                mutex_unlock(&canvas_control_lock);
                return -EALREADY;
        }

#ifdef CONFIG_MOONWATER_CANVAS_AUTOSTART
        cancel_delayed_work_sync(&canvas_probe_work);
#endif

        rt_mutex_lock(&desktop.lock);
        desktop.off = false;
        rt_mutex_unlock(&desktop.lock);

        taken = canvas_claim_all(answer, &refused);

        mutex_unlock(&canvas_control_lock);

        if (taken)
                return 0;

        return refused ? -EBUSY : -ENODEV;
}

/* What Canvas holds, for `moonwater canvas`. */
static void canvas_state(struct canvas_control *answer)
{
        struct canvas *canvas;
        struct output *output;
        struct pane *pane;

        mutex_lock(&canvas_list_lock);
        list_for_each_entry(canvas, &canvas_list, link)
                if (!answer->cards++)
                        strscpy(answer->driver, canvas->client.dev->driver->name,
                                sizeof(answer->driver));
        mutex_unlock(&canvas_list_lock);

        answer->running = answer->cards != 0;

        rt_mutex_lock(&desktop.lock);

        answer->suspended = desktop.suspended;

        list_for_each_entry(pane, &desktop.windows, link)
                if (pane->shared)
                        answer->windows++;

        list_for_each_entry(pane, &desktop.detached, link)
                answer->detached++;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                struct canvas_output_state *state;
                struct drm_mode_set *set = output->mode_set;

                if (answer->output_count == SPARK_CANVAS_OUTPUTS)
                        break;

                state = &answer->output[answer->output_count++];
                state->width = output->width;
                state->height = output->height;

                if (set && set->mode)
                        state->refresh = drm_mode_vrefresh(set->mode);
                if (set && set->num_connectors && set->connectors[0])
                        strscpy(state->connector, set->connectors[0]->name,
                                sizeof(state->connector));
        }

        rt_mutex_unlock(&desktop.lock);
}

