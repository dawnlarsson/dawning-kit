/*
        Canvas -- keys

        What a keyboard means is the compositor's to decide, the same way what
        a string looks like is. A program is handed characters, not scancodes
        and a table of its own.

        The default table is US ASCII. `moonwater keyboard` switches the live
        map; AltGr is level 3, not compositor Alt. Dead keys and compose are
        still a larger table and the same shape of code.
*/

/*
        Wide enough for the keypad, which is the last thing on a keyboard that
        means a character. Everything past it -- the arrows, Home and its
        neighbours, the function keys -- means no character at all, and is
        carried to a program as key.code with a character of zero, which is
        what code exists for.
*/
#define KEY_TABLE 128
#define KEY_LEVELS 4
#ifndef KEY_102ND
#define KEY_102ND 86
#endif

/*
        Backspace is DEL, not BS.

        A terminal line discipline erases on VERASE, which is 127 everywhere,
        and treats 8 as an ordinary character -- so sending 8 echoed as ^H and
        made the line two characters longer for every press.
*/
/*
        Which modifier a key is, in the held word's own spelling.

        A right-hand key is its left-hand flag moved up four bits, so
        keyboard_modifiers can put the side back by shifting down and one word
        carries both hands. That works for shift, control and alt because
        their flags are 2, 4 and 8 and nothing else lives at 32, 64 and 128.

        AltGr is the one that does not fit. WINDOW_KEY_CONTROL << 4 is 64 and
        WINDOW_KEY_ALTGR is 64, so right Control and right Alt were the same
        bit in this table: holding right Control selected the AltGr level of
        the map, holding right Alt read as Control -- so Control chords fired
        on it and Alt-Tab did not -- and either key held made the other's
        release go out on disconnect. AltGr is also not the other side of
        Alt, which is why it cannot simply be WINDOW_KEY_ALT << 4: since
        af20afca right Alt means level three of the map, not compositor Alt.

        So the held word gets a bit of its own, above everything the shift can
        reach and deliberately not a window flag, and keyboard_modifiers
        spells it as WINDOW_KEY_ALTGR on the way out. The flag itself does not
        move: it is in window_key.flags, which is what every client reads.

        WINDOW_KEY_SHIFT << 4 is 32, which is also WINDOW_KEY_POINTER_MOVE,
        and that one is harmless rather than lucky: a held word is never a
        client's flags. What reaches window_key.flags is keyboard_modifiers'
        answer, which carries no bit above 8 but the one it puts there, and a
        pointer's flags never enter a keyboard's held word.
*/
#define KEY_HELD_ALTGR 256u

static unsigned short key_mod[KEY_TABLE] = {
    [KEY_LEFTSHIFT] = WINDOW_KEY_SHIFT,
    [KEY_RIGHTSHIFT] = WINDOW_KEY_SHIFT << 4,
    [KEY_LEFTCTRL] = WINDOW_KEY_CONTROL,
    [KEY_RIGHTCTRL] = WINDOW_KEY_CONTROL << 4,
    [KEY_LEFTALT] = WINDOW_KEY_ALT,
    [KEY_RIGHTALT] = KEY_HELD_ALTGR,
};
static unsigned short key_live[KEY_TABLE][KEY_LEVELS];
static char canvas_layout_held[8] = "us";
static bool key_ready;

struct key_patch {
        unsigned char code;
        unsigned short level[KEY_LEVELS];
};

static const unsigned short key_us[KEY_TABLE][KEY_LEVELS] = {
    [1] = {27, 27},
    [2] = {'1', '!'},  [3] = {'2', '@'},  [4] = {'3', '#'},
    [5] = {'4', '$'},  [6] = {'5', '%'},  [7] = {'6', '^'},
    [8] = {'7', '&'},  [9] = {'8', '*'},  [10] = {'9', '('},
    [11] = {'0', ')'}, [12] = {'-', '_'}, [13] = {'=', '+'},
    [14] = {127, 127}, [15] = {'\t', '\t'},
    [16] = {'q', 'Q'}, [17] = {'w', 'W'}, [18] = {'e', 'E'},
    [19] = {'r', 'R'}, [20] = {'t', 'T'}, [21] = {'y', 'Y'},
    [22] = {'u', 'U'}, [23] = {'i', 'I'}, [24] = {'o', 'O'},
    [25] = {'p', 'P'}, [26] = {'[', '{'}, [27] = {']', '}'},
    [28] = {'\n', '\n'},
    [30] = {'a', 'A'}, [31] = {'s', 'S'}, [32] = {'d', 'D'},
    [33] = {'f', 'F'}, [34] = {'g', 'G'}, [35] = {'h', 'H'},
    [36] = {'j', 'J'}, [37] = {'k', 'K'}, [38] = {'l', 'L'},
    [39] = {';', ':'}, [40] = {'\'', '"'}, [41] = {'`', '~'},
    [43] = {'\\', '|'},
    [44] = {'z', 'Z'}, [45] = {'x', 'X'}, [46] = {'c', 'C'},
    [47] = {'v', 'V'}, [48] = {'b', 'B'}, [49] = {'n', 'N'},
    [50] = {'m', 'M'}, [51] = {',', '<'}, [52] = {'.', '>'},
    [53] = {'/', '?'}, [55] = {'*', '*'}, [57] = {' ', ' '},
    [71] = {'7', '7'}, [72] = {'8', '8'}, [73] = {'9', '9'},
    [74] = {'-', '-'}, [75] = {'4', '4'}, [76] = {'5', '5'},
    [77] = {'6', '6'}, [78] = {'+', '+'}, [79] = {'1', '1'},
    [80] = {'2', '2'}, [81] = {'3', '3'}, [82] = {'0', '0'},
    [83] = {'.', '.'}, [96] = {'\n', '\n'}, [98] = {'/', '/'},
};

static const struct key_patch key_uk[] = {
        {3, {'2', '"', '@'}},
        {4, {'3', 0xA3, 0xA3}},
        {40, {'\'', '@'}},
        {41, {'`', 0xAC, 0xA6}},
        {43, {'#', '~', '\\'}},
        {KEY_102ND, {'\\', '|'}},
};

static const struct key_patch key_de[] = {
        {12, {0xDF, '?', '\\'}},
        {13, {0xB4, '`'} },
        {21, {'z', 'Z'}},
        {26, {0xFC, 0xDC}},
        {27, {'+', '*', '~'}},
        {39, {0xF6, 0xD6}},
        {40, {0xE4, 0xC4}},
        {41, {'^', 0xB0}},
        {43, {'#', '\''}},
        {44, {'y', 'Y'}},
        {KEY_102ND, {'<', '>', '|'}},
        {8, {'7', '/', '{'}},
        {9, {'8', '(', '['}},
        {10, {'9', ')', ']'}},
        {11, {'0', '=', '}'}},
        {16, {'q', 'Q', '@'}},
        {18, {'e', 'E', 0x20AC}},
};

static const struct key_patch key_se[] = {
        {3, {'2', '"', '@'}},
        {4, {'3', '#', 0xA3}},
        {5, {'4', 0xA4, '$'}},
        {6, {'5', '%', 0x20AC}},
        {7, {'6', '&'}},
        {8, {'7', '/', '{'}},
        {9, {'8', '(', '['}},
        {10, {'9', ')', ']'}},
        {11, {'0', '=', '}'}},
        {12, {'+', '?', '\\'}},
        {13, {0xB4, '`'}},
        {26, {0xE5, 0xC5}},
        {27, {0xA8, '^', '~'}},
        {39, {0xF6, 0xD6}},
        {40, {0xE4, 0xC4}},
        {41, {0xA7, 0xBD}},
        {43, {'\'', '*'}},
        {53, {'-', '_'}},
        {KEY_102ND, {'<', '>', '|'}},
};

static const struct key_patch key_no[] = {
        {3, {'2', '"', '@'}},
        {4, {'3', '#', 0xA3}},
        {5, {'4', 0xA4, '$'}},
        {6, {'5', '%', 0x20AC}},
        {8, {'7', '/', '{'}},
        {9, {'8', '(', '['}},
        {10, {'9', ')', ']'}},
        {11, {'0', '=', '}'}},
        {12, {'+', '?'}},
        {13, {'\\', '`', 0xB4}},
        {26, {0xE5, 0xC5}},
        {39, {0xF8, 0xD8}},
        {40, {0xE6, 0xC6}},
        {41, {'|', 0xA7}},
        {43, {'\'', '*'}},
        {53, {'-', '_'}},
        {KEY_102ND, {'<', '>'}},
};

static const struct key_patch key_fr[] = {
        {2, {'&', '1'}},
        {3, {0xE9, '2', '~'}},
        {4, {'"', '3', '#'}},
        {5, {'\'', '4', '{'}},
        {6, {'(', '5', '['}},
        {7, {'-', '6', '|'}},
        {8, {0xE8, '7', '`'}},
        {9, {'_', '8', '\\'}},
        {10, {0xE7, '9', '^'}},
        {11, {0xE0, '0', '@'}},
        {12, {')', 0xB0, ']'}},
        {13, {'=', '+', '}'}},
        {16, {'a', 'A'}},
        {17, {'z', 'Z'}},
        {18, {'e', 'E', 0x20AC}},
        {26, {'^', 0xA8}},
        {27, {'$', 0xA3, 0xA4}},
        {30, {'q', 'Q'}},
        {38, {'m', 'M'}},
        {39, {0xF9, '%'}},
        {40, {'*', 0xB5}},
        {44, {'w', 'W'}},
        {50, {',', '?'}},
        {51, {';', '.'}},
        {52, {':', '/'}},
        {53, {'!', 0xA7}},
        {KEY_102ND, {'<', '>'}},
};

static const struct key_patch key_es[] = {
        {3, {'2', '"', '@'}},
        {4, {'3', 0xB7, '#'}},
        {8, {'7', '/', '{'}},
        {11, {'0', '=', '}'}},
        {12, {'\'', '?'}},
        {13, {0xA1, 0xBF}},
        {26, {'`', '^', '['}},
        {27, {'+', '*', ']'}},
        {39, {0xF1, 0xD1}},
        {40, {0xB4, 0xA8, '{'}},
        {41, {0xBA, 0xAA, '\\'}},
        {43, {0xE7, 0xC7, '}'}},
        {KEY_102ND, {'<', '>'}},
        {18, {'e', 'E', 0x20AC}},
};

static const struct key_patch key_it[] = {
        {3, {'2', '"'}},
        {4, {'3', 0xA3}},
        {8, {'7', '{'}},
        {9, {'8', '['}},
        {10, {'9', ']'}},
        {11, {'0', '}'}},
        {12, {'\'', '?'}},
        {13, {0xEC, '^'}},
        {26, {0xE8, 0xE9, '['}},
        {27, {'+', '*', ']'}},
        {39, {0xF2, 0xE7, '@'}},
        {40, {0xE0, 0xB0, '#'}},
        {41, {'\\', '|'}},
        {43, {0xF9, 0xA7}},
        {KEY_102ND, {'<', '>'}},
        {18, {'e', 'E', 0x20AC}},
};

static void key_load_us(void)
{
        memset(key_live, 0, sizeof(key_live));
        memcpy(key_live, key_us, sizeof(key_us));
}

static void key_patch_apply(const struct key_patch *patch, unsigned int count)
{
        unsigned int at;

        for (at = 0; at < count; at++)
                memcpy(key_live[patch[at].code], patch[at].level,
                       sizeof(patch[at].level));
}

static const char *canvas_layout_name(void)
{
        if (!key_ready)
        {
                key_load_us();
                key_ready = true;
                strscpy(canvas_layout_held, "us", sizeof(canvas_layout_held));
        }
        return canvas_layout_held;
}

static long canvas_layout_set(const char *name)
{
        key_load_us();
        key_ready = true;
        if (!name || !strcmp(name, "us"))
        {
                strscpy(canvas_layout_held, "us", sizeof(canvas_layout_held));
                return 0;
        }
        if (!strcmp(name, "uk") || !strcmp(name, "gb"))
        {
                key_patch_apply(key_uk, ARRAY_SIZE(key_uk));
        }
        else if (!strcmp(name, "de"))
                key_patch_apply(key_de, ARRAY_SIZE(key_de));
        else if (!strcmp(name, "se") || !strcmp(name, "sv") || !strcmp(name, "fi"))
                key_patch_apply(key_se, ARRAY_SIZE(key_se));
        else if (!strcmp(name, "no") || !strcmp(name, "nb") || !strcmp(name, "dk"))
                key_patch_apply(key_no, ARRAY_SIZE(key_no));
        else if (!strcmp(name, "fr"))
                key_patch_apply(key_fr, ARRAY_SIZE(key_fr));
        else if (!strcmp(name, "es"))
                key_patch_apply(key_es, ARRAY_SIZE(key_es));
        else if (!strcmp(name, "it"))
                key_patch_apply(key_it, ARRAY_SIZE(key_it));
        else
        {
                key_load_us();
                strscpy(canvas_layout_held, "us", sizeof(canvas_layout_held));
                return -EINVAL;
        }
        strscpy(canvas_layout_held, name, sizeof(canvas_layout_held));
        return 0;
}

// The input handler owns each device's held bits and the list to combine.
static unsigned int *keyboard_held(struct input_handle *handle);
static unsigned int keyboard_modifiers(void);

static unsigned int key_character(unsigned int code, unsigned int modifiers)
{
        unsigned int level = 0;
        unsigned int c;
        const unsigned short (*map)[KEY_LEVELS] = key_ready ? key_live : key_us;

        if (code >= KEY_TABLE)
                return 0;
        if (modifiers & WINDOW_KEY_ALTGR)
                level += 2;
        if (modifiers & WINDOW_KEY_SHIFT)
                level += 1;
        c = map[code][level];
        if (!c && level)
                c = map[code][level & 2];
        if (!c && (modifiers & WINDOW_KEY_SHIFT))
                c = map[code][1];
        if (!c)
                c = map[code][0];
        if (!c)
                return 0;

        // Control turns a letter into the control code that letter names,
        // which is the whole of why a terminal wants a modifier at all.
        if ((modifiers & WINDOW_KEY_CONTROL) &&
            (unsigned int)((c | 0x20) - 'a') < 26)
                return c & 0x1f;

        return c;
}

/*
        Whether a key is somebody typing.

        A modifier on its own is not. It has no character and no sequence, so
        term_key_modified emits nothing for it and nothing reaches the program
        -- and it is what one hand holds while the other works the wheel or
        the bar, which must not be read as a reason to leave where they were
        reading.
*/
static PURE _Bool key_typed(unsigned int code, unsigned int flags)
{
        if (!(flags & WINDOW_KEY_DOWN))
                return false;

        return code >= KEY_TABLE || !key_mod[code];
}

/*
        Called by the input core, so no lock and no sleeping. Which window has
        focus is decided under desktop.lock and the window can be freed, so
        this records what happened and the thread hands it over.
*/
static void keyboard_event(struct input_handle *handle, unsigned int code, int value)
{
        unsigned int modifiers;
        unsigned int bit;
        unsigned int head, tail;
        struct window_key key;

        if (bind_key_swallowed(code, value))
                return;

        modifiers = (unsigned int)atomic_read(&desktop.modifiers);
        bit = code < KEY_TABLE ? key_mod[code] : 0;

        if (bit)
        {
                unsigned int *held = keyboard_held(handle);
                *held = value ? *held | bit : *held & ~bit;
                modifiers = keyboard_modifiers();
                atomic_set(&desktop.modifiers, (int)modifiers);

                /*
                        Alt belongs to compositor chords.

                        Sending Alt-down to one client, changing focus on Tab,
                        then sending Alt-up to another leaves both clients with
                        a modifier state that never happened. Keep Alt in the
                        flags of ordinary keys, but consume its own events.
                */
                if (bit & (WINDOW_KEY_ALT | (WINDOW_KEY_ALT << 4) |
                           KEY_HELD_ALTGR))
                {
                        if (!(modifiers & WINDOW_KEY_ALT) &&
                            atomic_xchg(&desktop.focus_cycling, 0))
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

        /*
                Control-Shift-T is a new terminal.

                Shift, and not Control-T on its own, because Control-T is
                readline's transpose-characters and the shell in every window
                would lose it. Nothing is lost by taking this one: key_character
                folds Control over a letter before it looks at Shift, so
                Control-Shift-T and Control-T are the same byte 0x14 to a
                program, and a chord no client could tell apart from another is
                a chord no client can miss.

                A press and not a repeat, so a key held down does not fill the
                desktop with shells; the release is consumed with it, or the
                client sees half a chord.
        */
        if ((modifiers & (WINDOW_KEY_ALT | WINDOW_KEY_SHIFT |
                          WINDOW_KEY_CONTROL)) ==
                (WINDOW_KEY_SHIFT | WINDOW_KEY_CONTROL) &&
            code == KEY_T)
        {
                if (value == 1)
                {
                        atomic_set(&desktop.spawn, 1);
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
        unsigned int head, tail, at;

        head = (unsigned int)atomic_read(&desktop.key_head);
        tail = (unsigned int)atomic_read(&desktop.key_tail);

        if (head == tail)
                return;

        // Owned panes have no program. Off can leave the kernel log
        // focused, and a click can put it there; either way the keys
        // belong in a window that can take them, not in the ring of
        // nothing. Create still does not focus: this is only when a
        // key has already arrived and nothing shared holds it.
        if (!pane || !pane->shared)
        {
                pane = pane_topmost(NULL, false, INT_MAX);
                if (pane)
                {
                        pane_focus(pane);
                        desktop.damage_all = true;
                }
        }

        if (!pane || !pane->shared)
        {
                atomic_set(&desktop.key_tail, (int)head);
                return;
        }

        smp_rmb();

        /*
                Typing puts the window back at the end.

                What was typed lands at the bottom, and a view left where the
                hand put it shows none of it -- nor anything the program says
                about it, because nothing scrolled back is drawn at all. So a
                window read back through and then typed into looked like one
                that had stopped listening, when every key had in fact arrived.

                The window the keys land in, which is why this is below the
                return above rather than beside it: the kernel log has no
                program to type at and drops them, and reading back through a
                boot must not end at the first key pressed at it. Before the
                handover rather than after, so the frame it asks for is the
                one the program's answer is drawn in.
        */
        for (at = tail; at != head; at++)
        {
                const struct window_key *key = &desktop.key_ring[at % WINDOW_KEYS];

                if (!key_typed(key->code, key->flags))
                        continue;

                // Focus rather than what the pointer is over, which is where
                // the wheel goes: reading one window while typing into
                // another leaves the one being read where it was.
                if (pane_view_live(pane))
                        atomic_set(&desktop.frame_pending, 1);

                break;
        }

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

        // The program may be asleep on its file; this is what it was waiting
        // for, and the wake is the whole of the latency from here on.
        wake_up_interruptible(&pane->wait);
}
