#include <stdint.h>
#include <stdio.h>

typedef unsigned long positive;
typedef long bipolar;
typedef unsigned char p8;

static p8 output[2048];
static positive used, calls, lengths[1024];

static void capture(void *data, positive length)
{
        p8 *bytes = data;
        lengths[calls++] = length;
        for (positive i = 0; i < length; i++)
                output[used++] = bytes[i];
}

positive positive_into(p8 *out, positive value)
{
        p8 reverse[24];
        positive n = 0;
        do { reverse[n++] = (p8)('0' + value % 10); value /= 10; } while (value);
        for (positive i = 0; i < n; i++) out[i] = reverse[n - i - 1];
        return n;
}

#include "base_field.h"

extern void positive_to_base_field(void (*)(void *, positive), positive,
                                   positive, positive, bipolar, positive);
extern void writer_fill(void (*)(void *, positive), positive, p8);

int main(void)
{
        for (positive base = 2; base <= 36; base++)
                for (positive flags = 0; flags < 8; flags++)
                {
                        used = calls = 0;
                        positive style = '+' | ((positive)'0' << 8) |
                            ((positive)'x' << 16) | ((positive)2 << 24) |
                            ((flags & 1) << 26) | (((flags >> 1) & 1) << 27) |
                            (((flags >> 2) & 1) << 28);
                        positive_to_base_field(capture, 0xfffffffffffffffful,
                                               base, 80, flags & 1 ? 25 : -1,
                                               style);
                        if (!used || !calls || used < 25)
                                return 1;
                        for (positive i = 0; i < calls; i++)
                                if (!lengths[i])
                                        return 2;
                }

        used = calls = 0;
        writer_fill(capture, 257, 0xa5);
        if (used != 257 || calls != 257)
                return 3;
        for (positive i = 0; i < 257; i++)
                if (output[i] != 0xa5 || lengths[i] != 1)
                        return 4;

        puts("  arm64 native base field: correct");
        return 0;
}
