#include "../src/library.c"
#include "../src/window.c"

// Colours in no compositor palette, so a screenshot can tell them apart.
#define INK_BACK 0x00ff9900
#define INK_FRONT 0x000066cc
#define INK_BARE 0x0022bb55

static void fill(struct window *window, unsigned int colour)
{
        unsigned int *pixels = window_pixels(window);

        for (unsigned int y = 0; y < window->height; y++)
                for (unsigned int x = 0; x < window->width; x++)
                        pixels[y * window->pitch + x] = colour;
}

static void title(struct window *window, const char *text)
{
        unsigned int i;

        for (i = 0; i + 1 < WINDOW_TITLE_MAX && text[i]; i++)
                window->title[i] = text[i];

        window->title[i] = 0;
}

static void hold(long seconds)
{
        long timespec[2] = {seconds, 0};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

b32 main()
{
        struct window *back = window_open(400, 260);
        struct window *front = window_open(200, 120);
        struct window *bare = window_open(160, 100);

        if (!back || !front || !bare)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        fill(back, INK_BACK);
        back->region = WINDOW_CENTRED;
        back->edge = 16;
        title(back, "Centred, rounded");
        window_commit(back);

        fill(front, INK_FRONT);
        front->x = back->x + 60;
        front->y = back->y + 60;
        title(front, "On top");
        window_commit(front);

        // No frame, so no titlebar, no border, and nothing to drag it by.
        fill(bare, INK_BARE);
        bare->style = 0;
        bare->x = 100;
        bare->y = 620;
        window_commit(bare);

        hold(5);

        // Put one away and let the other cover its display.
        front->style |= WINDOW_MINIMIZED;
        window_commit(front);

        bare->style |= WINDOW_FULLSCREEN;
        window_commit(bare);

        hold(7);

        window_close(bare);
        window_close(front);
        window_close(back);
        return 0;
}
