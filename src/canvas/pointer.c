/*
        Canvas -- input

        This is the reason for putting the compositor in the kernel at all. A
        userspace display server sees a mouse move as: interrupt, input core,
        wake the server, the server reads the event, composites, and asks the
        kernel to move the cursor. Every one of those arrows is a context
        switch.

        The handler below is called by the input core directly, in the same
        path that received the event. There is one handoff left and it is
        unavoidable: input events arrive in atomic context and a DRM commit
        can sleep, so the position is taken immediately and a SCHED_FIFO
        thread applies it.
*/

static struct task_struct *canvas_thread;

/*
        How long the processor is allowed to take waking up.

        The largest cost from a pointer moving to the thread that draws it is
        the processor coming back from an idle state, and the deeper the state
        the longer that takes. Held for as long as there is a pointer rather
        than taken per movement: the move that pays the wakeup is the first
        after a pause, and at that moment the request would not exist yet.
*/
static struct pm_qos_request pointer_qos;

/*
        Keeps the cursor on a screen. The desktop is the bounding box of the
        outputs, so with screens of different heights it has corners no crtc
        scans out; a cursor left there would vanish.
*/
static void desktop_confine_cursor(int *x, int *y)
{
        struct output *output, *nearest = NULL;
        int best = INT_MAX;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                int dx = clamp(*x, output->x, output->x + (int)output->width - 1) - *x;
                int dy = clamp(*y, output->y, output->y + (int)output->height - 1) - *y;
                int distance = abs(dx) + abs(dy);

                if (output_holds(output, *x, *y))
                        return;

                if (distance < best)
                {
                        best = distance;
                        nearest = output;
                }
        }

        if (!nearest)
                return;

        *x = clamp(*x, nearest->x, nearest->x + (int)nearest->width - 1);
        *y = clamp(*y, nearest->y, nearest->y + (int)nearest->height - 1);
}

static void pointer_apply(void)
{
        _Bool button = atomic_xchg(&desktop.button_changed, 0);
        _Bool motion = atomic_xchg(&desktop.motion_pending, 0);
        int x, y;
        u64 started;

        if (!button && !motion)
                return;

        started = motion ? desktop.motion_stamp : 0;
        x = atomic_read(&desktop.pending_x);
        y = atomic_read(&desktop.pending_y);

        if (started)
                pointer_queue_total += ktime_get_ns() - started;

        mutex_lock(&desktop.lock);

        if (!list_empty(&desktop.outputs))
        {
                if (button)
                {
                        if (atomic_read(&desktop.button_down))
                                drag_press(atomic_read(&desktop.button_x),
                                           atomic_read(&desktop.button_y));
                        else
                                drag_release();
                }

                if (motion)
                {
                        desktop_confine_cursor(&x, &y);
                        atomic_set(&desktop.pending_x, x);
                        atomic_set(&desktop.pending_y, y);

                        if (desktop.dragging)
                        {
                                desktop.cursor_x = x;
                                desktop.cursor_y = y;
                                drag_move(x, y);
                        }
                        else
                        {
                                cursor_move(x, y);
                        }
                }
        }

        mutex_unlock(&desktop.lock);

        if (started)
        {
                u64 elapsed = ktime_get_ns() - started;

                pointer_latency_total += elapsed;
                pointer_events++;

                if (elapsed > pointer_latency_worst)
                        pointer_latency_worst = elapsed;
        }
}

static void pointer_event(struct input_handle *handle, unsigned int type,
                          unsigned int code, int value)
{
        int x, y;

        if (!desktop.width)
                return;

        x = atomic_read(&desktop.pending_x);
        y = atomic_read(&desktop.pending_y);

        if (type == EV_REL)
        {
                if (code == REL_X)
                        x += value;
                else if (code == REL_Y)
                        y += value;
                else
                        return;
        }
        else if (type == EV_ABS)
        {
                // Absolute devices report in their own range, so scale into
                // the desktop. QEMU's tablet is one of these.
                struct input_absinfo *abs;

                if (code != ABS_X && code != ABS_Y)
                        return;

                abs = &handle->dev->absinfo[code];

                if (abs->maximum <= abs->minimum)
                        return;

                if (code == ABS_X)
                        x = (int)div_u64((u64)(value - abs->minimum) * (u32)desktop.width,
                                         abs->maximum - abs->minimum);
                else
                        y = (int)div_u64((u64)(value - abs->minimum) * (u32)desktop.height,
                                         abs->maximum - abs->minimum);
        }
        else if (type == EV_KEY)
        {
                if (code != BTN_LEFT && code != BTN_TOUCH)
                        return;

                atomic_set(&desktop.button_x, x);
                atomic_set(&desktop.button_y, y);
                atomic_set(&desktop.button_down, !!value);
                atomic_set(&desktop.button_changed, 1);

                wake_up_process(canvas_thread);
                return;
        }
        else
        {
                return;
        }

        atomic_set(&desktop.pending_x, clamp(x, 0, desktop.width - 1));
        atomic_set(&desktop.pending_y, clamp(y, 0, desktop.height - 1));

        // Stamp only the first event of a burst, so the measurement is the age
        // of the oldest movement not yet on screen.
        if (!atomic_xchg(&desktop.motion_pending, 1))
                desktop.motion_stamp = ktime_get_ns();

        /*
                A wake, not a queue. The work is one atomic commit that returns
                without waiting, so a workqueue's pool and dispatch and kworker
                would all be overhead around it.
        */
        wake_up_process(canvas_thread);
}

static int pointer_connect(struct input_handler *handler, struct input_dev *dev,
                           const struct input_device_id *id)
{
        struct input_handle *handle;
        int ret;

        handle = kzalloc(sizeof(*handle), GFP_KERNEL);
        if (!handle)
                return -ENOMEM;

        handle->dev = dev;
        handle->handler = handler;
        handle->name = "moonwater";

        ret = input_register_handle(handle);
        if (ret)
                goto err_free;

        ret = input_open_device(handle);
        if (ret)
                goto err_unregister;

        log_canvas("pointer: %s\n", dev->name ? dev->name : "unnamed");
        return 0;

err_unregister:
        input_unregister_handle(handle);
err_free:
        kfree(handle);
        return ret;
}

static void pointer_disconnect(struct input_handle *handle)
{
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(handle);
}

// Anything that reports relative or absolute motion: mice, tablets, touchpads.
static const struct input_device_id pointer_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_REL)},
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_ABS)},
    },
    {},
};

static struct input_handler pointer_handler = {
    .event = pointer_event,
    .connect = pointer_connect,
    .disconnect = pointer_disconnect,
    .name = "moonwater",
    .id_table = pointer_ids,
};

static void canvas_thread_stop(void)
{
        if (!canvas_thread)
                return;

        input_unregister_handler(&pointer_handler);
        cpu_latency_qos_remove_request(&pointer_qos);

        desktop.awake = false;
        hrtimer_cancel(&desktop.frame);

        kthread_stop(canvas_thread);
        canvas_thread = NULL;
}

/*
        Sleeps until something moves, then draws it.

        set_current_state before the flag is read, which is what makes the
        sleep safe: a wake arriving between the two finds the task already
        marked and schedule() returns at once rather than losing the event.
*/
static void canvas_thread_wake(void)
{
        if (canvas_thread)
                wake_up_process(canvas_thread);
}

static int canvas_loop(void *unused)
{
        while (!kthread_should_stop())
        {
                set_current_state(TASK_IDLE);

                if (!atomic_read(&desktop.motion_pending) &&
                    !atomic_read(&desktop.button_changed) &&
                    !atomic_read(&desktop.frame_pending))
                        schedule();

                __set_current_state(TASK_RUNNING);

                pointer_apply();

                if (atomic_xchg(&desktop.frame_pending, 0))
                        desktop_frame_pass();
        }

        return 0;
}

static void canvas_thread_start(void)
{
        /*
                A thread rather than a workqueue.

                WQ_HIGHPRI raises a kworker's nice level and leaves it an
                ordinary task, so it is still scheduled against everything
                else running. SCHED_FIFO is a different queue entirely: the
                scheduler picks it before any normal task, which is the whole
                of what this thread is for.

                fifo_low rather than fifo: priority 1 is ahead of every
                SCHED_OTHER task and behind anything the machine considers
                more urgent than a cursor, which is the honest place for it.
        */
        hrtimer_setup(&desktop.frame, desktop_frame, CLOCK_MONOTONIC, HRTIMER_MODE_REL);

        canvas_thread = kthread_run(canvas_loop, NULL, "moonwater/canvas");

        if (IS_ERR(canvas_thread))
        {
                log_canvas("no thread for input\n");
                canvas_thread = NULL;
                return;
        }

        sched_set_fifo_low(canvas_thread);

        // 0 microseconds: no idle state whose exit can be measured.
        cpu_latency_qos_add_request(&pointer_qos, 0);

        if (input_register_handler(&pointer_handler))
                log_canvas("could not register the input handler\n");
}

// Nanoseconds, for the stats ioctl.
static void canvas_input_stats(unsigned long *events, unsigned long *mean,
                               unsigned long *worst, unsigned long *queue,
                               unsigned long *draw, unsigned long *flush)
{
        unsigned long n = pointer_events ? pointer_events : 1;

        *events = pointer_events;
        *mean = pointer_latency_total / n;
        *worst = pointer_latency_worst;
        *queue = pointer_queue_total / n;
        *draw = pointer_draw_total / n;
        *flush = pointer_flush_total / n;
}
