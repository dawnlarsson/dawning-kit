/*
        Canvas -- compose

        Windows are in desktop coordinates and outputs are rectangles of the
        desktop, so composing one output is drawing the desktop offset by where
        that output sits. Back to front, so a later window overlaps an earlier.
*/

static _Bool output_shows_cursor(struct output *output, int x, int y)
{
        return rects_overlap(x, y, CURSOR_W, CURSOR_H,
                             output->x, output->y,
                             (int)output->width, (int)output->height);
}

static void window_frame(struct window *window, int *x, int *y, int *w, int *h)
{
        *x = window->x - 2;
        *y = window->y - 2;
        *w = window->width + 4;
        *h = window->height + 26;
}

static void compose_window(struct window *window, struct output *output,
                           u32 *pixels, unsigned int pitch_pixels)
{
        int x = window->x - output->x;
        int y = window->y - output->y;
        int fx, fy, fw, fh;

        window_frame(window, &fx, &fy, &fw, &fh);

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         fx - output->x, fy - output->y, fw, fh,
                         canvas_colour(COLOUR_FRAME, output->format));

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         x, y, window->width, 20,
                         canvas_colour(COLOUR_TITLE, output->format));

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         x, y + 20, window->width, window->height,
                         canvas_colour(COLOUR_BODY, output->format));
}

/*
        One rectangle of the desktop rather than the whole of it. A mouse move
        dirties two small areas; repainting 1280x800 for it would be a megabyte
        of writes. Coordinates are the desktop's; clipping to the output is
        canvas_fill_rect's job.
*/
static void compose_rect(struct output *output, u32 *pixels, unsigned int pitch_pixels,
                         int rx, int ry, int rw, int rh)
{
        struct window *window;

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         rx - output->x, ry - output->y, rw, rh,
                         canvas_colour(COLOUR_DESKTOP, output->format));

        list_for_each_entry(window, &desktop.windows, link)
        {
                int fx, fy, fw, fh;

                window_frame(window, &fx, &fy, &fw, &fh);

                if (rects_overlap(fx, fy, fw, fh, rx, ry, rw, rh))
                        compose_window(window, output, pixels, pitch_pixels);
        }
}

static void compose_output(struct output *output)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        struct window *window;
        u32 *pixels;

        if (drm_client_buffer_vmap_local(output->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = output->buffer->fb->pitches[0] / sizeof(u32);

        canvas_fill_rect(pixels, pitch_pixels, output->width, output->height,
                         0, 0, output->width, output->height,
                         canvas_colour(COLOUR_DESKTOP, output->format));

        list_for_each_entry(window, &desktop.windows, link)
                compose_window(window, output, pixels, pitch_pixels);

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
