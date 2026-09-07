/* Body-to-body timing, separate from the shell's streaming benchmark. The C
   references retain the folded engine's runtime-width bit loops. QEMU runs
   establish correctness only; their ticks are not hardware measurements. */
#include "../src/compiler_memory.c"

#define NO_INLINE __attribute__((noinline, noclone))
static p8 input[5120], encoded[8192], decoded[5120], values[256];
static volatile positive sink;

NO_INLINE static positive encode_c(p8 address_to into, p8 address_to source,
                                   positive groups, string_address alphabet,
                                   positive width)
{
        positive bits = width == 9 ? 1 : width;
        positive bytes = bits == 6 ? 3 : bits == 5 ? 5 : 1;
        positive symbols = bytes * 8 / bits;
        for (positive group = 0; group < groups; group++)
        {
                positive value = 0;
                for (positive at = 0; at < bytes; at++)
                        value = (value << 8) | *source++;
                for (positive at = 0; at < symbols; at++)
                {
                        positive shift = width == 9 ? at : (symbols - at - 1) * bits;
                        *into++ = alphabet[(value >> shift) & (((positive)1 << bits) - 1)];
                }
        }
        return groups;
}

NO_INLINE static positive decode_c(p8 address_to into, p8 address_to source,
                                   positive groups, address_any table,
                                   positive width)
{
        positive bits = width == 9 ? 1 : width;
        positive bytes = bits == 6 ? 3 : bits == 5 ? 5 : 1;
        positive symbols = bytes * 8 / bits;
        p8 address_to map = table;
        positive accumulator = 0, held = 0;
        for (positive at = 0; at < groups * symbols; at++)
        {
                p8 value = map[source[at]];
                if (value == 255) return at / symbols;
                accumulator = width == 9 ? accumulator | ((positive)value << held)
                    : (accumulator << bits) | value;
                held += bits;
                if (held >= 8)
                {
                        held -= 8;
                        *into++ = (p8)(accumulator >> held);
                        accumulator &= ((positive)1 << held) - 1;
                }
        }
        return groups;
}

typedef positive (*codec_function)(address_any, address_any, positive,
                                    address_any, positive);
static positive measured(codec_function function, address_any into,
                          address_any source, positive groups,
                          address_any table, positive bits)
{
        positive best = positive_max;
        for (positive trial = 0; trial < 7; trial++)
        {
                positive start = get_cpu_time();
                for (positive round = 0; round < 2048; round++)
                        sink += function(into, source, groups, table, bits);
                positive elapsed = get_cpu_time() - start;
                if (elapsed < best) best = elapsed;
        }
        sink += ((p8 address_to)into)[0];
        return best;
}

b32 main(void)
{
        const char *alphabets[] = {
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567", "0123456789ABCDEFGHIJKLMNOPQRSTUV",
            "0123456789ABCDEF", "01", "01",
        };
        p8 widths[] = {6, 6, 5, 5, 4, 1, 9};
        for (positive at = 0; at < sizeof(input); at++)
                input[at] = (p8)(at * 197 + at / 97);
#if X64
        p8 vector = cpu_has_avx2;
        p8 wide = cpu_has_avx512, vbmi = cpu_has_avx512_vbmi;
        for (positive mode = 0; mode < 3; mode++)
        {
                cpu_has_avx2 = mode ? vector : 0;
                cpu_has_avx512 = mode > 1 ? wide : 0;
                cpu_has_avx512_vbmi = mode > 1 ? vbmi : 0;
                string_format(log, "avx2=%p avx512=%p vbmi=%p\n", (positive)cpu_has_avx2,
                              (positive)cpu_has_avx512, (positive)cpu_has_avx512_vbmi);
#endif
        for (positive kind = 0; kind < sizeof(widths); kind++)
        {
                positive bits = widths[kind] == 9 ? 1 : widths[kind];
                memory_fill(values, 255, sizeof(values));
                for (positive at = 0; at < ((positive)1 << bits); at++)
                        values[(p8)alphabets[kind][at]] = (p8)at;
                for (positive groups = 1; groups <= 1024; groups *= 32)
                {
                        positive before = measured((codec_function)encode_c, encoded,
                            input, groups, (address_any)alphabets[kind], widths[kind]);
                        positive after = measured((codec_function)memory_encode_power2,
                            encoded, input, groups, (address_any)alphabets[kind], widths[kind]);
                        string_format(log, "encode kind=%p groups=%p C=%p ASM=%p ticks\n",
                                      kind, groups, before, after);
                        before = measured((codec_function)decode_c, decoded, encoded,
                                          groups, values, widths[kind]);
                        after = measured((codec_function)memory_decode_power2, decoded,
                                         encoded, groups, values, widths[kind]);
                        string_format(log, "decode kind=%p groups=%p C=%p ASM=%p ticks\n",
                                      kind, groups, before, after);
                        log_flush();
                }
        }
#if X64
        }
        cpu_has_avx2 = vector;
        cpu_has_avx512 = wide;
        cpu_has_avx512_vbmi = vbmi;
#endif
        return 0;
}
