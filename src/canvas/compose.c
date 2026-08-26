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

// What a window with no program behind it says, and the one place wrapping is
// exercised until a program can ask for text of its own.
static const char pane_placeholder[] =
    "This window is the compositor's own. A program draws its contents "
    "through the page it shares, and this is what one looks like before "
    "anything has.";

// The cell the cursor occupies, which moves with the hotspot of its shape.
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
                canvas_row_blit(t->pixels + (size_t)y * t->pitch + x1,
                                source + (size_t)(y - band_y) * source_pitch +
                                    (x1 - band_x),
                                (unsigned long)(x2 - x1), t->opaque);
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
        int title = framed ? WINDOW_TITLE : 0;
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
                        text_draw(t, x + 8, y, pane->width - 16, title,
                                  pane->title, pane->title_length,
                                  TEXT_CENTRE | TEXT_MIDDLE, 1, t->ink[INK_TEXT]);
        }

        if (pane->pixels)
                shape_blit(t, &shape, x, y + title, pane->width, pane->height,
                           pane->pixels, pane->pitch);
        else
        {
                shape_fill(t, &shape, x, y + title, pane->width, pane->height,
                           t->ink[INK_BODY]);

                text_draw(t, x + 8, y + title + 8, pane->width - 16, pane->height - 16,
                          pane_placeholder, sizeof(pane_placeholder) - 1,
                          TEXT_LEFT | TEXT_TOP | TEXT_WRAP, 1, t->ink[INK_TEXT]);
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
        {
                canvas_painted += (unsigned long)(x2 - x1) * (y2 - y1);
                canvas_runs++;
                canvas_rect_fill(t->pixels + (size_t)y1 * t->pitch + x1, t->pitch,
                                 (unsigned long)(x2 - x1), (unsigned long)(y2 - y1),
                                 t->ink[INK_DESKTOP]);
        }

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
        unsigned int pitch_pixels;
        u32 *pixels;
        unsigned int i;
        u64 started;

        if (!count || drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);
        started = ktime_get_ns();

        for (i = 0; i < count; i++)
                compose_rect(output, pixels,
                             damage[i].x1, damage[i].y1,
                             damage[i].x2 - damage[i].x1,
                             damage[i].y2 - damage[i].y1);

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

        if (drm_client_buffer_vmap_local(output->buffer, &map))
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
