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

/*
        The sixteen a terminal has always had, so a cell carries an index
        rather than a colour and a program says "red" the way everything since
        1979 has.
*/
static const u32 canvas_terminal[16] = {
    0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
    0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
};

static void canvas_palette(u32 *palette, u32 format)
{
        u32 opaque = format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0;
        unsigned int i;

        for (i = 0; i < INK_COUNT; i++)
                palette[i] = canvas_ink[i] | opaque;
}


/*
        A bitmap, into whatever it is given. The set bits of a row are drawn as
        runs rather than one at a time: a call for every lit pixel is thousands
        of calls for a line of text, and a run of a few is what the fill is
        cheapest at.

        A glyph and a cursor are the same picture to this, so there is one walk
        and not two. It lives here rather than beside the glyphs because paint.c
        is included first and the cursor below is drawn from it.
*/
static void bits_draw(const struct target *t, int x, int y, int scale,
                      const unsigned char *bits, unsigned int pitch,
                      unsigned int w, unsigned int h, u32 colour)
{
        unsigned int row, column;

        /*
                The whole thing in one call, when it is entirely inside the
                damage and drawn at its own size. That is nearly every glyph;
                the rest go the long way round below.
        */
        if (scale == 1 && w == 8 && pitch == 1 &&
            x >= max(t->clip.x1, 0) && x + 8 <= min(t->clip.x2, t->width) &&
            y >= max(t->clip.y1, 0) && y + (int)h <= min(t->clip.y2, t->height))
        {
                canvas_painted += h * 8;
                canvas_runs++;
                canvas_glyph(t->pixels + (size_t)y * t->pitch + x, t->pitch,
                             bits, 1, h, colour);
                return;
        }

        for (row = 0; row < h; row++)
        {
                const unsigned char *line_bits = bits + row * pitch;

                for (column = 0; column < w;)
                {
                        unsigned int run = column;
                        int px, x1, x2, line;

                        if (!(line_bits[column / 8] & (0x80 >> (column % 8))))
                        {
                                column++;
                                continue;
                        }

                        while (run < w &&
                               (line_bits[run / 8] & (0x80 >> (run % 8))))
                                run++;

                        px = x + (int)column * scale;
                        x1 = max(max(px, t->clip.x1), 0);
                        x2 = min(min(px + (int)(run - column) * scale, t->clip.x2),
                                 t->width);

                        for (line = 0; x2 > x1 && line < scale; line++)
                        {
                                int py = y + (int)row * scale + line;

                                if (py >= max(t->clip.y1, 0) &&
                                    py < min(t->clip.y2, t->height))
                                        target_row(t, py, x1, x2, colour);
                        }

                        column = run;
                }
        }
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

/*
        The same shapes as bits, two bytes a row, built once.

        Drawing from the character art meant a test and a branch per pixel in
        C. As bits it is the same primitive a glyph uses, twice for the two
        halves of a sixteen wide row and twice again for the two colours.
*/
static u8 cursor_edge[CURSOR_SHAPES][CURSOR_H][2];
static u8 cursor_fill[CURSOR_SHAPES][CURSOR_H][2];

static void canvas_cursor_bits(void)
{
        unsigned int shape, row, column;

        for (shape = 0; shape < CURSOR_SHAPES; shape++)
                for (row = 0; row < CURSOR_H; row++)
                        for (column = 0; column < CURSOR_W; column++)
                        {
                                char pixel = canvas_cursors[shape][row][column];
                                u8 bit = 0x80 >> (column % 8);

                                if (pixel == 'X')
                                        cursor_edge[shape][row][column / 8] |= bit;
                                else if (pixel == '.')
                                        cursor_fill[shape][row][column / 8] |= bit;
                        }
}

static const int canvas_cursor_hot[CURSOR_SHAPES][2] = {
    {0, 0}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
};

/*
        The cursor, into whatever it is given. Two colours, so two passes of
        the same walk a glyph takes: the outline and the fill never share a
        pixel, so which goes down first does not matter.
*/
static void canvas_draw_cursor(const struct target *t, int x, int y,
                               unsigned int shape, unsigned int scale)
{
        x -= canvas_cursor_hot[shape][0] * (int)scale;
        y -= canvas_cursor_hot[shape][1] * (int)scale;

        // The whole cell in four calls, when it is at its own size and
        // entirely inside the damage. Two halves of a row, two colours.
        if (scale == 1 &&
            x >= max(t->clip.x1, 0) && x + CURSOR_W <= min(t->clip.x2, t->width) &&
            y >= max(t->clip.y1, 0) && y + CURSOR_H <= min(t->clip.y2, t->height))
        {
                u32 *at = t->pixels + (size_t)y * t->pitch + x;

                canvas_glyph(at, t->pitch, &cursor_fill[shape][0][0], 2,
                             CURSOR_H, t->ink[INK_CURSOR]);
                canvas_glyph(at + 8, t->pitch, &cursor_fill[shape][0][1], 2,
                             CURSOR_H, t->ink[INK_CURSOR]);
                canvas_glyph(at, t->pitch, &cursor_edge[shape][0][0], 2,
                             CURSOR_H, t->ink[INK_CURSOR_EDGE]);
                canvas_glyph(at + 8, t->pitch, &cursor_edge[shape][0][1], 2,
                             CURSOR_H, t->ink[INK_CURSOR_EDGE]);
                return;
        }

        bits_draw(t, x, y, (int)scale, &cursor_fill[shape][0][0], 2, CURSOR_W,
                  CURSOR_H, t->ink[INK_CURSOR]);
        bits_draw(t, x, y, (int)scale, &cursor_edge[shape][0][0], 2, CURSOR_W,
                  CURSOR_H, t->ink[INK_CURSOR_EDGE]);
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

