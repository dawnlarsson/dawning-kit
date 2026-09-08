#define GRID_STRIDE 256
#define GRID_HISTORY 64
#define GRID_LINES \
        (WINDOW_PIXELS + GRID_HISTORY * GRID_STRIDE * \
                             (positive)sizeof(struct window_cell))

static p8 page[GRID_LINES + GRID_HISTORY * 4];
static p8 out[TERMINAL_FIXTURE_OUTPUT];
static positive out_length;

static fn say_byte(unsigned int c)
{
        if (out_length < sizeof(out))
                out[out_length++] = (p8)c;
}

static fn tell(const char *text)
{
        while (*text)
                say_byte((p8)*text++);
}

static fn say_number(unsigned int value)
{
        if (value >= 10)
                say_number(value / 10);

        say_byte('0' + value % 10);
}

static fn say_hex(unsigned int value, unsigned int digits)
{
        while (digits--)
                say_byte("0123456789abcdef"[(value >> (digits * 4)) & 15]);
}

static unsigned int number(string_address text)
{
        unsigned int value = 0;

        while (*text >= '0' && *text <= '9')
                value = value * 10 + (unsigned int)(*text++ - '0');

        return value;
}

static bipolar next_byte(string_address text, positive *at)
{
        unsigned int c = text[*at];

        if (!c)
                return -1;

        (*at)++;

        if (c != '\\')
                return (bipolar)c;

        c = text[*at];

        if (!c)
                return '\\';

        (*at)++;

        switch (c)
        {
        case 'e': return 27;
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'b': return 8;
#ifdef TERMINAL_FIXTURE_BELL
        case 'a': return 7;
#endif
        case '0': return 0;
        case 'x':
        {
                unsigned int value = 0;

                for (int i = 0; i < 2; i++)
                {
                        unsigned int digit = text[*at];

                        if (digit >= '0' && digit <= '9')
                                value = value * 16 + digit - '0';
                        else if (digit >= 'a' && digit <= 'f')
                                value = value * 16 + digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F')
                                value = value * 16 + digit - 'A' + 10;
                        else
                                break;

                        (*at)++;
                }

                return (bipolar)value;
        }
        }

        return (bipolar)c;
}

static fn say_row(unsigned int row)
{
        struct window_cell *cells = row_cells(row);
        unsigned int length = *row_length(row);

        say_byte('[');

        for (unsigned int column = 0; column < length; column++)
        {
                unsigned int character = cells[column].character;

                if (character >= ' ' && character < 127)
                        say_byte(character);
                else
                {
                        say_byte('<');
                        say_hex(character, character > 0xfffff
                                               ? 6
                                               : character > 0xffff ? 5 : 4);
                        say_byte('>');
                }
        }

        tell("]\n");
}

static fn say_attribute(string_address text)
{
        positive at = 0;
        unsigned int row = number(text);
        unsigned int column;

        while (text[at] && text[at] != ',')
                at++;

        column = number(text + at + 1);
        say_number(row_cells(row)[column].ink);
        say_byte(',');
        say_number(row_cells(row)[column].paper);
        say_byte('\n');
}

static fn terminal_fixture_start(unsigned int columns, unsigned int rows)
{
        window = (struct window *)page;
        window->stride = GRID_STRIDE;
        window->history = GRID_HISTORY;
        window->lines = GRID_LINES;
        window->head = GRID_HISTORY + rows;
        window->columns = columns;
        window->rows = rows;
        window->max_columns = GRID_STRIDE;
        window->max_rows = GRID_HISTORY;

        COLUMNS = columns;
        ROWS = rows;
        full_reset();
}
