/* Hot text-loop folds against the literal scalar loops they replace. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define BLOCK 4096
#define ROUNDS (1u << 15)

static p8 one[BLOCK];
static p8 two[BLOCK];
static volatile positive sink;

NOT_INLINED static bipolar former_compare(p8 address_to a, positive la,
                                          p8 address_to b, positive lb)
{
        positive at = 0;
        positive length = la < lb ? la : lb;

        while (at < length)
        {
                if (a[at] != b[at])
                        return a[at] < b[at] ? -1 : 1;
                at++;
        }

        return la == lb ? 0 : la < lb ? -1 : 1;
}

NOT_INLINED static bipolar folded_compare(p8 address_to a, positive la,
                                          p8 address_to b, positive lb)
{
        positive length = la < lb ? la : lb;
        bipolar order = memory_compare(a, b, length);

        if (order)
                return order < 0 ? -1 : 1;

        return la == lb ? 0 : la < lb ? -1 : 1;
}

NOT_INLINED static positive former_equal_block(p8 address_to a,
                                               p8 address_to b,
                                               positive length)
{
        positive lines = 0;

        for (positive at = 0; at < length; at++)
        {
                if (a[at] != b[at])
                        return lines + 1;
                if (a[at] == '\n')
                        lines++;
        }

        return lines;
}

NOT_INLINED static positive folded_equal_block(p8 address_to a,
                                               p8 address_to b,
                                               positive length)
{
        if (memory_compare(a, b, length))
                return 1;

        return memory_count(a, length, '\n');
}

NOT_INLINED static positive former_late_difference(p8 address_to a,
                                                   p8 address_to b,
                                                   positive length)
{
        positive lines = 0;
        positive at = 0;

        while (at < length && a[at] == b[at])
        {
                if (a[at] == '\n')
                        lines++;
                at++;
        }

        return at + lines;
}

NOT_INLINED static positive folded_late_difference(p8 address_to a,
                                                   p8 address_to b,
                                                   positive length)
{
        if (!memory_compare(a, b, length))
                return length + memory_count(a, length, '\n');

        positive prefix = 0;

        while (a[prefix] == b[prefix])
                prefix++;

        return prefix + memory_count(a, prefix, '\n');
}

NOT_INLINED static positive former_newline(p8 address_to data, positive length)
{
        positive at = 0;

        while (at < length && data[at] != '\n')
                at++;

        return at;
}

NOT_INLINED static positive folded_newline(p8 address_to data, positive length)
{
        p8 address_to found = memory_first_of(data, '\n', length);
        return found ? (positive)(found - data) : length;
}

static p64 compare_once(bool folded)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
                sink += (positive)(folded ? folded_compare(one, BLOCK, two, BLOCK)
                                          : former_compare(one, BLOCK, two, BLOCK));

        return get_cpu_time() - start;
}

static p64 equal_once(bool folded)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
                sink += folded ? folded_equal_block(one, two, BLOCK)
                               : former_equal_block(one, two, BLOCK);

        return get_cpu_time() - start;
}

static p64 newline_once(bool folded)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
                sink += folded ? folded_newline(one, BLOCK)
                               : former_newline(one, BLOCK);

        return get_cpu_time() - start;
}

static p64 late_once(bool folded)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
                sink += folded ? folded_late_difference(one, two, BLOCK)
                               : former_late_difference(one, two, BLOCK);

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

static fn row(string_address name, p64 (*run)(bool))
{
        positive ratios[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 folded;

                if (trial & 1)
                {
                        folded = run(true);
                        former = run(false);
                }
                else
                {
                        former = run(false);
                        folded = run(true);
                }

                ratios[trial] = (positive)(folded * 10000 /
                                            (former ? former : 1));
        }

        order(ratios);
        string_format(log, "  %s  median folded/scalar %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        for (positive at = 0; at < BLOCK; at++)
        {
                one[at] = (p8)(at * 37 + 11);
                if ((at & 63) == 63)
                        one[at] = '\n';
                two[at] = one[at];
        }

        moonwater_cpu_detect();
        string_format(log, "hot text folds, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"sort equal 4K", compare_once);
        row((string_address)"cmp equal+line count 4K", equal_once);

        two[BLOCK - 1] ^= 1;
        row((string_address)"cmp late difference 4K", late_once);
        two[BLOCK - 1] ^= 1;

        /* sed P/D's first newline is deliberately late for the scan row. */
        for (positive at = 0; at < BLOCK; at++)
                one[at] = 'x';
        one[BLOCK - 1] = '\n';
        row((string_address)"sed newline late 4K", newline_once);

        log_flush();
        return 0;
}
