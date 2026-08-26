#include "../src/library.c"
#include "../src/window.c"

// Nowhere in the compositor's palette, so a screenshot can tell them apart.
#define INK 0x00ff9900

static void hold(long seconds)
{
        long timespec[2] = {seconds, 0};

        system_call_2(syscall(nanosleep), (positive)timespec, 0);
}

b32 main()
{
        struct window *window = window_open(320, 200);

        if (!window)
        {
                log_direct(str("no window\n"));
                return 1;
        }

        unsigned int *pixels = window_pixels(window);

        for (unsigned int y = 0; y < window->height; y++)
                for (unsigned int x = 0; x < window->width; x++)
                        pixels[y * window->pitch + x] = INK;

        // Somewhere of its own, so two of these do not land on top of
        // each other.
        b32 id = system_call(syscall(getpid));

        window->x = 200 + (id % 5) * 160;
        window->y = 120 + (id % 3) * 180;
        window_commit(window);

        hold(8);

        window_close(window);
        return 0;
}
