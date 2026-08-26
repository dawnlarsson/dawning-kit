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
static _Bool output_shows_cursor_shape(struct output *output, int x, int y,
                                       unsigned int shape, unsigned int scale)
{
        return rects_overlap(x - cursor_hot_x(shape) * (int)scale,
                             y - cursor_hot_y(shape) * (int)scale,
                             CURSOR_W * (int)scale, CURSOR_H * (int)scale,
                             output->x, output->y,
                             (int)output->width, (int)output->height);
}

static _Bool output_shows_cursor(struct output *output, int x, int y)
{
        return output_shows_cursor_shape(output, x, y, desktop.cursor_shape,
                                         desktop.cursor_scale);
}

struct shape
{
        int x, y, w, h;
        int radius;
};

/*
        A band of the shape: the corners follow the shape's curve, the sides
        are the band's own. The frame is a band the full width of the shape;
        the titlebar and the contents are inset by the border, and only meet
        the curve where they reach a corner.
*/
static void shape_fill(u32 *pixels, unsigned int pitch_pixels, struct output *output,
                       const struct drm_rect *clip, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h, u32 colour)
{
        int y;

        for (y = max(band_y, clip->y1); y < min(band_y + band_h, clip->y2); y++)
        {
                int inset = round_inset(y - shape->y, shape->h, shape->radius);
                int x1 = max(max(max(shape->x + inset, band_x), clip->x1), 0);
                int x2 = min(min(min(shape->x + shape->w - inset, band_x + band_w),
                                 clip->x2),
                             (int)output->width);

                if (y < 0 || y >= (int)output->height || x2 <= x1)
                        continue;

                canvas_painted += (unsigned long)(x2 - x1);
                canvas_row_fill(pixels + (size_t)y * pitch_pixels + x1,
                                (unsigned long)(x2 - x1), colour);
        }
}

static void shape_blit(u32 *pixels, unsigned int pitch_pixels, struct output *output,
                       const struct drm_rect *clip, const struct shape *shape,
                       int band_x, int band_y, int band_w, int band_h,
                       const u32 *source, unsigned int source_pitch, u32 format)
{
        u32 opaque = format == DRM_FORMAT_ARGB8888 ? 0xff000000 : 0;
        int y;

        for (y = max(band_y, clip->y1); y < min(band_y + band_h, clip->y2); y++)
        {
                int inset = round_inset(y - shape->y, shape->h, shape->radius);
                int x1 = max(max(max(shape->x + inset, band_x), clip->x1), 0);
                int x2 = min(min(min(shape->x + shape->w - inset, band_x + band_w),
                                 clip->x2),
                             (int)output->width);

                if (y < 0 || y >= (int)output->height || x2 <= x1)
                        continue;

                canvas_painted += (unsigned long)(x2 - x1);
                canvas_row_blit(pixels + (size_t)y * pitch_pixels + x1,
                                source + (size_t)(y - band_y) * source_pitch + (x1 - band_x),
                                (unsigned long)(x2 - x1), opaque);
        }
}

/*
        The part of the shape that is left over beside an inner rectangle.

        A frame is a border, not a filled rectangle with a window painted over
        it. Painting the whole of it and covering it up cost more pixels than
        the window itself: 47824 written for 2224 that could be seen.

        The curve is why this is not four thin bands. On a rounded corner the
        edge is not at the shape's edge, it is at the inset for that row, so
        the border has to be measured from there.
*/
static void shape_border(u32 *pixels, unsigned int pitch_pixels, struct output *output,
                         const struct drm_rect *clip, const struct shape *shape,
                         int inner_x, int inner_w, int band_y, int band_h, u32 colour)
{
        int y;

        for (y = max(band_y, clip->y1); y < min(band_y + band_h, clip->y2); y++)
        {
                int inset = round_inset(y - shape->y, shape->h, shape->radius);
                int outer_left = shape->x + inset;
                int outer_right = shape->x + shape->w - inset;
                int x1, x2;

                if (y < 0 || y >= (int)output->height)
                        continue;

                x1 = max(max(outer_left, clip->x1), 0);
                x2 = min(min(min(inner_x, outer_right), clip->x2), (int)output->width);

                if (x2 > x1)
                {
                        canvas_painted += (unsigned long)(x2 - x1);
                        canvas_row_fill(pixels + (size_t)y * pitch_pixels + x1,
                                        (unsigned long)(x2 - x1), colour);
                }

                x1 = max(max(max(inner_x + inner_w, outer_left), clip->x1), 0);
                x2 = min(min(outer_right, clip->x2), (int)output->width);

                if (x2 > x1)
                {
                        canvas_painted += (unsigned long)(x2 - x1);
                        canvas_row_fill(pixels + (size_t)y * pitch_pixels + x1,
                                        (unsigned long)(x2 - x1), colour);
                }
        }
}

static void compose_pane(struct pane *pane, struct output *output,
                         u32 *pixels, unsigned int pitch_pixels,
                         const struct drm_rect *clip)
{
        _Bool framed = pane->style & WINDOW_FRAME;
        int title = framed ? WINDOW_TITLE : 0;
        int x = pane->x - output->x;
        int y = pane->y - output->y;
        struct shape shape;
        int fx, fy, fw, fh;

        if (pane->style & WINDOW_MINIMIZED)
                return;

        pane_frame(pane, &fx, &fy, &fw, &fh);

        shape.x = fx - output->x;
        shape.y = fy - output->y;
        shape.w = fw;
        shape.h = fh;
        shape.radius = min(pane->edge, min(shape.w, shape.h) / 2);

        if (framed)
        {
                u32 frame = canvas_colour(COLOUR_FRAME, output->format);
                u32 bar = canvas_colour(pane->state & WINDOW_FOCUSED
                                            ? COLOUR_TITLE_FOCUSED
                                            : COLOUR_TITLE,
                                        output->format);

                // Only the parts of the frame something is not about to
                // cover: the strip above, the strip below, and the two sides.
                shape_fill(pixels, pitch_pixels, output, clip, &shape,
                           shape.x, shape.y, shape.w, y - shape.y, frame);
                shape_fill(pixels, pitch_pixels, output, clip, &shape,
                           shape.x, y + title + pane->height, shape.w,
                           shape.y + shape.h - (y + title + pane->height), frame);
                shape_border(pixels, pitch_pixels, output, clip, &shape,
                             x, pane->width, y, title + pane->height, frame);

                shape_fill(pixels, pitch_pixels, output, clip, &shape,
                           x, y, pane->width, title, bar);

                if (pane->title_length)
                        text_draw(pixels, pitch_pixels, output, clip,
                                  x + 8, y, pane->width - 16, title,
                                  pane->title, pane->title_length,
                                  TEXT_CENTRE | TEXT_MIDDLE, 1,
                                  canvas_colour(COLOUR_TEXT, output->format));
        }

        if (pane->pixels)
                shape_blit(pixels, pitch_pixels, output, clip, &shape,
                           x, y + title, pane->width, pane->height,
                           pane->pixels, pane->pitch, output->format);
        else
        {
                shape_fill(pixels, pitch_pixels, output, clip, &shape,
                           x, y + title, pane->width, pane->height,
                           canvas_colour(COLOUR_BODY, output->format));

                text_draw(pixels, pitch_pixels, output, clip,
                          x + 8, y + title + 8, pane->width - 16, pane->height - 16,
                          pane_placeholder, sizeof(pane_placeholder) - 1,
                          TEXT_LEFT | TEXT_TOP | TEXT_WRAP, 1,
                          canvas_colour(COLOUR_TEXT, output->format));
        }
}

static void compose_clip(struct output *output, u32 *pixels, unsigned int pitch_pixels,
                         const struct drm_rect *clip)
{
        struct pane *pane;

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         clip->x1, clip->y1, clip->x2 - clip->x1, clip->y2 - clip->y1,
                         canvas_colour(COLOUR_DESKTOP, output->format));

        list_for_each_entry(pane, &desktop.windows, link)
                compose_pane(pane, output, pixels, pitch_pixels, clip);
}

/*
        One rectangle of the desktop rather than the whole of it. A mouse move
        dirties two small areas; repainting 1280x800 for it would be a megabyte
        of writes. The rectangle is in desktop coordinates.
*/
static void compose_rect(struct output *output, u32 *pixels, unsigned int pitch_pixels,
                         int rx, int ry, int rw, int rh)
{
        struct drm_rect clip;

        clip.x1 = max(rx - output->x, 0);
        clip.y1 = max(ry - output->y, 0);
        clip.x2 = min(rx + rw - output->x, (int)output->width);
        clip.y2 = min(ry + rh - output->y, (int)output->height);

        if (clip.x2 <= clip.x1 || clip.y2 <= clip.y1)
                return;

        compose_clip(output, pixels, pitch_pixels, &clip);
}

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
        rect_set(rect, x - cursor_hot_x(shape) * (int)scale,
                 y - cursor_hot_y(shape) * (int)scale,
                 CURSOR_W * (int)scale, CURSOR_H * (int)scale);
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
        unsigned int pitch_pixels;
        struct drm_rect flush;
        u32 *pixels;
        unsigned int i;
        u64 started;

        if (!count || drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);
        started = ktime_get_ns();

        for (i = 0; i < count; i++)
                compose_rect(output, pixels, pitch_pixels,
                             damage[i].x1, damage[i].y1,
                             damage[i].x2 - damage[i].x1,
                             damage[i].y2 - damage[i].y1);

        output->cursor_shown =
            !output->cursor_plane &&
            output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y);

        if (output->cursor_shown)
                canvas_draw_cursor(pixels, pitch_pixels, output->width, output->height,
                                   desktop.cursor_x - output->x,
                                   desktop.cursor_y - output->y,
                                   desktop.cursor_shape, desktop.cursor_scale,
                                   canvas_colour(COLOUR_CURSOR, output->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, output->format));

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
        struct drm_rect whole = {0, 0, (int)output->width, (int)output->height};
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;

        if (drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);

        compose_clip(output, pixels, pitch_pixels, &whole);

        output->cursor_shown = false;

        // Only where the cursor actually is. On a plane it is never drawn in,
        // and on the outputs it is not over there is nothing to draw.
        if (!output->cursor_plane &&
            output_shows_cursor(output, desktop.cursor_x, desktop.cursor_y))
        {
                canvas_draw_cursor(pixels, pitch_pixels, output->width, output->height,
                                   desktop.cursor_x - output->x,
                                   desktop.cursor_y - output->y,
                                   desktop.cursor_shape, desktop.cursor_scale,
                                   canvas_colour(COLOUR_CURSOR, output->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, output->format));
                output->cursor_shown = true;
        }

        drm_client_buffer_vunmap_local(output->buffer);
}
