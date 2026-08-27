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

static void rect_set(struct drm_rect *rect, int x, int y, int w, int h)
{
        rect->x1 = x;
        rect->y1 = y;
        rect->x2 = x + w;
        rect->y2 = y + h;
}

// The cell the cursor occupies on the desktop, which moves with the hotspot of
// whichever shape it is wearing.
static void cursor_cell(struct drm_rect *rect, int x, int y,
                        unsigned int shape, unsigned int scale)
{
        rect_set(rect, x - canvas_cursor_hot[shape][0] * (int)scale,
                 y - canvas_cursor_hot[shape][1] * (int)scale,
                 CURSOR_W * (int)scale, CURSOR_H * (int)scale);
}

static _Bool output_shows_cursor(struct output *output, int x, int y)
{
        struct drm_rect cell;

        cursor_cell(&cell, x, y, desktop.cursor_shape, desktop.cursor_scale);

        return rects_overlap(cell.x1, cell.y1, cell.x2 - cell.x1, cell.y2 - cell.y1,
                             output->x, output->y,
                             (int)output->width, (int)output->height);
}

struct shape
{
        int x, y, w, h;
        int radius;
};

// One run of one row, already clipped. Everything that draws ends here.
static void target_row(const struct target *t, int y, int x1, int x2, u32 colour)
{
        if (x2 <= x1)
                return;

        canvas_painted += (unsigned long)(x2 - x1);
        canvas_runs++;
        canvas_row_fill(t->pixels + (size_t)y * t->pitch + x1,
                        (unsigned long)(x2 - x1), colour);
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
static void shape_fill(const struct target *t, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h, u32 colour)
{
        int top = max(max(band_y, t->clip.y1), 0);
        int bottom = min(min(band_y + band_h, t->clip.y2), t->height);
        // Clamped to the band, not just to the shape: a titlebar starts below
        // the shape's top, and a rectangle measured from the shape would paint
        // the whole window.
        int curve_top = clamp(shape->y + shape->radius, top, bottom);
        int curve_bottom = clamp(shape->y + shape->h - shape->radius, top, bottom);
        int y, x1, x2;

        for (y = top; y < curve_top; y++)
                if (shape_span(t, shape, band_x, band_w, y, &x1, &x2))
                        target_row(t, y, x1, x2, colour);

        if (curve_bottom > curve_top)
        {
                x1 = max(max(shape->x, band_x), t->clip.x1);
                x2 = min(min(shape->x + shape->w, band_x + band_w),
                         min(t->clip.x2, t->width));

                if (x2 > x1)
                {
                        canvas_painted += (unsigned long)(x2 - x1) *
                                          (curve_bottom - curve_top);
                        canvas_runs++;
                        canvas_rect_fill(t->pixels + (size_t)curve_top * t->pitch + x1,
                                         t->pitch, (unsigned long)(x2 - x1),
                                         (unsigned long)(curve_bottom - curve_top),
                                         colour);
                }
        }

        for (y = curve_bottom; y < bottom; y++)
                if (shape_span(t, shape, band_x, band_w, y, &x1, &x2))
                        target_row(t, y, x1, x2, colour);
}

static void shape_blit(const struct target *t, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h,
                       const u32 *source, unsigned int source_pitch)
{
        int y, x1, x2;

        for (y = band_y; y < band_y + band_h; y++)
        {
                if (!shape_span(t, shape, band_x, band_w, y, &x1, &x2))
                        continue;

                canvas_painted += (unsigned long)(x2 - x1);
                canvas_runs++;
                canvas_row_blit(t->pixels + (size_t)y * t->pitch + x1,
                                source + (size_t)(y - band_y) * source_pitch +
                                    (x1 - band_x),
                                (unsigned long)(x2 - x1), t->opaque);
        }
}

/*
        A window made of text.

        Runs of one paper colour go out as one rectangle, since a terminal is
        mostly one colour behind everything, and then the glyphs. Only the
        cells the damage reaches.
*/
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
                      u32 ink, u32 paper)
{
        if (glyph_is_cell() && desktop.scale == 1 &&
            x >= max(t->clip.x1, 0) && x + WINDOW_CELL_W <= min(t->clip.x2, t->width) &&
            y >= max(t->clip.y1, 0) && y + WINDOW_CELL_H <= min(t->clip.y2, t->height) &&
            !round_inset(y - shape->y, shape->h, shape->radius) &&
            !round_inset(y + WINDOW_CELL_H - 1 - shape->y, shape->h, shape->radius))
        {
                canvas_painted += WINDOW_CELL_W * WINDOW_CELL_H;
                canvas_runs++;
                canvas_cell(t->pixels + (size_t)y * t->pitch + x, t->pitch,
                            glyph_bits(cell->character), WINDOW_CELL_H, ink, paper);
                return;
        }

        shape_fill(t, shape, x, y, canvas_cell_w, canvas_cell_h, paper);
        glyph_draw(t, x, y, (int)desktop.scale, (unsigned char)cell->character, ink);
}

/*
        A window made of text.

        A cell with a letter in it is drawn whole, one pixel one store. Runs of
        blank cells sharing a background go out as one rectangle, since a
        terminal is mostly empty and a rectangle is what the fill is fastest
        at. Only the cells the damage reaches.
*/
static void compose_cells(struct pane *pane, const struct target *t,
                          const struct shape *shape, int x, int y)
{
        /*
                Both the grid the program laid out and the room the window has
                now. They are the same at rest and not during a resize: a
                window that has shrunk still has the larger grid until the
                program catches up, and drawing all of it puts the inside of
                the window on the desktop beside it.
        */
        int columns = (int)min(pane->grid_columns, pane->columns);
        int grid_rows = (int)min(pane->grid_rows, pane->rows);

        int first_row = max((t->clip.y1 - y) / canvas_cell_h, 0);
        int last_row = min((t->clip.y2 - y + canvas_cell_h - 1) / canvas_cell_h,
                           grid_rows);

        // Columns as well as rows. Clipping only the rows meant a cursor
        // moving over a terminal repainted two whole lines of it, eighty
        // cells wide, to put sixteen pixels somewhere.
        int first = max((t->clip.x1 - x) / canvas_cell_w, 0);
        int last = min((t->clip.x2 - x + canvas_cell_w - 1) / canvas_cell_w, columns);
        int row, column;

        for (row = first_row; row < last_row; row++)
        {
                const struct window_cell *cells = pane->cells + row * pane->grid_columns;
                int cy = y + row * canvas_cell_h;

                for (column = first; column < last;)
                {
                        unsigned int character = cells[column].character;
                        u32 paper = canvas_terminal[cells[column].paper & 15] | t->opaque;
                        int run;

                        if (character > ' ' && character <= 126)
                        {
                                cell_draw(t, shape, x + column * canvas_cell_w, cy,
                                          &cells[column],
                                          canvas_terminal[cells[column].ink & 15] |
                                              t->opaque,
                                          paper);
                                column++;
                                continue;
                        }

                        for (run = column; run < last; run++)
                        {
                                unsigned int c = cells[run].character;

                                if (c > ' ' && c <= 126)
                                        break;

                                if ((canvas_terminal[cells[run].paper & 15] |
                                     t->opaque) != paper)
                                        break;
                        }

                        shape_fill(t, shape, x + column * canvas_cell_w, cy,
                                   (run - column) * canvas_cell_w, canvas_cell_h,
                                   paper);

                        column = run;
                }
        }
}

/*
        A pane, in target coordinates.

        The frame is drawn as the parts of it something is not about to cover:
        the strip above the titlebar, the strip below the contents, and the two
        sides. Painting the whole rectangle and covering it up cost 47824
        pixels a window where 2224 could be seen.

        The sides are shape_fill with a band that stops at the contents, which
        is why there is no separate border function: the band clamp already
        measures from the row's inset, so a side follows the curve for free.
*/
static void compose_pane(struct pane *pane, const struct target *t)
{
        _Bool framed = pane->style & WINDOW_FRAME;
        int title = framed ? canvas_title : 0;
        int x = pane->x - t->x;
        int y = pane->y - t->y;
        int bottom = y + title + pane->height;
        struct shape shape;
        int fx, fy, fw, fh;

        if (pane->style & WINDOW_MINIMIZED)
                return;

        pane_frame(pane, &fx, &fy, &fw, &fh);

        /*
                Nothing at all for a window the damage does not touch, and for
                a cursor move that is every window but one. Without this every
                pane laid out its own text on every mouse move, whether or not
                a pixel of it could land.
        */
        if (!rects_overlap(fx - t->x, fy - t->y, fw, fh,
                           t->clip.x1, t->clip.y1,
                           t->clip.x2 - t->clip.x1, t->clip.y2 - t->clip.y1))
                return;

        shape.x = fx - t->x;
        shape.y = fy - t->y;
        shape.w = fw;
        shape.h = fh;
        shape.radius = min(pane->edge, min(shape.w, shape.h) / 2);

        if (framed)
        {
                u32 frame = t->ink[INK_FRAME];

                shape_fill(t, &shape, shape.x, shape.y, shape.w, y - shape.y, frame);
                shape_fill(t, &shape, shape.x, bottom, shape.w,
                           shape.y + shape.h - bottom, frame);
                shape_fill(t, &shape, shape.x, y, x - shape.x, title + pane->height, frame);
                shape_fill(t, &shape, x + pane->width, y,
                           shape.x + shape.w - (x + pane->width),
                           title + pane->height, frame);

                shape_fill(t, &shape, x, y, pane->width, title,
                           t->ink[pane->state & WINDOW_FOCUSED ? INK_TITLE_LIT
                                                              : INK_TITLE]);

                if (pane->title_length)
                        text_draw(t, x + canvas_cell_w, y,
                                  pane->width - canvas_cell_w * 2, title,
                                  pane->title, pane->title_length,
                                  TEXT_CENTRE | TEXT_MIDDLE, (int)desktop.scale,
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
                rect_set(&out[n++], a->x1, a->y1, a->x2 - a->x1, b->y1 - a->y1);

        if (b->y2 < a->y2)
                rect_set(&out[n++], a->x1, b->y2, a->x2 - a->x1, a->y2 - b->y2);

        {
                int top = max(a->y1, b->y1);
                int bottom = min(a->y2, b->y2);

                if (b->x1 > a->x1)
                        rect_set(&out[n++], a->x1, top, b->x1 - a->x1, bottom - top);

                if (b->x2 < a->x2)
                        rect_set(&out[n++], b->x2, top, a->x2 - b->x2, bottom - top);
        }

        return n;
}

static void desktop_fill(const struct target *t, int x1, int y1, int x2, int y2)
{
        struct drm_rect piece[DESKTOP_PIECES], spare[DESKTOP_PIECES];
        unsigned int count = 1, i;
        struct pane *pane;

        rect_set(&piece[0], x1, y1, x2 - x1, y2 - y1);

        list_for_each_entry_reverse(pane, &desktop.windows, link)
        {
                unsigned int kept = 0;
                struct drm_rect cut;
                int fx, fy, fw, fh, radius;

                if (!count)
                        return;

                if (pane->style & WINDOW_MINIMIZED)
                        continue;

                pane_frame(pane, &fx, &fy, &fw, &fh);
                radius = min(pane->edge, min(fw, fh) / 2);
                rect_set(&cut, fx + radius - t->x, fy + radius - t->y,
                         fw - radius * 2, fh - radius * 2);

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

                        memcpy(&spare[kept], part, n * sizeof(*part));
                        kept += n;
                }

                memcpy(piece, spare, kept * sizeof(*piece));
                count = kept;
        }

        for (i = 0; i < count; i++)
        {
                int w = piece[i].x2 - piece[i].x1;
                int h = piece[i].y2 - piece[i].y1;

                if (w <= 0 || h <= 0)
                        continue;

                canvas_painted += (unsigned long)w * h;
                canvas_runs++;
                canvas_rect_fill(t->pixels + (size_t)piece[i].y1 * t->pitch + piece[i].x1,
                                 t->pitch, (unsigned long)w, (unsigned long)h,
                                 t->ink[INK_DESKTOP]);
        }
}

static void compose_clip(const struct target *t)
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
static struct target target_of(struct output *output, u32 *pixels,
                               int rx, int ry, int rw, int rh)
{
        struct target t;

        t.pixels = pixels;
        t.pitch = output->buffer->fb->pitches[0] / sizeof(u32);
        t.width = (int)output->width;
        t.height = (int)output->height;
        t.x = output->x;
        t.y = output->y;
        t.opaque = output->opaque;
        t.ink = output->palette;

        t.clip.x1 = max(rx - output->x, 0);
        t.clip.y1 = max(ry - output->y, 0);
        t.clip.x2 = min(rx + rw - output->x, t.width);
        t.clip.y2 = min(ry + rh - output->y, t.height);

        return t;
}

/*
        One rectangle of the desktop rather than the whole of it. A mouse move
        dirties two small areas; repainting 1280x800 for it would be a megabyte
        of writes. The rectangle is in desktop coordinates.
*/
static void compose_rect(struct output *output, u32 *pixels,
                         int rx, int ry, int rw, int rh)
{
        struct target t = target_of(output, pixels, rx, ry, rw, rh);

        if (t.clip.x2 > t.clip.x1 && t.clip.y2 > t.clip.y1)
                compose_clip(&t);
}

/*
        The cursor, where this output shows it. On a hardware plane it is never
        drawn in, and on the outputs it is not over there is nothing to draw.
*/
static void output_draw_cursor(struct output *output, u32 *pixels)
{
        struct target t;

        output->cursor_shown =
            !output->cursor_plane &&
            output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);

        if (!output->cursor_shown)
                return;

        t = target_of(output, pixels, output->x, output->y,
                      (int)output->width, (int)output->height);

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
                log_canvas_error("the scanout buffer will not map (%d), "
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

        log_canvas("scanout %p4cc, %u bytes a row, modifier %llx, %s memory\n",
                   &fb->format->format, fb->pitches[0],
                   (unsigned long long)fb->modifier,
                   map.is_iomem ? "device" : "system");

        drm_client_buffer_vunmap_local(output->buffer);
        return true;
}

static _Bool output_touched(struct output *output, const struct drm_rect *damage,
                            unsigned int count)
{
        unsigned int i;

        for (i = 0; i < count; i++)
                if (rects_overlap(damage[i].x1, damage[i].y1,
                                  damage[i].x2 - damage[i].x1,
                                  damage[i].y2 - damage[i].y1,
                                  output->x, output->y,
                                  (int)output->width, (int)output->height))
                        return true;

        return false;
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
        for (i = 0; i < count; i++)
                merged[kept++] = damage[i];

        for (joined = true; joined;)
        {
                joined = false;

                for (i = 0; i < kept && !joined; i++)
                        for (j = i + 1; j < kept; j++)
                        {
                                if (!rects_overlap(merged[i].x1, merged[i].y1,
                                                   merged[i].x2 - merged[i].x1,
                                                   merged[i].y2 - merged[i].y1,
                                                   merged[j].x1, merged[j].y1,
                                                   merged[j].x2 - merged[j].x1,
                                                   merged[j].y2 - merged[j].y1))
                                        continue;

                                merged[i].x1 = min(merged[i].x1, merged[j].x1);
                                merged[i].y1 = min(merged[i].y1, merged[j].y1);
                                merged[i].x2 = max(merged[i].x2, merged[j].x2);
                                merged[i].y2 = max(merged[i].y2, merged[j].y2);

                                merged[j] = merged[--kept];
                                joined = true;
                                break;
                        }
        }

        pixels = map.vaddr;
        started = ktime_get_ns();

        for (i = 0; i < kept; i++)
                compose_rect(output, pixels,
                             merged[i].x1, merged[i].y1,
                             merged[i].x2 - merged[i].x1,
                             merged[i].y2 - merged[i].y1);

        output_draw_cursor(output, pixels);

        drm_client_buffer_vunmap_local(output->buffer);
        pointer_draw_total += ktime_get_ns() - started;

        flush = damage[0];

        for (i = 1; i < count; i++)
        {
                flush.x1 = min(flush.x1, damage[i].x1);
                flush.y1 = min(flush.y1, damage[i].y1);
                flush.x2 = max(flush.x2, damage[i].x2);
                flush.y2 = max(flush.y2, damage[i].y2);
        }

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
        u32 *pixels;

        if (!output_map(output, &map))
                return;

        pixels = map.vaddr;

        {
                struct target t = target_of(output, pixels, output->x, output->y,
                                            (int)output->width, (int)output->height);

                compose_clip(&t);
        }

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
