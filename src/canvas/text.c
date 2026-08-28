/*
        Canvas -- text

        Text is the compositor's to draw, not each program's. A program says
        what the words are and what box they go in; where the lines break and
        where they sit inside it is decided here, once, for everything on the
        machine.

        The face is one the kernel already carries for its own console, so
        there is no font file to ship and no parser to write. It is a bitmap,
        so a size is an integer scale of it rather than a point value.
*/

static const struct font_desc *canvas_font;

static const unsigned char *glyph_bits(unsigned int character)
{
        return font_data_buf(canvas_font->data) +
               (size_t)character *
                   font_glyph_size(canvas_font->width, canvas_font->height);
}

// Whether the face is the one canvas_cell assumes: eight wide, a byte a row,
// and a cell tall.
static _Bool glyph_is_cell(void)
{
        return canvas_font && canvas_font->width == WINDOW_CELL_W &&
               canvas_font->height == WINDOW_CELL_H &&
               font_glyph_pitch(canvas_font->width) == 1;
}

// One glyph, which is a bitmap like any other: bits_draw in paint.c is the
// walk, and the face says how wide and how tall.
static void glyph_draw(const struct target *t, int x, int y, int scale,
                       unsigned char character, u32 colour)
{
        bits_draw(t, x, y, scale, glyph_bits(character),
                  font_glyph_pitch(canvas_font->width), canvas_font->width,
                  canvas_font->height, colour);
}

/*
        Where one line ends.

        Greedy: take as many characters as fit, and if that lands mid word back
        up to the last space. A single word longer than the box breaks where it
        runs out of room, because the alternative is drawing off the stop.
*/
static unsigned int text_line_end(const char *text, unsigned int length,
                                  unsigned int start, unsigned int columns, _Bool wrap)
{
        unsigned int i, last_space = 0;

        for (i = start; i < length; i++)
        {
                if (text[i] == '\n')
                        return i;

                if (!wrap)
                        continue;

                if (text[i] == ' ')
                        last_space = i;

                if (i - start + 1 > columns)
                        return last_space > start ? last_space : i;
        }

        return length;
}

// Past the break, and past the character the break was made on.
static unsigned int text_line_next(const char *text, unsigned int length, unsigned int stop)
{
        if (stop < length && (text[stop] == '\n' || text[stop] == ' '))
                return stop + 1;

        return stop;
}

static unsigned int text_line_count(const char *text, unsigned int length,
                                    unsigned int columns, _Bool wrap)
{
        unsigned int start = 0, lines = 0;

        if (!length)
                return 0;

        while (start < length)
        {
                unsigned int stop = text_line_end(text, length, start, columns, wrap);

                lines++;
                start = text_line_next(text, length, stop);

                if (stop == length)
                        break;
        }

        return lines;
}

/*
        Lays a string out in a box and draws it. Coordinates are the output's,
        the clip is the damage, and nothing is drawn outside either.
*/
static void text_draw(const struct target *t, int x, int y, int w, int h,
                      const char *text, unsigned int length,
                      unsigned int align, int scale, u32 colour)
{
        u64 started = ktime_get_ns();
        struct target box;
        int cell_w, cell_h;
        _Bool wrap = align & TEXT_WRAP;
        unsigned int columns, start = 0;
        int line_y;

        if (!canvas_font || !length)
                return;

        /*
                The box is a clip like the damage is. A caller asks for a title
                in a titlebar and hands over whatever string the window has;
                one newline in it and the second line lands on the contents
                below, so the box is met at the same place the damage is rather
                than trusted to the layout above.
        */
        box = *t;
        box.clip.x1 = max(t->clip.x1, x);
        box.clip.y1 = max(t->clip.y1, y);
        box.clip.x2 = min(t->clip.x2, x + w);
        box.clip.y2 = min(t->clip.y2, y + h);

        if (box.clip.x2 <= box.clip.x1 || box.clip.y2 <= box.clip.y1)
                return;

        cell_w = (int)canvas_font->width * scale;
        cell_h = (int)canvas_font->height * scale;
        columns = cell_w > 0 && w > 0 ? (unsigned int)(w / cell_w) : 0;

        if (!columns)
                return;

        if (!wrap)
                columns = length;

        switch (align & (TEXT_MIDDLE | TEXT_BOTTOM))
        {
        case TEXT_MIDDLE:
                line_y = y + (h - (int)text_line_count(text, length, columns, wrap) * cell_h) / 2;
                break;
        case TEXT_BOTTOM:
                line_y = y + h - (int)text_line_count(text, length, columns, wrap) * cell_h;
                break;
        default:
                line_y = y;
                break;
        }

        while (start < length && line_y < y + h)
        {
                unsigned int stop = text_line_end(text, length, start, columns, wrap);
                int run = (int)(stop - start);
                int line_x = x;
                int i;

                // A line the damage cannot reach still has to be measured, so
                // the ones after it start in the right place, but nothing in
                // it needs looking at glyph by glyph. Repainting under a
                // cursor is twenty rows of a paragraph that is a hundred.
                if (line_y + cell_h <= box.clip.y1 || line_y >= box.clip.y2)
                {
                        line_y += cell_h;
                        start = text_line_next(text, length, stop);

                        if (stop == length)
                                break;

                        continue;
                }

                switch (align & (TEXT_CENTRE | TEXT_RIGHT))
                {
                case TEXT_CENTRE:
                        line_x = x + (w - run * cell_w) / 2;
                        break;
                case TEXT_RIGHT:
                        line_x = x + w - run * cell_w;
                        break;
                }

                for (i = 0; i < run; i++)
                {
                        int px = line_x + i * cell_w;

                        // Nothing to do for a glyph entirely outside the box or
                        // the damage.
                        if (px + cell_w <= box.clip.x1 || px >= box.clip.x2)
                                continue;

                        glyph_draw(&box, px, line_y, scale,
                                   (unsigned char)text[start + i], colour);
                }

                line_y += cell_h;
                start = text_line_next(text, length, stop);

                if (stop == length)
                        break;
        }

        canvas_text_ns += ktime_get_ns() - started;
}
