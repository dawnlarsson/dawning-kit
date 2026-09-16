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

static struct task_struct __rcu *canvas_thread;
static struct task_struct __rcu *canvas_flusher;
static _Bool pointer_handler_registered;

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

static CONST int pointer_gain(int speed)
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
        s64 scaled;
        int whole;

        if (!delta)
                return 0;

        // The remainder is what stops a gain that is not a whole number from
        // dropping the fraction of every movement: a slow drag would come up
        // short of where it was aimed.
        scaled = (s64)delta * gain + *remainder;
        whole = (int)clamp_t(s64, scaled / ACCEL_ONE, INT_MIN, INT_MAX);
        *remainder = (int)(scaled % ACCEL_ONE);

        pointer_counts += delta < 0 ? -(s64)delta : delta;
        pointer_moved += whole < 0 ? -(s64)whole : whole;

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

static void pointer_shake(int delta)
{
        int direction;
        u64 now;

        if (delta > -SHAKE_STEP && delta < SHAKE_STEP)
                return;

        direction = delta > 0 ? 1 : -1;
        now = ktime_get_ns();

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

                if (!dx && !dy)
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
        _Bool client = atomic_xchg(&desktop.client_changed, 0);
        _Bool motion = atomic_xchg(&desktop.motion_pending, 0);
        int x, y;
        u64 started;

        if (!button && !client && !motion)
                return;

        started = motion ? desktop.motion_stamp : 0;
        x = atomic_read(&desktop.pending_x);
        y = atomic_read(&desktop.pending_y);

        if (started)
                pointer_queue_total += ktime_get_ns() - started;

        rt_mutex_lock(&desktop.lock);

        if (!list_empty(&desktop.outputs))
        {
                if (button)
                {
                        if (atomic_read(&desktop.button_down))
                                drag_press(atomic_read(&desktop.button_x),
                                           atomic_read(&desktop.button_y));
                        else
                                drag_release(atomic_read(&desktop.button_x),
                                             atomic_read(&desktop.button_y));
                }

                if (client)
                {
                        struct pane *pane = desktop.focused;
                        unsigned int flags = atomic_read(&desktop.client_down)
                                                 ? WINDOW_KEY_DOWN
                                                 : 0;

                        if (pane)
                                pointer_report(pane,
                                               atomic_read(&desktop.button_x),
                                               atomic_read(&desktop.button_y),
                                               (unsigned int)atomic_read(
                                                   &desktop.client_button),
                                               flags);
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
                                        struct pane *pane = desktop.barring;
                                        struct pane_bar_geometry bar;

                                        // Bar movement schedules a later
                                        // frame, so both cursor paths land
                                        // synchronously here.
                                        cursor_move(x, y);

                                        if (pane && pane_bar(pane, &bar))
                                                bar_move(pane, y, &bar);
                                }

                                if (desktop.dragging)
                                        drag_move(x, y);
                                else if (desktop.resizing)
                                        resize_move(x, y);
                        }
                        else
                        {
                                cursor_move(x, y);
                                if (desktop.focused)
                                {
                                        unsigned int flags = WINDOW_KEY_POINTER_MOVE;

                                        if (atomic_read(&desktop.button_down))
                                                flags |= WINDOW_KEY_DOWN;
                                        pointer_report(desktop.focused, x, y, 0,
                                                       flags);
                                }
                        }

                        // cursor_move/reshape has painted the software
                        // fallback. Clear any old plane that resisted its
                        // explicit disable before this event is reported done.
                        cursor_plane_recover();
                }
        }

        rt_mutex_unlock(&desktop.lock);

        if (started)
                pointer_latency_record(started);
}

static void pointer_commit(s64 x, s64 y)
{
        atomic_set(&desktop.pending_x, (int)clamp_t(s64, x, 0, desktop.width - 1));
        atomic_set(&desktop.pending_y, (int)clamp_t(s64, y, 0, desktop.height - 1));

        // Stamp only the first event of a burst, so the measurement is the age
        // of the oldest movement not yet on screen.
        if (!atomic_xchg(&desktop.motion_pending, 1))
                desktop.motion_stamp = ktime_get_ns();

        /*
                A wake, not a queue. The work is one atomic commit that returns
                without waiting, so a workqueue's pool and dispatch and kworker
                would all be overhead around it.
        */
        canvas_thread_wake();
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
        int gain;
        u64 distance, speed;

        desktop.raw_x = 0;
        desktop.raw_y = 0;

        if (!dx && !dy)
                return;

        now = ktime_get_ns();
        interval = now - desktop.accel_stamp;
        desktop.accel_stamp = now;

        if (interval < NSEC_PER_MSEC)
                interval = NSEC_PER_MSEC;

        // The two INT_MIN squares sum to 2^63: widen each product and make
        // their addition unsigned before taking the integer square root.
        distance = int_sqrt((u64)((s64)dx * dx) + (u64)((s64)dy * dy));
        speed = div64_u64(distance * NSEC_PER_MSEC, interval);
        gain = pointer_gain((int)min_t(u64, speed, ACCEL_CEILING));

        pointer_commit((s64)atomic_read(&desktop.pending_x) +
                           accel_apply(dx, &desktop.accel_x, gain),
                       (s64)atomic_read(&desktop.pending_y) +
                           accel_apply(dy, &desktop.accel_y, gain));
}

static void pointer_event_locked(struct input_handle *handle, unsigned int type,
                                 unsigned int code, int value)
{
        if (type == EV_REL)
        {
                // Only remembered here. What it means depends on what else
                // arrives before the device says the movement is over.
                if (code == REL_X)
                {
                        desktop.raw_x = (int)clamp_t(s64,
                            (s64)desktop.raw_x + value, INT_MIN, INT_MAX);
                        pointer_shake(value);
                }
                else if (code == REL_Y)
                {
                        desktop.raw_y = (int)clamp_t(s64,
                            (s64)desktop.raw_y + value, INT_MIN, INT_MAX);
                }
                else if (code == REL_WHEEL_HI_RES ||
                         (code == REL_WHEEL &&
                          !test_bit(REL_WHEEL_HI_RES, handle->dev->relbit)))
                {
                        // Committed here rather than at the report, because a
                        // wheel is not movement: nothing else in the report
                        // changes what it means, and holding it back only
                        // delays the line by a frame.
                        // A consumer can exchange the pending distance with
                        // zero while this input lock is held. Retry against
                        // that new value instead of resurrecting drained input.
                        s64 delta = code == REL_WHEEL_HI_RES ? value
                                                              : (s64)value * WHEEL_V120;
                        int held = atomic_read(&desktop.wheel), wanted;
                        do {
                                wanted = (int)clamp_t(s64, (s64)held + delta,
                                                      INT_MIN, INT_MAX);
                        } while (!atomic_try_cmpxchg(&desktop.wheel, &held, wanted));
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
                        int x = desktop.abs_have & 1
                                    ? desktop.abs_x
                                    : atomic_read(&desktop.pending_x);
                        int y = desktop.abs_have & 2
                                    ? desktop.abs_y
                                    : atomic_read(&desktop.pending_y);

                        pointer_commit(x, y);
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

                // Input ranges may span the signed domain. Widen before
                // subtracting and clamp an out-of-range report before scaling.
                _Bool horizontal = code == ABS_X;
                u64 offset = (s64)clamp(value, abs->minimum, abs->maximum) - abs->minimum;
                u32 span = (s64)abs->maximum - abs->minimum;
                int *position = horizontal ? &desktop.abs_x : &desktop.abs_y;

                *position = (int)div_u64(offset * (u32)(horizontal ? desktop.width : desktop.height), span);
                desktop.abs_have |= horizontal ? 1 : 2;

                return;
        }

        if (type == EV_KEY)
        {
                unsigned int which;

                if (code == BTN_LEFT || code == BTN_TOUCH)
                        which = 0;
                else if (code == BTN_MIDDLE)
                        which = 1;
                else if (code == BTN_RIGHT)
                        which = 2;
                else
                {
                        keyboard_event(handle, code, value);
                        return;
                }

                atomic_set(&desktop.button_x, atomic_read(&desktop.pending_x));
                atomic_set(&desktop.button_y, atomic_read(&desktop.pending_y));

                if (which == 0)
                {
                        atomic_set(&desktop.button_down, !!value);
                        atomic_set(&desktop.button_changed, 1);
                }
                else
                {
                        atomic_set(&desktop.client_button, (int)which);
                        atomic_set(&desktop.client_down, !!value);
                        atomic_set(&desktop.client_changed, 1);
                }

                canvas_thread_wake();
                return;
        }
}

/*
        One attached device, with what it has done.

        The count is per device because the one fault this has to explain is
        a mouse that does nothing until it is plugged in again: whether the
        kernel delivered nothing from it, or delivered reports the cursor did
        not follow, is the whole question, and a total over every device
        cannot answer it. The list is kept here under the input lock rather
        than read out of the input core, whose handle list has a lock of its
        own that a stats ioctl has no business taking.

        An open that fails is tried again. The input core drops a device a
        connect refuses, and a mouse the BIOS was still driving when the
        kernel took the controller can refuse its first open and accept the
        next; a device that stays refused is logged and left, with the error
        where the pointer applet can show it.
*/
#define POINTER_OPEN_TRIES 30

struct pointer_handle
{
        struct input_handle handle;
        struct list_head link;
        struct delayed_work reopen;
        unsigned long events;
        int opened;
        unsigned int tries;
        unsigned int modifiers;
};

static LIST_HEAD(pointer_handles);

static inline struct pointer_handle *pointer_handle_of(struct input_handle *handle)
{
        return container_of(handle, struct pointer_handle, handle);
}

static unsigned int *keyboard_held(struct input_handle *handle)
{
        return &pointer_handle_of(handle)->modifiers;
}

static unsigned int keyboard_modifiers(void)
{
        struct pointer_handle *pointer;
        unsigned int held = 0;

        list_for_each_entry(pointer, &pointer_handles, link)
                held |= pointer->modifiers;
        return (held | (held >> 4)) & (WINDOW_KEY_SHIFT | WINDOW_KEY_CONTROL | WINDOW_KEY_ALT);
}

static HOT void pointer_event(struct input_handle *handle, unsigned int type,
                              unsigned int code, int value)
{
        unsigned long flags;

        // Counted before the desktop check: a report that arrived while there
        // was no screen is still a report the device sent.
        pointer_handle_of(handle)->events++;

        if (!READ_ONCE(desktop.width))
                return;

        spin_lock_irqsave(&desktop.input_lock, flags);
        pointer_event_locked(handle, type, code, value);
        spin_unlock_irqrestore(&desktop.input_lock, flags);
}

static const char *pointer_device_name(struct input_handle *handle)
{
        return handle->dev->name ? handle->dev->name : "unnamed";
}

static void pointer_reopen(struct work_struct *work)
{
        struct pointer_handle *pointer =
            container_of(to_delayed_work(work), struct pointer_handle, reopen);
        int ret = input_open_device(&pointer->handle);

        pointer->opened = ret;

        if (!ret)
        {
                pr_info("[moonwater canvas] " "input: %s opened on try %u\n", pointer_device_name(&pointer->handle), pointer->tries + 1);
                return;
        }

        if (++pointer->tries < POINTER_OPEN_TRIES)
                schedule_delayed_work(&pointer->reopen, HZ);
        else
                pr_info("[moonwater canvas] " "input: %s would not open (%d), given up\n", pointer_device_name(&pointer->handle), ret);
}

static COLD int pointer_connect(struct input_handler *handler,
                                struct input_dev *dev,
                                const struct input_device_id *id)
{
        struct pointer_handle *pointer;
        unsigned long flags;
        int ret;

        pointer = kzalloc(sizeof(*pointer), GFP_KERNEL);
        if (!pointer)
                return -ENOMEM;

        pointer->handle.dev = dev;
        pointer->handle.handler = handler;
        pointer->handle.name = "moonwater";
        INIT_DELAYED_WORK(&pointer->reopen, pointer_reopen);

        ret = input_register_handle(&pointer->handle);
        if (ret)
        {
                kfree(pointer);
                return ret;
        }

        spin_lock_irqsave(&desktop.input_lock, flags);
        list_add_tail(&pointer->link, &pointer_handles);
        spin_unlock_irqrestore(&desktop.input_lock, flags);

        pointer->opened = input_open_device(&pointer->handle);

        if (pointer->opened)
        {
                pr_info("[moonwater canvas] " "input: %s would not open (%d), trying again\n", pointer_device_name(&pointer->handle), pointer->opened);
                schedule_delayed_work(&pointer->reopen, HZ);
        }
        else
                pr_info("[moonwater canvas] " "input: %s\n", pointer_device_name(&pointer->handle));

        return 0;
}

static COLD void pointer_disconnect(struct input_handle *handle)
{
        struct pointer_handle *pointer = pointer_handle_of(handle);
        unsigned long flags;

        cancel_delayed_work_sync(&pointer->reopen);

        // Stop callbacks before releasing held keys; no late press may
        // resurrect a modifier after this device leaves the list.
        if (!pointer->opened)
                input_close_device(handle);

        spin_lock_irqsave(&desktop.input_lock, flags);
        for (unsigned int code = 0; code < KEY_TABLE; code++)
                if (pointer->modifiers & key_map[code][2])
                        keyboard_event(handle, code, 0);
        list_del(&pointer->link);
        spin_unlock_irqrestore(&desktop.input_lock, flags);

        input_unregister_handle(handle);
        kfree(pointer);
}

/*
        The console's keyboard, off while Canvas has the keys.

        The input core hands every key to every handler, and the VT's keyboard
        is one of them: it turns keys into input on the foreground console's
        tty. A machine booted without console= -- every real install -- points
        /dev/console at that tty, and init's shell reads it. So every line
        typed into a window here was typed a second time, unseen, into a root
        shell on tty1, and ran there too.

        K_OFF stops the VT turning keys into tty input and nothing else. evdev
        readers and sysrq are handlers of their own and still hear every key,
        which an exclusive grab would have taken from them, and the shell on
        tty1 stays where it is for whoever is at the console once Canvas lets
        go. Switching consoles from the keyboard is off with it, as it is under
        any display server that sets K_OFF.

        Every console, not only the one in front. Muting just that one left a
        console switch as the way round it: a program calling VT_ACTIVATE puts
        another console in front with its keyboard still on, and the keys go
        to whatever reads that tty. And a console is not muted once for good:
        allocating one runs vc_init, whose reset_vc puts its keyboard back to
        unicode, and a switch away from a VT_PROCESS owner that has died does
        the same. So the VT's own notifier is watched while Canvas has the
        keys, and a console it says was allocated or redrawn -- a switch is a
        redraw of the console coming to the front -- is muted again.

        Each console muted is remembered with the mode it had when it was
        first muted, and that is what it gets back. A mode somebody else set
        in the meantime is theirs and is left alone, and so is a console that
        was already off when Canvas arrived.

        The notifier runs with the console lock held and nothing may sleep, so
        what it and the two ends share is under a spinlock, taken outside the
        keyboard's own lock and never inside it.
*/
#ifdef CONFIG_VT
static DEFINE_SPINLOCK(canvas_vt_lock);
static _Bool canvas_vt_owned;
static _Bool canvas_vt_watching;
static signed char canvas_vt_mode[MAX_NR_CONSOLES] = {
        [0 ... MAX_NR_CONSOLES - 1] = -1,
};

// Under canvas_vt_lock.
static void canvas_keyboard_mute(unsigned int console)
{
        int mode;

        if (console >= MAX_NR_CONSOLES)
                return;

        mode = vt_do_kdgkbmode(console);
        if (mode == K_OFF || vt_do_kdskbmode(console, K_OFF))
                return;

        if (canvas_vt_mode[console] < 0)
                canvas_vt_mode[console] = (signed char)mode;
}

static int canvas_keyboard_follow(struct notifier_block *block,
                                  unsigned long event, void *data)
{
        struct vt_notifier_param *param = data;
        unsigned long flags;

        (void)block;

        if ((event != VT_ALLOCATE && event != VT_UPDATE) || !param || !param->vc)
                return NOTIFY_DONE;

        spin_lock_irqsave(&canvas_vt_lock, flags);
        if (canvas_vt_owned)
                canvas_keyboard_mute(param->vc->vc_num);
        spin_unlock_irqrestore(&canvas_vt_lock, flags);

        return NOTIFY_DONE;
}

static struct notifier_block canvas_keyboard_watch = {
        .notifier_call = canvas_keyboard_follow,
};

static void canvas_keyboard_take(void)
{
        unsigned long flags;
        unsigned int console;

        // Watching first: a console allocated between the loop below and
        // the watch starting would come up unmuted and never be told of.
        if (!canvas_vt_watching && !register_vt_notifier(&canvas_keyboard_watch))
                canvas_vt_watching = true;

        spin_lock_irqsave(&canvas_vt_lock, flags);
        canvas_vt_owned = true;
        for (console = 0; console < MAX_NR_CONSOLES; console++)
                canvas_keyboard_mute(console);
        spin_unlock_irqrestore(&canvas_vt_lock, flags);
}

static void canvas_keyboard_give(void)
{
        unsigned long flags;
        unsigned int console;

        // Unregistering waits out a notifier already running, so nothing
        // mutes a console again behind the loop below.
        if (canvas_vt_watching)
        {
                unregister_vt_notifier(&canvas_keyboard_watch);
                canvas_vt_watching = false;
        }

        spin_lock_irqsave(&canvas_vt_lock, flags);
        canvas_vt_owned = false;
        for (console = 0; console < MAX_NR_CONSOLES; console++)
        {
                if (canvas_vt_mode[console] < 0)
                        continue;

                if (vt_do_kdgkbmode(console) == K_OFF)
                        vt_do_kdskbmode(console, (unsigned int)canvas_vt_mode[console]);

                canvas_vt_mode[console] = -1;
        }
        spin_unlock_irqrestore(&canvas_vt_lock, flags);
}
#else
#define canvas_keyboard_take() ((void)0)
#define canvas_keyboard_give() ((void)0)
#endif

static void canvas_input_devices(struct input_devices *out)
{
        struct pointer_handle *pointer;
        unsigned long flags;

        memory_fill(out, 0, sizeof(*out));

        spin_lock_irqsave(&desktop.input_lock, flags);
        list_for_each_entry(pointer, &pointer_handles, link)
        {
                struct input_device_stats *device;

                if (out->count < INPUT_DEVICES_MAX)
                {
                        device = &out->device[out->count];
                        strscpy(device->name, pointer_device_name(&pointer->handle),
                                sizeof(device->name));
                        device->events = READ_ONCE(pointer->events);
                        device->opened = pointer->opened;
                }

                out->count++;
        }
        spin_unlock_irqrestore(&desktop.input_lock, flags);
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
        struct task_struct *thread = rcu_dereference_protected(
            canvas_thread, lockdep_is_held(&canvas_list_lock));

        if (!thread)
                return;
        RCU_INIT_POINTER(canvas_thread, NULL);

        /* Registration can be interrupted before the input core initializes
           the handler's lists.  Only hand a handler back after the matching
           registration completed. */
        if (pointer_handler_registered)
        {
                input_unregister_handler(&pointer_handler);
                pointer_handler_registered = false;
        }

        // Only once no key can reach the handler: from here the console's
        // keyboard is the only one again.
        canvas_keyboard_give();
        cpu_latency_qos_remove_request(&pointer_qos);

        rt_mutex_lock(&desktop.lock);
        desktop_set_awake(false);
        rt_mutex_unlock(&desktop.lock);
        hrtimer_cancel(&desktop.frame);

        synchronize_rcu();
        kthread_stop(thread);

        // After the canvas thread, which queues flushes until it stops. A
        // flush in flight finishes before the flusher does.
        thread = rcu_dereference_protected(canvas_flusher,
                                           lockdep_is_held(&canvas_list_lock));
        if (thread)
        {
                RCU_INIT_POINTER(canvas_flusher, NULL);
                synchronize_rcu();
                kthread_stop(thread);
        }

        if (canvas_plug_wq)
        {
                destroy_workqueue(canvas_plug_wq);
                canvas_plug_wq = NULL;
        }
}

/*
        Sleeps until something moves, then draws it.

        set_current_state before the flag is read, which is what makes the
        sleep safe: a wake arriving between the two finds the task already
        marked and schedule() returns at once rather than losing the event.
*/
static void canvas_thread_wake(void)
{
        struct task_struct *thread;

        rcu_read_lock();
        thread = rcu_dereference(canvas_thread);
        if (thread)
                wake_up_process(thread);
        rcu_read_unlock();
}

// Whether there is anything to answer a frame. Nothing arms one otherwise.
static _Bool canvas_thread_running(void)
{
        return rcu_access_pointer(canvas_thread) != NULL;
}

/*
        While another program is master of a card Canvas draws on.

        libinput does not grab a device, and neither does this, so a program
        that took the display -- Weston, a game -- and Canvas both heard every
        key. Everything typed into it went into the focused Canvas window as
        well, usually a root shell and often the one that started it, which
        read it all once the program exited. A Control-Shift-T typed there
        opened a terminal behind it.

        So Canvas drops what arrives while somebody else is master: every key,
        button, movement, wheel step, chord and asked-for terminal is taken and
        thrown away before any of it reaches a window, and no frame is drawn,
        since nothing drawn would land. The consoles stay off throughout. The
        other program reads the devices itself and loses nothing.

        The test is made at the top of every pass the thread wakes for, before
        anything is delivered, and every ~250 ms while suspended with nothing
        else to wake for, so a program that drops master and stays running
        does not leave a frozen screen until the mouse moves. A key that
        arrived before the other program became master can still be delivered
        by the pass that was already running; none after. Flushes queued
        before the program took the card answer EBUSY and change nothing.
*/
#define CANVAS_SUSPENDED_POLL_MS 250

static void canvas_input_drop(void)
{
        atomic_set(&desktop.button_changed, 0);
        atomic_set(&desktop.client_changed, 0);
        atomic_set(&desktop.motion_pending, 0);
        atomic_set(&desktop.wheel, 0);
        atomic_set(&desktop.focus_steps, 0);
        atomic_set(&desktop.focus_commit, 0);
        atomic_set(&desktop.minimize, 0);
        atomic_set(&desktop.spawn, 0);
        atomic_set(&desktop.frame_pending, 0);

        // The tail is this thread's to move; the handler only moves head.
        atomic_set(&desktop.key_tail, atomic_read(&desktop.key_head));
}

static _Bool canvas_suspend_check(void)
{
        _Bool taken;

        rt_mutex_lock(&desktop.lock);

        taken = desktop_taken();
        if (taken)
        {
                desktop.suspended = true;

                // No frame drawn now would land, so none is worth a timer: left
                // awake, desktop_frame wakes this thread every frame for as long
                // as the other program keeps the card. Programs make their call
                // again, which answers at once while the card is taken; the
                // resume redraws every pane, and the first commit after it
                // re-arms the timer through desktop_watch.
                if (desktop.awake)
                        desktop_set_awake(false);
        }
        else if (desktop.suspended)
                desktop_resume();

        rt_mutex_unlock(&desktop.lock);

        if (taken)
                canvas_input_drop();

        return taken;
}

static void canvas_flush_wake(void)
{
        struct task_struct *thread;

        rcu_read_lock();
        thread = rcu_dereference(canvas_flusher);
        if (thread)
                wake_up_process(thread);
        rcu_read_unlock();
}

// Without a flusher, a flush happens where it is asked for.
static _Bool canvas_flush_running(void)
{
        return rcu_access_pointer(canvas_flusher) != NULL;
}

static int canvas_loop(void *unused)
{
        while (!kthread_should_stop())
        {
                set_current_state(TASK_IDLE);

                if (!atomic_read(&desktop.motion_pending) &&
                    !atomic_read(&desktop.button_changed) &&
                    !atomic_read(&desktop.client_changed) &&
                    !atomic_read(&desktop.frame_pending) &&
                    !atomic_read(&desktop.wheel) &&
                    !atomic_read(&desktop.focus_steps) &&
                    !atomic_read(&desktop.focus_commit) &&
                    !atomic_read(&desktop.minimize) &&
                    !atomic_read(&desktop.spawn) &&
                    atomic_read(&desktop.key_head) == atomic_read(&desktop.key_tail))
                        schedule_timeout(READ_ONCE(desktop.suspended)
                                             ? msecs_to_jiffies(CANVAS_SUSPENDED_POLL_MS)
                                             : MAX_SCHEDULE_TIMEOUT);

                __set_current_state(TASK_RUNNING);

                if (canvas_suspend_check())
                        continue;

                pointer_apply();

                /*
                        Outside desktop.lock, and deliberately.

                        Starting a program allocates, makes a task and runs
                        execve on it, none of which the lock has anything to do
                        with -- and the window it ends up asking for is created
                        under that same lock by the ioctl the new program will
                        make. Holding it across the spawn is a lock held over an
                        unbounded amount of somebody else's work.
                */
                if (atomic_xchg(&desktop.spawn, 0))
                        spawn_terminal();

                if (atomic_read(&desktop.focus_steps) ||
                    atomic_read(&desktop.focus_commit) ||
                    atomic_read(&desktop.minimize))
                {
                        unsigned int steps =
                            (unsigned int)atomic_xchg(&desktop.focus_steps, 0);
                        _Bool commit = atomic_xchg(&desktop.focus_commit, 0);
                        _Bool minimize = atomic_xchg(&desktop.minimize, 0);
                        _Bool changed = false;

                        rt_mutex_lock(&desktop.lock);
                        while (steps--)
                                changed |= pane_focus_step();
                        if (minimize)
                                changed |= pane_minimize_focused();
                        if (commit)
                                changed |= pane_focus_commit();
                        if (changed)
                                desktop_redraw();
                        rt_mutex_unlock(&desktop.lock);
                }

                if (atomic_read(&desktop.key_head) != atomic_read(&desktop.key_tail))
                {
                        rt_mutex_lock(&desktop.lock);
                        keys_deliver();
                        rt_mutex_unlock(&desktop.lock);
                }

                // On this thread because it walks the window list and writes
                // cells, neither of which an input callback may do.
                if (atomic_read(&desktop.wheel))
                {
                        rt_mutex_lock(&desktop.lock);
                        wheel_deliver();
                        rt_mutex_unlock(&desktop.lock);
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

                Priority 1, as sched_set_fifo_low gives: ahead of every
                SCHED_OTHER task and behind anything the machine considers
                more urgent than a cursor, which is the honest place for it.

                And reset on fork, which sched_set_fifo_low does not ask for.
                This thread starts programs -- Control-Shift-T's terminal is
                user_mode_thread called from here -- and a task forked from a
                FIFO task is FIFO unless the parent says otherwise. So every
                terminal opened from the keyboard, its shell and whatever was
                run in it were FIFO 1 beside this thread: a stress-ng --cpu 0
                in one held every processor against the cursor, which FIFO
                never timeslices at equal priority, and left the other
                terminals and every kworker RT throttling's 5%. With the flag
                the scheduler starts each child SCHED_OTHER at nice 0, by its
                own rule and on every path a program can be started from.
        */
        static const struct sched_attr canvas_policy = {
                .size = sizeof(struct sched_attr),
                .sched_policy = SCHED_FIFO,
                .sched_priority = 1,
                .sched_flags = SCHED_FLAG_RESET_ON_FORK,
        };
        struct task_struct *thread, *flush;

        hrtimer_setup(&desktop.frame, desktop_frame, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        thread = kthread_run(canvas_loop, NULL, "moonwater/canvas");

        if (IS_ERR(thread))
        {
                pr_info("[moonwater canvas] " "no thread for input\n");
                return;
        }

        rcu_assign_pointer(canvas_thread, thread);
        WARN_ON_ONCE(sched_setattr_nocheck(thread, &canvas_policy));

        /*
                The flusher, at the same policy. Without it every flush happens
                where it is asked for, which still works: it is what the canvas
                thread and every committing program did before there was one.
        */
        flush = kthread_run(canvas_flush_loop, NULL, "moonwater/flush");
        if (IS_ERR(flush))
                pr_info("[moonwater canvas] " "no flush thread, flushing in place\n");
        else
        {
                WARN_ON_ONCE(sched_setattr_nocheck(flush, &canvas_policy));
                rcu_assign_pointer(canvas_flusher, flush);
        }

        // 0 microseconds: no idle state whose exit can be measured.
        cpu_latency_qos_add_request(&pointer_qos, 0);

        // Before the handler, so no key is ever delivered to both.
        canvas_keyboard_take();

        if (input_register_handler(&pointer_handler))
        {
                pr_info("[moonwater canvas] " "could not register the input handler\n");
                canvas_keyboard_give();
        }
        else
                pointer_handler_registered = true;
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
        struct drm_rect cursor;
        struct output *output;

        memory_fill(out, 0, sizeof(*out));
        rt_mutex_lock(&desktop.lock);

        out->requested_generation = cursor_plane_requested_generation;
        out->armed_generation = cursor_plane_armed_generation;
        out->updates = (unsigned long)atomic_long_read(&cursor_plane_updates);
        out->failures = (unsigned long)atomic_long_read(&cursor_plane_failures);
        out->requested_x = cursor_plane_requested_x;
        out->requested_y = cursor_plane_requested_y;
        out->armed_x = cursor_plane_armed_x;
        out->armed_y = cursor_plane_armed_y;
        cursor_cell(&cursor, desktop.cursor_x, desktop.cursor_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        list_for_each_entry(output, &desktop.outputs, link)
        {
                if (output_touched(output, &cursor, 1))
                        out->wanted++;

                if (output->cursor_plane)
                {
                        out->active++;

                        if (output->cursor_shown)
                                out->shown++;
                }
        }

        out->recovering = cursor_plane_recovery;

        rt_mutex_unlock(&desktop.lock);
}
