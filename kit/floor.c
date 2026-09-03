/*
        Every routine in library.c, against the speed of the machine.

        The lower-bound candidate is a loop that moves the unavoidable traffic
        and computes nothing.  A semantic routine's useful question is how
        much slower it remains.  This is an empirical bound, not a declaration:
        when the real routine beats it, the report says the bound is unresolved
        instead of printing an impossible efficiency or pretending victory.

        Traffic matters and is measured rather than multiplied.  Two streams
        can overlap in the load/store machinery, and reads and writes are not
        symmetric.  Multiplying a one-stream time by a byte count produced
        impossible results above 100 percent.  Copy, fill and paired-read each
        have their own assembly floor which performs the same traffic shape.

        The number to distrust is a small one: at four bytes a call costs more
        than the work, so the column says what the routine costs to reach, not
        how fast it runs. The large sizes are the ones that mean anything about
        the loop.

            sh kit/bench floor

        A native run is hardware evidence.  A foreign run under qemu remains
        useful for instruction-shape comparison, but is labelled as emulated
        by the dispatcher and must not be quoted as an architectural floor.
*/
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline, noclone))

static p8 one[1 << 22] __attribute__((aligned(64)));
static p8 two[1 << 22] __attribute__((aligned(64)));
static p8 out[1 << 22] __attribute__((aligned(64)));

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
    ASM_FUNC(floor_ticks)
#if X64
    "lfence\n   rdtsc\n   shl $32, %rdx\n   or %rdx, %rax\n   lfence\n"
#elif ARM64
    "isb\n   mrs x0, cntvct_el0\n   isb\n"
#else
    "fence iorw, iorw\n   rdtime a0\n   fence iorw, iorw\n"
#endif
    ASM_RET
    ASM_END(floor_ticks)
    ASM_FUNC(floor_read)
#if X64
    "xor %eax, %eax\n   test %rsi, %rsi\n   jz 9f\n"
    "cmpb $0, cpu_has_avx2(%rip)\n   je 5f\n"
    "cmpb $0, cpu_has_avx512(%rip)\n   jne 7f\n"
    "1:  cmp $128, %rsi\n   jb 4f\n"
    "vmovdqu 0(%rdi), %ymm0\n   vmovdqu 32(%rdi), %ymm1\n"
    "vmovdqu 64(%rdi), %ymm2\n   vmovdqu 96(%rdi), %ymm3\n"
    "add $128, %rdi\n   sub $128, %rsi\n   jmp 1b\n"
    "7:  cmp $256, %rsi\n   jb 4f\n"
    "vmovdqu64 0(%rdi), %zmm0\n   vmovdqu64 64(%rdi), %zmm1\n"
    "vmovdqu64 128(%rdi), %zmm2\n   vmovdqu64 192(%rdi), %zmm3\n"
    "add $256, %rdi\n   sub $256, %rsi\n   jmp 7b\n"
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
positive floor_ticks(void);

__asm__(
    ".text\n"
    ASM_FUNC(floor_read_two)
#if X64
    "xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    "cmpb $0, cpu_has_avx2(%rip)\n   je 5f\n"
    "cmpb $0, cpu_has_avx512(%rip)\n   jne 7f\n"
    "1:  cmp $128, %rdx\n   jb 4f\n"
    "vmovdqu 0(%rdi), %ymm0\n   vmovdqu 32(%rdi), %ymm1\n"
    "vmovdqu 64(%rdi), %ymm2\n   vmovdqu 96(%rdi), %ymm3\n"
    "vmovdqu 0(%rsi), %ymm4\n   vmovdqu 32(%rsi), %ymm5\n"
    "vmovdqu 64(%rsi), %ymm6\n   vmovdqu 96(%rsi), %ymm7\n"
    "add $128, %rdi\n   add $128, %rsi\n   sub $128, %rdx\n   jmp 1b\n"
    "7:  cmp $256, %rdx\n   jb 4f\n"
    "vmovdqu64 0(%rdi), %zmm0\n   vmovdqu64 64(%rdi), %zmm1\n"
    "vmovdqu64 128(%rdi), %zmm2\n   vmovdqu64 192(%rdi), %zmm3\n"
    "vmovdqu64 0(%rsi), %zmm4\n   vmovdqu64 64(%rsi), %zmm5\n"
    "vmovdqu64 128(%rsi), %zmm6\n   vmovdqu64 192(%rsi), %zmm7\n"
    "add $256, %rdi\n   add $256, %rsi\n   sub $256, %rdx\n   jmp 7b\n"
    "4:  vzeroupper\n"
    "5:  test %rdx, %rdx\n   jz 9f\n"
    "6:  add (%rdi), %rax\n   add (%rsi), %rax\n"
    "add $8, %rdi\n   add $8, %rsi\n   sub $8, %rdx\n"
    "cmp $8, %rdx\n   jae 6b\n"
    "9:  \n"
#elif ARM64
    "mov x3, #0\n   cbz x2, 9f\n"
    "1:  cmp x2, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x0]\n   ldp q2, q3, [x0, #32]\n"
    "ldp q4, q5, [x1]\n   ldp q6, q7, [x1, #32]\n"
    "add x0, x0, #64\n   add x1, x1, #64\n"
    "sub x2, x2, #64\n   b 1b\n"
    "5:  cbz x2, 9f\n"
    "6:  ldr x4, [x0], #8\n   ldr x5, [x1], #8\n"
    "add x3, x3, x4\n   add x3, x3, x5\n"
    "subs x2, x2, #8\n   b.hi 6b\n"
    "9:  mov x0, x3\n"
#else
    "li a3, 0\n   beqz a2, 9f\n"
    "1:  li a4, 32\n   bltu a2, a4, 5f\n"
    "ld a4, 0(a0)\n   ld a5, 8(a0)\n   ld a6, 16(a0)\n   ld a7, 24(a0)\n"
    "ld t0, 0(a1)\n   ld t1, 8(a1)\n   ld t2, 16(a1)\n   ld t3, 24(a1)\n"
    "addi a0, a0, 32\n   addi a1, a1, 32\n   addi a2, a2, -32\n   j 1b\n"
    "5:  beqz a2, 9f\n"
    "6:  ld a4, 0(a0)\n   ld a5, 0(a1)\n"
    "add a3, a3, a4\n   add a3, a3, a5\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   addi a2, a2, -8\n   bnez a2, 6b\n"
    "9:  mv a0, a3\n"
#endif
    ASM_RET
    ASM_END(floor_read_two)
    ASM_FUNC(floor_copy)
#if X64
    "xor %eax, %eax\n   test %rdx, %rdx\n   jz 9f\n"
    "cmpb $0, cpu_has_avx2(%rip)\n   je 5f\n"
    "cmpb $0, cpu_has_avx512(%rip)\n   jne 7f\n"
    "1:  cmp $128, %rdx\n   jb 4f\n"
    "vmovdqu 0(%rsi), %ymm0\n   vmovdqu 32(%rsi), %ymm1\n"
    "vmovdqu 64(%rsi), %ymm2\n   vmovdqu 96(%rsi), %ymm3\n"
    "vmovdqu %ymm0, 0(%rdi)\n   vmovdqu %ymm1, 32(%rdi)\n"
    "vmovdqu %ymm2, 64(%rdi)\n   vmovdqu %ymm3, 96(%rdi)\n"
    "add $128, %rdi\n   add $128, %rsi\n   sub $128, %rdx\n   jmp 1b\n"
    "7:  cmp $256, %rdx\n   jb 4f\n"
    "vmovdqu64 0(%rsi), %zmm0\n   vmovdqu64 64(%rsi), %zmm1\n"
    "vmovdqu64 128(%rsi), %zmm2\n   vmovdqu64 192(%rsi), %zmm3\n"
    "vmovdqu64 %zmm0, 0(%rdi)\n   vmovdqu64 %zmm1, 64(%rdi)\n"
    "vmovdqu64 %zmm2, 128(%rdi)\n   vmovdqu64 %zmm3, 192(%rdi)\n"
    "add $256, %rdi\n   add $256, %rsi\n   sub $256, %rdx\n   jmp 7b\n"
    "4:  vzeroupper\n"
    "5:  test %rdx, %rdx\n   jz 9f\n"
    "6:  mov (%rsi), %rax\n   mov %rax, (%rdi)\n"
    "add $8, %rdi\n   add $8, %rsi\n   sub $8, %rdx\n"
    "cmp $8, %rdx\n   jae 6b\n"
    "9:  \n"
#elif ARM64
    "cbz x2, 9f\n"
    "1:  cmp x2, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x1]\n   ldp q2, q3, [x1, #32]\n"
    "stp q0, q1, [x0]\n   stp q2, q3, [x0, #32]\n"
    "add x0, x0, #64\n   add x1, x1, #64\n"
    "sub x2, x2, #64\n   b 1b\n"
    "5:  cbz x2, 9f\n"
    "6:  ldr x3, [x1], #8\n   str x3, [x0], #8\n"
    "subs x2, x2, #8\n   b.hi 6b\n"
    "9:  mov x0, #0\n"
#else
    "beqz a2, 9f\n"
    "1:  li a3, 32\n   bltu a2, a3, 5f\n"
    "ld a3, 0(a1)\n   ld a4, 8(a1)\n   ld a5, 16(a1)\n   ld a6, 24(a1)\n"
    "sd a3, 0(a0)\n   sd a4, 8(a0)\n   sd a5, 16(a0)\n   sd a6, 24(a0)\n"
    "addi a0, a0, 32\n   addi a1, a1, 32\n   addi a2, a2, -32\n   j 1b\n"
    "5:  beqz a2, 9f\n"
    "6:  ld a3, 0(a1)\n   sd a3, 0(a0)\n"
    "addi a0, a0, 8\n   addi a1, a1, 8\n   addi a2, a2, -8\n   bnez a2, 6b\n"
    "9:  li a0, 0\n"
#endif
    ASM_RET
    ASM_END(floor_copy)
    ASM_FUNC(floor_fill)
#if X64
    "xor %eax, %eax\n   test %rsi, %rsi\n   jz 9f\n"
    "cmpb $0, cpu_has_avx2(%rip)\n   je 5f\n"
    "cmpb $0, cpu_has_avx512(%rip)\n   jne 7f\n"
    "vpxor %ymm0, %ymm0, %ymm0\n"
    "1:  cmp $128, %rsi\n   jb 4f\n"
    "vmovdqu %ymm0, 0(%rdi)\n   vmovdqu %ymm0, 32(%rdi)\n"
    "vmovdqu %ymm0, 64(%rdi)\n   vmovdqu %ymm0, 96(%rdi)\n"
    "add $128, %rdi\n   sub $128, %rsi\n   jmp 1b\n"
    "7:  vpxord %zmm0, %zmm0, %zmm0\n"
    "8:  cmp $256, %rsi\n   jb 4f\n"
    "vmovdqu64 %zmm0, 0(%rdi)\n   vmovdqu64 %zmm0, 64(%rdi)\n"
    "vmovdqu64 %zmm0, 128(%rdi)\n   vmovdqu64 %zmm0, 192(%rdi)\n"
    "add $256, %rdi\n   sub $256, %rsi\n   jmp 8b\n"
    "4:  vzeroupper\n"
    "5:  test %rsi, %rsi\n   jz 9f\n"
    "6:  mov %rax, (%rdi)\n   add $8, %rdi\n   sub $8, %rsi\n"
    "cmp $8, %rsi\n   jae 6b\n"
    "9:  \n"
#elif ARM64
    "movi v0.16b, #0\n   cbz x1, 9f\n"
    "1:  cmp x1, #64\n   b.lo 5f\n"
    "stp q0, q0, [x0]\n   stp q0, q0, [x0, #32]\n"
    "add x0, x0, #64\n   sub x1, x1, #64\n   b 1b\n"
    "5:  cbz x1, 9f\n"
    "6:  str xzr, [x0], #8\n   subs x1, x1, #8\n   b.hi 6b\n"
    "9:  mov x0, #0\n"
#else
    "beqz a1, 9f\n"
    "1:  li a2, 32\n   bltu a1, a2, 5f\n"
    "sd zero, 0(a0)\n   sd zero, 8(a0)\n"
    "sd zero, 16(a0)\n   sd zero, 24(a0)\n"
    "addi a0, a0, 32\n   addi a1, a1, -32\n   j 1b\n"
    "5:  beqz a1, 9f\n"
    "6:  sd zero, 0(a0)\n   addi a0, a0, 8\n"
    "addi a1, a1, -8\n   bnez a1, 6b\n"
    "9:  li a0, 0\n"
#endif
    ASM_RET
    ASM_END(floor_fill)
);

positive floor_read_two(address_any one, address_any two, positive size);
positive floor_copy(address_any out, address_any in, positive size);
positive floor_fill(address_any out, positive size);

static positive floor_one(p8 address_to p, positive n)
{
        return floor_read(p, n);
}

static positive floor_two(p8 address_to a, p8 address_to b, positive n)
{
        return floor_read_two(a, b, n);
}

static volatile positive sink;

#define TIMED_ONCE(rounds, body)                                              \
        ({                                                                    \
                p64 s = floor_ticks();                                        \
                for (b32 k = 0; k < (rounds); k++) { body; }                  \
                floor_ticks() - s;                                            \
        })

static fn row(string_address name, positive size, positive ratio)
{
        // State the paired median as remaining cost: zero percent over is the
        // lower bound.  If the candidate wins, the baseline was not a floor.
        positive over = ratio > 10000 ? ratio - 10000 : 0;

        string_format(log, "  %s", name);
        for (positive i = string_length(name); i < 24; i++)
                string_format(log, " ");
        string_format(log, "%p", size);
        for (positive i = 0; i < 9; i++) string_format(log, " ");
        if (ratio < 10000)
                string_format(log, "unresolved: candidate beat baseline\n");
        else
                string_format(log, "%p.%p%% slower\n", over / 100,
                              over % 100);
}

#define PAIRED_ROW(name, size, rounds, floor_body, candidate_body)            \
        do {                                                                  \
                positive ratios[5];                                           \
                for (positive trial = 0; trial < 5; trial++)                  \
                {                                                             \
                        p64 floor_time;                                        \
                        p64 candidate_time;                                    \
                        if (trial & 1)                                         \
                        {                                                     \
                                candidate_time = TIMED_ONCE(rounds, candidate_body); \
                                floor_time = TIMED_ONCE(rounds, floor_body);   \
                        }                                                     \
                        else                                                  \
                        {                                                     \
                                floor_time = TIMED_ONCE(rounds, floor_body);   \
                                candidate_time = TIMED_ONCE(rounds, candidate_body); \
                        }                                                     \
                        ratios[trial] = (positive)(candidate_time * 10000 /   \
                                                   (floor_time ? floor_time : 1)); \
                }                                                             \
                order(ratios, 5);                                             \
                row(name, size, ratios[2]);                                   \
        } while (0)

b32 main(void)
{
        for (positive i = 0; i < sizeof(one); i++)
        {
                one[i] = (p8)(i % 251 + 1);
                two[i] = one[i];
        }

        moonwater_cpu_detect();

        /*
                The kernel shape: a kernel build has no vector body, so what
                it runs is the word at a time path under every routine here,
                and the floor for that path is the scalar loop. Built with
                -DFLOOR_NARROW this measures exactly that, natively, which a
                kernel cannot do for itself.
        */
#ifdef FLOOR_NARROW
        cpu_has_avx2 = 0;
        cpu_has_avx512 = 0;
        string_format(log, "  (narrow bodies against the scalar floor: the kernel shape)\n");
#endif

        string_format(log, "  routine                 size     gap to traffic lower bound\n");
        string_format(log, "  -----------------------------------------------------\n");

        for (positive z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++)
        {
                positive n = sizes[z];
                b32 rounds = (b32)((1 << 24) / n) + 1;

                PAIRED_ROW("memory_fill", n, rounds,
                           sink += floor_fill(out, n), memory_fill(out, 7, n));
                PAIRED_ROW("memory_copy", n, rounds,
                           sink += floor_copy(out, one, n),
                           memory_copy(out, one, n));
                PAIRED_ROW("memory_copy_apart", n, rounds,
                           sink += floor_copy(out, one, n),
                           memory_copy_apart(out, one, n));
                PAIRED_ROW("memory_count", n, rounds,
                           sink += floor_one(one, n),
                           sink += memory_count(one, n, 7));
                PAIRED_ROW("memory_first_of", n, rounds,
                           sink += floor_one(one, n),
                           sink += (positive)memory_first_of(one, 0, n));
                PAIRED_ROW("memory_compare", n, rounds,
                           sink += floor_two(one, two, n),
                           sink += (positive)memory_compare(one, two, n));

                one[n - 1] = 0;
                PAIRED_ROW("string_length", n, rounds,
                           sink += floor_one(one, n),
                           sink += string_length(one));
                PAIRED_ROW("string_first_of", n, rounds,
                           sink += floor_one(one, n),
                           sink += (positive)string_first_of(one, 0));
                PAIRED_ROW("string_last_of_or_end", n, rounds,
                           sink += floor_one(one, n),
                           sink += (positive)string_last_of_or_end(one, 3));
                two[n - 1] = 0;
                PAIRED_ROW("string_compare", n, rounds,
                           sink += floor_two(one, two, n),
                           sink += (positive)string_compare(one, two));
                PAIRED_ROW("string_copy", n, rounds,
                           sink += floor_copy(out, one, n),
                           string_copy(out, one));
                one[n - 1] = (p8)((n - 1) % 251 + 1);
                two[n - 1] = one[n - 1];

                string_format(log, "\n");
        }

        log_flush();
        return 0;
}
