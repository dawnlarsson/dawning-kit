/* Fixed ASCII-insensitive search against the literal C loop it replaces. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 9
#define ROOM (1u << 20)

static p8 hay[ROOM + 64];
static p8 needle[64];
static volatile positive sink;

static p8 lower_ascii(p8 value)
{
        return value >= 'A' && value <= 'Z' ? (p8)(value + 32) : value;
}

NOT_INLINED static address_any former_find(p8 address_to text, positive length,
                                            p8 address_to want, positive size)
{
        if (!size)
                return text;

        p8 head = want[0];
        p8 upper = head >= 'a' && head <= 'z' ? (p8)(head - 32) : head;

        for (positive at = 0; at + size <= length;)
        {
                positive left = length - at - size + 1;
                p8 address_to low = memory_first_of(text + at, head, left);
                p8 address_to high = upper == head
                                         ? null
                                         : memory_first_of(text + at, upper, left);
                p8 address_to hit = !low ? high : (!high || low < high ? low : high);

                if (!hit)
                        return null;

                at = (positive)(hit - text);
                positive i = 1;

                while (i < size && lower_ascii(text[at + i]) == want[i])
                        i++;

                if (i == size)
                        return text + at;

                at++;
        }

        return null;
}

static p64 run(bool assembly, positive length, positive size, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
                sink += (positive)(assembly
                                       ? memory_search_ascii_case(hay, length,
                                                                  needle, size)
                                       : former_find(hay, length, needle, size));

        return get_cpu_time() - start;
}

static fn row(string_address name, positive length, string_address wanted,
              bool late, positive traffic)
{
        positive size = string_length(wanted);

        memory_copy(needle, wanted, size);

        if (late)
        {
                positive at = length - size;

                for (positive i = 0; i < size; i++)
                {
                        p8 value = needle[i];
                        hay[at + i] = value >= 'a' && value <= 'z'
                                          ? (p8)(value - 32)
                                          : value;
                }
        }

        positive rounds = traffic / (length ? length : 1) + 1;
        positive ratios[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 assembly;

                if (trial & 1)
                {
                        assembly = run(true, length, size, rounds);
                        former = run(false, length, size, rounds);
                }
                else
                {
                        former = run(false, length, size, rounds);
                        assembly = run(true, length, size, rounds);
                }

                ratios[trial] = (positive)(assembly * 10000 /
                                             (former ? former : 1));
        }

        order(ratios, TRIES);
        string_format(log, "  %s  median asm/former %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        moonwater_cpu_detect();

        for (positive i = 0; i < ROOM; i++)
                hay[i] = (p8)("alpha beta gamma delta epsilon "[i % 31]);

        string_format(log, "ASCII-insensitive fixed search, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"1 MiB absent rare", ROOM,
            (string_address)"zqxjv", false, 1u << 15);
        row((string_address)"4 KiB late hit", 4096,
            (string_address)"epsilon zeta", true, 1);

        for (positive i = 0; i < ROOM; i++)
                hay[i] = 'a';

        row((string_address)"4 KiB common false candidates", 4096,
            (string_address)"aaaaaaab", false, 1);

        for (positive i = 0; i < 96; i++)
                hay[i] = (p8)("alpha beta gamma delta epsilon "[i % 31]);

        row((string_address)"32 byte absent", 32,
            (string_address)"zeta", false, 1u << 16);
        row((string_address)"32 byte late hit", 32,
            (string_address)"epsilon", true, 1u << 16);

        log_flush();
        return 0;
}
