#include "../compiler_memory.c"
#include "counted.inc"

// Independent scalar oracle: decode into a code point, then reject overlong,
// surrogate and out-of-range values. Production validates the byte ranges.
static positive utf8_reference_width(p8 address_to data, positive size)
{
        if (!size)
                return 0;
        p32 first = data[0], value, least;
        positive width;
        if (first < 0x80)
                return 1;
        if ((first & 0xe0) == 0xc0)
                width = 2, value = first & 31, least = 0x80;
        else if ((first & 0xf0) == 0xe0)
                width = 3, value = first & 15, least = 0x800;
        else if ((first & 0xf8) == 0xf0)
                width = 4, value = first & 7, least = 0x10000;
        else
                return 1;
        if (size < width)
                return 1;
        for (positive at = 1; at < width; at++)
        {
                if ((data[at] & 0xc0) != 0x80)
                        return 1;
                value = (value << 6) | (data[at] & 63);
        }
        return value < least || value > 0x10ffff ||
                       (value >= 0xd800 && value <= 0xdfff) ? 1 : width;
}

static fn utf8_span_check(p8 address_to bytes, positive size, positive count)
{
        positive at = 0, characters = 0;
        while (at < size && characters < count)
        {
                at += utf8_reference_width(bytes + at, size - at);
                characters++;
        }
        positive2 got = memory_utf8_span(bytes, size, count);
        check("bounded UTF-8 span", got.x == at && got.y == characters);
}

b32 main()
{
        const p8 sample[] = {
            'A', 'B', 0, 0x7f, 0xc2, 0x80, 0xdf, 0xbf,
            0xe0, 0xa0, 0x80, 0xed, 0x9f, 0xbf, 0xee, 0x80, 0x80,
            0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf,
            0xc0, 0x80, 0xc1, 0xbf, 0xe0, 0x9f, 0xbf,
            0xed, 0xa0, 0x80, 0xf0, 0x8f, 0xbf, 0xbf,
            0xf4, 0x90, 0x80, 0x80, 0xf5, 0x80, 0x80, 0x80,
            0xff, 0x80, 0xbf, 0xe2, 'x', 0x80, 0xf0, 0x90, 'x', 0x80
        };
        p8 bytes[160];
        positive random = 0x7f6a29c3;
        utf8_span_check(null, 0, positive_max);
        utf8_span_check(null, 9, 0);

        for (positive shape = 0; shape < 4; shape++)
                for (positive align = 0; align < 16; align++)
                {
                        for (positive i = 0; i < 128; i++)
                        {
                                random = random * 1664525 + 1013904223;
                                bytes[align + i] = shape == 0 ? 'a' :
                                    shape == 1 ? sample[i % sizeof(sample)] :
                                    shape == 2 ? (p8)(random >> 16) :
                                    i < 64 ? 'x' : sample[i % sizeof(sample)];
                        }
                        for (positive size = 0; size <= 128; size++)
                                for (positive count = 0; count <= size + 1; count++)
                                        utf8_span_check(bytes + align, size, count);
                }

        // Every lead/second-byte pair, including partial three/four-byte
        // sequences, distinguishes continuation checks and special bounds.
        for (positive lead = 0; lead < 256; lead++)
                for (positive second = 0; second < 256; second++)
                {
                        bytes[0] = lead; bytes[1] = second;
                        bytes[2] = 0x80; bytes[3] = 0x80;
                        for (positive size = 1; size <= 4; size++)
                                utf8_span_check(bytes, size, 1);
                }

        positive quantum = 65536;
        p8 address_to guarded = memory(quantum * 3);
        check("UTF-8 guard mapping", guarded && (positive)guarded < positive_max - 4095);
        if (guarded && (positive)guarded < positive_max - 4095)
        {
                check("UTF-8 left guard", system_call_3(syscall(mprotect),
                      (positive)guarded, quantum, 0) == 0);
                check("UTF-8 right guard", system_call_3(syscall(mprotect),
                      (positive)(guarded + quantum * 2), quantum, 0) == 0);
                for (positive size = 0; size <= 96; size++)
                {
                        p8 address_to tail = guarded + quantum * 2 - size;
                        for (positive i = 0; i < size; i++)
                                tail[i] = sample[i % sizeof(sample)];
                        for (positive count = 0; count <= size + 1; count++)
                                utf8_span_check(tail, size, count);
                }
                utf8_span_check(guarded, 0, 8);
                utf8_span_check(guarded, 8, 0);
                memory_free(guarded, quantum * 3);
        }
        return test_report(null);
}
