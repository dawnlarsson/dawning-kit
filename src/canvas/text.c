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

/*
        One glyph. The set bits of a row are drawn as runs rather than one at a
        time: a call for every lit pixel is thousands of calls for a line of
        text, and a run of a few is what the fill is cheapest at.
*/
static void glyph_draw(const struct target *t, int x, int y, int scale,
                       unsigned char character, u32 colour)
{
        unsigned int pitch = font_glyph_pitch(canvas_font->width);
        const unsigned char *glyph =
            font_data_buf(canvas_font->data) +
            (size_t)character * font_glyph_size(canvas_font->width, canvas_font->height);
        unsigned int row, column;

        /*
                The whole glyph in one call, when it is entirely inside the
                damage and drawn at its own size. That is nearly every glyph;
                the rest go the long way round below.
        */
        if (scale == 1 && canvas_font->width == 8 && pitch == 1 &&
            x >= max(t->clip.x1, 0) && x + 8 <= min(t->clip.x2, t->width) &&
            y >= max(t->clip.y1, 0) &&
            y + (int)canvas_font->height <= min(t->clip.y2, t->height))
        {
                canvas_painted += canvas_font->height * 8;
                canvas_runs++;
                canvas_glyph(t->pixels + (size_t)y * t->pitch + x, t->pitch,
                             glyph, canvas_font->height, colour);
                return;
        }

        for (row = 0; row < canvas_font->height; row++)
        {
                const unsigned char *bits = glyph + row * pitch;

                for (column = 0; column < canvas_font->width;)
                {
                        unsigned int run = column;
                        int px, x1, x2, line;

                        if (!(bits[column / 8] & (0x80 >> (column % 8))))
                        {
                                column++;
                                continue;
                        }

                        while (run < canvas_font->width &&
                               (bits[run / 8] & (0x80 >> (run % 8))))
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
        int cell_w = (int)canvas_font->width * scale;
        int cell_h = (int)canvas_font->height * scale;
        _Bool wrap = align & TEXT_WRAP;
        unsigned int columns = cell_w > 0 && w > 0 ? (unsigned int)(w / cell_w) : 0;
        unsigned int start = 0;
        int line_y;

        u64 started = ktime_get_ns();

        if (!canvas_font || !length || !columns)
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
                        if (px + cell_w <= x || px >= x + w ||
                            px + cell_w <= t->clip.x1 || px >= t->clip.x2)
                                continue;

                        glyph_draw(t, px, line_y, scale,
                                   (unsigned char)text[start + i], colour);
                }

                line_y += cell_h;
                start = text_line_next(text, length, stop);

                if (stop == length)
                        break;
        }

        canvas_text_ns += ktime_get_ns() - started;
}
