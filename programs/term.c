#include "../src/library.c"
#include "../src/canvas/window.c"

/*
        A terminal.

        The pty and the escape sequences are here; the glyphs are not. The
        program writes characters and colour indices into cells and Canvas
        draws them, so there is no font in this file and no framebuffer --
        a keystroke is eight bytes, and scrolling is a memmove.
*/

#define COLUMNS_WANTED 80
#define ROWS_WANTED 24
#define SHELL "/shell"

#define O_NONBLOCK 04000
#define EINTR 4
#define EAGAIN 11
#define TIOCSPTLCK 0x40045431u
#define TIOCGPTN 0x80045430u
#define TIOCSCTTY 0x540Eu
#define TIOCSWINSZ 0x5414u

typedef struct
{
        unsigned short rows, columns, x_pixels, y_pixels;
} winsize;

static struct window *window;
static struct window_cell *cells;

// However many the compositor says there are, which changes when the window
// is resized.
static unsigned int COLUMNS, ROWS;
static unsigned int row, column;
static unsigned char ink = 7, paper = 0;
static unsigned int touched_top, touched_bottom;

// Where the block cursor was put, so it can be taken back off.
static unsigned int shown_row, shown_column;
static b32 shown;

static fn touch(unsigned int at)
{
        if (at < touched_top)
                touched_top = at;

        if (at + 1 > touched_bottom)
                touched_bottom = at + 1;
}

static fn cell_clear(unsigned int r, unsigned int c)
{
        struct window_cell address_to cell = cells + r * COLUMNS + c;

        cell->character = ' ';
        cell->ink = 7;
        cell->paper = 0;
        touch(r);
}

static fn scroll()
{
        unsigned int r, c;

        for (r = 1; r < ROWS; r++)
                for (c = 0; c < COLUMNS; c++)
                        cells[(r - 1) * COLUMNS + c] = cells[r * COLUMNS + c];

        for (c = 0; c < COLUMNS; c++)
                cell_clear(ROWS - 1, c);

        touched_top = 0;
        touched_bottom = ROWS;
        row = ROWS - 1;
}

static fn put(unsigned int character)
{
        struct window_cell address_to cell;

        if (column >= COLUMNS)
        {
                column = 0;
                row++;
        }

        while (row >= ROWS)
                scroll();

        cell = cells + row * COLUMNS + column;
        cell->character = character;
        cell->ink = ink;
        cell->paper = paper;

        touch(row);
        column++;
}

// One escape sequence at a time, so the parser is a state and a few numbers.
static unsigned int parameters[8];
static unsigned int parameter_count;
static b32 in_escape, in_csi;

static fn erase(unsigned int from_row, unsigned int from_column,
                unsigned int to_row, unsigned int to_column)
{
        unsigned int r, c;

        for (r = from_row; r <= to_row && r < ROWS; r++)
        {
                unsigned int first = r == from_row ? from_column : 0;
                unsigned int last = r == to_row ? to_column : COLUMNS - 1;

                for (c = first; c <= last && c < COLUMNS; c++)
                        cell_clear(r, c);
        }
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
        case 'J':
                if (parameter_count && parameters[0] == 2)
                        erase(0, 0, ROWS - 1, COLUMNS - 1);
                else if (parameter_count && parameters[0] == 1)
                        erase(0, 0, row, column);
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
        case 'm':
                if (!parameter_count)
                {
                        ink = 7;
                        paper = 0;
                        break;
                }

                for (i = 0; i < parameter_count; i++)
                {
                        unsigned int p = parameters[i];

                        if (p == 0)
                        {
                                ink = 7;
                                paper = 0;
                        }
                        else if (p == 1)
                                ink |= 8;
                        else if (p >= 30 && p <= 37)
                                ink = (unsigned char)((ink & 8) | (p - 30));
                        else if (p >= 90 && p <= 97)
                                ink = (unsigned char)(8 + (p - 90));
                        else if (p >= 40 && p <= 47)
                                paper = (unsigned char)(p - 40);
                        else if (p >= 100 && p <= 107)
                                paper = (unsigned char)(8 + (p - 100));
                }
                break;
        }
}

static fn consume(unsigned int c)
{
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
                if (c == ';')
                {
                        if (!parameter_count)
                                parameter_count = 1;

                        if (parameter_count < 8)
                                parameters[parameter_count++] = 0;

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
                        parameter_count = 0;
                        parameters[0] = 0;
                        return;
                }

                // ESC ( B and its like: an intermediate byte says the sequence
                // carries on to a final one. Ending the sequence at the first
                // of them printed the rest of it on the screen.
                if (c >= ' ' && c <= '/')
                        return;

                in_escape = false;
                return;
        }

        switch (c)
        {
        case 27:
                in_escape = true;
                return;
        case '\n':
                row++;
                while (row >= ROWS)
                        scroll();
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
                column = (column + 8) & ~7u;
                if (column >= COLUMNS)
                        column = COLUMNS - 1;
                return;
        case 7:
                return;
        }

        if (c >= ' ' && c < 127)
                put(c);
}

static fn cursor_hide()
{
        if (!shown)
                return;

        struct window_cell address_to cell = cells + shown_row * COLUMNS + shown_column;
        unsigned char was = cell->ink;

        cell->ink = cell->paper;
        cell->paper = was;
        touch(shown_row);
        shown = false;
}

static fn cursor_show()
{
        // put leaves column at COLUMNS after filling the last cell of a row and
        // only wraps on the next character, so the cursor has to be clamped:
        // unclamped it landed on the first cell of the row below, and on the
        // last row that is one cell past the grid.
        unsigned int at = column < COLUMNS ? column : COLUMNS - 1;
        struct window_cell address_to cell = cells + row * COLUMNS + at;
        unsigned char was = cell->ink;

        cell->ink = cell->paper;
        cell->paper = was;
        shown_row = row;
        shown_column = at;
        shown = true;
        touch(row);
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
} key_sequences[] = {
    {103, "\x1b[A"}, {108, "\x1b[B"}, {106, "\x1b[C"}, {105, "\x1b[D"},
    {102, "\x1b[H"}, {107, "\x1b[F"}, {104, "\x1b[5~"}, {109, "\x1b[6~"},
    {110, "\x1b[2~"}, {111, "\x1b[3~"},
    {59, "\x1bOP"}, {60, "\x1bOQ"}, {61, "\x1bOR"}, {62, "\x1bOS"},
    {63, "\x1b[15~"}, {64, "\x1b[17~"}, {65, "\x1b[18~"}, {66, "\x1b[19~"},
    {67, "\x1b[20~"}, {68, "\x1b[21~"}, {87, "\x1b[23~"}, {88, "\x1b[24~"},
};

static string_address key_sequence(unsigned int code)
{
        for (positive i = 0; i < sizeof(key_sequences) / sizeof(key_sequences[0]); i++)
                if (key_sequences[i].code == code)
                        return (string_address)key_sequences[i].sequence;

        return null;
}

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

/*
        The window was resized.

        The cells are one array with a row stride, so changing how many
        columns there are moves every row: growing means walking backwards so
        a row is never written over one not yet read, shrinking means walking
        forwards. What was on the screen stays on it -- lines do not reflow,
        they are kept and clipped, which is what a terminal without a
        scrollback can honestly do.
*/
fn regrid(b32 master)
{
        unsigned int was_columns;
        unsigned int was_rows;

        // Undone in the geometry it was made in. cursor_show inverts a cell in
        // place, so clearing "shown" and moving on left that cell inverted for
        // good, and the copy below carried it into the new layout.
        cursor_hide();

        was_columns = COLUMNS;
        was_rows = ROWS;
        unsigned int columns = window->columns;
        unsigned int rows = window->rows;
        unsigned int keep = was_columns < columns ? was_columns : columns;
        unsigned int carry = was_rows < rows ? was_rows : rows;
        winsize size;

        if (columns > was_columns)
        {
                for (unsigned int r = carry; r-- > 0;)
                {
                        for (unsigned int c = keep; c-- > 0;)
                                cells[r * columns + c] = cells[r * was_columns + c];

                        for (unsigned int c = keep; c < columns; c++)
                        {
                                cells[r * columns + c].character = ' ';
                                cells[r * columns + c].ink = 7;
                                cells[r * columns + c].paper = 0;
                        }
                }
        }
        else if (columns < was_columns)
        {
                for (unsigned int r = 0; r < carry; r++)
                        for (unsigned int c = 0; c < keep; c++)
                                cells[r * columns + c] = cells[r * was_columns + c];
        }

        COLUMNS = columns;
        ROWS = rows;

        for (unsigned int r = carry; r < rows; r++)
                for (unsigned int c = 0; c < columns; c++)
                        cell_clear(r, c);

        if (row >= ROWS)
                row = ROWS - 1;

        if (column >= COLUMNS)
                column = COLUMNS - 1;

        shown = false;
        touched_top = 0;
        touched_bottom = ROWS;

        window_grid(window, COLUMNS, ROWS);

        size.rows = (unsigned short)ROWS;
        size.columns = (unsigned short)COLUMNS;
        size.x_pixels = (unsigned short)(COLUMNS * WINDOW_CELL_W);
        size.y_pixels = (unsigned short)(ROWS * WINDOW_CELL_H);

        system_call_3(syscall(ioctl), master, TIOCSWINSZ, (positive)address_of size);
}

b32 main()
{
        claim_standard_descriptors();

        window = window_open_text(COLUMNS_WANTED, ROWS_WANTED);

        if (!window)
        {
                log_direct(str("term: no window\n"));
                return 1;
        }

        cells = window_cells(window);
        COLUMNS = window->columns;
        ROWS = window->rows;
        erase(0, 0, ROWS - 1, COLUMNS - 1);

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

        p8 name[16] = "/dev/pts/";
        positive at = 9;

        if (number >= 10)
                name[at++] = (p8)('0' + number / 10);

        name[at++] = (p8)('0' + number % 10);
        name[at] = 0;

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
        timespec nap = {0, 4000000};

        cursor_show();
        window_damage(window, 0, ROWS);
        window_commit(window);

        for (;;)
        {
                b32 changed = false;
                b32 gone = false;
                struct window_key key;

                touched_top = ROWS;
                touched_bottom = 0;

                if (window->columns != COLUMNS || window->rows != ROWS)
                {
                        regrid(master);
                        changed = true;
                }

                // Keys go out; they change nothing here until they come back.
                while (window_key(window, &key))
                {
                        if (!(key.flags & WINDOW_KEY_DOWN))
                                continue;

                        if (key.character)
                        {
                                p8 byte = (p8)key.character;

                                system_call_3(syscall(write), master,
                                              (positive)address_of byte, 1);
                                continue;
                        }

                        string_address sequence = key_sequence(key.code);

                        if (sequence)
                                system_call_3(syscall(write), master,
                                              (positive)sequence,
                                              string_length(sequence));
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

        system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        system_call_1(syscall(close), master);
        window_close(window);

        return 0;
}
