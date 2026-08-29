#include "../compiler_memory.c"
/*
        Every value that is known where the call is written.

        The single-value routines take a byte class or a word and answer from
        it directly, so what a literal folds here is not a shorter line of
        code but the whole call: byte_is_digit('7') is the token 1 and there
        is nothing left to run. Nothing that takes its value from a loop
        counter reaches any of that, because the choice is made by the
        compiler, from the token, so every value is written out here.

        There is no buffer and nothing is written, so exact.c's guard bytes
        have no counterpart. What takes their place is the range: an
        expansion that agrees with the routine on the printable bytes and
        disagrees on EOF, or on a byte plus two hundred and fifty six, or on
        the two ends of an int, is the failure this is looking for, and each
        of those is a literal here beside the two hundred and fifty six that
        a caller would actually write.

        The routine is called through a parenthesised name, which is how a
        function-like macro is stepped around: (byte_is_digit)(v) is the call
        and byte_is_digit(v) is whatever the macro makes of it, so both sides
        of every comparison below are the thing they claim to be.
*/

positive failures = 0;

void report(string_address name, bipolar value, b32 expanded, b32 routine)
{
        failures++;
        string_format(log, "%s(%b) expanded %b routine %b\n",
                      name, value, expanded, routine);
        log_flush();
}

#define CHECK(name, known, V)                                                \
        do {                                                                 \
                b32 expanded = known(V);                                     \
                b32 routine = (name)(V);                                     \
                if (expanded != routine)                                     \
                        report(#name, (bipolar)(V), expanded, routine);      \
        } while (0)

/*      Eight literals at a time, because a value built from a literal and a
        literal is still a literal and still folds; a loop counter would not. */
#define EIGHT(name, known, V)                                                \
        CHECK(name, known, (V) + 0); CHECK(name, known, (V) + 1);            \
        CHECK(name, known, (V) + 2); CHECK(name, known, (V) + 3);            \
        CHECK(name, known, (V) + 4); CHECK(name, known, (V) + 5);            \
        CHECK(name, known, (V) + 6); CHECK(name, known, (V) + 7)

#define SIXTY_FOUR(name, known, V)                                           \
        EIGHT(name, known, (V) +  0); EIGHT(name, known, (V) +  8);          \
        EIGHT(name, known, (V) + 16); EIGHT(name, known, (V) + 24);          \
        EIGHT(name, known, (V) + 32); EIGHT(name, known, (V) + 40);          \
        EIGHT(name, known, (V) + 48); EIGHT(name, known, (V) + 56)

/*      Every byte, then the values a caller reaches by accident: EOF, a byte
        that arrived with the high bits still on it, and the two ends. */
#define EVERY_VALUE(name, known)                                             \
        do {                                                                 \
                SIXTY_FOUR(name, known,   0); SIXTY_FOUR(name, known,  64);  \
                SIXTY_FOUR(name, known, 128); SIXTY_FOUR(name, known, 192);  \
                CHECK(name, known, -1);   CHECK(name, known, -2);            \
                CHECK(name, known, -48);  CHECK(name, known, -97);           \
                CHECK(name, known, 256);  CHECK(name, known, 304);           \
                CHECK(name, known, 321);  CHECK(name, known, 511);           \
                CHECK(name, known, 65535); CHECK(name, known, -65536);       \
                CHECK(name, known, 2147483647);                              \
                CHECK(name, known, -2147483647 - 1);                         \
        } while (0)

/*      One set bit at a time from the bottom of the word to the top, which is
        where a count that answered for a thirty two bit register rather than
        the sixty four bit one it was handed shows itself. */
#define ONE_BIT(name, known, S)                                              \
        CHECK(name, known, 1ull << ((S) + 0)); CHECK(name, known, 1ull << ((S) + 1)); \
        CHECK(name, known, 1ull << ((S) + 2)); CHECK(name, known, 1ull << ((S) + 3)); \
        CHECK(name, known, 1ull << ((S) + 4)); CHECK(name, known, 1ull << ((S) + 5)); \
        CHECK(name, known, 1ull << ((S) + 6)); CHECK(name, known, 1ull << ((S) + 7))

#define EVERY_WORD(name, known)                                              \
        do {                                                                 \
                ONE_BIT(name, known,  0); ONE_BIT(name, known,  8);          \
                ONE_BIT(name, known, 16); ONE_BIT(name, known, 24);          \
                ONE_BIT(name, known, 32); ONE_BIT(name, known, 40);          \
                ONE_BIT(name, known, 48); ONE_BIT(name, known, 56);          \
                CHECK(name, known, 0ull);                                    \
                CHECK(name, known, 1ull);                                    \
                CHECK(name, known, 3ull);                                    \
                CHECK(name, known, 0xffull);                                 \
                CHECK(name, known, 0x5555555555555555ull);                   \
                CHECK(name, known, 0xaaaaaaaaaaaaaaaaull);                   \
                CHECK(name, known, 0xdeadbeefull);                           \
                CHECK(name, known, 0x8000000000000000ull);                   \
                CHECK(name, known, 0xffffffffffffffffull);                   \
                CHECK(name, known, 0x7fffffffffffffffull);                   \
                CHECK(name, known, 0x100000000ull);                          \
                CHECK(name, known, 0xffffffffull);                           \
        } while (0)

/*      The narrow one takes an int, so a set bit above thirty one arrives
        sign extended and the answer is still the position of the lowest. */
#define EVERY_WHOLE(name, known)                                             \
        do {                                                                 \
                ONE_BIT(name, known,  0); ONE_BIT(name, known,  8);          \
                ONE_BIT(name, known, 16); ONE_BIT(name, known, 23);          \
                CHECK(name, known, 0);                                       \
                CHECK(name, known, -1);                                      \
                CHECK(name, known, -2);                                      \
                CHECK(name, known, 0x7fffffff);                              \
                CHECK(name, known, -2147483647 - 1);                         \
                CHECK(name, known, 0x5555);                                  \
        } while (0)

/*
        And the macro itself, which is the thing that actually fires at a call
        site. Everything above compares the expansion against the routine by
        naming both; this compares what the preprocessor makes of an ordinary
        call against the routine, so that a macro that expanded the wrong way
        round, or evaluated its argument twice, or stopped a caller taking the
        routine's address, is caught here rather than in somebody's program.
*/
b32 side_effects = 0;
b32 bump(void) { side_effects++; return 'q'; }
b32 (*taken_address)(b32) = byte_is_digit;
volatile b32 not_folded = '7';

void check_macro(void)
{
        if (byte_is_digit('7') != 1)          report("macro byte_is_digit", '7', byte_is_digit('7'), 1);
        if (byte_is_punctuation('!') != 1)    report("macro byte_is_punctuation", '!', byte_is_punctuation('!'), 1);
        if (byte_to_lower('A') != 'a')        report("macro byte_to_lower", 'A', byte_to_lower('A'), 'a');
        if (bits_leading_zeros(1ull) != 63)   report("macro bits_leading_zeros", 1, bits_leading_zeros(1ull), 63);
        if (bits_trailing_zeros(0ull) != 64)  report("macro bits_trailing_zeros", 0, bits_trailing_zeros(0ull), 64);
        if (bits_first_set(0) != 0)           report("macro bits_first_set", 0, bits_first_set(0), 0);
        if (bits_counted(0ull) != 0)          report("macro bits_counted", 0, bits_counted(0ull), 0);

        //      A value the compiler cannot know takes the ordinary call.
        if (byte_is_digit(not_folded) != 1)
                report("macro unfolded", not_folded, byte_is_digit(not_folded), 1);

        //      The argument is evaluated exactly once.
        byte_is_alpha(bump());
        if (side_effects != 1)
                report("macro evaluations", 0, side_effects, 1);

        //      A bare name is not a call, so the macro leaves it alone.
        if (taken_address('3') != 1)
                report("macro address", '3', taken_address('3'), 1);
}

b32 main(void)
{
        check_macro();

        EVERY_VALUE(byte_is_digit,       known_is_digit);
        EVERY_VALUE(byte_is_upper,       known_is_upper);
        EVERY_VALUE(byte_is_lower,       known_is_lower);
        EVERY_VALUE(byte_is_alpha,       known_is_alpha);
        EVERY_VALUE(byte_is_alnum,       known_is_alnum);
        EVERY_VALUE(byte_is_space,       known_is_space);
        EVERY_VALUE(byte_is_hexadecimal, known_is_hexadecimal);
        EVERY_VALUE(byte_is_printable,   known_is_printable);
        EVERY_VALUE(byte_is_graphic,     known_is_graphic);
        EVERY_VALUE(byte_is_control,     known_is_control);
        EVERY_VALUE(byte_is_punctuation, known_is_punctuation);
        EVERY_VALUE(byte_is_blank,       known_is_blank);
        EVERY_VALUE(byte_is_ascii,       known_is_ascii);
        EVERY_VALUE(byte_to_ascii,       known_to_ascii);
        EVERY_VALUE(byte_to_upper,       known_to_upper);
        EVERY_VALUE(byte_to_lower,       known_to_lower);

        EVERY_WORD(bits_counted,         known_counted);
        EVERY_WORD(bits_trailing_zeros,  known_trailing_zeros);
        EVERY_WORD(bits_leading_zeros,   known_leading_zeros);
        EVERY_WORD(bits_first_set_wide,  known_first_set_wide);
        EVERY_WHOLE(bits_first_set,      known_first_set);

        if (failures == 0)
                string_format(log, "single: every literal agrees with its routine\n");
        else
                string_format(log, "single: %p disagreements\n", failures);
        log_flush();
        return failures != 0;
}
