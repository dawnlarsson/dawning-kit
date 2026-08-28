/* memory_reverse and the rev fold against the literal C shapes they replace. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 7
#define MAXIMUM ((1u << 20) + 128)
#define OUT_MAX (1u << 16)
#define TARGET_BYTES (1u << 22)

static p8 block[MAXIMUM];
static p8 output[OUT_MAX];
static positive output_used;
static volatile positive sink;

NOT_INLINED address_any former_memory_reverse(address_any memory, positive size)
{
        p8 address_to left = memory;
        p8 address_to right = left + size;

        while (left < right)
        {
                p8 value;

                right--;
                if (left >= right)
                        break;

                value = address_to left;
                address_to left++ = address_to right;
                address_to right = value;
        }

        return memory;
}

/*
        These are text_put_character and the bounded half of text_put, with a
        flush represented by a volatile observation instead of a system call.
        The former rev body really did call the first once per byte.  The fold
        reverses the line in place and calls the second once, so this measures
        the actual old/new memory work without charging either side for I/O.
*/
static inline fn former_flush()
{
        if (output_used)
                sink += output[0] + output[output_used - 1] + output_used;
        output_used = 0;
}

static inline fn former_put_character(p8 value)
{
        if (output_used == OUT_MAX)
                former_flush();

        output[output_used++] = value;
}

NOT_INLINED fn former_rev_line(p8 address_to line, positive size)
{
        for (positive c = size; c > 0; c--)
                former_put_character(line[c - 1]);
}

NOT_INLINED fn folded_rev_line(p8 address_to line, positive size)
{
        memory_reverse(line, size);

        if (output_used + size > OUT_MAX)
                former_flush();

        memory_copy(output + output_used, line, size);
        output_used += size;
}

static fn prepare(positive size)
{
        // A palindrome makes every repeated folded call emit the same bytes
        // as the former body without adding a restore copy to either timing.
        for (positive i = 0; i < (size + 1) / 2; i++)
        {
                p8 value = (p8)(i * 37 + size * 11);

                block[i] = value;
                block[size - 1 - i] = value;
        }
}

static positive rounds_for(positive size)
{
        positive rounds = TARGET_BYTES / (size ? size : 1);

        if (rounds > (1u << 18))
                rounds = 1u << 18;
        if (rounds < 8)
                rounds = 8;
        return rounds;
}

static p64 primitive_run(bool assembly, positive size, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
        {
                address_any answer = assembly ? memory_reverse(block, size)
                                              : former_memory_reverse(block, size);

                sink += (positive)answer + block[0] + block[size - 1];
        }

        return get_cpu_time() - start;
}

static p64 rev_run(bool folded, positive size, positive rounds)
{
        output_used = 0;
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
                if (folded)
                        folded_rev_line(block, size);
                else
                        former_rev_line(block, size);

        p64 finish = get_cpu_time();
        former_flush();
        return finish - start;
}

static fn primitive_row(positive size)
{
        positive rounds = rounds_for(size);
        p64 former = positive_max;
        p64 assembly = positive_max;

        prepare(size);

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 one, two;

                if (trial & 1)
                {
                        two = primitive_run(true, size, rounds);
                        one = primitive_run(false, size, rounds);
                }
                else
                {
                        one = primitive_run(false, size, rounds);
                        two = primitive_run(true, size, rounds);
                }

                if (one < former) former = one;
                if (two < assembly) assembly = two;
        }

        string_format(log,
                      "  primitive %p bytes: former-C %p  assembly %p  asm/C %p%%\n",
                      size, (positive)former, (positive)assembly,
                      (positive)(assembly * 100 / (former ? former : 1)));
}

static fn rev_row(positive size)
{
        positive rounds = rounds_for(size);
        p64 former = positive_max;
        p64 folded = positive_max;

        prepare(size);

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 one, two;

                if (trial & 1)
                {
                        two = rev_run(true, size, rounds);
                        one = rev_run(false, size, rounds);
                }
                else
                {
                        one = rev_run(false, size, rounds);
                        two = rev_run(true, size, rounds);
                }

                if (one < former) former = one;
                if (two < folded) folded = two;
        }

        string_format(log,
                      "  rev fold  %p bytes: former-loop %p  folded %p  new/old %p%%\n",
                      size, (positive)former, (positive)folded,
                      (positive)(folded * 100 / (former ? former : 1)));
}

b32 main()
{
        static const positive primitive_sizes[] = {
            8, 32, 64, 256, 4096, 65536, 1u << 20,
        };
        static const positive rev_sizes[] = {8, 32, 64, 256, 4096, 32768};

        moonwater_cpu_detect();
        string_format(log, "memory_reverse, best of %p\n", (positive)TRIES);

        for (positive i = 0;
             i < sizeof(primitive_sizes) / sizeof(primitive_sizes[0]); i++)
                primitive_row(primitive_sizes[i]);

        for (positive i = 0; i < sizeof(rev_sizes) / sizeof(rev_sizes[0]); i++)
                rev_row(rev_sizes[i]);

        log_flush();
        return 0;
}
