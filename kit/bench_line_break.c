/* Canvas line breaking: the former byte walk against shared bounded hunts. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 9
#define MAXIMUM 4096
#define TARGET_BYTES (1u << 25)

static p8 text[MAXIMUM];
static volatile positive sink;

NOT_INLINED static positive former_line_end(string_address input,
                                             positive length,
                                             positive start,
                                             positive columns,
                                             bool wrap)
{
        positive i;
        positive last_space = 0;

        for (i = start; i < length; i++)
        {
                if (input[i] == '\n')
                        return i;

                if (!wrap)
                        continue;

                if (input[i] == ' ')
                        last_space = i;

                if (i - start + 1 > columns)
                        return last_space > start ? last_space : i;
        }

        return length;
}

NOT_INLINED static positive shared_line_end(string_address input,
                                             positive length,
                                             positive start,
                                             positive columns,
                                             bool wrap)
{
        positive remaining;
        positive scanned;
        string_address found;

        if (start >= length)
                return length;

        remaining = length - start;
        scanned = remaining;

        if (wrap && remaining > columns)
                scanned = columns + 1;

        found = memory_first_of((address_any)(input + start), '\n', scanned);

        if (found)
                return (positive)(found - input);

        if (!wrap || remaining <= columns)
                return length;

        found = memory_last_of((address_any)(input + start), ' ', scanned);

        return found && found > input + start ? (positive)(found - input)
                                              : start + columns;
}

static positive random_state = 0x93d7654bu;

static positive next_random(void)
{
        random_state = random_state * 1664525u + 1013904223u;
        return random_state;
}

static bool correctness(void)
{
        for (positive length = 0; length < 257; length++)
                for (positive trial = 0; trial < 257; trial++)
                {
                        positive columns = next_random() % 65;
                        positive start = next_random() % (length + 2);
                        bool wrap = (bool)(next_random() & 1);

                        for (positive at = 0; at < length; at++)
                        {
                                positive pick = next_random() & 31;
                                text[at] = pick == 0 ? '\n' : pick < 6 ? ' '
                                                                         : 'a';
                        }

                        if (former_line_end(text, length, start, columns, wrap) !=
                            shared_line_end(text, length, start, columns, wrap))
                                return false;
                }

        return true;
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / (length ? length : 1);
        if (rounds < 32) rounds = 32;
        if (rounds > (1u << 20)) rounds = 1u << 20;
        return rounds;
}

static p64 run(bool shared, positive length, positive columns, bool wrap,
               positive rounds)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
                sink += shared ? shared_line_end(text, length, 0, columns, wrap)
                               : former_line_end(text, length, 0, columns, wrap);

        return get_cpu_time() - start;
}

static void row(string_address name, positive length, positive columns, bool wrap,
                positive space)
{
        positive ratios[TRIES];
        positive rounds = rounds_for(length);

        memory_fill(text, 'a', length);
        if (space < length)
                text[space] = ' ';

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 shared;

                if (trial & 1)
                {
                        shared = run(true, length, columns, wrap, rounds);
                        former = run(false, length, columns, wrap, rounds);
                }
                else
                {
                        former = run(false, length, columns, wrap, rounds);
                        shared = run(true, length, columns, wrap, rounds);
                }

                ratios[trial] = (positive)(shared * 10000 / (former ? former : 1));
        }

        order(ratios, TRIES);
        string_format(log, "  %s  shared/C %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        if (!correctness())
        {
                string_format(log, "line-break correctness failed\n");
                log_flush();
                return 1;
        }

        moonwater_cpu_detect();
        string_format(log, "Canvas line break, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"label 8", 8, 8, false, MAXIMUM);
        row((string_address)"label 16", 16, 16, false, MAXIMUM);
        row((string_address)"label 32", 32, 32, false, MAXIMUM);
        row((string_address)"label 64", 64, 64, false, MAXIMUM);
        row((string_address)"label 128", 128, 128, false, MAXIMUM);
        row((string_address)"wrap 16 no space", 128, 16, true, MAXIMUM);
        row((string_address)"wrap 32 mid space", 128, 32, true, 20);
        row((string_address)"wrap 64 mid space", 192, 64, true, 40);
        row((string_address)"wrap 80 mid space", 256, 80, true, 48);
        log_flush();
        return 0;
}
