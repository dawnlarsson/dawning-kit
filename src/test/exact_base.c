#include "../compiler_memory.c"
/*
        Every base that is known where the call is written.

        string_to_number, string_to_number_unsigned and positive_into_base
        expand in place when the base is a literal, and the line they expand
        to depends on the number: base ten has no letter path at all, sixteen
        accumulates by shift, thirty six divides by a constant. Nothing that
        takes its base from a variable can reach any of that, because the
        choice is made by the compiler, from the token. So every base is
        written out here, one call per base, and the answer is compared
        against the routine reached through a base the compiler cannot see.

        The parse is checked on a list of subjects that covers what the
        contract says and what it is easy to get wrong: leading space, both
        signs, a hexadecimal prefix with and without a digit after it, the two
        saturation edges from either side, a string that is nothing but a
        sign, and a digit that belongs to a larger base than the one asked
        for. Both the answer and the end pointer are compared, and the parse
        is run a third time with a null end pointer, because that is a
        separate expansion: the store is a branch on a pointer the compiler
        has folded away.

        positive_into_base writes into a buffer with guard bytes either side,
        and both halves are looked at: that the bytes asked for arrived, and
        that the bytes either side did not move. A base class that reaches one
        byte too far writes into a guard, and one that stops a byte short
        leaves the guard pattern showing inside the answer. The values are the
        neighbours of every carry the conversion has a case for, and then a
        long pseudorandom sweep shifted by every width, because a carry that
        only shows at one bit pattern is what a table of round numbers misses.
*/

#define GUARD 32
#define ROOM 72
#define SWEEP 20000

static p8 routine_arena[GUARD + ROOM + GUARD];
static p8 known_arena[GUARD + ROOM + GUARD];

#define ROUTINE_FIELD (routine_arena + GUARD)
#define KNOWN_FIELD (known_arena + GUARD)

static positive checks;
static positive failures;

//      The base the routine is handed. Volatile so that the macro above
//      cannot fold it and the general path is what answers.
static volatile b32 hidden;

//      The guard pattern differs at every index so a write that lands one
//      byte off is visible.
static p8 guard_byte(positive at) { return (p8)(0x5a ^ (at * 7)); }

static fn fail(const p8 address_to what, positive base, positive one,
               positive two)
{
        failures++;
        if (failures < 40)
                string_format(log, "FAIL %s base %p: routine %p known %p\n",
                              what, base, one, two);
}

//      Both arenas, guards laid and field wiped, before every conversion.
static fn prepare(void)
{
        for (positive at = 0; at < sizeof(routine_arena); at++)
        {
                routine_arena[at] = guard_byte(at);
                known_arena[at] = guard_byte(at);
        }
}

static fn guards_held(const p8 address_to what, positive base)
{
        for (positive at = 0; at < GUARD; at++)
        {
                checks++;
                if (known_arena[at] != guard_byte(at))
                        fail(what, base, at, known_arena[at]);
                checks++;
                if (known_arena[GUARD + ROOM + at] !=
                    guard_byte(GUARD + ROOM + at))
                        fail(what, base, GUARD + ROOM + at,
                             known_arena[GUARD + ROOM + at]);
        }
}

//      One conversion, both ways, with the length, every written byte and
//      both guards compared.
static fn agree_into(positive value, positive base, bool upper, positive known)
{
        positive was;

        hidden = (b32)base;
        was = positive_into_base(ROUTINE_FIELD, value, (positive)hidden, upper);

        checks++;
        if (was != known)
                fail((const p8 address_to)"into length", base, was, known);

        for (positive at = 0; at < ROOM; at++)
        {
                checks++;
                if (routine_arena[GUARD + at] != known_arena[GUARD + at])
                {
                        fail((const p8 address_to)"into byte", base,
                             routine_arena[GUARD + at],
                             known_arena[GUARD + at]);
                        break;
                }
        }

        guards_held((const p8 address_to)"into guard", base);
}

/*
        One base, written as a token, so the macro fires. The routine side is
        reached through the volatile above, which the macro cannot fold, so
        the two sides of the comparison are genuinely the two paths.
*/
#define INTO(base)                                                            \
        do                                                                    \
        {                                                                     \
                prepare();                                                    \
                agree_into(value, (base), upper,                              \
                           positive_into_base(KNOWN_FIELD, value, (base),      \
                                              upper));                        \
        } while (0)

static fn into_sweep(positive value, bool upper)
{
        INTO(2);  INTO(3);  INTO(4);  INTO(5);  INTO(6);  INTO(7);
        INTO(8);  INTO(9);  INTO(10); INTO(11); INTO(12); INTO(13);
        INTO(14); INTO(15); INTO(16); INTO(17); INTO(18); INTO(19);
        INTO(20); INTO(21); INTO(22); INTO(23); INTO(24); INTO(25);
        INTO(26); INTO(27); INTO(28); INTO(29); INTO(30); INTO(31);
        INTO(32); INTO(33); INTO(34); INTO(35); INTO(36);
}

/*
        The parse, on one subject, at one base written as a token: the signed
        answer and its end pointer, the unsigned answer and its end pointer,
        and the signed answer again with no end pointer at all, which is the
        second fold and a different line of code.
*/
#define PARSE(base)                                                           \
        do                                                                    \
        {                                                                     \
                string_address was_stop = null;                               \
                string_address now_stop = null;                               \
                positive was;                                                 \
                positive now;                                                 \
                                                                              \
                hidden = (base);                                              \
                was = (positive)string_to_number(subject, address_of was_stop, \
                                                 hidden);                     \
                now = (positive)string_to_number(subject,                     \
                                                 address_of now_stop, (base)); \
                checks++;                                                     \
                if (was != now)                                               \
                        fail((const p8 address_to)"signed", (base), was, now); \
                checks++;                                                     \
                if (was_stop != now_stop)                                     \
                        fail((const p8 address_to)"signed end", (base),        \
                             (positive)(was_stop - subject),                  \
                             (positive)(now_stop - subject));                 \
                                                                              \
                was_stop = null;                                              \
                now_stop = null;                                              \
                hidden = (base);                                              \
                was = string_to_number_unsigned(subject,                       \
                                                address_of was_stop, hidden);  \
                now = string_to_number_unsigned(subject,                       \
                                                address_of now_stop, (base));  \
                checks++;                                                     \
                if (was != now)                                               \
                        fail((const p8 address_to)"unsigned", (base), was,     \
                             now);                                            \
                checks++;                                                     \
                if (was_stop != now_stop)                                      \
                        fail((const p8 address_to)"unsigned end", (base),      \
                             (positive)(was_stop - subject),                  \
                             (positive)(now_stop - subject));                 \
                                                                              \
                hidden = (base);                                              \
                was = (positive)string_to_number(subject, null, hidden);       \
                now = (positive)string_to_number(subject, null, (base));       \
                checks++;                                                     \
                if (was != now)                                               \
                        fail((const p8 address_to)"no end pointer", (base),    \
                             was, now);                                        \
        } while (0)

static fn parse_sweep(string_address subject)
{
        PARSE(2);  PARSE(3);  PARSE(4);  PARSE(5);  PARSE(6);
        PARSE(7);  PARSE(8);  PARSE(9);  PARSE(10); PARSE(16);
}

//      Everything the two have to agree on, and the reason each one is here
//      is in the shape of it rather than in a comment per line.
static const p8 address_to subjects[] = {
        "", " ", "   \t\n\v\f\r 42", "0", "00", "000", "7", "42", "-42", "+42",
        "  -0007", "9", "10", "99", "100", "2147483647", "2147483648",
        "4294967295", "9223372036854775807", "9223372036854775808",
        "18446744073709551615", "18446744073709551616",
        "-9223372036854775808", "-9223372036854775809",
        "99999999999999999999999999", "-99999999999999999999999999",
        "-1", "-0", "+0", "abc", "-abc", "+", "-", " + 7", "12a34", "1_2",
        "0x", "0X", "0x1", "0xg", "0Xff", "0xFF", "0xffffffffffffffff",
        "0x10000000000000000", "-0x20", "0x0", "0x00", "  \t 0x", "\n-0x1f",
        " 0x7fffffffffffffff", "0xdeadBEEF", "007", "089", "0b101", "0o17",
        "z", "Z", "F", "f", "g", "G", "9999999999999999999999",
        "1111111111111111", "11111111111111111", "77777777777777777777777",
        "3735928559", "111111111111111111111111111111111111111111111111",
};

b32 main(void)
{
        moonwater_cpu_detect();

        for (positive which = 0;
             which < sizeof(subjects) / sizeof(subjects[0]); which++)
                parse_sweep((string_address)subjects[which]);

        //      Every value the conversion has a case for, and the neighbours
        //      of every carry it could get wrong.
        static const positive values[] = {
                0, 1, 2, 3, 7, 8, 9, 10, 15, 16, 31, 32, 35, 36, 63, 64, 99,
                100, 255, 256, 999, 1000, 1023, 1024, 9999, 10000, 65535,
                65536, 99999999, 100000000, 4294967295UL, 4294967296UL,
                9223372036854775807UL, 9223372036854775808UL,
                18446744073709551615UL, 1844674407370955161UL,
                1844674407370955162UL,
        };

        for (positive which = 0;
             which < sizeof(values) / sizeof(values[0]); which++)
        {
                into_sweep(values[which], 0);
                into_sweep(values[which], 1);
        }

        positive seed = 0x243f6a8885a308d3UL;

        for (positive which = 0; which < SWEEP; which++)
        {
                seed = seed * 6364136223846793005UL + 1442695040888963407UL;
                into_sweep(seed >> (which & 63), (bool)(which & 1));
        }

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
