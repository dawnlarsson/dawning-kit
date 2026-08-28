/* Exact lifted ARM64 contiguous-field leaves on native Apple ARM silicon. */
#include "fields.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64 positive_into_padded(u8 *, u64, u64, u8);
u64 positive_into_pair(u8 *, u64);
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

// Satisfy the general-path relocation in the lifted body. The benchmark's
// bounded width-six/nine calls take their direct lanes before reaching it.
u64 positive_into(u8 *into, u64 value) { return reference_into(into, value); }

static u64 seed = 0x9e3779b97f4a7c15ul;
static u64 next(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

static int correctness(void)
{
        u8 got[128], want[128];
        u64 checks = 0, bad = 0;

        for (u64 value = 0; value < 100; value++)
                for (u64 off = 0; off < 8; off++) {
                        for (u64 i = 0; i < sizeof(got); i++) got[i] = want[i] = 0xa5;
                        u64 length = positive_into_pair(got + 16 + off, value);
                        want[16 + off] = (u8)('0' + value / 10);
                        want[17 + off] = (u8)('0' + value % 10);
                        checks++;
                        if (length != 2) { bad++; continue; }
                        for (u64 i = 0; i < sizeof(got); i++)
                                if (got[i] != want[i]) { bad++; break; }
                }

        for (u64 r = 0; r < 100000; r++) {
                u64 width = r & 1 ? 6 : 9;
                u64 limit = width == 6 ? 1000000 : 1000000000;
                u64 value = next() % limit;
                u64 off = r & 7;
                for (u64 i = 0; i < sizeof(got); i++) got[i] = want[i] = 0x5a;
                u8 digits[24];
                u64 dl = reference_into(digits, value), pad = width - dl;
                for (u64 i = 0; i < pad; i++) want[16 + off + i] = '0';
                for (u64 i = 0; i < dl; i++) want[16 + off + pad + i] = digits[i];
                u64 length = positive_into_padded(got + 16 + off, value, width, '0');
                checks++;
                if (length != width) { bad++; continue; }
                for (u64 i = 0; i < sizeof(got); i++)
                        if (got[i] != want[i]) { bad++; break; }
        }

        printf("arm64 contiguous fields: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}

#define COUNT 64
#define ROUNDS (1ul << 17)
#define TRIES 7

static u64 values[COUNT];
static u8 output[24];
static volatile u64 sink;

static __attribute__((noinline)) u64 former_pair(u8 *into, u64 value)
{
        into[0] = (u8)('0' + (value / 10) % 10);
        into[1] = (u8)('0' + value % 10);
        return 2;
}

static __attribute__((noinline)) u64 former_scaled(u8 *into, u64 value, u64 scale)
{
        u64 length = 0;
        while (scale) {
                into[length++] = (u8)('0' + (value / scale) % 10);
                scale /= 10;
        }
        return length;
}

static u64 ticks(void) { u64 v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }

static u64 run_once(u64 width, int assembly)
{
        u64 start = ticks();
        for (u64 r = 0; r < ROUNDS; r++) {
                u64 value = values[r & (COUNT - 1)];
                u64 length;
                if (width == 2) value %= 100;
                else if (width == 6) value %= 1000000;
                else value %= 1000000000;
                if (assembly)
                        length = width == 2 ? positive_into_pair(output, value)
                                            : positive_into_padded(output, value, width, '0');
                else
                        length = width == 2 ? former_pair(output, value)
                                            : former_scaled(output, value,
                                                  width == 6 ? 100000 : 100000000);
                sink += length + output[0] + output[width - 1];
        }
        return ticks() - start;
}

static void row(const char *name, u64 width)
{
        u64 old = ~0ul, assembly = ~0ul;
        for (u64 t = 0; t < TRIES; t++) {
                u64 got_old, got_assembly;
                if (t & 1) {
                        got_assembly = run_once(width, 1);
                        got_old = run_once(width, 0);
                } else {
                        got_old = run_once(width, 0);
                        got_assembly = run_once(width, 1);
                }
                if (got_old < old) old = got_old;
                if (got_assembly < assembly) assembly = got_assembly;
        }
        printf("  %-8s old %lu  assembly %lu  assembly/old %lu%%\n",
               name, old, assembly, assembly * 100 / old);
}

int main(void)
{
        int bad = correctness();
        for (u64 i = 0; i < COUNT; i++) values[i] = next();
        printf("arm64 native contiguous decimal, best of %d, %lu calls\n", TRIES, ROUNDS);
        row("width 2", 2); row("width 6", 6); row("width 9", 9);
        return bad;
}
