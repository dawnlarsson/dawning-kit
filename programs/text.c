#include "../src/library.c"
#include "../src/canvas/window.c"

// A window of text: the program writes cells, Canvas draws the glyphs.
//
// The size is the compositor's, not this program's: dragging an edge changes
// how many cells there are, and the layout is done again at whatever it
// becomes. window_grid is what says the cells are in that shape now.
#define COLUMNS_WANTED 60
#define ROWS_WANTED 18
#define TYPED_MAX 46

static struct window *window;
static unsigned int columns, rows;
static char typed[TYPED_MAX];
static unsigned int at;

static void say(unsigned int row, unsigned int column, const char *text,
                unsigned char ink, unsigned char paper)
{
        struct window_cell *cells;
        unsigned int i;

        if (row >= rows)
                return;

        cells = window_cells(window) + row * columns;

        for (i = 0; text[i] && column + i < columns; i++)
        {
                cells[column + i].character = (unsigned char)text[i];
                cells[column + i].ink = ink;
                cells[column + i].paper = paper;
        }

        window_damage(window, row, 1);
}

static void lay_out(void)
{
        struct window_cell *cells = window_cells(window);
        unsigned int row, column;

        columns = window->columns;
        rows = window->rows;

        for (row = 0; row < rows; row++)
                for (column = 0; column < columns; column++)
                {
                        cells[row * columns + column].character = ' ';
                        cells[row * columns + column].ink = 7;
                        cells[row * columns + column].paper = 0;
                }

        for (unsigned int i = 0; i < 8; i++)
                say(1, 2 + i * 6, "colour", (unsigned char)(8 + i), 0);

        say(3, 2, "Moonwater Canvas draws these glyphs.", 15, 0);
        say(4, 2, "This window is cells, not pixels.", 7, 0);
        say(6, 2, "Type here:", 11, 0);

        for (unsigned int i = 0; i < at; i++)
                if (rows > 6 && 13 + i < columns)
                {
                        struct window_cell *cell =
                            window_cells(window) + 6 * columns + 13 + i;

                        cell->character = (unsigned char)typed[i];
                        cell->ink = 15;
                        cell->paper = 4;
                }

        window_grid(window, columns, rows);
        window_damage(window, 0, rows);
}

static void hold_ms(long milliseconds)
{
        long timespec[2] = {0, milliseconds * 1000000};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

b32 main()
{
        window = window_open_text(COLUMNS_WANTED, ROWS_WANTED);

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
                                struct window_cell *cell =
                                    window_cells(window) + 6 * columns + 13 + at;

                                cell->character = key.character;
                                cell->ink = 15;
                                cell->paper = 4;
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
