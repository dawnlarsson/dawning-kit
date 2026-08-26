/*
        Canvas -- paint

        Pixels, and nothing above them. Everything here takes a pointer, a
        pitch and a rectangle; none of it knows what a window or a screen is.
        That is what makes it worth its own file: it is the only part that can
        be read without the DRM stack in your head.
*/

static const u32 canvas_ink[INK_COUNT] = {
    [INK_DESKTOP] = 0x1b2733,
    [INK_FRAME] = 0x2f3f52,
    [INK_TITLE] = 0x2b3a4c,
    [INK_TITLE_LIT] = 0x4c6785,
    [INK_BODY] = 0x101820,
    [INK_TEXT] = 0xdfe7ef,
    [INK_CURSOR] = 0xffffff,
    [INK_CURSOR_EDGE] = 0x000000,
};

static void canvas_palette(u32 *palette, u32 format)
{
        u32 opaque = format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0;
        unsigned int i;

        for (i = 0; i < INK_COUNT; i++)
                palette[i] = canvas_ink[i] | opaque;
}


/*
        Cursors.

        One cell for every shape so a shape change is a different bitmap and
        nothing else -- same buffer, same damage, same hardware plane. X is the
        outline, . the fill, a space transparent.

        The hotspot is the pixel the pointer actually is. It is the corner for
        an arrow and the centre for the resize shapes, which is why it is per
        shape rather than one constant.
*/
#define CURSOR_W 16
#define CURSOR_H 20

#define CURSOR_ARROW 0
#define CURSOR_RESIZE_H 1
#define CURSOR_RESIZE_V 2
#define CURSOR_RESIZE_NWSE 3
#define CURSOR_RESIZE_NESW 4
#define CURSOR_SHAPES 5

static const char canvas_cursors[CURSOR_SHAPES][CURSOR_H][CURSOR_W + 1] = {
    {
    "X               ",
    "XX              ",
    "X.X             ",
    "X..X            ",
    "X...X           ",
    "X....X          ",
    "X.....X         ",
    "X......X        ",
    "X.......X       ",
    "X........X      ",
    "X.....XXXXX     ",
    "X..X..X         ",
    "X.X X..X        ",
    "XX  X..X        ",
    "X    X..X       ",
    "     X..X       ",
    "      X.X       ",
    "      XXX       ",
    "                ",
    "                ",
    },
    {
    "                ",
    "                ",
    "                ",
    "    X     X     ",
    "   XX     XX    ",
    "  X.X     X.X   ",
    " X..XXXXXXX..X  ",
    "X.............X ",
    " X..XXXXXXX..X  ",
    "  X.X     X.X   ",
    "   XX     XX    ",
    "    X     X     ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "       X        ",
    "      X.X       ",
    "     X...X      ",
    "    X.....X     ",
    "   XXXX.XXXX    ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "   XXXX.XXXX    ",
    "    X.....X     ",
    "     X...X      ",
    "      X.X       ",
    "       X        ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "                ",
    " XXXXXXX        ",
    " X....X         ",
    " X...X          ",
    " X..XX          ",
    " X.XX.X         ",
    " XX  X.X        ",
    " X    X.X    X  ",
    "       X.X  XX  ",
    "        X.XX.X  ",
    "         XX..X  ",
    "         X...X  ",
    "        X....X  ",
    "       XXXXXXX  ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "                ",
    "       XXXXXXX  ",
    "        X....X  ",
    "         X...X  ",
    "         XX..X  ",
    "        X.XX.X  ",
    "       X.X  XX  ",
    " X    X.X    X  ",
    " XX  X.X        ",
    " X.XX.X         ",
    " X..XX          ",
    " X...X          ",
    " X....X         ",
    " XXXXXXX        ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
};

static const int canvas_cursor_hot[CURSOR_SHAPES][2] = {
    {0, 0}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
};

static int cursor_hot_x(unsigned int shape)
{
        return canvas_cursor_hot[shape][0];
}

static int cursor_hot_y(unsigned int shape)
{
        return canvas_cursor_hot[shape][1];
}

/*
        The cursor, into whatever it is given. Runs of the same colour go out
        together, the same reason a glyph does: a store per pixel would be a
        call per pixel at scale one.
*/
static void canvas_draw_cursor(const struct target *t, int x, int y,
                               unsigned int shape, unsigned int scale)
{
        int row, column;
        unsigned int line;

        x -= cursor_hot_x(shape) * (int)scale;
        y -= cursor_hot_y(shape) * (int)scale;

        for (row = 0; row < CURSOR_H; row++)
        {
                for (column = 0; column < CURSOR_W;)
                {
                        char pixel = canvas_cursors[shape][row][column];
                        int run = column, x1, x2;

                        if (pixel == ' ')
                        {
                                column++;
                                continue;
                        }

                        while (run < CURSOR_W && canvas_cursors[shape][row][run] == pixel)
                                run++;

                        x1 = max(max(x + column * (int)scale, t->clip.x1), 0);
                        x2 = min(min(x + run * (int)scale, t->clip.x2), t->width);

                        for (line = 0; x2 > x1 && line < scale; line++)
                        {
                                int py = y + row * (int)scale + (int)line;

                                if (py >= max(t->clip.y1, 0) && py < min(t->clip.y2, t->height))
                                        target_row(t, py, x1, x2,
                                                   t->ink[pixel == 'X' ? INK_CURSOR_EDGE
                                                                       : INK_CURSOR]);
                        }

                        column = run;
                }
        }
}

// xrgb8888 is the source of truth; argb differs only in the alpha byte.

/*
        How far a row of a rounded rectangle is inset from its edge. Zero
        everywhere except within radius of the top and bottom, where it follows
        the circle those corners are quarters of.
*/
static int round_inset(int row, int height, int radius)
{
        int dy;

        if (radius <= 0)
                return 0;

        if (row < radius)
                dy = radius - 1 - row;
        else if (row >= height - radius)
                dy = radius - (height - row);
        else
                return 0;

        return radius - (int)int_sqrt((unsigned long)(radius * radius - dy * dy));
}

