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

/*
        Input to cursor

        input_handler.event is called by the input core in the path that
        received the event, so nothing is woken to notice a mouse move. The
        position is updated there and then; the pixels cannot be, because that
        context cannot sleep and a DRM commit can. A high priority worker does
        that part, and the gap between the two is what gets measured.
*/
static struct task_struct *pointer_thread;
static void pointer_apply(void);

/*
        How long the processor is allowed to take waking up.

        The largest cost in the path from a pointer moving to the thread that
        draws it is the processor coming back from an idle state, and the
        deeper the state the longer that takes -- a cursor moving after a pause
        is exactly the case that pays it.

        cpu_latency_qos is how a driver says so. It is the generic interface
        cpuidle governors already honour, so it holds on every architecture
        with cpuidle rather than only where a boot argument exists: x86 has
        intel_idle.max_cstate, arm64 has cpuidle-psci and no such argument,
        and this reaches both.

        Held for as long as there is a pointer, rather than taken around each
        movement. Taking it on the way in would be exactly backwards: the move
        that pays the wakeup is the first one after a pause, and at that moment
        the request has not been made yet.
*/
static struct pm_qos_request pointer_qos;

// Nanoseconds from the event arriving to the cursor being on screen.
// Declared with the rest of the timing counters near the top of the file.

static void pointer_apply(void)
{
        struct canvas *canvas = canvas_active;
        int x, y;
        u64 started;

        if (!canvas)
                return;

        if (!atomic_xchg(&canvas->motion_pending, 0))
                return;

        started = canvas->motion_stamp;
        x = atomic_read(&canvas->pending_x);
        y = atomic_read(&canvas->pending_y);

        if (started)
                pointer_queue_total += ktime_get_ns() - started;

        mutex_lock(&canvas->lock);

        if (canvas->started && canvas->surface_count) {
                canvas_move_cursor(canvas, &canvas->surfaces[0], x, y);
                canvas->drawn_x = x;
                canvas->drawn_y = y;
                canvas->cursor_x = x;
                canvas->cursor_y = y;
        }

        mutex_unlock(&canvas->lock);

        if (started) {
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
        struct canvas *canvas = canvas_active;
        int x, y, limit;

        if (!canvas || !canvas->started || !canvas->screen_w)
                return;

        x = atomic_read(&canvas->pending_x);
        y = atomic_read(&canvas->pending_y);

        if (type == EV_REL) {
                if (code == REL_X)
                        x += value;
                else if (code == REL_Y)
                        y += value;
                else
                        return;
        } else if (type == EV_ABS) {
                // Absolute devices report in their own range, so scale into
                // the screen. QEMU's tablet is one of these.
                struct input_absinfo *abs;

                if (code != ABS_X && code != ABS_Y)
                        return;

                abs = &handle->dev->absinfo[code];

                if (abs->maximum <= abs->minimum)
                        return;

                if (code == ABS_X)
                        x = (int)div_u64((u64)(value - abs->minimum) *
                                         (u32)canvas->screen_w,
                                         abs->maximum - abs->minimum);
                else
                        y = (int)div_u64((u64)(value - abs->minimum) *
                                         (u32)canvas->screen_h,
                                         abs->maximum - abs->minimum);
        } else {
                return;
        }

        limit = canvas->screen_w - 1;
        x = clamp(x, 0, limit);
        limit = canvas->screen_h - 1;
        y = clamp(y, 0, limit);

        atomic_set(&canvas->pending_x, x);
        atomic_set(&canvas->pending_y, y);

        // Stamp only the first event of a burst, so the measurement is the age
        // of the oldest movement not yet on screen.
        if (!atomic_xchg(&canvas->motion_pending, 1))
                canvas->motion_stamp = ktime_get_ns();

        /*
                A wake, not a queue. The work this does is one atomic commit
                that returns without waiting, so the machinery a workqueue
                brings -- a pool, a dispatch, a kworker that is still an
                ordinary task -- is all overhead around it. A thread of our
                own, at a real time priority, is woken and runs.
        */
        wake_up_process(pointer_thread);
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

/*
        Unregistering the handler stops new events; cancelling the work waits
        for the one that may already be running. Both have to happen before
        the display it draws through is freed.
*/
static void pointer_stop(void)
{
        if (!pointer_thread)
                return;

        input_unregister_handler(&pointer_handler);
        cpu_latency_qos_remove_request(&pointer_qos);
        kthread_stop(pointer_thread);
        pointer_thread = NULL;
}

/*
        Sleeps until something moves, then draws it.

        set_current_state before the flag is read, which is what makes the
        sleep safe: a wake arriving between the two finds the task already
        marked and schedule() returns at once rather than losing the event.
*/
static int pointer_loop(void *unused)
{
        while (!kthread_should_stop()) {
                set_current_state(TASK_IDLE);

                if (!canvas_active ||
                    !atomic_read(&canvas_active->motion_pending))
                        schedule();

                __set_current_state(TASK_RUNNING);
                pointer_apply();
        }

        return 0;
}

static void pointer_start(void)
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
        pointer_thread = kthread_run(pointer_loop, NULL, "moonwater/pointer");

        if (IS_ERR(pointer_thread)) {
                log_canvas("no thread for input\n");
                pointer_thread = NULL;
                return;
        }

        sched_set_fifo_low(pointer_thread);

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
