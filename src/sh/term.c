/*
        The terminal.

        The pty and the escape sequences are here; the glyphs are not. The
        program writes characters and colour indices into cells and Canvas
        draws them, so there is no font in this file and no framebuffer.

        The cells are the ring every window of cells has. The rows are the last
        of it, so scrolling is a store to one number rather than a copy of the
        screen, what goes off the top is still there for the wheel to come back
        to, and the compositor draws the bar down the side -- none of which is
        anything this file does, and all of which it now has.

        Nothing below makes a system call. The emulator is a byte stream in and
        a grid of cells out, so it runs the same whether a pty is driving it or
        a test is, and the term lane of test/run drives it with no kernel underneath at
        all. The two functions at the bottom are the exception and they are the
        only ones: everything the parser, the keys and the line editor do is
        stores into a page.
*/

#define COLUMNS_WANTED 80
#define ROWS_WANTED 24
#define SHELL "/shell"

#ifndef EINTR
#define EINTR 4
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif

// Three below are the shell's, and the kernel's console has no use for them.
// Not the attribute that keeps a symbol: this one lets the compiler drop what
// it finds no call to, and only quiets the warning about having found none.
#define SPARE __attribute__((unused))
// The kernel provides its own ioctl constants and window-size type.
#ifndef KERNEL_MODE
#define TIOCSWINSZ 0x5414u

typedef struct
{
        unsigned short rows, columns, x_pixels, y_pixels;
} winsize;
#endif

static struct window *window;

// However many the compositor says there are, which changes when the window
// is resized.
static unsigned int COLUMNS, ROWS;
static unsigned int row, column;
static unsigned char ink = 7, paper = 0;
static unsigned short style;
static b32 reverse;
static unsigned int last_character;
static unsigned int touched_top, touched_bottom;

/*
        The rows a scroll happens between, and the rest of what a mode is.

        DECSTBM names a top and a bottom and everything that scrolls scrolls
        between them. When they are the whole screen -- which is what they are
        until something says otherwise -- moving on is the ring's own store;
        anywhere narrower it is a copy, because a region is by definition the
        screen not moving as one.
*/
static unsigned int region_top, region_bottom;
static b32 autowrap = true;
static b32 insert_mode;
static b32 cursor_visible = true;
static b32 application_keys;
/* DEC private mode 2026: programs may update a complete frame without the
   compositor presenting the intermediate rows. The cells are still written
   immediately; only publication is held by screen.c until the mode ends. */
static b32 synchronized_output;
static unsigned int mouse_mode;
static b32 mouse_sgr;
static b32 focus_events;
static b32 origin_mode;

#define CHARSET_ASCII 0
#define CHARSET_ACS 1
static unsigned char charset_g0;
static unsigned char charset_g1;
static unsigned char charset_gl;
static p8 escape_kind;
static p8 csi_intermediate;

// Where the block cursor was put, so it can be taken back off.
static unsigned int shown_row, shown_column;
static b32 shown;

/* What the cursor is, as DECSCUSR numbers it, and what showing it did to its
   cell: turned the colours round for a block, or added these marks. */
static unsigned int cursor_shape;
static b32 shown_block;
static unsigned short shown_marks;

/* A cursor with the colours it writes in: one saved by ESC 7 and CSI s, one
   kept for the primary screen while the alternate one is up. They are two, or
   a program saving its own cursor lost the shell's when it ended. */
struct cursor_state
{
        unsigned int row, column;
        unsigned char ink, paper;
        unsigned short style;
        b32 reverse;
        unsigned char charset_g0, charset_g1, charset_gl;
        b32 origin_mode;
};

static struct cursor_state cursor_saved = {0, 0, 7, 0, 0, false};
static struct cursor_state cursor_primary = {0, 0, 7, 0, 0, false};

// Tab stops, one byte a column, which is what makes HTS and TBC mean
// anything. Wide enough for a column of pixels on any screen this runs on.
#define TAB_STOPS 1024
static p8 tab_stop[TAB_STOPS] __attribute__((aligned(8)));

static fn touch(unsigned int at)
{
        if (at < touched_top)
                touched_top = at;

        if (at + 1 > touched_bottom)
                touched_bottom = at + 1;
}

static fn touch_all()
{
        touched_top = 0;
        touched_bottom = ROWS;
}

/*
        A row, in the ring.

        The screen is the last ROWS lines of it and ROWS is this program's own
        number, not one read back out of the shared page: the two differ for as
        long as a resize takes to be answered, and a row addressed at the
        compositor's count while the cells are still laid out at this one is a
        line taken from somewhere else entirely.

        Which line a row is, is a remainder by the ring's length, and that
        length is read out of the shared page, so every lookup is a division.
        A character paid four of them, and a scrolled region two a row. The
        paths that touch a row more than once find its slot once and hand the
        slot down.
*/
static inline INLINE unsigned int row_slot(unsigned int r)
{
        return (window->head - ROWS + r) % window->history;
}

// The slot count lines further on, without dividing again while that is
// less than a lap of the ring past the first.
static inline INLINE unsigned int slot_after(unsigned int slot, unsigned int count)
{
        unsigned int after = slot + count;

        if (after < window->history)
                return after;

        after -= window->history;
        return after < window->history ? after : after % window->history;
}

#define slot_cells(slot) (window_cells(window) + (slot) * window->stride)
#define slot_length(slot) (window_lengths(window) + (slot))
#define row_cells(row) slot_cells(row_slot(row))
#define row_length(row) slot_length(row_slot(row))

/*
        A cell is eight bytes and the ring begins 4096 bytes into its
        page-aligned mapping.  Every row therefore satisfies the aligned-u64
        fill contract, including on RV64 implementations that trap unaligned
        doubleword stores.
*/
#define BLANK_CELL_WORD ((positive)' ' | ((positive)7 << 32))

/*
        Bold draws the first eight inks in their bright spellings. It is done
        when a cell is written rather than to the ink itself, so SGR 22 takes
        the bold off without taking a bright colour chosen as one with it.
*/
static PURE unsigned char ink_drawn()
{
        return (style & WINDOW_CELL_BOLD) && ink < 8 ? (unsigned char)(ink | 8) : ink;
}

/*
        What an erase leaves: a blank in the background in force, and only
        that. Reverse video and the ink are what text is written in, and an
        erase that took them painted the line white.
*/
static PURE positive blank_cell_word()
{
        return BLANK_CELL_WORD | (positive)paper << 40;
}

/* Erase in the colours in force, so a program can clear a coloured panel. */
static fn cells_clear(unsigned int r, unsigned int first, unsigned int count)
{
        if (!count)
                return;

        memory_fill_u64_aligned(row_cells(r) + first, count,
                                blank_cell_word());
        touch(r);
}

/* Cells never written, which are blank in the colours a reset gives them:
   the colours in force belong to what an erase clears, not to what a tab or
   a cursor movement steps over. */
static fn cells_blank(unsigned int r, unsigned int first, unsigned int count)
{
        if (!count)
                return;

        memory_fill_u64_aligned(row_cells(r) + first, count, BLANK_CELL_WORD);
        touch(r);
}

/*
        Nothing moves.

        The screen ends at the newest line, so the next one is the whole of
        it, and the line that was at the top is still in the ring for the wheel
        to come back to. This used to be a copy of every cell on the screen
        with the top row thrown away.
*/
static fn ring_scroll()
{
        window_scroll(window);
        touch_all();
}

static fn slot_copy(unsigned int to, unsigned int from)
{
        struct window_cell address_to source = slot_cells(from);
        struct window_cell address_to target = slot_cells(to);
        unsigned int length = address_to slot_length(from);

        memory_copy(target, source,
                    (positive)length * sizeof(struct window_cell));

        address_to slot_length(to) = length;
}

static fn row_blank(unsigned int r)
{
        // A line given out blank takes the background in force, the way an
        // erase does: nano scrolls and inserts its blue title bar's rows.
        if (paper)
        {
                cells_clear(r, 0, COLUMNS);
                address_to row_length(r) = COLUMNS;
                return;
        }

        address_to row_length(r) = 0;
        touch(r);
}

/*
        The region moves up, and the whole screen does it by not moving.

        Only when the region is the screen can the ring be named forward, and
        only then is what leaves the top kept. A narrower region is a copy and
        what leaves it is gone, which is what a region means.
*/
static fn scroll_up(unsigned int count)
{
        if (region_top == 0 && region_bottom == ROWS)
        {
                /* Canvas calls this with interrupts off. Beyond ROWS the
                   visible result is already blank, so never let a hostile
                   CSI count turn that critical section into a long loop. */
                if (count > ROWS)
                        count = ROWS;

                for (unsigned int n = count; n; n--)
                        ring_scroll();

                if (paper)
                        for (unsigned int r = ROWS - count; r < ROWS; r++)
                                row_blank(r);

                return;
        }

        if (count > region_bottom - region_top)
                count = region_bottom - region_top;

        unsigned int top = row_slot(0);

        for (unsigned int r = region_top; r + count < region_bottom; r++)
                slot_copy(slot_after(top, r), slot_after(top, r + count));

        for (unsigned int r = region_bottom - count; r < region_bottom; r++)
                row_blank(r);

        touch_all();
}

static fn scroll_down(unsigned int count)
{
        if (count > region_bottom - region_top)
                count = region_bottom - region_top;

        unsigned int top = row_slot(0);

        for (unsigned int r = region_bottom; r-- > region_top + count;)
                slot_copy(slot_after(top, r), slot_after(top, r - count));

        for (unsigned int r = region_top; r < region_top + count; r++)
                row_blank(r);

        touch_all();
}

// One line down, and the bottom of the region is where that scrolls.
static fn line_feed()
{
        if (row + 1 == region_bottom)
                scroll_up(1);
        else if (row + 1 < ROWS)
                row++;
}

/*
        Every cell up to a column, made to exist.

        Nothing past the length of a line is drawn and the cells out there are
        whatever the ring last held, so anything that arrives at a column
        without having written its way there -- a tab, a cursor moved along and
        printed at -- has to clear what it stepped over first.
*/
static fn reach(unsigned int r, unsigned int to)
{
        unsigned int address_to length = row_length(r);

        if (address_to length < to)
        {
                cells_blank(r, address_to length, to - address_to length);
                address_to length = to;
        }
}

static fn open_gap(unsigned int at, unsigned int count)
{
        unsigned int address_to length = row_length(row);
        struct window_cell address_to cells;
        unsigned int last;

        if (at >= COLUMNS || !count)
                return;

        if (count > COLUMNS - at)
                count = COLUMNS - at;

        reach(row, at);

        // As far as what was there is pushed to, and no further: a line is as
        // long as it was written and padding it to the width of the window is
        // what the ring exists not to do.
        last = address_to length + count;

        if (last > COLUMNS)
                last = COLUMNS;

        reach(row, last);
        cells = row_cells(row);

        memory_copy(cells + at + count, cells + at,
                    (positive)(last - at - count) *
                        sizeof(struct window_cell));

        cells_clear(row, at, min(count, last - at));

        touch(row);
}

/*
        How many columns a character takes.

        Programs count columns with wcwidth, and a terminal that disagrees puts
        everything after an ideograph, an emoji or a combining accent a column
        away from where the program meant it. The ranges are Unicode 16's: East
        Asian Wide and Fullwidth take two, marks and format characters take
        none, and everything below U+0300 takes one.
*/
struct character_range
{
        unsigned int first, last;
};

static const struct character_range character_zero[] = {
    {0x00300, 0x0036f}, {0x00483, 0x00489}, {0x00591, 0x005bd}, {0x005bf, 0x005bf},
    {0x005c1, 0x005c2}, {0x005c4, 0x005c5}, {0x005c7, 0x005c7}, {0x00600, 0x00605},
    {0x00610, 0x0061a}, {0x0061c, 0x0061c}, {0x0064b, 0x0065f}, {0x00670, 0x00670},
    {0x006d6, 0x006dd}, {0x006df, 0x006e4}, {0x006e7, 0x006e8}, {0x006ea, 0x006ed},
    {0x0070f, 0x0070f}, {0x00711, 0x00711}, {0x00730, 0x0074a}, {0x007a6, 0x007b0},
    {0x007eb, 0x007f3}, {0x007fd, 0x007fd}, {0x00816, 0x00819}, {0x0081b, 0x00823},
    {0x00825, 0x00827}, {0x00829, 0x0082d}, {0x00859, 0x0085b}, {0x00890, 0x00891},
    {0x00897, 0x0089f}, {0x008ca, 0x00902}, {0x0093a, 0x0093a}, {0x0093c, 0x0093c},
    {0x00941, 0x00948}, {0x0094d, 0x0094d}, {0x00951, 0x00957}, {0x00962, 0x00963},
    {0x00981, 0x00981}, {0x009bc, 0x009bc}, {0x009c1, 0x009c4}, {0x009cd, 0x009cd},
    {0x009e2, 0x009e3}, {0x009fe, 0x009fe}, {0x00a01, 0x00a02}, {0x00a3c, 0x00a3c},
    {0x00a41, 0x00a42}, {0x00a47, 0x00a48}, {0x00a4b, 0x00a4d}, {0x00a51, 0x00a51},
    {0x00a70, 0x00a71}, {0x00a75, 0x00a75}, {0x00a81, 0x00a82}, {0x00abc, 0x00abc},
    {0x00ac1, 0x00ac5}, {0x00ac7, 0x00ac8}, {0x00acd, 0x00acd}, {0x00ae2, 0x00ae3},
    {0x00afa, 0x00aff}, {0x00b01, 0x00b01}, {0x00b3c, 0x00b3c}, {0x00b3f, 0x00b3f},
    {0x00b41, 0x00b44}, {0x00b4d, 0x00b4d}, {0x00b55, 0x00b56}, {0x00b62, 0x00b63},
    {0x00b82, 0x00b82}, {0x00bc0, 0x00bc0}, {0x00bcd, 0x00bcd}, {0x00c00, 0x00c00},
    {0x00c04, 0x00c04}, {0x00c3c, 0x00c3c}, {0x00c3e, 0x00c40}, {0x00c46, 0x00c48},
    {0x00c4a, 0x00c4d}, {0x00c55, 0x00c56}, {0x00c62, 0x00c63}, {0x00c81, 0x00c81},
    {0x00cbc, 0x00cbc}, {0x00cbf, 0x00cbf}, {0x00cc6, 0x00cc6}, {0x00ccc, 0x00ccd},
    {0x00ce2, 0x00ce3}, {0x00d00, 0x00d01}, {0x00d3b, 0x00d3c}, {0x00d41, 0x00d44},
    {0x00d4d, 0x00d4d}, {0x00d62, 0x00d63}, {0x00d81, 0x00d81}, {0x00dca, 0x00dca},
    {0x00dd2, 0x00dd4}, {0x00dd6, 0x00dd6}, {0x00e31, 0x00e31}, {0x00e34, 0x00e3a},
    {0x00e47, 0x00e4e}, {0x00eb1, 0x00eb1}, {0x00eb4, 0x00ebc}, {0x00ec8, 0x00ece},
    {0x00f18, 0x00f19}, {0x00f35, 0x00f35}, {0x00f37, 0x00f37}, {0x00f39, 0x00f39},
    {0x00f71, 0x00f7e}, {0x00f80, 0x00f84}, {0x00f86, 0x00f87}, {0x00f8d, 0x00f97},
    {0x00f99, 0x00fbc}, {0x00fc6, 0x00fc6}, {0x0102d, 0x01030}, {0x01032, 0x01037},
    {0x01039, 0x0103a}, {0x0103d, 0x0103e}, {0x01058, 0x01059}, {0x0105e, 0x01060},
    {0x01071, 0x01074}, {0x01082, 0x01082}, {0x01085, 0x01086}, {0x0108d, 0x0108d},
    {0x0109d, 0x0109d}, {0x01160, 0x011ff}, {0x0135d, 0x0135f}, {0x01712, 0x01714},
    {0x01732, 0x01733}, {0x01752, 0x01753}, {0x01772, 0x01773}, {0x017b4, 0x017b5},
    {0x017b7, 0x017bd}, {0x017c6, 0x017c6}, {0x017c9, 0x017d3}, {0x017dd, 0x017dd},
    {0x0180b, 0x0180f}, {0x01885, 0x01886}, {0x018a9, 0x018a9}, {0x01920, 0x01922},
    {0x01927, 0x01928}, {0x01932, 0x01932}, {0x01939, 0x0193b}, {0x01a17, 0x01a18},
    {0x01a1b, 0x01a1b}, {0x01a56, 0x01a56}, {0x01a58, 0x01a5e}, {0x01a60, 0x01a60},
    {0x01a62, 0x01a62}, {0x01a65, 0x01a6c}, {0x01a73, 0x01a7c}, {0x01a7f, 0x01a7f},
    {0x01ab0, 0x01ace}, {0x01b00, 0x01b03}, {0x01b34, 0x01b34}, {0x01b36, 0x01b3a},
    {0x01b3c, 0x01b3c}, {0x01b42, 0x01b42}, {0x01b6b, 0x01b73}, {0x01b80, 0x01b81},
    {0x01ba2, 0x01ba5}, {0x01ba8, 0x01ba9}, {0x01bab, 0x01bad}, {0x01be6, 0x01be6},
    {0x01be8, 0x01be9}, {0x01bed, 0x01bed}, {0x01bef, 0x01bf1}, {0x01c2c, 0x01c33},
    {0x01c36, 0x01c37}, {0x01cd0, 0x01cd2}, {0x01cd4, 0x01ce0}, {0x01ce2, 0x01ce8},
    {0x01ced, 0x01ced}, {0x01cf4, 0x01cf4}, {0x01cf8, 0x01cf9}, {0x01dc0, 0x01dff},
    {0x0200b, 0x0200f}, {0x0202a, 0x0202e}, {0x02060, 0x02064}, {0x02066, 0x0206f},
    {0x020d0, 0x020f0}, {0x02cef, 0x02cf1}, {0x02d7f, 0x02d7f}, {0x02de0, 0x02dff},
    {0x0302a, 0x0302d}, {0x03099, 0x0309a}, {0x0a66f, 0x0a672}, {0x0a674, 0x0a67d},
    {0x0a69e, 0x0a69f}, {0x0a6f0, 0x0a6f1}, {0x0a802, 0x0a802}, {0x0a806, 0x0a806},
    {0x0a80b, 0x0a80b}, {0x0a825, 0x0a826}, {0x0a82c, 0x0a82c}, {0x0a8c4, 0x0a8c5},
    {0x0a8e0, 0x0a8f1}, {0x0a8ff, 0x0a8ff}, {0x0a926, 0x0a92d}, {0x0a947, 0x0a951},
    {0x0a980, 0x0a982}, {0x0a9b3, 0x0a9b3}, {0x0a9b6, 0x0a9b9}, {0x0a9bc, 0x0a9bd},
    {0x0a9e5, 0x0a9e5}, {0x0aa29, 0x0aa2e}, {0x0aa31, 0x0aa32}, {0x0aa35, 0x0aa36},
    {0x0aa43, 0x0aa43}, {0x0aa4c, 0x0aa4c}, {0x0aa7c, 0x0aa7c}, {0x0aab0, 0x0aab0},
    {0x0aab2, 0x0aab4}, {0x0aab7, 0x0aab8}, {0x0aabe, 0x0aabf}, {0x0aac1, 0x0aac1},
    {0x0aaec, 0x0aaed}, {0x0aaf6, 0x0aaf6}, {0x0abe5, 0x0abe5}, {0x0abe8, 0x0abe8},
    {0x0abed, 0x0abed}, {0x0fb1e, 0x0fb1e}, {0x0fe00, 0x0fe0f}, {0x0fe20, 0x0fe2f},
    {0x0feff, 0x0feff}, {0x0fff9, 0x0fffb}, {0x101fd, 0x101fd}, {0x102e0, 0x102e0},
    {0x10376, 0x1037a}, {0x10a01, 0x10a03}, {0x10a05, 0x10a06}, {0x10a0c, 0x10a0f},
    {0x10a38, 0x10a3a}, {0x10a3f, 0x10a3f}, {0x10ae5, 0x10ae6}, {0x10d24, 0x10d27},
    {0x10d69, 0x10d6d}, {0x10eab, 0x10eac}, {0x10efc, 0x10eff}, {0x10f46, 0x10f50},
    {0x10f82, 0x10f85}, {0x11001, 0x11001}, {0x11038, 0x11046}, {0x11070, 0x11070},
    {0x11073, 0x11074}, {0x1107f, 0x11081}, {0x110b3, 0x110b6}, {0x110b9, 0x110ba},
    {0x110bd, 0x110bd}, {0x110c2, 0x110c2}, {0x110cd, 0x110cd}, {0x11100, 0x11102},
    {0x11127, 0x1112b}, {0x1112d, 0x11134}, {0x11173, 0x11173}, {0x11180, 0x11181},
    {0x111b6, 0x111be}, {0x111c9, 0x111cc}, {0x111cf, 0x111cf}, {0x1122f, 0x11231},
    {0x11234, 0x11234}, {0x11236, 0x11237}, {0x1123e, 0x1123e}, {0x11241, 0x11241},
    {0x112df, 0x112df}, {0x112e3, 0x112ea}, {0x11300, 0x11301}, {0x1133b, 0x1133c},
    {0x11340, 0x11340}, {0x11366, 0x1136c}, {0x11370, 0x11374}, {0x113bb, 0x113c0},
    {0x113ce, 0x113ce}, {0x113d0, 0x113d0}, {0x113d2, 0x113d2}, {0x113e1, 0x113e2},
    {0x11438, 0x1143f}, {0x11442, 0x11444}, {0x11446, 0x11446}, {0x1145e, 0x1145e},
    {0x114b3, 0x114b8}, {0x114ba, 0x114ba}, {0x114bf, 0x114c0}, {0x114c2, 0x114c3},
    {0x115b2, 0x115b5}, {0x115bc, 0x115bd}, {0x115bf, 0x115c0}, {0x115dc, 0x115dd},
    {0x11633, 0x1163a}, {0x1163d, 0x1163d}, {0x1163f, 0x11640}, {0x116ab, 0x116ab},
    {0x116ad, 0x116ad}, {0x116b0, 0x116b5}, {0x116b7, 0x116b7}, {0x1171d, 0x1171d},
    {0x1171f, 0x1171f}, {0x11722, 0x11725}, {0x11727, 0x1172b}, {0x1182f, 0x11837},
    {0x11839, 0x1183a}, {0x1193b, 0x1193c}, {0x1193e, 0x1193e}, {0x11943, 0x11943},
    {0x119d4, 0x119d7}, {0x119da, 0x119db}, {0x119e0, 0x119e0}, {0x11a01, 0x11a0a},
    {0x11a33, 0x11a38}, {0x11a3b, 0x11a3e}, {0x11a47, 0x11a47}, {0x11a51, 0x11a56},
    {0x11a59, 0x11a5b}, {0x11a8a, 0x11a96}, {0x11a98, 0x11a99}, {0x11c30, 0x11c36},
    {0x11c38, 0x11c3d}, {0x11c3f, 0x11c3f}, {0x11c92, 0x11ca7}, {0x11caa, 0x11cb0},
    {0x11cb2, 0x11cb3}, {0x11cb5, 0x11cb6}, {0x11d31, 0x11d36}, {0x11d3a, 0x11d3a},
    {0x11d3c, 0x11d3d}, {0x11d3f, 0x11d45}, {0x11d47, 0x11d47}, {0x11d90, 0x11d91},
    {0x11d95, 0x11d95}, {0x11d97, 0x11d97}, {0x11ef3, 0x11ef4}, {0x11f00, 0x11f01},
    {0x11f36, 0x11f3a}, {0x11f40, 0x11f40}, {0x11f42, 0x11f42}, {0x11f5a, 0x11f5a},
    {0x13430, 0x13440}, {0x13447, 0x13455}, {0x1611e, 0x16129}, {0x1612d, 0x1612f},
    {0x16af0, 0x16af4}, {0x16b30, 0x16b36}, {0x16f4f, 0x16f4f}, {0x16f8f, 0x16f92},
    {0x16fe4, 0x16fe4}, {0x1bc9d, 0x1bc9e}, {0x1bca0, 0x1bca3}, {0x1cf00, 0x1cf2d},
    {0x1cf30, 0x1cf46}, {0x1d167, 0x1d169}, {0x1d173, 0x1d182}, {0x1d185, 0x1d18b},
    {0x1d1aa, 0x1d1ad}, {0x1d242, 0x1d244}, {0x1da00, 0x1da36}, {0x1da3b, 0x1da6c},
    {0x1da75, 0x1da75}, {0x1da84, 0x1da84}, {0x1da9b, 0x1da9f}, {0x1daa1, 0x1daaf},
    {0x1e000, 0x1e006}, {0x1e008, 0x1e018}, {0x1e01b, 0x1e021}, {0x1e023, 0x1e024},
    {0x1e026, 0x1e02a}, {0x1e08f, 0x1e08f}, {0x1e130, 0x1e136}, {0x1e2ae, 0x1e2ae},
    {0x1e2ec, 0x1e2ef}, {0x1e4ec, 0x1e4ef}, {0x1e5ee, 0x1e5ef}, {0x1e8d0, 0x1e8d6},
    {0x1e944, 0x1e94a}, {0xe0001, 0xe0001}, {0xe0020, 0xe007f}, {0xe0100, 0xe01ef},
};
static const struct character_range character_wide[] = {
    {0x01100, 0x0115f}, {0x0231a, 0x0231b}, {0x02329, 0x0232a}, {0x023e9, 0x023ec},
    {0x023f0, 0x023f0}, {0x023f3, 0x023f3}, {0x025fd, 0x025fe}, {0x02614, 0x02615},
    {0x02630, 0x02637}, {0x02648, 0x02653}, {0x0267f, 0x0267f}, {0x0268a, 0x0268f},
    {0x02693, 0x02693}, {0x026a1, 0x026a1}, {0x026aa, 0x026ab}, {0x026bd, 0x026be},
    {0x026c4, 0x026c5}, {0x026ce, 0x026ce}, {0x026d4, 0x026d4}, {0x026ea, 0x026ea},
    {0x026f2, 0x026f3}, {0x026f5, 0x026f5}, {0x026fa, 0x026fa}, {0x026fd, 0x026fd},
    {0x02705, 0x02705}, {0x0270a, 0x0270b}, {0x02728, 0x02728}, {0x0274c, 0x0274c},
    {0x0274e, 0x0274e}, {0x02753, 0x02755}, {0x02757, 0x02757}, {0x02795, 0x02797},
    {0x027b0, 0x027b0}, {0x027bf, 0x027bf}, {0x02b1b, 0x02b1c}, {0x02b50, 0x02b50},
    {0x02b55, 0x02b55}, {0x02e80, 0x02e99}, {0x02e9b, 0x02ef3}, {0x02f00, 0x02fd5},
    {0x02ff0, 0x03029}, {0x0302e, 0x0303e}, {0x03041, 0x03096}, {0x0309b, 0x030ff},
    {0x03105, 0x0312f}, {0x03131, 0x0318e}, {0x03190, 0x031e5}, {0x031ef, 0x0321e},
    {0x03220, 0x03247}, {0x03250, 0x0a48c}, {0x0a490, 0x0a4c6}, {0x0a960, 0x0a97c},
    {0x0ac00, 0x0d7a3}, {0x0f900, 0x0faff}, {0x0fe10, 0x0fe19}, {0x0fe30, 0x0fe52},
    {0x0fe54, 0x0fe66}, {0x0fe68, 0x0fe6b}, {0x0ff01, 0x0ff60}, {0x0ffe0, 0x0ffe6},
    {0x16fe0, 0x16fe3}, {0x16ff0, 0x16ff1}, {0x17000, 0x187f7}, {0x18800, 0x18cd5},
    {0x18cff, 0x18d08}, {0x1aff0, 0x1aff3}, {0x1aff5, 0x1affb}, {0x1affd, 0x1affe},
    {0x1b000, 0x1b122}, {0x1b132, 0x1b132}, {0x1b150, 0x1b152}, {0x1b155, 0x1b155},
    {0x1b164, 0x1b167}, {0x1b170, 0x1b2fb}, {0x1d300, 0x1d356}, {0x1d360, 0x1d376},
    {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a},
    {0x1f200, 0x1f202}, {0x1f210, 0x1f23b}, {0x1f240, 0x1f248}, {0x1f250, 0x1f251},
    {0x1f260, 0x1f265}, {0x1f300, 0x1f320}, {0x1f32d, 0x1f335}, {0x1f337, 0x1f37c},
    {0x1f37e, 0x1f393}, {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3}, {0x1f3e0, 0x1f3f0},
    {0x1f3f4, 0x1f3f4}, {0x1f3f8, 0x1f43e}, {0x1f440, 0x1f440}, {0x1f442, 0x1f4fc},
    {0x1f4ff, 0x1f53d}, {0x1f54b, 0x1f54e}, {0x1f550, 0x1f567}, {0x1f57a, 0x1f57a},
    {0x1f595, 0x1f596}, {0x1f5a4, 0x1f5a4}, {0x1f5fb, 0x1f64f}, {0x1f680, 0x1f6c5},
    {0x1f6cc, 0x1f6cc}, {0x1f6d0, 0x1f6d2}, {0x1f6d5, 0x1f6d7}, {0x1f6dc, 0x1f6df},
    {0x1f6eb, 0x1f6ec}, {0x1f6f4, 0x1f6fc}, {0x1f7e0, 0x1f7eb}, {0x1f7f0, 0x1f7f0},
    {0x1f90c, 0x1f93a}, {0x1f93c, 0x1f945}, {0x1f947, 0x1f9ff}, {0x1fa70, 0x1fa7c},
    {0x1fa80, 0x1fa89}, {0x1fa8f, 0x1fac6}, {0x1face, 0x1fadc}, {0x1fadf, 0x1fae9},
    {0x1faf0, 0x1faf8}, {0x20000, 0x2fffd}, {0x30000, 0x3fffd},
};

static PURE b32 character_in(const struct character_range address_to ranges,
                             positive count, unsigned int c)
{
        positive low = 0, high = count;

        while (low < high)
        {
                positive middle = (low + high) / 2;

                if (ranges[middle].last < c)
                        low = middle + 1;
                else
                        high = middle;
        }

        return low < count && ranges[low].first <= c;
}

static PURE unsigned int character_width(unsigned int c)
{
        if (c < 0x300)
                return 1;

        /*
                The CJK ideographs and the Hangul syllables are most of the
                wide characters a terminal is sent, and neither block holds a
                mark, so they are answered before the two searches -- behind
                one compare, so box drawing and every script below them pay
                no more than that. The term lane checks every character
                against the tables alone.
        */
        if (c >= 0x3250 && (c <= 0xa48c || c - 0xac00u <= 0xd7a3 - 0xac00))
                return 2;

        if (character_in(character_zero, array_count(character_zero), c))
                return 0;

        return character_in(character_wide, array_count(character_wide), c)
                   ? 2
                   : 1;
}

/*
        A cell about to be written may be half of a double-width character,
        and the other half cannot stand alone: it becomes a blank.
*/
static fn unpair(struct window_cell address_to cells, unsigned int length,
                 unsigned int at)
{
        if (at >= length)
                return;

        if ((cells[at].flags & WINDOW_CELL_WIDE) && at + 1 < length)
        {
                cells[at + 1].character = ' ';
                cells[at + 1].flags &= (unsigned short)~WINDOW_CELL_WIDE_RIGHT;
        }

        if ((cells[at].flags & WINDOW_CELL_WIDE_RIGHT) && at)
        {
                cells[at - 1].character = ' ';
                cells[at - 1].flags &= (unsigned short)~WINDOW_CELL_WIDE;
        }
}

static fn put_cells(unsigned int character, unsigned int width)
{
        struct window_cell address_to cells;
        struct window_cell address_to cell;
        unsigned int address_to length;
        unsigned int slot;

        if (width > COLUMNS)
                width = 1;

        // A character wider than what is left of the line wraps whole and
        // leaves the last cell blank, as xterm does.
        if (column + width > COLUMNS)
        {
                // DECAWM off pins the cursor to the last column and every
                // further character overwrites it, which is what stops a
                // status line from scrolling the screen it is drawn on.
                if (!autowrap)
                        column = COLUMNS - width;
                else
                {
                        column = 0;
                        line_feed();
                }
        }

        // The row's slot, once: nothing below moves head.
        slot = row_slot(row);
        cells = slot_cells(slot);
        length = slot_length(slot);

        if (address_to length < column)
        {
                cells_blank(row, address_to length, column - address_to length);
                address_to length = column;
        }

        if (insert_mode)
                open_gap(column, width);

        unpair(cells, address_to length, column);
        if (width == 2)
                unpair(cells, address_to length, column + 1);

        cell = cells + column;
        cell->character = character;
        cell->ink = reverse ? paper : ink_drawn();
        cell->paper = reverse ? ink_drawn() : paper;
        cell->flags = style;

        if (width == 2)
        {
                cell->flags |= WINDOW_CELL_WIDE;
                cell[1] = cell[0];
                cell[1].flags = (unsigned short)(style | WINDOW_CELL_WIDE_RIGHT);
        }

        last_character = character;

        touch(row);
        column += width;

        if (address_to length < column)
                address_to length = column;
}

// A character in the columns it takes. A mark that belongs over the character
// before it takes none and is dropped: the face cannot draw it there.
static fn put(unsigned int character)
{
        unsigned int width = character_width(character);

        if (width)
                put_cells(character, width);
}

/*
        What goes back up the pty.

        A terminal answers questions -- where the cursor is, what it claims to
        be -- and a key is bytes rather than a call, so both land here and the
        one function in this file that has a file descriptor sends them. The
        emulator itself still makes no system call.
*/
#ifdef KERNEL_MODE
#define TO_SHELL_MAX 2048
static p8 to_shell[TO_SHELL_MAX];
#else
static p8 address_to to_shell;
static positive to_shell_room;
#endif
static positive to_shell_length;

static fn emit_bytes(address_any data, positive length)
{
#ifdef KERNEL_MODE
        positive room = TO_SHELL_MAX - to_shell_length;
        positive take = length < room ? length : room;

        if (take)
                memory_copy_apart(to_shell + to_shell_length, data, take);

        to_shell_length += take;
#else
        if (!length ||
            !array_store_reserve(to_shell, to_shell_room, to_shell_length,
                                 to_shell_length + length, 64))
                return;

        memory_copy_apart(to_shell + to_shell_length, data, length);
        to_shell_length += length;
#endif
}

static fn emit(unsigned int byte)
{
        p8 one = (p8)byte;

        emit_bytes(address_of one, 1);
}

#define emit_literal(text) \
        emit_bytes((address_any)(text), sizeof(text) - 1)

/* One CSI parameter machine serves both terminal output and editor input.
   The final byte is deliberately not stored: it is the caller's action, while
   digits, separators and the private marker are the reusable transition. */
#define TERMINAL_PARAMETERS 32
typedef struct
{
        unsigned int value[TERMINAL_PARAMETERS];
        unsigned int count;
        unsigned int colon; // bit n: value[n] was joined to the one before by ':'
        p8 marker;
        p8 full;            // a separator past the last slot: the rest is dropped
} terminal_parameters;

#define terminal_parameters_reset(sequence)                                 \
        ((sequence)->count = 0, (sequence)->marker = 0,                     \
         (sequence)->value[0] = 0, (sequence)->colon = 0,                   \
         (sequence)->full = 0)

static inline INLINE bool terminal_parameters_take(
    terminal_parameters address_to sequence, unsigned int byte)
{
        if (byte >= '0' && byte <= '9')
        {
                unsigned int digit = byte - '0';
                unsigned int address_to value;

                if (sequence->full)
                        return false;

                if (!sequence->count)
                        sequence->count = 1;

                value = address_of sequence->value[sequence->count - 1];

                // CSI parameters have no protocol-level machine-word limit.
                // Saturation preserves the only useful meaning of a larger
                // value -- past the edge -- instead of wrapping it to a small
                // cursor position or erase count.
                if (address_to value > (~0u - digit) / 10)
                        address_to value = ~0u;
                else
                        address_to value = address_to value * 10 + digit;

                return false;
        }

        if (byte == ';' || byte == ':')
        {
                if (!sequence->count)
                        sequence->count = 1;

                // Past the last slot the rest are dropped, where they used
                // to run their digits on into the last one's.
                if (sequence->count == TERMINAL_PARAMETERS)
                        sequence->full = 1;
                else if (!sequence->full)
                {
                        if (byte == ':')
                                sequence->colon |= 1u << sequence->count;
                        sequence->value[sequence->count++] = 0;
                }

                return false;
        }

        if (byte == '?' || byte == '<' || byte == '=' || byte == '>')
        {
                sequence->marker = (p8)byte;
                return false;
        }

        return byte >= '@' && byte <= '~';
}

static terminal_parameters terminal_csi;
static b32 in_escape, in_csi, in_string, escape_intermediate;

// A string sequence ends at ST, and ST is two bytes with an ESC in front.
static b32 string_escape;
static p8 string_kind;
static p8 osc_bytes[WINDOW_TITLE_MAX];
static unsigned int osc_length;

static CONST unsigned int acs_character(unsigned int c)
{
        static const unsigned int map[] = {
            0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
            0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
            0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
            0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
        };

        // The arrows and the block were never the VT100's, but ncurses's acsc
        // asks for them by these letters, and xterm and tmux draw them.
        static const unsigned int arrows[] = {0x2192, 0x2190, 0x2191, 0x2193,
                                              '/', 0x2588};

        if (c >= '+' && c <= '0')
                return arrows[c - '+'];

        if (c < 0x60 || c > 0x7e)
                return c;

        return map[c - 0x60];
}

// A colour in xterm's spelling, sixteen bits a channel, ended the way the
// question was.
static fn osc_colour(unsigned int colour, b32 bell)
{
        static const char hex[] = "0123456789abcdef";
        p8 text[] = "rgb:0000/0000/0000";

        for (unsigned int channel = 0; channel < 3; channel++)
        {
                unsigned int value = colour >> (16 - channel * 8) & 255;
                p8 address_to at = text + 4 + channel * 5;

                at[0] = at[2] = (p8)hex[value >> 4];
                at[1] = at[3] = (p8)hex[value & 15];
        }

        emit_bytes(text, sizeof(text) - 1);

        if (bell)
                emit(7);
        else
                emit_literal("\x1b\\");
}

static fn osc_finish(b32 bell)
{
        unsigned int i = 0;
        unsigned int command = 0;
        unsigned int n;

        // DCS, SOS, PM and APC are read the same way and say nothing here:
        // only an OSC names the window or asks for a colour.
        if (string_kind != ']')
                return;

        while (i < osc_length && osc_bytes[i] >= '0' && osc_bytes[i] <= '9')
                command = command * 10 + (unsigned int)(osc_bytes[i++] - '0');

        if (i < osc_length && osc_bytes[i] == ';')
                i++;

        if ((command == 0 || command == 2) && window)
        {
                n = osc_length - i;
                if (n >= WINDOW_TITLE_MAX)
                        n = WINDOW_TITLE_MAX - 1;
                memory_copy(window->title, osc_bytes + i, n);
                window->title[n] = 0;
        }

        /*
                OSC 4 ; index ; ? asks for a colour of the palette, and OSC 10,
                11 and 12 ; ? for the ink, the paper and the cursor, with more
                question marks after the first asking for the next ones along.
                vim, neovim and fish ask for the paper to choose a light or a
                dark theme, and wait out a timeout on every start when nothing
                answers. Setting a colour is not taken: every window shares the
                one table.
        */
        while (command == 4 || (command >= 10 && command <= 12))
        {
                unsigned int index = command == 11 ? 0 : 7;

                if (command == 4)
                {
                        unsigned int from = i;

                        for (index = 0; i < osc_length && osc_bytes[i] >= '0' &&
                                        osc_bytes[i] <= '9';
                             i++)
                                if (index < 256)
                                        index = index * 10 + (osc_bytes[i] - '0');

                        if (i == from || i >= osc_length || osc_bytes[i++] != ';')
                                break;
                }

                if (i < osc_length && osc_bytes[i] == '?' && index < 256)
                {
                        emit_literal("\x1b]");
                        positive_to_string(emit_bytes, command);
                        emit(';');
                        if (command == 4)
                        {
                                positive_to_string(emit_bytes, index);
                                emit(';');
                        }
                        osc_colour(window_cell_colour(index), bell);
                }

                while (i < osc_length && osc_bytes[i++] != ';')
                        ;

                if (i >= osc_length)
                        break;

                if (command != 4)
                        command++;
        }
}

static fn erase(unsigned int from_row, unsigned int from_column,
                unsigned int to_row, unsigned int to_column)
{
        b32 coloured = paper != 0;
        unsigned int r;

        for (r = from_row; r <= to_row && r < ROWS; r++)
        {
                unsigned int first = r == from_row ? from_column : 0;
                unsigned int last = r == to_row ? to_column : COLUMNS - 1;
                unsigned int past = min(last + 1, COLUMNS);
                unsigned int slot = row_slot(r);
                unsigned int address_to length = slot_length(slot);

                // Half of a double-width character cannot stand alone, so an
                // erase that takes one half takes the other with it.
                if (first < past)
                {
                        unpair(slot_cells(slot), address_to length, first);
                        unpair(slot_cells(slot), address_to length, past - 1);
                }

                /*
                        An erase that reaches the end of a line, or the edge
                        of the window, is the line getting shorter. That is
                        cheaper than the cells it would have written, and a
                        line stored wider than a window that has since
                        narrowed would otherwise keep a hidden tail for the
                        compositor to fold onto the next row: a full-screen
                        program redrawing after a resize came back to that.

                        A coloured erase is drawn, so it fills to the edge in
                        blank cells of that colour and still drops the tail.
                */
                if (!coloured)
                {
                        if (first >= address_to length)
                                continue;

                        if (past >= address_to length || past == COLUMNS)
                        {
                                address_to length = first;
                                touch(r);
                        }
                        else
                                cells_clear(r, first, past - first);

                        continue;
                }

                if (first >= past)
                {
                        if (address_to length > COLUMNS)
                        {
                                address_to length = COLUMNS;
                                touch(r);
                        }

                        continue;
                }

                reach(r, first);
                cells_clear(r, first, past - first);

                if (past == COLUMNS || address_to length < past)
                        address_to length = past;
        }
}

static fn tabs_reset()
{
        memory_fill_u64_aligned(tab_stop, TAB_STOPS / 8, 1);
}

static fn tab_forward()
{
        unsigned int c = column;

        while (++c < COLUMNS)
                if (c >= TAB_STOPS || tab_stop[c])
                        break;

        column = c < COLUMNS ? c : COLUMNS - 1;
}

static fn tab_backward()
{
        unsigned int c = column;

        if (!c)
                return;

        while (c)
        {
                c--;
                if (c < TAB_STOPS && tab_stop[c])
                        break;
        }

        column = c;
}

/*
        A cell carries an index into the two hundred and fifty six an xterm
        has, so the cube, the greys and a true-colour triple all land on one
        of those rather than being folded into the first sixteen. Folding
        them was what made 38;5;31m paint the text red: the number after 38;5
        fell through into the plain foreground range.
*/
static CONST unsigned char colour_cube_level(unsigned int v)
{
        if (v < 48)
                return 0;
        if (v < 115)
                return 1;
        v = (v - 35) / 40;
        return (unsigned char)(v > 5 ? 5 : v);
}

static CONST unsigned char colour_rgb(unsigned int r, unsigned int g,
                                      unsigned int b)
{
        unsigned int qr, qg, qb, grey, cube, cr, cg, cb, gr, dcube, dgrey;

        if (r > 255)
                r = 255;
        if (g > 255)
                g = 255;
        if (b > 255)
                b = 255;

        qr = colour_cube_level(r);
        qg = colour_cube_level(g);
        qb = colour_cube_level(b);
        cube = 16 + 36 * qr + 6 * qg + qb;
        cr = qr ? qr * 40 + 55 : 0;
        cg = qg ? qg * 40 + 55 : 0;
        cb = qb ? qb * 40 + 55 : 0;
        dcube = (r > cr ? r - cr : cr - r) + (g > cg ? g - cg : cg - g) +
                (b > cb ? b - cb : cb - b);

        grey = r < 8 ? 0 : (r > 238 ? 23 : (r - 8) / 10);
        gr = 8 + grey * 10;
        dgrey = (r > gr ? r - gr : gr - r) + (g > gr ? g - gr : gr - g) +
                (b > gr ? b - gr : gr - b);

        return (unsigned char)(dgrey < dcube ? 232 + grey : cube);
}

/*
        The parameters a colon joined to this one, which are its own and not
        SGR numbers in their own right: 4:3 is an underline's style and
        38:2::255:0:0 one colour with an empty colour space.
*/
static unsigned int sgr_joined(unsigned int at)
{
        unsigned int n = 0;

        while (at + n + 1 < terminal_csi.count &&
               (terminal_csi.colon >> (at + n + 1) & 1))
                n++;

        return n;
}

/*
        Returns how many parameters past this one it took, so the caller can
        step over the ones an extended colour is spelled with. Joined by
        colons the colour is those parameters and no more, and a direct colour
        may name a colour space before its channels or leave it out, so the
        channels are the last three either way. An index past the table leaves
        the colour as it was.
*/
static unsigned int sgr_extended(unsigned int at, unsigned int joined,
                                 unsigned char address_to which)
{
        unsigned int kind = at + 1 < terminal_csi.count
                                ? terminal_csi.value[at + 1] : 0;
        unsigned int took = joined;

        if (!joined)
                took = kind == 5 ? 2 : kind == 2 ? 4 : terminal_csi.count - at - 1;

        if (at + took >= terminal_csi.count)
                return terminal_csi.count - at - 1;

        if (kind == 5 && took >= 2 && terminal_csi.value[at + 2] < 256)
                address_to which = (unsigned char)terminal_csi.value[at + 2];
        else if (kind == 2 && took >= 4)
                address_to which = colour_rgb(terminal_csi.value[at + took - 2],
                                              terminal_csi.value[at + took - 1],
                                              terminal_csi.value[at + took]);

        return took;
}

static fn sgr()
{
        unsigned int count = terminal_csi.count ? terminal_csi.count : 1;

        // No parameter at all is a 0, and a reset parameter slot holds one.
        for (unsigned int i = 0; i < count; i++)
        {
                unsigned int p = terminal_csi.value[i];
                unsigned int took = sgr_joined(i);
                unsigned char unused;

                if (p == 0)
                {
                        ink = 7;
                        paper = 0;
                        reverse = false;
                        style = 0;
                }
                else if (p == 1)
                        style |= WINDOW_CELL_BOLD;
                else if (p == 2)
                        style |= WINDOW_CELL_DIM;
                else if (p == 3)
                        style |= WINDOW_CELL_ITALIC;
                // 4:0 is no underline and 4:1 to 4:5 its styles, and 21 is
                // ECMA-48's double one: all of them the one line here.
                else if (p == 4 && took && !terminal_csi.value[i + 1])
                        style &= (unsigned short)~WINDOW_CELL_UNDERLINE;
                else if (p == 4 || p == 21)
                        style |= WINDOW_CELL_UNDERLINE;
                else if (p == 5 || p == 6)
                        style |= WINDOW_CELL_BLINK;
                else if (p == 7)
                        reverse = true;
                else if (p == 8)
                        style |= WINDOW_CELL_HIDDEN;
                else if (p == 9)
                        style |= WINDOW_CELL_STRIKE;
                else if (p == 22)
                        style &= (unsigned short)~(WINDOW_CELL_BOLD | WINDOW_CELL_DIM);
                else if (p == 23)
                        style &= (unsigned short)~WINDOW_CELL_ITALIC;
                else if (p == 24)
                        style &= (unsigned short)~WINDOW_CELL_UNDERLINE;
                else if (p == 25)
                        style &= (unsigned short)~WINDOW_CELL_BLINK;
                else if (p == 27)
                        reverse = false;
                else if (p == 28)
                        style &= (unsigned short)~WINDOW_CELL_HIDDEN;
                else if (p == 29)
                        style &= (unsigned short)~WINDOW_CELL_STRIKE;
                else if (p >= 30 && p <= 37)
                        ink = (unsigned char)(p - 30);
                else if (p == 38)
                        took = sgr_extended(i, took, address_of ink);
                else if (p == 39)
                        ink = 7;
                else if (p >= 40 && p <= 47)
                        paper = (unsigned char)(p - 40);
                else if (p == 48)
                        took = sgr_extended(i, took, address_of paper);
                else if (p == 49)
                        paper = 0;
                // The underline's own colour, spelled the way 38 is, and with
                // nowhere to go.
                else if (p == 58)
                        took = sgr_extended(i, took, address_of unused);
                else if (p >= 90 && p <= 97)
                        ink = (unsigned char)(8 + (p - 90));
                else if (p >= 100 && p <= 107)
                        paper = (unsigned char)(8 + (p - 100));

                i += took;
        }
}

/*
        The other screen, which is this one further along.

        An alternate buffer is a screen a program is given, scribbles on and
        hands back with what was underneath still there. The ring already
        holds what was underneath: moving head on by a screenful gives out
        blank lines and leaves the old ones behind it, and moving head back is
        the hand back. There is no second buffer and nothing is copied.
*/
static b32 alternate;
static unsigned int alternate_head;

static fn cursor_save(struct cursor_state address_to into)
{
        address_to into = (struct cursor_state){row, column, ink, paper,
                                                style, reverse, charset_g0,
                                                charset_g1, charset_gl, origin_mode};
}

static fn cursor_restore(const struct cursor_state address_to from)
{
        row = from->row < ROWS ? from->row : ROWS - 1;
        column = from->column < COLUMNS ? from->column : COLUMNS - 1;
        ink = from->ink;
        paper = from->paper;
        style = from->style;
        reverse = from->reverse;
        charset_g0 = from->charset_g0;
        charset_g1 = from->charset_g1;
        charset_gl = from->charset_gl;
        origin_mode = from->origin_mode;
}

/*
        Only 1049 saves the cursor on the way in and puts it back on the way
        out, as DECSC and DECRC would; 47 and 1047 leave it where the program
        has it. None of them moves it: a program that wants the alternate
        screen from its top left says so.
*/
static fn alternate_enter(b32 save)
{
        if (alternate)
                return;

        if (save)
                cursor_save(address_of cursor_primary);

        alternate_head = window->head;

        for (unsigned int r = 0; r < ROWS; r++)
                window_scroll(window);

        alternate = true;
        touch_all();
}

static fn alternate_leave(b32 restore)
{
        if (!alternate)
                return;

        __atomic_store_n(address_of window->head, alternate_head, __ATOMIC_RELEASE);

        alternate = false;
        if (restore)
                cursor_restore(address_of cursor_primary);
        touch_all();
}

/*
        One mode, changed or only asked about. The answer is DECRPM's: 1 for
        set, 2 for reset and 0 for a mode this has never heard of, so SM, RM
        and DECRQM read the one list and cannot disagree.
*/
static unsigned int mode(unsigned int p, b32 change, b32 on)
{
        b32 now;

        if (!terminal_csi.marker)
        {
                if (p != 4)
                        return 0;

                if (change)
                        insert_mode = on;

                return insert_mode ? 1 : 2;
        }

        switch (p)
        {
        case 1:
                if (change)
                        application_keys = on;
                now = application_keys;
                break;
        case 7:
                if (change)
                        autowrap = on;
                now = autowrap;
                break;
        case 25:
                if (change)
                        cursor_visible = on;
                now = cursor_visible;
                break;
        case 6:
                if (change)
                {
                        origin_mode = on;
                        row = on ? region_top : 0;
                        column = 0;
                }
                now = origin_mode;
                break;
        case 47:
        case 1047:
        case 1049:
                if (change && on)
                        alternate_enter(p == 1049);
                else if (change)
                        alternate_leave(p == 1049);
                now = alternate;
                break;
        case 1000:
        case 1002:
        case 1003:
                if (change)
                {
                        mouse_mode = on ? p : 0;
                        if (window)
                                window->want = mouse_mode ? WINDOW_WANT_POINTER : 0;
                }
                now = mouse_mode == p;
                break;
        case 1004:
                if (change)
                        focus_events = on;
                now = focus_events;
                break;
        case 1006:
                if (change)
                        mouse_sgr = on;
                now = mouse_sgr;
                break;
        case 2026:
                if (change)
                        synchronized_output = on;
                now = synchronized_output;
                break;
        default:
                return 0;
        }

        return now ? 1 : 2;
}

static fn mode_set(b32 on)
{
        for (unsigned int i = 0; i < terminal_csi.count; i++)
                mode(terminal_csi.value[i], true, on);
}

static fn attributes_reset()
{
        ink = 7;
        paper = 0;
        reverse = false;
        style = 0;
        autowrap = true;
        insert_mode = false;
        cursor_visible = true;
        application_keys = false;
        synchronized_output = false;
        mouse_mode = 0;
        mouse_sgr = false;
        focus_events = false;
        origin_mode = false;
        charset_g0 = CHARSET_ASCII;
        charset_g1 = CHARSET_ASCII;
        charset_gl = 0;
        if (window)
                window->want = 0;
}

/*
        DECSTR, which is what is2 sends. The screen and the cursor stay; the
        modes do not.
        RIS is the one that blanks the page, and folding the two together
        made every ncurses init wipe a dashboard that had just drawn.
*/
static fn soft_reset()
{
        attributes_reset();
        last_character = 0;
        region_top = 0;
        region_bottom = ROWS;
        cursor_saved = (struct cursor_state){0, 0, 7, 0, 0, false};
}

/*
        RIS: DECSTR's resets, on the primary screen, with the tab stops back
        and the page blank. Leaving the alternate screen comes first: it puts
        back the cursor the shell had, and a reset is the cursor at home.
*/
static fn full_reset()
{
        alternate_leave(false);
        soft_reset();
        row = 0;
        column = 0;
        cursor_shape = 0;
        tabs_reset();
        erase(0, 0, ROWS - 1, COLUMNS - 1);
}

/*
        ED 3 is xterm's: the lines scrolled off the top are forgotten and the
        screen stays as it is. clear sends it after ED 2, and it blanked the
        screen a second time and kept the history. The alternate screen has no
        history, and the lines behind it are the primary screen waiting to
        come back.
*/
static fn history_clear()
{
        unsigned int slot = row_slot(0);

        if (alternate)
                return;

        for (unsigned int n = ROWS; n < window->history; n++)
        {
                slot = slot ? slot - 1 : window->history - 1;
                address_to slot_length(slot) = 0;
        }
}

/*
        A marker or an intermediate byte makes another sequence of the same
        final. CSI > 4 ; 2 m is xterm's modifyOtherKeys and not an underline,
        CSI ? u asks about the keyboard rather than restoring the cursor, and
        CSI 2 SP @ scrolls sideways rather than inserting. A final that does
        not take the marker or intermediate it came with is ignored, which is
        what xterm does with a sequence it has never heard of.
*/
static b32 csi_known(unsigned int final)
{
        switch (csi_intermediate)
        {
        case 0:
                break;
        case ' ':
                return final == 'q' && !terminal_csi.marker;
        case '!':
                return final == 'p' && !terminal_csi.marker;
        case '$':
                return final == 'p';
        default:
                return false;
        }

        switch (terminal_csi.marker)
        {
        case 0:
                return true;
        case '?':
                return final == 'h' || final == 'l' || final == 'n' ||
                       final == 'J' || final == 'K';
        case '>':
        case '=':
                return final == 'c';
        default:
                return false;
        }
}

static fn csi_final(unsigned int final)
{
        unsigned int a = terminal_csi.count && terminal_csi.value[0]
                             ? terminal_csi.value[0] : 1;
        unsigned int b = terminal_csi.count > 1 && terminal_csi.value[1]
                             ? terminal_csi.value[1] : 1;

        if (!csi_known(final))
                return;

        switch (final)
        {
        case 'H':
        case 'f':
        {
                unsigned int top = origin_mode ? region_top : 0;
                unsigned int bottom = origin_mode ? region_bottom : ROWS;
                unsigned int r = top + a - 1;

                row = r < bottom ? r : (bottom ? bottom - 1 : 0);
                column = b - 1 < COLUMNS ? b - 1 : COLUMNS - 1;
                break;
        }
        /*
                Up and down stop at the margin of a region the cursor is in or
                past, and at the edge of the page from the other side of it, as
                xterm and tmux do. A count is compared with the room left, since
                a hostile one wraps row + a, and a pending wrap is gone once the
                cursor has moved.
        */
        case 'A':
        case 'F':
        {
                unsigned int limit = row >= region_top ? region_top : 0;

                row = row - limit > a ? row - a : limit;
                column = final == 'F' ? 0 : min(column, COLUMNS - 1);
                break;
        }
        case 'B':
        case 'e':
        case 'E':
        {
                unsigned int limit = row < region_bottom ? region_bottom - 1 : ROWS - 1;

                row = limit - row > a ? row + a : limit;
                column = final == 'E' ? 0 : min(column, COLUMNS - 1);
                break;
        }
        case 'C':
        case 'a':
                column = a < COLUMNS - column ? column + a : COLUMNS - 1;
                break;
        case 'D':
                column = column > a ? column - a : 0;
                break;
        case 'G':
        case '`':
                column = a - 1 < COLUMNS ? a - 1 : COLUMNS - 1;
                break;
        case 'd':
        {
                unsigned int top = origin_mode ? region_top : 0;
                unsigned int bottom = origin_mode ? region_bottom : ROWS;
                unsigned int r = top + a - 1;

                row = r < bottom ? r : (bottom ? bottom - 1 : 0);
                break;
        }
        // A reset parameter slot is 0, so no parameter is ED 0 and EL 0, and
        // a number nobody has defined erases nothing.
        case 'J':
                if (terminal_csi.value[0] == 0)
                        erase(row, column, ROWS - 1, COLUMNS - 1);
                else if (terminal_csi.value[0] == 1)
                        erase(0, 0, row, column);
                else if (terminal_csi.value[0] == 2)
                        erase(0, 0, ROWS - 1, COLUMNS - 1);
                else if (terminal_csi.value[0] == 3)
                        history_clear();
                break;
        case 'K':
                if (terminal_csi.value[0] == 0)
                        erase(row, column, row, COLUMNS - 1);
                else if (terminal_csi.value[0] == 1)
                        erase(row, 0, row, column);
                else if (terminal_csi.value[0] == 2)
                        erase(row, 0, row, COLUMNS - 1);
                break;
        case 'L':
        case 'M':
                // Lines are put in and taken out at the cursor, and the rest
                // of the region moves. A cursor outside the region is a
                // sequence with nowhere to happen.
                if (row >= region_top && row < region_bottom)
                {
                        unsigned int was = region_top;

                        region_top = row;

                        if (final == 'L')
                                scroll_down(a);
                        else
                                scroll_up(a);

                        region_top = was;
                }
                break;
        case 'P':
        {
                unsigned int address_to length;
                struct window_cell address_to cells;
                unsigned int last;
                unsigned int gone;

                // In colour the line is drawn to the edge, and what DCH pulls
                // in at the right margin is that colour.
                if (paper && column < COLUMNS)
                        reach(row, COLUMNS);

                length = row_length(row);
                cells = row_cells(row);
                last = address_to length;

                if (column >= last)
                        break;

                gone = a < last - column ? a : last - column;

                memory_copy(cells + column, cells + column + gone,
                            (positive)(last - column - gone) *
                                sizeof(struct window_cell));

                if (paper)
                        cells_clear(row, last - gone, gone);
                else
                        address_to length = last - gone;

                touch(row);
                break;
        }
        case '@':
                open_gap(column, a);
                break;
        case 'X':
                // An erase like the others, so one in colour past the end of
                // the line is drawn there.
                if (column < COLUMNS)
                        erase(row, column, row, column + min(a, COLUMNS - column) - 1);
                break;
        case 'S':
                scroll_up(a);
                break;
        case 'T':
                scroll_down(a);
                break;
        case 'g':
                if (terminal_csi.count && terminal_csi.value[0] == 3)
                {
                        memory_fill(tab_stop, 0, sizeof(tab_stop));
                }
                else if (column < TAB_STOPS)
                        tab_stop[column] = 0;
                break;
        case 'h':
                mode_set(true);
                break;
        case 'l':
                mode_set(false);
                break;
        case 'm':
                sgr();
                break;
        case 'n':
                // The cursor is reported one based, which is the same
                // counting CUP takes it back in.
                if (terminal_csi.count && terminal_csi.value[0] == 6)
                {
                        emit_literal("\x1b[");
                        if (terminal_csi.marker)
                                emit('?');
                        positive_to_string(emit_bytes, row + 1);
                        emit(';');
                        positive_to_string(emit_bytes,
                                           column < COLUMNS ? column + 1 : COLUMNS);
                        emit('R');
                }
                else if (terminal_csi.count && terminal_csi.value[0] == 5 &&
                         !terminal_csi.marker)
                        emit_literal("\x1b[0n");
                break;
        case 'c':
                // Primary DA names a VT100 with AVO, which is what xterm
                // answers and what ncurses's u8 reads for. Secondary DA is
                // the xterm version report, and tertiary DA the unit's number,
                // which xterm gives as zeros.
                if (terminal_csi.marker == '>')
                        emit_literal("\x1b[>0;115;0c");
                else if (terminal_csi.marker == '=')
                        emit_literal("\x1bP!|00000000\x1b\\");
                else
                        emit_literal("\x1b[?1;2c");
                break;
        /*
                A count saturates at four billion rather than wrapping, and
                this runs in the kernel console with interrupts off: no count
                of tab stops can go further than the width has stops.
        */
        case 'I':
                for (a = a < COLUMNS ? a : COLUMNS; a; a--)
                        tab_forward();
                break;
        case 'Z':
                for (a = a < COLUMNS ? a : COLUMNS; a; a--)
                        tab_backward();
                break;
        case 'b':
                if (last_character)
                {
                        unsigned int n;

                        // Every cell of the screen once is all a repeat can
                        // show, and the count is as hostile as the tabs'.
                        for (n = 0; n < a && n < ROWS * COLUMNS; n++)
                                put(last_character);
                }
                break;
        case 'q':
                // DECSCUSR: 1 and 2 are a block, 3 and 4 an underline, 5 and 6
                // a bar. Nothing here blinks, so each pair is one shape. Without
                // the space it is DECLL, which has no lights here to set.
                if (csi_intermediate == ' ' && a <= 6)
                        cursor_shape = a;
                break;
        case 'p':
                if (csi_intermediate == '!')
                        soft_reset();
                // DECRQM. neovim and kakoune ask before they lean on
                // synchronized output or the mouse.
                else if (csi_intermediate == '$')
                {
                        unsigned int p = terminal_csi.count ? terminal_csi.value[0] : 0;

                        emit_literal("\x1b[");
                        if (terminal_csi.marker)
                                emit(terminal_csi.marker);
                        positive_to_string(emit_bytes, p);
                        emit(';');
                        emit('0' + mode(p, false, false));
                        emit_literal("$y");
                }
                break;
        case 't':
                break;
        case 'r':
        {
                unsigned int top;
                unsigned int bottom;

                top = terminal_csi.count && terminal_csi.value[0]
                          ? terminal_csi.value[0] - 1 : 0;
                bottom = terminal_csi.count > 1 && terminal_csi.value[1]
                             ? terminal_csi.value[1] : ROWS;

                if (bottom > ROWS)
                        bottom = ROWS;

                // A region has to have two rows in it to scroll, and one that
                // does not is left alone rather than half applied.
                if (top + 1 < bottom)
                {
                        region_top = top;
                        region_bottom = bottom;

                        // Home is the top of the region when origin mode is
                        // on, and the top of the page otherwise.
                        row = origin_mode ? region_top : 0;
                        column = 0;
                }
                break;
        }
        case 's':
                cursor_save(address_of cursor_saved);
                break;
        case 'u':
                cursor_restore(address_of cursor_saved);
                break;
        }
}

/*
        Bytes into characters.

        A cell holds a character and the stream is UTF-8, so the two are not
        the same thing and the bytes above 127 were being thrown away one at a
        time. Anything malformed becomes U+FFFD rather than disappearing,
        which is what stops one bad byte from eating the character after it.
*/
static memory_utf8_state terminal_utf8;

static fn utf8_byte(unsigned int c)
{
        // A byte that cuts a sequence short is refused along with it, and it
        // is still a byte of its own: the feed leaves replaying it to us.
        bool cut = terminal_utf8.left && ((p8)c & 0xc0) != 0x80;
        b32 result = memory_utf8_feed(address_of terminal_utf8, (p8)c);

        if (result < 0 && cut)
        {
                put(0xfffd);
                result = memory_utf8_feed(address_of terminal_utf8, (p8)c);
        }
        // A C1 control spelled in UTF-8 is a control nothing here acts on,
        // not a character to draw.
        if (result < 0)
                put(0xfffd);
        else if (result && terminal_utf8.value >= 0xa0)
                put(terminal_utf8.value);
}

static fn line_forget();

// A sequence that has stopped short is one character that never arrived, and
// whatever ended it is not it.
static fn utf8_flush()
{
        if (!terminal_utf8.left)
                return;

        terminal_utf8.left = 0;
        put(0xfffd);
}

static fn consume(unsigned int c)
{
        /*
                Two bytes that end whatever is being read.

                CAN and SUB abandon a sequence wherever in one they arrive,
                ECMA-48 8.3.5 and the control strings included. Leaving them
                to the parameter bytes took the final of the abandoned
                sequence out of the text after it -- ESC [ 3 ; 3 CAN X ran the
                X as an erase rather than printing it -- and leaving them to
                the string reader left an OSC that was cut short with nothing
                at all that could end it, and every byte after it went
                nowhere.
        */
        if (c == 24 || c == 26)
        {
                in_escape = in_csi = in_string = false;
                terminal_utf8.left = 0;
                return;
        }

        // Anything the far end says lands where the cursor is, so a line
        // being typed is no longer where it was drawn and the next keystroke
        // draws it again from wherever the output left off.
        line_forget();

        /*
                A string sequence carries a payload that is not for the
                screen.

                A window title arrives as ESC ] 0 ; text BEL and every byte of
                it used to be printed, because the parser gave up at the ] and
                went back to putting characters in cells.
        */
        if (in_string)
        {
                if (!string_escape)
                {
                        if (c == 27)
                                string_escape = true;
                        else if (c == 7)
                        {
                                osc_finish(true);
                                in_string = in_escape = false;
                        }
                        else if (osc_length < WINDOW_TITLE_MAX)
                                osc_bytes[osc_length++] = (p8)c;

                        return;
                }

                /*
                        An escape inside a string ends it either way.

                        With a backslash after it that is ST, the ending the
                        string was written to have. With anything else it is
                        the next sequence beginning, and taking it as one is
                        what stops a title nobody terminated from swallowing
                        every sequence sent after it.
                */
                string_escape = in_string = false;

                if (c == '\\')
                {
                        osc_finish(false);
                        in_escape = false;
                        return;
                }

                osc_finish(false);
                in_escape = true;
                escape_intermediate = false;
        }

        if (c == 27)
        {
                utf8_flush();
                in_escape = true;
                in_csi = false;
                escape_intermediate = false;
                return;
        }

        /*
                A control is done where it arrives, in the middle of a sequence
                too: ESC [ BS C steps back and then forward, and ESC LF 7 is a
                line feed and a save. Dropping them put what followed the
                sequence a column or a line away from where it was sent. One
                inside a UTF-8 character cuts it short like any other byte that
                is not a continuation, and DEL is nothing anywhere.
        */
        if (c < ' ' || c == 127)
        {
                utf8_flush();

                switch (c)
                {
                case '\n':
                case 11:
                case 12:
                        line_feed();
                        touch(row);
                        break;
                case '\r':
                        column = 0;
                        break;
                case '\b':
                        if (column)
                                column--;
                        break;
                case '\t':
                        tab_forward();
                        break;
                case 14:
                        charset_gl = 1;
                        break;
                case 15:
                        charset_gl = 0;
                        break;
                }

                return;
        }

        if (in_csi)
        {
                if (c >= 0x20 && c <= 0x2f)
                {
                        csi_intermediate = (p8)c;
                        return;
                }

                if (!terminal_parameters_take(address_of terminal_csi, c))
                        return;

                csi_final(c);
                in_csi = in_escape = false;
                csi_intermediate = 0;

                return;
        }

        if (in_escape)
        {
                if (c == '[')
                {
                        in_csi = true;
                        csi_intermediate = 0;
                        terminal_parameters_reset(address_of terminal_csi);
                        return;
                }

                if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_')
                {
                        in_string = true;
                        string_escape = false;
                        string_kind = (p8)c;
                        osc_length = 0;
                        return;
                }

                // ESC ( B and its like: an intermediate byte says the sequence
                // carries on to a final one. Ending the sequence at the first
                // of them printed the rest of it on the screen.
                if (c >= ' ' && c <= '/')
                {
                        escape_intermediate = true;
                        escape_kind = (p8)c;
                        return;
                }

                in_escape = false;

                // A final after an intermediate belongs to that sequence, so
                // ESC # 8 is one thing and not a DECRC hiding behind a hash.
                if (escape_intermediate)
                {
                        if (escape_kind == '(')
                                charset_g0 = c == '0' ? CHARSET_ACS : CHARSET_ASCII;
                        else if (escape_kind == ')')
                                charset_g1 = c == '0' ? CHARSET_ACS : CHARSET_ASCII;
                        escape_intermediate = false;
                        return;
                }

                switch (c)
                {
                case '7':
                        cursor_save(address_of cursor_saved);
                        break;
                case '8':
                        cursor_restore(address_of cursor_saved);
                        break;
                case 'D':
                        line_feed();
                        break;
                case 'E':
                        line_feed();
                        column = 0;
                        break;
                case 'M':
                        if (row == region_top)
                                scroll_down(1);
                        else if (row)
                                row--;
                        break;
                case 'H':
                        if (column < TAB_STOPS)
                                tab_stop[column] = 1;
                        break;
                case 'c':
                        full_reset();
                        break;
                // ESC = and ESC > are the keypad's modes, and the keypad
                // sends the same in both. They are not DECCKM's arrows.
                }

                return;
        }

        if (c < 128)
        {
                utf8_flush();
                put((charset_gl ? charset_g1 : charset_g0) == CHARSET_ACS
                        ? acs_character(c)
                        : c);
                return;
        }

        utf8_byte(c);
}

/*
        Text, a run at a time.

        Most of what a pty carries is characters, and a byte at a time every
        one of them went through each test the escape machine makes and a
        UTF-8 decoder that keeps its state in memory. Between sequences, with
        nothing half decoded, insert mode off and plain ASCII in GL, a run of
        printable ASCII goes straight into the row it lands on, and a whole,
        valid UTF-8 character goes to put() in one step, which is where its
        width is decided. Anything else ends the run -- a control, DEL, a
        byte that is not UTF-8 or a character the read has not all of -- and
        the byte it ended at is consume()'s, as it always was.

        Every loop here is bounded by the bytes it was handed: the kernel
        console calls this with interrupts off.
*/
static inline INLINE b32 text_ground()
{
        return !in_escape && !in_csi && !in_string && !terminal_utf8.left &&
               !insert_mode &&
               (charset_gl ? charset_g1 : charset_g0) == CHARSET_ASCII;
}

/* Printable ASCII from the first byte, as far as the row has room, left as
   put() a character at a time would leave it. Answers the bytes taken. */
static positive text_ascii(const p8 address_to bytes, positive count)
{
        struct window_cell address_to cells;
        unsigned int address_to length;
        unsigned int first, had, slot;
        positive room, attribute, n = 0;

        // put_cells' wrap, for a character one column wide.
        if (column + 1 > COLUMNS)
        {
                if (!autowrap)
                        column = COLUMNS - 1;
                else
                {
                        column = 0;
                        line_feed();
                }
        }

        first = column;
        room = COLUMNS - first;
        slot = row_slot(row);
        cells = slot_cells(slot);
        length = slot_length(slot);

        if (address_to length < first)
        {
                cells_blank(row, address_to length, first - address_to length);
                address_to length = first;
        }

        had = address_to length;

        // A cell as BLANK_CELL_WORD lays one out: the character in the low
        // half, the colours and flags above it.
        attribute = ((positive)(reverse ? paper : ink_drawn()) << 32) |
                    ((positive)(reverse ? ink_drawn() : paper) << 40) |
                    ((positive)style << 48);

        /*
                Half of a wide character among the cells written over makes
                put() blank its other half, and that half can be a cell of
                this same run or the one before it. Only a cell inside the
                line can be one, and unpairing a cell that is not one does
                nothing, so it is asked only of those -- still before the
                store, in put()'s own order.
        */
        for (;;)
        {
                unsigned int at = first + (unsigned int)n;
                positive word = attribute | bytes[n];

                if (at < had &&
                    (cells[at].flags & (WINDOW_CELL_WIDE | WINDOW_CELL_WIDE_RIGHT)))
                        unpair(cells, had, at);

                memory_copy(cells + at, address_of word, sizeof(word));
                n++;

                if (n == count || n == room || (unsigned int)bytes[n] - ' ' >= 95)
                        break;
        }

        last_character = bytes[n - 1];
        column = first + (unsigned int)n;
        touch(row);

        if (address_to length < column)
                address_to length = column;

        return n;
}

/* A whole, valid UTF-8 character of two to four bytes, just as
   memory_utf8_feed would finish it and utf8_byte would draw it, or 0 for one
   it would refuse, one that is not drawn, or one the bytes do not hold all
   of. */
static unsigned int text_utf8(const p8 address_to bytes, positive count,
                              unsigned int address_to character)
{
        unsigned int lead = bytes[0];
        unsigned int value;

        if (lead >= 0xc2 && lead < 0xe0)
        {
                if (count < 2 || (bytes[1] & 0xc0) != 0x80)
                        return 0;

                // U+0080 to U+009F are C1 controls, which utf8_byte drops
                // rather than draws: consume() says what becomes of them.
                if (lead == 0xc2 && bytes[1] < 0xa0)
                        return 0;

                address_to character = ((lead & 0x1f) << 6) | (bytes[1] & 0x3f);
                return 2;
        }

        if (lead >= 0xe0 && lead < 0xf0)
        {
                if (count < 3 || (bytes[1] & 0xc0) != 0x80 ||
                    (bytes[2] & 0xc0) != 0x80)
                        return 0;

                value = ((lead & 0x0f) << 12) | ((bytes[1] & 0x3f) << 6) |
                        (bytes[2] & 0x3f);

                if (value < 0x800 || (value >= 0xd800 && value <= 0xdfff))
                        return 0;

                address_to character = value;
                return 3;
        }

        if (lead >= 0xf0 && lead < 0xf5)
        {
                if (count < 4 || (bytes[1] & 0xc0) != 0x80 ||
                    (bytes[2] & 0xc0) != 0x80 || (bytes[3] & 0xc0) != 0x80)
                        return 0;

                value = ((lead & 0x07) << 18) | ((bytes[1] & 0x3f) << 12) |
                        ((bytes[2] & 0x3f) << 6) | (bytes[3] & 0x3f);

                if (value < 0x10000 || value > 0x10ffff)
                        return 0;

                address_to character = value;
                return 4;
        }

        return 0;
}

static positive text_run(const p8 address_to bytes, positive count)
{
        positive at = 0;

        while (at < count)
        {
                unsigned int c = bytes[at];
                unsigned int character;
                unsigned int used;

                if (c - ' ' < 95)
                {
                        at += text_ascii(bytes + at, count - at);
                        continue;
                }

                used = c >= 0x80 ? text_utf8(bytes + at, count - at,
                                             address_of character)
                                 : 0;

                if (!used)
                        break;

                put(character);
                at += used;
        }

        // What consume() does for every byte, and it is the same for a run.
        if (at)
                line_forget();

        return at;
}

/*
        A control sequence the read holds all of.

        A byte at a time, every byte of ESC [ 3 8 ; 5 ; 1 9 6 m went through
        consume()'s tests and stored its digit before the next byte could be
        looked at, and a dashboard is mostly such sequences. When the read has
        the whole of one it is taken here in one pass, through the same
        parameter machine -- the colon mask, the slots running out, digits
        saturating at ~0u, the marker kept apart, the last intermediate in
        csi_intermediate -- and csi_final() is its dispatch as it is
        consume()'s.

        What a sequence can be interrupted by is consume()'s to do: a control
        is done where it arrives, CAN and SUB abandon the sequence and ESC
        starts another. With one of those in it, or a final the read does not
        reach, nothing here has happened but parameter slots written as
        scratch, which the sequence's own [ resets, and consume() takes it
        from its ESC.
*/
static positive csi_parse(const p8 address_to bytes, positive count)
{
        unsigned int slots = 0, colon = 0, value = 0;
        p8 marker = 0, full = 0, intermediate = 0;
        positive at;

        if (count < 3 || bytes[1] != '[')
                return 0;

        /*
                terminal_parameters_take's transitions, with the count, the
                colon mask, the marker and the number being read in registers
                rather than stored and loaded again for every byte. A slot is
                written as it is left and the last one at the final, which
                leaves the array as a byte at a time leaves it.
        */
        for (at = 2; at < count; at++)
        {
                unsigned int c = bytes[at];

                // Below 429496729 another digit cannot pass ~0u, so the
                // saturating test is taken only past that.
                if (c - '0' < 10)
                {
                        if (full)
                                continue;

                        if (!slots)
                                slots = 1;

                        for (;;)
                        {
                                unsigned int digit = bytes[at] - '0';

                                if (value < 429496729)
                                        value = value * 10 + digit;
                                else if (value > (~0u - digit) / 10)
                                        value = ~0u;
                                else
                                        value = value * 10 + digit;

                                if (at + 1 == count ||
                                    (unsigned int)bytes[at + 1] - '0' >= 10)
                                        break;

                                at++;
                        }

                        continue;
                }

                if (c == ';' || c == ':')
                {
                        if (!slots)
                                slots = 1;

                        if (slots == TERMINAL_PARAMETERS)
                                full = 1;
                        else if (!full)
                        {
                                terminal_csi.value[slots - 1] = value;

                                if (c == ':')
                                        colon |= 1u << slots;

                                slots++;
                                value = 0;
                        }

                        continue;
                }

                if (c >= '@' && c <= '~')
                        break;

                if (c < ' ')
                        return 0;

                // DEL and bytes past ASCII are nothing inside a sequence, as
                // they are to consume().
                if (c <= '/')
                        intermediate = (p8)c;
                else if (c >= '<' && c <= '?')
                        marker = (p8)c;
        }

        if (at == count)
                return 0;

        // With no parameter at all, value[0] is still 0, as a reset leaves it.
        terminal_csi.value[slots ? slots - 1 : 0] = value;
        terminal_csi.count = slots;
        terminal_csi.colon = colon;
        terminal_csi.marker = marker;
        terminal_csi.full = full;

        // What the ESC and the [ did, and the state the final runs in.
        line_forget();
        utf8_flush();
        escape_intermediate = false;
        in_escape = in_csi = true;
        csi_intermediate = intermediate;

        csi_final(bytes[at]);

        in_csi = in_escape = false;
        csi_intermediate = 0;

        return at + 1;
}

/*
        A read's worth of what the far end sent.

        The pty loop and the kernel console hand over what they have in one
        call, so a run of text and a whole control sequence can each be taken
        in one step; everything else is still consume()'s a byte at a time.
*/
static fn term_bytes(const p8 address_to bytes, positive count)
{
        positive at = 0;

        while (at < count)
        {
                unsigned int c = bytes[at];

                if (c >= ' ' && c != 127 && text_ground())
                {
                        positive used = text_run(bytes + at, count - at);

                        if (used)
                        {
                                at += used;
                                continue;
                        }
                }
                else if (c == 27 && !in_string)
                {
                        positive used = csi_parse(bytes + at, count - at);

                        if (used)
                        {
                                at += used;
                                continue;
                        }
                }

                consume(c);
                at++;
        }
}

#ifdef KERNEL_MODE
/* A pty is a stream, but printk records are unrelated messages. Do not let an
   unfinished escape or UTF-8 sequence in attacker-controlled log text consume
   later diagnostics. */
static fn term_record_begin()
{
        in_escape = in_csi = in_string = false;
        escape_intermediate = string_escape = false;
        osc_length = 0;
        terminal_parameters_reset(address_of terminal_csi);
        terminal_utf8.left = 0;
}
#endif

static fn cursor_hide()
{
        if (!shown)
                return;

        struct window_cell address_to cell = row_cells(shown_row) + shown_column;
        unsigned char was = cell->ink;

        if (shown_block)
        {
                cell->ink = cell->paper;
                cell->paper = was;
        }
        else
                cell->flags &= (unsigned short)~shown_marks;

        touch(shown_row);
        shown = false;
}

static fn SPARE cursor_show()
{
        // put leaves column at COLUMNS after filling the last cell of a row and
        // only wraps on the next character, so the cursor has to be clamped:
        // unclamped it landed on the first cell of the row below, and on the
        // last row that is one cell past the grid.
        unsigned int at = column < COLUMNS ? column : COLUMNS - 1;
        struct window_cell address_to cell;
        unsigned char was;

        // Showing is a state transition, not a colour toggle. In particular,
        // a synchronous resize commit and the ordinary loop tail can both
        // ask in one iteration; the second must not invert the cell back.
        if (shown || !cursor_visible)
                return;

        // A block sits on a cell, so there has to be one to sit on.
        reach(row, at + 1);

        cell = row_cells(row) + at;
        was = cell->ink;

        // A block is the cell with its colours the other way round. An
        // underline or a bar is a mark added to the cell, and hiding takes
        // off only the mark this added.
        shown_block = cursor_shape < 3;

        if (shown_block)
        {
                cell->ink = cell->paper;
                cell->paper = was;
        }
        else
        {
                unsigned short mark = cursor_shape >= 5 ? WINDOW_CELL_BAR
                                                        : WINDOW_CELL_UNDERLINE;

                shown_marks = (unsigned short)(mark & ~cell->flags);
                cell->flags |= shown_marks;
        }

        shown_row = row;
        shown_column = at;
        shown = true;
        touch(row);
}

/*
        The line being typed, before it is a line.

        A terminal in canonical mode hands its shell whole lines and the kernel
        line discipline is what assembles them -- which can erase a character
        and kill a line and nothing else. There is no left arrow in it, no
        history, and no way to put a character anywhere but at the end.

        So the assembling happens here instead. It belongs to whichever end
        knows both what has been typed and where it is on the screen, and that
        is this one: the shell has the bytes but not the grid, and the line
        discipline has neither. screen.c turns the far end's echo off while
        this is running and turns this off the moment a program asks for raw
        input, so a program that wants its own keys still gets them.

        Nothing here is a system call either. What the shell is to be told is
        put in to_shell and sent by the loop that owns the descriptor.
*/
#define LINE_HISTORY 32

static positive line_drawn;
static b32 line_anchored;

// Where on the screen the line starts, as a line of the ring rather than a
// row: the screen scrolls out from under a long line, and a row would then be
// pointing at somebody else's text. Out here with the rest of what the output
// half touches, because a resize has an opinion about the column.
#ifndef KERNEL_MODE
static unsigned int line_anchor;
static positive line_view;
static unsigned int line_screen_anchor, line_screen_column;
#endif
static unsigned int line_anchor_column;

/*
        What the output half has to say about a line being typed, and no more.

        Anything the far end prints moves the cursor out from under a line the
        editor drew, so consume() says so and the next keystroke draws it
        again. That is the whole of what the kernel build needs: printk writes
        into the console window and nobody types into it, so the editor itself
        -- and the thirty four kilobytes of line and history behind it -- is
        left out below.
*/
static fn line_forget()
{
        line_anchored = false;
        line_drawn = 0;
#ifndef KERNEL_MODE
        line_view = 0;
#endif
}

#ifndef KERNEL_MODE

static b32 line_editing;
static p8 address_to line;
static positive line_room, line_length, line_point;

static p8 address_to history[LINE_HISTORY];
static positive history_room[LINE_HISTORY];
static positive history_length[LINE_HISTORY];
static positive history_count, history_at;

// The line being typed, kept while the history is walked so that coming back
// down off the end returns it rather than an empty line.
static p8 address_to history_held;
static positive history_held_room, history_held_length;

// Ctrl-L keeps the cells to the left of the line anchor without imposing a
// second, unrelated limit on the width of a prompt.
static struct window_cell address_to line_prompt;
static positive line_prompt_room, line_prompt_length;

static unsigned int line_anchor_row()
{
        unsigned int at = line_anchor - (window->head - ROWS);

        return at < ROWS ? at : 0;
}

static fn line_erase(b32 shorten);

/*
        The line, on the screen, wherever it now is.

        Everything is written again from the anchor: it is at most a screenful
        of cells and it is the only way a character put in the middle can move
        the ones after it. What the last pass wrote is blanked by writing over
        it, because a line that got shorter has to stop being on the screen.
*/
static fn line_show()
{
        positive at, capacity, caret, first, drawn;
        bipolar anchor_row;
        unsigned int top;

        if (!line_anchored)
                return;

        /*
                Redrawing an entire command longer than the viewport scrolled
                it once for every key.  Eventually the oldest part replaced
                the newest and the cursor was pinned to the bottom-right cell.

                Make the editable line a viewport of its own.  Its logical
                anchor never moves; shift whole rows only far enough to keep
                the insertion point visible, and draw no more than the screen
                can hold.  Moving Home reveals the beginning again without
                losing a byte of the command.
        */
        caret = line_point < line_length ? line_point
                                        : (line_point ? line_point - 1 : 0);

        line_erase(false);
        line_drawn = 0;

        /* Advance the real ring only when the insertion cell first crosses
           its bottom.  A redraw at the same point must never scroll again. */
        while ((positive)line_anchor +
                   (line_anchor_column + caret) / COLUMNS >= window->head)
                ring_scroll();

        top = window->head - ROWS;
        anchor_row = (bipolar)line_anchor - (bipolar)top;

        if (line_view && caret >= line_view &&
            caret - line_view < (positive)ROWS * COLUMNS)
        {
                first = line_view;
                line_screen_anchor = top;
                line_screen_column = 0;
        }
        else if (anchor_row < 0)
        {
                bipolar hidden = -anchor_row * (bipolar)COLUMNS -
                                 (bipolar)line_anchor_column;

                first = hidden > 0 ? (positive)hidden : 0;

                // Home and a long run of Lefts deliberately page back into
                // the part which has already scrolled out of the live ring.
                if (caret < first)
                        first = caret - caret % COLUMNS;

                line_screen_anchor = top;
                line_screen_column = 0;
        }
        else
        {
                first = 0;
                line_screen_anchor = top + (unsigned int)anchor_row;
                line_screen_column = line_anchor_column;
        }

        if (first > line_length)
                first = line_length;

        capacity = ((positive)window->head - line_screen_anchor) * COLUMNS -
                   line_screen_column;
        drawn = line_length - first;

        if (drawn > capacity)
                drawn = capacity;

        row = line_screen_anchor - top;
        column = line_screen_column;

        // The editor counts a column to a character.
        for (at = 0; at < drawn; at++)
                put_cells(line[first + at], 1);

        line_view = first;
        line_drawn = drawn;

        at = line_point > first ? line_point - first : 0;
        row = line_screen_anchor - (window->head - ROWS) +
              (unsigned int)((line_screen_column + at) / COLUMNS);
        column = (unsigned int)((line_screen_column + at) % COLUMNS);

        if (row >= ROWS)
        {
                row = ROWS - 1;
                column = COLUMNS - 1;
        }
}

/*
        Take the editable line out of the old grid before putting it into a
        new one.

        Merely drawing it again after COLUMNS changed left the old cells in
        the ring as well. Canvas then folded those old cells and the newly
        laid-out copy appeared a second time. The prompt is everything before
        the anchor and stays; rows occupied only by the editable line become
        empty by shortening their lengths, which is also what keeps blank
        folded rows from consuming the resized view.
*/
static fn line_erase(b32 shorten)
{
        unsigned int r, c, top;
        positive left, skipped = 0;
        bipolar screen_row;

        if (!line_anchored || !line_drawn)
                return;

        top = window->head - ROWS;
        screen_row = (bipolar)line_screen_anchor - (bipolar)top;
        c = line_screen_column;

        if (screen_row < 0)
        {
                positive past = (positive)(-screen_row) * COLUMNS;

                skipped = past > c ? past - c : 0;
                screen_row = 0;
                c = 0;
        }

        if (skipped >= line_drawn || screen_row >= (bipolar)ROWS)
                return;

        r = (unsigned int)screen_row;
        left = line_drawn - skipped;

        while (left && r < ROWS)
        {
                unsigned int room = COLUMNS - min(c, COLUMNS);
                positive taken = left < room ? left : room;
                unsigned int address_to length = row_length(r);

                if (shorten && address_to length > c)
                        address_to length = c;
                else if (!shorten)
                        cells_clear(r, c, (unsigned int)taken);

                touch(r);
                left -= taken;
                r++;
                c = 0;

                // A full last column wraps only when another character is
                // written. There is no next row to remove in that case.
                if (!room)
                        break;
        }
}

static fn line_insert(unsigned int character)
{
        if (!array_store_reserve(line, line_room, line_length,
                                 line_length + 1, 64))
                return;

        memory_copy(line + line_point + 1, line + line_point,
                    line_length - line_point);

        line[line_point++] = (p8)character;
        line_length++;
}

static fn line_take(positive from, positive to)
{
        positive gone = to - from;

        memory_copy(line + from, line + to, line_length - to);

        line_length -= gone;
        line_point = from;
}

// The word before the cursor is the run of spaces then the run that is not.
static PURE positive line_word()
{
        positive at = line_point;

        while (at && line[at - 1] == ' ')
                at--;

        while (at && line[at - 1] != ' ')
                at--;

        return at;
}

static fn line_from_history()
{
        const p8 address_to from = history_at < history_count
                                       ? history[history_at % LINE_HISTORY]
                                       : history_held;
        positive length = history_at < history_count
                              ? history_length[history_at % LINE_HISTORY]
                              : history_held_length;

        if (!array_store_reserve(line, line_room, line_length, length, 64))
                return;

        memory_copy_apart(line, (address_any)from, length);

        line_length = length;
        line_point = length;
}

static fn line_remember()
{
        unsigned int slot;

        if (!line_length)
                return;

        // The same command twice running is one entry, which is what makes
        // the up arrow worth pressing after a loop of them.
        if (history_count &&
            history_length[(history_count - 1) % LINE_HISTORY] == line_length &&
            history[(history_count - 1) % LINE_HISTORY][0] == line[0] &&
            (line_length == 1 ||
             !memory_compare(history[(history_count - 1) % LINE_HISTORY], line,
                             line_length)))
                return;

        slot = history_count % LINE_HISTORY;

        if (!array_store_reserve(history[slot], history_room[slot],
                                 history_length[slot], line_length, 64))
                return;

        memory_copy_apart(history[slot], line, line_length);

        history_length[slot] = line_length;
        history_count++;
}

/*
        The screen, cleared, with the prompt kept.

        What is to the left of where the line began is the prompt, and it is
        the only part of it this ever knows: the shell wrote it and never said
        what it was. Copying those cells to the top of the cleared screen is
        what makes Ctrl-L put back a prompt nobody told us the text of.
*/
static fn line_clear_screen()
{
        unsigned int kept = line_anchored
                                ? min((unsigned int)line_prompt_length, COLUMNS)
                                : 0;

        erase(0, 0, ROWS - 1, COLUMNS - 1);

        if (kept)
        {
                reach(0, kept);
                memory_copy_apart(row_cells(0), line_prompt,
                                  kept * sizeof(line_prompt[0]));
        }

        line_anchor = window->head - ROWS;
        line_anchor_column = kept;
        line_anchored = true;
        line_drawn = 0;
        touch_all();
}

/*
        The line is over, however it ended.

        What was typed stays on the screen -- it is the record of what was
        asked for, and both the shell's answer and the next prompt go under
        it -- so the cursor is put past the end of what was drawn and taken
        down one from there.
*/
// Nothing typed, the cursor at its start, and history back at the bottom.
static fn line_reset()
{
        line_length = 0;
        line_point = 0;
        history_at = history_count;
        history_held_length = 0;
        line_forget();
}

static fn line_done()
{
        row = line_anchor_row();
        column = line_anchor_column + line_drawn;

        while (column >= COLUMNS)
        {
                column -= COLUMNS;
                row++;
        }

        if (row >= ROWS)
                row = ROWS - 1;

        line_feed();
        column = 0;
        touch(row);
        line_reset();
}

static fn line_accept()
{
        line_remember();

        emit_bytes(line, line_length);

        emit('\n');
        line_done();
}

// Linux calls them these, and the compositor hands over the number rather than
// a sequence, so the editor reads the same key the terminal would encode.
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_DELETE 111

static b32 line_key(unsigned int character, unsigned int code)
{
        if (!line_anchored)
        {
                positive prompt;

                line_anchor = window->head - ROWS + row;
                line_anchor_column = column < COLUMNS ? column : COLUMNS - 1;
                prompt = line_anchor_column;

                /*
                        Capture the prompt while its row is unquestionably
                        still the anchor row.  A command taller than the
                        viewport scrolls that row into history; waiting until
                        Ctrl-L copied whatever happened to be at row zero and
                        restored two command characters as the prompt.
                */
                if (array_store_reserve(line_prompt, line_prompt_room,
                                        line_prompt_length, prompt, 64))
                {
                        memory_copy_apart(line_prompt, row_cells(row),
                                          prompt * sizeof(*line_prompt));
                        line_prompt_length = prompt;
                }
                else
                        line_prompt_length = 0;

                line_anchored = true;
                line_drawn = 0;
        }

        switch (code)
        {
        case KEY_LEFT:
                character = 2;
                break;
        case KEY_RIGHT:
                character = 6;
                break;
        case KEY_HOME:
                character = 1;
                break;
        case KEY_END:
                character = 5;
                break;
        case KEY_DELETE:
                if (line_point < line_length)
                        line_take(line_point, line_point + 1);
                return true;
        case KEY_UP:
        {
                positive first = history_count > LINE_HISTORY
                                     ? history_count - LINE_HISTORY
                                     : 0;

                if (history_at <= first)
                        return true;

                if (history_at == history_count)
                {
                        if (!array_store_reserve(
                                history_held, history_held_room,
                                history_held_length, line_length, 64))
                                return true;

                        memory_copy_apart(history_held, line, line_length);

                        history_held_length = line_length;
                }

                history_at--;
                line_from_history();
                return true;
        }
        case KEY_DOWN:
                if (history_at >= history_count)
                        return true;

                history_at++;
                line_from_history();
                return true;
        }

        /*
                A key that means no character and that the editor has no use
                for is nothing at all.

                Its sequence would otherwise be sent, and the line discipline
                puts the bytes of it in the line: pressing Page Up at a prompt
                ran a command with an escape, a bracket and a tilde in it.
        */
        if (!character)
                return true;

        switch (character)
        {
        case 1:
                line_point = 0;
                return true;
        case 2:
                if (line_point)
                        line_point--;
                return true;
        case 5:
                line_point = line_length;
                return true;
        case 6:
                if (line_point < line_length)
                        line_point++;
                return true;
        case 8:
        case 127:
                if (line_point)
                        line_take(line_point - 1, line_point);
                return true;
        case 4:
                /*
                        End of file, but only with nothing to delete.

                        On a line with text after the cursor it is the forward
                        delete, which is what every editor with these bindings
                        has done since they were named; on an empty line it is
                        the byte that ends the shell, and the line discipline
                        is what turns it into one.
                */
                if (line_length)
                {
                        if (line_point < line_length)
                                line_take(line_point, line_point + 1);

                        return true;
                }

                emit(4);
                return true;
        case 11:
                if (line_point < line_length)
                        line_take(line_point, line_length);
                return true;
        case 21:
                if (line_point)
                        line_take(0, line_point);
                return true;
        case 23:
                if (line_point)
                        line_take(line_word(), line_point);
                return true;
        case 12:
                line_clear_screen();
                return true;
        case '\r':
        case '\n':
                line_accept();
                return true;
        case 3:
                // The signal is the line discipline's to raise, so the byte
                // still goes. What was typed is left where it is, the way a
                // line that was run would be, and abandoned.
                emit(3);
                line_done();
                return true;
        }

        /*
                A control character the editor has no binding for is nothing.

                It used to be sent, and the line discipline kept it in front of
                the line still being edited, so a stray Control-X made the next
                command one that was not found. The two the discipline turns
                into signals still go, and a tab is part of the line.
        */
        if (character == '\t')
        {
                line_insert(character);
                return true;
        }

        if (character < ' ')
        {
                if (character == 26 || character == 28)
                        emit(character);
                return true;
        }

        line_insert(character);
        return true;
}

/*
        A key that is no character is still a key.

        Arrows, Home, the function keys: a program is handed the code and
        nothing else, and a terminal owes its shell the sequence ANSI names
        for each of them. TERM=xterm-256color is exported below, and this is
        what makes that true rather than a claim.
*/
static const struct
{
        unsigned int code;
        const char address_to sequence;
        const char address_to application;
} key_sequences[] = {
    {KEY_UP, "\x1b[A", "\x1bOA"}, {KEY_DOWN, "\x1b[B", "\x1bOB"},
    {KEY_RIGHT, "\x1b[C", "\x1bOC"}, {KEY_LEFT, "\x1b[D", "\x1bOD"},
    {KEY_HOME, "\x1b[H", "\x1bOH"}, {KEY_END, "\x1b[F", "\x1bOF"},
    {104, "\x1b[5~", 0}, {109, "\x1b[6~", 0},
    {110, "\x1b[2~", 0}, {KEY_DELETE, "\x1b[3~", 0},
    {59, "\x1bOP", 0}, {60, "\x1bOQ", 0}, {61, "\x1bOR", 0}, {62, "\x1bOS", 0},
    {63, "\x1b[15~", 0}, {64, "\x1b[17~", 0}, {65, "\x1b[18~", 0}, {66, "\x1b[19~", 0},
    {67, "\x1b[20~", 0}, {68, "\x1b[21~", 0}, {87, "\x1b[23~", 0}, {88, "\x1b[24~", 0},
};

static string_address key_sequence(unsigned int code)
{
        for (positive i = 0; i < array_count(key_sequences); i++)
                if (key_sequences[i].code == code)
                        return (string_address)(application_keys && key_sequences[i].application
                                                    ? key_sequences[i].application
                                                    : key_sequences[i].sequence);

        return null;
}

/*
        One keystroke, turned into whatever it means.

        With the line editor running that is a change to the buffer and what
        is drawn; without it, it is the byte or the sequence going straight
        out, which is all this ever did.
*/
static fn SPARE term_key_modified(unsigned int character, unsigned int code,
                                  unsigned int modifiers)
{
        string_address sequence;
        unsigned int held = modifiers &
            (WINDOW_KEY_SHIFT | WINDOW_KEY_ALT | WINDOW_KEY_CONTROL);

        if (line_editing && line_key(character, code))
        {
                line_show();
                return;
        }

        if (character)
        {
                // Shift+Tab is a key of its own, kcbt, and sending a tab left
                // the far end nothing to tell the two apart by.
                if (character == '\t' && (held & WINDOW_KEY_SHIFT))
                {
                        emit_literal("\x1b[Z");
                        return;
                }

                if (held & WINDOW_KEY_ALT)
                        emit(27);

                emit(character);
                return;
        }

        sequence = key_sequence(code);

        if (!sequence)
                return;

        /*
                The same sequence, with what was held written into it.

                ECMA-48 leaves this to the terminal and xterm settled it: the
                second parameter is one plus the sum of shift, alt and control,
                so Shift+Right is CSI 1;2C and Ctrl+Right is CSI 1;5C. A
                sequence that ends in a tilde takes the parameter before the
                tilde instead, which is why the two are built separately.

                Without this a program on the far end cannot tell Shift+Right
                from Right, because nothing in the bytes says. The compositor
                has known which modifiers were down since keys.c read them; it
                is only this last step that threw the answer away.
        */
        if (held)
        {
                unsigned int value =
                    1 + ((held & WINDOW_KEY_SHIFT) ? 1 : 0) +
                    ((held & WINDOW_KEY_ALT) ? 2 : 0) +
                    ((held & WINDOW_KEY_CONTROL) ? 4 : 0);
                positive length = string_length((string_address)sequence);

                if (length > 2 && sequence[1] == '[' &&
                    sequence[length - 1] == '~')
                {
                        emit_bytes((address_any)sequence, length - 1);
                        emit(';');
                        emit('0' + value);
                        emit('~');
                        return;
                }

                if (length == 3 && (sequence[1] == '[' || sequence[1] == 'O'))
                {
                        emit(27);
                        emit('[');
                        emit('1');
                        emit(';');
                        emit('0' + value);
                        emit(sequence[2]);
                        return;
                }
        }

        emit_bytes((address_any)sequence,
                   string_length((string_address)sequence));
}

// The test fixture and any caller without compositor modifiers use the
// ordinary two-argument spelling.
static fn SPARE term_key(unsigned int character, unsigned int code)
{
        term_key_modified(character, code, 0);
}

/*
        Whether the far end is assembling lines itself.

        ICANON off is a program that wants its own keys, and handing it a line
        at a time would be handing it nothing at all until Enter. So the
        editor is exactly as on as the line discipline it stands in for.
*/
static fn SPARE term_line_editing(b32 on)
{
        if (line_editing == on)
                return;

        line_editing = on;
        line_reset();
}

static fn term_pointer(unsigned int col, unsigned int row,
                       unsigned int button, unsigned int flags)
{
        unsigned int code;
        b32 down = (flags & WINDOW_KEY_DOWN) != 0;
        b32 move = (flags & WINDOW_KEY_POINTER_MOVE) != 0;

        if (!mouse_mode)
                return;
        if (move && mouse_mode == 1000)
                return;
        if (move && mouse_mode == 1002 && !down)
                return;
        if (!col)
                col = 1;
        if (!row)
                row = 1;

        code = button;
        if (move)
                code += 32;

        if (mouse_sgr)
        {
                emit_literal("\x1b[<");
                positive_to_string(emit_bytes, code);
                emit(';');
                positive_to_string(emit_bytes, col);
                emit(';');
                positive_to_string(emit_bytes, row);
                emit((down || move) ? 'M' : 'm');
                return;
        }

        if (col > 223)
                col = 223;
        if (row > 223)
                row = 223;
        if (!down && !move)
                code = 3;
        emit(27);
        emit('[');
        emit('M');
        emit((p8)(32 + code));
        emit((p8)(32 + col));
        emit((p8)(32 + row));
}

static fn term_focus(b32 in)
{
        if (!focus_events)
                return;

        if (in)
                emit_literal("\x1b[I");
        else
                emit_literal("\x1b[O");
}

#endif

#ifndef KERNEL_MODE

#define F_GETFD 1

/*
        Making sure the first three descriptors are taken.

        A program the kernel started has none open at all, so the pty would be
        descriptor zero and its other end descriptor one -- and then handing
        those to a shell as its input and output means dup3 onto a descriptor
        that is already the thing being duplicated, and closing the master
        closes what was just set up. Started from a shell the three are
        already taken and none of this is visible, which is exactly why it was
        not.
*/
fn claim_standard_descriptors()
{
        for (b32 i = 0; i < 3; i++)
                if (system_call_3(syscall(fcntl), i, F_GETFD, 0) < 0)
                        system_open_at(AT_FDCWD, "/dev/null",
                                      FILE_READ_WRITE);
}

#endif

// The grid the window has room for. A window narrower or shorter than one
// cell is not a grid, and every wrap and scroll divides by these: zero rows had
// put wrapped forever looking for a row to land on.
static fn grid_take()
{
        COLUMNS = window->columns ? window->columns : 1;
        ROWS = window->rows ? window->rows : 1;

        if (COLUMNS > window->stride)
                COLUMNS = window->stride;
}

#ifndef KERNEL_MODE
// The pty is told the grid, so a program that asks has the size it is drawn
// in, and a resize reaches it as SIGWINCH.
static fn grid_tell(b32 master)
{
        winsize size = {(unsigned short)ROWS, (unsigned short)COLUMNS,
                        (unsigned short)(COLUMNS * WINDOW_CELL_W),
                        (unsigned short)(ROWS * WINDOW_CELL_H)};

        system_control(master, TIOCSWINSZ, address_of size);
}
#endif

// Where a row of the screen is once it is ROWS tall instead of was_rows:
// anchored at the bottom, with added blank lines below everything it held.
static unsigned int regrid_row(unsigned int at, unsigned int was_rows,
                               unsigned int added)
{
        if (ROWS >= was_rows)
                at += ROWS - was_rows - added;
        else if (at >= was_rows - ROWS)
                at -= was_rows - ROWS;
        else
                at = 0;

        return at < ROWS ? at : ROWS - 1;
}

/*
        The window was resized.

        Nothing is copied and nothing moves. The rows are the last lines of the
        ring whatever there are of them, so a window made taller takes in the
        lines that had scrolled off the top rather than blank ones, and the
        cursor is still on the line it was on -- that many rows further down.

        What was on the screen stays on it. Lines are not moved or shortened
        here: the compositor folds a stored line at the width it is drawn in,
        so narrowing and then widening a window is lossless. The pty learns
        the new grid below and future output uses it; an application that owns
        the screen can then redraw in response to SIGWINCH.
*/
fn regrid(b32 master)
{
        unsigned int was_rows = ROWS;
        unsigned int added = 0;
#ifndef KERNEL_MODE
        b32 cursor_was_shown = shown;
#endif

        /* A resize is itself a synchronous publication in the new geometry.
           End any program-held frame first; SIGWINCH will ask a full-screen
           program to begin and draw another transaction at the new size. */
        synchronized_output = false;

        // Undone in the geometry it was made in. cursor_show inverts a cell in
        // place, so clearing "shown" and moving on left that cell inverted for
        // good.
        cursor_hide();

#ifndef KERNEL_MODE
        if (line_editing)
                line_erase(true);
#endif

        grid_take();

        /*
                The alternate screen is the lines after alternate_head, and a
                taller window must not reach back past it into the primary
                screen it keeps: that put the shell's last lines above a
                full-screen program's picture, and left the picture on the
                shell's screen once the program ended. The new rows are blank
                lines at the bottom, and the cursor does not move for them.
        */
        if (alternate)
                while (window->head - alternate_head < ROWS)
                {
                        window_scroll(window);
                        added++;
                }

        row = regrid_row(row, was_rows, added);
        cursor_saved.row = regrid_row(cursor_saved.row, was_rows, added);

        if (alternate)
                cursor_primary.row = regrid_row(cursor_primary.row, was_rows, 0);

        if (column >= COLUMNS)
                column = COLUMNS - 1;

        // The anchor of the line being typed is a column too, and a narrower
        // window has fewer of them to be at.
        if (line_anchor_column >= COLUMNS)
                line_anchor_column = COLUMNS - 1;

        // A region is measured in rows that may no longer be there.
        region_top = 0;
        region_bottom = ROWS;

        // The line being typed is anchored to a line of the ring, so it
        // survives the resize. Redraw it now at the new width: waiting for the
        // next keystroke left the cursor and the editable text in the old
        // geometry even though the window had already changed underneath it.
        line_drawn = 0;
#ifndef KERNEL_MODE
        line_view = 0;
#endif

#ifndef KERNEL_MODE
        if (line_editing)
                line_show();
#endif

        shown = false;
        touch_all();

        window_grid(window, COLUMNS, ROWS);

#ifndef KERNEL_MODE
        /*
                A resize is one transaction, including the cursor.

                Leaving it hidden here and relying on the caller's ordinary
                end-of-loop redraw made the resize path the only state in
                which the cursor could remain absent until some later input.
                Publish the new grid, put the cursor into that grid, damage
                it, and synchronously commit before returning. master is -1
                in the pure emulator harness and in the kernel console; both
                have an owner that paints their cells directly.
        */
        if (master >= 0 || cursor_was_shown)
                cursor_show();

        if (master >= 0)
        {
                window_damage(window, 0, ROWS);
                window_flush(window);

                // That damage was consumed by the synchronous commit. Keep
                // later input in this same loop iteration visible, but do not
                // manufacture a second cursor-only flush at the loop tail.
                touched_top = ROWS;
                touched_bottom = 0;
        }
#endif

#ifndef KERNEL_MODE
        grid_tell(master);
#endif
}
