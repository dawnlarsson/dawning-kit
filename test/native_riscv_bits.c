/*
        The two things riscv64 has to do by hand, checked here.

        Base RV64I has no count-trailing-zeros and no count-leading-zeros, so
        the new routines in the riscv block build both out of shifts. This is
        that arithmetic on its own, against a plain loop, over every input it
        can be handed: one flag bit per byte, at bit 8k+7.
*/
typedef unsigned long u64;
int printf(const char *, ...);

#define ONES  0x0101010101010101ul
#define SEVEN 0x7f7f7f7f7f7f7f7ful

// isolate the lowest flag, then count the bytes under it
static u64 lowest_byte(u64 flags)
{
        u64 t = flags & (~flags + 1);            // sub t,zero,x / and
        t = t - 1;                               // addi -1
        t = t & ONES;                            // and t0
        t = t * ONES;                            // mul t0
        t = t >> 56;                             // srli 56
        return t - 1;                            // addi -1
}

// smear down, isolate the highest, then the same count
static u64 highest_byte(u64 flags)
{
        u64 x = flags;
        x |= x >> 1;  x |= x >> 2;  x |= x >> 4;
        x |= x >> 8;  x |= x >> 16; x |= x >> 32;
        u64 t = (x >> 1) + 1;                    // srli 1 / addi 1
        t = t - 1;
        t = t & ONES;
        t = t * ONES;
        t = t >> 56;
        return t - 1;
}

// the exact per byte zero test, which must not carry between bytes
static u64 zero_flags(u64 v)
{
        return ~(((v & SEVEN) + SEVEN) | v | SEVEN);
}

int main(void)
{
        u64 checks = 0, bad_lo = 0, bad_hi = 0, bad_zero = 0;

        // every non-empty subset of the eight flag positions
        for (unsigned m = 1; m < 256; m++) {
                u64 flags = 0;
                int lo = -1, hi = -1;
                for (int k = 0; k < 8; k++)
                        if (m & (1u << k)) {
                                flags |= 1ul << (8*k + 7);
                                if (lo < 0) lo = k;
                                hi = k;
                        }
                checks += 2;
                if ((long)lowest_byte(flags)  != lo) bad_lo++;
                if ((long)highest_byte(flags) != hi) bad_hi++;
        }

        // the zero test against a byte by byte answer, including the
        // adjacencies the cheap test gets wrong
        for (u64 t = 0; t < 400000; t++) {
                static u64 seed = 0x2545F4914F6CDD1Dul;
                seed ^= seed<<13; seed ^= seed>>7; seed ^= seed<<17;
                u64 v = seed;
                if (t < 2000) v &= 0x0101010101010101ul * (t % 3);  // lots of zero bytes
                u64 want = 0;
                for (int k = 0; k < 8; k++)
                        if (((v >> (8*k)) & 0xff) == 0) want |= 1ul << (8*k + 7);
                checks++;
                if (zero_flags(v) != want) bad_zero++;
        }

        printf("riscv64 bit arithmetic: %lu checks | lowest %lu | highest %lu | zero test %lu\n",
               checks, bad_lo, bad_hi, bad_zero);
        return (bad_lo || bad_hi || bad_zero) != 0;
}
