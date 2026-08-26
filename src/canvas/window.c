/*
        Moonwater window

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

        style is what the window looks like: whether it wears a frame, whether
        it covers its display, whether it is put away. A new window starts
        framed.

        edge is the corner radius, in pixels. Zero is square.

        awake says whether the compositor is currently watching the shared
        pages. window_commit only makes a call when it is not, so a program
        drawing every frame makes none at all. The first commit after opening a
        window is always a call, so whatever the compositor decides -- where a
        region put the window, how large its display is -- is there to read as
        soon as it returns.
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

// style
#define WINDOW_FRAME 1u
#define WINDOW_FULLSCREEN 2u
#define WINDOW_MINIMIZED 4u

/*
        A window takes the pointer over the whole of itself, not only over the
        parts that do something with it: hovering its middle must not offer to
        resize whatever is buried underneath. WINDOW_PASSTHROUGH says not to,
        for a window that is meant to be looked through rather than used.
*/
#define WINDOW_PASSTHROUGH 8u

// state
#define WINDOW_FOCUSED 1u

/*
        Text layout. Alignment is one field: a horizontal one, a vertical one,
        and whether long text wraps at the width of its box or runs off the end
        of one line.
*/
#define TEXT_LEFT 0u
#define TEXT_CENTRE 1u
#define TEXT_RIGHT 2u
#define TEXT_TOP 0u
#define TEXT_MIDDLE 4u
#define TEXT_BOTTOM 8u
#define TEXT_WRAP 16u

/*
        Keys reach the window that has focus, through a ring in this page.

        The compositor writes at head and the program reads at tail, so a
        program that draws every frame reads its input the same way it reads
        everything else: no call. A ring that fills drops what is oldest,
        which is what a keyboard buffer does.
*/
#define WINDOW_KEYS 64

#define WINDOW_KEY_DOWN 1u
#define WINDOW_KEY_SHIFT 2u
#define WINDOW_KEY_CONTROL 4u
#define WINDOW_KEY_ALT 8u

struct window_key
{
        unsigned int code;      // the key itself, as Linux numbers them
        unsigned int character; // what it means, or zero for a key that means
                                // nothing on its own
        unsigned int flags;
        unsigned int reserved;
};

// A window is never resized below this, whatever is dragged.
#define WINDOW_MIN_WIDTH 96
#define WINDOW_MIN_HEIGHT 48

// How long a title can be, which is the one string that lives in this page
// rather than in a window's own content.
#define WINDOW_TITLE_MAX 128

struct window
{
        // The program writes these, and the compositor writes x, y and z back
        // when it is the one that moved them.
        int x, y, z; // higher is in front
        unsigned int width, height;
        unsigned int region;  // WINDOW_FREE, WINDOW_CENTRED
        unsigned int display; // which output a region is measured against
        unsigned int style;
        unsigned int edge;     // corner radius
        unsigned int sequence; // bump after drawing
        char title[WINDOW_TITLE_MAX];

        // The compositor writes these.
        unsigned int state;         // WINDOW_FOCUSED
        unsigned int pitch;         // pixels per row of the contents
        unsigned int max_width;     // what the mapping actually holds
        unsigned int max_height;
        unsigned int display_width; // the output this window is on
        unsigned int display_height;
        unsigned int awake;         // whether the compositor is watching

        // window_open's own bookkeeping. The compositor never reads it.
        unsigned int handle;

        unsigned int reserved[4];

        // The compositor writes head, the program writes tail.
        unsigned int key_head;
        unsigned int key_tail;
        struct window_key keys[WINDOW_KEYS];

        // For a window of cells: its shape, and which rows have changed since
        // the compositor last looked. Rows rather than a rectangle because
        // that is what text changes.
        unsigned int columns;
        unsigned int rows;
        unsigned int damage_row;
        unsigned int damage_rows;
};

static inline void window_damage(struct window *window, unsigned int row,
                                 unsigned int rows)
{
        unsigned int last = window->damage_rows ? window->damage_row + window->damage_rows
                                                : row;

        if (!window->damage_rows || row < window->damage_row)
                window->damage_row = row;

        if (row + rows > last)
                last = row + rows;

        window->damage_rows = last - window->damage_row;
}

// _IOW('s', 4, struct window_request)
#define WINDOW_IOCTL_CREATE 0x40107304u

// _IO('s', 5) -- redraw what changed
#define WINDOW_IOCTL_COMMIT 0x00007305u

struct window_request
{
        unsigned int width;
        unsigned int height;
        unsigned int columns; // non-zero asks for cells rather than pixels,
        unsigned int rows;    // and decides the size
};

/*
        A cell, for a window made of text.

        A program that draws with words writes eight bytes a character and the
        compositor draws the glyph, rather than the program writing the five
        hundred and twelve bytes of an eight by sixteen cell itself with a font
        of its own. It is also what makes scrolling a memmove of cells instead
        of a copy of a framebuffer.

        The colours are indices into the sixteen a terminal has always had.
*/
struct window_cell
{
        unsigned int character;
        unsigned char ink;
        unsigned char paper;
        unsigned short flags;
};

#define WINDOW_CELL_W 8
#define WINDOW_CELL_H 16

static inline struct window_cell *window_cells(struct window *window)
{
        return (struct window_cell *)((char *)window + WINDOW_PIXELS);
}

// The next key, or false when there is none waiting.
static inline int window_key(struct window *window, struct window_key *key)
{
        if (window->key_tail == window->key_head)
                return 0;

        *key = window->keys[window->key_tail % WINDOW_KEYS];
        window->key_tail++;

        return 1;
}

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
        if (window->columns)
                return WINDOW_PIXELS + (unsigned long)window->columns *
                                           window->rows * sizeof(struct window_cell);

        return WINDOW_PIXELS +
               (unsigned long)window->pitch * window->max_height * 4;
}

/*
        One file per window: the ioctl sizes it and the mapping is at offset
        zero, so there is no offset for either side to get wrong, and closing
        the file is what destroys the window.
*/
static struct window *window_request_open(struct window_request request)
{
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
                             request.columns
                                 ? WINDOW_PIXELS + (unsigned long)request.columns *
                                                       request.rows *
                                                       sizeof(struct window_cell)
                                 : WINDOW_PIXELS + (unsigned long)request.width *
                                                       request.height * 4,
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

static struct window *window_open(unsigned int width, unsigned int height)
{
        struct window_request request = {width, height, 0, 0};

        return window_request_open(request);
}

// A window of text. The compositor decides how large that is in pixels.
static struct window *window_open_text(unsigned int columns, unsigned int rows)
{
        struct window_request request = {0, 0, columns, rows};

        return window_request_open(request);
}

/*
        Bumping sequence is what says a window changed. While the compositor is
        awake it is already reading that, so there is nothing else to do; the
        call below only happens on the first change after it went to sleep.
*/
static void window_commit(struct window *window)
{
        window->sequence++;

        if (!window->awake)
                window_call(WINDOW_SYS_IOCTL, window->handle, WINDOW_IOCTL_COMMIT, 0, 0, 0, 0);
}

/*
        The same, but now rather than within a frame.

        window_commit leaves it to the compositor's own timer while that is
        running, which is right for something animating and wrong for
        something echoing a keystroke: the letter would wait up to a frame for
        a clock it has no reason to be on. This one always makes the call.
*/
static void window_flush(struct window *window)
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
