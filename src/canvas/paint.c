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
#define COLOUR_TEXT 0xdfe7ef
#define COLOUR_CURSOR 0xffffff
#define COLOUR_CURSOR_EDGE 0x000000

static void canvas_fill_rect(u32 *pixels, unsigned int pitch_pixels,
                             unsigned int target_w, unsigned int target_h,
                             int x, int y, int width, int height, u32 colour)
{
        int row;

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
                canvas_row_fill(pixels + (size_t)(y + row) * pitch_pixels + x,
                                (unsigned long)width, colour);
}

/*
        Cursors.

        One cell for every shape so a shape change is a different bitmap and
        nothing else -- same buffer, same damage, same hardware plane. X is the
        outline, . the fill, a space transparent.

        The hotspot is the pixel the pointer actually is. It is the corner for
        an arrow and the centre for the resize shapes, which is why it is per
        shape rather than one constant.
*/
#define CURSOR_W 16
#define CURSOR_H 20

#define CURSOR_ARROW 0
#define CURSOR_RESIZE_H 1
#define CURSOR_RESIZE_V 2
#define CURSOR_RESIZE_NWSE 3
#define CURSOR_RESIZE_NESW 4
#define CURSOR_SHAPES 5

static const char canvas_cursors[CURSOR_SHAPES][CURSOR_H][CURSOR_W + 1] = {
    {
    "X               ",
    "XX              ",
    "X.X             ",
    "X..X            ",
    "X...X           ",
    "X....X          ",
    "X.....X         ",
    "X......X        ",
    "X.......X       ",
    "X........X      ",
    "X.....XXXXX     ",
    "X..X..X         ",
    "X.X X..X        ",
    "XX  X..X        ",
    "X    X..X       ",
    "     X..X       ",
    "      X.X       ",
    "      XXX       ",
    "                ",
    "                ",
    },
    {
    "                ",
    "                ",
    "                ",
    "    X     X     ",
    "   XX     XX    ",
    "  X.X     X.X   ",
    " X..XXXXXXX..X  ",
    "X.............X ",
    " X..XXXXXXX..X  ",
    "  X.X     X.X   ",
    "   XX     XX    ",
    "    X     X     ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "       X        ",
    "      X.X       ",
    "     X...X      ",
    "    X.....X     ",
    "   XXXX.XXXX    ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "      X.X       ",
    "   XXXX.XXXX    ",
    "    X.....X     ",
    "     X...X      ",
    "      X.X       ",
    "       X        ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "                ",
    " XXXXXXX        ",
    " X....X         ",
    " X...X          ",
    " X..XX          ",
    " X.XX.X         ",
    " XX  X.X        ",
    " X    X.X    X  ",
    "       X.X  XX  ",
    "        X.XX.X  ",
    "         XX..X  ",
    "         X...X  ",
    "        X....X  ",
    "       XXXXXXX  ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
    {
    "                ",
    "       XXXXXXX  ",
    "        X....X  ",
    "         X...X  ",
    "         XX..X  ",
    "        X.XX.X  ",
    "       X.X  XX  ",
    " X    X.X    X  ",
    " XX  X.X        ",
    " X.XX.X         ",
    " X..XX          ",
    " X...X          ",
    " X....X         ",
    " XXXXXXX        ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    "                ",
    },
};

static const int canvas_cursor_hot[CURSOR_SHAPES][2] = {
    {0, 0}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
};

static int cursor_hot_x(unsigned int shape)
{
        return canvas_cursor_hot[shape][0];
}

static int cursor_hot_y(unsigned int shape)
{
        return canvas_cursor_hot[shape][1];
}

static void canvas_draw_cursor(u32 *pixels, unsigned int pitch_pixels,
                               unsigned int target_w, unsigned int target_h,
                               int x, int y, unsigned int shape, unsigned int scale,
                               u32 fill, u32 edge)
{
        int row, column;
        unsigned int sx, sy;

        x -= cursor_hot_x(shape) * (int)scale;
        y -= cursor_hot_y(shape) * (int)scale;

        for (row = 0; row < CURSOR_H; row++)
        {
                for (column = 0; column < CURSOR_W; column++)
                {
                        char pixel = canvas_cursors[shape][row][column];
                        u32 colour;

                        if (pixel == ' ')
                                continue;

                        colour = pixel == 'X' ? edge : fill;

                        for (sy = 0; sy < scale; sy++)
                        {
                                int py = y + row * (int)scale + (int)sy;

                                if (py < 0 || py >= (int)target_h)
                                        continue;

                                for (sx = 0; sx < scale; sx++)
                                {
                                        int px = x + column * (int)scale + (int)sx;

                                        if (px < 0 || px >= (int)target_w)
                                                continue;

                                        pixels[(size_t)py * pitch_pixels + px] = colour;
                                }
                        }
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

