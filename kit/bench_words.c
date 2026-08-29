/* Stateful ASCII word counting: scalar reference against library assembly. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM 65536
#define TARGET_BYTES (1u << 26)

static p8 block[MAXIMUM + 16];
static volatile positive sink;

static bool is_space(p8 value)
{
        return value == 0x20 || (value >= 0x09 && value <= 0x0d);
}

NOT_INLINED static positive2 former_words(address_any memory, positive length,
                                          bool inside)
{
        p8 address_to bytes = memory;
        positive2 answer = {.x = 0, .y = inside ? 1 : 0};

        for (positive at = 0; at < length; at++)
        {
                bool next = !is_space(bytes[at]);

                if (next && !answer.y)
                        answer.x++;

                answer.y = next;
        }

        return answer;
}

static bool same(positive2 a, positive2 b)
{
        return a.x == b.x && a.y == b.y;
}

static bool correctness(void)
{
        p8 byte;

        for (positive value = 0; value < 256; value++)
                for (positive inside = 0; inside < 2; inside++)
                {
                        byte = (p8)value;
                        if (!same(memory_count_words(address_of byte, 1, (bool)inside),
                                  former_words(address_of byte, 1, (bool)inside)))
                                return false;
                }

        for (positive offset = 0; offset < 16; offset++)
                for (positive length = 0; length <= 257; length++)
                {
                        static const p8 alphabet[] = {
                            0, 8, 9, 10, 11, 12, 13, 14, 31, 32, 33, 127, 128, 255,
                        };

                        for (positive at = 0; at < length; at++)
                                block[offset + at] = alphabet[(at * 7 + length) %
                                                              sizeof(alphabet)];

                        for (positive inside = 0; inside < 2; inside++)
                        {
                                positive2 want = former_words(block + offset, length,
                                                               (bool)inside);
                                positive2 got = memory_count_words(block + offset, length,
                                                                   (bool)inside);

                                if (!same(want, got))
                                        return false;

                                positive splits[] = {0, length / 2, length, 7, 8,
                                                     15, 16, 31, 32, 63, 64};

                                for (positive which = 0;
                                     which < sizeof(splits) / sizeof(splits[0]);
                                     which++)
                                {
                                        positive split = splits[which];

                                        if (split > length)
                                                continue;

                                        positive2 first = memory_count_words(
                                            block + offset, split, (bool)inside);
                                        positive2 second = memory_count_words(
                                            block + offset + split, length - split,
                                            (bool)first.y);

                                        if (first.x + second.x != want.x ||
                                            second.y != want.y)
                                                return false;
                                }
                        }
                }

        return true;
}

static fn prepare(positive length, positive shape)
{
        static const p8 prose[] = "one two\tthree\nfour\r\nfive six ";

        for (positive at = 0; at < length; at++)
                block[at] = shape == 0   ? ' '
                            : shape == 1 ? 'x'
                            : shape == 2 ? ((at & 1) ? ' ' : 'x')
                                         : prose[at % (sizeof(prose) - 1)];
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / length;
        if (rounds < 16) rounds = 16;
        if (rounds > (1u << 20)) rounds = 1u << 20;
        return rounds;
}

static p64 run(bool assembly, positive length, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
        {
                positive2 answer = assembly
                                       ? memory_count_words(block, length, false)
                                       : former_words(block, length, false);
                sink += answer.x + answer.y;
        }

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

static fn row(string_address name, positive length, positive shape)
{
        positive ratios[TRIES];
        positive rounds = rounds_for(length);

        prepare(length, shape);

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
        string_format(log, "  %s  median asm/C %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        if (!correctness())
        {
                string_format(log, "memory_count_words check failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "memory_count_words, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"spaces 64", 64, 0);
        row((string_address)"word 64", 64, 1);
        row((string_address)"alternating 64", 64, 2);
        row((string_address)"prose 64", 64, 3);
        row((string_address)"spaces 64K", MAXIMUM, 0);
        row((string_address)"word 64K", MAXIMUM, 1);
        row((string_address)"alternating 64K", MAXIMUM, 2);
        row((string_address)"prose 64K", MAXIMUM, 3);
        log_flush();
        return 0;
}
