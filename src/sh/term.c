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
        a test is, and src/test/term.sh drives it with no kernel underneath at
        all. The two functions at the bottom are the exception and they are the
        only ones: everything the parser, the keys and the line editor do is
        stores into a page.
*/

#define COLUMNS_WANTED 80
#define ROWS_WANTED 24
#define SHELL "/shell"

#define EINTR 4
#define EAGAIN 11

// Three below are the shell's, and the kernel's console has no use for them.
// Not the attribute that keeps a symbol: this one lets the compiler drop what
// it finds no call to, and only quiets the warning about having found none.
#define SPARE __attribute__((unused))
// The kernel spells all four itself, and the two below are only wanted where
// there is a pty to say them to.
#ifndef KERNEL_MODE
#define TIOCSPTLCK 0x40045431u
#define TIOCGPTN 0x80045430u
#define TIOCSCTTY 0x540Eu
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
static b32 reverse;
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

// Where the block cursor was put, so it can be taken back off.
static unsigned int shown_row, shown_column;
static b32 shown;

static unsigned int saved_row, saved_column;
static unsigned char saved_ink = 7, saved_paper = 0;
static b32 saved_reverse;

// Tab stops, one byte a column, which is what makes HTS and TBC mean
// anything. Wide enough for a column of pixels on any screen this runs on.
#define TAB_STOPS 1024
static p8 tab_stop[TAB_STOPS];

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
static struct window_cell address_to row_cells(unsigned int r)
{
        return window_line(window, window->head - ROWS + r);
}

static unsigned int address_to row_length(unsigned int r)
{
        return window_length(window, window->head - ROWS + r);
}

/*
        Blanked in the colours in force, not in the ones it started in.

        An erase takes the current background, which is what lets a program
        paint a panel by setting a colour and clearing to the end of the line.
        Filling with black instead made every such program draw a black hole.
*/
static fn cell_clear(unsigned int r, unsigned int c)
{
        struct window_cell address_to cell = row_cells(r) + c;

        cell->character = ' ';
        cell->ink = reverse ? paper : ink;
        cell->paper = reverse ? ink : paper;
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

        while (address_to length < to)
        {
                cell_clear(r, address_to length);
                address_to length = address_to length + 1;
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

        for (unsigned int c = at; c < at + count && c < last; c++)
                cell_clear(row, c);

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
#define TO_SHELL_MAX 2048
static p8 to_shell[TO_SHELL_MAX];
static unsigned int to_shell_length;

static fn emit(unsigned int byte)
{
        if (to_shell_length < TO_SHELL_MAX)
                to_shell[to_shell_length++] = (p8)byte;
}

static fn emit_bytes(address_any data, positive length)
{
        positive room = TO_SHELL_MAX - to_shell_length;
        positive take = length < room ? length : room;

        if (take)
                memory_copy_fast(to_shell + to_shell_length, data, take);

        to_shell_length += (unsigned int)take;
}

static fn emit_string(const char address_to text)
{
        emit_bytes((address_any)text, string_length((string_address)text));
}

// One escape sequence at a time, so the parser is a state and a few numbers.
#define PARAMETERS 16
static unsigned int parameters[PARAMETERS];
static unsigned int parameter_count;
static b32 in_escape, in_csi, in_string, escape_intermediate, csi_private;

// A string sequence ends at ST, and ST is two bytes with an ESC in front.
static b32 string_escape;

static fn erase(unsigned int from_row, unsigned int from_column,
                unsigned int to_row, unsigned int to_column)
{
        unsigned int r, c;

        for (r = from_row; r <= to_row && r < ROWS; r++)
        {
                unsigned int first = r == from_row ? from_column : 0;
                unsigned int last = r == to_row ? to_column : COLUMNS - 1;
                unsigned int address_to length = row_length(r);

                for (c = first; c <= last && c < COLUMNS; c++)
                        cell_clear(r, c);

                // An erase that reaches the end of a line is the line getting
                // shorter, which is cheaper than the cells it would have
                // written and is what stops the width of a window deciding how
                // much of it is blanked.
                if (last + 1 >= address_to length && first < address_to length)
                        address_to length = first;
        }
}

static fn tabs_reset()
{
        for (unsigned int c = 0; c < TAB_STOPS; c++)
                tab_stop[c] = (p8)(c % 8 == 0);
}

static fn tab_forward()
{
        unsigned int c = column;

        while (++c < COLUMNS)
                if (c >= TAB_STOPS || tab_stop[c])
                        break;

        column = c < COLUMNS ? c : COLUMNS - 1;
}

/*
        Two hundred and fifty six colours said in sixteen.

        A cell carries an index into the sixteen a terminal has always had, so
        the cube and the greys have to land on one of those rather than be
        dropped -- and dropped is what they were: the number after 38;5 fell
        through to the plain foreground range and 38;5;31m painted the text
        red for no reason anyone asked for.
*/
static unsigned char colour_256(unsigned int n)
{
        unsigned int r, g, b;

        if (n < 16)
                return (unsigned char)n;

        if (n >= 232)
        {
                unsigned int grey = n < 256 ? n - 232 : 23;

                return grey < 4 ? 0 : grey < 12 ? 8 : grey < 20 ? 7 : 15;
        }

        n -= 16;
        b = n % 6;
        n /= 6;
        g = n % 6;
        r = n / 6;

        return (unsigned char)((r > 3 || g > 3 || b > 3 ? 8 : 0) |
                               (r > 2 ? 1 : 0) | (g > 2 ? 2 : 0) | (b > 2 ? 4 : 0));
}

static unsigned char colour_rgb(unsigned int r, unsigned int g, unsigned int b)
{
        return (unsigned char)((r > 192 || g > 192 || b > 192 ? 8 : 0) |
                               (r > 96 ? 1 : 0) | (g > 96 ? 2 : 0) | (b > 96 ? 4 : 0));
}

// Returns how many parameters past this one it took, so the caller can step
// over the ones an extended colour is spelled with.
static unsigned int sgr_extended(unsigned int at, unsigned char address_to which)
{
        unsigned int kind = at + 1 < parameter_count ? parameters[at + 1] : 0;

        if (kind == 5 && at + 2 < parameter_count)
        {
                address_to which = colour_256(parameters[at + 2]);
                return 2;
        }

        if (kind == 2 && at + 4 < parameter_count)
        {
                address_to which = colour_rgb(parameters[at + 2], parameters[at + 3],
                                              parameters[at + 4]);
                return 4;
        }

        return parameter_count - at - 1;
}

static fn sgr()
{
        if (!parameter_count)
        {
                ink = 7;
                paper = 0;
                reverse = false;
                return;
        }

        for (unsigned int i = 0; i < parameter_count; i++)
        {
                unsigned int p = parameters[i];

                if (p == 0)
                {
                        ink = 7;
                        paper = 0;
                        reverse = false;
                }
                else if (p == 1)
                        ink |= 8;
                else if (p == 2 || p == 21 || p == 22)
                        ink &= 7;
                else if (p == 7)
                        reverse = true;
                else if (p == 27)
                        reverse = false;
                else if (p >= 30 && p <= 37)
                        ink = (unsigned char)((ink & 8) | (p - 30));
                else if (p == 38)
                        i += sgr_extended(i, address_of ink);
                else if (p == 39)
                        ink = (unsigned char)((ink & 8) | 7);
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
static unsigned int alternate_head, alternate_row, alternate_column;

static fn cursor_save()
{
        saved_row = row;
        saved_column = column;
        saved_ink = ink;
        saved_paper = paper;
        saved_reverse = reverse;
}

static fn cursor_restore()
{
        row = saved_row < ROWS ? saved_row : ROWS - 1;
        column = saved_column < COLUMNS ? saved_column : COLUMNS - 1;
        ink = saved_ink;
        paper = saved_paper;
        reverse = saved_reverse;
}

static fn alternate_enter()
{
        if (alternate)
                return;

        cursor_save();
        alternate_head = window->head;
        alternate_row = row;
        alternate_column = column;

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

        row = alternate_row < ROWS ? alternate_row : ROWS - 1;
        column = alternate_column < COLUMNS ? alternate_column : COLUMNS - 1;
        alternate = false;
        cursor_restore();
        touch_all();
}

static fn mode_set(b32 on)
{
        for (unsigned int i = 0; i < parameter_count; i++)
        {
                unsigned int p = parameters[i];

                if (!csi_private)
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
                case 47:
                case 1047:
                case 1049:
                        if (on)
                                alternate_enter();
                        else
                                alternate_leave();
                        break;
                }
        }
}

static fn full_reset()
{
        ink = 7;
        paper = 0;
        reverse = false;
        row = 0;
        column = 0;
        region_top = 0;
        region_bottom = ROWS;
        autowrap = true;
        insert_mode = false;
        cursor_visible = true;
        application_keys = false;
        alternate_leave();
        tabs_reset();
        erase(0, 0, ROWS - 1, COLUMNS - 1);
}

static fn csi_final(unsigned int final)
{
        unsigned int a = parameter_count > 0 && parameters[0] ? parameters[0] : 1;
        unsigned int b = parameter_count > 1 && parameters[1] ? parameters[1] : 1;
        unsigned int i;

        switch (final)
        {
        case 'H':
        case 'f':
                row = a - 1 < ROWS ? a - 1 : ROWS - 1;
                column = b - 1 < COLUMNS ? b - 1 : COLUMNS - 1;
                break;
        case 'A':
                row = row > a ? row - a : 0;
                break;
        case 'B':
                row = row + a < ROWS ? row + a : ROWS - 1;
                break;
        case 'C':
                column = column + a < COLUMNS ? column + a : COLUMNS - 1;
                break;
        case 'D':
                column = column > a ? column - a : 0;
                break;
        case 'E':
                row = row + a < ROWS ? row + a : ROWS - 1;
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
                row = a - 1 < ROWS ? a - 1 : ROWS - 1;
                break;
        case 'J':
                if (parameter_count && parameters[0] == 1)
                        erase(0, 0, row, column);
                else if (parameter_count && parameters[0] >= 2)
                        erase(0, 0, ROWS - 1, COLUMNS - 1);
                else
                        erase(row, column, ROWS - 1, COLUMNS - 1);
                break;
        case 'K':
                if (parameter_count && parameters[0] == 1)
                        erase(row, 0, row, column);
                else if (parameter_count && parameters[0] == 2)
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

                for (i = column; i < column + a && i < COLUMNS; i++)
                        cell_clear(row, i);
                break;
        case 'S':
                scroll_up(a);
                break;
        case 'T':
                scroll_down(a);
                break;
        case 'g':
                if (parameter_count && parameters[0] == 3)
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
                if (parameter_count && parameters[0] == 6)
                {
                        emit_string("\x1b[");
                        positive_to_string(emit_bytes, row + 1);
                        emit(';');
                        positive_to_string(emit_bytes,
                                           column < COLUMNS ? column + 1 : COLUMNS);
                        emit('R');
                }
                else if (parameter_count && parameters[0] == 5)
                        emit_string("\x1b[0n");
                break;
        case 'c':
                // A VT102, which is what the sequences above add up to.
                emit_string("\x1b[?6c");
                break;
        case 'r':
        {
                unsigned int top;
                unsigned int bottom;

                if (csi_private)
                        break;

                top = parameter_count > 0 && parameters[0] ? parameters[0] - 1 : 0;
                bottom = parameter_count > 1 && parameters[1] ? parameters[1] : ROWS;

                if (bottom > ROWS)
                        bottom = ROWS;

                // A region has to have two rows in it to scroll, and one that
                // does not is left alone rather than half applied.
                if (top + 1 < bottom)
                {
                        region_top = top;
                        region_bottom = bottom;

                        // Home is the top of the page and not the top of the
                        // region, origin mode being what changes that and
                        // origin mode being off.
                        row = 0;
                        column = 0;
                }
                break;
        }
        case 's':
                cursor_save();
                break;
        case 'u':
                cursor_restore();
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
static unsigned int utf8_code, utf8_left, utf8_least;

static fn utf8_byte(unsigned int c)
{
        if (utf8_left)
        {
                if ((c & 0xc0) != 0x80)
                {
                        utf8_left = 0;
                        put(0xfffd);
                        return;
                }

                utf8_code = (utf8_code << 6) | (c & 0x3f);

                if (--utf8_left)
                        return;

                // Overlong, and the halves of a surrogate pair, are not
                // characters and a terminal that lets them through is a
                // terminal that can be told to hold what its font cannot draw.
                put(utf8_code < utf8_least || (utf8_code >= 0xd800 && utf8_code < 0xe000) ||
                            utf8_code > 0x10ffff
                        ? 0xfffd
                        : utf8_code);
                return;
        }

        if (c < 0xc0)
        {
                put(0xfffd);
                return;
        }

        if (c < 0xe0)
        {
                utf8_code = c & 0x1f;
                utf8_left = 1;
                utf8_least = 0x80;
        }
        else if (c < 0xf0)
        {
                utf8_code = c & 0x0f;
                utf8_left = 2;
                utf8_least = 0x800;
        }
        else if (c < 0xf8)
        {
                utf8_code = c & 0x07;
                utf8_left = 3;
                utf8_least = 0x10000;
        }
        else
                put(0xfffd);
}

static fn line_forget();

// A sequence that has stopped short is one character that never arrived, and
// whatever ended it is not it.
static fn utf8_flush()
{
        if (!utf8_left)
                return;

        utf8_left = 0;
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
                in_escape = false;
                in_csi = false;
                in_string = false;
                utf8_left = 0;
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
                                in_string = false;
                                in_escape = false;
                        }

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
                string_escape = false;
                in_string = false;

                if (c == '\\')
                {
                        in_escape = false;
                        return;
                }

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
                if (c >= '0' && c <= '9')
                {
                        if (!parameter_count)
                                parameter_count = 1;

                        parameters[parameter_count - 1] =
                            parameters[parameter_count - 1] * 10 + (c - '0');
                        return;
                }

                /*
                        A separator ends the parameter it follows, so an empty
                        one before it counts. Beginning a parameter here
                        instead read ESC[;5H as ESC[5H, and a row nobody asked
                        to move became the row moved to.
                */
                if (c == ';' || c == ':')
                {
                        if (!parameter_count)
                                parameter_count = 1;

                        if (parameter_count < PARAMETERS)
                                parameters[parameter_count++] = 0;

                        return;
                }

                // The marker that says the parameters are a private mode's
                // and not ECMA-48's, which is the whole difference between
                // ESC[?25l and ESC[25l.
                if (c == '?' || c == '<' || c == '=' || c == '>')
                {
                        csi_private = true;
                        return;
                }

                if (c >= '@' && c <= '~')
                {
                        csi_final(c);
                        in_csi = false;
                        in_escape = false;
                }

                return;
        }

        if (in_escape)
        {
                if (c == '[')
                {
                        in_csi = true;
                        csi_private = false;
                        parameter_count = 0;
                        parameters[0] = 0;
                        return;
                }

                if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_')
                {
                        in_string = true;
                        string_escape = false;
                        return;
                }

                // ESC ( B and its like: an intermediate byte says the sequence
                // carries on to a final one. Ending the sequence at the first
                // of them printed the rest of it on the screen.
                if (c >= ' ' && c <= '/')
                {
                        escape_intermediate = true;
                        return;
                }

                in_escape = false;

                // A final after an intermediate belongs to that sequence, so
                // ESC # 8 is one thing and not a DECRC hiding behind a hash.
                if (escape_intermediate)
                {
                        escape_intermediate = false;
                        return;
                }

                switch (c)
                {
                case '7':
                        cursor_save();
                        break;
                case '8':
                        cursor_restore();
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
        case 14:
        case 15:
                return;
        }

        if (c < ' ' || c == 127)
                return;

        if (c < 128)
        {
                utf8_flush();
                put(c);
                return;
        }

        utf8_byte(c);
}

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

        if (!cursor_visible)
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
#define LINE_MAX 1024
#define LINE_HISTORY 32

// How much of a prompt Ctrl-L can put back, the prompt being cells rather than
// anything the shell ever said the text of.
#define LINE_PROMPT_MAX 128

static unsigned int line_drawn;
static b32 line_anchored;

// Where on the screen the line starts, as a line of the ring rather than a
// row: the screen scrolls out from under a long line, and a row would then be
// pointing at somebody else's text. Out here with the rest of what the output
// half touches, because a resize has an opinion about the column.
#ifndef KERNEL_MODE
static unsigned int line_anchor;
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
}

#ifndef KERNEL_MODE

static b32 line_editing;
static p8 line[LINE_MAX];
static unsigned int line_length, line_point;


static p8 history[LINE_HISTORY][LINE_MAX];
static unsigned int history_length[LINE_HISTORY];
static unsigned int history_count, history_at;

// The line being typed, kept while the history is walked so that coming back
// down off the end returns it rather than an empty line.
static p8 history_held[LINE_MAX];
static unsigned int history_held_length;

static unsigned int line_anchor_row()
{
        unsigned int at = line_anchor - (window->head - ROWS);

        return at < ROWS ? at : 0;
}

/*
        The line, on the screen, wherever it now is.

        Everything is written again from the anchor: it is at most a screenful
        of cells and it is the only way a character put in the middle can move
        the ones after it. What the last pass wrote is blanked by writing over
        it, because a line that got shorter has to stop being on the screen.
*/
static fn line_show()
{
        unsigned int at, written;

        if (!line_anchored)
                return;

        // The anchor went off the top, so there is nowhere left to write the
        // line but the top of what is still on screen.
        if (line_anchor - (window->head - ROWS) >= ROWS)
        {
                line_anchor = window->head - ROWS;
                line_anchor_column = 0;
        }

        row = line_anchor_row();
        column = line_anchor_column;

        for (at = 0; at < line_length; at++)
                put(line[at]);

        for (written = line_length; written < line_drawn; written++)
                put(' ');

        line_drawn = line_length;

        at = line_anchor_row() * COLUMNS + line_anchor_column + line_point;
        row = at / COLUMNS;
        column = at % COLUMNS;

        if (row >= ROWS)
        {
                row = ROWS - 1;
                column = COLUMNS - 1;
        }
}

static fn line_insert(unsigned int character)
{
        if (line_length + 1 >= LINE_MAX)
                return;

        memory_copy(line + line_point + 1, line + line_point,
                    line_length - line_point);

        line[line_point++] = (p8)character;
        line_length++;
}

static fn line_take(unsigned int from, unsigned int to)
{
        unsigned int gone = to - from;

        memory_copy(line + from, line + to, line_length - to);

        line_length -= gone;
        line_point = from;
}

// The word before the cursor is the run of spaces then the run that is not.
static unsigned int line_word()
{
        unsigned int at = line_point;

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
        unsigned int length = history_at < history_count
                                  ? history_length[history_at % LINE_HISTORY]
                                  : history_held_length;

        memory_copy_fast(line, (address_any)from, length);

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
            history_length[(history_count - 1) % LINE_HISTORY] == line_length)
        {
                const p8 address_to last = history[(history_count - 1) % LINE_HISTORY];
                unsigned int at = 0;

                while (at < line_length && last[at] == line[at])
                        at++;

                if (at == line_length)
                        return;
        }

        slot = history_count % LINE_HISTORY;

        memory_copy_fast(history[slot], line, line_length);

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
        struct window_cell held[LINE_PROMPT_MAX];
        unsigned int kept = line_anchor_column < LINE_PROMPT_MAX ? line_anchor_column
                                                                 : LINE_PROMPT_MAX;
        unsigned int r = line_anchor_row();

        if (line_anchored)
        {
                struct window_cell address_to cells = row_cells(r);

                memory_copy_fast(held, cells,
                                 (positive)kept *
                                     sizeof(struct window_cell));
        }
        else
                kept = 0;

        erase(0, 0, ROWS - 1, COLUMNS - 1);

        for (unsigned int at = 0; at < kept; at++)
        {
                reach(0, at + 1);
                row_cells(0)[at] = held[at];
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

        line_length = 0;
        line_point = 0;
        history_at = history_count;
        history_held_length = 0;
        line_forget();
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
                line_anchor = window->head - ROWS + row;
                line_anchor_column = column < COLUMNS ? column : COLUMNS - 1;
                line_anchored = true;
                line_drawn = 0;
        }

        switch (code)
        {
        case KEY_LEFT:
                if (line_point)
                        line_point--;
                return true;
        case KEY_RIGHT:
                if (line_point < line_length)
                        line_point++;
                return true;
        case KEY_HOME:
                line_point = 0;
                return true;
        case KEY_END:
                line_point = line_length;
                return true;
        case KEY_DELETE:
                if (line_point < line_length)
                        line_take(line_point, line_point + 1);
                return true;
        case KEY_UP:
                if (!history_at)
                        return true;

                if (history_at == history_count)
                {
                        memory_copy_fast(history_held, line, line_length);

                        history_held_length = line_length;
                }

                history_at--;
                line_from_history();
                return true;
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
        for each of them. TERM=ansi is exported below, and this is what makes
        that true rather than a claim.
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
        for (positive i = 0; i < sizeof(key_sequences) / sizeof(key_sequences[0]); i++)
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
static fn SPARE term_key(unsigned int character, unsigned int code)
{
        string_address sequence;

        if (line_editing && line_key(character, code))
        {
                line_show();
                return;
        }

        if (character)
        {
                emit(character);
                return;
        }

        sequence = key_sequence(code);

        if (sequence)
                emit_bytes((address_any)sequence,
                           string_length((string_address)sequence));
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
        line_length = 0;
        line_point = 0;
        line_forget();
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
                        system_call_4(syscall(openat), AT_FDCWD, (positive)"/dev/null",
                                      FILE_READ_WRITE, 0);
}

#endif

/*
        The window was resized.

        Nothing is copied and nothing moves. The rows are the last lines of the
        ring whatever there are of them, so a window made taller takes in the
        lines that had scrolled off the top rather than blank ones, and the
        cursor is still on the line it was on -- that many rows further down.

        What was on the screen stays on it. Lines are not folded at the new
        width here: the compositor wraps a line longer than the window it is
        drawn in, and doing that to a live row would move the cursor of the
        program on the other end of the pty, which is not this program's to
        move. So the rows are clipped, and it is the scrollback above them
        that re-wraps.
*/
fn regrid(b32 master)
{
        unsigned int was_rows = ROWS;
        unsigned int columns = window->columns;
        unsigned int rows = window->rows;

        // Undone in the geometry it was made in. cursor_show inverts a cell in
        // place, so clearing "shown" and moving on left that cell inverted for
        // good.
        cursor_hide();

        // A window narrower or shorter than one cell is not a grid, and every
        // wrap and scroll below divides by these: zero rows had put wrapped
        // forever looking for a row to land on.
        COLUMNS = columns ? columns : 1;
        ROWS = rows ? rows : 1;

        if (COLUMNS > window->stride)
                COLUMNS = window->stride;

        if (ROWS >= was_rows)
                row += ROWS - was_rows;
        else if (row >= was_rows - ROWS)
                row -= was_rows - ROWS;
        else
                row = 0;

        if (row >= ROWS)
                row = ROWS - 1;

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
        // survives the resize; what it cannot survive is being redrawn at a
        // width it was not written at, and the next keystroke does that.
        line_drawn = 0;

        for (unsigned int r = 0; r < ROWS; r++)
        {
                unsigned int address_to length = row_length(r);

                if (address_to length > COLUMNS)
                        address_to length = COLUMNS;
        }

        shown = false;
        touch_all();

        window_grid(window, COLUMNS, ROWS);

#ifndef KERNEL_MODE
        winsize size;

        size.rows = (unsigned short)ROWS;
        size.columns = (unsigned short)COLUMNS;
        size.x_pixels = (unsigned short)(COLUMNS * WINDOW_CELL_W);
        size.y_pixels = (unsigned short)(ROWS * WINDOW_CELL_H);

        system_call_3(syscall(ioctl), master, TIOCSWINSZ, (positive)address_of size);
#endif
}
