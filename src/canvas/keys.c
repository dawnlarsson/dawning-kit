/*
        Canvas -- keys

        What a keyboard means is the compositor's to decide, the same way what
        a string looks like is. A program is handed characters, not scancodes
        and a table of its own.

        The layout is US ASCII, which is what the machine has been booted with
        so far. Anything beyond it -- other layouts, dead keys, compose -- is a
        larger table and the same shape of code.
*/

/*
        Wide enough for the keypad, which is the last thing on a keyboard that
        means a character. Everything past it -- the arrows, Home and its
        neighbours, the function keys -- means no character at all, and is
        carried to a program as key.code with a character of zero, which is
        what code exists for.
*/
#define KEY_TABLE 128

/*
        Backspace is DEL, not BS.

        A terminal line discipline erases on VERASE, which is 127 everywhere,
        and treats 8 as an ordinary character -- so sending 8 echoed as ^H and
        made the line two characters longer for every press.
*/
static const char key_plain[KEY_TABLE] = {
    [1] = 27,
    [2] = '1',  [3] = '2',  [4] = '3',  [5] = '4',  [6] = '5',
    [7] = '6',  [8] = '7',  [9] = '8',  [10] = '9', [11] = '0',
    [12] = '-', [13] = '=', [14] = 127, [15] = '\t',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']', [28] = '\n',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
    [39] = ';', [40] = '\'', [41] = '`', [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [55] = '*', [57] = ' ',
    // The keypad as NumLock on, there being nothing here that turns it off.
    [71] = '7', [72] = '8', [73] = '9', [74] = '-',
    [75] = '4', [76] = '5', [77] = '6', [78] = '+',
    [79] = '1', [80] = '2', [81] = '3', [82] = '0',
    [83] = '.', [96] = '\n', [98] = '/',
};

static const char key_shifted[KEY_TABLE] = {
    [1] = 27,
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',
    [7] = '^',  [8] = '&',  [9] = '*',  [10] = '(', [11] = ')',
    [12] = '_', [13] = '+', [14] = 127, [15] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '{', [27] = '}', [28] = '\n',
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = 'M', [51] = '<', [52] = '>', [53] = '?',
    [55] = '*', [57] = ' ',
    [71] = '7', [72] = '8', [73] = '9', [74] = '-',
    [75] = '4', [76] = '5', [77] = '6', [78] = '+',
    [79] = '1', [80] = '2', [81] = '3', [82] = '0',
    [83] = '.', [96] = '\n', [98] = '/',
};

static unsigned int key_modifier(unsigned int code)
{
        switch (code)
        {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
                return WINDOW_KEY_SHIFT;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
                return WINDOW_KEY_CONTROL;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
                return WINDOW_KEY_ALT;
        default:
                return 0;
        }
}

static unsigned int key_character(unsigned int code, unsigned int modifiers)
{
        char c;

        if (code >= KEY_TABLE)
                return 0;

        c = modifiers & WINDOW_KEY_SHIFT ? key_shifted[code] : key_plain[code];

        if (!c)
                return 0;

        // Control turns a letter into the control code that letter names,
        // which is the whole of why a terminal wants a modifier at all.
        if ((modifiers & WINDOW_KEY_CONTROL) &&
            (unsigned int)((c | 0x20) - 'a') < 26)
                return (unsigned int)c & 0x1f;

        return (unsigned int)(unsigned char)c;
}

/*
        Called by the input core, so no lock and no sleeping. Which window has
        focus is decided under desktop.lock and the window can be freed, so
        this records what happened and the thread hands it over.
*/
static void keyboard_event(unsigned int code, int value)
{
        unsigned int modifiers = (unsigned int)atomic_read(&desktop.modifiers);
        unsigned int bit = key_modifier(code);
        unsigned int head, tail;
        struct window_key key;

        if (bit)
        {
                if (value)
                        modifiers |= bit;
                else
                        modifiers &= ~bit;

                atomic_set(&desktop.modifiers, (int)modifiers);

                /*
                        Alt belongs to compositor chords.

                        Sending Alt-down to one client, changing focus on Tab,
                        then sending Alt-up to another leaves both clients with
                        a modifier state that never happened. Keep Alt in the
                        flags of ordinary keys, but consume its own events.
                */
                if (bit == WINDOW_KEY_ALT)
                {
                        if (!value && atomic_xchg(&desktop.focus_cycling, 0))
                        {
                                atomic_set(&desktop.focus_commit, 1);
                                canvas_thread_wake();
                        }

                        return;
                }
        }

        /*
                The conservative chord is unmodified Alt-Tab. Shift-Alt-Tab
                is left with the client until modifier buffering exists; that
                is safer than moving focus after Shift-down was already sent
                to the old client.
        */
        if ((modifiers & (WINDOW_KEY_ALT | WINDOW_KEY_SHIFT |
                          WINDOW_KEY_CONTROL)) == WINDOW_KEY_ALT &&
            code == KEY_TAB)
        {
                if (value)
                {
                        // library.c has an address-first atomic_inc of its
                        // own, so use the kernel spelling that cannot collide.
                        atomic_fetch_add(1, &desktop.focus_steps);
                        atomic_set(&desktop.focus_cycling, 1);
                        canvas_thread_wake();
                }

                return;
        }

        // Alt-F9 is the compositor-owned minimize affordance. Its release is
        // consumed as part of the same chord.
        if ((modifiers & (WINDOW_KEY_ALT | WINDOW_KEY_SHIFT |
                          WINDOW_KEY_CONTROL)) == WINDOW_KEY_ALT &&
            code == KEY_F9 && !atomic_read(&desktop.focus_cycling))
        {
                if (value)
                {
                        atomic_set(&desktop.minimize, 1);
                        canvas_thread_wake();
                }

                return;
        }

        // Once an Alt-Tab traversal has started, do not leak another
        // Alt-modified key into the selected-but-not-yet-raised client.
        if ((modifiers & WINDOW_KEY_ALT) &&
            atomic_read(&desktop.focus_cycling))
                return;

        key.code = code;
        key.character = value ? key_character(code, modifiers) : 0;
        // Autorepeat arrives as 2, and a terminal wants it like a press.
        key.flags = (value ? WINDOW_KEY_DOWN : 0) | modifiers;
        key.reserved = 0;

        head = (unsigned int)atomic_read(&desktop.key_head);
        tail = (unsigned int)atomic_read(&desktop.key_tail);

        /*
                One writer each: this moves head, the thread moves tail. A full
                ring therefore loses the newest, because losing the oldest
                would mean moving tail from here as well, and two writers on
                one index is a ring that delivers a key twice or reads a slot
                while it is being overwritten.
        */
        if (head - tail >= WINDOW_KEYS)
                return;

        desktop.key_ring[head % WINDOW_KEYS] = key;
        smp_wmb();
        atomic_set(&desktop.key_head, (int)(head + 1));

        canvas_thread_wake();
}

// Under desktop.lock, so the focused window is safe to reach.
static void keys_deliver(void)
{
        struct pane *pane = desktop.focused;
        unsigned int head, tail;

        head = (unsigned int)atomic_read(&desktop.key_head);
        tail = (unsigned int)atomic_read(&desktop.key_tail);

        if (head == tail)
                return;

        if (!pane || !pane->shared)
        {
                atomic_set(&desktop.key_tail, (int)head);
                return;
        }

        smp_rmb();

        /*
                Every one is consumed here whether or not it lands.

                Nothing obliges a program to read its keys, and holding the
                desktop's ring until it does makes one window able to stop the
                keyboard for every other -- and to spin the thread, which sleeps
                only while the ring is empty. A window that will not listen
                loses what was said to it, which is what a keyboard buffer has
                always done.
        */
        for (; tail != head; tail++)
        {
                struct window *shared = pane->shared;
                unsigned int at = READ_ONCE(shared->key_head);

                // Tail belongs to the program. Moving it from here to make
                // room would put two writers on it, and window_key is the
                // other one.
                if (at - READ_ONCE(shared->key_tail) < WINDOW_KEYS)
                {
                        shared->keys[at % WINDOW_KEYS] = desktop.key_ring[tail % WINDOW_KEYS];
                        smp_wmb();
                        WRITE_ONCE(shared->key_head, at + 1);
                }
        }

        atomic_set(&desktop.key_tail, (int)head);
}
