/* Bounded reverse byte search: scalar reference against library assembly. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 26)
#define NEEDLE 0xa7

static p8 block[MAXIMUM + 64];
static volatile positive sink;

NOT_INLINED static address_any former_last(address_any memory, b8 value,
                                           positive length)
{
        p8 address_to bytes = memory;

        while (length)
        {
                length--;
                if (bytes[length] == (p8)value)
                        return bytes + length;
        }

        return null;
}

static bool correctness(void)
{
        static const positive values[] = {0, 37, 255};
        p8 guarded[336];

        if (memory_last_of(null, 0, 0) != null)
                return false;

        for (positive offset = 0; offset < 16; offset++)
                for (positive length = 0; length <= 257; length++)
                        for (positive v = 0; v < sizeof(values) / sizeof(values[0]); v++)
                        {
                                p8 needle = (p8)values[v];

                                for (positive at = 0; at < sizeof(guarded); at++)
                                        guarded[at] = (p8)(needle + 1);

                                if (memory_last_of(guarded + offset, (b8)needle,
                                                   length) != null)
                                        return false;

                                for (positive position = 0; position < length; position++)
                                {
                                        guarded[offset + position] = needle;

                                        if (memory_last_of(guarded + offset, (b8)needle,
                                                           length) !=
                                            guarded + offset + position)
                                                return false;

                                        guarded[offset + position] = (p8)(needle + 1);
                                }

                                if (length > 1)
                                {
                                        guarded[offset] = needle;
                                        guarded[offset + length - 1] = needle;

                                        if (memory_last_of(guarded + offset, (b8)needle,
                                                           length) !=
                                            guarded + offset + length - 1)
                                                return false;
                                }
                        }

        return true;
}

static fn prepare(positive length, positive hit)
{
        for (positive at = 0; at < length; at++)
                block[at] = (p8)(at * 2 + 2);

        if (hit < length)
                block[hit] = NEEDLE;
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / length;
        if (rounds < 8) rounds = 8;
        if (rounds > (1u << 20)) rounds = 1u << 20;
        return rounds;
}

static p64 run(bool assembly, positive length, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
        {
                address_any found = assembly
                                        ? memory_last_of(block, (b8)NEEDLE, length)
                                        : former_last(block, (b8)NEEDLE, length);
                sink += (positive)found;
        }

        return get_cpu_time() - start;
}

static fn row(string_address name, positive length, positive hit)
{
        positive ratios[TRIES];
        positive rounds = rounds_for(length);

        prepare(length, hit);

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 assembly;

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

                ratios[trial] = (positive)(assembly * 10000 /
                                            (former ? former : 1));
        }

        order(ratios, TRIES);
        string_format(log, "  %s  median asm/C %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        if (!correctness())
        {
                string_format(log, "memory_last_of check failed\n");
                log_flush();
                return 1;
        }

        moonwater_cpu_detect();
        string_format(log, "memory_last_of, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"absent 8", 8, 8);
        row((string_address)"absent 64", 64, 64);
        row((string_address)"absent 4K", 4096, 4096);
        row((string_address)"absent 1M", MAXIMUM, MAXIMUM);
        row((string_address)"front hit 4K", 4096, 0);
        row((string_address)"end hit 64", 64, 63);
        log_flush();
        return 0;
}
