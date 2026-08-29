/*
        Fixed-needle record counting at three layers:

        read-only is an explicit serialized traffic lower-bound candidate;
        byte count is a semantic delimiter-count proxy; repeated is grep's
        former prepared-search/newline-search loop; and fused is the
        record-aware assembly core which keeps its anchors live.

        The absolute result is counter ticks per KiB.  On x86_64 those are TSC
        ticks; under qemu they are useful only as paired ratios, as kit/bench
        says.  Output is consumed through sink and every shape is checked
        against a deliberately obvious record-by-record reference first.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9

#if defined(__x86_64__)
#define ROOM (1u << 22)
#define ROUNDS 32
#else
#define ROOM (1u << 16)
#define ROUNDS 4
#endif

static p8 block[ROOM + 64];
static p8 needle[] = "needle";
static volatile positive sink;
static positive2 anchors;

/* The explicit read-only traffic candidate from kit/floor.c, kept beside the
   semantic microkernel so both are timed with the same fences and corpus. */
__asm__(
    ".text\n"
    ASM_FUNC(grep_floor_ticks)
#if X64
    "lfence\n   rdtsc\n   shl $32, %rdx\n   or %rdx, %rax\n   lfence\n"
#elif ARM64
    "isb\n   mrs x0, cntvct_el0\n   isb\n"
#else
    "fence iorw, iorw\n   rdtime a0\n   fence iorw, iorw\n"
#endif
    ASM_RET
    ASM_END(grep_floor_ticks)
    ASM_FUNC(grep_floor_read)
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
    "6:  add (%rdi), %rax\n   add $8, %rdi\n   sub $8, %rsi\n"
    "cmp $8, %rsi\n   jae 6b\n   9:\n"
#elif ARM64
    "mov x2, #0\n   cbz x1, 9f\n"
    "1:  cmp x1, #64\n   b.lo 5f\n"
    "ldp q0, q1, [x0]\n   ldp q2, q3, [x0, #32]\n"
    "add x0, x0, #64\n   sub x1, x1, #64\n   b 1b\n"
    "5:  cbz x1, 9f\n"
    "6:  ldr x3, [x0]\n   add x2, x2, x3\n   add x0, x0, #8\n"
    "subs x1, x1, #8\n   b.hi 6b\n   9:  mov x0, x2\n"
#else
    "li a2, 0\n   beqz a1, 9f\n"
    "1:  li a3, 8\n   bltu a1, a3, 9f\n"
    "ld a4, 0(a0)\n   add a2, a2, a4\n   addi a0, a0, 8\n"
    "addi a1, a1, -8\n   j 1b\n   9:  mv a0, a2\n"
#endif
    ASM_RET
    ASM_END(grep_floor_read)
);

positive grep_floor_read(address_any data, positive size);
p64 grep_floor_ticks(void);

static positive reference(p8 address_to data, positive size)
{
        positive count = 0;
        positive start = 0;

        while (start < size)
        {
                positive stop = start;

                while (stop < size && data[stop] != '\n')
                        stop++;

                for (positive at = start; at + 6 <= stop; at++)
                        if (!memory_compare(data + at, needle, 6))
                        {
                                count++;
                                break;
                        }

                start = stop + (stop < size);
        }

        return count;
}

NOT_INLINED static positive repeated(p8 address_to data, positive size)
{
        positive count = 0;
        positive at = 0;

        while (at + 6 <= size)
        {
                p8 address_to found = memory_search_prepared(
                    data + at, size - at, needle, 6, anchors.x, anchors.y);

                if (!found)
                        break;

                count++;
                found += 6;

                p8 address_to delimiter = memory_first_of(
                    found, '\n', size - (positive)(found - data));

                if (!delimiter)
                        break;

                at = (positive)(delimiter - data) + 1;
        }

        return count;
}

NOT_INLINED static positive fused(p8 address_to data, positive size)
{
        return memory_count_records_with_prepared(
            data, size, needle, 6, anchors.x, anchors.y, '\n');
}

static p64 run(positive which)
{
        p64 started = grep_floor_ticks();

        for (positive round = 0; round < ROUNDS; round++)
                sink += which == 0 ? grep_floor_read(block, ROOM)
                                   : which == 1 ? memory_count(block, ROOM, '\n')
                                   : which == 2 ? repeated(block, ROOM)
                                                : fused(block, ROOM);

        return grep_floor_ticks() - started;
}

static fn order(positive address_to values)
{
        for (positive i = 1; i < TRIES; i++)
        {
                positive value = values[i];
                positive at = i;

                while (at && values[at - 1] > value)
                {
                        values[at] = values[at - 1];
                        at--;
                }

                values[at] = value;
        }
}

static fn row(string_address name, positive which)
{
        positive samples[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
                samples[trial] = (positive)run(which);

        order(samples);
        positive ticks = samples[TRIES / 2];
        positive thousandths = ticks * 1024 * 1000 /
                               ((positive)ROOM * ROUNDS);

        string_format(log, "  %s  %p.%p ticks/KiB\n", name,
                      thousandths / 1000, thousandths % 1000);
}

static fn gap(void)
{
        positive ratios[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 floor_time;
                p64 fused_time;

                if (trial & 1)
                {
                        fused_time = run(3);
                        floor_time = run(0);
                }
                else
                {
                        floor_time = run(0);
                        fused_time = run(3);
                }

                ratios[trial] = (positive)(fused_time * 10000 /
                                            (floor_time ? floor_time : 1));
        }

        order(ratios);
        positive ratio = ratios[TRIES / 2];

        if (ratio < 10000)
                string_format(log, "  lower-bound status  unresolved "
                                   "(read-only candidate slower than fused)\n");
        else
                string_format(log, "  fused gap to read-only bound  %p.%p%% slower\n",
                              (ratio - 10000) / 100,
                              (ratio - 10000) % 100);
}

static bipolar named(string_address name)
{
        if (!string_compare(name, (string_address)"read"))
                return 0;
        if (!string_compare(name, (string_address)"delimiter"))
                return 1;
        if (!string_compare(name, (string_address)"repeated"))
                return 2;
        if (!string_compare(name, (string_address)"fused"))
                return 3;

        return -1;
}

static bool boundaries(void)
{
        p8 small[320];

        for (positive size = 0; size <= 257; size++)
                for (positive shift = 0; shift < 23; shift++)
                {
                        for (positive at = 0; at < size; at++)
                                small[at] = (at + shift) % 17 == 16 ? '\n' : 'x';

                        for (positive at = shift; at + 6 <= size; at += 43)
                                memory_copy(small + at, needle, 6);

                        positive expected = reference(small, size);

                        if (repeated(small, size) != expected ||
                            fused(small, size) != expected)
                                return false;
                }

        return true;
}

b32 main(void)
{
        moonwater_cpu_detect();
        anchors = memory_search_prepare(needle, 6, false);

        for (positive at = 0; at < ROOM; at++)
                block[at] = (p8)"alpha:123:plain\n"[at % 16];

        for (positive at = 121; at + 6 < ROOM; at += 272)
                memory_copy(block + at, needle, 6);

        /* A named lane gives perf a long, quiet interval for retired-event
           counters. Its denominator is 16 * ROUNDS * ROOM bytes. */
        if (program_argument_count() > 1)
        {
                bipolar which = named(program_argument(1));

                if (which < 0)
                        return 2;

                for (positive pass = 0; pass < 16; pass++)
                        sink += (positive)run((positive)which);

                return 0;
        }

        if (!boundaries() || repeated(block, ROOM) != fused(block, ROOM))
        {
                string_format(log, "grep record count check failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "grep record count, %p-byte resident input\n",
                      (positive)ROOM);
        row((string_address)"read-only traffic candidate", 0);
        row((string_address)"full-byte delimiter count", 1);
        row((string_address)"repeated search + newline", 2);
        row((string_address)"fused semantic scan", 3);
        gap();
        log_flush();
        return 0;
}
