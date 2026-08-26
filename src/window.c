/*
        Moonwater windows

        The whole interface a program needs to put something on screen. Include
        this one file and nothing else: it carries its own syscalls, so it
        works from a spark image with no runtime and from an ordinary ELF
        against libc.

                struct window *w = window_open(640, 480);

                unsigned int *pixels = window_pixels(w);
                pixels[y * w->pitch + x] = 0x00ff9900;

                w->x = 200;
                w->y = 120;
                window_commit(w);

        The struct is a page the program and the compositor both have mapped,
        so moving or resizing a window is a store, not a call. The fields above
        the line are the program's to write and the compositor reads them; the
        ones below are the compositor's and the program reads them.

        The pixels are the window's contents. The compositor draws the frame
        and the titlebar around them, so x,y is the top left of the whole
        window and the contents land WINDOW_TITLE below it.

        z is the order they stack in: higher is in front. Clicking a window
        puts it above every other, and the compositor writes the new value
        back, so a program can always read where it ended up.

        region is where a window wants to be rather than where it is. Zero is
        free floating and x,y are the program's; anything else is the
        compositor's placement and it writes x,y to say where that landed. It
        is a number and not a flag because there is more than one answer --
        centred is the one that exists so far.
*/

#ifndef WINDOW_INCLUDED
#define WINDOW_INCLUDED

#define WINDOW_DEVICE "/dev/spark"

// The contents start one page in, so the struct and the pixels are one mapping
// and the pixels stay page aligned.
#define WINDOW_PIXELS 4096

// The titlebar the compositor draws above the contents.
#define WINDOW_TITLE 20

// region
#define WINDOW_FREE 0
#define WINDOW_CENTRED 1

struct window
{
        // The program writes these, and the compositor writes x, y and z back
        // when it is the one that moved them.
        int x, y;
        int z;                 // higher is in front
        unsigned int width, height;
        unsigned int region;   // WINDOW_FREE, WINDOW_CENTRED
        unsigned int display;  // which output a region is measured against
        unsigned int style;
        unsigned int edge;     // corner radius
        unsigned int sequence; // bump after drawing

        // The compositor writes these.
        unsigned int state;
        unsigned int pitch;      // pixels per row of the contents
        unsigned int max_width;  // what the mapping actually holds
        unsigned int max_height;

        // window_open's own bookkeeping. The compositor never reads it.
        unsigned int handle;

        unsigned int reserved[3];
};

// _IOW('s', 4, struct window_request)
#define WINDOW_IOCTL_CREATE 0x40087304u

// _IO('s', 5) -- redraw what changed
#define WINDOW_IOCTL_COMMIT 0x00007305u

struct window_request
{
        unsigned int width;
        unsigned int height;
};

static inline unsigned int *window_pixels(struct window *window)
{
        return (unsigned int *)((char *)window + WINDOW_PIXELS);
}

#ifndef KERNEL_MODE

#if defined(__x86_64__)
#define WINDOW_SYS_OPENAT 257
#define WINDOW_SYS_MMAP 9
#define WINDOW_SYS_MUNMAP 11
#define WINDOW_SYS_IOCTL 16
#define WINDOW_SYS_CLOSE 3

static long window_call(long number, long a, long b, long c, long d, long e, long f)
{
        register long r10 __asm__("r10") = d;
        register long r8 __asm__("r8") = e;
        register long r9 __asm__("r9") = f;
        long result;

        __asm__ volatile("syscall"
                         : "=a"(result)
                         : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
        return result;
}
#else
// arm64 and riscv share the asm-generic numbering.
#define WINDOW_SYS_OPENAT 56
#define WINDOW_SYS_MMAP 222
#define WINDOW_SYS_MUNMAP 215
#define WINDOW_SYS_IOCTL 29
#define WINDOW_SYS_CLOSE 57

#if defined(__aarch64__)
static long window_call(long number, long a, long b, long c, long d, long e, long f)
{
        register long x8 __asm__("x8") = number;
        register long x0 __asm__("x0") = a;
        register long x1 __asm__("x1") = b;
        register long x2 __asm__("x2") = c;
        register long x3 __asm__("x3") = d;
        register long x4 __asm__("x4") = e;
        register long x5 __asm__("x5") = f;

        __asm__ volatile("svc #0"
                         : "+r"(x0)
                         : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                         : "memory");
        return x0;
}
#else
static long window_call(long number, long a, long b, long c, long d, long e, long f)
{
        register long a7 __asm__("a7") = number;
        register long a0 __asm__("a0") = a;
        register long a1 __asm__("a1") = b;
        register long a2 __asm__("a2") = c;
        register long a3 __asm__("a3") = d;
        register long a4 __asm__("a4") = e;
        register long a5 __asm__("a5") = f;

        __asm__ volatile("ecall"
                         : "+r"(a0)
                         : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                         : "memory");
        return a0;
}
#endif
#endif

static unsigned long window_bytes(struct window *window)
{
        return WINDOW_PIXELS +
               (unsigned long)window->pitch * window->max_height * 4;
}

/*
        One file per window: the ioctl sizes it and the mapping is at offset
        zero, so there is no offset for either side to get wrong, and closing
        the file is what destroys the window.
*/
static struct window *window_open(unsigned int width, unsigned int height)
{
        struct window_request request = {width, height};
        struct window *window;
        long file, mapped;

        file = window_call(WINDOW_SYS_OPENAT, -100, (long)WINDOW_DEVICE, 2, 0, 0, 0);
        if (file < 0)
                return 0;

        if (window_call(WINDOW_SYS_IOCTL, file, WINDOW_IOCTL_CREATE, (long)&request, 0, 0, 0) < 0)
        {
                window_call(WINDOW_SYS_CLOSE, file, 0, 0, 0, 0, 0);
                return 0;
        }

        // PROT_READ | PROT_WRITE, MAP_SHARED
        mapped = window_call(WINDOW_SYS_MMAP, 0,
                             WINDOW_PIXELS + (unsigned long)width * height * 4,
                             3, 1, file, 0);
        if (mapped < 0 && mapped > -4096)
        {
                window_call(WINDOW_SYS_CLOSE, file, 0, 0, 0, 0, 0);
                return 0;
        }

        window = (struct window *)mapped;
        window->handle = (unsigned int)file;

        return window;
}

// Everything except the first frame after the compositor has gone to sleep is
// a store to the shared page; this is what wakes it.
static void window_commit(struct window *window)
{
        window->sequence++;
        window_call(WINDOW_SYS_IOCTL, window->handle, WINDOW_IOCTL_COMMIT, 0, 0, 0, 0);
}

static void window_close(struct window *window)
{
        long file = window->handle;

        window_call(WINDOW_SYS_MUNMAP, (long)window, window_bytes(window), 0, 0, 0, 0);
        window_call(WINDOW_SYS_CLOSE, file, 0, 0, 0, 0, 0);
}

#endif // KERNEL_MODE
#endif // WINDOW_INCLUDED
