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

        What the window shows is a view onto a ring of lines, not the lines
        themselves. The thing worth reading is nearly always the part that has
        already gone past, so the ring keeps far more than fits and the wheel
        moves the view over it.
*/

// Indices into the sixteen a terminal has always had: light grey on black.
#define CONSOLE_INK 7
#define CONSOLE_PAPER 0

#define CONSOLE_COLUMNS 100
#define CONSOLE_ROWS 30

// Enough to hold a boot. Every line costs its columns in cells, so this is
// about four hundred kilobytes at the width above -- paid once, and only when
// there is a display to put it on.
#define CONSOLE_HISTORY 512

static struct pane *console_pane;
static DEFINE_SPINLOCK(console_cells);

static struct window_cell *console_ring;
static unsigned int console_width;
static unsigned int console_head;   // the line being written
static unsigned int console_filled; // how many the ring holds
/*
        Where the view is, as a line number rather than a distance back.

        It was a distance from the newest line, and the newest line moves: a
        line arriving while somebody was reading history slid what they were
        reading out from under them, one row per message, and scrolling back
        against a talkative kernel got nowhere. An absolute anchor stays where
        it was put. CONSOLE_LIVE means follow the end instead.
*/
#define CONSOLE_LIVE ((unsigned int)-1)

static unsigned int console_view = CONSOLE_LIVE;
static unsigned int console_column;

static struct window_cell *console_line(unsigned int index)
{
        return console_ring + (index % CONSOLE_HISTORY) * console_width;
}

static void console_blank(struct window_cell *line)
{
        unsigned int i;

        for (i = 0; i < console_width; i++)
        {
                line[i].character = ' ';
                line[i].ink = CONSOLE_INK;
                line[i].paper = CONSOLE_PAPER;
                line[i].flags = 0;
        }
}

/*
        The view, copied into the cells the compositor draws.

        Only when the view is live: scrolled back, what arrives changes the
        ring and not what is being looked at, which is the whole point of
        having scrolled back.
*/
static void console_show(struct pane *pane)
{
        unsigned int rows = pane->grid_rows;
        unsigned int newest = console_view == CONSOLE_LIVE ? console_head
                                                           : console_view;
        unsigned int r;

        for (r = 0; r < rows; r++)
        {
                // The bottom row is the newest, less however far back the
                // view has been moved.
                unsigned int from = newest + CONSOLE_HISTORY - (rows - 1 - r);

                memcpy(pane->cells + r * pane->grid_columns, console_line(from),
                       console_width * sizeof(struct window_cell));
        }

        pane->damage_row = 0;
        pane->damage_rows = rows;
}

static void console_break(void)
{
        console_column = 0;
        console_head++;
        console_blank(console_line(console_head));

        if (console_filled < CONSOLE_HISTORY - 1)
                console_filled++;
}

static void console_put_byte(char c)
{
        struct window_cell *cell;

        if (c == '\n')
        {
                console_break();
                return;
        }

        // A carriage return on its own is a line the kernel is rewriting, and
        // the tab stops are the ordinary eight.
        if (c == '\r')
        {
                console_column = 0;
                return;
        }

        if (c == '\t')
        {
                do
                {
                        console_put_byte(' ');
                }
                while (console_column & 7);

                return;
        }

        if (c < ' ' || (unsigned char)c > '~')
                return;

        if (console_column >= console_width)
                console_break();

        cell = console_line(console_head) + console_column;
        cell->character = (unsigned char)c;
        cell->ink = CONSOLE_INK;
        cell->paper = CONSOLE_PAPER;
        cell->flags = 0;

        console_column++;
}

static void console_put_line(struct console *console, const char *text,
                             unsigned int count)
{
        struct pane *pane = READ_ONCE(console_pane);
        unsigned long flags;
        unsigned int i;

        if (!pane || !pane->cells || !console_ring)
                return;

        spin_lock_irqsave(&console_cells, flags);

        for (i = 0; i < count; i++)
                console_put_byte(text[i]);

        if (console_view == CONSOLE_LIVE)
                console_show(pane);

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

/*
        The wheel, in lines. Positive is away from the hand, which is back
        through what has already been said.

        Clamped to what the ring holds rather than to what was written, so a
        console that has wrapped stops at the oldest line it still has instead
        of scrolling into lines it gave away.
*/
static _Bool console_scroll(struct pane *pane, int lines)
{
        unsigned long flags;
        unsigned int rows = pane->grid_rows;
        unsigned int was;
        unsigned int at;
        unsigned int oldest;

        if (pane != READ_ONCE(console_pane) || !console_ring)
                return false;

        spin_lock_irqsave(&console_cells, flags);

        was = console_view;
        at = was == CONSOLE_LIVE ? console_head : was;

        // The oldest line still in the ring, and never further back than the
        // window is tall or the view would show blanks above the first line.
        oldest = console_filled > rows ? console_head - (console_filled - rows)
                                       : console_head;

        if (lines > 0)
                at = (at - oldest) >= (unsigned int)lines ? at - lines : oldest;
        else if (at + (unsigned int)-lines >= console_head)
                at = console_head;
        else
                at += (unsigned int)-lines;

        // Back at the end is following again, not sitting on the last line:
        // what arrives next should appear.
        console_view = at == console_head ? CONSOLE_LIVE : at;

        if (console_view != was)
                console_show(pane);

        spin_unlock_irqrestore(&console_cells, flags);

        return console_view != was;
}

/*
        Where the view sits in what there is, for something to draw a bar with.

        In lines rather than pixels: what the bar is made of is the compositor's
        business, and this file has no idea how tall a row is.

        False when there is nothing to scroll, which is also when a bar would
        say nothing worth the pixels.
*/
static _Bool console_extent(struct pane *pane, unsigned int *first,
                            unsigned int *shown, unsigned int *total)
{
        unsigned long flags;
        unsigned int rows = pane->grid_rows;
        unsigned int at;

        if (pane != READ_ONCE(console_pane) || !console_ring)
                return false;

        spin_lock_irqsave(&console_cells, flags);

        *total = console_filled;
        *shown = rows;
        at = console_view == CONSOLE_LIVE ? console_head : console_view;

        // How far the bottom of the view is from the oldest line held.
        *first = console_filled > rows
                     ? console_filled - rows - (console_head - at)
                     : 0;

        spin_unlock_irqrestore(&console_cells, flags);

        return *total > *shown;
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
        unsigned int i;

        if (console_registered || console_pane)
                return;

        pane = pane_create_owned(CONSOLE_COLUMNS, CONSOLE_ROWS);

        if (!pane)
        {
                log_canvas("no room for a console window\n");
                return;
        }

        // The pane decides its own width: a small desktop gives fewer columns
        // than were asked for, and the ring has to match what is drawn.
        console_width = pane->grid_columns;
        console_ring = vzalloc((unsigned long)CONSOLE_HISTORY * console_width *
                               sizeof(struct window_cell));

        if (!console_ring)
        {
                log_canvas("no room for a console history\n");
                return;
        }

        for (i = 0; i < CONSOLE_HISTORY; i++)
                console_blank(console_line(i));

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

        vfree(console_ring);
        console_ring = NULL;
}
