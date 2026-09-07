/* Exact production ARM64 decimal-record loop, checked against libc output. */
#include "series.h"
#include <stdio.h>
#include <string.h>

unsigned long memory_decimal_series(unsigned char *, unsigned long,
                                    unsigned long, unsigned long,
                                    unsigned long, long);

int main(void)
{
        static const long steps[] = {0, 1, -1, 3, -3, 25, -25, 999, -999};
        unsigned char actual[2048], expected[2048];
        unsigned long checks = 0;
        for (unsigned width = 1; width <= 7; width++)
        {
                long limit = 1;
                for (unsigned i = 0; i < width; i++) limit *= 10;
                for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
                        for (unsigned offset = 0; offset < 16; offset++)
                                for (unsigned room = width + 3; room <= 1024; room += 17)
                                {
                                        long value = steps[s] < 0 ? limit - 1 : 0;
                                        unsigned length = width + 3, used = 0;
                                        memset(actual, 0xa5, sizeof(actual));
                                        memset(expected, 0xa5, sizeof(expected));
                                        char record[32];
                                        while (room - used >= length && value >= 0 && value < limit)
                                        {
                                                snprintf(record, sizeof(record), "[%0*ld]\n", width, value);
                                                memcpy(expected + offset + used, record, length);
                                                used += length;
                                                value += steps[s];
                                        }
                                        memcpy(actual + offset, expected + offset, length);
                                        unsigned long got = memory_decimal_series(actual + offset,
                                            room, length, 1, width + 1, steps[s]);
                                        checks++;
                                        if (got != used || memcmp(actual + offset, expected + offset, used))
                                                return printf("series mismatch width=%u step=%ld room=%u\n",
                                                              width, steps[s], room), 1;
                                        for (unsigned i = 0; i < sizeof(actual); i++)
                                                if ((i < offset || i >= offset + room) && actual[i] != 0xa5)
                                                        return printf("series bounds\n"), 1;
                                }
        }
        printf("native decimal series: %lu checks, 0 failures\n", checks);
        return 0;
}
