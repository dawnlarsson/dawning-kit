/* 32-bit span fill: the shared Canvas/window primitive against its two floors. */
#include "../src/compiler_memory.c"

#define TRIES 9
#define TARGET_BYTES (1u << 26)
#define MAXIMUM 4096

static unsigned int block[MAXIMUM + 2] __attribute__((aligned(64)));

/* REP pays setup; the scalar body pays a back edge per four words. */
__asm__(
    ".text\n"
    ASM_FUNC(fill_u32_scalar_floor)
#if X64
    "mov %edx, %eax\n   mov %eax, %r8d\n   shl $32, %r8\n   or %r8, %rax\n"
    "cmp $4, %rsi\n   jb 2f\n"
    "1:  mov %rax, (%rdi)\n   mov %rax, 8(%rdi)\n   add $16, %rdi\n"
    "sub $4, %rsi\n   cmp $4, %rsi\n   jae 1b\n"
    "2:  test $2, %sil\n   jz 3f\n   mov %rax, (%rdi)\n   add $8, %rdi\n"
    "3:  test $1, %sil\n   jz 4f\n   mov %eax, (%rdi)\n"
    "4:\n"
#elif ARM64
    "mov w2, w2\n   orr x3, x2, x2, lsl #32\n"
    "1:  cmp x1, #4\n   b.lo 2f\n   stp x3, x3, [x0], #16\n"
    "sub x1, x1, #4\n   b 1b\n"
    "2:  tbz x1, #1, 3f\n   str x3, [x0], #8\n"
    "3:  tbz x1, #0, 4f\n   str w2, [x0]\n   4:\n"
#else
    "slli a2, a2, 32\n   srli a2, a2, 32\n   slli t0, a2, 32\n"
    "or t0, t0, a2\n   li t1, 2\n"
    "1:  blt a1, t1, 2f\n   sd t0, 0(a0)\n   addi a0, a0, 8\n"
    "addi a1, a1, -2\n   j 1b\n"
    "2:  beqz a1, 3f\n   sw a2, 0(a0)\n   3:\n"
#endif
    ASM_RET
    ASM_END(fill_u32_scalar_floor)
    ASM_FUNC(fill_u32_bulk_floor)
#if X64
    "mov %edx, %eax\n   mov %eax, %r8d\n   shl $32, %r8\n   or %r8, %rax\n"
    "mov %rsi, %rcx\n   shr $1, %rcx\n   rep stosq\n"
    "test $1, %sil\n   jz 1f\n   mov %eax, (%rdi)\n   1:\n"
#elif ARM64
    "b fill_u32_scalar_floor\n"
#else
    "j fill_u32_scalar_floor\n"
#endif
    ASM_RET
    ASM_END(fill_u32_bulk_floor)
);

fn fill_u32_scalar_floor(address_any destination, positive count,
                         unsigned int value);
fn fill_u32_bulk_floor(address_any destination, positive count,
                       unsigned int value);

typedef fn (*fill_call)(address_any, positive, unsigned int);
static fill_call volatile calls[] = {
    memory_fill_u32, fill_u32_scalar_floor, fill_u32_bulk_floor,
};

static bool correctness(void)
{
        for (positive offset = 0; offset < 2; offset++)
                for (positive count = 0; count <= MAXIMUM; count++)
                {
                        memory_fill(block, 0xa5, sizeof(block));
                        memory_fill_u32(block + offset, count, 0x13579bdfu);

                        for (positive i = 0; i < MAXIMUM + 2; i++)
                        {
                                unsigned int expected =
                                    i >= offset && i < offset + count
                                        ? 0x13579bdfu
                                        : 0xa5a5a5a5u;

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
                calls[which](block + offset, count, 0x13579bdfu);

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
            1, 2, 3, 4, 8, 16, 32, 64, 96, 128, 160, 192,
            200, 208, 216, 224, 232, 240, 248, 256,
            320, 384, 512, 1024, 4096,
        };

        if (!correctness())
        {
                string_format(log, "memory_fill_u32 correctness failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "memory_fill_u32, paired median of %p\n",
                      (positive)TRIES);

        for (positive i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        {
                row(sizes[i], 0);
                row(sizes[i], 1);
        }

        log_flush();
        return 0;
}
