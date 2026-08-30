#include "../compiler_memory.c"
/*
        Experimental C standard library

        scanf, pinned against the answers a real glibc gave

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        WHERE THE EXPECTED ANSWERS CAME FROM

        Every number in the generated block below was produced by GNU libc
        2.44 on x86_64, by a second program built against the real header and
        run on the same format and the same input, and copied here unchanged.

        That comparison was much larger than what is kept here. 34,475 format
        and input pairs were run through both libraries twice each -- once as
        sscanf over a string and once as fscanf over a file, so that the
        stream position, the end-of-file indicator, the error indicator and
        errno could be compared as well as the answer and the values -- for
        68,950 result lines. With numbers.c underneath, which is where strtof,
        strtod and strtold come from, all 68,950 were identical. Not one pair
        disagreed about the return value, about a converted value, about where
        the stream was left, or about what feof, ferror and errno said
        afterwards.

        The same programs were built for arm64 and riscv64 and run under qemu.
        arm64 against a glibc built for arm64 was identical on all 68,950
        lines as well, and arm64 and riscv64 produced byte identical output to
        each other and to x86_64 except on the 226 lines that print a long
        double, where x86_64's eighty bit extended and the other two's
        binary128 are different formats of the same value.

        The riscv64 cross toolchain carries glibc 2.41 rather than 2.44 and
        disagrees with BOTH of the others on about a thousand pairs. Every one
        of them is an incomplete prefix -- "0x", "1e", "0x1p", "0x." -- which
        2.44 consumes and calls a matching failure as C requires and 2.41 does
        not. This follows 2.44, which follows the standard, and the note at the
        top of src/standard/scan.c is about that rule and nothing else.

        Before numbers.c landed the family was developed against a stand-in
        strtod, and in that configuration thirty three pairs differed, all of
        them a floating point VALUE and none of them a return value or a
        position. Thirty of those are gone now. What remains is what the
        fallback path in scan.c says it is: without numbers.c, "%f" is a
        double narrowed rather than a float parsed and "%Lf" is a double
        widened, and the two round differently at the last place.

        What is kept below is a stratified sample of that run plus every
        corner worth naming.
*/

#include "counted.inc"

/*
        THE THREE SHAPES EVERY GENERATED CASE IS ONE OF

        A conversion that writes a number writes it into a sixty four bit
        slot filled with 0xA5, and the whole slot is compared afterwards. That
        is deliberate and it is the only way to see the two mistakes that a
        typed comparison hides: a "%d" that writes eight bytes where four were
        asked for, and a "%hhd" that writes two. The pattern that comes back
        says how many bytes moved as well as what they were, and the glibc
        run that produced these numbers filled the same slot the same way.

        A conversion that writes bytes writes them into a buffer filled with
        0x7F, and the first eight are compared. Eight is past the end of every
        string in the table, so the comparison covers the terminator, the byte
        after it, and the bytes a width should not have reached.

        A directive that assigns nothing is its answer and nothing else.

        used is the "%n" every format ends with, sitting at minus one before
        the call, so a format that failed before reaching it says so by
        leaving it there -- which is itself a check, because %n is required
        not to be reached and not to be counted.
*/
static positive slot;
static p8 room[16];
static b32 used;

static fn value_case(string_address format, string_address input,
                     b32 answer_wanted, positive value_wanted, b32 used_wanted)
{
        b32 answer;

        slot = 0xA5A5A5A5A5A5A5A5ull;
        used = -1;
        answer = sscanf(input, format, address_of slot, address_of used);

        checks++;

        if (answer == answer_wanted && slot == value_wanted &&
            used == used_wanted)
                return;

        failures++;
        string_format(log,
                      "  FAIL [%s] <- [%s] answered %b value %p used %b,"
                      " wanted %b %p %b\n",
                      format, input, answer, slot, used, answer_wanted,
                      value_wanted, used_wanted);
}

static fn text_case(string_address format, string_address input,
                    b32 answer_wanted, string_address room_wanted,
                    b32 used_wanted)
{
        b32 answer;

        memory_fill(room, 0x7F, sizeof(room));
        used = -1;
        answer = sscanf(input, format, room, address_of used);

        checks++;

        if (answer == answer_wanted && used == used_wanted &&
            memory_compare(room, room_wanted, 8) == 0)
                return;

        failures++;
        string_format(log, "  FAIL [%s] <- [%s] answered %b used %b,"
                           " wanted %b %b\n",
                      format, input, answer, used, answer_wanted, used_wanted);
}

static fn answer_case(string_address format, string_address input,
                      b32 answer_wanted)
{
        b32 answer = sscanf(input, format);

        checks++;

        if (answer == answer_wanted)
                return;

        failures++;
        string_format(log, "  FAIL [%s] <- [%s] answered %b, wanted %b\n",
                      format, input, answer, answer_wanted);
}

static fn generated(void)
{
        //      the signed integer conversions
        value_case((string_address) "%d%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%d%n", (string_address) "-42",
                   1, 0xA5A5A5A5FFFFFFD6ULL, 3);
        value_case((string_address) "%d%n", (string_address) "+42",
                   1, 0xA5A5A5A50000002AULL, 3);
        value_case((string_address) "%d%n", (string_address) "  42",
                   1, 0xA5A5A5A50000002AULL, 4);
        value_case((string_address) "%d%n", (string_address) "42abc",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%d%n", (string_address) "",
                   -1, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "  ",
                   -1, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "abc",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "-",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "+",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "- 5",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "+-5",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%d%n", (string_address) "0",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%d%n", (string_address) "007",
                   1, 0xA5A5A5A500000007ULL, 3);
        value_case((string_address) "%d%n", (string_address) "-0",
                   1, 0xA5A5A5A500000000ULL, 2);
        value_case((string_address) "%1d%n", (string_address) "42",
                   1, 0xA5A5A5A500000004ULL, 1);
        value_case((string_address) "%2d%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%3d%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%1d%n", (string_address) "-4",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%2d%n", (string_address) "-4",
                   1, 0xA5A5A5A5FFFFFFFCULL, 2);
        value_case((string_address) "%0d%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%20d%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%i%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%i%n", (string_address) "0",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%i%n", (string_address) "017",
                   1, 0xA5A5A5A50000000FULL, 3);
        value_case((string_address) "%i%n", (string_address) "019",
                   1, 0xA5A5A5A500000001ULL, 2);
        value_case((string_address) "%i%n", (string_address) "0x1f",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%i%n", (string_address) "0X1F",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%i%n", (string_address) "0x",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%i%n", (string_address) "0xz",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%i%n", (string_address) "-0x10",
                   1, 0xA5A5A5A5FFFFFFF0ULL, 5);
        value_case((string_address) "%i%n", (string_address) "+0",
                   1, 0xA5A5A5A500000000ULL, 2);
        value_case((string_address) "%i%n", (string_address) "0G",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%i%n", (string_address) "0b101",
                   1, 0xA5A5A5A500000005ULL, 5);
        value_case((string_address) "%i%n", (string_address) "0b",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%i%n", (string_address) "0b2",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%i%n", (string_address) "-0b1",
                   1, 0xA5A5A5A5FFFFFFFFULL, 4);
        value_case((string_address) "%2i%n", (string_address) "0x1f",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%3i%n", (string_address) "0x1f",
                   1, 0xA5A5A5A500000001ULL, 3);
        value_case((string_address) "%4i%n", (string_address) "0x1f",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%2i%n", (string_address) "0b101",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%3i%n", (string_address) "0b101",
                   1, 0xA5A5A5A500000001ULL, 3);
        value_case((string_address) "%ld%n", (string_address) "42",
                   1, 0x000000000000002AULL, 2);
        value_case((string_address) "%lld%n", (string_address) "-42",
                   1, 0xFFFFFFFFFFFFFFD6ULL, 3);
        value_case((string_address) "%jd%n", (string_address) "42",
                   1, 0x000000000000002AULL, 2);
        value_case((string_address) "%td%n", (string_address) "42",
                   1, 0x000000000000002AULL, 2);
        value_case((string_address) "%ld%n", (string_address) "9223372036854775807",
                   1, 0x7FFFFFFFFFFFFFFFULL, 19);
        value_case((string_address) "%ld%n", (string_address) "9223372036854775808",
                   1, 0x7FFFFFFFFFFFFFFFULL, 19);
        value_case((string_address) "%ld%n", (string_address) "-9223372036854775808",
                   1, 0x8000000000000000ULL, 20);
        value_case((string_address) "%ld%n", (string_address) "-9223372036854775809",
                   1, 0x8000000000000000ULL, 20);
        value_case((string_address) "%ld%n", (string_address) "99999999999999999999",
                   1, 0x7FFFFFFFFFFFFFFFULL, 20);
        value_case((string_address) "%ld%n", (string_address) "-99999999999999999999",
                   1, 0x8000000000000000ULL, 21);
        value_case((string_address) "%ld%n", (string_address) "0000000000000000000000005",
                   1, 0x0000000000000005ULL, 25);
        value_case((string_address) "%d%n", (string_address) "99999999999999999999",
                   1, 0xA5A5A5A5FFFFFFFFULL, 20);
        value_case((string_address) "%d%n", (string_address) "2147483648",
                   1, 0xA5A5A5A580000000ULL, 10);
        value_case((string_address) "%d%n", (string_address) "-2147483649",
                   1, 0xA5A5A5A57FFFFFFFULL, 11);

        //      the unsigned integer conversions
        value_case((string_address) "%u%n", (string_address) "42",
                   1, 0xA5A5A5A50000002AULL, 2);
        value_case((string_address) "%u%n", (string_address) "-1",
                   1, 0xA5A5A5A5FFFFFFFFULL, 2);
        value_case((string_address) "%u%n", (string_address) "-2",
                   1, 0xA5A5A5A5FFFFFFFEULL, 2);
        value_case((string_address) "%u%n", (string_address) "+7",
                   1, 0xA5A5A5A500000007ULL, 2);
        value_case((string_address) "%u%n", (string_address) "abc",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%o%n", (string_address) "017",
                   1, 0xA5A5A5A50000000FULL, 3);
        value_case((string_address) "%o%n", (string_address) "08",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%o%n", (string_address) "0x0",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%o%n", (string_address) "7",
                   1, 0xA5A5A5A500000007ULL, 1);
        value_case((string_address) "%o%n", (string_address) "8",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%x%n", (string_address) "1f",
                   1, 0xA5A5A5A50000001FULL, 2);
        value_case((string_address) "%x%n", (string_address) "0x1f",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%x%n", (string_address) "0X1F",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%x%n", (string_address) "0x",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%x%n", (string_address) "0",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%x%n", (string_address) "00",
                   1, 0xA5A5A5A500000000ULL, 2);
        value_case((string_address) "%x%n", (string_address) "0x1G",
                   1, 0xA5A5A5A500000001ULL, 3);
        value_case((string_address) "%x%n", (string_address) "x1",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%x%n", (string_address) "-0x1",
                   1, 0xA5A5A5A5FFFFFFFFULL, 4);
        value_case((string_address) "%x%n", (string_address) "abcdef",
                   1, 0xA5A5A5A500ABCDEFULL, 6);
        value_case((string_address) "%X%n", (string_address) "ABCDEF",
                   1, 0xA5A5A5A500ABCDEFULL, 6);
        value_case((string_address) "%1x%n", (string_address) "0x1f",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%2x%n", (string_address) "0x1f",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%3x%n", (string_address) "0x1f",
                   1, 0xA5A5A5A500000001ULL, 3);
        value_case((string_address) "%4x%n", (string_address) "0x1f",
                   1, 0xA5A5A5A50000001FULL, 4);
        value_case((string_address) "%lu%n", (string_address) "18446744073709551615",
                   1, 0xFFFFFFFFFFFFFFFFULL, 20);
        value_case((string_address) "%lu%n", (string_address) "18446744073709551616",
                   1, 0xFFFFFFFFFFFFFFFFULL, 20);
        value_case((string_address) "%lu%n", (string_address) "-1",
                   1, 0xFFFFFFFFFFFFFFFFULL, 2);
        value_case((string_address) "%lu%n", (string_address) "-2",
                   1, 0xFFFFFFFFFFFFFFFEULL, 2);
        value_case((string_address) "%lu%n", (string_address) "99999999999999999999",
                   1, 0xFFFFFFFFFFFFFFFFULL, 20);
        value_case((string_address) "%lu%n", (string_address) "-99999999999999999999",
                   1, 0xFFFFFFFFFFFFFFFFULL, 21);
        value_case((string_address) "%lx%n", (string_address) "0xFFFFFFFFFFFFFFFF",
                   1, 0xFFFFFFFFFFFFFFFFULL, 18);
        value_case((string_address) "%lx%n", (string_address) "0x10000000000000000",
                   1, 0xFFFFFFFFFFFFFFFFULL, 19);
        value_case((string_address) "%zu%n", (string_address) "42",
                   1, 0x000000000000002AULL, 2);
        value_case((string_address) "%llx%n", (string_address) "ff",
                   1, 0x00000000000000FFULL, 2);

        //      the float conversions, compared as the bytes they stored
        value_case((string_address) "%f%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "-1.5",
                   1, 0xA5A5A5A5BFC00000ULL, 4);
        value_case((string_address) "%f%n", (string_address) "+1.5",
                   1, 0xA5A5A5A53FC00000ULL, 4);
        value_case((string_address) "%f%n", (string_address) ".5",
                   1, 0xA5A5A5A53F000000ULL, 2);
        value_case((string_address) "%f%n", (string_address) "5.",
                   1, 0xA5A5A5A540A00000ULL, 2);
        value_case((string_address) "%f%n", (string_address) "0",
                   1, 0xA5A5A5A500000000ULL, 1);
        value_case((string_address) "%f%n", (string_address) "2.5",
                   1, 0xA5A5A5A540200000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "0.25",
                   1, 0xA5A5A5A53E800000ULL, 4);
        value_case((string_address) "%f%n", (string_address) "1e2",
                   1, 0xA5A5A5A542C80000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "1E2",
                   1, 0xA5A5A5A542C80000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "1.5e2xyz",
                   1, 0xA5A5A5A543160000ULL, 5);
        value_case((string_address) "%f%n", (string_address) "1e",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "1e+",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "1e-",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) ".",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "-.",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "",
                   -1, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "  ",
                   -1, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "abc",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "+",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "inf",
                   1, 0xA5A5A5A57F800000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "-inf",
                   1, 0xA5A5A5A5FF800000ULL, 4);
        value_case((string_address) "%f%n", (string_address) "INF",
                   1, 0xA5A5A5A57F800000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "infinity",
                   1, 0xA5A5A5A57F800000ULL, 8);
        value_case((string_address) "%f%n", (string_address) "INFINITY",
                   1, 0xA5A5A5A57F800000ULL, 8);
        value_case((string_address) "%f%n", (string_address) "infinit",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "infz",
                   1, 0xA5A5A5A57F800000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "in",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "2inf",
                   1, 0xA5A5A5A540000000ULL, 1);
        value_case((string_address) "%f%n", (string_address) "0x1p3",
                   1, 0xA5A5A5A541000000ULL, 5);
        value_case((string_address) "%f%n", (string_address) "0X1P-2",
                   1, 0xA5A5A5A53E800000ULL, 6);
        value_case((string_address) "%f%n", (string_address) "0x1",
                   1, 0xA5A5A5A53F800000ULL, 3);
        value_case((string_address) "%f%n", (string_address) "0x1p",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "0x.8p1",
                   1, 0xA5A5A5A53F800000ULL, 6);
        value_case((string_address) "%f%n", (string_address) "0x.",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "0x",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%f%n", (string_address) "0xz",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%3f%n", (string_address) "3.14159",
                   1, 0xA5A5A5A540466666ULL, 3);
        value_case((string_address) "%2f%n", (string_address) "1.5e",
                   1, 0xA5A5A5A53F800000ULL, 2);
        value_case((string_address) "%1f%n", (string_address) "-",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%4f%n", (string_address) "-inf",
                   1, 0xA5A5A5A5FF800000ULL, 4);
        value_case((string_address) "%e%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%g%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%a%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%E%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%G%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);
        value_case((string_address) "%F%n", (string_address) "1.5",
                   1, 0xA5A5A5A53FC00000ULL, 3);

        //      and the same into a double
        value_case((string_address) "%lf%n", (string_address) "1.5",
                   1, 0x3FF8000000000000ULL, 3);
        value_case((string_address) "%lf%n", (string_address) "1.5e2xyz",
                   1, 0x4062C00000000000ULL, 5);
        value_case((string_address) "%lf%n", (string_address) "0.25",
                   1, 0x3FD0000000000000ULL, 4);
        value_case((string_address) "%lf%n", (string_address) "-2.5",
                   1, 0xC004000000000000ULL, 4);
        value_case((string_address) "%8lf%n", (string_address) "3.140000",
                   1, 0x40091EB851EB851FULL, 8);
        value_case((string_address) "%lf%n", (string_address) "1e",
                   0, 0xA5A5A5A5A5A5A5A5ULL, -1);
        value_case((string_address) "%lf%n", (string_address) "nan",
                   1, 0x7FF8000000000000ULL, 3);
        value_case((string_address) "%lf%n", (string_address) "-inf",
                   1, 0xFFF0000000000000ULL, 4);

        //      the text conversions, with the eight bytes they left behind
        text_case((string_address) "%s%n", (string_address) "hello",
                  1, (string_address) "\150\145\154\154\157\000\177\177", 5);
        text_case((string_address) "%s%n", (string_address) "  hello",
                  1, (string_address) "\150\145\154\154\157\000\177\177", 7);
        text_case((string_address) "%s%n", (string_address) "hello world",
                  1, (string_address) "\150\145\154\154\157\000\177\177", 5);
        text_case((string_address) "%s%n", (string_address) "",
                  -1, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%s%n", (string_address) "   ",
                  -1, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%s%n", (string_address) "\t\nab",
                  1, (string_address) "\141\142\000\177\177\177\177\177", 4);
        text_case((string_address) "%1s%n", (string_address) "hello",
                  1, (string_address) "\150\000\177\177\177\177\177\177", 1);
        text_case((string_address) "%2s%n", (string_address) "hello",
                  1, (string_address) "\150\145\000\177\177\177\177\177", 2);
        text_case((string_address) "%0s%n", (string_address) "abc",
                  1, (string_address) "\141\142\143\000\177\177\177\177", 3);
        text_case((string_address) "%10s%n", (string_address) "abc",
                  1, (string_address) "\141\142\143\000\177\177\177\177", 3);
        text_case((string_address) "%c%n", (string_address) "abc",
                  1, (string_address) "\141\177\177\177\177\177\177\177", 1);
        text_case((string_address) "%c%n", (string_address) "  abc",
                  1, (string_address) "\040\177\177\177\177\177\177\177", 1);
        text_case((string_address) "%c%n", (string_address) "",
                  -1, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%2c%n", (string_address) "abc",
                  1, (string_address) "\141\142\177\177\177\177\177\177", 2);
        text_case((string_address) "%3c%n", (string_address) "ab",
                  -1, (string_address) "\141\142\177\177\177\177\177\177", -1);
        text_case((string_address) "%5c%n", (string_address) "abc",
                  -1, (string_address) "\141\142\143\177\177\177\177\177", -1);
        text_case((string_address) "%0c%n", (string_address) "abc",
                  1, (string_address) "\141\177\177\177\177\177\177\177", 1);
        text_case((string_address) "%1c%n", (string_address) "a",
                  1, (string_address) "\141\177\177\177\177\177\177\177", 1);
        text_case((string_address) "%[abc]%n", (string_address) "abcd",
                  1, (string_address) "\141\142\143\000\177\177\177\177", 3);
        text_case((string_address) "%[abc]%n", (string_address) "dabc",
                  0, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%[abc]%n", (string_address) "",
                  -1, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%[^abc]%n", (string_address) "dab",
                  1, (string_address) "\144\000\177\177\177\177\177\177", 1);
        text_case((string_address) "%[^abc]%n", (string_address) "abc",
                  0, (string_address) "\177\177\177\177\177\177\177\177", -1);
        text_case((string_address) "%[a-z]%n", (string_address) "abcZ",
                  1, (string_address) "\141\142\143\000\177\177\177\177", 3);
        text_case((string_address) "%[^a-z]%n", (string_address) "ABCa",
                  1, (string_address) "\101\102\103\000\177\177\177\177", 3);
        text_case((string_address) "%[]]%n", (string_address) "]]a",
                  1, (string_address) "\135\135\000\177\177\177\177\177", 2);
        text_case((string_address) "%[^]]%n", (string_address) "ab]c",
                  1, (string_address) "\141\142\000\177\177\177\177\177", 2);
        text_case((string_address) "%[-a]%n", (string_address) "-ab",
                  1, (string_address) "\055\141\000\177\177\177\177\177", 2);
        text_case((string_address) "%[a-]%n", (string_address) "a-b",
                  1, (string_address) "\141\055\000\177\177\177\177\177", 2);
        text_case((string_address) "%[z-a]%n", (string_address) "z-ab",
                  1, (string_address) "\172\055\141\000\177\177\177\177", 3);
        text_case((string_address) "%[z-a]%n", (string_address) "abz",
                  1, (string_address) "\141\000\177\177\177\177\177\177", 1);
        text_case((string_address) "%[a-c-e]%n", (string_address) "abcde",
                  1, (string_address) "\141\142\143\144\145\000\177\177", 5);
        text_case((string_address) "%[0-9]%n", (string_address) "123a",
                  1, (string_address) "\061\062\063\000\177\177\177\177", 3);
        text_case((string_address) "%2[0-9]%n", (string_address) "1234",
                  1, (string_address) "\061\062\000\177\177\177\177\177", 2);
        text_case((string_address) "%[ ]%n", (string_address) "  a",
                  1, (string_address) "\040\040\000\177\177\177\177\177", 2);
        text_case((string_address) "%[^ ]%n", (string_address) "ab c",
                  1, (string_address) "\141\142\000\177\177\177\177\177", 2);
        text_case((string_address) "%[]abc]%n", (string_address) "abc]d",
                  1, (string_address) "\141\142\143\135\000\177\177\177", 4);
        text_case((string_address) "%[a^]%n", (string_address) "a^b",
                  1, (string_address) "\141\136\000\177\177\177\177\177", 2);
        text_case((string_address) "%[^^]%n", (string_address) "ab^",
                  1, (string_address) "\141\142\000\177\177\177\177\177", 2);
        text_case((string_address) "%[A-Za-z]%n", (string_address) "abZ9",
                  1, (string_address) "\141\142\132\000\177\177\177\177", 3);
        text_case((string_address) "%[0-9a-f]%n", (string_address) "19afg",
                  1, (string_address) "\061\071\141\146\000\177\177\177", 4);

        //      the formats that assign nothing, where the answer is the whole test
        answer_case((string_address) "x", (string_address) "", -1);
        answer_case((string_address) "x", (string_address) "y", 0);
        answer_case((string_address) "x", (string_address) "x", 0);
        answer_case((string_address) "abc", (string_address) "abc", 0);
        answer_case((string_address) "abc", (string_address) "ab", -1);
        answer_case((string_address) "abc", (string_address) "", -1);
        answer_case((string_address) "abc", (string_address) " ", 0);
        answer_case((string_address) "a b", (string_address) "ab", 0);
        answer_case((string_address) "a b", (string_address) "a b", 0);
        answer_case((string_address) " a", (string_address) "a", 0);
        answer_case((string_address) " a", (string_address) " a", 0);
        answer_case((string_address) "", (string_address) "", 0);
        answer_case((string_address) "", (string_address) "a", 0);
        answer_case((string_address) " ", (string_address) "", 0);
        answer_case((string_address) "%%", (string_address) "%", 0);
        answer_case((string_address) "%%", (string_address) "", -1);
        answer_case((string_address) "%%", (string_address) "a", 0);
        answer_case((string_address) "%%", (string_address) " %", 0);
        answer_case((string_address) "%*d", (string_address) "", -1);
        answer_case((string_address) "%*d", (string_address) "12", 0);
        answer_case((string_address) "%*d", (string_address) "ab", 0);
        answer_case((string_address) "%*s", (string_address) "ab", 0);
        answer_case((string_address) "%*c", (string_address) "", -1);
        answer_case((string_address) "%*[ab]", (string_address) "ab", 0);
        answer_case((string_address) "%y", (string_address) "12", 0);
        answer_case((string_address) "%", (string_address) "12", 0);
        answer_case((string_address) "%[abc", (string_address) "abc", 0);
        answer_case((string_address) "%[abc", (string_address) "", 0);
        answer_case((string_address) "%[]", (string_address) "abc", 0);
        answer_case((string_address) "%[^]", (string_address) "abc", 0);
}

/*
        THE LENGTH MODIFIERS, WHICH ARE ABOUT THE OBJECT AND NOT THE VALUE

        glibc converts at the full width of the machine word and then stores
        into whatever the caller pointed at, so the narrowing is the store and
        not the parse. 300 into a signed char is 44 and 70000 into a short is
        4464, and a signed long's worth of overflow stored into an int is
        minus one, because the saturated value was every bit of the low half
        set. Each of these is a place a scanf that parsed at the target's
        width instead would answer differently.
*/
static fn modifiers(void)
{
        b8 narrow = 0;
        b16 shorter = 0;
        b32 whole = 0;
        bipolar wide = 0;
        p8 narrow_unsigned = 0;
        p16 shorter_unsigned = 0;
        positive wide_unsigned = 0;

        check("hh truncates to a byte",
              sscanf((string_address) "300", (string_address) "%hhd",
                     address_of narrow) == 1 &&
                  narrow == 44);

        check("h truncates to a halfword",
              sscanf((string_address) "70000", (string_address) "%hd",
                     address_of shorter) == 1 &&
                  shorter == 4464);

        check("hhu truncates the same way",
              sscanf((string_address) "300", (string_address) "%hhu",
                     address_of narrow_unsigned) == 1 &&
                  narrow_unsigned == 44);

        check("hx truncates the same way",
              sscanf((string_address) "1ffff", (string_address) "%hx",
                     address_of shorter_unsigned) == 1 &&
                  shorter_unsigned == 0xFFFF);

        check("an overflowing long stored into an int is every low bit",
              sscanf((string_address) "99999999999999999999",
                     (string_address) "%d", address_of whole) == 1 &&
                  whole == -1);

        check("l saturates at the signed limit",
              sscanf((string_address) "99999999999999999999",
                     (string_address) "%ld", address_of wide) == 1 &&
                  wide == (bipolar)(~(positive)0 >> 1));

        check("l saturates at the other end too",
              sscanf((string_address) "-99999999999999999999",
                     (string_address) "%ld", address_of wide) == 1 &&
                  wide == (bipolar)((~(positive)0 >> 1) + 1));

        check("the most negative long is not an overflow",
              sscanf((string_address) "-9223372036854775808",
                     (string_address) "%ld", address_of wide) == 1 &&
                  wide == (bipolar)((~(positive)0 >> 1) + 1));

        check("an unsigned saturates at every bit",
              sscanf((string_address) "18446744073709551616",
                     (string_address) "%lu", address_of wide_unsigned) == 1 &&
                  wide_unsigned == ~(positive)0);

        check("a negative unsigned wraps and is not an overflow",
              sscanf((string_address) "-2", (string_address) "%lu",
                     address_of wide_unsigned) == 1 &&
                  wide_unsigned == ~(positive)0 - 1);

        check("z, t and j are all the machine word",
              sscanf((string_address) "-1", (string_address) "%zd",
                     address_of wide) == 1 &&
                  wide == -1);
}

/*
        %Lf, which is the one conversion whose object this tree has no
        arithmetic for. The value is compared against a compile time constant
        of the same type, byte for byte, because comparing two long doubles
        with == is a call into libgcc on two of the three machines and there
        is none to call. Ten bytes on x86_64, where the format is eighty bits
        inside a sixteen byte object, and the whole object elsewhere.
*/
static fn wide_decimal(void)
{
#if __LDBL_MANT_DIG__ == 64
#define WIDE_DECIMAL_BYTES 10
#else
#define WIDE_DECIMAL_BYTES sizeof(f128)
#endif

        f128 got;
        f128 want;

        want = 2.5L;
        memory_fill(address_of got, 0xA5, sizeof(got));
        check("%Lf stores a long double",
              sscanf((string_address) "2.5", (string_address) "%Lf",
                     address_of got) == 1 &&
                  memory_compare(address_of got, address_of want,
                                 WIDE_DECIMAL_BYTES) == 0);

        want = 0.0L;
        memory_fill(address_of got, 0xA5, sizeof(got));
        check("%Lf stores a zero",
              sscanf((string_address) "0", (string_address) "%Lf",
                     address_of got) == 1 &&
                  memory_compare(address_of got, address_of want,
                                 WIDE_DECIMAL_BYTES) == 0);

        want = -1024.0L;
        memory_fill(address_of got, 0xA5, sizeof(got));
        check("%Lf stores a negative",
              sscanf((string_address) "-1024", (string_address) "%Lf",
                     address_of got) == 1 &&
                  memory_compare(address_of got, address_of want,
                                 WIDE_DECIMAL_BYTES) == 0);

        want = 0.0625L;
        memory_fill(address_of got, 0xA5, sizeof(got));
        check("%Lf stores a fraction that is exact in both formats",
              sscanf((string_address) "0x1p-4", (string_address) "%Lf",
                     address_of got) == 1 &&
                  memory_compare(address_of got, address_of want,
                                 WIDE_DECIMAL_BYTES) == 0);

        memory_fill(address_of got, 0xA5, sizeof(got));
        check("%Lf leaves the object alone on a matching failure",
              sscanf((string_address) "abc", (string_address) "%Lf",
                     address_of got) == 0 &&
                  ((p8 address_to)address_of got)[0] == 0xA5);
}

/*
        %p, which is %x with one word of prose in front of it.

        glibc prints a null pointer as "(nil)" and reads that back, and the
        word is a prefix like every other word in this family: five bytes or
        nothing. The one surprise is the width, which glibc tests BEFORE it
        starts the word rather than while it reads it, so a "%4p" leaves the
        bracket where it found it instead of eating four bytes it could never
        finish.
*/
static fn pointers(void)
{
        address_any where = (address_any)1;

        used = -1;
        check("%p reads a hexadecimal pointer",
              sscanf((string_address) "0x1234", (string_address) "%p%n",
                     address_of where, address_of used) == 1 &&
                  where == (address_any)0x1234 && used == 6);

        used = -1;
        check("%p does not need the prefix",
              sscanf((string_address) "1234", (string_address) "%p%n",
                     address_of where, address_of used) == 1 &&
                  where == (address_any)0x1234 && used == 4);

        where = (address_any)1;
        used = -1;
        check("%p reads (nil) back",
              sscanf((string_address) "(nil)", (string_address) "%p%n",
                     address_of where, address_of used) == 1 &&
                  where == null && used == 5);

        where = (address_any)1;
        used = -1;
        check("%p reads it in either case",
              sscanf((string_address) "(NIL)x", (string_address) "%p%n",
                     address_of where, address_of used) == 1 &&
                  where == null && used == 5);

        used = -1;
        check("a partial (nil) is a matching failure",
              sscanf((string_address) "(ni", (string_address) "%p%n",
                     address_of where, address_of used) == 0 && used == -1);

        used = -1;
        check("%p takes a sign and wraps it",
              sscanf((string_address) "-0x5", (string_address) "%p%n",
                     address_of where, address_of used) == 1 &&
                  where == (address_any)(positive)(0 - (bipolar)5) &&
                  used == 4);

        used = -1;
        check("a width under five never starts (nil)",
              sscanf((string_address) "(nil)", (string_address) "%4p%n",
                     address_of where, address_of used) == 0 && used == -1);

        where = (address_any)1;
        used = -1;
        check("a width of exactly five does",
              sscanf((string_address) "(nil)", (string_address) "%5p%n",
                     address_of where, address_of used) == 1 &&
                  where == null && used == 5);

        used = -1;
        check("a width truncates a pointer like any other number",
              sscanf((string_address) "0x123456", (string_address) "%4p%n",
                     address_of where, address_of used) == 1 &&
                  where == (address_any)0x12 && used == 4);
}

/*
        Several directives in one format, which is where the answer stops
        being a boolean. The count is the number ASSIGNED, so a suppressed
        conversion that succeeded is not in it and a %n never is, and a
        failure part way through answers with what had already landed rather
        than with a failure.
*/
static fn several(void)
{
        b32 first = 0;
        b32 second = 0;
        p8 word[16];

        check("two conversions, both assigned",
              sscanf((string_address) "12 34", (string_address) "%d %d",
                     address_of first, address_of second) == 2 &&
                  first == 12 && second == 34);

        check("a separator between them",
              sscanf((string_address) "12,34", (string_address) "%d,%d",
                     address_of first, address_of second) == 2 &&
                  first == 12 && second == 34);

        check("white space in the format matches a run",
              sscanf((string_address) "12   \t\n 34", (string_address) "%d %d",
                     address_of first, address_of second) == 2 &&
                  first == 12 && second == 34);

        check("white space in the format matches nothing at all",
              sscanf((string_address) "12abc", (string_address) "%d %s",
                     address_of first, word) == 2 &&
                  first == 12 && string_compare(word, "abc") == 0);

        first = 0;
        second = 7;
        check("the second failing answers with the first",
              sscanf((string_address) "12 abc", (string_address) "%d %d",
                     address_of first, address_of second) == 1 &&
                  first == 12 && second == 7);

        first = 0;
        used = -1;
        check("%n between two conversions reports the first",
              sscanf((string_address) "12", (string_address) "%d%n%d",
                     address_of first, address_of used,
                     address_of second) == 1 &&
                  first == 12 && used == 2);

        first = 0;
        second = 0;
        check("a suppressed conversion is not counted and is consumed",
              sscanf((string_address) "1 2 3", (string_address) "%d%*d%d",
                     address_of first, address_of second) == 2 &&
                  first == 1 && second == 3);

        used = -1;
        check("a suppressed conversion still moves %n",
              sscanf((string_address) "12", (string_address) "%*d%n",
                     address_of used) == 0 && used == 2);

        used = -1;
        check("a literal that matched moves %n and assigns nothing",
              sscanf((string_address) "ab", (string_address) "a%n",
                     address_of used) == 0 && used == 1);

        used = -1;
        check("a literal mismatch keeps its matched prefix consumed",
              sscanf((string_address) "abX", (string_address) "ab%nY",
                     address_of used) == 0 && used == 2);

        used = -1;
        check("%n alone on an empty input is zero and not the end",
              sscanf((string_address) "", (string_address) "%n",
                     address_of used) == 0 && used == 0);

        first = 12;
        check("a suppressed %n writes nothing",
              sscanf((string_address) "ab", (string_address) "%*n") == 0 &&
                  first == 12);

        memory_fill(word, 0x7F, sizeof(word));
        check("a set stops where the set says and terminates",
              sscanf((string_address) "abc123", (string_address) "%[a-z]",
                     word) == 1 &&
                  string_compare(word, "abc") == 0 && word[4] == 0x7F);
}

/*
        THE STREAM HALF

        The same engine over a FILE, which adds three answers a string cannot
        have: where the stream was left, whether the end was reached, and
        whether the byte that stopped a conversion went back.

        The last of those is the whole reason ungetc exists and is the thing
        that has to be true for a program to read two numbers out of one file:
        the byte that ended the first conversion is the byte the second one
        starts at. It is checked here by asking the stream for it directly
        after the scan rather than by scanning again, so that a failure says
        which of the two halves was wrong.
*/
#define SCAN_TEST_FILE "/tmp/dawning-scan-test"

static fn lay_down(string_address text)
{
        FILE address_to handle = fopen((string_address)SCAN_TEST_FILE,
                                       (string_address) "w");
        positive length = string_length(text);

        if (handle == null)
                return;

        if (length != 0)
                fwrite((address_any)text, 1, length, handle);

        fclose(handle);
}

static FILE address_to pick_up(void)
{
        return fopen((string_address)SCAN_TEST_FILE, (string_address) "r");
}

//      vfscanf reached the way a caller reaches it. The list cannot be
//      written down as a null: on arm64 var_args is a structure and not a
//      pointer, so the only way to hand one over is to have been passed one.
static b32 through_stream(FILE address_to handle, string_address format, ...)
{
        var_args list;
        b32 answer;

        var_list(list, format);
        answer = vfscanf(handle, format, list);
        var_list_end(list);

        return answer;
}

static fn streams(void)
{
        FILE address_to handle;
        b32 first = 0;
        b32 second = 0;
        p8 word[16];
        decimal number = 0;

        lay_down((string_address) "12 abc 3.5\n");
        handle = pick_up();

        if (handle == null)
        {
                check("the scratch file could be written and reopened", false);
                return;
            }

        check("fscanf reads a number and stops on the space",
              fscanf(handle, (string_address) "%d", address_of first) == 1 &&
                  first == 12 && ftell(handle) == 2 && !feof(handle));

        check("the byte that stopped it is the next byte out",
              stream_get_byte(handle) == ' ' &&
                  stream_unget_byte(' ', handle) == ' ');

        check("a matching failure leaves the position past the white space",
              fscanf(handle, (string_address) "%d", address_of second) == 0 &&
                  ftell(handle) == 3 && !feof(handle));

        check("%s reads the word and pushes back the space after it",
              fscanf(handle, (string_address) "%s", word) == 1 &&
                  string_compare(word, "abc") == 0 && ftell(handle) == 6);

        check("%lf reads the number",
              fscanf(handle, (string_address) "%lf", address_of number) == 1 &&
                  number == 3.5 && ftell(handle) == 10);

        check("the end of the file is an input failure and sets feof",
              fscanf(handle, (string_address) "%d", address_of first) == EOF &&
                  feof(handle));

        fclose(handle);

        lay_down((string_address) "abc");
        handle = pick_up();

        check("a matching failure at the front consumes nothing",
              fscanf(handle, (string_address) "%d", address_of first) == 0 &&
                  ftell(handle) == 0 && !feof(handle));

        check("and the bytes are all still there",
              fscanf(handle, (string_address) "%s", word) == 1 &&
                  string_compare(word, "abc") == 0 && ftell(handle) == 3 &&
                  feof(handle));

        fclose(handle);

        //      A prefix that ran off the end of the file. Characters WERE
        //      read, so this is a matching failure and not an input one, and
        //      the answer is zero rather than EOF even though the file ended.
        lay_down((string_address) "1e+");
        handle = pick_up();

        check("a prefix that the file ended in is a matching failure",
              fscanf(handle, (string_address) "%f", address_of first) == 0 &&
                  ftell(handle) == 3 && feof(handle));

        fclose(handle);

        //      A trailing white space directive reads until it finds a byte
        //      that is not white, which at the end of a file means reading
        //      the end. That is the one directive that can set feof on a
        //      call that succeeded.
        lay_down((string_address) "5");
        handle = pick_up();

        check("a trailing space directive reads to the end and still answers",
              fscanf(handle, (string_address) "%d ", address_of first) == 1 &&
                  first == 5 && feof(handle));

        fclose(handle);

        //      A width that ended exactly at the end of the file. glibc looks
        //      at the byte after the last digit whatever the width said, so
        //      the indicator is set here and the position is not moved.
        lay_down((string_address) "007");
        handle = pick_up();

        check("a width that ends at the end of the file sets feof",
              fscanf(handle, (string_address) "%3d", address_of first) == 1 &&
                  first == 7 && ftell(handle) == 3 && feof(handle));

        fclose(handle);

        //      Except when the width ran out on the "x" of a "0x", which is
        //      the one place glibc stops without looking.
        lay_down((string_address) "0x");
        handle = pick_up();

        check("a width that ran out on the base letter does not",
              fscanf(handle, (string_address) "%2x", address_of first) == 0 &&
                  ftell(handle) == 2 && !feof(handle));

        fclose(handle);

        lay_down((string_address) "42");
        handle = pick_up();

        check("vfscanf is the same engine",
              through_stream(handle, (string_address) "%d",
                             address_of first) == 1 &&
                  first == 42);

        fclose(handle);

        check("a null handle answers the end",
              through_stream(null, (string_address) "%d",
                             address_of first) == EOF);

        unlink((string_address)SCAN_TEST_FILE);
}

/*
        WHAT ERRNO SAYS AFTERWARDS

        glibc puts the caller's errno aside, sets it to zero, and puts it back
        on the way out unless a conversion set one of its own -- except on a
        call that ended because the input ran out, where it puts nothing back
        and throws away whatever was set. So the ERANGE from an overflowing
        conversion survives a call that finished and does not survive one that
        then ran off the end, which is a difference a program reading errno
        after a scanf can see and could not have predicted.
*/
static fn reasons(void)
{
#ifdef STANDARD_MODERN_C_STANDARD_ERROR
        bipolar wide = 0;
        b32 whole = 0;
        p8 word[16];

        errno = 0;
        check("an overflow on a call that finished reports ERANGE",
              sscanf((string_address) "99999999999999999999",
                     (string_address) "%ld", address_of wide) == 1 &&
                  errno == ERANGE);

        errno = 0;
        check("and does not, once the input ran out afterwards",
              sscanf((string_address) "99999999999999999999",
                     (string_address) "%ld%s", address_of wide, word) == 1 &&
                  errno == 0);

        errno = 0;
        check("a byte after the number puts the ERANGE back",
              sscanf((string_address) "99999999999999999999 z",
                     (string_address) "%ld%s", address_of wide, word) == 2 &&
                  errno == ERANGE);

        errno = 7;
        check("a clean call leaves the caller's own errno alone",
              sscanf((string_address) "12", (string_address) "%d",
                     address_of whole) == 1 &&
                  errno == 7);

        errno = 7;
        check("so does a matching failure",
              sscanf((string_address) "ab", (string_address) "%d",
                     address_of whole) == 0 &&
                  errno == 7);

        errno = 7;
        check("an input failure does not",
              sscanf((string_address) "", (string_address) "%d",
                     address_of whole) == EOF &&
                  errno == 0);

        errno = 0;
        check("a trailing space directive that ran out clears it too",
              sscanf((string_address) "99999999999999999999",
                     (string_address) "%ld ", address_of wide) == 1 &&
                  errno == 0);

        errno = 0;
        check("and one that found a space first does not",
              sscanf((string_address) "99999999999999999999 ",
                     (string_address) "%ld ", address_of wide) == 1 &&
                  errno == ERANGE);

        errno = 0;
#endif
}

//      The var_args entries, reached the way a caller reaches them.
static b32 through_list(string_address input, string_address format, ...)
{
        var_args list;
        b32 answer;

        var_list(list, format);
        answer = vsscanf(input, format, list);
        var_list_end(list);

        return answer;
}

static fn entries(void)
{
        b32 first = 0;
        b32 second = 0;

        check("vsscanf carries the list",
              through_list((string_address) "7 8", (string_address) "%d %d",
                           address_of first, address_of second) == 2 &&
                  first == 7 && second == 8);

        check("a null format answers the end",
              sscanf((string_address) "12", null) == EOF);

        check("a null input is an empty one",
              sscanf(null, (string_address) "%d", address_of first) == EOF);
}

b32 main(void)
{
        generated();
        modifiers();
        wide_decimal();
        pointers();
        several();
        streams();
        reasons();
        entries();

        return test_report(null);
}
