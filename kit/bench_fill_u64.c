/* Naturally aligned 64-bit pattern fill against scalar and bulk-store floors. */
#include "../src/compiler_memory.c"

#define TRIES 9
#define TARGET_BYTES (1u << 26)
#define MAXIMUM 4096

static positive block[MAXIMUM + 2] __attribute__((aligned(64)));

__asm__(
    ".text\n"
    ASM_FUNC(fill_u64_scalar_floor)
#if X64
    "1:  cmp $4, %rsi\n   jb 2f\n"
    "mov %rdx, (%rdi)\n   mov %rdx, 8(%rdi)\n"
    "mov %rdx, 16(%rdi)\n   mov %rdx, 24(%rdi)\n"
    "add $32, %rdi\n   sub $4, %rsi\n   jmp 1b\n"
    "2:  test $2, %sil\n   jz 3f\n   mov %rdx, (%rdi)\n"
    "mov %rdx, 8(%rdi)\n   add $16, %rdi\n"
    "3:  test $1, %sil\n   jz 4f\n   mov %rdx, (%rdi)\n   4:\n"
#elif ARM64
    "1:  cmp x1, #4\n   b.lo 2f\n"
    "stp x2, x2, [x0], #16\n   stp x2, x2, [x0], #16\n"
    "sub x1, x1, #4\n   b 1b\n"
    "2:  tbz x1, #1, 3f\n   stp x2, x2, [x0], #16\n"
    "3:  tbz x1, #0, 4f\n   str x2, [x0]\n   4:\n"
#else
    "li t0, 4\n"
    "1:  blt a1, t0, 2f\n   sd a2, 0(a0)\n   sd a2, 8(a0)\n"
    "sd a2, 16(a0)\n   sd a2, 24(a0)\n   addi a0, a0, 32\n"
    "addi a1, a1, -4\n   j 1b\n"
    "2:  andi t0, a1, 2\n   beqz t0, 3f\n"
    "sd a2, 0(a0)\n   sd a2, 8(a0)\n   addi a0, a0, 16\n"
    "3:  andi t0, a1, 1\n   beqz t0, 4f\n   sd a2, 0(a0)\n   4:\n"
#endif
    ASM_RET
    ASM_END(fill_u64_scalar_floor)
    ASM_FUNC(fill_u64_bulk_floor)
#if X64
    "mov %rdx, %rax\n   mov %rsi, %rcx\n   rep stosq\n"
#elif ARM64
    "b fill_u64_scalar_floor\n"
#else
    "j fill_u64_scalar_floor\n"
#endif
    ASM_RET
    ASM_END(fill_u64_bulk_floor)
);

fn fill_u64_scalar_floor(address_any, positive, positive);
fn fill_u64_bulk_floor(address_any, positive, positive);

typedef fn (*fill_call)(address_any, positive, positive);
static fill_call volatile calls[] = {
    memory_fill_u64_aligned, fill_u64_scalar_floor, fill_u64_bulk_floor,
};

static bool correctness(void)
{
        for (positive offset = 0; offset < 2; offset++)
                for (positive count = 0; count <= MAXIMUM; count++)
                {
                        memory_fill(block, 0xa5, sizeof(block));
                        memory_fill_u64_aligned(block + offset, count,
                                                0x13579bdf2468ace0ull);

                        for (positive i = 0; i < MAXIMUM + 2; i++)
                        {
                                positive expected =
                                    i >= offset && i < offset + count
                                        ? 0x13579bdf2468ace0ull
                                        : 0xa5a5a5a5a5a5a5a5ull;

                                if (block[i] != expected)
                                        return false;
                        }
                }

        return true;
}

static p64 run(unsigned int which, positive count, positive rounds,
               positive offset)
{
        p64 started = get_cpu_time();

        while (rounds--)
                calls[which](block + offset, count, 0x13579bdf2468ace0ull);

        return get_cpu_time() - started;
}

static fn order(positive *values)
{
        for (positive i = 1; i < TRIES; i++)
        {
                positive value = values[i], at = i;

                while (at && values[at - 1] > value)
                {
                        values[at] = values[at - 1];
                        at--;
                }

                values[at] = value;
        }
}

static fn row(positive count, positive offset)
{
        positive scalar[TRIES], bulk[TRIES];
        positive rounds = TARGET_BYTES / max(count * sizeof(*block), 1ul);

        if (rounds < 32)
                rounds = 32;

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 ours, floor;

                if (trial & 1)
                {
                        floor = run(1, count, rounds, offset);
                        ours = run(0, count, rounds, offset);
                }
                else
                {
                        ours = run(0, count, rounds, offset);
                        floor = run(1, count, rounds, offset);
                }

                scalar[trial] = (positive)(ours * 10000 / max(floor, 1ull));

                if (trial & 1)
                {
                        floor = run(2, count, rounds, offset);
                        ours = run(0, count, rounds, offset);
                }
                else
                {
                        ours = run(0, count, rounds, offset);
                        floor = run(2, count, rounds, offset);
                }

                bulk[trial] = (positive)(ours * 10000 / max(floor, 1ull));
        }

        order(scalar);
        order(bulk);
        string_format(log,
                      "  %p words +%p  current/scalar %p.%p%%  current/bulk %p.%p%%\n",
                      count, offset, scalar[TRIES / 2] / 100,
                      scalar[TRIES / 2] % 100, bulk[TRIES / 2] / 100,
                      bulk[TRIES / 2] % 100);
}

b32 main(void)
{
        static const positive sizes[] = {
            1, 2, 3, 4, 8, 16, 32, 48, 64, 80, 88, 96, 100, 104,
            108, 112, 120, 128, 160, 192, 256, 512, 1024, 4096,
        };

        if (!correctness())
        {
                string_format(log, "memory_fill_u64_aligned correctness failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "memory_fill_u64_aligned, paired median of %p\n",
                      (positive)TRIES);

        for (positive i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        {
                row(sizes[i], 0);
                row(sizes[i], 1);
        }

        log_flush();
        return 0;
}
