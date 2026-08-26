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

static _Bool output_shows_cursor(struct output *output, int x, int y)
{
        return rects_overlap(x, y, CURSOR_W, CURSOR_H,
                             output->x, output->y,
                             (int)output->width, (int)output->height);
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

                canvas_row_fill(pixels, pitch_pixels, x1, x2, y, colour);
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

                canvas_row_blit(pixels, pitch_pixels, x1, x2, y,
                                source + (size_t)(y - band_y) * source_pitch + (x1 - band_x),
                                opaque);
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

                shape_fill(pixels, pitch_pixels, output, clip, &shape,
                           shape.x, shape.y, shape.w, shape.h, frame);
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
                                   canvas_colour(COLOUR_CURSOR, output->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, output->format));
                output->cursor_shown = true;
        }

        drm_client_buffer_vunmap_local(output->buffer);
}
