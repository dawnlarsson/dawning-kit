/*
        positive_into_base on native arm64 silicon.

        Correctness is already crossed with every architecture by verify.c;
        this lifted case establishes that the exact arm64 body also executes
        here, then times it against printf_render's former runtime-base C loop.
*/
#include "bases.h"

typedef unsigned char u8;
typedef unsigned long u64;

u64 positive_into_base(u8 *, u64, u64, u8);
int printf(const char *, ...);

static u64 reference(u8 *into, u64 value, u64 base, u8 upper)
{
        u8 scratch[64];
        u64 have = 0;

        do {
                u64 digit = value % base;
                scratch[have++] = (u8)(digit < 10 ? '0' + digit
                                                     : (upper ? 'A' : 'a') + digit - 10);
                value /= base;
        } while (value);

        for (u64 i = 0; i < have; i++) into[i] = scratch[have - i - 1];
        return have;
}

// The lifted routine has a decimal tail relocation even though this case
// never takes it. Give the linker the exact no-terminator contract it expects.
u64 positive_into(u8 *into, u64 value)
{
        return reference(into, value, 10, 0);
}

static u64 seed = 0x9e3779b97f4a7c15ul;
static u64 next(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }

static u8 got_room[96], want_room[96];

static int correctness(void)
{
        u64 checks = 0, bad = 0;

        for (u64 base = 2; base <= 36; base++)
                for (u8 upper = 0; upper <= 1; upper++)
                        for (u64 r = 0; r < 4096; r++) {
                                u64 value = r < 16 ? (u64[]){0,1,7,8,9,10,15,16,
                                                           63,64,255,256,4095,4096,
                                                           ~0ul-1,~0ul}[r] : next();
                                u64 off = (value + r + base) & 7;
                                for (u64 i = 0; i < sizeof(got_room); i++)
                                        got_room[i] = want_room[i] = 0xa5;

                                u64 gl = positive_into_base(got_room + 8 + off,
                                                            value, base, upper);
                                u64 wl = reference(want_room + 8 + off,
                                                   value, base, upper);
                                checks++;
                                if (gl != wl) { bad++; continue; }
                                for (u64 i = 0; i < sizeof(got_room); i++)
                                        if (got_room[i] != want_room[i]) {
                                                bad++;
                                                break;
                                        }
                        }

        printf("arm64 base correctness: %lu checks | %lu failures\n", checks, bad);
        return bad != 0;
}

#define COUNT 64
#define ROUNDS (1ul << 17)
#define TRIES 7

static u64 values[COUNT];
static u8 output[64];
static volatile u64 sink;

static __attribute__((noinline)) u64 old_runtime_base(u8 *into, u64 value,
                                                       u64 base, u8 upper)
{
        return reference(into, value, base, upper);
}

static u64 ticks(void)
{
        u64 value;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
}

static u64 run_old(u64 base, u8 upper)
{
        u64 best = ~0ul;
        for (u64 t = 0; t < TRIES; t++) {
                u64 start = ticks();
                for (u64 r = 0; r < ROUNDS; r++)
                        sink += old_runtime_base(output, values[r & (COUNT - 1)],
                                                 base, upper);
                u64 took = ticks() - start;
                if (took < best) best = took;
        }
        return best;
}

static u64 run_asm(u64 base, u8 upper)
{
        u64 best = ~0ul;
        for (u64 t = 0; t < TRIES; t++) {
                u64 start = ticks();
                for (u64 r = 0; r < ROUNDS; r++)
                        sink += positive_into_base(output,
                                                   values[r & (COUNT - 1)],
                                                   base, upper);
                u64 took = ticks() - start;
                if (took < best) best = took;
        }
        return best;
}

static void row(const char *name, u64 base, u8 upper)
{
        u64 old = run_old(base, upper), assembly = run_asm(base, upper);
        printf("  %-18s old %lu  assembly %lu  assembly/old %lu%%\n",
               name, old, assembly, assembly * 100 / old);
}

int main(void)
{
        int bad = correctness();

        for (u64 i = 0; i < COUNT; i++) values[i] = next();
        values[0] = 0; values[1] = 7; values[2] = 8; values[3] = 15;
        values[4] = 16; values[5] = 07777; values[6] = ~0ul;

        printf("arm64 native base conversion, best of %d, %lu calls\n", TRIES, ROUNDS);
        row("binary", 2, 0);
        row("base 4", 4, 0);
        row("octal", 8, 0);
        row("hexadecimal lower", 16, 0);
        row("hexadecimal upper", 16, 1);
        row("base 32 lower", 32, 0);
        row("base 32 upper", 32, 1);
        row("base 36 lower", 36, 0);
        return bad;
}
