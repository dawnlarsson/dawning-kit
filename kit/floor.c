/*
        Every routine in library.c, against the speed of the machine.

        The floor is a loop that reads the same memory and computes nothing.
        Nothing that reads its input once can beat it, so a routine's worth is
        how close it gets, and a routine already there is finished -- there is
        no faster version of it, only a faster machine.

        Traffic matters and is stated rather than hidden. A copy moves its
        payload twice across the bus and a compare reads two streams, so those
        are reported against a floor measured the same way. Without that a copy
        at the bus limit reads as fifty percent and looks like slack.

        The number to distrust is a small one: at four bytes a call costs more
        than the work, so the column says what the routine costs to reach, not
        how fast it runs. The large sizes are the ones that mean anything about
        the loop.

            sh kit/bench floor        every architecture
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))

static p8 one[1 << 22];
static p8 two[1 << 22];
static p8 out[1 << 22];

static positive sizes[] = {64, 4096, 65536, 1048576};

/*
        The floor, in assembly, for the same reason the routines are.

        Two earlier versions of this were wrong in opposite directions and both
        looked plausible. Stepping a cache line at a time moves the same
        traffic but issues a sixty fourth of the loads, so in cache it beats
        anything that must look at every byte and every routine read as slack
        that was not there. Writing it in C with an asm body and a memory
        clobber made the compiler reload each turn, so the floor measured
        itself and every routine came out above it.

        A ceiling has to be built the way the thing under it is built. This
        reads every byte at the widest load the machine has, accumulates into
        registers nothing ever looks at, and is written out by hand so no
        compiler decides how to schedule it.
*/
__asm__(
    ".text\n"
    ASM_FUNC(floor_read)
#if X64
    "xor %eax, %eax\n   test %rsi, %rsi\n   jz 9f\n"
    "cmpb $0, cpu_has_avx2(%rip)\n   je 5f\n"
    "vpxor %ymm0, %ymm0, %ymm0\n   vpxor %ymm1, %ymm1, %ymm1\n"
    "vpxor %ymm2, %ymm2, %ymm2\n   vpxor %ymm3, %ymm3, %ymm3\n"
    "1:  cmp $128, %rsi\n   jb 4f\n"
    "vpaddb (%rdi), %ymm0, %ymm0\n   vpaddb 32(%rdi), %ymm1, %ymm1\n"
    "vpaddb 64(%rdi), %ymm2, %ymm2\n   vpaddb 96(%rdi), %ymm3, %ymm3\n"
    "add $128, %rdi\n   sub $128, %rsi\n   jmp 1b\n"
    "4:  vzeroupper\n"
    "5:  test %rsi, %rsi\n   jz 9f\n"
    "6:  add (%rdi), %rax\n   add $8, %rdi\n   sub $8, %rsi\n   cmp $8, %rsi\n   jae 6b\n"
    "9:  \n"
#elif ARM64
    "mov x2, #0\n   cbz x1, 9f\n"
    "1:  cmp x1, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x0]\n   ldp q2, q3, [x0, #32]\n"
    "add x0, x0, #64\n   sub x1, x1, #64\n   b 1b\n"
    "5:  cbz x1, 9f\n"
    "6:  ldr x3, [x0]\n   add x2, x2, x3\n   add x0, x0, #8\n"
    "subs x1, x1, #8\n   b.hi 6b\n"
    "9:  mov x0, x2\n"
#else
    "li a2, 0\n   beqz a1, 9f\n"
    "1:  li a3, 8\n   bltu a1, a3, 9f\n"
    "ld a4, 0(a0)\n   add a2, a2, a4\n   addi a0, a0, 8\n   addi a1, a1, -8\n   j 1b\n"
    "9:  mv a0, a2\n"
#endif
    ASM_RET
    ASM_END(floor_read)
);

positive floor_read(address_any block, positive size);

static positive floor_one(p8 address_to p, positive n)
{
        return floor_read(p, n);
}

static positive floor_two(p8 address_to a, p8 address_to b, positive n)
{
        return floor_read(a, n) + floor_read(b, n);
}

static volatile positive sink;

#define TIMED(rounds, body)                                                   \
        ({                                                                    \
                p64 best = ~(p64)0;                                           \
                for (b32 r = 0; r < 5; r++)                                   \
                {                                                             \
                        p64 s = get_cpu_time();                               \
                        for (b32 k = 0; k < (rounds); k++) { body; }           \
                        p64 e = get_cpu_time() - s;                           \
                        if (e < best) best = e;                               \
                }                                                             \
                best;                                                         \
        })

static fn row(string_address name, positive size, p64 ours, p64 floor,
              positive traffic)
{
        // ticks are a free running counter, so a ratio is the only honest
        // thing to print; the percentage is against the same traffic
        positive pct = floor ? (positive)((floor * 100 * traffic) / (ours ? ours : 1)) : 0;

        string_format(log, "  %s", name);
        for (positive i = string_length(name); i < 24; i++)
                string_format(log, " ");
        string_format(log, "%p", size);
        for (positive i = 0; i < 9; i++) string_format(log, " ");
        string_format(log, "%p%%\n", pct);
}

b32 main(void)
{
        for (positive i = 0; i < sizeof(one); i++)
        {
                one[i] = (p8)(i % 251 + 1);
                two[i] = one[i];
        }

        moonwater_cpu_detect();

        string_format(log, "  routine                 size     %% of floor\n");
        string_format(log, "  ------------------------------------------\n");

        for (positive z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++)
        {
                positive n = sizes[z];
                b32 rounds = (b32)((1 << 24) / n) + 1;

                p64 f1 = TIMED(rounds, sink += floor_one(one, n));
                p64 f2 = TIMED(rounds, sink += floor_two(one, two, n));

                row("memory_fill", n, TIMED(rounds, memory_fill(out, 7, n)), f1, 2);
                row("memory_copy", n, TIMED(rounds, memory_copy(out, one, n)), f1, 2);
                row("memory_copy_fast", n, TIMED(rounds, memory_copy_fast(out, one, n)), f1, 2);
                row("memory_count", n, TIMED(rounds, sink += memory_count(one, n, 7)), f1, 1);
                row("memory_first_of", n, TIMED(rounds, sink += (positive)memory_first_of(one, 0, n)), f1, 1);
                row("memory_compare", n, TIMED(rounds, sink += (positive)memory_compare(one, two, n)), f2, 1);

                one[n - 1] = 0;
                row("string_length", n, TIMED(rounds, sink += string_length(one)), f1, 1);
                row("string_first_of", n, TIMED(rounds, sink += (positive)string_first_of(one, 0)), f1, 1);
                row("string_last_of_or_end", n, TIMED(rounds, sink += (positive)string_last_of_or_end(one, 3)), f1, 1);
                two[n - 1] = 0;
                row("string_compare", n, TIMED(rounds, sink += (positive)string_compare(one, two)), f2, 1);
                row("string_copy", n, TIMED(rounds, string_copy(out, one)), f1, 3);
                one[n - 1] = (p8)((n - 1) % 251 + 1);
                two[n - 1] = one[n - 1];

                string_format(log, "\n");
        }

        log_flush();
        return 0;
}
