/* Exact lifted ARM64 compact binary-size formatter on native Apple silicon. */
#include "human.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64 positive_into_human_1024_string(u8 *, u64);
void positive_to_human_1024(void (*)(void *, u64), u64);
int printf(const char *, ...);

static u64 reference_into(u8 *into, u64 value)
{
        u8 reverse[24];
        u64 length = 0;

        do {
                reverse[length++] = (u8)('0' + value % 10);
                value /= 10;
        } while (value);

        for (u64 i = 0; i < length; i++) into[i] = reverse[length - i - 1];
        return length;
}

// The two relocations the lifted formatter reaches on its decimal paths.
__attribute__((noinline)) u64 positive_into(u8 *into, u64 value)
{
        return reference_into(into, value);
}
__attribute__((noinline)) u64 positive_into_string(u8 *into, u64 value)
{
        u64 length = reference_into(into, value);
        into[length] = 0;
        return length;
}
void positive_to_string(void (*write)(void *, u64), u64 value)
{
        u8 digits[24];
        u64 length = reference_into(digits, value);
        write(digits, length);
}

static __attribute__((noinline)) u64 former_human(u8 *into, u64 value)
{
        static u8 units[] = "BKMGTPE";
        u64 divisor = 1, unit = 0;

        while (value / divisor >= 1024 && unit < 6) {
                divisor *= 1024;
                unit++;
        }

        if (!unit) return positive_into_string(into, value);

        u64 quotient = value / divisor;
        u64 remainder = value % divisor;
        u64 whole = quotient + (remainder != 0);
        u64 length;

        if (whole >= 10)
                length = reference_into(into, whole);
        else {
                u64 tenths = quotient * 10 +
                    (remainder * 10 + divisor - 1) / divisor;
                length = reference_into(into, tenths / 10);
                into[length++] = '.';
                length += reference_into(into + length, tenths % 10);
        }

        into[length++] = units[unit];
        into[length] = 0;
        return length;
}

static u64 seed = 0x9e3779b97f4a7c15ul;
static u64 next(void)
{
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
}

static u8 written[8];
static u64 written_length;
static u64 writer_calls;
static u64 call_lengths[4];

static void capture(void *data, u64 length)
{
        u8 *bytes = data;
        if (writer_calls < 4) call_lengths[writer_calls] = length;
        writer_calls++;
        for (u64 i = 0; i < length && written_length < sizeof(written); i++)
                written[written_length++] = bytes[i];
}

static int one(u64 value, u64 offset)
{
        u8 got[48], want[48];
        for (u64 i = 0; i < sizeof(got); i++) got[i] = want[i] = 0xa5;

        u64 gl = positive_into_human_1024_string(got + 16 + offset, value);
        u64 wl = former_human(want + 16 + offset, value);

        if (gl != wl) return 1;
        for (u64 i = 0; i < sizeof(got); i++) if (got[i] != want[i]) return 1;

        written_length = writer_calls = 0;
        positive_to_human_1024(capture, value);
        if (written_length != wl) return 1;
        for (u64 i = 0; i < wl; i++) if (written[i] != want[16 + offset + i]) return 1;

        int fractional = wl == 4 && want[17 + offset] == '.';
        int scaled = want[16 + offset + wl - 1] > '9';
        u64 expected = fractional ? 4 : (scaled ? 2 : 1);
        if (writer_calls != expected) return 1;
        if (fractional) {
                for (u64 i = 0; i < 4; i++) if (call_lengths[i] != 1) return 1;
        } else if (scaled) {
                if (call_lengths[0] != wl - 1 || call_lengths[1] != 1) return 1;
        } else if (call_lengths[0] != wl) return 1;
        return 0;
}

static int correctness(void)
{
        u64 checks = 0, bad = 0;

        for (u64 value = 0; value <= 65535; value++) {
                bad += one(value, value & 15);
                checks++;
        }

        u64 divisor = 1024;
        for (u64 unit = 1; unit <= 6; unit++) {
                u64 values[] = {
                    divisor - 1, divisor, divisor + 1,
                    9 * divisor - 1, 9 * divisor, 9 * divisor + 1,
                    10 * divisor - 1, 10 * divisor, 10 * divisor + 1,
                };
                for (u64 i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
                        bad += one(values[i], (i + unit) & 15);
                        checks++;
                }
                if (divisor <= (~0ul) / 1024) divisor *= 1024;
        }

        divisor = 1ul << 60;
        u64 tops[] = {15 * divisor, 15 * divisor + 1, ~0ul - 1, ~0ul};
        for (u64 i = 0; i < sizeof(tops) / sizeof(tops[0]); i++) {
                bad += one(tops[i], i);
                checks++;
        }

        for (u64 i = 0; i < 100000; i++) {
                bad += one(next(), i & 15);
                checks++;
        }

        printf("arm64 compact human: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}

#define COUNT 64
#define ROUNDS (1ul << 18)
#define TRIES 7

static u64 values[4][COUNT];
static u8 output[8];
static volatile u64 sink;

static u64 ticks(void)
{
        u64 value;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
}

static u64 run_once(u64 shape, int assembly)
{
        u64 start = ticks();
        for (u64 r = 0; r < ROUNDS; r++) {
                u64 value = values[shape][r & (COUNT - 1)];
                u64 length = assembly ? positive_into_human_1024_string(output, value)
                                      : former_human(output, value);
                sink += length + output[0] + output[length - 1];
        }
        return ticks() - start;
}

static void row(const char *name, u64 shape)
{
        u64 old = ~0ul, assembly = ~0ul;
        for (u64 t = 0; t < TRIES; t++) {
                u64 got_old, got_assembly;
                if (t & 1) {
                        got_assembly = run_once(shape, 1);
                        got_old = run_once(shape, 0);
                } else {
                        got_old = run_once(shape, 0);
                        got_assembly = run_once(shape, 1);
                }
                if (got_old < old) old = got_old;
                if (got_assembly < assembly) assembly = got_assembly;
        }
        printf("  %-12s old %lu  assembly %lu  assembly/old %lu%%\n",
               name, old, assembly, assembly * 100 / old);
}

int main(void)
{
        int bad = correctness();

        for (u64 i = 0; i < COUNT; i++) {
                u64 random = next();
                values[0][i] = random & 1023;
                values[1][i] = 1024 + random % (8 * 1024);
                values[2][i] = 10 * 1024 + random % (1014 * 1024);
                values[3][i] = random;
        }

        printf("arm64 native compact human, best of %d, %lu calls\n",
               TRIES, ROUNDS);
        row("plain", 0);
        row("fractional", 1);
        row("integer", 2);
        row("mixed u64", 3);
        return bad;
}
