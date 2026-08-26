/*
        Canvas -- compose

        What a window looks like and what order they are drawn in: back to
        front, so a later window overlaps an earlier one.

        Windows are solid colour -- a frame, a titlebar, a body -- because
        nothing in userspace can hand over pixels of its own yet. When that
        arrives it arrives here, and close to only here.
*/

/*
        Composes one rectangle rather than the whole screen.

        Moving the cursor dirties two small areas: where it was and where it
        is. Repainting a 1280x800 desktop for a mouse move would be about a
        megabyte of writes per event; this is a few kilobytes.
*/
static void canvas_compose_rect(struct canvas *canvas,
                                      struct surface *surface,
                                      u32 *pixels, unsigned int pitch_pixels,
                                      int rx, int ry, int rw, int rh)
{
        unsigned int i;

        canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               rx, ry, rw, rh,
                               canvas_colour(COLOUR_DESKTOP, surface->format));

        for (i = 0; i < MAX_WINDOWS; i++) {
                struct window *window = &canvas->windows[i];
                int fx, fy, fw, fh;

                if (!window->present)
                        continue;

                fx = window->x - 2;
                fy = window->y - 2;
                fw = window->width + 4;
                fh = window->height + 26;

                // Skip windows that cannot touch the damaged area at all.
                if (fx >= rx + rw || fx + fw <= rx || fy >= ry + rh || fy + fh <= ry)
                        continue;

                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       fx, fy, fw, fh,
                                       canvas_colour(COLOUR_FRAME, surface->format));
                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y, window->width, 20,
                                       canvas_colour(COLOUR_TITLE, surface->format));
                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                                       window->x, window->y + 20, window->width, window->height,
                                       canvas_colour(COLOUR_BODY, surface->format));
        }
}

static void canvas_compose(struct canvas *canvas, struct surface *surface)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        u32 *pixels;
        unsigned int i;

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                       0, 0, surface->width, surface->height,
                       canvas_colour(COLOUR_DESKTOP, surface->format));

        // Back to front, so a later window overlaps an earlier one.
        for (i = 0; i < MAX_WINDOWS; i++) {
                struct window *window = &canvas->windows[i];

                if (!window->present)
                        continue;

                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x - 2, window->y - 2,
                               window->width + 4, window->height + 26,
                               canvas_colour(COLOUR_FRAME, surface->format));

                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y, window->width, 20,
                               canvas_colour(COLOUR_TITLE, surface->format));

                canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                               window->x, window->y + 20, window->width, window->height,
                               canvas_colour(COLOUR_BODY, surface->format));
        }

        // Skipped when the cursor lives on its own plane, or the arrow would
        // be baked into the desktop underneath the real one and left behind
        // wherever the pointer last was.
        if (!surface->cursor_plane)
                canvas_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                 canvas->cursor_x, canvas->cursor_y,
                                 canvas_colour(COLOUR_CURSOR, surface->format),
                                 canvas_colour(COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);
}
