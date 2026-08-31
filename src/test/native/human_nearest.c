/* Exact lifted ARM64 dd human formatter and wait decoder on Apple silicon. */
#include "human_nearest.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64 positive_into_human_nearest_string(u8 *, u64, u8);
int wait_status_code(u64);
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

static __attribute__((noinline, noclone)) u64 former_human(u8 *into, u64 n, u8 binary)
{
        u64 base = binary ? 1024 : 1000;
        u64 amount = n, tenths = 0, rounding = 0, exponent = 0;
        static u8 letters[] = {0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y', 'R', 'Q'};
        u64 used = 0, fraction = 0;
        u8 point = 0;

        if (base <= amount) {
                do {
                        u64 ten = (amount % base) * 10 + tenths;
                        u64 two = (ten % base) * 2 + (rounding >> 1);
                        amount /= base;
                        tenths = ten / base;
                        rounding = two < base ? ((two + rounding) != 0)
                                              : 2 + (base < two + rounding);
                        exponent++;
                } while (base <= amount && exponent < 10);

                if (amount < 10) {
                        if (2 < rounding + (tenths & 1)) {
                                tenths++;
                                rounding = 0;
                                if (tenths == 10) { amount++; tenths = 0; }
                        }
                        if (amount < 10) {
                                point = 1;
                                fraction = tenths;
                                tenths = rounding = 0;
                        }
                }
        }

        if (5 < tenths + (0 < rounding + (amount & 1))) {
                amount++;
                if (amount == base && exponent < 10) {
                        exponent++;
                        point = 1;
                        fraction = 0;
                        amount = 1;
                }
        }

        used += reference_into(into + used, amount);
        if (point) { into[used++] = '.'; into[used++] = (u8)('0' + fraction); }
        into[used++] = ' ';
        if (exponent) into[used++] = !binary && exponent == 1 ? 'k' : letters[exponent];
        if (binary && exponent) into[used++] = 'i';
        into[used++] = 'B';
        into[used] = 0;
        return used;
}

static u64 seed = 0x9e3779b97f4a7c15ul;
static u64 next(void)
{
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
}

static int one(u64 value, u8 binary, u64 offset, u8 guard)
{
        u8 got[64], want[64];
        for (u64 i = 0; i < sizeof(got); i++) got[i] = want[i] = guard;

        u64 gl = positive_into_human_nearest_string(got + 16 + (offset & 15),
                                                     value, binary);
        u64 wl = former_human(want + 16 + (offset & 15), value, binary);

        if (gl != wl || wl > 8) return 1;
        for (u64 i = 0; i < sizeof(got); i++) if (got[i] != want[i]) return 1;
        return 0;
}

static int correctness(void)
{
        static u8 guards[] = {0, 0x55, 0xff};
        static u64 fixed[] = {
            0, 1, 9, 10, 99, 999, 1000, 1023, 1024, 9999,
            999499, 999500, 999501, 999999, 1000000,
            1023 * 1024, 1024 * 1024 - 1, 1024 * 1024,
            ~0ul - 1, ~0ul,
        };
        u64 checks = 0, bad = 0;

        for (u64 binary = 0; binary < 2; binary++)
                for (u64 value = 0; value <= 65535; value++) {
                        bad += one(value, (u8)binary, value & 15,
                                   guards[(value >> 4) % sizeof(guards)]);
                        checks++;
                }

        for (u64 binary = 0; binary < 2; binary++)
                for (u64 offset = 0; offset < 16; offset++)
                        for (u64 g = 0; g < sizeof(guards); g++)
                                for (u64 i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
                                        bad += one(fixed[i], (u8)binary, offset, guards[g]);
                                        checks++;
                                }

        for (u64 binary = 0; binary < 2; binary++) {
                u64 base = binary ? 1024 : 1000, power = 1, serial = 0;
                for (u64 unit = 1; unit <= 6; unit++) {
                        if (power > ~0ul / base) break;
                        power *= base;
                        for (u64 delta = 0; delta <= 4096; delta++) {
                                if (power >= delta) {
                                        bad += one(power - delta, (u8)binary, serial & 15,
                                                   guards[serial % sizeof(guards)]);
                                        serial++; checks++;
                                }
                                if (power <= ~0ul - delta) {
                                        bad += one(power + delta, (u8)binary, serial & 15,
                                                   guards[serial % sizeof(guards)]);
                                        serial++; checks++;
                                }
                        }
                        u64 half = power / 2;
                        for (u64 q = 1; q <= 20; q++)
                                if (q <= (~0ul - half) / power) {
                                        u64 tie = q * power + half;
                                        bad += one(tie - 1, (u8)binary, serial++ & 15, 0xa5);
                                        bad += one(tie, (u8)binary, serial++ & 15, 0x5a);
                                        bad += one(tie + 1, (u8)binary, serial++ & 15, 0xff);
                                        checks += 3;
                                }
                }
        }

        for (u64 binary = 0; binary < 2; binary++)
                for (u64 i = 0; i < 65536; i++) {
                        bad += one(next(), (u8)binary, i & 15,
                                   guards[(i >> 4) % sizeof(guards)]);
                        checks++;
                }

        for (u64 raw = 0; raw <= 65535; raw++) {
                u64 signal = raw & 0x7f;
                int want = signal ? (int)(128 + signal) : (int)((raw >> 8) & 0xff);
                if (wait_status_code(raw) != want) bad++;
                checks++;
        }

        printf("arm64 nearest human/wait: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}

#define COUNT 64
#define ROUNDS (1ul << 18)
#define TRIES 7

static u64 values[2][COUNT];
static u8 output[9];
static volatile u64 sink;

static u64 ticks(void)
{
        u64 value;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
}

static u64 run_once(u8 binary, int assembly)
{
        u64 start = ticks();
        for (u64 r = 0; r < ROUNDS; r++) {
                u64 value = values[binary][r & (COUNT - 1)];
                u64 length = assembly
                    ? positive_into_human_nearest_string(output, value, binary)
                    : former_human(output, value, binary);
                sink += length + output[0] + output[length - 1];
        }
        return ticks() - start;
}

static void row(const char *name, u8 binary)
{
        u64 old = ~0ul, assembly = ~0ul;
        for (u64 t = 0; t < TRIES; t++) {
                u64 got_old, got_assembly;
                if (t & 1) {
                        got_assembly = run_once(binary, 1);
                        got_old = run_once(binary, 0);
                } else {
                        got_old = run_once(binary, 0);
                        got_assembly = run_once(binary, 1);
                }
                if (got_old < old) old = got_old;
                if (got_assembly < assembly) assembly = got_assembly;
        }
        printf("  %-10s old %lu  assembly %lu  assembly/old %lu%%\n",
               name, old, assembly, assembly * 100 / old);
}

int main(void)
{
        int bad = correctness();

        for (u64 i = 0; i < COUNT; i++) {
                values[0][i] = next();
                values[1][i] = next();
        }
        values[0][0] = 999999;
        values[0][1] = ~0ul;
        values[1][0] = 1023 * 1024;
        values[1][1] = ~0ul;

        printf("arm64 native nearest human, best of %d, %lu calls\n", TRIES, ROUNDS);
        row("SI mixed", 0);
        row("IEC mixed", 1);
        return bad;
}
