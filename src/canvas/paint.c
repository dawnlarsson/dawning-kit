/*
        Canvas -- paint

        Pixels, and nothing above them. Everything here takes a pointer, a
        pitch and a rectangle; none of it knows what a window or a screen is.
        That is what makes it worth its own file: it is the only part that can
        be read without the DRM stack in your head.
*/

// Colours are written as plain xrgb8888 and converted once per output.
#define COLOUR_DESKTOP 0x1b2733
#define COLOUR_FRAME 0x2f3f52
#define COLOUR_TITLE 0x2b3a4c
#define COLOUR_TITLE_FOCUSED 0x4c6785
#define COLOUR_BODY 0x101820
#define COLOUR_CURSOR 0xffffff
#define COLOUR_CURSOR_EDGE 0x000000

static void canvas_fill_rect(u32 *pixels, unsigned int pitch_pixels,
                             unsigned int target_w, unsigned int target_h,
                             int x, int y, int width, int height, u32 colour)
{
        int row, column;

        // Clip rather than trusting callers: a window dragged off the edge is
        // the normal case, not an error.
        if (x < 0)
        {
                width += x;
                x = 0;
        }

        if (y < 0)
        {
                height += y;
                y = 0;
        }

        if (x + width > (int)target_w)
                width = (int)target_w - x;

        if (y + height > (int)target_h)
                height = (int)target_h - y;

        if (width <= 0 || height <= 0)
                return;

        for (row = 0; row < height; row++)
        {
                u32 *line = pixels + (size_t)(y + row) * pitch_pixels + x;

                for (column = 0; column < width; column++)
                        line[column] = colour;
        }
}

/*
        An arrow, as a bitmap. Drawn in software for now; this is exactly what
        a hardware cursor plane exists to avoid, and moving it there is the
        next step.
*/
#define CURSOR_W 12
#define CURSOR_H 19

// The arrow sits in the top left of the cursor buffer; the rest is
// transparent, since the buffer is whatever size the device asked for.
static const char canvas_cursor_bitmap[CURSOR_H][CURSOR_W + 1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X.X   ",
    "      XXX   ",
    "            ",
};

static void canvas_draw_cursor(u32 *pixels, unsigned int pitch_pixels,
                               unsigned int target_w, unsigned int target_h,
                               int x, int y, u32 fill, u32 edge)
{
        int row, column;

        for (row = 0; row < CURSOR_H; row++)
        {
                int py = y + row;

                if (py < 0 || py >= (int)target_h)
                        continue;

                for (column = 0; column < CURSOR_W; column++)
                {
                        int px = x + column;
                        char pixel = canvas_cursor_bitmap[row][column];

                        if (pixel == ' ')
                                continue;

                        if (px < 0 || px >= (int)target_w)
                                continue;

                        pixels[(size_t)py * pitch_pixels + px] =
                            (pixel == 'X') ? edge : fill;
                }
        }
}

// xrgb8888 is the source of truth; argb differs only in the alpha byte.
static u32 canvas_colour(u32 xrgb, u32 format)
{
        if (format == DRM_FORMAT_ARGB8888)
                return xrgb | 0xff000000;

        return xrgb;
}

/*
        How far a row of a rounded rectangle is inset from its edge. Zero
        everywhere except within radius of the top and bottom, where it follows
        the circle those corners are quarters of.
*/
static int round_inset(int row, int height, int radius)
{
        int dy;

        if (radius <= 0)
                return 0;

        if (row < radius)
                dy = radius - 1 - row;
        else if (row >= height - radius)
                dy = radius - (height - row);
        else
                return 0;

        return radius - (int)int_sqrt((unsigned long)(radius * radius - dy * dy));
}

static void canvas_row_fill(u32 *pixels, unsigned int pitch_pixels,
                            int x1, int x2, int y, u32 colour)
{
        u32 *line = pixels + (size_t)y * pitch_pixels;
        int x;

        for (x = x1; x < x2; x++)
                line[x] = colour;
}

static void canvas_row_blit(u32 *pixels, unsigned int pitch_pixels,
                            int x1, int x2, int y, const u32 *source, u32 opaque)
{
        u32 *line = pixels + (size_t)y * pitch_pixels;
        int x;

        for (x = x1; x < x2; x++)
                line[x] = source[x - x1] | opaque;
}
