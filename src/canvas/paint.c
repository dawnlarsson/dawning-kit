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
        canvas_row_blit(palette, canvas_ink, INK_COUNT,
                        format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0);
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

        if (!w || !h)
                return;

        /*
                Whole eight-pixel tiles share the glyph floor when the bitmap
                is entirely inside the damage. Scale two is the Retina metric
                of the same floor, so a cursor, a title and a close button do
                not go back to scanning bits into rectangles. The destination
                rectangle is what has to fit, not the source.
        */
        if ((scale == 1 || scale == 2) && !(w & 7) &&
            x >= max(t->clip.x1, 0) &&
            (long)x + (long)w * scale <= min(t->clip.x2, t->width) &&
            y >= max(t->clip.y1, 0) &&
            (long)y + (long)h * scale <= min(t->clip.y2, t->height))
        {
                target_mark((unsigned long)h * w * (unsigned long)scale *
                            (unsigned long)scale);
                canvas_runs += w / 8 - 1;
                for (column = 0; column < w; column += 8)
                {
                        u32 *at = t->pixels + (size_t)y * t->pitch + x +
                                  (int)column * scale;
                        const u8 *src = bits + column / 8;

                        if (scale == 1)
                                canvas_glyph(at, t->pitch, src, pitch, h,
                                             colour);
                        else
                                canvas_glyph2(at, t->pitch, src, pitch, h,
                                              colour);
                }
                return;
        }

        for (row = 0; row < h; row++)
        {
                const unsigned char *line_bits = bits + row * pitch;

                for (column = 0; column < w;)
                {
                        unsigned int run = column;
                        int px, x1, x2, y1, y2;

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
                        y1 = y + (int)row * scale;
                        y2 = min(min(y1 + scale, t->clip.y2), t->height);
                        y1 = max(max(y1, t->clip.y1), 0);
                        target_rectangle(t, x1, y1, x2 - x1, y2 - y1, colour);

                        column = run;
                }
        }
}


/*
        Cursors.

        One cell for every shape so a shape change is a different bitmap and
        nothing else -- same buffer, same damage, same hardware plane. Each
        word is one sixteen-pixel row, in the byte order bits_draw consumes.

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

static const u16 cursor_edge[CURSOR_SHAPES][CURSOR_H] = {
    {0x0080, 0x00c0, 0x00a0, 0x0090, 0x0088, 0x0084, 0x0082, 0x0081, 0x8080, 0x4080,
     0xe083, 0x0092, 0x00a9, 0x00c9, 0x8084, 0x8004, 0x8002, 0x8003, 0x0000, 0x0000},
    {0x0000, 0x0000, 0x0000, 0x2008, 0x3018, 0x2828, 0xe44f, 0x0280, 0xe44f, 0x2828,
     0x3018, 0x2008, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0001, 0x8002, 0x4004, 0x2008, 0xf01e, 0x8002, 0x8002, 0x8002, 0x8002, 0x8002,
     0xf01e, 0x2008, 0x4004, 0x8002, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0000, 0x007f, 0x0042, 0x0044, 0x004c, 0x005a, 0x0065, 0x8442, 0x4c01, 0xb400,
     0x6400, 0x4400, 0x8400, 0xfc01, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0000, 0xfc01, 0x8400, 0x4400, 0x6400, 0xb400, 0x4c01, 0x8442, 0x0065, 0x005a,
     0x004c, 0x0044, 0x0042, 0x007f, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
};

static const u16 cursor_fill[CURSOR_SHAPES][CURSOR_H] = {
    {0x0000, 0x0000, 0x0040, 0x0060, 0x0070, 0x0078, 0x007c, 0x007e, 0x007f, 0x807f,
     0x007c, 0x006c, 0x0046, 0x0006, 0x0003, 0x0003, 0x0001, 0x0000, 0x0000, 0x0000},
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1010, 0x1830, 0xfc7f, 0x1830, 0x1010,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0000, 0x0001, 0x8003, 0xc007, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
     0x0001, 0xc007, 0x8003, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0000, 0x0000, 0x003c, 0x0038, 0x0030, 0x0024, 0x0002, 0x0001, 0x8000, 0x4800,
     0x1800, 0x3800, 0x7800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x0000, 0x0000, 0x7800, 0x3800, 0x1800, 0x4800, 0x8000, 0x0001, 0x0002, 0x0024,
     0x0030, 0x0038, 0x003c, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
};

static const int canvas_cursor_hot[CURSOR_SHAPES][2] = {
    {0, 0}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
};

static void cursor_cell(struct drm_rect *rect, int x, int y,
                        unsigned int shape, unsigned int scale)
{
        int hx = canvas_cursor_hot[shape][0] * (int)scale;
        int hy = canvas_cursor_hot[shape][1] * (int)scale;

        drm_rect_init(rect, x - hx, y - hy, CURSOR_W * (int)scale,
                      CURSOR_H * (int)scale);
}

/*
        The cursor, into whatever it is given. Two colours, so two passes of
        the same walk a glyph takes: the outline and the fill never share a
        pixel, so which goes down first does not matter. (x, y) is the hotspot.
*/
static HOT void canvas_draw_cursor(const struct target *t, int x, int y,
                                   unsigned int shape, unsigned int scale)
{
        struct drm_rect cell;

        cursor_cell(&cell, x, y, shape, scale);
        bits_draw(t, cell.x1, cell.y1, (int)scale, (const u8 *)cursor_fill[shape],
                  2, CURSOR_W, CURSOR_H, t->ink[INK_CURSOR]);
        bits_draw(t, cell.x1, cell.y1, (int)scale, (const u8 *)cursor_edge[shape],
                  2, CURSOR_W, CURSOR_H, t->ink[INK_CURSOR_EDGE]);
}

// xrgb8888 is the source of truth; argb differs only in the alpha byte.

/*
        How far a row of a rounded rectangle is inset from its edge. Zero
        everywhere except within radius of the top and bottom, where it follows
        the circle those corners are quarters of.
*/
static CONST int round_inset(int row, int height, int radius)
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
