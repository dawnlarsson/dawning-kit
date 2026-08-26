/*
        Canvas -- compose

        Back to front, so a later window overlaps an earlier one. Windows are
        solid colour until userspace can hand over pixels of its own.
*/

static void canvas_compose_window(struct window *window, struct surface *surface,
                                  u32 *pixels, unsigned int pitch_pixels)
{
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

/*
        One rectangle rather than the whole screen. A mouse move dirties two
        small areas; repainting 1280x800 for it would be a megabyte of writes.
*/
static void canvas_compose_rect(struct canvas *canvas, struct surface *surface,
                                u32 *pixels, unsigned int pitch_pixels,
                                int rx, int ry, int rw, int rh)
{
        struct window *window;

        canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                         rx, ry, rw, rh,
                         canvas_colour(COLOUR_DESKTOP, surface->format));

        list_for_each_entry(window, &canvas->windows, link)
        {
                int fx = window->x - 2;
                int fy = window->y - 2;
                int fw = window->width + 4;
                int fh = window->height + 26;

                if (fx >= rx + rw || fx + fw <= rx || fy >= ry + rh || fy + fh <= ry)
                        continue;

                canvas_compose_window(window, surface, pixels, pitch_pixels);
        }
}

static void canvas_compose(struct canvas *canvas, struct surface *surface)
{
        struct iosys_map map;
        unsigned int pitch_pixels;
        struct window *window;
        u32 *pixels;

        if (drm_client_buffer_vmap_local(surface->buffer, &map))
                return;

        pixels = map.vaddr;
        pitch_pixels = surface->buffer->fb->pitches[0] / sizeof(u32);

        canvas_fill_rect(pixels, pitch_pixels, surface->width, surface->height,
                         0, 0, surface->width, surface->height,
                         canvas_colour(COLOUR_DESKTOP, surface->format));

        list_for_each_entry(window, &canvas->windows, link)
            canvas_compose_window(window, surface, pixels, pitch_pixels);

        // Skipped on a cursor plane, or the arrow is baked into the desktop
        // underneath the real one and left behind wherever the pointer was.
        if (!surface->cursor_plane)
                canvas_draw_cursor(pixels, pitch_pixels, surface->width, surface->height,
                                   canvas->cursor_x, canvas->cursor_y,
                                   canvas_colour(COLOUR_CURSOR, surface->format),
                                   canvas_colour(COLOUR_CURSOR_EDGE, surface->format));

        drm_client_buffer_vunmap_local(surface->buffer);
}
