#include "../compiler_memory.c"
/*
        Every set that is known where the call is written.

        string_span_of_set, string_span_without_set and string_first_of_set
        expand in place when the members are a literal, and which line they
        expand to depends on the literal: no members at all is the answer
        without a scan, one member is a byte hunt, and anything more is a
        table folded into read only memory. Nothing that takes its members
        from a pointer can reach any of that, because the choice is made by
        the compiler, from the token. So every shape of set is written out
        here.

        Each check is run against two answers that were arrived at
        differently: the routine, reached by parenthesising its name so the
        macro does not fire, and a reference walk that carries no cleverness
        at all. The expansion has to agree with both.

        A scan that reads past the terminator is caught by running every
        source twice over. The bytes after the terminator are members the
        first time and strangers the second, and the terminator ends the run
        in all three routines, so the two runs must give the same answer. One
        that reads on gets a longer run from one layout than from the other.

        Guard bytes either side of the room say that nothing wrote, which none
        of these three is allowed to do.
*/

#define GUARD 32
#define ROOM 384
#define SHAPES 7

static p8 arena[GUARD + ROOM + GUARD];

#define FIELD (arena + GUARD)

static positive checks;
static positive failures;

//      The pattern the guards carry. It differs at every index so a store
//      that lands one byte off is visible.
static p8 guard_byte(positive i) { return (p8)(0x5a ^ (i * 7)); }

static void prepare(void)
{
        for (positive i = 0; i < sizeof(arena); i++) arena[i] = guard_byte(i);
}

static bool guards_intact(void)
{
        for (positive i = 0; i < GUARD; i++)
                if (arena[i] != guard_byte(i)) return false;

        for (positive i = GUARD + ROOM; i < sizeof(arena); i++)
                if (arena[i] != guard_byte(i)) return false;

        return true;
}

//      The three answers, worked out the long way. A member is looked for by
//      walking the whole set for every byte, which is the definition and
//      nothing else.
static bool member_of(string_address set, p8 value)
{
        if (value == end) return false;

        for (positive i = 0; set[i]; i++)
                if (set[i] == value) return true;

        return false;
}

static positive reference_span_of_set(string_address source, string_address set)
{
        positive length = 0;
        while (source[length] && member_of(set, source[length])) length++;
        return length;
}

static positive reference_span_without_set(string_address source, string_address set)
{
        positive length = 0;
        while (source[length] && !member_of(set, source[length])) length++;
        return length;
}

static string_address reference_first_of_set(string_address source, string_address set)
{
        for (positive at = 0; source[at]; at++)
                if (member_of(set, source[at])) return source + at;

        return (string_address)null;
}

//      A byte the set holds and a byte it does not, so that a source can be
//      built out of both. The set with every byte in it has no stranger but
//      the terminator, which is what the fallback answers.
static p8 a_member(string_address set) { return set[0] ? set[0] : (p8)'m'; }

static p8 a_stranger(string_address set)
{
        for (positive value = 1; value < 256; value++)
                if (!member_of(set, (p8)value)) return (p8)value;

        return (p8)end;
}

//      Seven shapes, so that the run stops at the front, at the back, in the
//      middle, and never; and two of them walk every byte value there is.
static p8 shaped(string_address set, positive shape, positive at, positive length)
{
        p8 member = a_member(set);
        p8 stranger = a_stranger(set);

        switch (shape)
        {
        case 0: return member;
        case 1: return stranger;
        case 2: return (at & 1) ? stranger : member;
        case 3: return at + 1 == length ? member : stranger;
        case 4: return at + 1 == length ? stranger : member;
        case 5: return (p8)(at % 255 + 1);
        default: return (p8)(255 - at % 255);
        }
}

//      The source, and behind its terminator either members or strangers.
static string_address lay(string_address set, positive shape, positive length,
                          bool members_behind)
{
        p8 behind = members_behind ? a_member(set) : a_stranger(set);

        prepare();

        for (positive i = 0; i < length; i++)
                FIELD[i] = shaped(set, shape, i, length);

        FIELD[length] = end;

        for (positive i = length + 1; i < ROOM; i++) FIELD[i] = behind;

        return FIELD;
}

static void fail(string_address what, positive shape, positive length)
{
        failures++;
        string_format(log, "FAIL %s shape %p length %p\n", what, shape, length);
}

//      The three answers the expansion gave, against the routine and against
//      the reference. Both, because the routine and the expansion could agree
//      on the same misreading of what the set means.
static void judge(string_address set, string_address source, positive shape,
                  positive length, positive span, positive without,
                  string_address first)
{
        checks += 3;

        if (span != (string_span_of_set)(source, set) ||
            span != reference_span_of_set(source, set))
                fail((string_address)"span_of_set", shape, length);

        if (without != (string_span_without_set)(source, set) ||
            without != reference_span_without_set(source, set))
                fail((string_address)"span_without_set", shape, length);

        if (first != (string_first_of_set)(source, set) ||
            first != reference_first_of_set(source, set))
                fail((string_address)"first_of_set", shape, length);

        if (!guards_intact()) fail((string_address)"guards", shape, length);
}

//      Lengths that matter: every one up to past a block, then the sizes at
//      which a byte hunt changes width.
static positive lengths[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
        37, 38, 39, 40, 47, 48, 49, 63, 64, 65, 95, 96, 97, 127, 128, 129,
        191, 192, 193, 255, 256, 257, 300, 350
};

#define LENGTHS (sizeof(lengths) / sizeof(lengths[0]))

//      The literal has to be written into every call, because a set held in a
//      variable is a set the compiler has not been told.
#define CHECK_SET(literal)                                                    \
        do                                                                    \
        {                                                                     \
                string_address set = (string_address)(literal);               \
                for (positive shape = 0; shape < SHAPES; shape++)             \
                        for (positive behind = 0; behind < 2; behind++)       \
                                for (positive n = 0; n < LENGTHS; n++)        \
                                {                                             \
                                        positive length = lengths[n];         \
                                        string_address at =                   \
                                                lay(set, shape, length, behind); \
                                        judge(set, at, shape, length,         \
                                              string_span_of_set(at, literal), \
                                              string_span_without_set(at, literal), \
                                              string_first_of_set(at, literal)); \
                                }                                             \
        } while (0)

//      A set the compiler cannot see, so that the other arm of every macro is
//      compiled and run too. volatile is what stops the pointer folding.
static string_address volatile hidden = (string_address)"\n";

static void unknown_sets(void)
{
        static string_address sets[] = {
                (string_address)"", (string_address)"a", (string_address)" \t\n",
                (string_address)"\x01\x40\x80\xff",
        };

        for (positive s = 0; s < sizeof(sets) / sizeof(sets[0]); s++)
        {
                hidden = sets[s];

                for (positive shape = 0; shape < SHAPES; shape++)
                        for (positive n = 0; n < LENGTHS; n++)
                        {
                                string_address set = hidden;
                                positive length = lengths[n];
                                string_address at = lay(set, shape, length, 0);

                                judge(set, at, shape, length,
                                      string_span_of_set(at, hidden),
                                      string_span_without_set(at, hidden),
                                      string_first_of_set(at, hidden));
                        }
        }
}

//      A set the compiler can read that is not written at the call site: a
//      named const array, a slice of one, and an array something has stored
//      into, which is the one that must go to the routine instead of folding
//      to a table of whatever the front end could not see.
static const p8 named_set[] = " \t\n";

static void named_sets(void)
{
        for (positive shape = 0; shape < SHAPES; shape++)
                for (positive n = 0; n < LENGTHS; n++)
                {
                        positive length = lengths[n];
                        string_address at = lay(named_set, shape, length, 0);

                        judge(named_set, at, shape, length,
                              string_span_of_set(at, named_set),
                              string_span_without_set(at, named_set),
                              string_first_of_set(at, named_set));

                        at = lay(named_set + 1, shape, length, 0);

                        judge(named_set + 1, at, shape, length,
                              string_span_of_set(at, named_set + 1),
                              string_span_without_set(at, named_set + 1),
                              string_first_of_set(at, named_set + 1));
                }

        for (positive shape = 0; shape < SHAPES; shape++)
                for (positive n = 0; n < LENGTHS; n++)
                {
                        p8 written[4];
                        positive length = lengths[n];

                        written[0] = (p8)' ';
                        written[1] = (p8)'\t';
                        written[2] = (p8)hidden[0];
                        written[3] = end;

                        string_address at = lay(written, shape, length, 0);

                        judge(written, at, shape, length,
                              string_span_of_set(at, written),
                              string_span_without_set(at, written),
                              string_first_of_set(at, written));
                }
}

b32 main(void)
{
        //      No members at all, which answers without scanning.
        CHECK_SET("");

        //      One member, which is a byte hunt and no table: the lowest, the
        //      highest, either side of the word boundary the routine's bitmap
        //      has at sixty four, and the one at a hundred and twenty eight
        //      where a signed byte would have gone negative.
        CHECK_SET("a");
        CHECK_SET("\x01");
        CHECK_SET("\x3f");
        CHECK_SET("\x40");
        CHECK_SET("\x41");
        CHECK_SET("\x7f");
        CHECK_SET("\x80");
        CHECK_SET("\xff");

        //      Two and three, which is what a lexer writes down.
        CHECK_SET("=;");
        CHECK_SET(" \t");
        CHECK_SET(" \t\n");
        CHECK_SET(" \t\n\r");

        //      The same member twice, which must not be two members.
        CHECK_SET("aa");
        CHECK_SET("\xff\xff\xff");

        //      A terminator inside the literal ends the set: the members here
        //      are the letter a and nothing else.
        CHECK_SET("a\0b");

        //      Across every word of the bitmap the routine builds, and across
        //      the seam where a byte stops fitting in a signed char.
        CHECK_SET("\x01\x40\x80\xff");
        CHECK_SET("\x7f\x80");
        CHECK_SET("\x3f\x40\x41");

        //      Eight, sixteen and thirty two members.
        CHECK_SET(" \t\n\r;=()");
        CHECK_SET(" \t\n\r;=()[]{}<>|&");
        CHECK_SET("0123456789abcdefABCDEF !\"#$%&'()*+,-");

        //      Every byte value there is except the terminator, which cannot
        //      be a member however the set was written.
        CHECK_SET("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
                  "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
                  "\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f"
                  "\x30\x31\x32\x33\x34\x35\x36\x37\x38\x39\x3a\x3b\x3c\x3d\x3e\x3f"
                  "\x40\x41\x42\x43\x44\x45\x46\x47\x48\x49\x4a\x4b\x4c\x4d\x4e\x4f"
                  "\x50\x51\x52\x53\x54\x55\x56\x57\x58\x59\x5a\x5b\x5c\x5d\x5e\x5f"
                  "\x60\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6a\x6b\x6c\x6d\x6e\x6f"
                  "\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7a\x7b\x7c\x7d\x7e\x7f"
                  "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f"
                  "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f"
                  "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"
                  "\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf"
                  "\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf"
                  "\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf"
                  "\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef"
                  "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff");

        unknown_sets();
        named_sets();

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
