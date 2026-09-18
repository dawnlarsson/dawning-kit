/*
        Canvas -- compose

        Panes are in desktop coordinates and outputs are rectangles of the
        desktop, so composing one output is drawing the desktop offset by where
        that output sits. Back to front, which is the list order, which
        pane_by_z keeps in z order.

        Everything takes a clip in output coordinates. Repainting damage rather
        than a whole screen is the reason: a pane that merely overlaps the
        damage would otherwise repaint all of itself, over whatever was drawn
        above it outside that rectangle.

        A pane is drawn as bands of one rounded rectangle -- the frame, the
        titlebar, the contents -- so every band's corners follow the same
        curve, and a band that is not at a corner is a plain run of pixels.
*/

// Whether any of these desktop rectangles reaches this output. Early, because
// it is the one overlap question everything from cursor cells to damage asks.
static PURE _Bool output_touched(struct output *output, const struct drm_rect *damage,
                                 unsigned int count)
{
        struct drm_rect screen;
        unsigned int i;

        drm_rect_init(&screen, output->x, output->y, (int)output->width,
                      (int)output->height);
        for (i = 0; i < count; i++)
                if (drm_rects_overlap(&damage[i], &screen))
                        return true;

        return false;
}

struct shape
{
        int x, y, w, h;
        int radius;
};

// One run of one row, already clipped. Everything that draws ends here.
static void target_row(const struct target *t, int y, int x1, int x2, u32 colour)
{
        if ((unsigned int)y >= (unsigned int)t->height)
                return;

        x1 = max(x1, 0);
        x2 = min(x2, t->width);
        if (x2 <= x1)
                return;

        target_mark((unsigned long)(x2 - x1));
        memory_fill_u32(t->pixels + (size_t)y * t->pitch + x1,
                        (unsigned long)(x2 - x1), colour);
}

// Clipped solid rectangles share one accounting and strided-store floor.
static void target_rectangle(const struct target *t, int x, int y, int w, int h,
                              u32 colour)
{
        if (x < 0)
        {
                w += x;
                x = 0;
        }
        if (y < 0)
        {
                h += y;
                y = 0;
        }
        w = min(w, t->width - x);
        h = min(h, t->height - y);
        if (w <= 0 || h <= 0)
                return;
        target_mark((unsigned long)w * h);
        canvas_rect_fill(t->pixels + (size_t)y * t->pitch + x, t->pitch,
                         (unsigned long)w, (unsigned long)h, colour);
}

/*
        The run of one row of a shape that survives its band and the clip, or
        false when nothing does.

        This is the whole of what the rounded corners cost: an inset per row,
        and every band measured from there rather than from the edge.
*/
static _Bool shape_span(const struct target *t, const struct shape *shape,
                        int band_x, int band_w, int y, int *x1, int *x2)
{
        int inset;

        if (y < max(t->clip.y1, 0) || y >= min(t->clip.y2, t->height))
                return false;

        inset = round_inset(y - shape->y, shape->h, shape->radius);

        *x1 = max(max(shape->x + inset, band_x), t->clip.x1);
        *x2 = min(min(shape->x + shape->w - inset, band_x + band_w),
                  min(t->clip.x2, t->width));

        return *x2 > *x1;
}

/*
        A band of the shape.

        Split three ways, because only the rows inside a corner have an inset
        and only they need looking at one at a time. The straight middle is one
        rectangle and goes out as one call: a window's two sides are two pixels
        wide and a hundred and ninety rows tall, which was 4560 calls a compose
        before this, each of them to write two pixels.
*/
static _Bool shape_band_rows(const struct target *t, int band_y, int band_h,
                             int *top, int *bottom)
{
        *top = max(max(band_y, t->clip.y1), 0);
        *bottom = min(min(band_y + band_h, t->clip.y2), t->height);
        return *bottom > *top;
}

static void shape_fill_rows(const struct target *t, const struct shape *shape,
                            int band_x, int band_w, int y, int stop, u32 colour)
{
        int x1, x2;

        for (; y < stop; y++)
                if (shape_span(t, shape, band_x, band_w, y, &x1, &x2))
                        target_row(t, y, x1, x2, colour);
}

static void shape_fill(const struct target *t, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h, u32 colour)
{
        int top, bottom, x1, x2;
        int curve_top, curve_bottom;

        if (!shape_band_rows(t, band_y, band_h, &top, &bottom))
                return;

        // Clamped to the band, not just to the shape: a titlebar starts below
        // the shape's top, and a rectangle measured from the shape would paint
        // the whole window.
        curve_top = clamp(shape->y + shape->radius, top, bottom);
        curve_bottom = clamp(shape->y + shape->h - shape->radius, top, bottom);

        shape_fill_rows(t, shape, band_x, band_w, top, curve_top, colour);

        if (curve_bottom > curve_top)
        {
                x1 = max(max(shape->x, band_x), t->clip.x1);
                x2 = min(min(shape->x + shape->w, band_x + band_w),
                         min(t->clip.x2, t->width));

                target_rectangle(t, x1, curve_top, x2 - x1,
                                  curve_bottom - curve_top, colour);
        }

        shape_fill_rows(t, shape, band_x, band_w, curve_bottom, bottom, colour);
}

static void shape_blit(const struct target *t, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h,
                       const u32 *source, unsigned int source_pitch)
{
        int top, bottom, y, x1, x2;

        if (!shape_band_rows(t, band_y, band_h, &top, &bottom))
                return;

        for (y = top; y < bottom; y++)
        {
                if (!shape_span(t, shape, band_x, band_w, y, &x1, &x2))
                        continue;

                target_mark((unsigned long)(x2 - x1));
                canvas_row_blit(t->pixels + (size_t)y * t->pitch + x1,
                                source + (size_t)(y - band_y) * source_pitch +
                                    (x1 - band_x),
                                (unsigned long)(x2 - x1), t->opaque);
        }
}

#define BX_N 1u
#define BX_S 2u
#define BX_W 4u
#define BX_E 8u

static void glyph_hline(unsigned char *bits, unsigned int y, unsigned char mask)
{
        if (y < WINDOW_CELL_H)
                bits[y] |= mask;
}

static void glyph_vline(unsigned char *bits, unsigned int x0, unsigned int x1,
                        unsigned int y0, unsigned int y1)
{
        unsigned int y;
        unsigned char mask = 0;

        while (x0 <= x1 && x0 < 8)
        {
                mask |= (unsigned char)(0x80u >> x0);
                x0++;
        }

        if (y1 >= WINDOW_CELL_H)
                y1 = WINDOW_CELL_H - 1;
        for (y = y0; y <= y1; y++)
                bits[y] |= mask;
}

static void glyph_box_nsew(unsigned char *bits, unsigned int nsew, unsigned int thick)
{
        unsigned int mid_y = 7;
        unsigned int mid_x = 3;
        unsigned char hmask = 0;
        unsigned int y;

        if (nsew & BX_W)
                hmask |= (unsigned char)(0xffu << (8 - (mid_x + 1 + thick)));
        if (nsew & BX_E)
                hmask |= (unsigned char)(0xffu >> mid_x);
        if ((nsew & (BX_W | BX_E)) == (BX_W | BX_E))
                hmask = 0xff;

        for (y = 0; y < thick; y++)
                glyph_hline(bits, mid_y + y, hmask);

        if (nsew & BX_N)
                glyph_vline(bits, mid_x, mid_x + thick - 1, 0, mid_y + thick - 1);
        if (nsew & BX_S)
                glyph_vline(bits, mid_x, mid_x + thick - 1, mid_y, WINDOW_CELL_H - 1);
}

/*
        Which edges each of U+2500 to U+257F draws.

        The block is a chart and this is the chart: the code point's low seven
        bits pick the row, and a row is the edges that meet in the middle of
        the cell. It was a hundred and twenty five cases returning fifteen
        answers between them, which is a table spelled out at four lines an
        entry. Weight is not in here -- the light, heavy, dashed and double
        spellings of a join all draw the same edges, and glyph_synthesize asks
        the code point itself how thick to draw them. U+2571 to U+2573 are the
        diagonals, which no combination of edges describes, so they answer
        nothing and fall through to the face.
*/
static const unsigned char box_nsew[0x80] = {
    [0x00 ... 0x01]    = BX_W | BX_E,
    [0x02 ... 0x03]    = BX_N | BX_S,
    [0x04 ... 0x05]    = BX_W | BX_E,
    [0x06 ... 0x07]    = BX_N | BX_S,
    [0x08 ... 0x09]    = BX_W | BX_E,
    [0x0a ... 0x0b]    = BX_N | BX_S,
    [0x0c ... 0x0f]    = BX_S | BX_E,
    [0x10 ... 0x13]    = BX_S | BX_W,
    [0x14 ... 0x17]    = BX_N | BX_E,
    [0x18 ... 0x1b]    = BX_N | BX_W,
    [0x1c ... 0x23]    = BX_N | BX_S | BX_E,
    [0x24 ... 0x2b]    = BX_N | BX_S | BX_W,
    [0x2c ... 0x33]    = BX_S | BX_W | BX_E,
    [0x34 ... 0x3b]    = BX_N | BX_W | BX_E,
    [0x3c ... 0x4b]    = BX_N | BX_S | BX_W | BX_E,
    [0x4c ... 0x4d]    = BX_W | BX_E,
    [0x4e ... 0x4f]    = BX_N | BX_S,
    [0x50]             = BX_W | BX_E,
    [0x51]             = BX_N | BX_S,
    [0x52 ... 0x54]    = BX_S | BX_E,
    [0x55 ... 0x57]    = BX_S | BX_W,
    [0x58 ... 0x5a]    = BX_N | BX_E,
    [0x5b ... 0x5d]    = BX_N | BX_W,
    [0x5e ... 0x60]    = BX_N | BX_S | BX_E,
    [0x61 ... 0x63]    = BX_N | BX_S | BX_W,
    [0x64 ... 0x66]    = BX_S | BX_W | BX_E,
    [0x67 ... 0x69]    = BX_N | BX_W | BX_E,
    [0x6a ... 0x6c]    = BX_N | BX_S | BX_W | BX_E,
    [0x6d]             = BX_S | BX_E,
    [0x6e]             = BX_S | BX_W,
    [0x6f]             = BX_N | BX_W,
    [0x70]             = BX_N | BX_E,
    [0x74]             = BX_W,
    [0x75]             = BX_N,
    [0x76]             = BX_E,
    [0x77]             = BX_S,
    [0x78]             = BX_W,
    [0x79]             = BX_N,
    [0x7a]             = BX_E,
    [0x7b]             = BX_S,
    [0x7c]             = BX_W | BX_E,
    [0x7d]             = BX_N | BX_S,
    [0x7e]             = BX_W | BX_E,
    [0x7f]             = BX_N | BX_S,
};

static unsigned int glyph_box_nsew_from(unsigned int c)
{
        return c - 0x2500 < sizeof(box_nsew) ? box_nsew[c - 0x2500] : 0;
}

/*
        The block elements, each of which is one rectangle of the cell filled.

        A run of rows and a column mask is the whole of what any of them is,
        so they are a table of the two rather than a test apiece: 2581 to 2587
        are the eighths growing up from the floor, 2589 to 258f the eighths
        growing in from the right edge -- which is where 258c, the left half,
        already came from -- and the quadrants and the one eighth bars are
        each a rectangle of their own. The shades are the exception, being a
        pattern per row rather than a fill, and anything with no rectangle
        here is the full block: that is what the quadrant pairs 2599 to 259c,
        259e and 259f have always been drawn as.
*/
static void glyph_block(unsigned int c, unsigned char *bits)
{
        static const struct { unsigned char from, rows, mask; } filled[0x20] = {
            [0x00] = {0, 8, 0xff},   [0x01] = {14, 2, 0xff},
            [0x02] = {12, 4, 0xff},  [0x03] = {10, 6, 0xff},
            [0x04] = {8, 8, 0xff},   [0x05] = {6, 10, 0xff},
            [0x06] = {4, 12, 0xff},  [0x07] = {2, 14, 0xff},
            [0x08] = {0, 16, 0xff},  [0x09] = {0, 16, 0xfe},
            [0x0a] = {0, 16, 0xfc},  [0x0b] = {0, 16, 0xf8},
            [0x0c] = {0, 16, 0xf0},  [0x0d] = {0, 16, 0xe0},
            [0x0e] = {0, 16, 0xc0},  [0x0f] = {0, 16, 0x80},
            [0x10] = {0, 16, 0x0f},  [0x14] = {0, 2, 0xff},
            [0x15] = {0, 16, 0x01},  [0x16] = {8, 8, 0xf0},
            [0x17] = {8, 8, 0x0f},   [0x18] = {0, 8, 0xf0},
            [0x1d] = {0, 8, 0x0f},
        };
        unsigned int at = c - 0x2580;
        unsigned int y;

        if (c >= 0x2591 && c <= 0x2593)
        {
                unsigned char shade = c == 0x2591 ? 0x44 : c == 0x2592 ? 0xaa : 0xee;

                for (y = 0; y < WINDOW_CELL_H; y++)
                        bits[y] = (unsigned char)(y & 1 ? shade >> 1 : shade);
                return;
        }

        if (at < sizeof(filled) / sizeof(filled[0]) && filled[at].rows)
                memory_fill(bits + filled[at].from, filled[at].mask,
                            filled[at].rows);
        else
                memory_fill(bits, 0xff, WINDOW_CELL_H);
}

static void glyph_braille(unsigned int dots, unsigned char *bits)
{
        static const unsigned char ox[8] = {1, 1, 1, 4, 4, 4, 1, 4};
        static const unsigned char oy[8] = {1, 5, 9, 1, 5, 9, 13, 13};
        unsigned int i, x, y;

        for (i = 0; i < 8; i++)
        {
                if (!(dots & (1u << i)))
                        continue;
                for (y = 0; y < 3; y++)
                        for (x = 0; x < 2; x++)
                                bits[oy[i] + y] |=
                                    (unsigned char)(0x80u >> (ox[i] + x));
        }
}

static void glyph_tofu(unsigned char *bits)
{
        unsigned int y;

        bits[1] = 0x7e;
        bits[WINDOW_CELL_H - 2] = 0x7e;
        for (y = 2; y < WINDOW_CELL_H - 2; y++)
                bits[y] = 0x42;
}

// One half of a box two cells wide, for a double-width character the face
// has no glyph for.
static void glyph_tofu_half(unsigned char *bits, _Bool right)
{
        unsigned int y;

        memory_fill(bits, 0, WINDOW_CELL_H);
        bits[1] = right ? 0xfe : 0x7f;
        bits[WINDOW_CELL_H - 2] = bits[1];
        for (y = 2; y < WINDOW_CELL_H - 2; y++)
                bits[y] = right ? 0x02 : 0x40;
}

/*
        What the VGA face already draws.

        The cell font is the IBM VGA ROM face, 256 glyphs in code page 437
        order. Past ASCII its slots hold accented Latin, Greek, arrows, card
        suits and maths, so a character with one of those shapes is drawn from
        the font rather than as a question mark. Box drawing and the blocks are
        not here: glyph_synthesize draws those to the edges of the cell so that
        neighbours join. Sorted by character for the search.
*/
static const struct
{
        unsigned short character;
        unsigned char glyph;
} glyph_face[] = {
    {0x00a0, 0xff}, {0x00a1, 0xad}, {0x00a2, 0x9b}, {0x00a3, 0x9c},
    {0x00a5, 0x9d}, {0x00a7, 0x15}, {0x00aa, 0xa6}, {0x00ab, 0xae},
    {0x00ac, 0xaa}, {0x00b0, 0xf8}, {0x00b1, 0xf1}, {0x00b5, 0xe6},
    {0x00b6, 0x14}, {0x00b7, 0xfa}, {0x00ba, 0xa7}, {0x00bb, 0xaf},
    {0x00bc, 0xac}, {0x00bd, 0xab}, {0x00bf, 0xa8}, {0x00c4, 0x8e},
    {0x00c5, 0x8f}, {0x00c6, 0x92}, {0x00c7, 0x80}, {0x00c9, 0x90},
    {0x00d1, 0xa5}, {0x00d6, 0x99}, {0x00dc, 0x9a}, {0x00df, 0xe1},
    {0x00e0, 0x85}, {0x00e1, 0xa0}, {0x00e2, 0x83}, {0x00e4, 0x84},
    {0x00e5, 0x86}, {0x00e6, 0x91}, {0x00e7, 0x87}, {0x00e8, 0x8a},
    {0x00e9, 0x82}, {0x00ea, 0x88}, {0x00eb, 0x89}, {0x00ec, 0x8d},
    {0x00ed, 0xa1}, {0x00ee, 0x8c}, {0x00ef, 0x8b}, {0x00f1, 0xa4},
    {0x00f2, 0x95}, {0x00f3, 0xa2}, {0x00f4, 0x93}, {0x00f6, 0x94},
    {0x00f7, 0xf6}, {0x00f9, 0x97}, {0x00fa, 0xa3}, {0x00fb, 0x96},
    {0x00fc, 0x81}, {0x00ff, 0x98}, {0x0192, 0x9f}, {0x0393, 0xe2},
    {0x0398, 0xe9}, {0x03a3, 0xe4}, {0x03a6, 0xe8}, {0x03a9, 0xea},
    {0x03b1, 0xe0}, {0x03b4, 0xeb}, {0x03b5, 0xee}, {0x03c0, 0xe3},
    {0x03c3, 0xe5}, {0x03c4, 0xe7}, {0x03c6, 0xed}, {0x2022, 0x07},
    {0x203c, 0x13}, {0x207f, 0xfc}, {0x20a7, 0x9e}, {0x2190, 0x1b},
    {0x2191, 0x18}, {0x2192, 0x1a}, {0x2193, 0x19}, {0x2194, 0x1d},
    {0x2195, 0x12}, {0x21a8, 0x17}, {0x2219, 0xf9}, {0x221a, 0xfb},
    {0x221e, 0xec}, {0x221f, 0x1c}, {0x2229, 0xef}, {0x2248, 0xf7},
    {0x2261, 0xf0}, {0x2264, 0xf3}, {0x2265, 0xf2}, {0x2302, 0x7f},
    {0x2310, 0xa9}, {0x2320, 0xf4}, {0x2321, 0xf5}, {0x25a0, 0xfe},
    {0x25ac, 0x16}, {0x25b2, 0x1e}, {0x25ba, 0x10}, {0x25bc, 0x1f},
    {0x25c4, 0x11}, {0x25cb, 0x09}, {0x25d8, 0x08}, {0x25d9, 0x0a},
    {0x263a, 0x01}, {0x263b, 0x02}, {0x263c, 0x0f}, {0x2640, 0x0c},
    {0x2642, 0x0b}, {0x2660, 0x06}, {0x2663, 0x05}, {0x2665, 0x03},
    {0x2666, 0x04}, {0x266a, 0x0d}, {0x266b, 0x0e},
};

// The font's slot for a character past ASCII, or 0 when it has none.
static unsigned int glyph_in_face(unsigned int c)
{
        unsigned int low = 0, high = sizeof(glyph_face) / sizeof(glyph_face[0]);

        while (low < high)
        {
                unsigned int middle = (low + high) / 2;

                if (glyph_face[middle].character < c)
                        low = middle + 1;
                else
                        high = middle;
        }

        return low < sizeof(glyph_face) / sizeof(glyph_face[0]) &&
                       glyph_face[low].character == c
                   ? glyph_face[low].glyph
                   : 0;
}

/*
        A digit raised or lowered: the face's own digit at half size, each
        pair of rows and of columns folded into one, in the top or the bottom
        half of the cell. btop names every box with a superscript digit.
*/
static __attribute__((__cold__)) _Bool
glyph_script_digit(unsigned int c, const unsigned char *face,
                   size_t glyph_size, unsigned char *bits)
{
        static const unsigned short raised[10] = {
            0x2070, 0x00b9, 0x00b2, 0x00b3, 0x2074,
            0x2075, 0x2076, 0x2077, 0x2078, 0x2079};
        const unsigned char *from;
        unsigned int digit = 0, top = 0, y, x;

        if (c >= 0x2080 && c <= 0x2089)
        {
                digit = c - 0x2080;
                top = WINDOW_CELL_H / 2;
        }
        else
        {
                while (digit < 10 && raised[digit] != c)
                        digit++;
                if (digit == 10)
                        return false;
        }

        from = face + (size_t)('0' + digit) * glyph_size;
        memory_fill(bits, 0, WINDOW_CELL_H);

        for (y = 0; y < WINDOW_CELL_H / 2; y++)
        {
                unsigned char pair = (unsigned char)(from[2 * y] | from[2 * y + 1]);
                unsigned char half = 0;

                for (x = 0; x < WINDOW_CELL_W / 2; x++)
                        if (pair & (0xc0 >> (2 * x)))
                                half |= (unsigned char)(0x20 >> x);

                bits[top + y] = half;
        }

        return true;
}

static __attribute__((__cold__)) _Bool
glyph_synthesize(unsigned int c, unsigned char *bits)
{
        unsigned int nsew;

        memory_fill(bits, 0, WINDOW_CELL_H);

        if (c >= 0x2800 && c <= 0x28ff)
        {
                glyph_braille(c - 0x2800, bits);
                return true;
        }

        if (c >= 0x2580 && c <= 0x259f)
        {
                glyph_block(c, bits);
                return true;
        }

        if (c >= 0x2500 && c <= 0x257f)
        {
                nsew = glyph_box_nsew_from(c);
                if (!nsew)
                {
                        // The two diagonals, which are no arrangement of
                        // edges: one pixel a row, stepping across the cell in
                        // whichever direction the code point leans.
                        if (c == 0x2571 || c == 0x2572)
                        {
                                for (unsigned int y = 0; y < 8; y++)
                                        bits[2 * y] = (unsigned char)(
                                            c == 0x2571 ? 1u << y : 0x80u >> y);
                                return true;
                        }
                        glyph_tofu(bits);
                        return true;
                }
                glyph_box_nsew(bits, nsew, (c == 0x2501 || c == 0x2503 ||
                                            (c >= 0x2550 && c <= 0x256c))
                                               ? 2
                                               : 1);
                return true;
        }

        if (c == 0x25e6)
        {
                bits[7] = 0x18;
                bits[8] = 0x18;
                return true;
        }

        if (c == 0x2260)
        {
                bits[3] = 0x02;
                bits[4] = 0x7e;
                bits[5] = 0x04;
                bits[6] = 0x08;
                bits[7] = 0x7e;
                bits[8] = 0x10;
                bits[9] = 0x20;
                return true;
        }

        if (c == 0x25ae || c == 0x25fc || c == 0x25fe)
        {
                memory_fill(bits + 3, 0x7e, 10);
                return true;
        }

        if (c == 0x25c6)
        {
                bits[3] = 0x18;
                bits[4] = 0x3c;
                bits[5] = 0x7e;
                bits[6] = 0xff;
                bits[7] = 0xff;
                bits[8] = 0x7e;
                bits[9] = 0x3c;
                bits[10] = 0x18;
                return true;
        }

        // The four horizontal scan lines a VT100 drew, one row each and the
        // rows they sat on.
        if (c >= 0x23ba && c <= 0x23bd)
        {
                static const unsigned char scan[4] = {0, 5, 10, 15};

                bits[scan[c - 0x23ba]] = 0xff;
                return true;
        }

        if (c == 0xfffd)
        {
                glyph_tofu(bits);
                return true;
        }

        return false;
}

static void glyph_apply_style(unsigned char *bits, unsigned short flags)
{
        unsigned int y;

        if (flags & WINDOW_CELL_ITALIC)
                for (y = 0; y < 8; y++)
                        bits[y] = (unsigned char)(bits[y] >> 1);

        if (flags & WINDOW_CELL_BOLD)
                for (y = 0; y < WINDOW_CELL_H; y++)
                        bits[y] |= (unsigned char)(bits[y] >> 1);

        if (flags & WINDOW_CELL_STRIKE)
                bits[7] |= 0xff;

        if (flags & WINDOW_CELL_UNDERLINE)
        {
                bits[WINDOW_CELL_H - 2] |= 0xff;
                bits[WINDOW_CELL_H - 1] |= 0xff;
        }

        if (flags & WINDOW_CELL_BAR)
                for (y = 0; y < WINDOW_CELL_H; y++)
                        bits[y] |= 0x80;
}

static u32 cell_palette(unsigned char index, u32 opaque)
{
        return canvas_terminal[index] | opaque;
}

static u32 cell_ink_colour(const struct window_cell *cell, u32 opaque)
{
        u32 ink = cell_palette(cell->ink, opaque);
        u32 paper;

        if (!(cell->flags & WINDOW_CELL_DIM))
                return ink;

        paper = cell_palette(cell->paper, opaque);
        return ((ink & 0xfefefe) >> 1) + ((paper & 0xfefefe) >> 1);
}

/*
        One cell, background and glyph together.

        The whole point is the single pass: filling the paper and then drawing
        the letter over it writes most of the cell twice, and the display is
        reading the buffer while that happens. A cell at the edge of the damage
        or inside a rounded corner still goes the long way round, where a
        double write is worth more than the case is worth handling.
*/
static void cell_draw(const struct target *t, const struct shape *shape,
                      int x, int y, const unsigned char *bits, _Bool direct,
                      u32 ink, u32 paper)
{
        if (direct && bits && x >= max(t->clip.x1, 0) &&
            x + canvas_cell_w <= min(t->clip.x2, t->width))
        {
                target_mark((unsigned long)canvas_cell_w *
                            (unsigned long)canvas_cell_h);
                if (desktop.scale == 1)
                        canvas_cell(t->pixels + (size_t)y * t->pitch + x,
                                    t->pitch, bits, WINDOW_CELL_H, ink, paper);
                else
                        canvas_cell2(t->pixels + (size_t)y * t->pitch + x,
                                     t->pitch, bits, WINDOW_CELL_H, ink, paper);
                return;
        }

        shape_fill(t, shape, x, y, canvas_cell_w, canvas_cell_h, paper);
        if (bits)
                bits_draw(t, x, y, (int)desktop.scale, bits, 1, WINDOW_CELL_W,
                          WINDOW_CELL_H, ink);
}

/*
        One row of a window made of text.

        A cell with a letter in it is drawn whole, one pixel one store. Runs of
        blank cells sharing a background go out as one rectangle, since a
        terminal is mostly empty and a rectangle is what the fill is fastest
        at.
*/
static HOT void compose_row(const struct target *t, const struct shape *shape,
                            int x, int y, const struct window_cell *cells,
                            int used, int first, int last)
{
        int column = first;
        int cell_w = canvas_cell_w;
        const unsigned char *font_data = NULL;
        size_t glyph_size = 0;
        _Bool direct = glyph_is_cell() &&
                       (desktop.scale == 1 || desktop.scale == 2) &&
                       y >= max(t->clip.y1, 0) &&
                       y + canvas_cell_h <= min(t->clip.y2, t->height) &&
                       !round_inset(y - shape->y, shape->h, shape->radius) &&
                       !round_inset(y + canvas_cell_h - 1 - shape->y,
                                    shape->h, shape->radius);

        if (glyph_is_cell() && canvas_font)
        {
                font_data = font_data_buf(canvas_font->data);
                glyph_size = font_glyph_size(canvas_font->width,
                                             canvas_font->height);
        }

        while (column < used)
        {
                unsigned int character = cells[column].character;
                unsigned short flags = cells[column].flags;
                u32 paper = cell_palette(cells[column].paper, t->opaque);
                u32 ink;
                unsigned char made[WINDOW_CELL_H];
                const unsigned char *bits;
                _Bool styled;
                int run;

                styled = (flags & (WINDOW_CELL_BOLD | WINDOW_CELL_ITALIC |
                                   WINDOW_CELL_UNDERLINE | WINDOW_CELL_STRIKE |
                                   WINDOW_CELL_HIDDEN | WINDOW_CELL_DIM |
                                   WINDOW_CELL_BAR)) != 0;

                if (character <= ' ' && !styled)
                {
                        for (run = column + 1; run < used; run++)
                        {
                                if (cells[run].character > ' ' ||
                                    cells[run].flags ||
                                    cell_palette(cells[run].paper, t->opaque) !=
                                        paper)
                                        break;
                        }

                        shape_fill(t, shape, x + column * cell_w, y,
                                   (run - column) * cell_w, canvas_cell_h,
                                   paper);
                        column = run;
                        continue;
                }

                bits = NULL;
                if (!(flags & WINDOW_CELL_HIDDEN) && character > ' ')
                {
                        unsigned int glyph = character <= 126
                                                 ? character
                                                 : glyph_in_face(character);

                        if (glyph && font_data && !styled)
                                bits = font_data + (size_t)glyph * glyph_size;
                        else
                        {
                                if (glyph && font_data)
                                        memory_copy(made,
                                                    font_data +
                                                        (size_t)glyph *
                                                            glyph_size,
                                                    WINDOW_CELL_H);
                                else if (flags & (WINDOW_CELL_WIDE |
                                                  WINDOW_CELL_WIDE_RIGHT))
                                        glyph_tofu_half(
                                            made,
                                            (flags & WINDOW_CELL_WIDE_RIGHT) != 0);
                                else if (!glyph_synthesize(character, made) &&
                                         !(font_data &&
                                           glyph_script_digit(character,
                                                              font_data,
                                                              glyph_size,
                                                              made)))
                                {
                                        if (font_data)
                                                memory_copy(made,
                                                            font_data +
                                                                (size_t)'?' *
                                                                    glyph_size,
                                                            WINDOW_CELL_H);
                                        else
                                                glyph_tofu(made);
                                }
                                glyph_apply_style(made, flags);
                                bits = made;
                        }
                }
                else if (flags & (WINDOW_CELL_UNDERLINE | WINDOW_CELL_STRIKE |
                                  WINDOW_CELL_BAR))
                {
                        memory_fill(made, 0, WINDOW_CELL_H);
                        glyph_apply_style(made, flags);
                        bits = made;
                }

                ink = cell_ink_colour(&cells[column], t->opaque);
                cell_draw(t, shape, x + column * cell_w, y, bits, direct, ink,
                          paper);
                column++;
        }

        if (column < last)
                shape_fill(t, shape, x + column * cell_w, y,
                           (last - column) * cell_w, canvas_cell_h,
                           cell_palette(0, t->opaque));
}

/*
        A window made of text.

        The rows are a window onto a ring of lines rather than the whole of a
        grid, so what is drawn is wherever the view is sitting -- the end of
        the ring while nothing has touched the wheel, and a line written long
        ago once something has.

        A line is folded into as many rows as it needs at the width the window
        is now, which is what makes a window widened re-wrap everything already
        in it. Only the rows the damage reaches are drawn; the walk down to
        them is a few additions a line and costs nothing beside a fill.
*/
static void compose_cells(struct pane *pane, const struct target *t,
                          const struct shape *shape, int x, int y)
{
        /*
                Both the width whoever owns the cells laid its lines out at and
                the room the window has now. They are the same at rest and not
                during a resize: a window that has shrunk still has the wider
                lines until whoever writes them catches up, and drawing all of
                one puts the inside of the window on the desktop beside it.
        */
        unsigned int width = max(pane->grid_columns, 1u);
        int columns = (int)min(width, pane->columns);
        int rows = (int)pane_rows(pane);

        int first_row = max((t->clip.y1 - y) / canvas_cell_h, 0);
        int last_row = min((t->clip.y2 - y + canvas_cell_h - 1) / canvas_cell_h, rows);

        // Columns as well as rows. Clipping only the rows meant a cursor
        // moving over a terminal repainted two whole lines of it, eighty
        // cells wide, to put sixteen pixels somewhere.
        int first = max((t->clip.x1 - x) / canvas_cell_w, 0);
        int last = min((t->clip.x2 - x + canvas_cell_w - 1) / canvas_cell_w, columns);
        unsigned int skip;
        unsigned int line;
        int row = 0;

        /*
                Frame and title damage reaches compose_pane too.  With no
                content cell inside the clip, walking the live view and every
                folded row can only arrive at compose_row calls that reject
                themselves.  Stop before any ring arithmetic instead.
        */
        if (first_row >= last_row || first >= last)
                return;

        canvas_terminal_prepare();

        line = pane_view_at(pane, pane->view, &skip);

        while (row < last_row && line != pane->head)
        {
                unsigned int slot = line % pane->history;
                unsigned int length = min(pane->lengths[slot], pane->stride);
                unsigned int folds = length ? (length + width - 1) / width : 1;
                const struct window_cell *cells =
                    pane->cells + (size_t)slot * pane->stride;
                unsigned int fold = skip;

                if (row < first_row && folds > skip)
                {
                        unsigned int omitted = min((unsigned int)(first_row - row),
                                                   folds - skip);
                        fold += omitted;
                        row += omitted;
                }
                for (; fold < folds && row < last_row; fold++, row++)
                {
                        unsigned int from = fold * width;
                        int used = (int)min(length > from ? length - from : 0, width);

                        compose_row(t, shape, x, y + row * canvas_cell_h,
                                    cells + from,
                                    min(used, last), first, last);
                }

                skip = 0;
                line++;
        }

        // Below the newest line, for a window with more room in it than there
        // is anything to put there.
        for (row = max(row, first_row); row < last_row; row++)
                compose_row(t, shape, x, y + row * canvas_cell_h, NULL,
                            0, first, last);
}

/*
        The bar down the right of a window that has more than it is showing.

        The grid ends before its reserved gutter, so every cell stays visible.
        The gutter remains when everything fits; acquiring scrollback must not
        change the terminal's columns.
*/
struct pane_bar_geometry
{
        int x, y, width, height;
        int thumb_at, thumb_span;
        unsigned int total;
};

/*
        The bar, and the thumb in it, in the desktop's own coordinates.

        Drawn here and taken hold of in drag.c, worked out in one place so the
        thumb a hand grabs is the thumb that was drawn. Answers false for a
        window with nothing to scroll, which is also the answer to whether
        there is anything there to press.
*/
static _Bool pane_bar(struct pane *pane, struct pane_bar_geometry *bar)
{
        unsigned int first, shown, total;
        struct drm_rect gutter;

        if (!pane_extent(pane, &first, &shown, &total) || !total)
                return false;

        pane_gutter_rect(pane, &gutter);
        bar->x = gutter.x1;
        bar->y = gutter.y1;
        bar->width = drm_rect_width(&gutter);
        bar->height = drm_rect_height(&gutter);
        bar->total = total;

        bar->thumb_span = max((int)((unsigned long)bar->height * shown / total),
                              canvas_cell_h);
        bar->thumb_at = (int)((unsigned long)bar->height * first / total);

        if (bar->thumb_at + bar->thumb_span > bar->height)
                bar->thumb_at = bar->height - bar->thumb_span;

        return true;
}

static void compose_bar(struct pane *pane, const struct target *t,
                        const struct shape *shape)
{
        struct pane_bar_geometry bar;
        struct drm_rect gutter;

        pane_gutter_rect(pane, &gutter);
        drm_rect_translate(&gutter, -t->x, -t->y);
        if (!drm_rects_overlap(&gutter, &t->clip))
                return;

        if (!pane_bar(pane, &bar))
                return;

        shape_fill(t, shape, bar.x - t->x, bar.y - t->y,
                   bar.width, bar.height, t->ink[INK_FRAME]);
        shape_fill(t, shape, bar.x - t->x, bar.y - t->y + bar.thumb_at,
                   bar.width, bar.thumb_span,
                   t->ink[INK_TITLE_LIT]);
}

static void compose_pane(struct pane *pane, const struct target *t)
{
        int title = pane_title(pane);
        int x = pane->x - t->x;
        int y = pane->y - t->y;
        int bottom = y + title + pane->height;
        struct shape shape;
        struct drm_rect frame, local_frame;
        int cx, cy, side, reserved;
        _Bool has_close;

        if (pane->style & WINDOW_MINIMIZED)
                return;

        pane_frame(pane, &frame);
        local_frame = frame;
        drm_rect_translate(&local_frame, -t->x, -t->y);

        /*
                Nothing at all for a window the damage does not touch, and for
                a cursor move that is every window but one. Without this every
                pane laid out its own text on every mouse move, whether or not
                a pixel of it could land.
        */
        if (!drm_rects_overlap(&local_frame, &t->clip))
                return;

        shape.x = local_frame.x1;
        shape.y = local_frame.y1;
        shape.w = drm_rect_width(&local_frame);
        shape.h = drm_rect_height(&local_frame);
        shape.radius = min(pane->edge, min(shape.w, shape.h) / 2);

        if (title)
        {
                u32 frame_ink = t->ink[INK_FRAME];
                int span = title + pane->height;
                struct {
                        int x, y, w, h;
                } chrome[] = {
                        {shape.x, shape.y, shape.w, y - shape.y},
                        {shape.x, bottom, shape.w, shape.y + shape.h - bottom},
                        {shape.x, y, x - shape.x, span},
                        {x + pane->width, y, shape.x + shape.w - (x + pane->width),
                         span},
                };
                unsigned int i;

                for (i = 0; i < ARRAY_SIZE(chrome); i++)
                        shape_fill(t, &shape, chrome[i].x, chrome[i].y,
                                   chrome[i].w, chrome[i].h, frame_ink);

                shape_fill(t, &shape, x, y, pane->width, title,
                           t->ink[pane->state & WINDOW_FOCUSED ? INK_TITLE_LIT
                                                              : INK_TITLE]);

                reserved = canvas_cell_w;
                has_close = pane_close_box(pane, &cx, &cy, &side);
                if (has_close)
                        reserved = side + canvas_border * 2;

                if (pane->title_length)
                        text_draw(t, x + canvas_cell_w, y,
                                  pane->width - canvas_cell_w - reserved, title,
                                  pane->title, pane->title_length,
                                  TEXT_CENTRE | TEXT_MIDDLE, (int)desktop.scale,
                                  t->ink[INK_TEXT]);

                if (has_close)
                        bits_draw(t, cx - t->x + (side - canvas_cell_w) / 2,
                                  cy - t->y + (side - canvas_cell_w) / 2,
                                  (int)desktop.scale, close_bits, 1, 8, 8,
                                  t->ink[INK_TEXT]);
        }

        if (pane->cells)
        {
                int gw = (int)min(pane->grid_columns, pane->columns) * canvas_cell_w;
                int gh = (int)min(pane->grid_rows, pane->rows) * canvas_cell_h;

                compose_cells(pane, t, &shape, x, y + title);

                // What the window has grown into but the program has not laid
                // out yet, which would otherwise show the desktop through it.
                if (gw < pane->width)
                        shape_fill(t, &shape, x + gw, y + title,
                                   pane->width - gw, pane->height, t->ink[INK_BODY]);

                if (gh < pane->height)
                        shape_fill(t, &shape, x, y + title + gh, min(gw, pane->width),
                                   pane->height - gh, t->ink[INK_BODY]);

                compose_bar(pane, t, &shape);
        }
        else if (pane->pixels)
                shape_blit(t, &shape, x, y + title, pane->width, pane->height,
                           pane->pixels, pane->pitch);
        else
                shape_fill(t, &shape, x, y + title, pane->width, pane->height,
                           t->ink[INK_BODY]);
}

/*
        The desktop, everywhere a window is not.

        There is one buffer and the display is scanning it, so a pixel written
        twice is a pixel seen twice: painting the background and then a window
        over it is a flash of the desktop through that window, and during a
        resize that is its whole body, sixty times a second. So the windows are
        cut out of the rectangle first and only what is left is painted.

        Windows are cut inset by their corner radius, which is the one place a
        window does not cover its own rectangle.
*/
#define DESKTOP_PIECES 8

static unsigned int rect_subtract(struct drm_rect *out, const struct drm_rect *a,
                                  const struct drm_rect *b)
{
        unsigned int n = 0;

        // An empty cut takes nothing away, and going the long way round for it
        // returns the whole of a as four pieces that then cost four slots.
        if (b->x2 <= b->x1 || b->y2 <= b->y1 ||
            b->x1 >= a->x2 || b->x2 <= a->x1 || b->y1 >= a->y2 || b->y2 <= a->y1)
        {
                out[0] = *a;
                return 1;
        }

        if (b->y1 > a->y1)
                drm_rect_init(&out[n++], a->x1, a->y1, a->x2 - a->x1, b->y1 - a->y1);

        if (b->y2 < a->y2)
                drm_rect_init(&out[n++], a->x1, b->y2, a->x2 - a->x1, a->y2 - b->y2);

        {
                int top = max(a->y1, b->y1);
                int bottom = min(a->y2, b->y2);

                if (b->x1 > a->x1)
                        drm_rect_init(&out[n++], a->x1, top, b->x1 - a->x1, bottom - top);

                if (b->x2 < a->x2)
                        drm_rect_init(&out[n++], b->x2, top, a->x2 - b->x2, bottom - top);
        }

        return n;
}

static void desktop_fill(const struct target *t, int x1, int y1, int x2, int y2)
{
        struct drm_rect storage[2][DESKTOP_PIECES];
        struct drm_rect *piece = storage[0], *spare = storage[1], *swap;
        unsigned int count = 1, i;
        struct pane *pane;

        drm_rect_init(&piece[0], x1, y1, x2 - x1, y2 - y1);

        list_for_each_entry_reverse(pane, &desktop.windows, link)
        {
                unsigned int kept = 0;
                struct drm_rect cut;
                struct drm_rect frame;
                int radius;

                if (!count)
                        return;

                if (pane->style & WINDOW_MINIMIZED)
                        continue;

                pane_frame(pane, &frame);
                radius = min(pane->edge, min(drm_rect_width(&frame),
                                             drm_rect_height(&frame)) / 2);
                drm_rect_init(&cut, frame.x1 + radius - t->x,
                         frame.y1 + radius - t->y,
                         drm_rect_width(&frame) - radius * 2,
                         drm_rect_height(&frame) - radius * 2);

                for (i = 0; i < count; i++)
                {
                        struct drm_rect part[4];
                        unsigned int n = rect_subtract(part, &piece[i], &cut);

                        /*
                                Past the array the pieces cost more than the
                                paint, so a split that would not leave room for
                                the pieces still to come is dropped and that
                                piece kept whole. What it counts is what the
                                cut actually made, not the four a cut can make
                                at worst: on the worst case a window several
                                windows down was never cut out at all, and the
                                desktop under it is a flash of the background
                                through it every compose.
                        */
                        if (kept + n + (count - i - 1) > DESKTOP_PIECES)
                        {
                                spare[kept++] = piece[i];
                                continue;
                        }

                        memory_copy_apart(&spare[kept], part, n * sizeof(*part));
                        kept += n;
                }

                swap = piece;
                piece = spare;
                spare = swap;
                count = kept;
        }

        for (i = 0; i < count; i++)
                target_rectangle(t, piece[i].x1, piece[i].y1,
                                  piece[i].x2 - piece[i].x1,
                                  piece[i].y2 - piece[i].y1, t->ink[INK_DESKTOP]);
}

static HOT void compose_clip(const struct target *t)
{
        int x1 = max(t->clip.x1, 0);
        int y1 = max(t->clip.y1, 0);
        int x2 = min(t->clip.x2, t->width);
        int y2 = min(t->clip.y2, t->height);
        struct pane *pane;

        if (x2 > x1 && y2 > y1)
                desktop_fill(t, x1, y1, x2, y2);

        list_for_each_entry(pane, &desktop.windows, link)
                compose_pane(pane, t);
}

// Somewhere to draw: an output, a pointer into its scanout buffer, and the
// damage. The clip is in target coordinates; the rectangle asked for is in
// desktop ones.
static PURE struct target target_of(struct output *output, u32 *pixels,
                                    const struct drm_rect *r)
{
        struct target t = {
                .pixels = pixels,
                .pitch = output->buffer->fb->pitches[0] / sizeof(u32),
                .width = (int)output->width,
                .height = (int)output->height,
                .x = output->x,
                .y = output->y,
                .opaque = output->opaque,
                .ink = output->palette,
                .clip = {
                        .x1 = max(r->x1 - output->x, 0),
                        .y1 = max(r->y1 - output->y, 0),
                        .x2 = min(r->x2 - output->x, (int)output->width),
                        .y2 = min(r->y2 - output->y, (int)output->height),
                },
        };

        return t;
}

/*
        One rectangle of the desktop rather than the whole of it. A mouse move
        dirties two small areas; repainting 1280x800 for it would be a megabyte
        of writes. The rectangle is in desktop coordinates.
*/
static void compose_rect(struct output *output, u32 *pixels,
                         const struct drm_rect *r)
{
        struct target t = target_of(output, pixels, r);

        if (t.clip.x2 > t.clip.x1 && t.clip.y2 > t.clip.y1)
                compose_clip(&t);
}

/*
        The cursor, where this output shows it. On a hardware plane it is never
        drawn in, and on the outputs it is not over there is nothing to draw.
*/
static void output_draw_cursor(struct output *output, u32 *pixels)
{
        struct drm_rect cell, screen;
        struct target t;

        cursor_cell(&cell, desktop.cursor_x, desktop.cursor_y,
                    desktop.cursor_shape, desktop.cursor_scale);

        /*
                Whether this draws the cursor, kept here. cursor_shown is
                plane.c's -- set when a plane is armed over the pointer and
                counted by the pointer applet as a plane showing it -- and
                storing this answer into it cleared a live plane's flag on
                every compose: a terminal redrawing under a cursor that sat on
                its plane made the applet say no plane was showing.
        */
        _Bool drawn = !output->cursor_plane && output_touched(output, &cell, 1);

        if (!drawn)
                return;

        drm_rect_init(&screen, output->x, output->y, (int)output->width,
                      (int)output->height);
        t = target_of(output, pixels, &screen);
        canvas_draw_cursor(&t, desktop.cursor_x - output->x,
                           desktop.cursor_y - output->y,
                           desktop.cursor_shape, desktop.cursor_scale);
}

/*
        The scanout buffer, mapped.

        Everything Canvas draws goes through this pointer, so failing to get
        one is not a dropped frame, it is a screen that stays as it was. It
        used to be a bare return.
*/
static _Bool output_map(struct output *output, struct iosys_map *map)
{
        int ret = drm_client_buffer_vmap_local(output->buffer, map);

        if (!ret)
        {
                output->unmappable = false;
                return true;
        }

        if (!output->unmappable)
        {
                output->unmappable = true;
                pr_err("[moonwater canvas] " "the scanout buffer will not map (%d), "
                                                 "so nothing can be drawn on it\n", ret);
        }

        return false;
}

/*
        What the driver actually gave us to scan out, and whether it can be
        written to at all.

        The mapping is made once here rather than first discovered halfway
        through a compose. Everything Canvas draws goes through it, so a driver
        that will not give us one is a screen that stays exactly as it was
        while mode setting and the cursor plane both go on working -- black
        from the first moment, with a cursor moving over it. Answering no here
        is what lets that screen be handed back instead of held.
*/
static _Bool output_describe(struct output *output)
{
        struct drm_framebuffer *fb = output->buffer->fb;
        struct iosys_map map;

        if (!output_map(output, &map))
                return false;

        pr_info("[moonwater canvas] " "scanout %p4cc, %u bytes a row (%lu KiB), modifier %llx, "
                           "%s memory\n", &fb->format->format, fb->pitches[0], ((unsigned long)fb->pitches[0] * output->height) >> 10, (unsigned long long)fb->modifier, map.is_iomem ? "device" : "system");

        drm_client_buffer_vunmap_local(output->buffer);
        return true;
}

/*
        Hands a flush to the flusher: a rectangle in the output's own
        coordinates, or NULL for the whole buffer, which is what compose_output
        tells the driver. With no flusher -- before the canvas thread starts,
        or if its flusher could not be made -- the flush happens here, as every
        flush did before there was one.

        Process context only, which is why flush_lock is a plain spin_lock.
        Every caller composes, and composing sleeps: the console's write path
        only wakes the canvas thread and never queues. Anything that would
        queue from the console, an interrupt or a panic has to make this lock
        irqsave first, or hand the flush to process context instead.
*/
static void output_flush_queue(struct output *output, const struct drm_rect *rect)
{
        if (!canvas_flush_running())
        {
                struct drm_rect clip = rect ? *rect : (struct drm_rect){0};
                u64 started = ktime_get_ns();

                drm_client_buffer_flush(output->buffer, rect ? &clip : NULL);
                canvas_flush_ns += ktime_get_ns() - started;
                return;
        }

        spin_lock(&desktop.flush_lock);

        if (!output->flush_queued)
        {
                output->flush_queued = true;
                output->flush_whole = !rect;
                if (rect)
                        output->flush_pending = *rect;
                list_add_tail(&output->flush_link, &desktop.flush_queue);
        }
        else if (!rect)
                output->flush_whole = true;
        else if (!output->flush_whole)
                canvas_rect_join(&output->flush_pending, rect);

        spin_unlock(&desktop.flush_lock);

        canvas_flush_wake();
}

// The next output with a flush waiting, now marked as with the driver.
static struct output *output_flush_take(struct drm_rect *rect, _Bool *whole)
{
        struct output *output = NULL;

        spin_lock(&desktop.flush_lock);

        if (!list_empty(&desktop.flush_queue))
        {
                output = list_first_entry(&desktop.flush_queue, struct output,
                                          flush_link);
                list_del_init(&output->flush_link);
                *rect = output->flush_pending;
                *whole = output->flush_whole;
                output->flush_queued = false;
                output->flush_whole = false;
                output->flushing = true;
        }

        spin_unlock(&desktop.flush_lock);
        return output;
}

/*
        The flush is back. An output dropped while its buffer was with the
        driver was left for this, because the buffer could not be deleted out
        from under a flush in flight, and its card's release is waiting on it.
*/
static void output_flush_done(struct output *output)
{
        struct canvas *canvas = output->canvas;
        _Bool retired;

        spin_lock(&desktop.flush_lock);
        output->flushing = false;
        retired = output->retired;
        spin_unlock(&desktop.flush_lock);

        if (retired)
        {
                output_free(output);
                atomic_fetch_sub(1, &canvas->retiring);
        }

        wake_up_all(&desktop.flush_idle);
}

/*
        The flusher: the canvas thread's policy, started and stopped beside it,
        and it never takes desktop.lock. Waiting out the driver is all it does.
*/
static int canvas_flush_loop(void *unused)
{
        while (!kthread_should_stop())
        {
                struct drm_rect rect;
                struct output *output;
                _Bool whole = false;
                u64 started;

                set_current_state(TASK_IDLE);
                output = output_flush_take(&rect, &whole);

                if (!output)
                {
                        schedule();
                        continue;
                }

                __set_current_state(TASK_RUNNING);

                started = ktime_get_ns();
                drm_client_buffer_flush(output->buffer, whole ? NULL : &rect);
                canvas_flush_ns += ktime_get_ns() - started;

                output_flush_done(output);
        }

        return 0;
}

/*
        Repaints a set of damaged rectangles on one output and hands the driver
        their union. A set rather than a pair because moving a window damages
        four things: where its frame was and is, and where the cursor was and
        is. The cursor's cell reaches outside the frame it is dragging, so
        leaving it out of the damage leaves a trail of it behind.

        Every rectangle is in desktop coordinates.
*/
static void output_repaint(struct output *output, const struct drm_rect *damage,
                           unsigned int count)
{
        struct iosys_map map;
        struct drm_rect flush;
        u32 *pixels;
        unsigned int i, j;
        u64 started;

        struct drm_rect merged[4];
        unsigned int kept = 0;
        _Bool joined;

        if (!count || count > ARRAY_SIZE(merged) || !output_map(output, &map))
                return;

        /*
                Overlapping damage composed twice is composed twice: a cursor
                that moved four pixels leaves two cells that are nearly the
                same cell, and every window and every glyph under them was
                laid out once for each.

                Until nothing more joins, rather than once through. Joining two
                rectangles grows one of them, and what it grew into can reach a
                third that neither of them touched -- which a single pass has
                already walked past. A window dragged in one step arrives as
                four: where its frame was and is, where the cursor was and is,
                and those chain.
        */
        flush = damage[0];
        memory_copy_apart(merged, (address_any)damage, count * sizeof(*damage));
        kept = count;

        for (i = 1; i < kept; i++)
                canvas_rect_join(&flush, &merged[i]);

        for (joined = true; joined;)
        {
                joined = false;

                for (i = 0; i < kept && !joined; i++)
                        for (j = i + 1; j < kept; j++)
                        {
                                if (!drm_rects_overlap(&merged[i], &merged[j]))
                                        continue;

                                canvas_rect_join(&merged[i], &merged[j]);

                                merged[j] = merged[--kept];
                                joined = true;
                                break;
                        }
        }

        pixels = map.vaddr;
        started = ktime_get_ns();

        for (i = 0; i < kept; i++)
                compose_rect(output, pixels, &merged[i]);

        output_draw_cursor(output, pixels);

        drm_client_buffer_vunmap_local(output->buffer);
        pointer_draw_total += ktime_get_ns() - started;

        drm_rect_translate(&flush, -output->x, -output->y);
        if (!drm_rect_intersect(&flush, &(struct drm_rect){
                        .x2 = (int)output->width, .y2 = (int)output->height }))
                return;

        output_flush_queue(output, &flush);
}

static void compose_output(struct output *output)
{
        struct iosys_map map;
        struct drm_rect screen;
        u32 *pixels;

        if (!output_map(output, &map))
                return;

        pixels = map.vaddr;
        drm_rect_init(&screen, output->x, output->y, (int)output->width,
                      (int)output->height);
        compose_rect(output, pixels, &screen);

        output_draw_cursor(output, pixels);

        drm_client_buffer_vunmap_local(output->buffer);

        /*
                Telling the driver the whole buffer changed, which the damage
                path did and this one did not.

                It is not only for drivers that shadow the framebuffer. i915
                maps a dumb buffer write-back cached and implements dirty as a
                frontbuffer flush, which is what invalidates framebuffer
                compression and panel self refresh. Without it the display
                keeps serving the compressed copy it already had for regions we
                have just painted, and what reaches the screen is the new
                picture with holes of the old one through it.
        */
        output_flush_queue(output, NULL);
}
