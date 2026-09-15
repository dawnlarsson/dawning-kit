/*
        The terminal.

        The pty and the escape sequences are here; the glyphs are not. The
        program writes characters and colour indices into cells and Canvas
        draws them, so there is no font in this file and no framebuffer.

        The cells are the ring every window of cells has. The rows are the last
        of it, so scrolling is a store to one number rather than a copy of the
        screen, what goes off the top is still there for the wheel to come back
        to, and the compositor draws the bar down the side -- none of which is
        anything this file does, and all of which it now has.

        Nothing below makes a system call. The emulator is a byte stream in and
        a grid of cells out, so it runs the same whether a pty is driving it or
        a test is, and the term lane of test/run drives it with no kernel underneath at
        all. The two functions at the bottom are the exception and they are the
        only ones: everything the parser, the keys and the line editor do is
        stores into a page.
*/

#define COLUMNS_WANTED 80
#define ROWS_WANTED 24
#define SHELL "/shell"

#ifndef EINTR
#define EINTR 4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

// Three below are the shell's, and the kernel's console has no use for them.
// Not the attribute that keeps a symbol: this one lets the compiler drop what
// it finds no call to, and only quiets the warning about having found none.
#define SPARE __attribute__((unused))
// The kernel provides its own ioctl constants and window-size type.
#ifndef KERNEL_MODE
#define TIOCSWINSZ 0x5414u

typedef struct
{
        unsigned short rows, columns, x_pixels, y_pixels;
} winsize;
#endif

static struct window *window;

// However many the compositor says there are, which changes when the window
// is resized.
static unsigned int COLUMNS, ROWS;
static unsigned int row, column;
static unsigned char ink = 7, paper = 0;
static unsigned short style;
static b32 reverse;
static unsigned int last_character;
static unsigned int touched_top, touched_bottom;

/*
        The rows a scroll happens between, and the rest of what a mode is.

        DECSTBM names a top and a bottom and everything that scrolls scrolls
        between them. When they are the whole screen -- which is what they are
        until something says otherwise -- moving on is the ring's own store;
        anywhere narrower it is a copy, because a region is by definition the
        screen not moving as one.
*/
static unsigned int region_top, region_bottom;
static b32 autowrap = true;
static b32 insert_mode;
static b32 cursor_visible = true;
static b32 application_keys;
/* DEC private mode 2026: programs may update a complete frame without the
   compositor presenting the intermediate rows. The cells are still written
   immediately; only publication is held by screen.c until the mode ends. */
static b32 synchronized_output;
static unsigned int mouse_mode;
static b32 mouse_sgr;
static b32 focus_events;
static b32 origin_mode;

#define CHARSET_ASCII 0
#define CHARSET_ACS 1
static unsigned char charset_g0;
static unsigned char charset_g1;
static unsigned char charset_gl;
static p8 escape_kind;
static p8 csi_intermediate;

// Where the block cursor was put, so it can be taken back off.
static unsigned int shown_row, shown_column;
static b32 shown;

/* A cursor with the colours it writes in: one saved by ESC 7 and CSI s, one
   kept for the primary screen while the alternate one is up. They are two, or
   a program saving its own cursor lost the shell's when it ended. */
struct cursor_state
{
        unsigned int row, column;
        unsigned char ink, paper;
        unsigned short style;
        b32 reverse;
};

static struct cursor_state cursor_saved = {0, 0, 7, 0, 0, false};
static struct cursor_state cursor_primary = {0, 0, 7, 0, 0, false};

// Tab stops, one byte a column, which is what makes HTS and TBC mean
// anything. Wide enough for a column of pixels on any screen this runs on.
#define TAB_STOPS 1024
static p8 tab_stop[TAB_STOPS] __attribute__((aligned(8)));

static fn touch(unsigned int at)
{
        if (at < touched_top)
                touched_top = at;

        if (at + 1 > touched_bottom)
                touched_bottom = at + 1;
}

static fn touch_all()
{
        touched_top = 0;
        touched_bottom = ROWS;
}

/*
        A row, in the ring.

        The screen is the last ROWS lines of it and ROWS is this program's own
        number, not one read back out of the shared page: the two differ for as
        long as a resize takes to be answered, and a row addressed at the
        compositor's count while the cells are still laid out at this one is a
        line taken from somewhere else entirely.
*/
#define row_cells(row) window_line(window, window->head - ROWS + (row))
#define row_length(row) window_length(window, window->head - ROWS + (row))

/*
        A cell is eight bytes and the ring begins 4096 bytes into its
        page-aligned mapping.  Every row therefore satisfies the aligned-u64
        fill contract, including on RV64 implementations that trap unaligned
        doubleword stores.
*/
#define BLANK_CELL_WORD ((positive)' ' | ((positive)7 << 32))

static PURE positive blank_cell_word()
{
        positive clear_ink = reverse ? paper : ink;
        positive clear_paper = reverse ? ink : paper;

        return (positive)' ' | (clear_ink << 32) | (clear_paper << 40);
}

/* Erase in the colours in force, so a program can clear a coloured panel. */
static fn cells_clear(unsigned int r, unsigned int first, unsigned int count)
{
        if (!count)
                return;

        memory_fill_u64_aligned(row_cells(r) + first, count,
                                blank_cell_word());
        touch(r);
}

/* Cells never written, which are blank in the colours a reset gives them:
   the colours in force belong to what an erase clears, not to what a tab or
   a cursor movement steps over. */
static fn cells_blank(unsigned int r, unsigned int first, unsigned int count)
{
        if (!count)
                return;

        memory_fill_u64_aligned(row_cells(r) + first, count, BLANK_CELL_WORD);
        touch(r);
}

/*
        Nothing moves.

        The screen ends at the newest line, so the next one is the whole of
        it, and the line that was at the top is still in the ring for the wheel
        to come back to. This used to be a copy of every cell on the screen
        with the top row thrown away.
*/
static fn ring_scroll()
{
        window_scroll(window);
        touch_all();
}

static fn row_copy(unsigned int to, unsigned int from)
{
        struct window_cell address_to source = row_cells(from);
        struct window_cell address_to target = row_cells(to);
        unsigned int length = address_to row_length(from);

        memory_copy(target, source,
                    (positive)length * sizeof(struct window_cell));

        address_to row_length(to) = length;
}

static fn row_blank(unsigned int r)
{
        address_to row_length(r) = 0;
        touch(r);
}

/*
        The region moves up, and the whole screen does it by not moving.

        Only when the region is the screen can the ring be named forward, and
        only then is what leaves the top kept. A narrower region is a copy and
        what leaves it is gone, which is what a region means.
*/
static fn scroll_up(unsigned int count)
{
        if (region_top == 0 && region_bottom == ROWS)
        {
                /* Canvas calls this with interrupts off. Beyond ROWS the
                   visible result is already blank, so never let a hostile
                   CSI count turn that critical section into a long loop. */
                if (count > ROWS)
                        count = ROWS;

                while (count--)
                        ring_scroll();

                return;
        }

        if (count > region_bottom - region_top)
                count = region_bottom - region_top;

        for (unsigned int r = region_top; r + count < region_bottom; r++)
                row_copy(r, r + count);

        for (unsigned int r = region_bottom - count; r < region_bottom; r++)
                row_blank(r);

        touch_all();
}

static fn scroll_down(unsigned int count)
{
        if (count > region_bottom - region_top)
                count = region_bottom - region_top;

        for (unsigned int r = region_bottom; r-- > region_top + count;)
                row_copy(r, r - count);

        for (unsigned int r = region_top; r < region_top + count; r++)
                row_blank(r);

        touch_all();
}

// One line down, and the bottom of the region is where that scrolls.
static fn line_feed()
{
        if (row + 1 == region_bottom)
                scroll_up(1);
        else if (row + 1 < ROWS)
                row++;
}

/*
        Every cell up to a column, made to exist.

        Nothing past the length of a line is drawn and the cells out there are
        whatever the ring last held, so anything that arrives at a column
        without having written its way there -- a tab, a cursor moved along and
        printed at -- has to clear what it stepped over first.
*/
static fn reach(unsigned int r, unsigned int to)
{
        unsigned int address_to length = row_length(r);

        if (address_to length < to)
        {
                cells_blank(r, address_to length, to - address_to length);
                address_to length = to;
        }
}

static fn open_gap(unsigned int at, unsigned int count)
{
        unsigned int address_to length = row_length(row);
        struct window_cell address_to cells;
        unsigned int last;

        if (at >= COLUMNS || !count)
                return;

        if (count > COLUMNS - at)
                count = COLUMNS - at;

        reach(row, at);

        // As far as what was there is pushed to, and no further: a line is as
        // long as it was written and padding it to the width of the window is
        // what the ring exists not to do.
        last = address_to length + count;

        if (last > COLUMNS)
                last = COLUMNS;

        reach(row, last);
        cells = row_cells(row);

        memory_copy(cells + at + count, cells + at,
                    (positive)(last - at - count) *
                        sizeof(struct window_cell));

        cells_clear(row, at, min(count, last - at));

        touch(row);
}

static fn put(unsigned int character)
{
        struct window_cell address_to cell;
        unsigned int address_to length;

        if (column >= COLUMNS)
        {
                // DECAWM off pins the cursor to the last column and every
                // further character overwrites it, which is what stops a
                // status line from scrolling the screen it is drawn on.
                if (!autowrap)
                        column = COLUMNS - 1;
                else
                {
                        column = 0;
                        line_feed();
                }
        }

        reach(row, column);

        if (insert_mode)
                open_gap(column, 1);

        cell = row_cells(row) + column;
        cell->character = character;
        cell->ink = reverse ? paper : ink;
        cell->paper = reverse ? ink : paper;
        cell->flags = style;
        last_character = character;

        touch(row);
        column++;

        length = row_length(row);

        if (address_to length < column)
                address_to length = column;
}

/*
        What goes back up the pty.

        A terminal answers questions -- where the cursor is, what it claims to
        be -- and a key is bytes rather than a call, so both land here and the
        one function in this file that has a file descriptor sends them. The
        emulator itself still makes no system call.
*/
#ifdef KERNEL_MODE
#define TO_SHELL_MAX 2048
static p8 to_shell[TO_SHELL_MAX];
#else
static p8 address_to to_shell;
static positive to_shell_room;
#endif
static positive to_shell_length;

static fn emit_bytes(address_any data, positive length)
{
#ifdef KERNEL_MODE
        positive room = TO_SHELL_MAX - to_shell_length;
        positive take = length < room ? length : room;

        if (take)
                memory_copy_apart(to_shell + to_shell_length, data, take);

        to_shell_length += take;
#else
        if (!length ||
            !array_store_reserve(to_shell, to_shell_room, to_shell_length,
                                 to_shell_length + length, 64))
                return;

        memory_copy_apart(to_shell + to_shell_length, data, length);
        to_shell_length += length;
#endif
}

static fn emit(unsigned int byte)
{
        p8 one = (p8)byte;

        emit_bytes(address_of one, 1);
}

#define emit_literal(text) \
        emit_bytes((address_any)(text), sizeof(text) - 1)

/* One CSI parameter machine serves both terminal output and editor input.
   The final byte is deliberately not stored: it is the caller's action, while
   digits, separators and the private marker are the reusable transition. */
#define TERMINAL_PARAMETERS 16
typedef struct
{
        unsigned int value[TERMINAL_PARAMETERS];
        unsigned int count;
        p8 marker;
} terminal_parameters;

#define terminal_parameters_reset(sequence)                                 \
        ((sequence)->count = 0, (sequence)->marker = 0,                     \
         (sequence)->value[0] = 0)

static inline INLINE bool terminal_parameters_take(
    terminal_parameters address_to sequence, unsigned int byte)
{
        if (byte >= '0' && byte <= '9')
        {
                unsigned int digit = byte - '0';
                unsigned int address_to value;

                if (!sequence->count)
                        sequence->count = 1;

                value = address_of sequence->value[sequence->count - 1];

                // CSI parameters have no protocol-level machine-word limit.
                // Saturation preserves the only useful meaning of a larger
                // value -- past the edge -- instead of wrapping it to a small
                // cursor position or erase count.
                if (address_to value > (~0u - digit) / 10)
                        address_to value = ~0u;
                else
                        address_to value = address_to value * 10 + digit;

                return false;
        }

        if (byte == ';' || byte == ':')
        {
                if (!sequence->count)
                        sequence->count = 1;

                if (sequence->count < TERMINAL_PARAMETERS)
                        sequence->value[sequence->count++] = 0;

                return false;
        }

        if (byte == '?' || byte == '<' || byte == '=' || byte == '>')
        {
                sequence->marker = (p8)byte;
                return false;
        }

        return byte >= '@' && byte <= '~';
}

static terminal_parameters terminal_csi;
static b32 in_escape, in_csi, in_string, escape_intermediate;

// A string sequence ends at ST, and ST is two bytes with an ESC in front.
static b32 string_escape;
static p8 osc_bytes[WINDOW_TITLE_MAX];
static unsigned int osc_length;

static CONST unsigned int acs_character(unsigned int c)
{
        static const unsigned int map[] = {
            0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
            0x2592, 0x2603, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
            0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
            0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
        };

        if (c < 0x60 || c > 0x7e)
                return c;

        return map[c - 0x60];
}

static fn osc_finish()
{
        unsigned int i = 0;
        unsigned int command = 0;
        unsigned int n;

        while (i < osc_length && osc_bytes[i] >= '0' && osc_bytes[i] <= '9')
                command = command * 10 + (unsigned int)(osc_bytes[i++] - '0');

        if (i < osc_length && osc_bytes[i] == ';')
                i++;

        if ((command == 0 || command == 2) && window)
        {
                n = osc_length - i;
                if (n >= WINDOW_TITLE_MAX)
                        n = WINDOW_TITLE_MAX - 1;
                memory_copy(window->title, osc_bytes + i, n);
                window->title[n] = 0;
        }
}

static fn erase(unsigned int from_row, unsigned int from_column,
                unsigned int to_row, unsigned int to_column)
{
        b32 coloured = blank_cell_word() != BLANK_CELL_WORD;
        unsigned int r;

        for (r = from_row; r <= to_row && r < ROWS; r++)
        {
                unsigned int first = r == from_row ? from_column : 0;
                unsigned int last = r == to_row ? to_column : COLUMNS - 1;
                unsigned int past = min(last + 1, COLUMNS);
                unsigned int address_to length = row_length(r);

                /*
                        An erase that reaches the end of a line, or the edge
                        of the window, is the line getting shorter. That is
                        cheaper than the cells it would have written, and a
                        line stored wider than a window that has since
                        narrowed would otherwise keep a hidden tail for the
                        compositor to fold onto the next row: a full-screen
                        program redrawing after a resize came back to that.

                        A coloured erase is drawn, so it fills to the edge in
                        blank cells of that colour and still drops the tail.
                */
                if (!coloured)
                {
                        if (first >= address_to length)
                                continue;

                        if (past >= address_to length || past == COLUMNS)
                        {
                                address_to length = first;
                                touch(r);
                        }
                        else
                                cells_clear(r, first, past - first);

                        continue;
                }

                if (first >= past)
                {
                        if (address_to length > COLUMNS)
                        {
                                address_to length = COLUMNS;
                                touch(r);
                        }

                        continue;
                }

                reach(r, first);
                cells_clear(r, first, past - first);

                if (past == COLUMNS || address_to length < past)
                        address_to length = past;
        }
}

static fn tabs_reset()
{
        memory_fill_u64_aligned(tab_stop, TAB_STOPS / 8, 1);
}

static fn tab_forward()
{
        unsigned int c = column;

        while (++c < COLUMNS)
                if (c >= TAB_STOPS || tab_stop[c])
                        break;

        column = c < COLUMNS ? c : COLUMNS - 1;
}

static fn tab_backward()
{
        unsigned int c = column;

        if (!c)
                return;

        while (c)
        {
                c--;
                if (c < TAB_STOPS && tab_stop[c])
                        break;
        }

        column = c;
}

/*
        A cell carries an index into the two hundred and fifty six an xterm
        has, so the cube, the greys and a true-colour triple all land on one
        of those rather than being folded into the first sixteen. Folding
        them was what made 38;5;31m paint the text red: the number after 38;5
        fell through into the plain foreground range.
*/
static CONST unsigned char colour_256(unsigned int n)
{
        return (unsigned char)(n < 256 ? n : 15);
}

static CONST unsigned char colour_cube_level(unsigned int v)
{
        if (v < 48)
                return 0;
        if (v < 115)
                return 1;
        v = (v - 35) / 40;
        return (unsigned char)(v > 5 ? 5 : v);
}

static CONST unsigned char colour_rgb(unsigned int r, unsigned int g,
                                      unsigned int b)
{
        unsigned int qr, qg, qb, grey, cube, cr, cg, cb, gr, dcube, dgrey;

        if (r > 255)
                r = 255;
        if (g > 255)
                g = 255;
        if (b > 255)
                b = 255;

        qr = colour_cube_level(r);
        qg = colour_cube_level(g);
        qb = colour_cube_level(b);
        cube = 16 + 36 * qr + 6 * qg + qb;
        cr = qr ? qr * 40 + 55 : 0;
        cg = qg ? qg * 40 + 55 : 0;
        cb = qb ? qb * 40 + 55 : 0;
        dcube = (r > cr ? r - cr : cr - r) + (g > cg ? g - cg : cg - g) +
                (b > cb ? b - cb : cb - b);

        grey = r < 8 ? 0 : (r > 238 ? 23 : (r - 8) / 10);
        gr = 8 + grey * 10;
        dgrey = (r > gr ? r - gr : gr - r) + (g > gr ? g - gr : gr - g) +
                (b > gr ? b - gr : gr - b);

        return (unsigned char)(dgrey < dcube ? 232 + grey : cube);
}

// Returns how many parameters past this one it took, so the caller can step
// over the ones an extended colour is spelled with.
static unsigned int sgr_extended(unsigned int at, unsigned char address_to which)
{
        unsigned int kind = at + 1 < terminal_csi.count
                                ? terminal_csi.value[at + 1] : 0;

        if (kind == 5 && at + 2 < terminal_csi.count)
        {
                address_to which = colour_256(terminal_csi.value[at + 2]);
                return 2;
        }

        if (kind == 2 && at + 4 < terminal_csi.count)
        {
                address_to which = colour_rgb(
                    terminal_csi.value[at + 2], terminal_csi.value[at + 3],
                    terminal_csi.value[at + 4]);
                return 4;
        }

        return terminal_csi.count - at - 1;
}

static fn sgr()
{
        if (!terminal_csi.count)
        {
                ink = 7;
                paper = 0;
                reverse = false;
                style = 0;
                return;
        }

        for (unsigned int i = 0; i < terminal_csi.count; i++)
        {
                unsigned int p = terminal_csi.value[i];

                if (p == 0)
                {
                        ink = 7;
                        paper = 0;
                        reverse = false;
                        style = 0;
                }
                else if (p == 1)
                {
                        style |= WINDOW_CELL_BOLD;
                        if (ink < 8)
                                ink |= 8;
                }
                else if (p == 2)
                        style |= WINDOW_CELL_DIM;
                else if (p == 3)
                        style |= WINDOW_CELL_ITALIC;
                else if (p == 4)
                        style |= WINDOW_CELL_UNDERLINE;
                else if (p == 5 || p == 6)
                        style |= WINDOW_CELL_BLINK;
                else if (p == 7)
                        reverse = true;
                else if (p == 8)
                        style |= WINDOW_CELL_HIDDEN;
                else if (p == 9)
                        style |= WINDOW_CELL_STRIKE;
                else if (p == 21 || p == 22)
                {
                        style &= (unsigned short)~(WINDOW_CELL_BOLD | WINDOW_CELL_DIM);
                        if (ink >= 8 && ink < 16)
                                ink &= 7;
                }
                else if (p == 23)
                        style &= (unsigned short)~WINDOW_CELL_ITALIC;
                else if (p == 24)
                        style &= (unsigned short)~WINDOW_CELL_UNDERLINE;
                else if (p == 25)
                        style &= (unsigned short)~WINDOW_CELL_BLINK;
                else if (p == 27)
                        reverse = false;
                else if (p == 28)
                        style &= (unsigned short)~WINDOW_CELL_HIDDEN;
                else if (p == 29)
                        style &= (unsigned short)~WINDOW_CELL_STRIKE;
                else if (p >= 30 && p <= 37)
                        ink = (unsigned char)(((style & WINDOW_CELL_BOLD) ? 8 : 0) |
                                              (p - 30));
                else if (p == 38)
                        i += sgr_extended(i, address_of ink);
                else if (p == 39)
                        ink = (unsigned char)((style & WINDOW_CELL_BOLD) ? 15 : 7);
                else if (p >= 40 && p <= 47)
                        paper = (unsigned char)(p - 40);
                else if (p == 48)
                        i += sgr_extended(i, address_of paper);
                else if (p == 49)
                        paper = 0;
                else if (p >= 90 && p <= 97)
                        ink = (unsigned char)(8 + (p - 90));
                else if (p >= 100 && p <= 107)
                        paper = (unsigned char)(8 + (p - 100));
        }
}

/*
        The other screen, which is this one further along.

        An alternate buffer is a screen a program is given, scribbles on and
        hands back with what was underneath still there. The ring already
        holds what was underneath: moving head on by a screenful gives out
        blank lines and leaves the old ones behind it, and moving head back is
        the hand back. There is no second buffer and nothing is copied.
*/
static b32 alternate;
static unsigned int alternate_head;

static fn cursor_save(struct cursor_state address_to into)
{
        address_to into = (struct cursor_state){row, column, ink, paper,
                                                style, reverse};
}

static fn cursor_restore(const struct cursor_state address_to from)
{
        row = from->row < ROWS ? from->row : ROWS - 1;
        column = from->column < COLUMNS ? from->column : COLUMNS - 1;
        ink = from->ink;
        paper = from->paper;
        style = from->style;
        reverse = from->reverse;
}

static fn alternate_enter()
{
        if (alternate)
                return;

        cursor_save(address_of cursor_primary);
        alternate_head = window->head;

        for (unsigned int r = 0; r < ROWS; r++)
                window_scroll(window);

        row = 0;
        column = 0;
        alternate = true;
        touch_all();
}

static fn alternate_leave()
{
        if (!alternate)
                return;

        __atomic_store_n(address_of window->head, alternate_head, __ATOMIC_RELEASE);

        alternate = false;
        cursor_restore(address_of cursor_primary);
        touch_all();
}

static fn mode_set(b32 on)
{
        for (unsigned int i = 0; i < terminal_csi.count; i++)
        {
                unsigned int p = terminal_csi.value[i];

                if (!terminal_csi.marker)
                {
                        if (p == 4)
                                insert_mode = on;

                        continue;
                }

                switch (p)
                {
                case 1:
                        application_keys = on;
                        break;
                case 7:
                        autowrap = on;
                        break;
                case 25:
                        cursor_visible = on;
                        break;
                case 6:
                        origin_mode = on;
                        row = on ? region_top : 0;
                        column = 0;
                        break;
                case 47:
                case 1047:
                case 1049:
                        if (on)
                                alternate_enter();
                        else
                                alternate_leave();
                        break;
                case 1000:
                case 1002:
                case 1003:
                        mouse_mode = on ? p : 0;
                        if (window)
                                window->want = mouse_mode ? WINDOW_WANT_POINTER : 0;
                        break;
                case 1004:
                        focus_events = on;
                        break;
                case 1006:
                        mouse_sgr = on;
                        break;
                case 2026:
                        synchronized_output = on;
                        break;
                }
        }
}

static fn attributes_reset()
{
        ink = 7;
        paper = 0;
        reverse = false;
        style = 0;
        autowrap = true;
        insert_mode = false;
        cursor_visible = true;
        application_keys = false;
        synchronized_output = false;
        mouse_mode = 0;
        mouse_sgr = false;
        focus_events = false;
        origin_mode = false;
        charset_g0 = CHARSET_ASCII;
        charset_g1 = CHARSET_ASCII;
        charset_gl = 0;
        if (window)
                window->want = 0;
}

/*
        DECSTR, which is what is2 sends. The screen stays; the modes do not.
        RIS is the one that blanks the page, and folding the two together
        made every ncurses init wipe a dashboard that had just drawn.
*/
static fn soft_reset()
{
        attributes_reset();
        last_character = 0;
        region_top = 0;
        region_bottom = ROWS;
        row = 0;
        column = 0;
        cursor_saved = (struct cursor_state){0, 0, 7, 0, 0, false};
}

static fn full_reset()
{
        attributes_reset();
        last_character = 0;
        row = 0;
        column = 0;
        region_top = 0;
        region_bottom = ROWS;
        alternate_leave();
        tabs_reset();
        erase(0, 0, ROWS - 1, COLUMNS - 1);
}

static fn csi_final(unsigned int final)
{
        unsigned int a = terminal_csi.count && terminal_csi.value[0]
                             ? terminal_csi.value[0] : 1;
        unsigned int b = terminal_csi.count > 1 && terminal_csi.value[1]
                             ? terminal_csi.value[1] : 1;

        switch (final)
        {
        case 'H':
        case 'f':
        {
                unsigned int top = origin_mode ? region_top : 0;
                unsigned int bottom = origin_mode ? region_bottom : ROWS;
                unsigned int r = top + a - 1;

                row = r < bottom ? r : (bottom ? bottom - 1 : 0);
                column = b - 1 < COLUMNS ? b - 1 : COLUMNS - 1;
                break;
        }
        case 'A':
                row = row > a ? row - a : 0;
                break;
        /* Compare against room left: a hostile CSI count can wrap row + a. */
        case 'B':
                row = a < ROWS - row ? row + a : ROWS - 1;
                break;
        case 'C':
                column = a < COLUMNS - column ? column + a : COLUMNS - 1;
                break;
        case 'D':
                column = column > a ? column - a : 0;
                break;
        case 'E':
                row = a < ROWS - row ? row + a : ROWS - 1;
                column = 0;
                break;
        case 'F':
                row = row > a ? row - a : 0;
                column = 0;
                break;
        case 'G':
        case '`':
                column = a - 1 < COLUMNS ? a - 1 : COLUMNS - 1;
                break;
        case 'd':
        {
                unsigned int top = origin_mode ? region_top : 0;
                unsigned int bottom = origin_mode ? region_bottom : ROWS;
                unsigned int r = top + a - 1;

                row = r < bottom ? r : (bottom ? bottom - 1 : 0);
                break;
        }
        case 'J':
                if (terminal_csi.count && terminal_csi.value[0] == 1)
                        erase(0, 0, row, column);
                else if (terminal_csi.count && terminal_csi.value[0] >= 2)
                        erase(0, 0, ROWS - 1, COLUMNS - 1);
                else
                        erase(row, column, ROWS - 1, COLUMNS - 1);
                break;
        case 'K':
                if (terminal_csi.count && terminal_csi.value[0] == 1)
                        erase(row, 0, row, column);
                else if (terminal_csi.count && terminal_csi.value[0] == 2)
                        erase(row, 0, row, COLUMNS - 1);
                else
                        erase(row, column, row, COLUMNS - 1);
                break;
        case 'L':
        case 'M':
                // Lines are put in and taken out at the cursor, and the rest
                // of the region moves. A cursor outside the region is a
                // sequence with nowhere to happen.
                if (row >= region_top && row < region_bottom)
                {
                        unsigned int was = region_top;

                        region_top = row;

                        if (final == 'L')
                                scroll_down(a);
                        else
                                scroll_up(a);

                        region_top = was;
                }
                break;
        case 'P':
        {
                unsigned int address_to length = row_length(row);
                struct window_cell address_to cells = row_cells(row);
                unsigned int last = address_to length;
                unsigned int gone;

                if (column >= last)
                        break;

                gone = a < last - column ? a : last - column;

                memory_copy(cells + column, cells + column + gone,
                            (positive)(last - column - gone) *
                                sizeof(struct window_cell));

                address_to length = last - gone;
                touch(row);
                break;
        }
        case '@':
                open_gap(column, a);
                break;
        case 'X':
                reach(row, column);

                cells_clear(row, column, min(a, COLUMNS - column));
                break;
        case 'S':
                scroll_up(a);
                break;
        case 'T':
                scroll_down(a);
                break;
        case 'g':
                if (terminal_csi.count && terminal_csi.value[0] == 3)
                {
                        memory_fill(tab_stop, 0, sizeof(tab_stop));
                }
                else if (column < TAB_STOPS)
                        tab_stop[column] = 0;
                break;
        case 'h':
                mode_set(true);
                break;
        case 'l':
                mode_set(false);
                break;
        case 'm':
                sgr();
                break;
        case 'n':
                // The cursor is reported one based, which is the same
                // counting CUP takes it back in.
                if (terminal_csi.count && terminal_csi.value[0] == 6)
                {
                        emit_literal("\x1b[");
                        positive_to_string(emit_bytes, row + 1);
                        emit(';');
                        positive_to_string(emit_bytes,
                                           column < COLUMNS ? column + 1 : COLUMNS);
                        emit('R');
                }
                else if (terminal_csi.count && terminal_csi.value[0] == 5)
                        emit_literal("\x1b[0n");
                break;
        case 'c':
                // Primary DA names a VT100 with AVO, which is what xterm
                // answers and what ncurses's u8 reads for. Secondary DA is
                // the xterm version report.
                if (terminal_csi.marker == '>')
                        emit_literal("\x1b[>0;115;0c");
                else
                        emit_literal("\x1b[?1;2c");
                break;
        case 'Z':
                while (a--)
                        tab_backward();
                break;
        case 'b':
                if (last_character)
                {
                        unsigned int n;

                        for (n = 0; n < a; n++)
                                put(last_character);
                }
                break;
        case 'q':
                break;
        case 'p':
                if (csi_intermediate == '!')
                        soft_reset();
                break;
        case 't':
                break;
        case 'r':
        {
                unsigned int top;
                unsigned int bottom;

                if (terminal_csi.marker)
                        break;

                top = terminal_csi.count && terminal_csi.value[0]
                          ? terminal_csi.value[0] - 1 : 0;
                bottom = terminal_csi.count > 1 && terminal_csi.value[1]
                             ? terminal_csi.value[1] : ROWS;

                if (bottom > ROWS)
                        bottom = ROWS;

                // A region has to have two rows in it to scroll, and one that
                // does not is left alone rather than half applied.
                if (top + 1 < bottom)
                {
                        region_top = top;
                        region_bottom = bottom;

                        // Home is the top of the region when origin mode is
                        // on, and the top of the page otherwise.
                        row = origin_mode ? region_top : 0;
                        column = 0;
                }
                break;
        }
        case 's':
                cursor_save(address_of cursor_saved);
                break;
        case 'u':
                cursor_restore(address_of cursor_saved);
                break;
        }
}

/*
        Bytes into characters.

        A cell holds a character and the stream is UTF-8, so the two are not
        the same thing and the bytes above 127 were being thrown away one at a
        time. Anything malformed becomes U+FFFD rather than disappearing,
        which is what stops one bad byte from eating the character after it.
*/
static memory_utf8_state terminal_utf8;

static fn utf8_byte(unsigned int c)
{
        // A byte that cuts a sequence short is refused along with it, and it
        // is still a byte of its own: the feed leaves replaying it to us.
        bool cut = terminal_utf8.left && ((p8)c & 0xc0) != 0x80;
        b32 result = memory_utf8_feed(address_of terminal_utf8, (p8)c);

        if (result < 0 && cut)
        {
                put(0xfffd);
                result = memory_utf8_feed(address_of terminal_utf8, (p8)c);
        }
        if (result)
                put(result < 0 ? 0xfffd : terminal_utf8.value);
}

static fn line_forget();

// A sequence that has stopped short is one character that never arrived, and
// whatever ended it is not it.
static fn utf8_flush()
{
        if (!terminal_utf8.left)
                return;

        terminal_utf8.left = 0;
        put(0xfffd);
}

static fn consume(unsigned int c)
{
        /*
                Two bytes that end whatever is being read.

                CAN and SUB abandon a sequence wherever in one they arrive,
                ECMA-48 8.3.5 and the control strings included. Leaving them
                to the parameter bytes took the final of the abandoned
                sequence out of the text after it -- ESC [ 3 ; 3 CAN X ran the
                X as an erase rather than printing it -- and leaving them to
                the string reader left an OSC that was cut short with nothing
                at all that could end it, and every byte after it went
                nowhere.
        */
        if (c == 24 || c == 26)
        {
                in_escape = in_csi = in_string = false;
                terminal_utf8.left = 0;
                return;
        }

        // Anything the far end says lands where the cursor is, so a line
        // being typed is no longer where it was drawn and the next keystroke
        // draws it again from wherever the output left off.
        line_forget();

        /*
                A string sequence carries a payload that is not for the
                screen.

                A window title arrives as ESC ] 0 ; text BEL and every byte of
                it used to be printed, because the parser gave up at the ] and
                went back to putting characters in cells.
        */
        if (in_string)
        {
                if (!string_escape)
                {
                        if (c == 27)
                                string_escape = true;
                        else if (c == 7)
                        {
                                osc_finish();
                                in_string = in_escape = false;
                        }
                        else if (osc_length < WINDOW_TITLE_MAX)
                                osc_bytes[osc_length++] = (p8)c;

                        return;
                }

                /*
                        An escape inside a string ends it either way.

                        With a backslash after it that is ST, the ending the
                        string was written to have. With anything else it is
                        the next sequence beginning, and taking it as one is
                        what stops a title nobody terminated from swallowing
                        every sequence sent after it.
                */
                string_escape = in_string = false;

                if (c == '\\')
                {
                        osc_finish();
                        in_escape = false;
                        return;
                }

                osc_finish();
                in_escape = true;
                escape_intermediate = false;
        }

        if (c == 27)
        {
                utf8_flush();
                in_escape = true;
                in_csi = false;
                escape_intermediate = false;
                return;
        }

        if (in_csi)
        {
                if (c >= 0x20 && c <= 0x2f)
                {
                        csi_intermediate = (p8)c;
                        return;
                }

                if (!terminal_parameters_take(address_of terminal_csi, c))
                        return;

                csi_final(c);
                in_csi = in_escape = false;
                csi_intermediate = 0;

                return;
        }

        if (in_escape)
        {
                if (c == '[')
                {
                        in_csi = true;
                        csi_intermediate = 0;
                        terminal_parameters_reset(address_of terminal_csi);
                        return;
                }

                if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_')
                {
                        in_string = true;
                        string_escape = false;
                        osc_length = 0;
                        return;
                }

                // ESC ( B and its like: an intermediate byte says the sequence
                // carries on to a final one. Ending the sequence at the first
                // of them printed the rest of it on the screen.
                if (c >= ' ' && c <= '/')
                {
                        escape_intermediate = true;
                        escape_kind = (p8)c;
                        return;
                }

                in_escape = false;

                // A final after an intermediate belongs to that sequence, so
                // ESC # 8 is one thing and not a DECRC hiding behind a hash.
                if (escape_intermediate)
                {
                        if (escape_kind == '(')
                                charset_g0 = c == '0' ? CHARSET_ACS : CHARSET_ASCII;
                        else if (escape_kind == ')')
                                charset_g1 = c == '0' ? CHARSET_ACS : CHARSET_ASCII;
                        escape_intermediate = false;
                        return;
                }

                switch (c)
                {
                case '7':
                        cursor_save(address_of cursor_saved);
                        break;
                case '8':
                        cursor_restore(address_of cursor_saved);
                        break;
                case 'D':
                        line_feed();
                        break;
                case 'E':
                        line_feed();
                        column = 0;
                        break;
                case 'M':
                        if (row == region_top)
                                scroll_down(1);
                        else if (row)
                                row--;
                        break;
                case 'H':
                        if (column < TAB_STOPS)
                                tab_stop[column] = 1;
                        break;
                case 'c':
                        full_reset();
                        break;
                case '=':
                        application_keys = true;
                        break;
                case '>':
                        application_keys = false;
                        break;
                }

                return;
        }

        switch (c)
        {
        case '\n':
        case 11:
        case 12:
                line_feed();
                touch(row);
                return;
        case '\r':
                column = 0;
                return;
        case '\b':
                if (column)
                        column--;
                return;
        case '\t':
                tab_forward();
                return;
        case 7:
                return;
        case 14:
                charset_gl = 1;
                return;
        case 15:
                charset_gl = 0;
                return;
        }

        if (c < ' ' || c == 127)
                return;

        if (c < 128)
        {
                utf8_flush();
                put((charset_gl ? charset_g1 : charset_g0) == CHARSET_ACS
                        ? acs_character(c)
                        : c);
                return;
        }

        utf8_byte(c);
}

#ifdef KERNEL_MODE
/* A pty is a stream, but printk records are unrelated messages. Do not let an
   unfinished escape or UTF-8 sequence in attacker-controlled log text consume
   later diagnostics. */
static fn term_record_begin()
{
        in_escape = in_csi = in_string = false;
        escape_intermediate = string_escape = false;
        osc_length = 0;
        terminal_parameters_reset(address_of terminal_csi);
        terminal_utf8.left = 0;
}
#endif

static fn cursor_hide()
{
        if (!shown)
                return;

        struct window_cell address_to cell = row_cells(shown_row) + shown_column;
        unsigned char was = cell->ink;

        cell->ink = cell->paper;
        cell->paper = was;
        touch(shown_row);
        shown = false;
}

static fn SPARE cursor_show()
{
        // put leaves column at COLUMNS after filling the last cell of a row and
        // only wraps on the next character, so the cursor has to be clamped:
        // unclamped it landed on the first cell of the row below, and on the
        // last row that is one cell past the grid.
        unsigned int at = column < COLUMNS ? column : COLUMNS - 1;
        struct window_cell address_to cell;
        unsigned char was;

        // Showing is a state transition, not a colour toggle. In particular,
        // a synchronous resize commit and the ordinary loop tail can both
        // ask in one iteration; the second must not invert the cell back.
        if (shown || !cursor_visible)
                return;

        // A block sits on a cell, so there has to be one to sit on.
        reach(row, at + 1);

        cell = row_cells(row) + at;
        was = cell->ink;

        cell->ink = cell->paper;
        cell->paper = was;
        shown_row = row;
        shown_column = at;
        shown = true;
        touch(row);
}

/*
        The line being typed, before it is a line.

        A terminal in canonical mode hands its shell whole lines and the kernel
        line discipline is what assembles them -- which can erase a character
        and kill a line and nothing else. There is no left arrow in it, no
        history, and no way to put a character anywhere but at the end.

        So the assembling happens here instead. It belongs to whichever end
        knows both what has been typed and where it is on the screen, and that
        is this one: the shell has the bytes but not the grid, and the line
        discipline has neither. screen.c turns the far end's echo off while
        this is running and turns this off the moment a program asks for raw
        input, so a program that wants its own keys still gets them.

        Nothing here is a system call either. What the shell is to be told is
        put in to_shell and sent by the loop that owns the descriptor.
*/
#define LINE_HISTORY 32

static positive line_drawn;
static b32 line_anchored;

// Where on the screen the line starts, as a line of the ring rather than a
// row: the screen scrolls out from under a long line, and a row would then be
// pointing at somebody else's text. Out here with the rest of what the output
// half touches, because a resize has an opinion about the column.
#ifndef KERNEL_MODE
static unsigned int line_anchor;
static positive line_view;
static unsigned int line_screen_anchor, line_screen_column;
#endif
static unsigned int line_anchor_column;

/*
        What the output half has to say about a line being typed, and no more.

        Anything the far end prints moves the cursor out from under a line the
        editor drew, so consume() says so and the next keystroke draws it
        again. That is the whole of what the kernel build needs: printk writes
        into the console window and nobody types into it, so the editor itself
        -- and the thirty four kilobytes of line and history behind it -- is
        left out below.
*/
static fn line_forget()
{
        line_anchored = false;
        line_drawn = 0;
#ifndef KERNEL_MODE
        line_view = 0;
#endif
}

#ifndef KERNEL_MODE

static b32 line_editing;
static p8 address_to line;
static positive line_room, line_length, line_point;

static p8 address_to history[LINE_HISTORY];
static positive history_room[LINE_HISTORY];
static positive history_length[LINE_HISTORY];
static positive history_count, history_at;

// The line being typed, kept while the history is walked so that coming back
// down off the end returns it rather than an empty line.
static p8 address_to history_held;
static positive history_held_room, history_held_length;

// Ctrl-L keeps the cells to the left of the line anchor without imposing a
// second, unrelated limit on the width of a prompt.
static struct window_cell address_to line_prompt;
static positive line_prompt_room, line_prompt_length;

static unsigned int line_anchor_row()
{
        unsigned int at = line_anchor - (window->head - ROWS);

        return at < ROWS ? at : 0;
}

static fn line_erase(b32 shorten);

/*
        The line, on the screen, wherever it now is.

        Everything is written again from the anchor: it is at most a screenful
        of cells and it is the only way a character put in the middle can move
        the ones after it. What the last pass wrote is blanked by writing over
        it, because a line that got shorter has to stop being on the screen.
*/
static fn line_show()
{
        positive at, capacity, caret, first, drawn;
        bipolar anchor_row;
        unsigned int top;

        if (!line_anchored)
                return;

        /*
                Redrawing an entire command longer than the viewport scrolled
                it once for every key.  Eventually the oldest part replaced
                the newest and the cursor was pinned to the bottom-right cell.

                Make the editable line a viewport of its own.  Its logical
                anchor never moves; shift whole rows only far enough to keep
                the insertion point visible, and draw no more than the screen
                can hold.  Moving Home reveals the beginning again without
                losing a byte of the command.
        */
        caret = line_point < line_length ? line_point
                                        : (line_point ? line_point - 1 : 0);

        line_erase(false);
        line_drawn = 0;

        /* Advance the real ring only when the insertion cell first crosses
           its bottom.  A redraw at the same point must never scroll again. */
        while ((positive)line_anchor +
                   (line_anchor_column + caret) / COLUMNS >= window->head)
                ring_scroll();

        top = window->head - ROWS;
        anchor_row = (bipolar)line_anchor - (bipolar)top;

        if (line_view && caret >= line_view &&
            caret - line_view < (positive)ROWS * COLUMNS)
        {
                first = line_view;
                line_screen_anchor = top;
                line_screen_column = 0;
        }
        else if (anchor_row < 0)
        {
                bipolar hidden = -anchor_row * (bipolar)COLUMNS -
                                 (bipolar)line_anchor_column;

                first = hidden > 0 ? (positive)hidden : 0;

                // Home and a long run of Lefts deliberately page back into
                // the part which has already scrolled out of the live ring.
                if (caret < first)
                        first = caret - caret % COLUMNS;

                line_screen_anchor = top;
                line_screen_column = 0;
        }
        else
        {
                first = 0;
                line_screen_anchor = top + (unsigned int)anchor_row;
                line_screen_column = line_anchor_column;
        }

        if (first > line_length)
                first = line_length;

        capacity = ((positive)window->head - line_screen_anchor) * COLUMNS -
                   line_screen_column;
        drawn = line_length - first;

        if (drawn > capacity)
                drawn = capacity;

        row = line_screen_anchor - top;
        column = line_screen_column;

        for (at = 0; at < drawn; at++)
                put(line[first + at]);

        line_view = first;
        line_drawn = drawn;

        at = line_point > first ? line_point - first : 0;
        row = line_screen_anchor - (window->head - ROWS) +
              (unsigned int)((line_screen_column + at) / COLUMNS);
        column = (unsigned int)((line_screen_column + at) % COLUMNS);

        if (row >= ROWS)
        {
                row = ROWS - 1;
                column = COLUMNS - 1;
        }
}

/*
        Take the editable line out of the old grid before putting it into a
        new one.

        Merely drawing it again after COLUMNS changed left the old cells in
        the ring as well. Canvas then folded those old cells and the newly
        laid-out copy appeared a second time. The prompt is everything before
        the anchor and stays; rows occupied only by the editable line become
        empty by shortening their lengths, which is also what keeps blank
        folded rows from consuming the resized view.
*/
static fn line_erase(b32 shorten)
{
        unsigned int r, c, top;
        positive left, skipped = 0;
        bipolar screen_row;

        if (!line_anchored || !line_drawn)
                return;

        top = window->head - ROWS;
        screen_row = (bipolar)line_screen_anchor - (bipolar)top;
        c = line_screen_column;

        if (screen_row < 0)
        {
                positive past = (positive)(-screen_row) * COLUMNS;

                skipped = past > c ? past - c : 0;
                screen_row = 0;
                c = 0;
        }

        if (skipped >= line_drawn || screen_row >= (bipolar)ROWS)
                return;

        r = (unsigned int)screen_row;
        left = line_drawn - skipped;

        while (left && r < ROWS)
        {
                unsigned int room = COLUMNS - min(c, COLUMNS);
                positive taken = left < room ? left : room;
                unsigned int address_to length = row_length(r);

                if (shorten && address_to length > c)
                        address_to length = c;
                else if (!shorten)
                        cells_clear(r, c, (unsigned int)taken);

                touch(r);
                left -= taken;
                r++;
                c = 0;

                // A full last column wraps only when another character is
                // written. There is no next row to remove in that case.
                if (!room)
                        break;
        }
}

static fn line_insert(unsigned int character)
{
        if (!array_store_reserve(line, line_room, line_length,
                                 line_length + 1, 64))
                return;

        memory_copy(line + line_point + 1, line + line_point,
                    line_length - line_point);

        line[line_point++] = (p8)character;
        line_length++;
}

static fn line_take(positive from, positive to)
{
        positive gone = to - from;

        memory_copy(line + from, line + to, line_length - to);

        line_length -= gone;
        line_point = from;
}

// The word before the cursor is the run of spaces then the run that is not.
static PURE positive line_word()
{
        positive at = line_point;

        while (at && line[at - 1] == ' ')
                at--;

        while (at && line[at - 1] != ' ')
                at--;

        return at;
}

static fn line_from_history()
{
        const p8 address_to from = history_at < history_count
                                       ? history[history_at % LINE_HISTORY]
                                       : history_held;
        positive length = history_at < history_count
                              ? history_length[history_at % LINE_HISTORY]
                              : history_held_length;

        if (!array_store_reserve(line, line_room, line_length, length, 64))
                return;

        memory_copy_apart(line, (address_any)from, length);

        line_length = length;
        line_point = length;
}

static fn line_remember()
{
        unsigned int slot;

        if (!line_length)
                return;

        // The same command twice running is one entry, which is what makes
        // the up arrow worth pressing after a loop of them.
        if (history_count &&
            history_length[(history_count - 1) % LINE_HISTORY] == line_length &&
            history[(history_count - 1) % LINE_HISTORY][0] == line[0] &&
            (line_length == 1 ||
             !memory_compare(history[(history_count - 1) % LINE_HISTORY], line,
                             line_length)))
                return;

        slot = history_count % LINE_HISTORY;

        if (!array_store_reserve(history[slot], history_room[slot],
                                 history_length[slot], line_length, 64))
                return;

        memory_copy_apart(history[slot], line, line_length);

        history_length[slot] = line_length;
        history_count++;
}

/*
        The screen, cleared, with the prompt kept.

        What is to the left of where the line began is the prompt, and it is
        the only part of it this ever knows: the shell wrote it and never said
        what it was. Copying those cells to the top of the cleared screen is
        what makes Ctrl-L put back a prompt nobody told us the text of.
*/
static fn line_clear_screen()
{
        unsigned int kept = line_anchored
                                ? min((unsigned int)line_prompt_length, COLUMNS)
                                : 0;

        erase(0, 0, ROWS - 1, COLUMNS - 1);

        if (kept)
        {
                reach(0, kept);
                memory_copy_apart(row_cells(0), line_prompt,
                                  kept * sizeof(line_prompt[0]));
        }

        line_anchor = window->head - ROWS;
        line_anchor_column = kept;
        line_anchored = true;
        line_drawn = 0;
        touch_all();
}

/*
        The line is over, however it ended.

        What was typed stays on the screen -- it is the record of what was
        asked for, and both the shell's answer and the next prompt go under
        it -- so the cursor is put past the end of what was drawn and taken
        down one from there.
*/
// Nothing typed, the cursor at its start, and history back at the bottom.
static fn line_reset()
{
        line_length = 0;
        line_point = 0;
        history_at = history_count;
        history_held_length = 0;
        line_forget();
}

static fn line_done()
{
        row = line_anchor_row();
        column = line_anchor_column + line_drawn;

        while (column >= COLUMNS)
        {
                column -= COLUMNS;
                row++;
        }

        if (row >= ROWS)
                row = ROWS - 1;

        line_feed();
        column = 0;
        touch(row);
        line_reset();
}

static fn line_accept()
{
        line_remember();

        emit_bytes(line, line_length);

        emit('\n');
        line_done();
}

// Linux calls them these, and the compositor hands over the number rather than
// a sequence, so the editor reads the same key the terminal would encode.
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_DELETE 111

static b32 line_key(unsigned int character, unsigned int code)
{
        if (!line_anchored)
        {
                positive prompt;

                line_anchor = window->head - ROWS + row;
                line_anchor_column = column < COLUMNS ? column : COLUMNS - 1;
                prompt = line_anchor_column;

                /*
                        Capture the prompt while its row is unquestionably
                        still the anchor row.  A command taller than the
                        viewport scrolls that row into history; waiting until
                        Ctrl-L copied whatever happened to be at row zero and
                        restored two command characters as the prompt.
                */
                if (array_store_reserve(line_prompt, line_prompt_room,
                                        line_prompt_length, prompt, 64))
                {
                        memory_copy_apart(line_prompt, row_cells(row),
                                          prompt * sizeof(*line_prompt));
                        line_prompt_length = prompt;
                }
                else
                        line_prompt_length = 0;

                line_anchored = true;
                line_drawn = 0;
        }

        switch (code)
        {
        case KEY_LEFT:
                character = 2;
                break;
        case KEY_RIGHT:
                character = 6;
                break;
        case KEY_HOME:
                character = 1;
                break;
        case KEY_END:
                character = 5;
                break;
        case KEY_DELETE:
                if (line_point < line_length)
                        line_take(line_point, line_point + 1);
                return true;
        case KEY_UP:
        {
                positive first = history_count > LINE_HISTORY
                                     ? history_count - LINE_HISTORY
                                     : 0;

                if (history_at <= first)
                        return true;

                if (history_at == history_count)
                {
                        if (!array_store_reserve(
                                history_held, history_held_room,
                                history_held_length, line_length, 64))
                                return true;

                        memory_copy_apart(history_held, line, line_length);

                        history_held_length = line_length;
                }

                history_at--;
                line_from_history();
                return true;
        }
        case KEY_DOWN:
                if (history_at >= history_count)
                        return true;

                history_at++;
                line_from_history();
                return true;
        }

        /*
                A key that means no character and that the editor has no use
                for is nothing at all.

                Its sequence would otherwise be sent, and the line discipline
                puts the bytes of it in the line: pressing Page Up at a prompt
                ran a command with an escape, a bracket and a tilde in it.
        */
        if (!character)
                return true;

        switch (character)
        {
        case 1:
                line_point = 0;
                return true;
        case 2:
                if (line_point)
                        line_point--;
                return true;
        case 5:
                line_point = line_length;
                return true;
        case 6:
                if (line_point < line_length)
                        line_point++;
                return true;
        case 8:
        case 127:
                if (line_point)
                        line_take(line_point - 1, line_point);
                return true;
        case 4:
                /*
                        End of file, but only with nothing to delete.

                        On a line with text after the cursor it is the forward
                        delete, which is what every editor with these bindings
                        has done since they were named; on an empty line it is
                        the byte that ends the shell, and the line discipline
                        is what turns it into one.
                */
                if (line_length)
                {
                        if (line_point < line_length)
                                line_take(line_point, line_point + 1);

                        return true;
                }

                emit(4);
                return true;
        case 11:
                if (line_point < line_length)
                        line_take(line_point, line_length);
                return true;
        case 21:
                if (line_point)
                        line_take(0, line_point);
                return true;
        case 23:
                if (line_point)
                        line_take(line_word(), line_point);
                return true;
        case 12:
                line_clear_screen();
                return true;
        case '\r':
        case '\n':
                line_accept();
                return true;
        case 3:
                // The signal is the line discipline's to raise, so the byte
                // still goes. What was typed is left where it is, the way a
                // line that was run would be, and abandoned.
                emit(3);
                line_done();
                return true;
        }

        if (character < ' ')
        {
                emit(character);
                return true;
        }

        line_insert(character);
        return true;
}

/*
        A key that is no character is still a key.

        Arrows, Home, the function keys: a program is handed the code and
        nothing else, and a terminal owes its shell the sequence ANSI names
        for each of them. TERM=xterm-256color is exported below, and this is
        what makes that true rather than a claim.
*/
static const struct
{
        unsigned int code;
        const char address_to sequence;
        const char address_to application;
} key_sequences[] = {
    {KEY_UP, "\x1b[A", "\x1bOA"}, {KEY_DOWN, "\x1b[B", "\x1bOB"},
    {KEY_RIGHT, "\x1b[C", "\x1bOC"}, {KEY_LEFT, "\x1b[D", "\x1bOD"},
    {KEY_HOME, "\x1b[H", "\x1bOH"}, {KEY_END, "\x1b[F", "\x1bOF"},
    {104, "\x1b[5~", 0}, {109, "\x1b[6~", 0},
    {110, "\x1b[2~", 0}, {KEY_DELETE, "\x1b[3~", 0},
    {59, "\x1bOP", 0}, {60, "\x1bOQ", 0}, {61, "\x1bOR", 0}, {62, "\x1bOS", 0},
    {63, "\x1b[15~", 0}, {64, "\x1b[17~", 0}, {65, "\x1b[18~", 0}, {66, "\x1b[19~", 0},
    {67, "\x1b[20~", 0}, {68, "\x1b[21~", 0}, {87, "\x1b[23~", 0}, {88, "\x1b[24~", 0},
};

static string_address key_sequence(unsigned int code)
{
        for (positive i = 0; i < array_count(key_sequences); i++)
                if (key_sequences[i].code == code)
                        return (string_address)(application_keys && key_sequences[i].application
                                                    ? key_sequences[i].application
                                                    : key_sequences[i].sequence);

        return null;
}

/*
        One keystroke, turned into whatever it means.

        With the line editor running that is a change to the buffer and what
        is drawn; without it, it is the byte or the sequence going straight
        out, which is all this ever did.
*/
static fn SPARE term_key_modified(unsigned int character, unsigned int code,
                                  unsigned int modifiers)
{
        string_address sequence;
        unsigned int held = modifiers &
            (WINDOW_KEY_SHIFT | WINDOW_KEY_ALT | WINDOW_KEY_CONTROL);

        if (line_editing && line_key(character, code))
        {
                line_show();
                return;
        }

        if (character)
        {
                if (held & WINDOW_KEY_ALT)
                        emit(27);

                emit(character);
                return;
        }

        sequence = key_sequence(code);

        if (!sequence)
                return;

        /*
                The same sequence, with what was held written into it.

                ECMA-48 leaves this to the terminal and xterm settled it: the
                second parameter is one plus the sum of shift, alt and control,
                so Shift+Right is CSI 1;2C and Ctrl+Right is CSI 1;5C. A
                sequence that ends in a tilde takes the parameter before the
                tilde instead, which is why the two are built separately.

                Without this a program on the far end cannot tell Shift+Right
                from Right, because nothing in the bytes says. The compositor
                has known which modifiers were down since keys.c read them; it
                is only this last step that threw the answer away.
        */
        if (held)
        {
                unsigned int value =
                    1 + ((held & WINDOW_KEY_SHIFT) ? 1 : 0) +
                    ((held & WINDOW_KEY_ALT) ? 2 : 0) +
                    ((held & WINDOW_KEY_CONTROL) ? 4 : 0);
                positive length = string_length((string_address)sequence);

                if (length > 2 && sequence[1] == '[' &&
                    sequence[length - 1] == '~')
                {
                        emit_bytes((address_any)sequence, length - 1);
                        emit(';');
                        emit('0' + value);
                        emit('~');
                        return;
                }

                if (length == 3 && (sequence[1] == '[' || sequence[1] == 'O'))
                {
                        emit(27);
                        emit('[');
                        emit('1');
                        emit(';');
                        emit('0' + value);
                        emit(sequence[2]);
                        return;
                }
        }

        emit_bytes((address_any)sequence,
                   string_length((string_address)sequence));
}

// The test fixture and any caller without compositor modifiers use the
// ordinary two-argument spelling.
static fn SPARE term_key(unsigned int character, unsigned int code)
{
        term_key_modified(character, code, 0);
}

/*
        Whether the far end is assembling lines itself.

        ICANON off is a program that wants its own keys, and handing it a line
        at a time would be handing it nothing at all until Enter. So the
        editor is exactly as on as the line discipline it stands in for.
*/
static fn SPARE term_line_editing(b32 on)
{
        if (line_editing == on)
                return;

        line_editing = on;
        line_reset();
}

static fn term_pointer(unsigned int col, unsigned int row,
                       unsigned int button, unsigned int flags)
{
        unsigned int code;
        b32 down = (flags & WINDOW_KEY_DOWN) != 0;
        b32 move = (flags & WINDOW_KEY_POINTER_MOVE) != 0;

        if (!mouse_mode)
                return;
        if (move && mouse_mode == 1000)
                return;
        if (move && mouse_mode == 1002 && !down)
                return;
        if (!col)
                col = 1;
        if (!row)
                row = 1;

        code = button;
        if (move)
                code += 32;

        if (mouse_sgr)
        {
                emit_literal("\x1b[<");
                positive_to_string(emit_bytes, code);
                emit(';');
                positive_to_string(emit_bytes, col);
                emit(';');
                positive_to_string(emit_bytes, row);
                emit((down || move) ? 'M' : 'm');
                return;
        }

        if (col > 223)
                col = 223;
        if (row > 223)
                row = 223;
        if (!down && !move)
                code = 3;
        emit(27);
        emit('[');
        emit('M');
        emit((p8)(32 + code));
        emit((p8)(32 + col));
        emit((p8)(32 + row));
}

static fn term_focus(b32 in)
{
        if (!focus_events)
                return;

        if (in)
                emit_literal("\x1b[I");
        else
                emit_literal("\x1b[O");
}

#endif

#ifndef KERNEL_MODE

#define F_GETFD 1

/*
        Making sure the first three descriptors are taken.

        A program the kernel started has none open at all, so the pty would be
        descriptor zero and its other end descriptor one -- and then handing
        those to a shell as its input and output means dup3 onto a descriptor
        that is already the thing being duplicated, and closing the master
        closes what was just set up. Started from a shell the three are
        already taken and none of this is visible, which is exactly why it was
        not.
*/
fn claim_standard_descriptors()
{
        for (b32 i = 0; i < 3; i++)
                if (system_call_3(syscall(fcntl), i, F_GETFD, 0) < 0)
                        system_open_at(AT_FDCWD, "/dev/null",
                                      FILE_READ_WRITE);
}

#endif

// Where a row of the screen is once it is ROWS tall instead of was_rows:
// anchored at the bottom, with added blank lines below everything it held.
static unsigned int regrid_row(unsigned int at, unsigned int was_rows,
                               unsigned int added)
{
        if (ROWS >= was_rows)
                at += ROWS - was_rows - added;
        else if (at >= was_rows - ROWS)
                at -= was_rows - ROWS;
        else
                at = 0;

        return at < ROWS ? at : ROWS - 1;
}

/*
        The window was resized.

        Nothing is copied and nothing moves. The rows are the last lines of the
        ring whatever there are of them, so a window made taller takes in the
        lines that had scrolled off the top rather than blank ones, and the
        cursor is still on the line it was on -- that many rows further down.

        What was on the screen stays on it. Lines are not moved or shortened
        here: the compositor folds a stored line at the width it is drawn in,
        so narrowing and then widening a window is lossless. The pty learns
        the new grid below and future output uses it; an application that owns
        the screen can then redraw in response to SIGWINCH.
*/
fn regrid(b32 master)
{
        unsigned int was_rows = ROWS;
        unsigned int columns = window->columns;
        unsigned int rows = window->rows;
        unsigned int added = 0;
#ifndef KERNEL_MODE
        b32 cursor_was_shown = shown;
#endif

        /* A resize is itself a synchronous publication in the new geometry.
           End any program-held frame first; SIGWINCH will ask a full-screen
           program to begin and draw another transaction at the new size. */
        synchronized_output = false;

        // Undone in the geometry it was made in. cursor_show inverts a cell in
        // place, so clearing "shown" and moving on left that cell inverted for
        // good.
        cursor_hide();

#ifndef KERNEL_MODE
        if (line_editing)
                line_erase(true);
#endif

        // A window narrower or shorter than one cell is not a grid, and every
        // wrap and scroll below divides by these: zero rows had put wrapped
        // forever looking for a row to land on.
        COLUMNS = columns ? columns : 1;
        ROWS = rows ? rows : 1;

        if (COLUMNS > window->stride)
                COLUMNS = window->stride;

        /*
                The alternate screen is the lines after alternate_head, and a
                taller window must not reach back past it into the primary
                screen it keeps: that put the shell's last lines above a
                full-screen program's picture, and left the picture on the
                shell's screen once the program ended. The new rows are blank
                lines at the bottom, and the cursor does not move for them.
        */
        if (alternate)
                while (window->head - alternate_head < ROWS)
                {
                        window_scroll(window);
                        added++;
                }

        row = regrid_row(row, was_rows, added);
        cursor_saved.row = regrid_row(cursor_saved.row, was_rows, added);

        if (alternate)
                cursor_primary.row = regrid_row(cursor_primary.row, was_rows, 0);

        if (column >= COLUMNS)
                column = COLUMNS - 1;

        // The anchor of the line being typed is a column too, and a narrower
        // window has fewer of them to be at.
        if (line_anchor_column >= COLUMNS)
                line_anchor_column = COLUMNS - 1;

        // A region is measured in rows that may no longer be there.
        region_top = 0;
        region_bottom = ROWS;

        // The line being typed is anchored to a line of the ring, so it
        // survives the resize. Redraw it now at the new width: waiting for the
        // next keystroke left the cursor and the editable text in the old
        // geometry even though the window had already changed underneath it.
        line_drawn = 0;
#ifndef KERNEL_MODE
        line_view = 0;
#endif

#ifndef KERNEL_MODE
        if (line_editing)
                line_show();
#endif

        shown = false;
        touch_all();

        window_grid(window, COLUMNS, ROWS);

#ifndef KERNEL_MODE
        /*
                A resize is one transaction, including the cursor.

                Leaving it hidden here and relying on the caller's ordinary
                end-of-loop redraw made the resize path the only state in
                which the cursor could remain absent until some later input.
                Publish the new grid, put the cursor into that grid, damage
                it, and synchronously commit before returning. master is -1
                in the pure emulator harness and in the kernel console; both
                have an owner that paints their cells directly.
        */
        if (master >= 0 || cursor_was_shown)
                cursor_show();

        if (master >= 0)
        {
                window_damage(window, 0, ROWS);
                window_flush(window);

                // That damage was consumed by the synchronous commit. Keep
                // later input in this same loop iteration visible, but do not
                // manufacture a second cursor-only flush at the loop tail.
                touched_top = ROWS;
                touched_bottom = 0;
        }
#endif

#ifndef KERNEL_MODE
        winsize size;

        size.rows = (unsigned short)ROWS;
        size.columns = (unsigned short)COLUMNS;
        size.x_pixels = (unsigned short)(COLUMNS * WINDOW_CELL_W);
        size.y_pixels = (unsigned short)(ROWS * WINDOW_CELL_H);

        system_control(master, TIOCSWINSZ, address_of size);
#endif
}
