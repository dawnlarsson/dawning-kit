#include "../compiler_memory.c"
/*
        Every needle length that is known where the call is written.

        memory_search expands in place when the needle's length is a literal,
        and what it expands to depends on the number: nothing at all is a
        pointer to the front of the haystack, one byte is memory_first_of, and
        anything longer is still the call. Nothing that takes its needle
        length from a loop counter can reach any of that, because the choice
        is made by the compiler, from the token. So every length is written
        out here.

        Two things are checked at once and they need different apparatus.

        That the answer is right: the expansion is compared against the
        routine it replaces and against the obvious loop, at every haystack
        length from nothing to past a block, with the needle absent, at the
        front, at the last position it fits, in the middle, and at every
        position at once. The overlap case is in there by name -- "aab" is in
        "aaab", and a scan that resumes past what it matched cannot see it --
        because that is the one a rewrite gets wrong.

        That nothing was read that was not asked for: guard bytes cannot show
        this. memory_copy writes, so a store one byte past the size lands in a
        guard and is visible; memory_search only reads, and an expansion that
        loads a whole word past the end of the haystack corrupts nothing and
        passes every guard there is. So the haystack is put with its last byte
        against the last byte of a mapped page and the page after it is given
        back to the kernel. A read one byte too far is then a fault and not a
        silence.
*/

#define TOP 48
#define GUARD 32
#define SLAB (256u * 1024u)
#define PAGE (64u * 1024u)

static positive checks;
static positive failures;

static p8 arena[GUARD + TOP + GUARD];
static p8 address_to edge;              // one past the last mapped byte

#define FIELD (arena + GUARD)

static p8 guard_byte(positive i) { return (p8)(0x5a ^ (i * 7)); }

static void fail(const p8 address_to what, positive length, positive size,
                 const p8 address_to why)
{
        failures++;
        string_format(log, "FAIL %s needle %p hay %p: %s\n", what, length, size, why);
}

//      What a search means, spelled out, for the two quick ones to be judged
//      against. An empty needle is at the front, which is what memmem says
//      and is the case a rewrite forgets.
static address_any reference_search(const p8 address_to hay, positive size,
                                    const p8 address_to want, positive length)
{
        if (length == 0)
                return (address_any)hay;
        if (length > size)
                return null;
        for (positive at = 0; at + length <= size; at++) {
                positive i = 0;
                while (i < length && hay[at + i] == want[i])
                        i++;
                if (i == length)
                        return (address_any)(hay + at);
        }
        return null;
}

/*
        One haystack, three answers, and the guards either side looked at
        afterwards: a search writes nothing, so a byte that moved is a defect
        whatever else was right.
*/
static void judge(const p8 address_to what, address_any got, address_any want,
                  positive length, positive size)
{
        checks++;
        if (got != want) {
                fail(what, length, size, "a different answer from the routine");
                return;
        }
        for (positive i = 0; i < GUARD; i++)
                if (arena[i] != guard_byte(i) ||
                    arena[GUARD + TOP + i] != guard_byte(GUARD + TOP + i)) {
                        fail(what, length, size, "wrote outside the haystack");
                        return;
                }
}

//      The haystack, built so that the needle is where the placement says and
//      nowhere earlier. Byte 'a' fills, 'b' breaks, and the needles below are
//      spelled out of both.
static void lay(positive size, positive where, const p8 address_to want,
                positive length)
{
        for (positive i = 0; i < sizeof(arena); i++)
                arena[i] = guard_byte(i);
        for (positive i = 0; i < size; i++)
                FIELD[i] = (p8)('a' + (i % 3));
        if (where != (positive)-1 && where + length <= size)
                for (positive i = 0; i < length; i++)
                        FIELD[where + i] = want[i];
}

/*
        The same haystack again with its last byte against the edge of the
        map. Only the answer is compared here: the arena's guards are not
        this copy's, and the point of this copy is the fault it does not take.
*/
static void judge_at_edge(const p8 address_to what, const p8 address_to want,
                          positive length, positive size, positive where)
{
        p8 address_to hay = edge - size;

        for (positive i = 0; i < size; i++)
                hay[i] = (p8)('a' + (i % 3));
        if (where != (positive)-1 && where + length <= size)
                for (positive i = 0; i < length; i++)
                        hay[where + i] = want[i];

        checks++;
        if (memory_search((address_any)hay, size, want, length) !=
            reference_search(hay, size, want, length))
                fail(what, length, size, "a different answer against the map edge");
}

/*
        One literal needle length, every haystack length, every placement.

        The length is written into the call as a token, which is the only way
        the expansion is reached at all, and the placements are the ones that
        separate a right answer from one that happens to agree: absent, at the
        front, at the last position it fits, in the middle, and -- through the
        overlap sweep below -- at more than one position at once.
*/
#define CHECK_SEARCH(K, LITERAL)                                              \
        do {                                                                  \
                const p8 address_to want = (const p8 address_to)LITERAL;      \
                for (positive size = 0; size <= TOP; size++) {                \
                        static const positive spots[] = {                     \
                                (positive)-1, 0, 1, 2, 7, TOP / 2, TOP        \
                        };                                                    \
                        for (positive s = 0; s < sizeof(spots) / sizeof(*spots); s++) { \
                                positive where = spots[s];                    \
                                if (where != (positive)-1 && where + (K) > size) \
                                        where = (size >= (K)) ? size - (K)    \
                                                              : (positive)-1; \
                                lay(size, where, want, (K));                  \
                                judge((const p8 address_to)"memory_search",   \
                                      memory_search((address_any)FIELD, size, \
                                                    LITERAL, (K)),            \
                                      reference_search(FIELD, size, want, (K)), \
                                      (K), size);                             \
                                judge((const p8 address_to)"against routine", \
                                      memory_search((address_any)FIELD, size, \
                                                    LITERAL, (K)),            \
                                      (memory_search)((address_any)FIELD, size, \
                                                      LITERAL, (K)),          \
                                      (K), size);                             \
                                judge_at_edge((const p8 address_to)"memory_search", \
                                              want, (K), size, where);        \
                        }                                                     \
                }                                                             \
        } while (0)

//      The case-folded sibling, whose routine has no one byte redirect of its
//      own, so the expansion is the only place one exists. Both spellings of
//      a letter are asked for, and a byte that is not a letter as well.
#define CHECK_SEARCH_CASE(K, LITERAL)                                         \
        do {                                                                  \
                const p8 address_to want = (const p8 address_to)LITERAL;      \
                for (positive size = 0; size <= TOP; size++)                  \
                        for (positive s = 0; s < 4; s++) {                    \
                                positive where = s == 0 ? (positive)-1        \
                                        : (size >= (K) ? (s - 1) % (size - (K) + 1) \
                                                       : (positive)-1);       \
                                lay(size, where, want, (K));                  \
                                judge((const p8 address_to)"memory_search_ascii_case", \
                                      memory_search_ascii_case(               \
                                              (address_any)FIELD, size,       \
                                              LITERAL, (K)),                  \
                                      (memory_search_ascii_case)(             \
                                              (address_any)FIELD, size,       \
                                              LITERAL, (K)),                  \
                                      (K), size);                             \
                        }                                                     \
        } while (0)

/*
        "aab" in "aaab", and every other shape a resume past the match would
        lose. The haystack is a run of one letter with a break somewhere in
        it, so the needle matches at overlapping positions and only the first
        of them is the answer.
*/
#define CHECK_OVERLAP(K, LITERAL)                                             \
        do {                                                                  \
                const p8 address_to want = (const p8 address_to)LITERAL;      \
                for (positive size = 0; size <= TOP; size++)                  \
                        for (positive breaks = 0; breaks <= size; breaks++) { \
                                for (positive i = 0; i < sizeof(arena); i++)  \
                                        arena[i] = guard_byte(i);             \
                                for (positive i = 0; i < size; i++)           \
                                        FIELD[i] = 'a';                       \
                                if (breaks < size)                            \
                                        FIELD[breaks] = 'b';                  \
                                judge((const p8 address_to)"overlap",         \
                                      memory_search((address_any)FIELD, size, \
                                                    LITERAL, (K)),            \
                                      reference_search(FIELD, size, want, (K)), \
                                      (K), size);                             \
                        }                                                     \
        } while (0)

//      Every length, once. Called once per tier below.
static void sweep(void)
{
        CHECK_SEARCH(0, "");
        CHECK_SEARCH(1, "b");
        CHECK_SEARCH(2, "bc");
        CHECK_SEARCH(3, "abc");
        CHECK_SEARCH(4, "bcab");
        CHECK_SEARCH(5, "cabca");
        CHECK_SEARCH(6, "abcabc");
        CHECK_SEARCH(7, "bcabcab");
        CHECK_SEARCH(8, "cabcabca");
        CHECK_SEARCH(9, "abcabcabc");
        CHECK_SEARCH(16, "abcabcabcabcabca");
        CHECK_SEARCH(33, "abcabcabcabcabcabcabcabcabcabcabc");

        CHECK_SEARCH_CASE(0, "");
        CHECK_SEARCH_CASE(1, "b");
        CHECK_SEARCH_CASE(1, "B");
        CHECK_SEARCH_CASE(1, "-");
        CHECK_SEARCH_CASE(2, "bc");
        CHECK_SEARCH_CASE(3, "aBc");

        CHECK_OVERLAP(1, "b");
        CHECK_OVERLAP(2, "ab");
        CHECK_OVERLAP(3, "aab");
        CHECK_OVERLAP(4, "aaab");
        CHECK_OVERLAP(5, "aaaab");
        CHECK_OVERLAP(8, "aaaaaaab");
}

//      Every byte value as a one byte needle, since that length is the one
//      the expansion actually replaces and a byte the hunt treats specially
//      -- zero, 0xff, a letter -- would otherwise never be asked for.
static void every_byte(void)
{
        for (positive value = 0; value < 256; value++)
                for (positive size = 0; size <= 8; size++) {
                        p8 one = (p8)value;

                        for (positive i = 0; i < sizeof(arena); i++)
                                arena[i] = guard_byte(i);
                        for (positive i = 0; i < size; i++)
                                FIELD[i] = (p8)(value + i);

                        judge((const p8 address_to)"one byte",
                              memory_search((address_any)FIELD, size, &one, 1),
                              (memory_search)((address_any)FIELD, size, &one, 1),
                              1, size);
                }
}

/*
        Once per tier, not once.

        The wide bodies are chosen at run time, by a byte, and a machine that
        has AVX2 takes that branch every time -- so a whole tier ships having
        never run. memory_first_of, which is what a one byte needle becomes,
        has three of them.
*/
b32 main(void)
{
        p8 address_to slab;

        moonwater_cpu_detect();

        //      A page given back to the kernel, and the haystack laid so its
        //      last byte is the last byte still mapped before it.
        slab = (p8 address_to)memory(SLAB);
        if ((positive)slab > (positive)-4096 || slab == null) {
                string_format(log, "no memory for the edge\n");
                log_flush();
                return 1;
        }
        edge = (p8 address_to)(((positive)slab + PAGE - 1) & ~(positive)(PAGE - 1))
               + PAGE;
        memory_free((address_any)edge, PAGE);

        p8 found_avx2 = cpu_has_avx2;
        p8 found_avx512 = cpu_has_avx512;

        for (b32 tier = 0; tier < 2; tier++) {
                cpu_has_avx2 = tier ? 0 : found_avx2;
                cpu_has_avx512 = tier ? 0 : found_avx512;

                positive before = failures;
                sweep();
                every_byte();

                string_format(log, "  %s: %p checks, %p failures\n",
                              tier ? (const p8 address_to)"narrow"
                                   : (const p8 address_to)"as found",
                              checks, failures - before);
        }

        cpu_has_avx2 = found_avx2;
        cpu_has_avx512 = found_avx512;

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
