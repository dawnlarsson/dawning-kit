#include "../src/compiler_memory.c"
#include "counted.inc"

static char high_alphabet[64];
static const char *alphabets[] = {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", "0123456789ABCDEFGHIJKLMNOPQRSTUV",
    "0123456789ABCDEF", "01", "01",
    "fedcba9876543210", "10", high_alphabet,
};
static const p8 widths[] = {6, 6, 5, 5, 4, 1, 9, 4, 9, 6};

static fn codec_check(p8 address_to source, p8 address_to output,
                      p8 address_to decoded, positive groups, positive kind)
{
        positive bits = widths[kind] == 9 ? 1 : widths[kind];
        positive bytes = bits == 6 ? 3 : bits == 5 ? 5 : 1;
        positive symbols = bytes * 8 / bits;
        p8 values[256];
        memory_fill(values, 255, sizeof(values));
        for (positive at = 0; at < ((positive)1 << bits); at++)
                values[(p8)alphabets[kind][at]] = (p8)at;
        check("codec encoded groups", memory_encode_power2(output, source,
              groups, (string_address)alphabets[kind], widths[kind]) == groups);
        bool valid = true;
        for (positive group = 0; group < groups; group++)
                for (positive at = 0; at < symbols; at++)
                {
                        positive value = 0;
                        for (positive bit = 0; bit < bits; bit++)
                        {
                                positive position = at * bits + bit;
                                p8 byte = source[group * bytes + position / 8];
                                positive shift = widths[kind] == 9 ? position & 7
                                    : 7 - (position & 7);
                                value = (value << 1) | ((byte >> shift) & 1);
                        }
                        if (output[group * symbols + at] !=
                            (p8)alphabets[kind][value])
                                valid = false;
                }
        check("codec independent bit oracle", valid);
        check("codec decoded groups", memory_decode_power2(decoded, output,
              groups, values, widths[kind]) == groups);
        check("codec round trip", !memory_compare(source, decoded, groups * bytes));
        if (kind == 4)
        {
                positive cases[] = {0, 1, (positive)1 << 32, (positive)1 << 63};
                for (positive at = 0; at < sizeof(cases) / sizeof(cases[0]); at++)
                {
                        check("hex case exact length", memory_into_hex_case(output,
                              source, groups, cases[at]) == groups * 2);
                        bool correct = true;
                        for (positive byte = 0; byte < groups * 2; byte++)
                        {
                                p8 nibble = (source[byte / 2] >> ((byte & 1) ? 0 : 4)) & 15;
                                p8 expected = nibble < 10 ? '0' + nibble
                                    : (cases[at] ? 'A' : 'a') + nibble - 10;
                                if (output[byte] != expected) correct = false;
                        }
                        check("hex case arithmetic oracle", correct);
                }
        }
}

b32 main(void)
{
        p8 source[400], output[640], decoded[400];
        for (positive at = 0; at < sizeof(high_alphabet); at++)
                high_alphabet[at] = (char)(192 + at);
#if X64
        p8 vector = cpu_has_avx2;
        p8 wide = cpu_has_avx512, vbmi = cpu_has_avx512_vbmi;
        for (positive mode = 0; mode < 4; mode++)
        {
                cpu_has_avx2 = mode ? vector : 0;
                cpu_has_avx512 = mode > 1 ? wide : 0;
                cpu_has_avx512_vbmi = mode > 2 ? vbmi : 0;
#endif
        for (positive kind = 0; kind < sizeof(widths); kind++)
        {
                positive bits = widths[kind] == 9 ? 1 : widths[kind];
                positive bytes = bits == 6 ? 3 : bits == 5 ? 5 : 1;
                positive symbols = bytes * 8 / bits;
                check("codec empty inaccessible encode", memory_encode_power2(
                      address_bad, address_bad, 0, address_bad, widths[kind]) == 0);
                check("codec empty inaccessible decode", memory_decode_power2(
                      address_bad, address_bad, 0, address_bad, widths[kind]) == 0);
                for (positive offset = 1; offset <= 16; offset++)
                        for (positive groups = 0; groups <= 64; groups++)
                        {
                                for (positive at = 0; at < sizeof(source); at++)
                                        source[at] = (p8)(at * 197 + groups * 47);
                                memory_fill(output, 0xa5, sizeof(output));
                                memory_fill(decoded, 0xa5, sizeof(decoded));
                                codec_check(source + offset, output + offset,
                                            decoded + offset, groups, kind);
                                bool valid = true;
                                for (positive at = 0; at < sizeof(output); at++)
                                        if ((at < offset || at >= offset + groups * symbols) &&
                                            output[at] != 0xa5) valid = false;
                                for (positive at = 0; at < sizeof(decoded); at++)
                                        if ((at < offset || at >= offset + groups * bytes) &&
                                            decoded[at] != 0xa5) valid = false;
                                check("codec exact output canaries", valid);
                        }
                p8 values[256];
                memory_fill(values, 255, sizeof(values));
                for (positive at = 0; at < ((positive)1 << bits); at++)
                        values[(p8)alphabets[kind][at]] = (p8)at;
                for (positive invalid = 0; invalid < 256; invalid++)
                        if (values[invalid] == 255)
                                for (positive at = 0; at < symbols * 16; at++)
                                {
                                        memory_fill(output, alphabets[kind][0], symbols * 16);
                                        memory_fill(decoded, 0xa5, sizeof(decoded));
                                        output[at] = (p8)invalid;
                                        positive done = memory_decode_power2(decoded,
                                            output, 16, values, widths[kind]);
                                        check("codec first malformed quantum", done == at / symbols);
                                        bool valid = true;
                                        for (positive byte = done * bytes; byte < sizeof(decoded); byte++)
                                                if (decoded[byte] != 0xa5) valid = false;
                                        check("codec malformed quantum untouched", valid);
                                }
        }
        for (positive kind = 0; kind < 32; kind++)
                if (kind != 1 && kind != 4 && kind != 5 && kind != 6 && kind != 9)
                {
                        check("codec invalid encode shape", memory_encode_power2(
                              address_bad, address_bad, 1, address_bad, kind) == 0);
                        check("codec invalid decode shape", memory_decode_power2(
                              address_bad, address_bad, 1, address_bad, kind) == 0);
                }

        const positive quantum = 65536;
        p8 address_to guarded[4] = {memory(quantum * 3), memory(quantum * 3),
                                   memory(quantum * 3), memory(quantum * 3)};
        bool mapped = true;
        for (positive at = 0; at < 4; at++)
                mapped &= guarded[at] && (positive)guarded[at] < positive_max - 4095;
        check("codec guard mappings", mapped);
        if (mapped)
        {
                for (positive at = 0; at < 4; at++)
                {
                        check("codec left guard", system_call_3(syscall(mprotect),
                              (positive)guarded[at], quantum, 0) == 0);
                        check("codec right guard", system_call_3(syscall(mprotect),
                              (positive)(guarded[at] + quantum * 2), quantum, 0) == 0);
                }
                for (positive kind = 0; kind < sizeof(widths); kind++)
                        for (positive groups = 0; groups <= 64; groups++)
                                for (positive edges = 0; edges < 8; edges++)
                                {
                                        positive bits = widths[kind] == 9 ? 1 : widths[kind];
                                        positive bytes = bits == 6 ? 3 : bits == 5 ? 5 : 1;
                                        positive symbols = bytes * 8 / bits;
                                        p8 address_to input = guarded[0] + ((edges & 1) ?
                                            quantum * 2 - groups * bytes : quantum);
                                        p8 address_to out = guarded[1] + ((edges & 2) ?
                                            quantum * 2 - groups * symbols : quantum);
                                        p8 address_to back = guarded[2] + ((edges & 4) ?
                                            quantum * 2 - groups * bytes : quantum);
                                        for (positive at = 0; at < groups * bytes; at++)
                                                input[at] = (p8)(at * 37 + groups);
                                        codec_check(input, out, back, groups, kind);
                                        p8 address_to map = guarded[3] + ((edges & 1) ?
                                            quantum * 2 - 256 : quantum);
                                        memory_fill(map, 255, 256);
                                        for (positive at = 0; at < ((positive)1 << bits); at++)
                                                map[(p8)alphabets[kind][at]] = (p8)at;
                                        check("codec exact table guard", memory_decode_power2(
                                              back, out, groups, map, widths[kind]) == groups);
                                        check("codec table guard decoded bytes",
                                              !memory_compare(back, input, groups * bytes));
                                }
        }
        for (positive at = 0; at < 4; at++)
                if (guarded[at] && (positive)guarded[at] < positive_max - 4095)
                        memory_free(guarded[at], quantum * 3);
#if X64
        }
        cpu_has_avx2 = vector;
        cpu_has_avx512 = wide;
        cpu_has_avx512_vbmi = vbmi;
#endif
        return test_report(null);
}
