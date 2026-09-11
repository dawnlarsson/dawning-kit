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

/*
        One cell, background and glyph together.

        The whole point is the single pass: filling the paper and then drawing
        the letter over it writes most of the cell twice, and the display is
        reading the buffer while that happens. A cell at the edge of the damage
        or inside a rounded corner still goes the long way round, where a
        double write is worth more than the case is worth handling.
*/
static void cell_draw(const struct target *t, const struct shape *shape,
                      int x, int y, const struct window_cell *cell,
                      const unsigned char *bits, _Bool direct,
                      u32 ink, u32 paper)
{
        if (direct && x >= max(t->clip.x1, 0) &&
            x + WINDOW_CELL_W <= min(t->clip.x2, t->width))
        {
                target_mark(WINDOW_CELL_W * WINDOW_CELL_H);
                canvas_cell(t->pixels + (size_t)y * t->pitch + x, t->pitch,
                            bits, WINDOW_CELL_H, ink, paper);
                return;
        }

        shape_fill(t, shape, x, y, canvas_cell_w, canvas_cell_h, paper);
        glyph_draw(t, x, y, (int)desktop.scale, (unsigned char)cell->character, ink);
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
        _Bool direct = glyph_is_cell() && desktop.scale == 1 &&
                       y >= max(t->clip.y1, 0) &&
                       y + WINDOW_CELL_H <= min(t->clip.y2, t->height) &&
                       !round_inset(y - shape->y, shape->h, shape->radius) &&
                       !round_inset(y + WINDOW_CELL_H - 1 - shape->y,
                                    shape->h, shape->radius);

        /*
                These are properties of the face, not of a cell.  Looking
                them up in cell_draw made every printable character reload
                the font descriptor around an out-of-line assembly call.
        */
        if (direct)
        {
                font_data = font_data_buf(canvas_font->data);
                glyph_size = font_glyph_size(canvas_font->width,
                                             canvas_font->height);
        }

        while (column < used)
        {
                unsigned int character = cells[column].character;
                u32 paper = canvas_terminal[cells[column].paper & 15] | t->opaque;
                int run;

                if (character > ' ' && character <= 126)
                {
                        cell_draw(t, shape, x + column * cell_w, y,
                                  &cells[column],
                                  direct ? font_data + (size_t)character * glyph_size
                                         : NULL,
                                  direct,
                                  canvas_terminal[cells[column].ink & 15] | t->opaque,
                                  paper);
                        column++;
                        continue;
                }

                for (run = column; run < used; run++)
                {
                        unsigned int c = cells[run].character;

                        if (c > ' ' && c <= 126)
                                break;

                        if ((canvas_terminal[cells[run].paper & 15] | t->opaque) != paper)
                                break;
                }

                shape_fill(t, shape, x + column * cell_w, y,
                           (run - column) * cell_w, canvas_cell_h, paper);

                column = run;
        }

        /*
                Past the end of a line there is no cell to take a colour from,
                and what is still in the ring out there is older text: a line
                is as long as it was written, not as wide as the window it is
                being shown in.
        */
        if (column < last)
                shape_fill(t, shape, x + column * cell_w, y,
                           (last - column) * cell_w, canvas_cell_h,
                           canvas_terminal[0] | t->opaque);
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

        /*
                Frame and title damage reaches compose_pane too.  With no
                content cell inside the clip, walking the live view and every
                folded row can only arrive at compose_row calls that reject
                themselves.  Stop before any ring arithmetic instead.
        */
        if (first_row >= last_row || first >= last)
                return;

        unsigned int skip;
        unsigned int line = pane_view_at(pane, pane->view, &skip);
        int row = 0;

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
        gutter.x1 -= t->x;
        gutter.x2 -= t->x;
        gutter.y1 -= t->y;
        gutter.y2 -= t->y;
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
        local_frame.x1 -= t->x;
        local_frame.x2 -= t->x;
        local_frame.y1 -= t->y;
        local_frame.y2 -= t->y;

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
        output->cursor_shown = !output->cursor_plane &&
                               output_touched(output, &cell, 1);

        if (!output->cursor_shown)
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

        flush.x1 = max(flush.x1 - output->x, 0);
        flush.y1 = max(flush.y1 - output->y, 0);
        flush.x2 = min(flush.x2 - output->x, (int)output->width);
        flush.y2 = min(flush.y2 - output->y, (int)output->height);

        if (flush.x2 <= flush.x1 || flush.y2 <= flush.y1)
                return;

        started = ktime_get_ns();
        drm_client_buffer_flush(output->buffer, &flush);
        pointer_flush_total += ktime_get_ns() - started;
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
        {
                u64 started = ktime_get_ns();

                drm_client_buffer_flush(output->buffer, NULL);
                canvas_flush_ns += ktime_get_ns() - started;
        }
}
