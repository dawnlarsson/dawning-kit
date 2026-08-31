/*
        Grep's repeated literal hunt: preparing the rare byte once versus
        asking memory_search to choose its anchors again after every match.
*/
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 9
#define ROOM (1u << 20)

static p8 hay[ROOM + 64];
static p8 needle[64];
static volatile positive sink;

struct prepared_search
{
        p8 address_to needle;
        positive size;
        positive anchor;
        positive second_anchor;
        bool icase;
};

static fn prepare(struct prepared_search address_to search,
                  p8 address_to wanted, positive size, bool icase)
{
        positive2 anchors = memory_search_prepare(wanted, size, icase);

        search->needle = wanted;
        search->size = size;
        search->anchor = anchors.x;
        search->second_anchor = anchors.y;
        search->icase = icase;
}

NOT_INLINED static p8 address_to prepared_find(
    p8 address_to text, positive length,
    const struct prepared_search address_to search)
{
        return search->icase
                   ? memory_search_ascii_case_prepared(
                         text, length, search->needle, search->size,
                         search->anchor)
                   : memory_search_prepared(text, length, search->needle,
                                            search->size, search->anchor,
                                            search->second_anchor);
}

static positive scan(bool prepared, positive length,
                     struct prepared_search address_to search)
{
        positive at = 0;
        positive count = 0;

        while (at <= length)
        {
                p8 address_to found = prepared
                                          ? prepared_find(hay + at, length - at,
                                                          search)
                                          : (search->icase
                                                 ? memory_search_ascii_case(
                                                       hay + at, length - at,
                                                       search->needle, search->size)
                                                 : memory_search(
                                                       hay + at, length - at,
                                                       search->needle, search->size));

                if (!found)
                        break;

                count++;
                at = (positive)(found - hay) + max(search->size, 1ull);
        }

        return count;
}

static p64 run(bool prepared, positive length,
               struct prepared_search address_to search, positive rounds)
{
        p64 started = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
                sink += scan(prepared, length, search);

        return get_cpu_time() - started;
}

static fn row(string_address name, string_address wanted, bool icase,
              positive traffic)
{
        struct prepared_search search;
        positive size = string_length(wanted);
        positive rounds = traffic / ROOM + 1;
        positive ratios[TRIES];

        memory_copy(needle, wanted, size);
        prepare(address_of search, needle, size, icase);

        if (scan(false, ROOM, address_of search) !=
            scan(true, ROOM, address_of search))
        {
                string_format(log, "  %s: answers differ\n", name);
                return;
        }

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 current;
                p64 prepared_time;

                if (trial & 1)
                {
                        prepared_time = run(true, ROOM, address_of search, rounds);
                        current = run(false, ROOM, address_of search, rounds);
                }
                else
                {
                        current = run(false, ROOM, address_of search, rounds);
                        prepared_time = run(true, ROOM, address_of search, rounds);
                }

                ratios[trial] = (positive)(prepared_time * 10000 /
                                            (current ? current : 1));
        }

        order(ratios, TRIES);
        string_format(log, "  %s  prepared/current %p.%p%%\n", name,
                      ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        moonwater_cpu_detect();

        for (positive at = 0; at < ROOM; at++)
                hay[at] = (p8)("alpha:123:plain\n"[at % 16]);

        for (positive at = 121; at + 6 < ROOM; at += 256)
                memory_copy(hay + at, (address_any)"needle", 6);

        string_format(log, "prepared grep search, paired median of %p\n",
                      (positive)TRIES);
        row((string_address)"needle every 256 bytes", (string_address)"needle",
            false, 1u << 25);
        row((string_address)"absent rare", (string_address)"zqxjv", false,
            1u << 25);
        row((string_address)"folded needle", (string_address)"NEEDLE", true,
            1u << 25);

        for (positive at = 0; at < ROOM; at++)
                hay[at] = 'a';

        row((string_address)"common false candidates",
            (string_address)"aaaaaaab", false, 1u << 25);
        row((string_address)"dense matches", (string_address)"aaaa", false,
            1u << 22);

        log_flush();
        return 0;
}
