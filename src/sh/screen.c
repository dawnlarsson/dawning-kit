/*
        The commands that draw.

        Each was its own program and each is a window on the compositor: a
        terminal, a field of colour, a page of text, a report on how long the
        pointer takes to move. They are together because they want the same
        thing -- src/canvas/window.c, the client side of Canvas -- and that is
        included once, above.

        A window that cannot be opened is not an error worth a special path:
        every one of these says so and returns, which is what running them on
        a machine with no compositor does.
*/

/*
        What the far end's line discipline is set to.

        The terminal edits the line being typed rather than sending it a
        character at a time, and that is only right while the program on the
        other end is asking for whole lines. ICANON is the question and this
        is how it is asked; ECHO has to go off with it, or every character
        appears twice -- once where the editor drew it and once where the
        kernel echoed it back.
*/
#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TERMINAL_CANONICAL 0x0002u
#define TERMINAL_ECHO 0x0008u
#define TERMINAL_CONTROLS 19

// The struct the kernel fills in, which is not the larger one a C library
// hands its callers. A pty master answers these for the slave, so this is
// asking the program on the other end what it has asked the line discipline
// for.
typedef struct
{
        unsigned int arriving, leaving, hardware, behaviour;
        p8 discipline;
        p8 controls[TERMINAL_CONTROLS];
} terminal_modes;

/*
        What a write did to the queued bytes.

        The pty is nonblocking, so a successful write may consume only a
        prefix and EAGAIN may consume nothing. Keep both cases queued for the
        next pass through the event loop. Any other result means the pty can
        no longer make progress and lets the caller end the session instead
        of spinning on it.
*/
static b32 term_sent(bipolar wrote)
{
        if (wrote > 0)
        {
                positive taken = (positive)wrote < to_shell_length
                                     ? (positive)wrote
                                     : to_shell_length;

                memory_copy(to_shell, to_shell + taken,
                            to_shell_length - taken);
                to_shell_length -= taken;
                return true;
        }

        return wrote == -EAGAIN || wrote == -EINTR;
}

// What the emulator has to say, on its way. A keystroke at a time, so a line
// long enough to fill the buffer cannot be cut in half by the next one.
static b32 term_send(b32 master)
{
        if (!to_shell_length)
                return true;

        return term_sent(system_call_3(syscall(write), master,
                                       (positive)to_shell, to_shell_length));
}

static fn term_follow_modes(b32 master)
{
        terminal_modes modes;

        if (system_call_3(syscall(ioctl), master, TCGETS,
                          (positive)address_of modes) != 0)
                return;

        term_line_editing((modes.behaviour & TERMINAL_CANONICAL) != 0);

        // Left off when the far end goes raw. What it turns off for itself is
        // its own to turn back on, and doing that from here would be echoing
        // into a program that had just asked for silence.
        if (!line_editing || !(modes.behaviour & TERMINAL_ECHO))
                return;

        modes.behaviour &= ~TERMINAL_ECHO;
        system_call_3(syscall(ioctl), master, TCSETS, (positive)address_of modes);
}

// term ---------------------------------------------------------
static b32 screen_term()
{
        claim_standard_descriptors();

        window = window_open_text(COLUMNS_WANTED, ROWS_WANTED);

        if (!window)
        {
                log_direct(str("term: no window\n"));
                return 1;
        }

        // Named, because the compositor puts a window of its own beside this
        // one and two untitled terminals are a guessing game.
        string_copy((string_address)window->title, (string_address) "shell");

        // A window narrower or shorter than one cell is not a grid, and every
        // wrap and scroll divides by these.
        COLUMNS = window->columns ? window->columns : 1;
        ROWS = window->rows ? window->rows : 1;

        if (COLUMNS > window->stride)
                COLUMNS = window->stride;

        full_reset();

        /*
                Waiting for the other end to exist.

                The compositor starts this the moment it has a screen, which
                is before init has mounted devpts -- there is no ordering
                between the two and there does not need to be, so long as this
                is willing to wait a moment for it.
        */
        timespec wait = {0, 20000000};
        b32 master = -1;

        for (int tries = 0; tries < 200 && master < 0; tries++)
        {
                master = system_call_4(syscall(openat), AT_FDCWD, (positive)"/dev/ptmx",
                                       FILE_READ_WRITE | O_NONBLOCK, 0);

                if (master < 0)
                        system_call_2(syscall(nanosleep), (positive)address_of wait, 0);
        }

        if (master < 0)
        {
                log_direct(str("term: no /dev/ptmx\n"));
                return 1;
        }

        int unlock = 0;
        unsigned int number = 0;

        system_call_3(syscall(ioctl), master, TIOCSPTLCK, (positive)address_of unlock);
        system_call_3(syscall(ioctl), master, TIOCGPTN, (positive)address_of number);

        p8 name[32] = "/dev/pts/";
        positive at = 9;

        at += positive_into_string(name + at, number);

        b32 slave = system_call_4(syscall(openat), AT_FDCWD, (positive)name,
                                  FILE_READ_WRITE, 0);

        if (slave < 0)
        {
                log_direct(str("term: no slave\n"));
                return 1;
        }

        winsize size = {(unsigned short)ROWS, (unsigned short)COLUMNS,
                        (unsigned short)(COLUMNS * WINDOW_CELL_W),
                        (unsigned short)(ROWS * WINDOW_CELL_H)};

        system_call_3(syscall(ioctl), master, TIOCSWINSZ, (positive)address_of size);

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                string_address argv[] = {SHELL, null};
                string_address envp[] = {"TERM=ansi", null};

                system_call(syscall(setsid));
                system_call_3(syscall(ioctl), slave, TIOCSCTTY, 0);
                system_call_3(syscall(dup3), slave, 0, 0);
                system_call_3(syscall(dup3), slave, 1, 0);
                system_call_3(syscall(dup3), slave, 2, 0);
                system_call_1(syscall(close), master);

                system_call_3(syscall(execve), (positive)SHELL,
                              (positive)argv, (positive)envp);
                system_call_1(syscall(exit), 127);
        }

        system_call_1(syscall(close), slave);

        window->region = WINDOW_CENTRED;
        window_damage(window, 0, ROWS);
        window_commit(window);

        p8 from_shell[1024];
        struct window_key typed[WINDOW_KEYS];
        timespec nap = {0, 4000000};

        cursor_show();
        window_damage(window, 0, ROWS);
        window_commit(window);

        for (;;)
        {
                b32 changed = false;
                b32 gone = false;
                unsigned int keys = 0;

                touched_top = ROWS;
                touched_bottom = 0;

                if (window->columns != COLUMNS || window->rows != ROWS)
                        regrid(master);

                term_follow_modes(master);

                /*
                        Taken out of the ring first, drawn second.

                        The cursor is a cell with its colours the wrong way
                        round, so it has to come off before anything writes
                        where it is -- and taking it off marks a row changed
                        whether or not one was. Reading the keys before
                        deciding is how the loop can tell the difference
                        between a keystroke and four milliseconds passing.
                */
                while (keys < WINDOW_KEYS && window_key(window, address_of typed[keys]))
                        keys++;

                if (keys)
                        cursor_hide();

                for (unsigned int i = 0; i < keys; i++)
                        if (typed[i].flags & WINDOW_KEY_DOWN)
                        {
                                term_key(typed[i].character, typed[i].code);

                                if (!term_send(master))
                                {
                                        gone = true;
                                        break;
                                }
                        }

                for (;;)
                {
                        bipolar got = system_call_3(syscall(read), master,
                                                    (positive)from_shell,
                                                    sizeof(from_shell));

                        if (got > 0)
                        {
                                if (!changed)
                                        cursor_hide();

                                for (bipolar i = 0; i < got; i++)
                                        consume(from_shell[i]);

                                changed = true;
                                continue;
                        }

                        /*
                                Nothing waiting is EAGAIN and only EAGAIN.

                                Everything else is the shell gone -- zero, or
                                EIO once the session went with it -- and
                                reading that as nothing waiting left this
                                spinning on a dead pty forever, with a zombie
                                behind it and a window nothing could dismiss.
                        */
                        if (got != -EAGAIN && got != -EINTR)
                                gone = true;

                        break;
                }

                // A report the far end asked for, which is bytes going the
                // way keys go.
                if (!gone && !term_send(master))
                        gone = true;

                // The line editor draws where the shell would have echoed, so
                // what it touched is what says the screen changed.
                if (touched_bottom > touched_top)
                        changed = true;

                /*
                        Only when something actually arrived.

                        Taking the cursor off and putting it back marks a row
                        changed whether or not anything did, and asking for a
                        redraw every four milliseconds because of that is a
                        whole screen recomposed two hundred and fifty times a
                        second for nothing.
                */
                if (changed)
                {
                        cursor_show();
                        window_damage(window, touched_top, touched_bottom - touched_top);
                        window_flush(window);
                }

                if (gone)
                        break;

                system_call_2(syscall(nanosleep), (positive)address_of nap, 0);
        }

        positive status = 0;

        system_wait4_retry(child, address_of status, 0, null);
        system_call_1(syscall(close), master);
        window_close(window);

        return 0;
}

// window ---------------------------------------------------------
// Colours in no compositor palette, so a screenshot can tell them apart.
#define INK_BACK 0x00ff9900
#define INK_FRONT 0x000066cc
#define INK_BARE 0x0022bb55

static void fill(struct window *window, unsigned int colour)
{
        unsigned int *pixels = window_pixels(window);

        for (unsigned int y = 0; y < window->height; y++)
                for (unsigned int x = 0; x < window->width; x++)
                        pixels[y * window->pitch + x] = colour;
}

static void hold(long seconds)
{
        long timespec[2] = {seconds, 0};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

static void hold_ms(long milliseconds)
{
        long timespec[2] = {0, milliseconds * 1000000};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

static b32 screen_window()
{
        struct window *back = window_open(400, 260);
        struct window *front = window_open(200, 120);
        struct window *bare = window_open(160, 100);

        if (!back || !front || !bare)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        fill(back, INK_BACK);
        back->region = WINDOW_CENTRED;
        back->edge = 16;
        string_copy_max_end((p8 address_to)back->title,
                            (string_address) "Centred, rounded",
                            WINDOW_TITLE_MAX - 1);
        window_commit(back);

        fill(front, INK_FRONT);
        front->x = back->x + 60;
        front->y = back->y + 60;
        string_copy_max_end((p8 address_to)front->title,
                            (string_address) "On top", WINDOW_TITLE_MAX - 1);
        window_commit(front);

        // No frame, so no titlebar, no border, and nothing to drag it by.
        fill(bare, INK_BARE);
        bare->style = 0;
        bare->x = 100;
        bare->y = 620;
        window_commit(bare);

        // Show what a keyboard says, in the titlebar of the window that has
        // focus. Nothing is polled: keys arrive in the page.
        for (int i = 0; i < 240; i++)
        {
                struct window_key key;
                unsigned int typed = 0;
                char line[WINDOW_TITLE_MAX];
                unsigned int at = 0;

                while (window_key(back, &key))
                        if ((key.flags & WINDOW_KEY_DOWN) && key.character >= ' ')
                                typed = key.character;

                if (typed)
                {
                        const char *label = "typed: ";

                        while (label[at])
                        {
                                line[at] = label[at];
                                at++;
                        }

                        line[at++] = (char)typed;
                        line[at] = 0;
                        string_copy_max_end((p8 address_to)back->title,
                                            (string_address)line,
                                            WINDOW_TITLE_MAX - 1);
                        window_commit(back);
                }

                hold_ms(25);
        }

        // Put one away and let the other cover its display.
        front->style |= WINDOW_MINIMIZED;
        window_commit(front);

        bare->style |= WINDOW_FULLSCREEN;
        window_commit(bare);

        hold(7);

        window_close(bare);
        window_close(front);
        window_close(back);
        return 0;
}

// text ---------------------------------------------------------
// A window of text: the program writes cells, Canvas draws the glyphs.
//
// The size is the compositor's, not this program's: dragging an edge changes
// how many cells there are, and the layout is done again at whatever it
// becomes. window_grid is what says the cells are in that shape now.
#define TEXT_COLUMNS_WANTED 60
#define TEXT_ROWS_WANTED 18
#define TYPED_MAX 46

static unsigned int columns, rows;
static char typed[TYPED_MAX];
static unsigned int at;

// The rows are the last lines of the ring, the same as any window of cells.
static struct window_cell *text_row(unsigned int row)
{
        return window_line(window, window->head - rows + row);
}

static void text_write(unsigned int row, unsigned int column,
                       unsigned int character, unsigned char ink,
                       unsigned char paper)
{
        struct window_cell *cell = text_row(row) + column;
        unsigned int *length = window_length(window, window->head - rows + row);

        // Every cell up to this one has to exist, because nothing past the
        // length of a line is drawn.
        while (*length < column)
        {
                struct window_cell *blank = text_row(row) + *length;

                blank->character = ' ';
                blank->ink = 7;
                blank->paper = 0;
                *length += 1;
        }

        cell->character = character;
        cell->ink = ink;
        cell->paper = paper;

        if (*length < column + 1)
                *length = column + 1;
}

static void say(unsigned int row, unsigned int column, const char *text,
                unsigned char ink, unsigned char paper)
{
        unsigned int i;

        if (row >= rows)
                return;

        for (i = 0; text[i] && column + i < columns; i++)
                text_write(row, column + i, (unsigned char)text[i], ink, paper);

        window_damage(window, row, 1);
}

static void lay_out(void)
{
        unsigned int row;

        columns = window->columns;
        rows = window->rows;

        // Emptying a line is its length, not its cells.
        for (row = 0; row < rows; row++)
                *window_length(window, window->head - rows + row) = 0;

        for (unsigned int i = 0; i < 8; i++)
                say(1, 2 + i * 6, "colour", (unsigned char)(8 + i), 0);

        say(3, 2, "Moonwater Canvas draws these glyphs.", 15, 0);
        say(4, 2, "This window is cells, not pixels.", 7, 0);
        say(6, 2, "Type here:", 11, 0);

        for (unsigned int i = 0; i < at; i++)
                if (rows > 6 && 13 + i < columns)
                        text_write(6, 13 + i, (unsigned char)typed[i], 15, 4);

        window_grid(window, columns, rows);
        window_damage(window, 0, rows);
}


static b32 screen_text()
{
        window = window_open_text(TEXT_COLUMNS_WANTED, TEXT_ROWS_WANTED);

        if (!window)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        lay_out();
        window->region = WINDOW_CENTRED;
        window_commit(window);

        for (int tick = 0; tick < 800; tick++)
        {
                struct window_key key;
                b32 changed = 0;

                if (window_regrid(window))
                {
                        lay_out();
                        changed = 1;
                }

                while (window_key(window, &key))
                {
                        if (!(key.flags & WINDOW_KEY_DOWN) || key.character < ' ')
                                continue;

                        if (at < TYPED_MAX && rows > 6 && 13 + at < columns)
                        {
                                text_write(6, 13 + at, key.character, 15, 4);
                                typed[at++] = (char)key.character;
                                window_damage(window, 6, 1);
                                changed = 1;
                        }
                }

                // Now, not on the compositor's clock: a letter that has been
                // typed should not wait for a frame it has no reason to be on.
                if (changed)
                        window_flush(window);

                hold_ms(10);
        }

        window_close(window);
        return 0;
}

// pointer ---------------------------------------------------------
// Reports how long the kernel takes from a pointer event arriving to the
// cursor being on screen. Move the mouse, then run this.
static b32 screen_pointer()
{
        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        if (device < 0)
        {
                string_format(log, "cannot open %s: %b\n", SPARK_DEVICE, device);
                log_flush();
                return 1;
        }

        struct input_stats stats;

        if (system_call_3(syscall(ioctl), device, SPARK_IOCTL_INPUT_STATS,
                          (positive)address_of stats) != 0)
        {
                string_format(log, "could not read input stats\n");
                log_flush();
                return 1;
        }

        // Drawing happens whether or not anything has touched the mouse.
        string_format(log, "composes         %p\n", stats.composes);
        string_format(log, "compose ns       %p\n", stats.compose_ns);
        string_format(log, "pixels painted   %p\n", stats.painted);
        string_format(log, "runs             %p\n", stats.runs);
        string_format(log, "driver ns        %p\n", stats.driver_ns);
        string_format(log, "text ns          %p\n", stats.text_ns);

        if (!stats.events)
        {
                string_format(log, "no pointer movement seen yet\n");
                log_flush();
                return 0;
        }

        string_format(log, "pointer events   %p\n", stats.events);
        string_format(log, "event to screen  %p ns mean, %p ns worst\n",
                      stats.mean_ns, stats.worst_ns);
        string_format(log, "  queued         %p ns\n", stats.queue_ns);
        string_format(log, "  drawing        %p ns\n", stats.draw_ns);
        string_format(log, "  flush          %p ns\n", stats.flush_ns);
        string_format(log, "counts reported  %p\n", stats.counts);
        string_format(log, "pixels moved     %p\n", stats.moved);
        log_flush();
        return 0;
}
