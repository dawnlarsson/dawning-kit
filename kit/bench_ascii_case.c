/* ASCII-folded bounded comparison: scalar reference against library assembly. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 26)

static p8 one[MAXIMUM + 8];
static p8 two[MAXIMUM + 8];
static volatile positive sink;

static p8 folded(p8 value)
{
        return value >= 'a' && value <= 'z' ? (p8)(value - 32) : value;
}

NOT_INLINED static bipolar former_compare(p8 address_to a, p8 address_to b,
                                          positive length)
{
        for (positive at = 0; at < length; at++)
        {
                p8 left = folded(a[at]);
                p8 right = folded(b[at]);

                if (left != right)
                        return (bipolar)left - (bipolar)right;
        }

        return 0;
}

static bool correctness(void)
{
        p8 a;
        p8 b;

        for (positive left = 0; left < 256; left++)
                for (positive right = 0; right < 256; right++)
                {
                        a = (p8)left;
                        b = (p8)right;

                        bipolar want = (bipolar)folded(a) - (bipolar)folded(b);
                        b32 got = memory_compare_ascii_case(address_of a,
                                                            address_of b, 1);

                        if (got != want)
                        {
                                string_format(log, "pair check failed: %p %p signs %p %p\n",
                                              left, right, (positive)(want < 0),
                                              (positive)(got < 0));
                                return false;
                        }
                }

        for (positive left_offset = 0; left_offset < 8; left_offset++)
                for (positive right_offset = 0; right_offset < 8; right_offset++)
                        for (positive length = 0; length <= 257; length++)
                        {
                                for (positive at = 0; at < length; at++)
                                {
                                        one[left_offset + at] = (p8)(at * 37 + length);
                                        two[right_offset + at] = one[left_offset + at];

                                        if ((at & 7) == 3 && one[left_offset + at] >= 'A' &&
                                            one[left_offset + at] <= 'Z')
                                                two[right_offset + at] += 32;
                                }

                                if (memory_compare_ascii_case(one + left_offset,
                                                              two + right_offset,
                                                              length))
                                {
                                        string_format(log,
                                                      "equal check failed: offsets %p %p length %p\n",
                                                      left_offset, right_offset, length);
                                        return false;
                                }

                                if (!length)
                                        continue;

                                positive positions[] = {0, length / 2, length - 1,
                                                        7, 8, 15, 16, 31, 32};

                                for (positive which = 0;
                                     which < sizeof(positions) / sizeof(positions[0]);
                                     which++)
                                {
                                        positive position = positions[which];

                                        if (position >= length)
                                                continue;

                                        p8 address_to changed = two + right_offset + position;
                                        p8 saved = address_to changed;
                                        address_to changed = saved == 0xff ? 0 : (p8)(saved + 1);

                                        bipolar want = former_compare(one + left_offset,
                                                                       two + right_offset,
                                                                       length);
                                        b32 got = memory_compare_ascii_case(
                                            one + left_offset, two + right_offset, length);

                                        if (got != want)
                                        {
                                                string_format(log,
                                                              "span check failed: offsets %p %p length %p position %p signs %p %p\n",
                                                              left_offset, right_offset, length,
                                                              position, (positive)(want < 0),
                                                              (positive)(got < 0));
                                                return false;
                                        }

                                        address_to changed = saved;
                                }
                        }

        return true;
}

static fn prepare(positive length, bool late)
{
        for (positive at = 0; at < length; at++)
        {
                p8 value = (p8)(at * 37 + 11);
                one[at] = value;
                two[at] = value >= 'A' && value <= 'Z' && (at & 1)
                              ? (p8)(value + 32)
                              : value;
        }

        if (late)
                two[length - 1] ^= 1;
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
                sink += (positive)(assembly
                                       ? memory_compare_ascii_case(one, two, length)
                                       : former_compare(one, two, length));

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

static fn row(string_address name, positive length, bool late)
{
        positive ratios[TRIES];
        positive rounds = rounds_for(length);

        prepare(length, late);

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
                string_format(log, "memory_compare_ascii_case check failed\n");
                log_flush();
                return 1;
        }

        moonwater_cpu_detect();
        string_format(log, "ASCII case compare, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"equal 8", 8, false);
        row((string_address)"equal 32", 32, false);
        row((string_address)"equal 4K", 4096, false);
        row((string_address)"equal 1M", MAXIMUM, false);
        row((string_address)"late 4K", 4096, true);
        log_flush();
        return 0;
}
