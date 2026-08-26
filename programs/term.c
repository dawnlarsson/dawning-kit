#include "../src/library.c"
#include "../src/canvas/window.c"

/*
        A terminal.

        The pty and the escape sequences are here; the glyphs are not. The
        program writes characters and colour indices into cells and Canvas
        draws them, so there is no font in this file and no framebuffer --
        a keystroke is eight bytes, and scrolling is a memmove.
*/

#define COLUMNS 80
#define ROWS 24
#define SHELL "/shell"

#define O_NONBLOCK 04000
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

                if (c == ';')
                {
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
                }
                else
                {
                        in_escape = false;
                }

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
        struct window_cell address_to cell = cells + row * COLUMNS + column;
        unsigned char was = cell->ink;

        cell->ink = cell->paper;
        cell->paper = was;
        shown_row = row;
        shown_column = column;
        shown = true;
        touch(row);
}

b32 main()
{
        window = window_open_text(COLUMNS, ROWS);

        if (!window)
        {
                log_direct(str("term: no window\n"));
                return 1;
        }

        cells = window_cells(window);
        erase(0, 0, ROWS - 1, COLUMNS - 1);

        b32 master = system_call_4(syscall(openat), AT_FDCWD, (positive)"/dev/ptmx",
                                   FILE_READ_WRITE | O_NONBLOCK, 0);

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

        winsize size = {ROWS, COLUMNS, COLUMNS * WINDOW_CELL_W, ROWS * WINDOW_CELL_H};

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

        for (;;)
        {
                touched_top = ROWS;
                touched_bottom = 0;

                cursor_hide();

                struct window_key key;

                while (window_key(window, &key))
                {
                        if (!(key.flags & WINDOW_KEY_DOWN) || !key.character)
                                continue;

                        p8 byte = (p8)key.character;

                        system_call_3(syscall(write), master, (positive)address_of byte, 1);
                }

                for (;;)
                {
                        bipolar got = system_call_3(syscall(read), master,
                                                    (positive)from_shell,
                                                    sizeof(from_shell));

                        if (got <= 0)
                                break;

                        for (bipolar i = 0; i < got; i++)
                                consume(from_shell[i]);

                }

                cursor_show();

                if (touched_bottom > touched_top)
                {
                        window_damage(window, touched_top, touched_bottom - touched_top);

                        // Now, not on the compositor's clock: what has been
                        // typed should not wait for a frame it is not on.
                        window_flush(window);
                }

                system_call_2(syscall(nanosleep), (positive)address_of nap, 0);
        }

        return 0;
}
