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
        the ones every window of cells has, in pane.c. What is here is only
        what turns a stream of bytes into lines -- which is the whole of what a
        console is, and none of what a window is.
*/

// Indices into the sixteen a terminal has always had: light grey on black.
#define CONSOLE_INK 7
#define CONSOLE_PAPER 0

#define CONSOLE_COLUMNS 100
#define CONSOLE_ROWS 30

static struct pane *console_pane;
static DEFINE_SPINLOCK(console_cells);

// How far along the line being written. The line itself is the newest in the
// ring, so there is no cursor to keep beyond this.
static unsigned int console_column;

static void console_break(struct pane *pane)
{
        console_column = 0;
        pane_set_length(pane, pane->head, 0);
        pane->head++;
}

static void console_put_byte(struct pane *pane, char c)
{
        struct window_cell *cell;

        if (c == '\n')
        {
                console_break(pane);
                return;
        }

        /*
                A carriage return on its own is a line the kernel is rewriting,
                so the line is as long as what is written next and not as long
                as what was there before. Nothing past the length is drawn,
                which is what saves this from having to blank the rest of it.
        */
        if (c == '\r')
        {
                console_column = 0;
                pane_set_length(pane, pane->head - 1, 0);
                return;
        }

        // The tab stops are the ordinary eight.
        if (c == '\t')
        {
                do
                {
                        console_put_byte(pane, ' ');
                }
                while (console_column & 7);

                return;
        }

        if (c < ' ' || (unsigned char)c > '~')
                return;

        // At the end of the ring's line and not at the edge of the window: a
        // line is kept as long as it was written and folded when it is drawn,
        // so what the window is doing to it is not this file's business.
        if (console_column >= pane->stride)
                console_break(pane);

        cell = pane_line(pane, pane->head - 1) + console_column;
        cell->character = (unsigned char)c;
        cell->ink = CONSOLE_INK;
        cell->paper = CONSOLE_PAPER;
        cell->flags = 0;

        console_column++;
        pane_set_length(pane, pane->head - 1, console_column);
}

static void console_put_line(struct console *console, const char *text,
                             unsigned int count)
{
        struct pane *pane = READ_ONCE(console_pane);
        unsigned long flags;
        unsigned int i;

        if (!pane || !pane->cells)
                return;

        spin_lock_irqsave(&console_cells, flags);

        for (i = 0; i < count; i++)
                console_put_byte(pane, text[i]);

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

        // Set here rather than read from a shared page, because an owned
        // pane has no program behind it to have named itself.
        strscpy(pane->title, "kernel log", WINDOW_TITLE_MAX);
        pane->title_length = strlen(pane->title);

        WRITE_ONCE(console_pane, pane);

        register_console(&canvas_console);
        console_registered = true;
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
