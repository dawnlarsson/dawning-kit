/* DJB2 byte hash: dependent scalar loop against four-byte assembly. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 25)

static p8 block[MAXIMUM + 16];
static volatile positive sink;

NOT_INLINED static positive former_hash(address_any block_address, positive length)
{
        p8 address_to text = block_address;
        positive value = 5381;

        for (positive at = 0; at < length; at++)
                value = value * 33 + text[at];

        return value;
}

typedef positive (*hash_call)(address_any block_address, positive length);
static hash_call volatile hash_calls[2] = {former_hash, memory_hash_33};

static bool correctness(void)
{
        static const positive offsets[] = {0, 1, 3, 7, 15, 4093, 4095};

        for (positive at = 0; at < sizeof(block); at++)
                block[at] = (p8)(at * 37 + 11);

        for (positive oi = 0; oi < sizeof(offsets) / sizeof(*offsets); oi++)
                for (positive length = 0; length <= 257; length++)
                {
                        positive offset = offsets[oi];

                        if (offset + length > sizeof(block))
                                continue;

                        if (memory_hash_33(block + offset, length) !=
                            former_hash(block + offset, length))
                                return false;
                }

        return true;
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / (length ? length : 1);

        if (rounds < 8)
                rounds = 8;
        if (rounds > (1u << 22))
                rounds = 1u << 22;
        return rounds;
}

static p64 run(bool assembly, positive length, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
                sink += hash_calls[assembly](block, length);

        return get_cpu_time() - start;
}

static fn order(positive address_to ratios)
{
        for (positive i = 1; i < TRIES; i++)
        {
                positive value = ratios[i];
                positive at = i;

                while (at && ratios[at - 1] > value)
                {
                        ratios[at] = ratios[at - 1];
                        at--;
                }

                ratios[at] = value;
        }
}

static fn row(positive length)
{
        positive ratios[TRIES];
        positive rounds = rounds_for(length);

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

        order(ratios);
        string_format(log, "  %p bytes  median asm/C %p.%p%%\n", length,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        static const positive sizes[] = {0, 1, 4, 8, 12, 16, 24,
                                         32, 64, 128, 256, 4096, MAXIMUM};

        if (!correctness())
        {
                string_format(log, "memory_hash_33 correctness failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "memory_hash_33, paired median of %p\n",
                      (positive)TRIES);
        for (positive at = 0; at < sizeof(sizes) / sizeof(*sizes); at++)
                row(sizes[at]);

        log_flush();
        return 0;
}
