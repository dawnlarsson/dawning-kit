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
        Acceleration, the way a desktop does it.

        A mouse reports counts, not pixels, and the same count means different
        things depending on how fast the hand is moving: slow is aiming and
        fast is crossing the screen. So the gain is a curve on speed rather
        than a constant. Below the floor it is exactly one to one, which is
        what makes careful movement land where it is aimed; above the ceiling
        it is flat, because past that the hand is already travelling and more
        gain only makes it hard to stop.

        Fixed point, since there is no floating point in here. Speed is counts
        per millisecond.
*/
#define ACCEL_ONE 1024
#define ACCEL_FLOOR 1
#define ACCEL_CEILING 8
#define ACCEL_MAX (ACCEL_ONE * 5 / 2)

static int pointer_gain(int speed)
{
        if (speed <= ACCEL_FLOOR)
                return ACCEL_ONE;

        if (speed >= ACCEL_CEILING)
                return ACCEL_MAX;

        return ACCEL_ONE + (ACCEL_MAX - ACCEL_ONE) * (speed - ACCEL_FLOOR) /
                               (ACCEL_CEILING - ACCEL_FLOOR);
}

static int accel_apply(int delta, int *remainder, int gain)
{
        int whole;

        if (!delta)
                return 0;

        // The remainder is what stops a gain that is not a whole number from
        // dropping the fraction of every movement: a slow drag would come up
        // short of where it was aimed.
        *remainder += delta * gain;
        whole = *remainder / ACCEL_ONE;
        *remainder -= whole * ACCEL_ONE;

        pointer_counts += abs(delta);
        pointer_moved += abs(whole);

        return whole;
}

/*
        Shake to find it.

        Reversing direction takes a real movement each time, so a slow wobble
        or a hand resting on the mouse is not a shake, and the reversals have
        to arrive inside one window or the count starts again.
*/
#define SHAKE_WINDOW_NS (700ULL * NSEC_PER_MSEC)
#define SHAKE_STEP 6
#define SHAKE_REVERSALS 5
#define CURSOR_MAGNIFIED 3
#define MAGNIFIED_NS (1200ULL * NSEC_PER_MSEC)

static void pointer_shake(int delta, u64 now)
{
        int direction = delta > 0 ? 1 : -1;

        if (abs(delta) < SHAKE_STEP)
                return;

        if (now - desktop.shake_window > SHAKE_WINDOW_NS)
        {
                desktop.shake_window = now;
                atomic_set(&desktop.shake_count, 0);
        }

        if (direction == atomic_read(&desktop.shake_dir))
                return;

        atomic_set(&desktop.shake_dir, direction);

        if (atomic_inc_return(&desktop.shake_count) >= SHAKE_REVERSALS)
        {
                atomic_set(&desktop.shake_count, 0);
                atomic_set(&desktop.magnify, 1);
        }
}

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

static void pointer_latency_record(u64 started)
{
        u64 elapsed = ktime_get_ns() - started;

        pointer_latency_total += elapsed;
        pointer_events++;

        if (elapsed > pointer_latency_worst)
                pointer_latency_worst = elapsed;
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

                        desktop.cursor_shape = cursor_shape_at(x, y);

                        if (atomic_xchg(&desktop.magnify, 0))
                        {
                                desktop.cursor_scale = CURSOR_MAGNIFIED * desktop.scale;
                                desktop.magnified_until = ktime_get_ns() + MAGNIFIED_NS;
                                desktop_watch();
                        }

                        if (desktop.dragging || desktop.resizing ||
                            desktop.barring)
                        {
                                if (desktop.dragging || desktop.resizing)
                                {
                                        /*
                                                A resize repaints the window,
                                                not the independent hardware
                                                cursor plane. Move that plane
                                                first so the pointer never
                                                waits behind composition; the
                                                integrated repaint below
                                                carries software cursors.
                                        */
                                        if (cursor_move_planes(x, y) && started)
                                        {
                                                /*
                                                        The independent plane
                                                        is on screen now. Do
                                                        not charge the window
                                                        compose behind it to
                                                        pointer latency.
                                                */
                                                pointer_latency_record(started);
                                                started = 0;
                                        }
                                }
                                else
                                {
                                        // Bar movement schedules a later
                                        // frame, so both cursor paths land
                                        // synchronously here.
                                        cursor_move(x, y);
                                }

                                if (desktop.dragging)
                                        drag_move(x, y);
                                else if (desktop.resizing)
                                        resize_move(x, y);
                                else
                                        bar_move(y);
                        }
                        else
                        {
                                cursor_move(x, y);
                        }

                        // cursor_move/reshape has painted the software
                        // fallback. Clear any old plane that resisted its
                        // explicit disable before this event is reported done.
                        cursor_plane_recover();
                }
        }

        mutex_unlock(&desktop.lock);

        if (started)
                pointer_latency_record(started);
}

static void pointer_commit(int x, int y)
{
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

/*
        One movement of the hand, once the device has said it is over.

        Measuring each axis on its own was a bug you could feel. A mouse sends
        REL_X and REL_Y back to back for one movement, so X was timed against
        the gap since the last report and Y against nothing at all -- floored
        to a millisecond, which read as several times faster and earned
        several times the gain. Left and right moved at one speed, up and down
        at another.

        So the speed is the speed of the movement, not of an axis, and both
        axes are given the same gain.
*/
static void pointer_frame(void)
{
        int dx = desktop.raw_x;
        int dy = desktop.raw_y;
        u64 now, interval;
        int gain, speed;

        desktop.raw_x = 0;
        desktop.raw_y = 0;

        if (!dx && !dy)
                return;

        now = ktime_get_ns();
        interval = now - desktop.accel_stamp;
        desktop.accel_stamp = now;

        if (interval < NSEC_PER_MSEC)
                interval = NSEC_PER_MSEC;

        speed = (int)div_u64((u64)int_sqrt((unsigned long)(dx * dx + dy * dy)) *
                                 NSEC_PER_MSEC,
                             interval);
        gain = pointer_gain(speed);

        pointer_commit(atomic_read(&desktop.pending_x) +
                           accel_apply(dx, &desktop.accel_x, gain),
                       atomic_read(&desktop.pending_y) +
                           accel_apply(dy, &desktop.accel_y, gain));
}

static void pointer_event_locked(struct input_handle *handle, unsigned int type,
                                 unsigned int code, int value)
{
        int x, y;

        x = atomic_read(&desktop.pending_x);
        y = atomic_read(&desktop.pending_y);

        if (type == EV_REL)
        {
                // Only remembered here. What it means depends on what else
                // arrives before the device says the movement is over.
                if (code == REL_X)
                {
                        desktop.raw_x += value;
                        pointer_shake(value, ktime_get_ns());
                }
                else if (code == REL_Y)
                {
                        desktop.raw_y += value;
                }
                else if (code == REL_WHEEL_HI_RES ||
                         (code == REL_WHEEL &&
                          !test_bit(REL_WHEEL_HI_RES, handle->dev->relbit)))
                {
                        // Committed here rather than at the report, because a
                        // wheel is not movement: nothing else in the report
                        // changes what it means, and holding it back only
                        // delays the line by a frame.
                        // atomic_fetch_add and not atomic_add: library.c
                        // above defines an atomic_add of its own, taking the
                        // address first, and it shadows the kernel's here.
                        atomic_fetch_add(code == REL_WHEEL_HI_RES ? value
                                                                 : value * WHEEL_V120,
                                         &desktop.wheel);
                        canvas_thread_wake();
                }

                return;
        }

        if (type == EV_SYN)
        {
                if (code != SYN_REPORT)
                        return;

                // A tablet reports both axes and then says it is done, the
                // same as a mouse. Committing each axis as it arrived moved
                // the cursor twice for one movement, the second time with the
                // other axis a report out of date.
                if (desktop.abs_have)
                {
                        pointer_commit(desktop.abs_have & 1 ? desktop.abs_x : x,
                                       desktop.abs_have & 2 ? desktop.abs_y : y);
                        desktop.abs_have = 0;
                        return;
                }

                pointer_frame();
                return;
        }

        if (type == EV_ABS)
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
                {
                        desktop.abs_x = (int)div_u64(
                            (u64)(value - abs->minimum) * (u32)desktop.width,
                            abs->maximum - abs->minimum);
                        desktop.abs_have |= 1;
                }
                else
                {
                        desktop.abs_y = (int)div_u64(
                            (u64)(value - abs->minimum) * (u32)desktop.height,
                            abs->maximum - abs->minimum);
                        desktop.abs_have |= 2;
                }

                return;
        }

        if (type == EV_KEY)
        {
                if (code != BTN_LEFT && code != BTN_TOUCH)
                {
                        keyboard_event(code, value);
                        return;
                }

                atomic_set(&desktop.button_x, x);
                atomic_set(&desktop.button_y, y);
                atomic_set(&desktop.button_down, !!value);
                atomic_set(&desktop.button_changed, 1);

                wake_up_process(canvas_thread);
                return;
        }
}

static void pointer_event(struct input_handle *handle, unsigned int type,
                          unsigned int code, int value)
{
        unsigned long flags;

        if (!READ_ONCE(desktop.width))
                return;

        spin_lock_irqsave(&desktop.input_lock, flags);
        pointer_event_locked(handle, type, code, value);
        spin_unlock_irqrestore(&desktop.input_lock, flags);
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

        log_canvas("input: %s\n", dev->name ? dev->name : "unnamed");
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

// Anything that reports motion or keys: mice, tablets, touchpads, keyboards.
static const struct input_device_id pointer_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_REL)},
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_ABS)},
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
        .evbit = {BIT_MASK(EV_KEY)},
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

// Whether there is anything to answer a frame. Nothing arms one otherwise.
static _Bool canvas_thread_running(void)
{
        return canvas_thread != NULL;
}

static int canvas_loop(void *unused)
{
        while (!kthread_should_stop())
        {
                set_current_state(TASK_IDLE);

                if (!atomic_read(&desktop.motion_pending) &&
                    !atomic_read(&desktop.button_changed) &&
                    !atomic_read(&desktop.frame_pending) &&
                    !atomic_read(&desktop.wheel) &&
                    !atomic_read(&desktop.focus_steps) &&
                    !atomic_read(&desktop.focus_commit) &&
                    !atomic_read(&desktop.minimize) &&
                    atomic_read(&desktop.key_head) == atomic_read(&desktop.key_tail))
                        schedule();

                __set_current_state(TASK_RUNNING);

                pointer_apply();

                if (atomic_read(&desktop.focus_steps) ||
                    atomic_read(&desktop.focus_commit) ||
                    atomic_read(&desktop.minimize))
                {
                        unsigned int steps =
                            (unsigned int)atomic_xchg(&desktop.focus_steps, 0);
                        _Bool commit = atomic_xchg(&desktop.focus_commit, 0);
                        _Bool minimize = atomic_xchg(&desktop.minimize, 0);

                        mutex_lock(&desktop.lock);
                        while (steps--)
                                pane_focus_step();
                        if (minimize)
                                pane_minimize_focused();
                        if (commit)
                                pane_focus_commit();
                        mutex_unlock(&desktop.lock);
                }

                if (atomic_read(&desktop.key_head) != atomic_read(&desktop.key_tail))
                {
                        mutex_lock(&desktop.lock);
                        keys_deliver();
                        mutex_unlock(&desktop.lock);
                }

                // On this thread because it walks the window list and writes
                // cells, neither of which an input callback may do.
                if (atomic_read(&desktop.wheel))
                {
                        mutex_lock(&desktop.lock);
                        wheel_deliver();
                        mutex_unlock(&desktop.lock);
                }

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
static void canvas_input_stats(struct input_stats *out)
{
        unsigned long n = pointer_events ? pointer_events : 1;

        out->events = pointer_events;
        out->mean_ns = pointer_latency_total / n;
        out->worst_ns = pointer_latency_worst;
        out->queue_ns = pointer_queue_total / n;
        out->draw_ns = pointer_draw_total / n;
        out->flush_ns = pointer_flush_total / n;
        out->counts = pointer_counts;
        out->moved = pointer_moved;
        out->composes = canvas_composes;
        out->compose_ns = canvas_compose_ns;
        out->painted = canvas_painted;
        out->runs = canvas_runs;
        out->driver_ns = canvas_flush_ns;
        out->text_ns = canvas_text_ns;
}

static void canvas_cursor_stats(struct cursor_stats *out)
{
        struct output *output;

        memory_fill(out, 0, sizeof(*out));
        mutex_lock(&desktop.lock);

        out->requested_generation = cursor_plane_requested_generation;
        out->armed_generation = cursor_plane_armed_generation;
        out->updates = (unsigned long)atomic_long_read(&cursor_plane_updates);
        out->failures = (unsigned long)atomic_long_read(&cursor_plane_failures);
        out->requested_x = cursor_plane_requested_x;
        out->requested_y = cursor_plane_requested_y;
        out->armed_x = cursor_plane_armed_x;
        out->armed_y = cursor_plane_armed_y;

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y))
                        out->wanted++;

                if (output->cursor_plane)
                {
                        out->active++;

                        if (output->cursor_shown)
                                out->shown++;
                }
        }

        out->recovering = cursor_plane_recovery;

        mutex_unlock(&desktop.lock);
}
