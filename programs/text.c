#include "../src/library.c"
#include "../src/canvas/window.c"

// A window of text: the program writes cells, Canvas draws the glyphs.
#define COLUMNS 60
#define ROWS 18

static void say(struct window *window, unsigned int row, unsigned int column,
                const char *text, unsigned char ink, unsigned char paper)
{
        struct window_cell *cells = window_cells(window) + row * COLUMNS;
        unsigned int i;

        for (i = 0; text[i] && column + i < COLUMNS; i++)
        {
                cells[column + i].character = (unsigned char)text[i];
                cells[column + i].ink = ink;
                cells[column + i].paper = paper;
        }

        window_damage(window, row, 1);
}

static void hold_ms(long milliseconds)
{
        long timespec[2] = {0, milliseconds * 1000000};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

b32 main()
{
        struct window *window = window_open_text(COLUMNS, ROWS);
        struct window_cell *cells;
        unsigned int row, column, at = 0;

        if (!window)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        cells = window_cells(window);

        for (row = 0; row < ROWS; row++)
                for (column = 0; column < COLUMNS; column++)
                {
                        cells[row * COLUMNS + column].character = ' ';
                        cells[row * COLUMNS + column].ink = 7;
                        cells[row * COLUMNS + column].paper = 0;
                }

        for (unsigned int i = 0; i < 8; i++)
                say(window, 1, 2 + i * 6, "colour", (unsigned char)(8 + i), 0);

        say(window, 3, 2, "Moonwater Canvas draws these glyphs.", 15, 0);
        say(window, 4, 2, "This window is cells, not pixels.", 7, 0);
        say(window, 6, 2, "Type here:", 11, 0);

        window->region = WINDOW_CENTRED;
        window_damage(window, 0, ROWS);
        window_commit(window);

        for (int tick = 0; tick < 800; tick++)
        {
                struct window_key key;
                b32 changed = 0;

                while (window_key(window, &key))
                {
                        if (!(key.flags & WINDOW_KEY_DOWN) || key.character < ' ')
                                continue;

                        if (at < COLUMNS - 14)
                        {
                                struct window_cell *cell =
                                    window_cells(window) + 6 * COLUMNS + 13 + at;

                                cell->character = key.character;
                                cell->ink = 15;
                                cell->paper = 4;
                                at++;
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
