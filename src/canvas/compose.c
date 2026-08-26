/*
        Canvas -- compose

        Panes are in desktop coordinates and outputs are rectangles of the
        desktop, so composing one output is drawing the desktop offset by where
        that output sits. Back to front, which is the list order, which
        pane_by_z keeps in z order.

        Everything here takes a clip in output coordinates. Repainting damage
        rather than a whole screen is the reason: a pane that merely overlaps
        the damage would otherwise repaint all of itself, over whatever was
        drawn above it outside that rectangle.
*/

static _Bool output_shows_cursor(struct output *output, int x, int y)
{
        return rects_overlap(x, y, CURSOR_W, CURSOR_H,
                             output->x, output->y,
                             (int)output->width, (int)output->height);
}

static void pane_frame(struct pane *pane, int *x, int *y, int *w, int *h)
{
        *x = pane->x - 2;
        *y = pane->y - 2;
        *w = pane->width + 4;
        *h = pane->height + WINDOW_TITLE + 6;
}

static void fill_within(u32 *pixels, unsigned int pitch_pixels, struct output *output,
                        const struct drm_rect *clip,
                        int x, int y, int w, int h, u32 colour)
{
        int x1 = max(x, clip->x1);
        int y1 = max(y, clip->y1);
        int x2 = min(x + w, clip->x2);
        int y2 = min(y + h, clip->y2);

        if (x2 <= x1 || y2 <= y1)
                return;

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         x1, y1, x2 - x1, y2 - y1, colour);
}

static void blit_within(u32 *pixels, unsigned int pitch_pixels, struct output *output,
                        const struct drm_rect *clip,
                        int x, int y, int w, int h,
                        const u32 *source, unsigned int source_pitch, u32 format)
{
        int x1 = max(x, clip->x1);
        int y1 = max(y, clip->y1);
        int x2 = min(x + w, clip->x2);
        int y2 = min(y + h, clip->y2);

        if (x2 <= x1 || y2 <= y1)
                return;

        // The source moves with the corner the clip cut off.
        source += (size_t)(y1 - y) * source_pitch + (x1 - x);

        canvas_blit_rect(pixels, pitch_pixels, output->width, output->height,
                         x1, y1, x2 - x1, y2 - y1, source, source_pitch, format);
}

static void compose_pane(struct pane *pane, struct output *output,
                         u32 *pixels, unsigned int pitch_pixels,
                         const struct drm_rect *clip)
{
        int x = pane->x - output->x;
        int y = pane->y - output->y;
        int fx, fy, fw, fh;

        pane_frame(pane, &fx, &fy, &fw, &fh);

        fill_within(pixels, pitch_pixels, output, clip,
                    fx - output->x, fy - output->y, fw, fh,
                    canvas_colour(COLOUR_FRAME, output->format));

        fill_within(pixels, pitch_pixels, output, clip,
                    x, y, pane->width, WINDOW_TITLE,
                    canvas_colour(COLOUR_TITLE, output->format));

        if (pane->pixels)
                blit_within(pixels, pitch_pixels, output, clip,
                            x, y + WINDOW_TITLE, pane->width, pane->height,
                            pane->pixels, pane->pitch, output->format);
        else
                fill_within(pixels, pitch_pixels, output, clip,
                            x, y + WINDOW_TITLE, pane->width, pane->height,
                            canvas_colour(COLOUR_BODY, output->format));
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
