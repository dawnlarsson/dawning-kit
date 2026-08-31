/*
        Every name library.c gives a standard spelling to, called by that
        spelling.

        This file is the point of the whole exercise and it is deliberately
        plain C. It declares no types of its own, casts nothing, and reaches
        for no house name: it says memcpy and strlen and sqrt the way a
        program ported into this distribution would say them. That is what
        makes it a test of the declarations rather than of the routines.

        It is compiled twice from two files that share nothing but this one.
        src/test/declare.c includes src/compiler_memory.c and gets every name
        below from src/standard/declare.c and the ASM_ALIAS symbols under it.
        src/test/declare_reference.c includes the host's <string.h>, <ctype.h>,
        <stdlib.h>, <strings.h>, <math.h> and <setjmp.h> and gets them from
        glibc. If a declaration in declare.c disagreed with the one in a real
        header by so much as a const, the second build would not compile, and
        the second build compiling is the assertion that matters most here.
        The two are then supposed to print the same verdict.

        The wrapper supplies three things and this file assumes nothing
        else, and prints the verdict itself afterwards:

            declare_check(name, condition)   counts, and names the failure
            declare_jump_state               jump_state here, jmp_buf there
            DECLARE_FREESTANDING             defined only in the first build

        DECLARE_FREESTANDING appears exactly twice below. Once for the
        identity checks, which ask whether the standard name and the prose
        name are one address -- a question glibc has no opinion about. And
        once for strncpy, which is the single case in four hundred and forty
        two where the answer here and glibc's answer are not the same byte;
        both sides of it are written out, so the divergence is recorded in
        the file rather than in a comment about the file.
*/

static char declare_left[64];
static char declare_right[64];

static int declare_offset(const void *found, const void *base)
{
        if (found == 0)
                return -1;

        return (int)((const char *)found - (const char *)base);
}

/*
        Two addresses compared where the linker settled them and not where
        the compiler guessed. The reason is at the identity checks below.
*/
#define declare_same(name, one, two)                                          \
        do {                                                                  \
                void *volatile first = (void *)(one);                         \
                void *volatile second = (void *)(two);                        \
                                                                              \
                declare_check(name, first == second);                         \
        } while (0)

static int declare_sign(int value)
{
        if (value < 0)
                return -1;

        return value > 0;
}

/*
        The C locale, written out.

        Every ctype answer below is compared against this rather than against
        a table of expected bits, because the point is that both libraries
        implement the same definition and a table would only prove they match
        each other. isascii and toascii are not C's at all -- they are the
        pre-standard pair POSIX kept -- and their definition is the mask.
*/
static int declare_is_upper(int c) { return c >= 'A' && c <= 'Z'; }
static int declare_is_lower(int c) { return c >= 'a' && c <= 'z'; }
static int declare_is_digit(int c) { return c >= '0' && c <= '9'; }
static int declare_is_alpha(int c) { return declare_is_upper(c) || declare_is_lower(c); }
static int declare_is_alnum(int c) { return declare_is_alpha(c) || declare_is_digit(c); }
static int declare_is_space(int c)
{
        return c == ' ' || (c >= '\t' && c <= '\r');
}
static int declare_is_print(int c) { return c >= ' ' && c <= '~'; }
static int declare_is_graph(int c) { return c > ' ' && c <= '~'; }
static int declare_is_cntrl(int c) { return (c >= 0 && c < ' ') || c == 0x7f; }
static int declare_is_punct(int c) { return declare_is_graph(c) && !declare_is_alnum(c); }
static int declare_is_blank(int c) { return c == ' ' || c == '\t'; }
static int declare_is_hex(int c)
{
        return declare_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void declare_cases(void)
{
        char *found;
        void *block;
        char *stop;
        int index;

        /*
                The byte hunts, and what each answers when the byte is not
                there. strchr and memchr say null; strchrnul says the
                terminator's address, which is the only reason it exists.
                strrchr is string_last_of_or_end, whose name is about the one
                case where the byte hunted for IS the terminator -- there the
                answer is the end of the string, which is what C says too.
        */
        declare_check("strchr hit", declare_offset(strchr("hello", 'l'), "hello") == 2);
        declare_check("strchr miss", strchr("hello", 'z') == 0);
        declare_check("strchr terminator",
                      declare_offset(strchr("hello", 0), "hello") == 5);
        declare_check("strrchr hit", declare_offset(strrchr("hello", 'l'), "hello") == 3);
        declare_check("strrchr miss", strrchr("hello", 'z') == 0);
        declare_check("strrchr terminator",
                      declare_offset(strrchr("hello", 0), "hello") == 5);
        declare_check("strchrnul hit",
                      declare_offset(strchrnul("hello", 'l'), "hello") == 2);
        declare_check("strchrnul miss",
                      declare_offset(strchrnul("hello", 'z'), "hello") == 5);

        /*
                The character argument is an int and the routine reads its
                low byte, which is what C means by "converted to unsigned
                char". 0x16c is 'l' with a ninth bit set and -152 is 'h' sign
                extended; both have to find their byte.
        */
        declare_check("strchr high bits",
                      declare_offset(strchr("hello", 0x16c), "hello") == 2);
        declare_check("strchr negative",
                      declare_offset(strchr("hello", -152), "hello") == 0);
        declare_check("memchr hit", declare_offset(memchr("hello", 'l', 5), "hello") == 2);
        declare_check("memchr miss", memchr("hello", 'z', 5) == 0);
        declare_check("memchr high bits",
                      declare_offset(memchr("hello", 0x16c, 5), "hello") == 2);
        declare_check("memchr no bytes", memchr("hello", 'h', 0) == 0);
        declare_check("memrchr hit",
                      declare_offset(memrchr("hello", 'l', 5), "hello") == 3);
        declare_check("memrchr miss", memrchr("hello", 'z', 5) == 0);
        declare_check("memrchr high bits",
                      declare_offset(memrchr("hello", 0x16c, 5), "hello") == 3);

        /*
                The comparisons, and the sign each hands back. C says the
                bytes are compared as unsigned char, so 0x80 is above 0x01
                and not below it, which is the one way a comparison written
                with a signed char is wrong on every input with the top bit
                set.
        */
        declare_check("memcmp below", declare_sign(memcmp("\x01", "\x02", 1)) == -1);
        declare_check("memcmp above", declare_sign(memcmp("\x02", "\x01", 1)) == 1);
        declare_check("memcmp equal", memcmp("\x02", "\x02", 1) == 0);
        declare_check("memcmp unsigned", declare_sign(memcmp("\x80", "\x01", 1)) == 1);
        declare_check("memcmp no bytes", memcmp("\x80", "\x01", 0) == 0);
        declare_check("strcmp unsigned", declare_sign(strcmp("\x80", "\x01")) == 1);
        declare_check("strcmp prefix", declare_sign(strcmp("ab", "abc")) == -1);
        declare_check("strcmp equal", strcmp("abc", "abc") == 0);
        declare_check("strncmp bound", strncmp("abZ", "abY", 2) == 0);
        declare_check("strncmp past", declare_sign(strncmp("abZ", "abY", 3)) == 1);
        declare_check("strncmp no bytes", strncmp("a", "b", 0) == 0);
        declare_check("bcmp differs", bcmp("\x01", "\x02", 1) != 0);
        declare_check("bcmp same", bcmp("\x02", "\x02", 1) == 0);
        declare_check("strcasecmp equal", strcasecmp("AbC", "aBc") == 0);
        declare_check("strcasecmp below", declare_sign(strcasecmp("abc", "abd")) == -1);
        declare_check("strncasecmp bound", strncasecmp("AbZ", "aBY", 2) == 0);

        /*
                The lengths and the spans.
        */
        declare_check("strlen", strlen("hello") == 5);
        declare_check("strlen empty", strlen("") == 0);
        declare_check("strnlen under", strnlen("hello", 3) == 3);
        declare_check("strnlen over", strnlen("hello", 9) == 5);
        declare_check("strnlen none", strnlen("hello", 0) == 0);
        declare_check("strspn", strspn("abcXY", "cba") == 3);
        declare_check("strspn none", strspn("Xabc", "abc") == 0);
        declare_check("strspn empty set", strspn("abc", "") == 0);
        declare_check("strcspn", strcspn("abcXY", "YX") == 3);
        declare_check("strcspn none", strcspn("abc", "XY") == 3);
        declare_check("strcspn empty set", strcspn("abc", "") == 3);
        declare_check("strpbrk hit", declare_offset(strpbrk("abcXY", "YX"), "abcXY") == 3);
        declare_check("strpbrk miss", strpbrk("abc", "XY") == 0);
        declare_check("strpbrk empty set", strpbrk("abc", "") == 0);

        /*
                The searches. An empty needle is found at the front, which is
                what C says for strstr and what POSIX says for memmem, and a
                needle longer than the haystack is not found at all.
        */
        declare_check("strstr hit", declare_offset(strstr("abcdef", "cd"), "abcdef") == 2);
        declare_check("strstr miss", strstr("abcdef", "cz") == 0);
        declare_check("strstr empty", declare_offset(strstr("abcdef", ""), "abcdef") == 0);
        declare_check("memmem hit",
                      declare_offset(memmem("abcdef", 6, "cd", 2), "abcdef") == 2);
        declare_check("memmem miss", memmem("abcdef", 6, "cz", 2) == 0);
        declare_check("memmem empty",
                      declare_offset(memmem("abcdef", 6, "", 0), "abcdef") == 0);
        declare_check("memmem too long", memmem("abc", 3, "abcd", 4) == 0);
        declare_check("strcasestr hit",
                      declare_offset(strcasestr("abCDef", "cd"), "abCDef") == 2);
        declare_check("strcasestr miss", strcasestr("abcdef", "cz") == 0);

        /*
                The block moves. memset takes an int and keeps its low byte,
                so 0x141 fills with 'A' and -1 fills with 0xff.
        */
        memset(declare_left, 'X', sizeof declare_left);
        memset(declare_left, 0x141, 4);
        declare_check("memset low byte",
                      declare_left[0] == 'A' && declare_left[3] == 'A' &&
                              declare_left[4] == 'X');
        memset(declare_left, -1, 4);
        declare_check("memset negative",
                      (unsigned char)declare_left[0] == 0xff &&
                              declare_left[4] == 'X');
        block = memset(declare_left, 'X', sizeof declare_left);
        declare_check("memset returns destination", block == declare_left);

        memset(declare_left, 'X', sizeof declare_left);
        bzero(declare_left, 3);
        declare_check("bzero", declare_left[0] == 0 && declare_left[2] == 0 &&
                                       declare_left[3] == 'X');

        memset(declare_left, 'X', sizeof declare_left);
        block = memcpy(declare_left, "abcdefgh", 8);
        declare_check("memcpy", memcmp(declare_left, "abcdefgh", 8) == 0);
        declare_check("memcpy returns destination", block == declare_left);

        /*
                memmove and memcpy are one routine here, so the overlap has
                to be right in both directions or the alias is a lie. This is
                the check that says which way the guarantee runs.
        */
        memset(declare_left, 'X', sizeof declare_left);
        memcpy(declare_left, "abcdefgh", 8);
        block = memmove(declare_left + 2, declare_left, 8);
        declare_check("memmove forward overlap",
                      memcmp(declare_left, "ababcdefgh", 10) == 0);
        declare_check("memmove returns destination", block == declare_left + 2);
        memset(declare_left, 'X', sizeof declare_left);
        memcpy(declare_left + 2, "abcdefgh", 8);
        memmove(declare_left, declare_left + 2, 8);
        declare_check("memmove backward overlap",
                      memcmp(declare_left, "abcdefgh", 8) == 0);

        memset(declare_left, 'X', sizeof declare_left);
        bcopy("abcd", declare_left, 4);
        declare_check("bcopy source first",
                      memcmp(declare_left, "abcdX", 5) == 0);

        /*
                memccpy stops after the byte it was given, and answers with
                the address one past where that byte landed, or null when it
                never turned up.
        */
        memset(declare_left, 'X', sizeof declare_left);
        block = memccpy(declare_left, "abcdef", 'c', 6);
        declare_check("memccpy stops", memcmp(declare_left, "abcXX", 5) == 0);
        declare_check("memccpy returns past", declare_offset(block, declare_left) == 3);
        memset(declare_left, 'X', sizeof declare_left);
        block = memccpy(declare_left, "abcdef", 'z', 6);
        declare_check("memccpy copies all", memcmp(declare_left, "abcdef", 6) == 0);
        declare_check("memccpy misses", block == 0);
        memset(declare_left, 'X', sizeof declare_left);
        block = memccpy(declare_left, "abcdef", 0x163, 6);
        declare_check("memccpy low byte", declare_offset(block, declare_left) == 3);

        /*
                The copies that terminate, and the two that answer with the
                end rather than the start.
        */
        memset(declare_left, 'X', sizeof declare_left);
        found = strcpy(declare_left, "abc");
        declare_check("strcpy", memcmp(declare_left, "abc\0X", 5) == 0);
        declare_check("strcpy returns destination", found == declare_left);
        memset(declare_left, 'X', sizeof declare_left);
        found = stpcpy(declare_left, "abc");
        declare_check("stpcpy", memcmp(declare_left, "abc\0X", 5) == 0);
        declare_check("stpcpy returns end", declare_offset(found, declare_left) == 3);

        memset(declare_left, 'X', sizeof declare_left);
        found = strncpy(declare_left, "abcdefgh", 4);
        declare_check("strncpy full writes no terminator",
                      memcmp(declare_left, "abcdX", 5) == 0);
        declare_check("strncpy returns destination", found == declare_left);

        /*
                THE ONE DIVERGENCE.

                C says strncpy pads the rest of the bound with zeroes. The
                symbol here is string_copy_max, which stops at the source's
                terminator and writes nothing after it, and library.c says so
                where the alias is made. Both answers are written out so that
                the two builds agree with their own libraries and the
                difference is visible in the source of the test rather than
                only in a report about it.
        */
        memset(declare_left, 'X', sizeof declare_left);
        strncpy(declare_left, "abc", 8);
#ifdef DECLARE_FREESTANDING
        declare_check("strncpy short pads nothing",
                      memcmp(declare_left, "abc\0XXXX", 8) == 0);
#else
        declare_check("strncpy short pads with zeroes",
                      memcmp(declare_left, "abc\0\0\0\0\0", 8) == 0);
#endif

        /*
                stpncpy, which does pad, and is where the difference above is
                easiest to see: the two routines are a bound apart in the
                same file and only one of them fills the tail.
        */
        memset(declare_left, 'X', sizeof declare_left);
        found = stpncpy(declare_left, "abc", 8);
        declare_check("stpncpy short pads",
                      memcmp(declare_left, "abc\0\0\0\0\0X", 9) == 0);
        declare_check("stpncpy returns first zero",
                      declare_offset(found, declare_left) == 3);
        memset(declare_left, 'X', sizeof declare_left);
        found = stpncpy(declare_left, "abcdefgh", 4);
        declare_check("stpncpy full", memcmp(declare_left, "abcdX", 5) == 0);
        declare_check("stpncpy full returns bound",
                      declare_offset(found, declare_left) == 4);

        memset(declare_left, 'X', sizeof declare_left);
        strcpy(declare_left, "ab");
        found = strcat(declare_left, "cd");
        declare_check("strcat", memcmp(declare_left, "abcd\0X", 6) == 0);
        declare_check("strcat returns destination", found == declare_left);
        memset(declare_left, 'X', sizeof declare_left);
        strcpy(declare_left, "ab");
        found = strncat(declare_left, "cdef", 2);
        declare_check("strncat bound", memcmp(declare_left, "abcd\0X", 6) == 0);
        declare_check("strncat returns destination", found == declare_left);
        memset(declare_left, 'X', sizeof declare_left);
        strcpy(declare_left, "ab");
        strncat(declare_left, "cd", 8);
        declare_check("strncat under bound", memcmp(declare_left, "abcd\0X", 6) == 0);

        /*
                The bounded pair BSD added and glibc took in 2.38. Both
                answer with the length the source would have needed.
        */
        memset(declare_left, 'X', sizeof declare_left);
        declare_check("strlcpy returns source length",
                      strlcpy(declare_left, "abcdef", 4) == 6);
        declare_check("strlcpy truncates",
                      memcmp(declare_left, "abc\0X", 5) == 0);
        memset(declare_left, 'X', sizeof declare_left);
        strcpy(declare_left, "ab");
        declare_check("strlcat returns wanted length",
                      strlcat(declare_left, "cdef", 4) == 6);
        declare_check("strlcat truncates", memcmp(declare_left, "abc\0X", 5) == 0);

        /*
                The duplications, which allocate, and the joke that is its
                own inverse.
        */
        found = strdup("copied");
        declare_check("strdup", found != 0 && strcmp(found, "copied") == 0);
        found = strndup("copied", 3);
        declare_check("strndup", found != 0 && strcmp(found, "cop") == 0);
        memcpy(declare_left, "abcd", 5);
        memfrob(declare_left, 4);
        declare_check("memfrob", memcmp(declare_left, "abcd", 4) != 0);
        memfrob(declare_left, 4);
        declare_check("memfrob twice", memcmp(declare_left, "abcd\0", 5) == 0);

        /*
                The three that cut a string up, and the state each keeps.
        */
        strcpy(declare_left, "a,b,,c");
        found = strtok(declare_left, ",");
        declare_check("strtok first", found != 0 && strcmp(found, "a") == 0);
        found = strtok(0, ",");
        declare_check("strtok second", found != 0 && strcmp(found, "b") == 0);
        found = strtok(0, ",");
        declare_check("strtok skips empty", found != 0 && strcmp(found, "c") == 0);
        declare_check("strtok end", strtok(0, ",") == 0);

        strcpy(declare_left, "x:y");
        {
                char *held = 0;

                found = strtok_r(declare_left, ":", &held);
                declare_check("strtok_r first", found != 0 && strcmp(found, "x") == 0);
                found = strtok_r(0, ":", &held);
                declare_check("strtok_r second", found != 0 && strcmp(found, "y") == 0);
                declare_check("strtok_r end", strtok_r(0, ":", &held) == 0);
        }

        strcpy(declare_left, "a,b,,c");
        {
                char *held = declare_left;

                found = strsep(&held, ",");
                declare_check("strsep first", found != 0 && strcmp(found, "a") == 0);
                found = strsep(&held, ",");
                declare_check("strsep second", found != 0 && strcmp(found, "b") == 0);
                found = strsep(&held, ",");
                declare_check("strsep keeps empty", found != 0 && found[0] == 0);
                found = strsep(&held, ",");
                declare_check("strsep last", found != 0 && strcmp(found, "c") == 0);
                declare_check("strsep end", strsep(&held, ",") == 0);
        }

        /*
                <ctype.h> over its whole domain, which is EOF and every
                unsigned char, against the C locale written out above. The
                argument is an int for exactly this reason: -1 has to be
                sayable and has to answer no to all twelve.
        */
        for (index = -1; index < 256; index++) {
                declare_check("isalnum", !!isalnum(index) == declare_is_alnum(index));
                declare_check("isalpha", !!isalpha(index) == declare_is_alpha(index));
                declare_check("isblank", !!isblank(index) == declare_is_blank(index));
                declare_check("iscntrl", !!iscntrl(index) == declare_is_cntrl(index));
                declare_check("isdigit", !!isdigit(index) == declare_is_digit(index));
                declare_check("isgraph", !!isgraph(index) == declare_is_graph(index));
                declare_check("islower", !!islower(index) == declare_is_lower(index));
                declare_check("isprint", !!isprint(index) == declare_is_print(index));
                declare_check("ispunct", !!ispunct(index) == declare_is_punct(index));
                declare_check("isspace", !!isspace(index) == declare_is_space(index));
                declare_check("isupper", !!isupper(index) == declare_is_upper(index));
                declare_check("isxdigit", !!isxdigit(index) == declare_is_hex(index));
                declare_check("tolower",
                              tolower(index) ==
                                      (declare_is_upper(index) ? index + 32 : index));
                declare_check("toupper",
                              toupper(index) ==
                                      (declare_is_lower(index) ? index - 32 : index));
                declare_check("isascii", !!isascii(index) == (index >= 0 && index < 128));
                declare_check("toascii", toascii(index) == (index & 0x7f));
        }

        /*
                The conversions. atoi skips leading space, takes a sign, and
                stops at the first byte that is not a digit; strtol says
                where it stopped and takes a base, with zero meaning "read
                the prefix".
        */
        declare_check("atoi", atoi("  -42xyz") == -42);
        declare_check("atoi nothing", atoi("xyz") == 0);
        declare_check("atoi plus", atoi("+17") == 17);
        declare_check("atol", atol("-9007199254740993") == -9007199254740993L);
        declare_check("atoll", atoll("9007199254740993") == 9007199254740993LL);

        declare_check("strtol", strtol("  -42xyz", &stop, 10) == -42);
        declare_check("strtol stopped", stop != 0 && strcmp(stop, "xyz") == 0);
        declare_check("strtol hex", strtol("0x1f", &stop, 16) == 31);
        declare_check("strtol hex stopped", stop != 0 && *stop == 0);
        declare_check("strtol base zero hex", strtol("0x1f", &stop, 0) == 31);
        declare_check("strtol base zero octal", strtol("017", &stop, 0) == 15);
        declare_check("strtol base thirty six", strtol("zz", &stop, 36) == 1295);
        declare_check("strtol nothing", strtol("nope", &stop, 10) == 0);
        declare_check("strtol nothing stopped",
                      stop != 0 && strcmp(stop, "nope") == 0);
        declare_check("strtoll", strtoll("-9223372036854775807", &stop, 10) ==
                                         -9223372036854775807LL);
        declare_check("strtoul", strtoul("4294967296", &stop, 10) == 4294967296UL);
        declare_check("strtoull", strtoull("ff", &stop, 16) == 255ULL);

        declare_check("abs", abs(-7) == 7 && abs(7) == 7);
        declare_check("labs", labs(-9007199254740993L) == 9007199254740993L);
        declare_check("llabs", llabs(-9007199254740993LL) == 9007199254740993LL);

        /*
                ffs counts from one and answers zero for a word with nothing
                in it, which is the one bit routine in the library whose
                index is not zero based.
        */
        declare_check("ffs none", ffs(0) == 0);
        declare_check("ffs first", ffs(1) == 1);
        declare_check("ffs ninth", ffs(0x100) == 9);
        declare_check("ffs all", ffs(-1) == 1);
        declare_check("ffsl none", ffsl(0) == 0);
        declare_check("ffsl wide", ffsl(0x100000000L) == 33);
        declare_check("ffsll none", ffsll(0) == 0);
        declare_check("ffsll wide", ffsll(0x100000000LL) == 33);

        /*
                The roundings, which differ from each other only on a tie and
                only on a negative, so the table is halves.
        */
        declare_check("trunc", trunc(2.5) == 2.0 && trunc(-2.5) == -2.0);
        declare_check("floor", floor(2.5) == 2.0 && floor(-2.5) == -3.0);
        declare_check("floor whole", floor(-1.5) == -2.0 && floor(1.0) == 1.0);
        declare_check("ceil", ceil(2.5) == 3.0 && ceil(-2.5) == -2.0);
        declare_check("round away", round(2.5) == 3.0 && round(-2.5) == -3.0);
        declare_check("round half", round(0.5) == 1.0 && round(-0.5) == -1.0);
        declare_check("nearbyint even", nearbyint(2.5) == 2.0 && nearbyint(1.5) == 2.0);
        declare_check("rint even", rint(2.5) == 2.0 && rint(3.5) == 4.0);
        declare_check("trunc large", trunc(1e300) == 1e300);
        declare_check("floor large", floor(-1e300) == -1e300);

        declare_check("sqrt", sqrt(144.0) == 12.0 && sqrt(0.25) == 0.5);
        declare_check("sqrt zero", sqrt(0.0) == 0.0);
        declare_check("fabs", fabs(-3.7) == 3.7 && fabs(3.7) == 3.7);
        declare_check("copysign", copysign(3.0, -1.0) == -3.0 &&
                                          copysign(-3.0, 1.0) == 3.0);
        declare_check("fmin", fmin(2.0, 3.0) == 2.0 && fmin(-2.0, -3.0) == -3.0);
        declare_check("fmax", fmax(2.0, 3.0) == 3.0 && fmax(-2.0, -3.0) == -2.0);
        declare_check("fdim", fdim(5.0, 3.0) == 2.0 && fdim(3.0, 5.0) == 0.0);
        declare_check("fma", fma(2.0, 3.0, 1.0) == 7.0);
        {
                /*
                        fma is one rounding and not two, which is the whole
                        of why it exists. 0.1 times ten is a hair above one
                        and the plain product rounds it down to exactly one,
                        so the plain expression is zero and the fused one is
                        the hair. The operands are volatile because otherwise
                        the compiler works it out itself and the test
                        measures gcc. The hair is exactly two to the minus
                        fifty fourth, so the answer is written as a hex
                        float and not as a tolerance. Comparing against the
                        plain product instead would measure the contraction
                        setting rather than fma: arm64 and riscv64 fuse
                        `a * b - c` into one instruction at -O2 by default,
                        so the plain expression is not plain there.
                */
                volatile double tenth = 0.1;
                volatile double ten = 10.0;

                declare_check("fma rounds once",
                              fma(tenth, ten, -1.0) == 0x1p-54);
        }

        declare_check("sqrtf", sqrtf(144.0f) == 12.0f);
        declare_check("fabsf", fabsf(-3.5f) == 3.5f);
        declare_check("truncf", truncf(-2.5f) == -2.0f);
        declare_check("floorf", floorf(-2.5f) == -3.0f);
        declare_check("ceilf", ceilf(-2.5f) == -2.0f);
        declare_check("roundf", roundf(-2.5f) == -3.0f);
        declare_check("copysignf", copysignf(3.0f, -1.0f) == -3.0f);
        declare_check("fminf", fminf(2.0f, 3.0f) == 2.0f);
        declare_check("fmaxf", fmaxf(2.0f, 3.0f) == 3.0f);

        /*
                The non-local jump, whose buffer is the one type in this file
                the two builds cannot spell the same way: jump_state here is
                thirty two slots and glibc's jmp_buf is a struct. Both decay
                to a pointer at the call, so only the name differs.
        */
        {
                declare_jump_state mark;
                volatile int arrivals = 0;
                volatile int answer;

                answer = setjmp(mark);
                arrivals++;

                if (answer == 0)
                        longjmp(mark, 7);

                declare_check("setjmp first arrival is zero", arrivals == 2);
                declare_check("longjmp value arrives", answer == 7);
        }
        {
                declare_jump_state mark;
                volatile int answer;

                answer = _setjmp(mark);

                if (answer == 0)
                        _longjmp(mark, 0);

                declare_check("longjmp zero becomes one", answer == 1);
        }

#ifdef DECLARE_FREESTANDING
        /*
                And the thing no real library has an opinion about: that
                the standard name and the prose name are one address rather
                than two functions that happen to agree. This is what
                ASM_ALIAS promises, what a forwarding wrapper would quietly
                not deliver, and the reason declare.c is prototypes and never
                macros -- a macro has no address to take.

                Each goes through declare_same, whose two volatile locals are
                not decoration. gcc is entitled to assume that two functions
                it has separate declarations for are two different objects,
                and it folds the comparison to false without ever asking the
                linker. It does that for strdup and string_duplicate, which
                ARE one address -- nm says 40abb5 for both -- because
                string_duplicate has a body in this translation unit and the
                .set that renames it is inside a string literal the compiler
                does not read. Storing each address through volatile puts the
                question to the link, where the answer is.

                Which is worth knowing beyond this test: ASM_ALIAS over a C
                body is invisible to the compiler in a way
                __attribute__((alias)) -- what allocator.c uses for malloc --
                is not.
        */
        /*
                memcpy and memmove are no longer one address.

                They were one routine until the kernel audit measured what the
                shared body cost: memory_copy tests for overlap first, and
                above thirty two bytes hands the work to the same rep movsb
                the architecture's own memcpy uses -- so both ran the same
                instruction and ours paid six extra to reach it, a flat 0.90x
                from thirty three bytes to sixteen megabytes.

                memcpy is memory_copy_apart now, which is what C already says
                about it; memmove keeps memory_copy, the body that promises
                overlap. The behavioural check below, which slides a block
                over itself both ways, is what proves memmove kept that
                promise. This pair only records that the two names stopped
                being one routine.
        */
        declare_same("memcpy is memory_copy_apart", memcpy, memory_copy_apart);
        declare_same("memmove is memory_copy", memmove, memory_copy);
        declare_same("memset is memory_fill", memset, memory_fill);
        declare_same("memcmp is memory_compare", memcmp, memory_compare);
        declare_same("bcmp is memory_compare", bcmp, memory_compare);
        declare_same("strlen is string_length", strlen, string_length);
        declare_same("strcmp is string_compare", strcmp, string_compare);
        declare_same("strchr is string_first_of", strchr, string_first_of);
        declare_same("strstr is string_search", strstr, string_search);
        declare_same("strncpy is string_copy_max", strncpy, string_copy_max);
        declare_same("toupper is byte_to_upper", toupper, byte_to_upper);
        declare_same("isdigit is byte_is_digit", isdigit, byte_is_digit);
        declare_same("sqrt is square_root", sqrt, square_root);
        declare_same("fabs is absolute", fabs, absolute);
        declare_same("floor is decimal_floor", floor, decimal_floor);
        declare_same("labs is llabs", labs, llabs);
        declare_same("atol is atoll", atol, atoll);
        declare_same("setjmp is _setjmp", setjmp, _setjmp);
        declare_same("setjmp is jump_mark", setjmp, jump_mark);
        declare_same("strdup is string_duplicate", strdup, string_duplicate);
        declare_same("strtok is string_token", strtok, string_token);
        declare_same("__memcpy is memcpy", __memcpy, memcpy);
#endif
}
