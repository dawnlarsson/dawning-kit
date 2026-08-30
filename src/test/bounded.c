#include "../compiler_memory.c"
/*
        Every bound that is known where the call is written.

        The bounded string routines expand in place when the bound is a
        literal, and which line they expand to depends on the number: two
        bytes is two compares, sixteen is two word loads, sixty four is still
        a call. Nothing that takes its bound from a loop counter reaches any
        of it, because the choice is made by the compiler, from the token. So
        every bound is written out here, the way src/test/exact.c writes out
        every size.

        Two things are being checked and they need different machinery.

        That nothing is written past what the caller allowed is checked with
        guard bytes, the way exact.c does: the room in front of every
        destination carries a pattern, and so does the room behind it as far
        as the routine's own precondition allows.

        That nothing is READ past the bound cannot be checked with a pattern
        at all -- a read that runs on takes bytes that are there and says
        nothing. So every subject sits with its last byte against the last
        byte of a mapped page, and the page after it is taken away. A read one
        byte past the bound is then a fault and not a quiet pass, which is
        exactly the failure a buffer at the end of a mapping would suffer in
        the field. The destinations are placed the same way, so a write one
        past the end faults rather than landing in a guard nobody inspects.

        The three routines that take a bound and a source both need two fenced
        rooms, so there are three live pages with a dead one either side of
        each.

        The contracts differ in ways that matter to where the fence goes:
        string_copy_max writes at most the bound; string_copy_max_endptr
        writes exactly the bound and does not terminate when the source filled
        it; string_copy_max_end and string_append_max are allowed the bound
        plus one, which is their documented precondition. One guard rule for
        all six would fail three of them.
*/
#define TOP 64
#define GUARD 32
#define PREFIX 7

#include "counted.inc"

static p8 address_to source_edge;
static p8 address_to twin_edge;
static p8 address_to target_edge;

static p8 model[GUARD + TOP + 8];
static p8 taken[GUARD + TOP + 8];
static p8 spare[GUARD + TOP + 8];

#define NOT_INLINED __attribute__((noinline))

//      What each of the six means, spelled out, so the expansion and the
//      routine are both judged against something neither of them is.
NOT_INLINED positive reference_length_max(string_address source, positive bound)
{
        positive walked = 0;

        while (walked < bound && source[walked])
                walked++;

        return walked;
}

NOT_INLINED b32 reference_compare_max(string_address a, string_address b,
                                      positive bound)
{
        for (positive i = 0; i < bound; i++)
        {
                if (a[i] != b[i])
                        return (b32)a[i] - (b32)b[i];

                if (!a[i])
                        return 0;
        }

        return 0;
}

NOT_INLINED fn reference_copy_max(p8 address_to into, string_address source,
                                  positive bound)
{
        for (positive i = 0; i < bound; i++)
        {
                into[i] = source[i];

                if (!source[i])
                        return;
        }
}

NOT_INLINED positive reference_copy_max_end(p8 address_to into,
                                            string_address source, positive bound)
{
        positive walked = 0;

        while (walked < bound && source[walked])
        {
                into[walked] = source[walked];
                walked++;
        }

        into[walked] = 0;

        return walked;
}

NOT_INLINED positive reference_copy_max_endptr(p8 address_to into,
                                               string_address source,
                                               positive bound)
{
        positive walked = 0;

        while (walked < bound && source[walked])
        {
                into[walked] = source[walked];
                walked++;
        }

        for (positive pad = walked; pad < bound; pad++)
                into[pad] = 0;

        return walked;
}

static fn fail(string_address what, positive bound, positive where,
               string_address why)
{
        failures++;
        string_format(log, "FAIL %s bound %p at %p: %s\n", what, bound, where, why);
}

static fn judge(string_address what, positive bound, positive where,
                positive got, positive want)
{
        checks++;

        if (got != want)
                fail(what, bound, where, "the wrong answer");
}

static fn judge_bytes(string_address what, positive bound, positive where,
                      const p8 address_to got, const p8 address_to want,
                      positive size)
{
        checks++;

        for (positive i = 0; i < size; i++)
                if (got[i] != want[i])
                {
                        fail(what, bound, where, "the wrong bytes");
                        return;
                }
}

//      Three live pages with a dead one either side of each. Nothing here is
//      freed: the program is the test.
static fn fence_open(void)
{
        p8 address_to base = (p8 address_to)memory(7 * 4096);

        for (positive page = 0; page < 7; page += 2)
                system_call_3(syscall(mprotect),
                              (positive)(address_any)(base + page * 4096), 4096,
                              FILE_PROTECT_NONE);

        source_edge = base + 2 * 4096;
        twin_edge = base + 4 * 4096;
        target_edge = base + 6 * 4096;
}

//      The subject, its last byte against the last byte of a live page.
static string_address subject(positive bound, positive terminator)
{
        string_address source = source_edge - bound;

        for (positive i = 0; i < bound; i++)
                source[i] = (p8)('a' + (i * 5 + terminator) % 23);

        if (terminator < bound)
                source[terminator] = 0;

        return source;
}

/*
        A string may end before its maximum length, and no byte beyond that
        terminator has to be mapped.  The literal-bound sweep above puts its
        subjects exactly K bytes before a page edge; at K=64 that accidentally
        aligns the AVX-512 load and cannot catch an unaligned vector crossing
        the dead page.  Put every possible short suffix at the edge while
        keeping a deliberately larger runtime bound.  A page-safe scan finds
        the final NUL; an unaligned wide load faults before it can report it.
*/
static fn length_guard_edge(void)
{
        positive bound = 128;

        for (positive suffix = 1; suffix <= 64; suffix++)
        {
                string_address source = source_edge - suffix;

                for (positive i = 0; i < suffix; i++)
                        source[i] = (p8)('a' + (i * 7 + suffix) % 23);

                source[suffix - 1] = 0;
                judge((string_address)"string_length_max page edge", bound,
                      suffix, (string_length_max)(source, bound), suffix - 1);

                string_address found = null;

                if (suffix > 1)
                {
                        source[suffix - 2] = (p8)'z';
                        found = source + suffix - 2;
                }

                judge((string_address)"string_first_of_max page edge", bound,
                      suffix,
                      (positive)(string_first_of_max)(source, bound, (p8)'z'),
                      (positive)found);

                string_address twin = twin_edge - suffix;

                for (positive i = 0; i < suffix; i++)
                        twin[i] = source[i];

                judge((string_address)"string_compare_max page edge", bound,
                      suffix,
                      (positive)(string_compare_max)(source, twin, bound), 0);
        }
}

static string_address mirror(string_address source, positive bound,
                             positive differ, p8 by)
{
        string_address twin = twin_edge - bound;

        for (positive i = 0; i < bound; i++)
                twin[i] = source[i];

        if (differ < bound)
                twin[differ] = (p8)(twin[differ] ^ by);

        return twin;
}

//      The destination, with its last allowed byte against the page edge and
//      a pattern in the room in front of it.
static p8 address_to target(positive allowed)
{
        p8 address_to into = target_edge - allowed;

        for (positive i = 0; i < allowed; i++)
                into[i] = (p8)0xc3;

        for (positive i = 1; i <= GUARD; i++)
                into[0 - i] = (p8)(0x5a ^ (i * 7));

        return into;
}

static fn keep(p8 address_to into, positive allowed, p8 address_to where)
{
        for (positive i = 0; i < GUARD; i++)
                where[i] = into[0 - GUARD + i];

        for (positive i = 0; i < allowed; i++)
                where[GUARD + i] = into[i];
}

static fn model_room(positive allowed)
{
        for (positive i = 0; i < GUARD; i++)
                model[i] = (p8)(0x5a ^ ((GUARD - i) * 7));

        for (positive i = 0; i < allowed; i++)
                model[GUARD + i] = (p8)0xc3;
}

#define LENGTH_CASE(K)                                                         \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++) {                          \
                        string_address s = subject((K), t);                    \
                        positive want = reference_length_max(s, (K));          \
                                                                               \
                        judge((string_address)"string_length_max routine",     \
                              (K), t, (string_length_max)(s, (K)), want);      \
                        judge((string_address)"string_length_max expanded",    \
                              (K), t, string_length_max(s, (K)), want);        \
                }                                                              \
        } while (0)

#define COMPARE_CASE(K)                                                        \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++)                            \
                        for (positive d = 0; d <= (K); d++)                    \
                                for (positive f = 0; f < 2; f++) {             \
                                        string_address s = subject((K), t);    \
                                        string_address w = mirror(s, (K), d,   \
                                                f ? (p8)0x80 : (p8)0x01);      \
                                        positive want = (positive)             \
                                                reference_compare_max(s, w, (K)); \
                                                                               \
                                        judge((string_address)                 \
                                              "string_compare_max routine",    \
                                              (K), d,                          \
                                              (positive)(string_compare_max)(  \
                                                      s, w, (K)), want);       \
                                        judge((string_address)                 \
                                              "string_compare_max expanded",   \
                                              (K), d,                          \
                                              (positive)string_compare_max(    \
                                                      s, w, (K)), want);       \
                                        judge((string_address)                 \
                                              "string_compare_max swapped",    \
                                              (K), d,                          \
                                              (positive)string_compare_max(    \
                                                      w, s, (K)),              \
                                              (positive)                       \
                                              reference_compare_max(w, s, (K))); \
                                }                                              \
        } while (0)

//      string_copy_max is the one of the six with no expansion -- its window
//      was a bound of four and under, and nothing in the tree passes one that
//      small -- so this pins the routine against the model instead. Which is
//      worth doing on its own: strncpy is aliased to it and strncpy pads,
//      and this routine deliberately does not.
#define COPY_CASE(K)                                                           \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++) {                          \
                        string_address s = subject((K), t);                    \
                        p8 address_to into;                                    \
                                                                               \
                        model_room((K));                                       \
                        reference_copy_max(model + GUARD, s, (K));             \
                                                                               \
                        into = target((K));                                    \
                        (string_copy_max)(into, s, (K));                       \
                        keep(into, (K), taken);                                \
                                                                               \
                        judge_bytes((string_address)"string_copy_max",         \
                                    (K), t, taken, model, GUARD + (K));        \
                }                                                              \
        } while (0)

#define END_CASE(K)                                                            \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++) {                          \
                        string_address s = subject((K), t);                    \
                        p8 address_to into;                                    \
                        positive want;                                         \
                                                                               \
                        model_room((K) + 1);                                   \
                        want = reference_copy_max_end(model + GUARD, s, (K));  \
                                                                               \
                        into = target((K) + 1);                                \
                        judge((string_address)"string_copy_max_end routine",   \
                              (K), t,                                          \
                              (positive)((string_copy_max_end)(into, s, (K)) - \
                                         into), want);                         \
                        keep(into, (K) + 1, taken);                            \
                                                                               \
                        s = subject((K), t);                                   \
                        into = target((K) + 1);                                \
                        judge((string_address)"string_copy_max_end expanded",  \
                              (K), t,                                          \
                              (positive)(string_copy_max_end(into, s, (K)) -   \
                                         into), want);                         \
                        keep(into, (K) + 1, spare);                            \
                                                                               \
                        judge_bytes((string_address)                           \
                                    "string_copy_max_end routine bytes",       \
                                    (K), t, taken, model, GUARD + (K) + 1);    \
                        judge_bytes((string_address)                           \
                                    "string_copy_max_end expanded bytes",      \
                                    (K), t, spare, model, GUARD + (K) + 1);    \
                }                                                              \
        } while (0)

#define PTR_CASE(K)                                                            \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++) {                          \
                        string_address s = subject((K), t);                    \
                        p8 address_to into;                                    \
                        positive want;                                         \
                                                                               \
                        model_room((K));                                       \
                        want = reference_copy_max_endptr(model + GUARD, s, (K)); \
                                                                               \
                        into = target((K));                                    \
                        judge((string_address)"string_copy_max_endptr routine",\
                              (K), t,                                          \
                              (positive)((string_copy_max_endptr)(into, s, (K)) \
                                         - into), want);                       \
                        keep(into, (K), taken);                                \
                                                                               \
                        s = subject((K), t);                                   \
                        into = target((K));                                    \
                        judge((string_address)                                 \
                              "string_copy_max_endptr expanded", (K), t,       \
                              (positive)(string_copy_max_endptr(into, s, (K))  \
                                         - into), want);                       \
                        keep(into, (K), spare);                                \
                                                                               \
                        judge_bytes((string_address)                           \
                                    "string_copy_max_endptr routine bytes",    \
                                    (K), t, taken, model, GUARD + (K));        \
                        judge_bytes((string_address)                           \
                                    "string_copy_max_endptr expanded bytes",   \
                                    (K), t, spare, model, GUARD + (K));        \
                }                                                              \
        } while (0)

//      strncat: the bound counts source bytes only, the terminator is always
//      written, and the room it needs is what is already in the destination
//      plus the bound plus one.
#define APPEND_CASE(K)                                                         \
        do {                                                                   \
                for (positive t = 0; t <= (K); t++) {                          \
                        string_address s = subject((K), t);                    \
                        p8 address_to into;                                    \
                        positive want;                                         \
                                                                               \
                        model_room(PREFIX + (K) + 1);                          \
                        for (positive i = 0; i < PREFIX; i++)                  \
                                model[GUARD + i] = (p8)'z';                    \
                        model[GUARD + PREFIX] = 0;                             \
                        want = reference_copy_max_end(model + GUARD + PREFIX,  \
                                                      s, (K));                 \
                                                                               \
                        into = target(PREFIX + (K) + 1);                       \
                        for (positive i = 0; i < PREFIX; i++)                  \
                                into[i] = (p8)'z';                             \
                        into[PREFIX] = 0;                                      \
                        judge((string_address)"string_append_max routine",     \
                              (K), t,                                          \
                              (positive)((string_append_max)(into, s, (K)) ==  \
                                         into), 1);                            \
                        keep(into, PREFIX + (K) + 1, taken);                   \
                                                                               \
                        s = subject((K), t);                                   \
                        into = target(PREFIX + (K) + 1);                       \
                        for (positive i = 0; i < PREFIX; i++)                  \
                                into[i] = (p8)'z';                             \
                        into[PREFIX] = 0;                                      \
                        judge((string_address)"string_append_max expanded",    \
                              (K), t,                                          \
                              (positive)(string_append_max(into, s, (K)) ==    \
                                         into), 1);                            \
                        keep(into, PREFIX + (K) + 1, spare);                   \
                                                                               \
                        judge_bytes((string_address)                           \
                                    "string_append_max routine bytes", (K), t, \
                                    taken, model,                              \
                                    GUARD + PREFIX + (K) + 1);                 \
                        judge_bytes((string_address)                           \
                                    "string_append_max expanded bytes", (K), t,\
                                    spare, model,                              \
                                    GUARD + PREFIX + (K) + 1);                 \
                }                                                              \
        } while (0)

#define EVERY_BOUND(CASE)                                                      \
        CASE(0); CASE(1); CASE(2); CASE(3); CASE(4); CASE(5); CASE(6);         \
        CASE(7); CASE(8); CASE(9); CASE(10); CASE(11); CASE(12); CASE(13);     \
        CASE(14); CASE(15); CASE(16); CASE(17); CASE(18); CASE(19); CASE(20);  \
        CASE(21); CASE(22); CASE(23); CASE(24); CASE(25); CASE(26); CASE(27);  \
        CASE(28); CASE(29); CASE(30); CASE(31); CASE(32); CASE(33); CASE(34);  \
        CASE(35); CASE(36); CASE(37); CASE(38); CASE(39); CASE(40); CASE(41);  \
        CASE(42); CASE(43); CASE(44); CASE(45); CASE(46); CASE(47); CASE(48);  \
        CASE(49); CASE(50); CASE(51); CASE(52); CASE(53); CASE(54); CASE(55);  \
        CASE(56); CASE(57); CASE(58); CASE(59); CASE(60); CASE(61); CASE(62);  \
        CASE(63); CASE(64)

b32 main(void)
{
        fence_open();

        EVERY_BOUND(LENGTH_CASE);
        EVERY_BOUND(COMPARE_CASE);
        EVERY_BOUND(COPY_CASE);
        EVERY_BOUND(END_CASE);
        EVERY_BOUND(PTR_CASE);
        EVERY_BOUND(APPEND_CASE);
        length_guard_edge();

        return test_report((string_address) "bounded: ");
}
