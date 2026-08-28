/*
        The screen the machine always has.

        A window the compositor owns and writes itself, carrying the kernel
        log. Everything else on the desktop belongs to a program: if userspace
        never starts, or starts and dies, or the display comes up and nothing
        claims it, the desktop is empty and there is nothing to read. This is
        what is there instead.

        It is also the answer to a black screen on a machine with no serial
        port. Canvas says why it could not draw through printk, and printk now
        has somewhere to go that is not a serial line -- so the reason lands on
        the monitor of the machine that has the problem.

        A console write arrives from any context, including one that must not
        sleep, so this puts characters into cells and asks for a frame. The
        drawing happens later on the compositor's own thread, which is the only
        thing here that touches the display.

        The lines, the ring they sit in and the wheel that moves over it are
        the ones every window of cells has, in pane.c, and what turns a stream
        of bytes into lines is the emulator in sh/term.c -- the same one the
        shell's terminal is. So a carriage return, a tab and an escape sequence
        mean here what they mean there, and this file is only the wiring.
*/

#define CONSOLE_COLUMNS 100
#define CONSOLE_ROWS 30

static struct pane *console_pane;
static DEFINE_SPINLOCK(console_cells);

static void console_put_line(struct console *console, const char *text,
                             unsigned int count)
{
        struct pane *pane = READ_ONCE(console_pane);
        unsigned long flags;
        unsigned int i;

        if (!pane || !pane->cells)
                return;

        spin_lock_irqsave(&console_cells, flags);

        /*
                A newline here is a line feed and nothing else.

                The emulator is what sits behind a pty, and the line discipline
                in front of one turns \n into \r\n before it ever arrives.
                printk has no line discipline: it says \n and means the start
                of the next line. Fed raw, the cursor drops a row and stays in
                the column it was in, so every message begins where the one
                before it ended and the log walks off to the right until it
                wraps. The return is put in here, which is where the pty that
                is missing would have put it.
        */
        for (i = 0; i < count; i++)
        {
                if (text[i] == '\n')
                        consume('\r');

                consume((unsigned char)text[i]);
        }

        // The emulator counts the ring on in the page; the compositor reads
        // its own copy, and this is where the two meet.
        pane->head = window->head;

        /*
                Only while the view is at the end: scrolled back, what arrives
                changes the ring and not what is being looked at, which is the
                whole point of having scrolled back.
        */
        if (pane->view == PANE_LIVE)
        {
                pane->damage_row = 0;
                pane->damage_rows = pane->grid_rows;
        }

        spin_unlock_irqrestore(&console_cells, flags);

        /*
                Asking for a frame rather than drawing one.

                This can be called with interrupts off, from a spinlock, or
                from the middle of a panic. wake_up_process is safe in all
                three; a modeset is not.
        */
        atomic_set(&desktop.frame_pending, 1);
        canvas_thread_wake();
}

static struct console canvas_console = {
        .name = "canvas",
        .write = console_put_line,
        /*
                CON_ENABLED because this one is not a choice.

                Naming console= on the command line turns off every console
                the user did not name, and a machine booted with console=ttyS0
                would register this and never enable it -- which is exactly
                the machine that has no screen output to spare. The point of
                this window is to be there when nothing else is.
        */
        .flags = CON_PRINTBUFFER | CON_ANYTIME | CON_ENABLED,
        .index = -1,
};

static _Bool console_registered;

/*
        Opened once the desktop has a size, and never closed while the module
        is loaded. CON_PRINTBUFFER replays everything printk has kept, so the
        window comes up carrying the boot it missed rather than starting from
        whatever was printed next.
*/
static void console_start(void)
{
        struct pane *pane;

        if (console_registered || console_pane)
                return;

        pane = pane_create(0, 0, CONSOLE_COLUMNS, CONSOLE_ROWS, true);

        if (!pane)
        {
                log_canvas("no room for a console window\n");
                return;
        }

        /*
                The emulator writes through the page every window of cells has,
                so it is pointed at this one's and started the way the terminal
                starts.

                Wrapped at the width the window is drawn at, and at nothing
                else. The byte handler this replaced kept a line as long as it
                was written and let the compositor fold it, which is right for
                something that only ever appends -- it has no cursor and does
                not care which row anything lands on. An emulator does: it owns
                a grid, and every erase, every scroll and every cursor move is
                said in rows of it. Wrap at one width and fold at another and
                the two disagree about where row four is, by one row for every
                line long enough to fold.
        */
        window = pane->mapping;
        COLUMNS = min(pane->columns, pane->stride);
        ROWS = pane->rows;
        full_reset();

        // Set here rather than read from a shared page, because an owned
        // pane has no program behind it to have named itself.
        strscpy(pane->title, "kernel log", WINDOW_TITLE_MAX);
        pane->title_length = strlen(pane->title);

        WRITE_ONCE(console_pane, pane);

        register_console(&canvas_console);
        console_registered = true;
}

/*
        The window changed shape.

        An owned pane has no program to be told, so pane_regrid sets the grid
        and returns; the emulator behind this one still believes the width it
        was started at. It is told here, through the same page a program would
        have been told through.
*/
static void console_regrid(struct pane *pane)
{
        unsigned long flags;

        if (pane != READ_ONCE(console_pane) || !pane->cells)
                return;

        spin_lock_irqsave(&console_cells, flags);

        window->columns = pane->columns;
        window->rows = pane->rows;

        regrid(-1);

        pane->head = window->head;

        spin_unlock_irqrestore(&console_cells, flags);
}

static void console_stop(void)
{
        if (!console_registered)
                return;

        unregister_console(&canvas_console);
        console_registered = false;

        // The pane is the desktop's to free, like any other window.
        WRITE_ONCE(console_pane, NULL);
}
