/* In-place byte-table translation: former scalar C against library assembly. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 26)

static p8 former_block[MAXIMUM];
static p8 assembly_block[MAXIMUM];
static p8 translation[256];
static volatile positive sink;

NOT_INLINED static address_any former_translate(address_any block,
                                                positive length,
                                                address_any table_address)
{
        p8 address_to bytes = block;
        p8 address_to table = table_address;

        for (positive at = 0; at < length; at++)
                bytes[at] = table[bytes[at]];

        return block;
}

static fn prepare(positive length)
{
        for (positive at = 0; at < length; at++)
        {
                p8 value = (p8)(at * 37 + 11);
                former_block[at] = value;
                assembly_block[at] = value;
        }
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / length;

        if (rounds < 8)
                rounds = 8;
        if (rounds > (1u << 20))
                rounds = 1u << 20;
        return rounds;
}

static p64 run(bool assembly, positive length, positive rounds)
{
        p8 address_to block = assembly ? assembly_block : former_block;
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
        {
                address_any answer = assembly
                                         ? memory_translate(block, length, translation)
                                         : former_translate(block, length, translation);
                sink += (positive)answer + block[0] + block[length - 1];
        }

        return get_cpu_time() - start;
}

static bool row(positive length)
{
        positive rounds = rounds_for(length);
        positive ratios[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 assembly;

                prepare(length);

                if (trial & 1)
                {
                        assembly = run(true, length, rounds);
                        former = run(false, length, rounds);
                }
                else
                {
                        former = run(false, length, rounds);
                        assembly = run(true, length, rounds);
                }

                if (memory_compare(former_block, assembly_block, length))
                        return false;

                ratios[trial] = (positive)(assembly * 10000 /
                                            (former ? former : 1));
        }

        order(ratios, TRIES);
        string_format(log, "  %p bytes  median asm/C %p.%p%%\n", length,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
        return true;
}

static bool boundaries(void)
{
        p8 guarded[264];
        p8 expected[264];

        for (positive length = 0; length <= 257; length++)
        {
                for (positive at = 0; at < sizeof(guarded); at++)
                        guarded[at] = expected[at] = (p8)(at * 19 + length);

                for (positive at = 0; at < length; at++)
                        expected[3 + at] = translation[expected[3 + at]];

                if (memory_translate(guarded + 3, length, translation) != guarded + 3 ||
                    memory_compare(guarded, expected, sizeof(guarded)))
                        return false;
        }

        return true;
}

b32 main(void)
{
        static const positive sizes[] = {8, 64, 4096, MAXIMUM};

        for (positive value = 0; value < 256; value++)
                translation[value] = (p8)(value * 197 + 101);

        if (!boundaries())
        {
                string_format(log, "memory_translate boundary check failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "memory_translate, paired median of %p\n",
                      (positive)TRIES);

        for (positive at = 0; at < sizeof(sizes) / sizeof(sizes[0]); at++)
                if (!row(sizes[at]))
                {
                        string_format(log, "memory_translate result mismatch\n");
                        log_flush();
                        return 1;
                }

        log_flush();
        return 0;
}
