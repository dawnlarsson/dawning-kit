#include "../src/library.c"
#include "../src/window.c"

// Two colours in no compositor palette, so a screenshot can tell the windows
// apart and say which one is in front.
#define INK_BACK 0x00ff9900
#define INK_FRONT 0x000066cc

static void fill(struct window *window, unsigned int colour)
{
        unsigned int *pixels = window_pixels(window);

        for (unsigned int y = 0; y < window->height; y++)
                for (unsigned int x = 0; x < window->width; x++)
                        pixels[y * window->pitch + x] = colour;
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

        if (!back || !front)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        fill(back, INK_BACK);
        back->region = WINDOW_CENTRED;
        window_commit(back);

        fill(front, INK_FRONT);
        front->x = back->x + 60;
        front->y = back->y + 60;
        window_commit(front);

        hold(8);

        window_close(front);
        window_close(back);
        return 0;
}
