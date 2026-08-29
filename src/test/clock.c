#include "../compiler_memory.c"

/*
        <time.h> against the answers glibc gives.

        The calendar has one property that makes it awkward to test the way
        the rest of this tree is tested: there is no second implementation to
        keep beside it, because the C it replaced never existed. What exists
        instead is glibc, on the machine this is built on, and the way to use
        it is to run the same sweep through both and compare -- 73,000 days
        for the dense one, another 71,000 timestamps for the coarse one,
        thousands of deliberately out-of-range struct tm for mktime, every
        specifier of strftime at every buffer size from zero to past the
        answer, and then every specifier again crossed with every flag, nine
        widths and both modifiers.

        That is far too much output to keep in a source file, so what is kept
        is a hash of it. Each sweep below folds every field of every answer
        into one sixty four bit number, and the number glibc produced for the
        identical sweep is the constant it is compared against. A single wrong
        day in eighty thousand changes it.

        The sweeps between the two marker lines are the text that was compiled
        against glibc to produce those constants -- extracted from this file
        mechanically, under a shim that spells positive, bipolar and
        address_to for a hosted compiler, so the two sides cannot be running
        different loops. To regenerate them:

            awk '/^.\* SWEEPS-BEGIN \*.$/{f=1;next}
                 /^.\* SWEEPS-END \*.$/{f=0} f' src/test/clock.c

        goes between the shim and a main that prints each hash, that is built
        with a plain hosted gcc, and it is run as TZ=UTC LC_ALL=C. The full
        differential harness, which prints every line rather than hashing it,
        does the same extraction and is what found the disagreements listed
        below; neither belongs in the tree, because both need a glibc and
        nothing else here does.

        Eight places where this deliberately does not match glibc 2.44, all
        eight checked by hand and all eight believed to be the better answer.
        The first four are the calendar and strftime:

          - tm_zone is "UTC" and not "GMT". glibc's gmtime says GMT; musl says
            UTC; this computes Coordinated Universal Time and says so.
          - the ISO week-based year of a date whose Thursday falls in the year
            2147483648 is that number here and -2147483648 in glibc, which
            holds it in an int. %G and %g both follow it over.
          - %Y, %C and %G are unpadded, which is what glibc 2.44 does and is
            not what musl does; see the note in src/standard/clock.c.
          - localtime is gmtime, because there is no zoneinfo. glibc with
            TZ unset would disagree with that on any machine that is not in
            London, which is why the reference side was pinned to TZ=UTC.

        The other four are strptime, and three of the four are glibc defects
        rather than choices it made. Each of them is checked below by hand,
        against this answer, and each is kept out of the two hashed sweeps:

          - %E in front of a specifier reads that specifier here. In glibc it
            reads the number twice: "%Ey" against "20000229" consumes "2000"
            into tm_year, then falls into the plain %y arm without winding the
            input back and consumes "02" on top of it, answering the year
            2002 and a cursor six bytes in. Four digits and no more, without
            an era to select, is what the C locale means.
          - %O in front of a specifier likewise reads that specifier. glibc
            keeps a list of the specifiers %O may precede and refuses the
            rest, so %OY is a parse failure there and a year here -- and it
            also gets only one %O per call: the second one asks the locale for
            alternative digits, is told for the second time that there are
            none, and returns null instead of falling back the way the first
            one did. "%Od %OH %Om" parses nothing at all in glibc.
          - %P is %p. glibc's strftime writes "%P" and its strptime will not
            read it back, which is a hole rather than a decision.
          - the weekday of a date in January or February of the year zero.
            glibc computes it with a formula whose integer division goes the
            wrong way for a year before the first, and answers Sunday for
            0000-01-01, which was a Saturday. The corner table below has that
            date in it from the day this file was written.
*/

static positive failures;
static positive checks;

static fn same(string_address name, positive got, positive want)
{
        checks++;

        if (got == want)
                return;

        failures++;
        string_format(log, "  FAILED %s: got %p want %p\n", name, got, want);
}

static fn same_signed(string_address name, bipolar got, bipolar want)
{
        checks++;

        if (got == want)
                return;

        failures++;
        string_format(log, "  FAILED %s: got %b want %b\n", name, got, want);
}

static fn same_text(string_address name, string_address got, string_address want)
{
        checks++;

        if (!is_null(got) && string_compare(got, want) == 0)
                return;

        failures++;
        string_format(log, "  FAILED %s: got %s want %s\n", name,
                      is_null(got) ? (string_address) "(null)" : got, want);
}

static fn good(string_address name, bool held)
{
        checks++;

        if (held)
                return;

        failures++;
        string_format(log, "  FAILED %s\n", name);
}

/* SWEEPS-BEGIN */
/*
        One round of FNV-1a's multiply with an extra fold, which is enough to
        make a single changed field change the answer and is short enough to
        be written the same way twice without a mistake. It is not a hash
        anybody should rely on for anything else.
*/
static positive clock_test_stir(positive state, bipolar value)
{
        state ^= (positive)value;
        state *= 1099511628211ULL;
        state ^= state >> 29;

        return state;
}

static positive clock_test_stir_text(positive state, const char address_to text)
{
        while (address_to text != end)
        {
                state = clock_test_stir(state, (bipolar)(p8)(address_to text));
                text++;
        }

        return state;
}

static positive clock_test_broken(positive state, bipolar stamp)
{
        struct tm broken;
        time_t moment = (time_t)stamp;

        if (is_null(gmtime_r(address_of moment, address_of broken)))
                return clock_test_stir(state, -999999);

        state = clock_test_stir(state, stamp);
        state = clock_test_stir(state, (bipolar)broken.tm_year + 1900);
        state = clock_test_stir(state, broken.tm_mon);
        state = clock_test_stir(state, broken.tm_mday);
        state = clock_test_stir(state, broken.tm_hour);
        state = clock_test_stir(state, broken.tm_min);
        state = clock_test_stir(state, broken.tm_sec);
        state = clock_test_stir(state, broken.tm_wday);
        state = clock_test_stir(state, broken.tm_yday);

        return state;
}

/*
        Every day from 1900-01-01 to 2100-01-01, with the time of day walking
        round the clock as the days go by so that the hour, minute and second
        arithmetic is swept too rather than being zero eighty thousand times.
*/
static positive clock_test_dense(void)
{
        positive state = 1469598103934665603ULL;
        bipolar day;

        for (day = -25567; day <= 47482; day++)
        {
                positive spin = (positive)day * 7919ULL;

                state = clock_test_broken(state, day * 86400 +
                                                         (bipolar)(spin % 86400ULL));
        }

        return state;
}

/*
        And the same again from about the year -1993 to the last second of
        9999, which is the sweep that actually reaches the negative branches
        of the two civil-date routines. The dense one above does not: 1900 is
        still six hundred thousand days after the start of the era the
        arithmetic counts from, so its sign guards never fire.
*/
static positive clock_test_coarse(void)
{
        positive state = 1469598103934665603ULL;
        bipolar stamp;

        for (stamp = -125000000000LL; stamp < 253402300800LL; stamp += 5270401LL)
                state = clock_test_broken(state, stamp);

        return state;
}

static const b32 clock_test_years[] = {-1900, -1000, -1, 0, 69, 70,
                                       100,   120,   200, 8099};
static const b32 clock_test_months[] = {-25, -13, -12, -1, 0,  1,
                                        2,   11,  12,  13, 25, 400};
static const b32 clock_test_days_of[] = {-400, -31, -1,  0,   1,   28, 29,
                                         30,   31,  32,  60,  366, 1000};
static const b32 clock_test_hours[] = {-25, -1, 0, 1, 12, 23, 24, 100};
static const b32 clock_test_minutes[] = {-61, -1, 0, 30, 59, 60, 61, 1000};
static const b32 clock_test_seconds[] = {-61, -1, 0, 30, 59, 60, 61, 3700};

static positive clock_test_one_normalise(positive state, b32 year, b32 month,
                                         b32 day, b32 hour, b32 minute,
                                         b32 second, bool through_mktime)
{
        struct tm broken;
        time_t got;

        broken.tm_year = year;
        broken.tm_mon = month;
        broken.tm_mday = day;
        broken.tm_hour = hour;
        broken.tm_min = minute;
        broken.tm_sec = second;
        broken.tm_wday = 99;
        broken.tm_yday = 99;
        broken.tm_isdst = 0;

        got = through_mktime ? mktime(address_of broken) : timegm(address_of broken);

        state = clock_test_stir(state, (bipolar)got);
        state = clock_test_stir(state, broken.tm_year);
        state = clock_test_stir(state, broken.tm_mon);
        state = clock_test_stir(state, broken.tm_mday);
        state = clock_test_stir(state, broken.tm_hour);
        state = clock_test_stir(state, broken.tm_min);
        state = clock_test_stir(state, broken.tm_sec);
        state = clock_test_stir(state, broken.tm_wday);
        state = clock_test_stir(state, broken.tm_yday);

        return state;
}

/*
        mktime and timegm are required to take fields that are out of range
        and answer the right second anyway, and to write the in-range spelling
        of that second back through the caller's pointer. A round trip through
        gmtime cannot see whether the second half happens, because gmtime only
        ever hands back a structure that is already normal -- so every input
        here is deliberately wrong in at least one field.
*/
static positive clock_test_normalise(void)
{
        positive state = 1469598103934665603ULL;
        positive year, month, day, hour, minute, second;

        for (year = 0; year < sizeof(clock_test_years) / sizeof(b32); year++)
                for (month = 0; month < sizeof(clock_test_months) / sizeof(b32);
                     month++)
                        for (day = 0;
                             day < sizeof(clock_test_days_of) / sizeof(b32);
                             day++)
                        {
                                state = clock_test_one_normalise(
                                        state, clock_test_years[year],
                                        clock_test_months[month],
                                        clock_test_days_of[day], 23, 59, 61,
                                        false);
                                state = clock_test_one_normalise(
                                        state, clock_test_years[year],
                                        clock_test_months[month],
                                        clock_test_days_of[day], 5, 6, 7, true);
                        }

        for (year = 0; year < sizeof(clock_test_years) / sizeof(b32); year++)
                for (hour = 0; hour < sizeof(clock_test_hours) / sizeof(b32);
                     hour++)
                        for (minute = 0;
                             minute < sizeof(clock_test_minutes) / sizeof(b32);
                             minute++)
                                for (second = 0;
                                     second < sizeof(clock_test_seconds) /
                                                      sizeof(b32);
                                     second++)
                                        state = clock_test_one_normalise(
                                                state, clock_test_years[year],
                                                1, 15, clock_test_hours[hour],
                                                clock_test_minutes[minute],
                                                clock_test_seconds[second],
                                                false);

        return state;
}

#define CLOCK_TEST_EVERY_SPECIFIER \
        "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%H|%I|%j|%k|%l|%m|%M|%n|%p|%P|" \
        "%r|%R|%s|%S|%t|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%z|%%|%Q|%Ey|%OS|%h|"

/*
        Every specifier at once, over the whole range and then densely over
        the years either side of the epoch. %Z is not in the format: it is the
        one specifier this deliberately answers differently from glibc, and
        including it would drown the constant it is compared against.
*/
static positive clock_test_format(void)
{
        positive state = 1469598103934665603ULL;
        bipolar stamp;

        for (stamp = -125000000000LL; stamp < 253402300800LL;
             stamp += 194713891LL)
        {
                struct tm broken;
                time_t moment = (time_t)stamp;
                char written[512];

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        state = clock_test_stir(state, -999999);
                        continue;
                }

                state = clock_test_stir(
                        state,
                        (bipolar)strftime((void *)written, sizeof(written),
                                          CLOCK_TEST_EVERY_SPECIFIER,
                                          address_of broken));
                state = clock_test_stir_text(state, written);
        }

        for (stamp = -100000000LL; stamp < 100000000LL; stamp += 91711LL)
        {
                struct tm broken;
                time_t moment = (time_t)stamp;
                char written[512];

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        state = clock_test_stir(state, -999999);
                        continue;
                }

                state = clock_test_stir(
                        state,
                        (bipolar)strftime((void *)written, sizeof(written),
                                          CLOCK_TEST_EVERY_SPECIFIER,
                                          address_of broken));
                state = clock_test_stir_text(state, written);
        }

        return state;
}

/*
        The half of strftime that is not the specifiers.

        A result that will not fit, terminator included, must answer zero and
        must not have written a byte past max -- and max of zero must not write
        even the terminator, which is the case a generous buffer never reaches.
        Every byte past max is checked here for having been left alone, and
        both the answer and the contents are folded in at every size from
        nothing to past the longest result.

        One of the formats is decorated and one of the moments is before the
        common era, because the widest path through the writer is the one a
        negative number under a zero-padded width takes: the sign, then the
        run of zeros, then the digits, three separately bounded writes for one
        field. An undecorated format never reaches the second of them, so a
        sweep without one would leave the strictest promise this family makes
        resting on a proof rather than on a test.

        The bound is past the longest answer and not a round number: the
        every-specifier format is two hundred and twenty one bytes at its
        longest, so a sweep that stopped at two hundred and twenty would never
        once see it succeed.
*/
static positive clock_test_bounds(void)
{
        static const char address_to formats[] = {
                CLOCK_TEST_EVERY_SPECIFIER,
                "%Y-%m-%d %H:%M:%S",
                "",
                "%",
                "%%",
                "x",
                "%c",
                "%n%t%n",
                "no specifiers at all",
                "%010Y|%_10Y|%-10Y|%040F|%5z|%-6s|%^a|%#p|%3e|%03k|%_3l",
        };
        static const bipolar moments[] = {0LL, 1234567890LL, -1234567890LL,
                                          951782400LL, -62167219201LL};
        positive state = 1469598103934665603ULL;
        positive which, when, max, byte;

        for (which = 0; which < sizeof(formats) / sizeof(formats[0]); which++)
                for (when = 0; when < sizeof(moments) / sizeof(moments[0]);
                     when++)
                {
                        struct tm broken;
                        time_t moment = (time_t)moments[when];

                        if (is_null(gmtime_r(address_of moment,
                                             address_of broken)))
                                continue;

                        for (max = 0; max < 240; max++)
                        {
                                char written[256];
                                positive answered;

                                for (byte = 0; byte < sizeof(written); byte++)
                                        written[byte] = '@';

                                answered = (positive)strftime(
                                        (void *)written, max, formats[which],
                                        address_of broken);

                                state = clock_test_stir(state, (bipolar)max);
                                state = clock_test_stir(state,
                                                        (bipolar)answered);

                                if (answered)
                                        state = clock_test_stir_text(state,
                                                                     written);

                                for (byte = max; byte < sizeof(written); byte++)
                                        if (written[byte] != '@')
                                        {
                                                state = clock_test_stir(
                                                        state, -424242);
                                                break;
                                        }
                        }
                }

        return state;
}

/*
        Every specifier crossed with every flag, a range of widths and both
        modifiers.

        Ninety three specifier bytes -- every printable one, so that a
        directive nobody has ever heard of is swept as hard as one that works
        -- times fourteen combinations of the five flags, times nine widths,
        times three modifiers, at eight instants chosen for their corners.
        Something over a quarter of a million directives, which is the sweep
        that says a flag is not being quietly ignored, that a width pads the
        side it should with the byte it should, and that a modifier is refused
        in exactly the places glibc refuses it and nowhere else.

        %Z is left out for the reason it is left out of the sweep above: it is
        the one specifier this deliberately answers differently, and it would
        drown the constant the rest of them produce.
*/
static const char address_to clock_test_flags[] = {
        "", "-", "_", "0", "^", "#", "-^",
        "0#", "_^", "^#", "0_", "-0", "^0#", "#_"};

static const char address_to clock_test_widths[] = {
        "", "1", "2", "3", "5", "6", "10", "12", "31"};

static const char address_to clock_test_modifiers[] = {"", "E", "O"};

static positive clock_test_directive(positive state,
                                     const struct tm address_to broken,
                                     const char address_to format)
{
        char written[256];
        positive answered = (positive)strftime((void address_to)written,
                                               sizeof(written), format,
                                               broken);

        state = clock_test_stir(state, (bipolar)answered);

        /*
                A refusal leaves the buffer unspecified and the two sides are
                not obliged to have left the same bytes in it, so a refusal
                folds in its own marker and not the wreckage.
        */
        if (answered)
                return clock_test_stir_text(state, written);

        return clock_test_stir(state, -313131);
}

static positive clock_test_decorated(void)
{
        static const bipolar moments[] = {
                951825045LL, 0LL, -1LL, -62167219201LL, 1136073600LL,
                253402300799LL, -2203977600LL, 1451606400LL};
        positive state = 1469598103934665603ULL;
        positive when, spec, flag, width, modifier;

        for (when = 0; when < sizeof(moments) / sizeof(moments[0]); when++)
        {
                struct tm broken;
                time_t moment = (time_t)moments[when];

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        state = clock_test_stir(state, -999999);
                        continue;
                }

                for (spec = 0x21; spec < 0x7f; spec++)
                {
                        if (spec == 'Z')
                                continue;

                        for (flag = 0;
                             flag < sizeof(clock_test_flags) /
                                            sizeof(clock_test_flags[0]);
                             flag++)
                                for (width = 0;
                                     width < sizeof(clock_test_widths) /
                                                     sizeof(clock_test_widths[0]);
                                     width++)
                                        for (modifier = 0;
                                             modifier <
                                             sizeof(clock_test_modifiers) /
                                                     sizeof(clock_test_modifiers[0]);
                                             modifier++)
                                        {
                                                char format[16];
                                                positive at = 0;
                                                positive part;

                                                format[at++] = '%';

                                                for (part = 0;
                                                     clock_test_flags[flag][part];
                                                     part++)
                                                        format[at++] =
                                                                clock_test_flags[flag][part];

                                                for (part = 0;
                                                     clock_test_widths[width][part];
                                                     part++)
                                                        format[at++] =
                                                                clock_test_widths[width][part];

                                                for (part = 0;
                                                     clock_test_modifiers[modifier][part];
                                                     part++)
                                                        format[at++] =
                                                                clock_test_modifiers[modifier][part];

                                                format[at++] = (char)spec;
                                                format[at] = 0;

                                                state = clock_test_directive(
                                                        state,
                                                        address_of broken,
                                                        format);
                                        }
                }
        }

        return state;
}

/*
        Every day from 1965 to 2035, through the four counters that disagree
        with each other around the new year on purpose.

        %U counts weeks from the first Sunday, %W from the first Monday, %V
        from the first week with four days of the new year in it, and %G is
        the year that %V belongs to rather than the year the date is in -- so
        the second of January can be week 53 of the year before it and the
        thirty first of December can be week 1 of the year after it, and the
        three of them can be 00, 52 and 53 on the same day. That week either
        side of New Year is where a hand-written strftime is wrong, and this
        walks seventy of them, bare and then decorated.
*/
static positive clock_test_weeks(void)
{
        positive state = 1469598103934665603ULL;
        bipolar day;

        for (day = -1826; day <= 23741; day++)
        {
                struct tm broken;
                time_t moment = (time_t)(day * 86400 + 43200);

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        state = clock_test_stir(state, -999999);
                        continue;
                }

                state = clock_test_directive(
                        state, address_of broken,
                        "%G|%g|%V|%U|%W|%u|%w|%j|%C|%y|%Y|%a|%A|%F");
                state = clock_test_directive(
                        state, address_of broken,
                        "%-G|%_V|%03U|%^a|%#A|%5G|%05g|%-j|%_5V|%2C");
        }

        return state;
}

/*
        strptime, against the answers glibc gives, twice over.

        First a cross of every format worth writing against every input worth
        refusing -- fifty six formats by a hundred inputs, most of which do
        not match and are there for exactly that reason, because a parser is
        as much its refusals as its acceptances. A refusal folds in the
        refusal and nothing else: the standard leaves the structure
        unspecified after one, and the two sides are not obliged to have
        stopped writing in the same place.

        Then the round trip: every moment written out with strftime and read
        straight back in through the same format. strftime already agrees with
        glibc byte for byte, so the text going in is the same text on both
        sides and anything that comes out different is strptime's fault.

        What is deliberately not in either sweep is listed with the rest of
        the disagreements at the top of this file: %E and %O in front of a
        specifier glibc will not take them on, %P, and the year zero.
*/
static const char address_to clock_test_scan_formats[] = {
        "%Y-%m-%d", "%Y-%m-%d %H:%M:%S", "%d/%m/%Y", "%m/%d/%y", "%D", "%F",
        "%T", "%R", "%r", "%c", "%x", "%X", "%a %b %e %H:%M:%S %Y",
        "%A, %B %d, %Y", "%j %Y", "%Y %j", "%Y%m%d", "%Y%m%d%H%M%S",
        "%C%y-%m-%d", "%C %y", "%y", "%Y", "%I:%M %p", "%I:%M:%S %p",
        "%H:%M", "%s", "%Y-%m-%dT%H:%M:%S%z", "%u", "%w", "%U %W %V %G %g",
        "%b %d", "%B %d", "%h %d", "%e %b %Y", "%k:%M", "%l:%M %p",
        "%n%Y%t%m%t%d", "%%", "%Y-%m-%d%n", "%Z %Y", "%z",
        "%Y-%m-%d %H:%M:%S %z", "  %Y  %m  %d  ", "%Y.%m.%d", "%p %I",
        "%a", "%A", "%m", "%d", "%H", "%M", "%S", "%j", "%C", "%G%j"};

static const char address_to clock_test_scan_inputs[] = {
        "2000-02-29", "2000-02-29 11:50:45", "29/02/2000", "02/29/00",
        "2000-02-29", "11:50:45", "11:50", "11:50:45 AM",
        "Tue Feb 29 11:50:45 2000", "Tuesday, February 29, 2000",
        "060 2000", "2000 060", "20000229", "20000229115045",
        "19 70", "0001-01-01", "00", "2000", "11:50 PM", "11:50:45 pm",
        "951825045", "2000-02-29T11:50:45+0100", "7", "0",
        "09 09 09 2000 00", "Feb 29", "February 29", "feb 29",
        "29 Feb 2000", " 5:06", " 5:06 am", "\n2000\t02\t29", "%",
        "2000-02-29\n", "UTC 2000", "-0530", "2000-02",
        "2000-02-29 11:50:45 -08:00", "  2000  02  29  ", "2000.02.29",
        "PM 11", "Sun", "Sunday", "12", "31", "23", "59", "60", "366", "20",
        "", "x", "1", "312", "0102", "13", "32", "  7", "7x",
        "SUNDAY", "sunday", "SuNdAy", "Sun day", "Junk", "+05", "+0530",
        "+05:30", "Z", "-2400", "+2401", "+0560", "2000-2-9",
        "2000-02-29 25:00:00", "1969-12-31", "9999-12-31",
        "  Feb  29", "\t2000", "2000 ", " 2000", "68", "69", "70",
        "951825045junk", "1e5", "2000-02-30", "-1", "  ", "AM", "am", "pm",
        "P", "A", "1:2:3", "001:02:03", "23:59:60", "23:59:61", "23:59:62"};

static positive clock_test_scan_one(positive state, const char address_to input,
                                    const char address_to format)
{
        struct tm broken;
        char address_to answered;

        /*
                Nothing is cleared, on purpose: strptime does not clear the
                structure and a caller who did not clear it first sees what
                was there. A recognisable value in every field is what makes
                "did not touch this one" visible in the hash.
        */
        broken.tm_sec = -77;
        broken.tm_min = -77;
        broken.tm_hour = -77;
        broken.tm_mday = -77;
        broken.tm_mon = -77;
        broken.tm_year = -77;
        broken.tm_wday = -77;
        broken.tm_yday = -77;
        broken.tm_isdst = -77;
        broken.tm_gmtoff = -77;
        broken.tm_zone = 0;

        answered = (char address_to)strptime(input, format, address_of broken);

        state = clock_test_stir_text(state, format);
        state = clock_test_stir_text(state, input);

        if (is_null(answered))
                return clock_test_stir(state, -212121);

        state = clock_test_stir(state, (bipolar)(answered - input));
        state = clock_test_stir(state, broken.tm_year);
        state = clock_test_stir(state, broken.tm_mon);
        state = clock_test_stir(state, broken.tm_mday);
        state = clock_test_stir(state, broken.tm_hour);
        state = clock_test_stir(state, broken.tm_min);
        state = clock_test_stir(state, broken.tm_sec);
        state = clock_test_stir(state, broken.tm_wday);
        state = clock_test_stir(state, broken.tm_yday);
        state = clock_test_stir(state, (bipolar)broken.tm_gmtoff);

        return state;
}

static positive clock_test_scan(void)
{
        positive state = 1469598103934665603ULL;
        positive which, when;

        for (which = 0; which < sizeof(clock_test_scan_formats) /
                                        sizeof(clock_test_scan_formats[0]);
             which++)
                for (when = 0; when < sizeof(clock_test_scan_inputs) /
                                              sizeof(clock_test_scan_inputs[0]);
                     when++)
                        state = clock_test_scan_one(
                                state, clock_test_scan_inputs[when],
                                clock_test_scan_formats[which]);

        return state;
}

static const char address_to clock_test_round_formats[] = {
        "%Y-%m-%d %H:%M:%S", "%a %b %e %H:%M:%S %Y", "%D %T", "%F %T",
        "%m/%d/%y %I:%M:%S %p", "%Y %j %H %M %S", "%C%y%m%d", "%s",
        "%A %B %d %Y", "%e %b %Y %k:%M", "%Y-%m-%dT%H:%M:%S%z",
        "%j", "%Y", "%y", "%u %w", "%H %I %p", "%G %V %U %W"};

static positive clock_test_scan_round(void)
{
        positive state = 1469598103934665603ULL;
        bipolar stamp;
        positive which;

        for (stamp = 0; stamp < 4102444800LL; stamp += 5270401LL)
                for (which = 0;
                     which < sizeof(clock_test_round_formats) /
                                     sizeof(clock_test_round_formats[0]);
                     which++)
                {
                        struct tm broken;
                        time_t moment = (time_t)stamp;
                        char written[256];

                        if (is_null(gmtime_r(address_of moment,
                                             address_of broken)))
                                continue;

                        if (strftime((void address_to)written, sizeof(written),
                                     clock_test_round_formats[which],
                                     address_of broken) == 0)
                                continue;

                        state = clock_test_scan_one(
                                state, written,
                                clock_test_round_formats[which]);
                }

        return state;
}

/*
        asctime's twenty six bytes, over a range wide enough to reach the
        years it cannot spell inside them and has to refuse.
*/
static positive clock_test_asctime(void)
{
        positive state = 1469598103934665603ULL;
        bipolar stamp;

        for (stamp = -62167219200LL; stamp < 253402300800LL;
             stamp += 1943713891LL)
        {
                struct tm broken;
                time_t moment = (time_t)stamp;
                char written[64];

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        state = clock_test_stir(state, -999999);
                        continue;
                }

                if (is_null(asctime_r(address_of broken, (void *)written)))
                {
                        state = clock_test_stir(state, -111111);
                        continue;
                }

                state = clock_test_stir_text(state, written);
        }

        return state;
}
/* SWEEPS-END */

/*
        The constants the sweeps above produced under glibc 2.44 on x86_64,
        with TZ=UTC and LC_ALL=C, from the same source text.
*/
#define CLOCK_HASH_DENSE 17294647340756726478ULL
#define CLOCK_HASH_COARSE 511831104147379237ULL
#define CLOCK_HASH_NORMALISE 9235080613808746534ULL
#define CLOCK_HASH_FORMAT 5799989417746424739ULL
#define CLOCK_HASH_BOUNDS 18209079861022630276ULL
#define CLOCK_HASH_DECORATED 2088967578927533204ULL
#define CLOCK_HASH_WEEKS 5920955691035123170ULL
#define CLOCK_HASH_SCAN 5430387038418749550ULL
#define CLOCK_HASH_SCAN_ROUND 15503327748273598656ULL
#define CLOCK_HASH_ASCTIME 15797585760646486659ULL

/*
        A hash says something changed and says nothing about what. These say
        what: one date from each of the corners the arithmetic has, spelled
        out, so that a failure is readable before the sweeps are reached.
*/
typedef struct clock_test_date
{
        bipolar stamp;
        bipolar year;
        b32 month;
        b32 day;
        b32 hour;
        b32 minute;
        b32 second;
        b32 weekday;
        b32 day_of_year;
} clock_test_date;

static const clock_test_date clock_test_corners[] = {
        {0LL, 1970, 0, 1, 0, 0, 0, 4, 0},
        {-1LL, 1969, 11, 31, 23, 59, 59, 3, 364},
        {86399LL, 1970, 0, 1, 23, 59, 59, 4, 0},
        {86400LL, 1970, 0, 2, 0, 0, 0, 5, 1},
        {-86400LL, 1969, 11, 31, 0, 0, 0, 3, 364},
        {951782400LL, 2000, 1, 29, 0, 0, 0, 2, 59},
        {-2203977600LL, 1900, 1, 28, 0, 0, 0, 3, 58},
        {-2203977601LL, 1900, 1, 27, 23, 59, 59, 2, 57},
        {-2203891200LL, 1900, 2, 1, 0, 0, 0, 4, 59},
        {-2203891201LL, 1900, 1, 28, 23, 59, 59, 3, 58},
        {4107542400LL, 2100, 2, 1, 0, 0, 0, 1, 59},
        {4107542399LL, 2100, 1, 28, 23, 59, 59, 0, 58},
        {-11670998400LL, 1600, 1, 29, 0, 0, 0, 2, 59},
        {-62167219200LL, 0, 0, 1, 0, 0, 0, 6, 0},
        {-62162035200LL, 0, 2, 1, 0, 0, 0, 3, 60},
        {-62162035201LL, 0, 1, 29, 23, 59, 59, 2, 59},
        {-62167219201LL, -1, 11, 31, 23, 59, 59, 5, 364},
        {-62198755200LL, -1, 0, 1, 0, 0, 0, 5, 0},
        {-62135596800LL, 1, 0, 1, 0, 0, 0, 1, 0},
        {253402300799LL, 9999, 11, 31, 23, 59, 59, 5, 364},
        {2147483647LL, 2038, 0, 19, 3, 14, 7, 2, 18},
};
static fn check_corners(void)
{
        positive at;

        for (at = 0; at < sizeof(clock_test_corners) / sizeof(clock_test_date);
             at++)
        {
                const clock_test_date address_to want = clock_test_corners + at;
                time_t moment = (time_t)want->stamp;
                struct tm broken;
                time_t back;

                if (is_null(gmtime_r(address_of moment, address_of broken)))
                {
                        checks++;
                        failures++;
                        string_format(log, "  FAILED gmtime refused %b\n",
                                      want->stamp);
                        continue;
                }

                same_signed((string_address) "corner year",
                            (bipolar)broken.tm_year + 1900, want->year);
                same_signed((string_address) "corner month", broken.tm_mon,
                            want->month);
                same_signed((string_address) "corner day", broken.tm_mday,
                            want->day);
                same_signed((string_address) "corner hour", broken.tm_hour,
                            want->hour);
                same_signed((string_address) "corner minute", broken.tm_min,
                            want->minute);
                same_signed((string_address) "corner second", broken.tm_sec,
                            want->second);
                same_signed((string_address) "corner weekday", broken.tm_wday,
                            want->weekday);
                same_signed((string_address) "corner day of year",
                            broken.tm_yday, want->day_of_year);

                back = timegm(address_of broken);
                same_signed((string_address) "corner round trip",
                            (bipolar)back, want->stamp);
        }
}

static fn check_format_strings(void)
{
        struct tm broken;
        time_t moment;
        p8 written[128];

        moment = 951825045;                     /* 2000-02-29 11:50:45 UTC */
        gmtime_r(address_of moment, address_of broken);

        strftime(written, sizeof(written), "%Y-%m-%d %H:%M:%S", address_of broken);
        same_text((string_address) "%F-alike", written,
                  (string_address) "2000-02-29 11:50:45");

        strftime(written, sizeof(written), "%a %A %b %B", address_of broken);
        same_text((string_address) "names", written,
                  (string_address) "Tue Tuesday Feb February");

        strftime(written, sizeof(written), "%c", address_of broken);
        same_text((string_address) "%c", written,
                  (string_address) "Tue Feb 29 11:50:45 2000");

        strftime(written, sizeof(written), "%x %X %D %T %R %r", address_of broken);
        same_text((string_address) "compound", written,
                  (string_address) "02/29/00 11:50:45 02/29/00 11:50:45 "
                                   "11:50 11:50:45 AM");

        strftime(written, sizeof(written), "%j %U %W %V %G %u %w %C %y %e",
                 address_of broken);
        same_text((string_address) "counters", written,
                  (string_address) "060 09 09 09 2000 2 2 20 00 29");

        strftime(written, sizeof(written), "%s %z %Z %p %P %%%Q", address_of broken);
        same_text((string_address) "the rest", written,
                  (string_address) "951825045 +0000 UTC AM am %%Q");

        written[0] = '@';
        same((string_address) "max of zero writes nothing",
             strftime(written, 0, "abc", address_of broken), 0);
        same((string_address) "not even a terminator", (positive)written[0],
             (positive)'@');

        /*
                Three bytes of room for three bytes of answer is not enough,
                because the terminator is the fourth. The answer is zero and
                what is left in the buffer is unspecified -- both this and
                glibc leave the three bytes there -- but the byte at max must
                not have been touched, and that is what is checked.
        */
        written[3] = '@';
        same((string_address) "a result that just fits is refused",
             strftime(written, 3, "abc", address_of broken), 0);
        same((string_address) "and wrote nothing at max", (positive)written[3],
             (positive)'@');

        same((string_address) "a result that fits with its terminator",
             strftime(written, 4, "abc", address_of broken), 3);
        same_text((string_address) "and is the result", written,
                  (string_address) "abc");

        moment = -62167219201LL;                /* -0001-12-31 23:59:59 */
        gmtime_r(address_of moment, address_of broken);
        strftime(written, sizeof(written), "%Y|%F|%C|%G|%g|%y", address_of broken);
        same_text((string_address) "a year before the common era", written,
                  (string_address) "-1|-1-12-31|-1|-1|99|99");
}

/*
        The flags, the widths and the two modifiers, spelled out.

        Every expected string below was read off glibc 2.44 under TZ=UTC and
        LC_ALL=C before it was written down, and every line is one of the
        places where the answer is not what a first reading of the manual
        would have guessed. A hash says something moved; these say what.
*/
static fn check_format_flags(void)
{
        struct tm broken;
        time_t moment = 951800767;              /* 2000-02-29 05:06:07 UTC */
        p8 written[256];

        gmtime_r(address_of moment, address_of broken);

        strftime(written, sizeof(written), "%H|%k|%I|%l|%e|%_d|%-d|%0e|%-e",
                 address_of broken);
        same_text((string_address) "the space padded fields", written,
                  (string_address) "05| 5|05| 5|29|29|29|29|29");

        strftime(written, sizeof(written), "%5d|%05d|%-5d|%_5d|%3j|%-j|%_j",
                 address_of broken);
        same_text((string_address) "a width and the three padding flags",
                  written, (string_address) "00029|00029|   29|   29|060|60| 60");

        /*
                ^ folds up and # folds the other way from usual, and "usual"
                is decided by the specifier: a name is capitalised so # makes
                it shout, and %p is already shouting so # makes it quiet. %P
                is lower case whatever the flags say.
        */
        strftime(written, sizeof(written), "%^a|%#a|%^A|%#B|%^p|%#p|%^P|%#P",
                 address_of broken);
        same_text((string_address) "the two case flags", written,
                  (string_address) "TUE|TUE|TUESDAY|FEBRUARY|AM|am|am|am");

        strftime(written, sizeof(written), "%^c", address_of broken);
        same_text((string_address) "a compound folds once, as a whole",
                  written, (string_address) "TUE FEB 29 05:06:07 2000");

        strftime(written, sizeof(written), "%012F|%-12F|%_12F",
                 address_of broken);
        same_text((string_address) "a compound pads once, as a whole", written,
                  (string_address) "002000-02-29|  2000-02-29|  2000-02-29");

        /*
                The offset is one four digit number and not two of two, which
                only shows once a width is named: the five in "%5z" widens the
                sign and then widens all four digits again.
        */
        strftime(written, sizeof(written), "%z|%5z|%-5z|%_z|%0z",
                 address_of broken);
        same_text((string_address) "the offset and its width", written,
                  (string_address) "+0000|    +00000|    +    0|+   0|+0000");

        strftime(written, sizeof(written),
                 "%Ec|%EC|%Ex|%EX|%Ey|%EY|%Ep|%Eu|%Ez", address_of broken);
        same_text((string_address) "the era modifier where it is taken",
                  written,
                  (string_address) "Tue Feb 29 05:06:07 2000|20|02/29/00|"
                                   "05:06:07|00|2000|AM|2|+0000");

        strftime(written, sizeof(written),
                 "%Od|%Oe|%OH|%OS|%Ou|%OV|%Om|%Oy|%Ob", address_of broken);
        same_text((string_address) "the digits modifier where it is taken",
                  written, (string_address) "29|29|05|07|2|09|02|00|Feb");

        /*
                And where it is not taken, which is a table with no rule in
                it: %EC is a directive and %Eb is not, %OS is one and %ES is
                not. A refused modifier is not an error, it is a directive
                nobody knows, and it comes back out as the text it was.
        */
        strftime(written, sizeof(written),
                 "%Ea|%Ed|%ES|%OY|%Ox|%Oc|%Eb|%EV", address_of broken);
        same_text((string_address) "a modifier the specifier will not take",
                  written,
                  (string_address) "%Ea|%Ed|%ES|%OY|%Ox|%Oc|%Eb|%EV");

        // The width goes before the modifier and not after it, and getting
        // that backwards is not a warning, it is a different answer.
        strftime(written, sizeof(written), "%E5Y|%5EY|%O5d|%5OS",
                 address_of broken);
        same_text((string_address) "the width goes before the modifier",
                  written, (string_address) "%E5Y|02000|%O5d|00007");

        // An unknown directive comes back out through the same path a name
        // does, so it is folded and padded on the way.
        strftime(written, sizeof(written), "%^q|%5Q|%05q|%-q|%^Q",
                 address_of broken);
        same_text((string_address) "an unknown directive is decorated too",
                  written, (string_address) "%^Q|  %5Q|0%05q|%-q|%^Q");

        // %s is the one number that does not pad its own digits, so a width
        // in front of it spaces rather than zeroes and its zeros, when asked
        // for, land in front of the sign rather than behind it.
        strftime(written, sizeof(written), "%12s|%020s|%-12s|%_12s",
                 address_of broken);
        same_text((string_address) "the seconds since the epoch", written,
                  (string_address) "   951800767|00000000000951800767|"
                                   "   951800767|   951800767");

        strftime(written, sizeof(written), "%%|%5%|%05%|%n|%3n|%t|%3t",
                 address_of broken);
        same_text((string_address) "the three that are not fields at all",
                  written, (string_address) "%|    %|0000%|\n|  \n|\t|  \t");

        moment = -62167219201LL;                /* -0001-12-31 23:59:59 */
        gmtime_r(address_of moment, address_of broken);

        strftime(written, sizeof(written),
                 "%Y|%5Y|%05Y|%_5Y|%-5Y|%3G|%2C|%C|%G|%g|%y", address_of broken);
        same_text((string_address) "a negative year takes its sign first",
                  written, (string_address) "-1|-0001|-0001|   -1|   -1|-01|"
                                            "-1|-1|-1|99|99");

        strftime(written, sizeof(written), "%k|%l|%e|%3e|%_3d",
                 address_of broken);
        same_text((string_address) "the last hour of the year before the era",
                  written, (string_address) "23|11|31| 31| 31");

        moment = 0;
        gmtime_r(address_of moment, address_of broken);

        strftime(written, sizeof(written), "%k|%l|%I|%p|%P|%e|%s|%5s|%-5s|%05s",
                 address_of broken);
        same_text((string_address) "midnight at the epoch", written,
                  (string_address) " 0|12|12|AM|am| 1|0|    0|    0|00000");
}

/*
        mktime and timegm are required to take fields that are outside their
        ranges and answer the right second anyway, and then to write the
        in-range spelling of that second back through the caller's pointer.
        The thirteenth month is January of the next year, month minus one is
        December of the year before, day zero is the last day of the month
        before, and an hour of twenty five is one in the morning of the day
        after. The hashed sweep walks thousands of these against glibc; these
        five are the ones anybody would name.
*/
static fn check_normalise(void)
{
        static const clock_test_date wanted[] = {
                {979516800LL, 2001, 0, 15, 0, 0, 0, 1, 14},
                {945216000LL, 1999, 11, 15, 0, 0, 0, 3, 348},
                {951782400LL, 2000, 1, 29, 0, 0, 0, 2, 59},
                {946774800LL, 2000, 0, 2, 1, 0, 0, 0, 1},
                {946684799LL, 1999, 11, 31, 23, 59, 59, 5, 364},
        };
        static const b32 given[5][6] = {
                {100, 12, 15, 0, 0, 0},
                {100, -1, 15, 0, 0, 0},
                {100, 2, 0, 0, 0, 0},
                {100, 0, 1, 25, 0, 0},
                {100, 0, 1, 0, 0, -1},
        };
        positive at;

        for (at = 0; at < sizeof(wanted) / sizeof(clock_test_date); at++)
        {
                struct tm broken;
                time_t answered;

                broken.tm_year = given[at][0];
                broken.tm_mon = given[at][1];
                broken.tm_mday = given[at][2];
                broken.tm_hour = given[at][3];
                broken.tm_min = given[at][4];
                broken.tm_sec = given[at][5];
                broken.tm_wday = 99;
                broken.tm_yday = 99;
                broken.tm_isdst = 0;

                answered = timegm(address_of broken);

                same_signed((string_address) "normalised second",
                            (bipolar)answered, wanted[at].stamp);
                same_signed((string_address) "normalised year",
                            (bipolar)broken.tm_year + 1900, wanted[at].year);
                same_signed((string_address) "normalised month", broken.tm_mon,
                            wanted[at].month);
                same_signed((string_address) "normalised day", broken.tm_mday,
                            wanted[at].day);
                same_signed((string_address) "normalised hour", broken.tm_hour,
                            wanted[at].hour);
                same_signed((string_address) "normalised minute", broken.tm_min,
                            wanted[at].minute);
                same_signed((string_address) "normalised second of the minute",
                            broken.tm_sec, wanted[at].second);
                same_signed((string_address) "normalised weekday",
                            broken.tm_wday, wanted[at].weekday);
                same_signed((string_address) "normalised day of the year",
                            broken.tm_yday, wanted[at].day_of_year);
        }
}

/*
        strptime, spelled out, with a pointer answer to check as well as the
        fields: it returns where it stopped, and where it stopped is half of
        what a caller uses it for.
*/
static fn check_scan(void)
{
        struct tm broken;
        p8 address_to rest;

        broken.tm_isdst = 0;
        broken.tm_gmtoff = 0;
        broken.tm_zone = null;

        rest = strptime("2000-02-29 11:50:45", "%Y-%m-%d %H:%M:%S",
                        address_of broken);
        good((string_address) "strptime read a whole timestamp",
             !is_null(rest));
        same_signed((string_address) "scanned year",
                    (bipolar)broken.tm_year + 1900, 2000);
        same_signed((string_address) "scanned month", broken.tm_mon, 1);
        same_signed((string_address) "scanned day", broken.tm_mday, 29);
        same_signed((string_address) "scanned hour", broken.tm_hour, 11);
        same_signed((string_address) "scanned minute", broken.tm_min, 50);
        same_signed((string_address) "scanned second", broken.tm_sec, 45);
        same_signed((string_address) "derived weekday", broken.tm_wday, 2);
        same_signed((string_address) "derived day of the year", broken.tm_yday,
                    59);
        same_text((string_address) "and stopped at the end", rest,
                  (string_address) "");

        // It answers where it stopped and does not care that there is more.
        rest = strptime("2000-02-29 and more", "%Y-%m-%d", address_of broken);
        same_text((string_address) "strptime stops where the format does",
                  rest, (string_address) " and more");

        good((string_address) "strptime refuses what does not match",
             is_null(strptime("not a date", "%Y-%m-%d", address_of broken)));

        // Longest name first, and case does not matter.
        broken.tm_wday = -1;
        rest = strptime("TUESDAY afternoon", "%A", address_of broken);
        same_signed((string_address) "a name in the wrong case",
                    broken.tm_wday, 2);
        same_text((string_address) "and the longest one that fits", rest,
                  (string_address) " afternoon");

        broken.tm_wday = -1;
        rest = strptime("Tues", "%A", address_of broken);
        same_signed((string_address) "the short name where the long one fails",
                    broken.tm_wday, 2);
        same_text((string_address) "leaves what it did not use", rest,
                  (string_address) "s");

        // A number skips whatever whitespace is in front of it, whether or
        // not the format asked for any.
        rest = strptime(" 5:06", "%H:%M", address_of broken);
        same_signed((string_address) "a number skips leading whitespace",
                    broken.tm_hour, 5);
        same_signed((string_address) "and reads what follows it",
                    broken.tm_min, 6);

        /*
                And it stops before it overflows the field rather than after,
                which is what lets two fields sit against each other with no
                separator: "0102" is the first of February and not month one
                hundred and two.
        */
        broken.tm_mon = -1;
        broken.tm_mday = -1;
        strptime("0102", "%m%d", address_of broken);
        same_signed((string_address) "two fields with nothing between them",
                    broken.tm_mon, 0);
        same_signed((string_address) "stop at the top of the range",
                    broken.tm_mday, 2);

        strptime("11:50 pm", "%I:%M %p", address_of broken);
        same_signed((string_address) "an afternoon on a twelve hour clock",
                    broken.tm_hour, 23);

        strptime("12:00 am", "%I:%M %p", address_of broken);
        same_signed((string_address) "and midnight on one", broken.tm_hour, 0);

        strptime("951825045", "%s", address_of broken);
        same_signed((string_address) "seconds since the epoch fill everything",
                    (bipolar)broken.tm_year + 1900, 2000);
        same_signed((string_address) "and the month with them", broken.tm_mon,
                    1);
        same_signed((string_address) "and the hour with them", broken.tm_hour,
                    11);

        broken.tm_gmtoff = -1;
        strptime("+05:30", "%z", address_of broken);
        same_signed((string_address) "an offset with a colon in it",
                    (bipolar)broken.tm_gmtoff, 19800);

        broken.tm_gmtoff = -1;
        strptime("-0530", "%z", address_of broken);
        same_signed((string_address) "an offset behind Greenwich",
                    (bipolar)broken.tm_gmtoff, -19800);

        broken.tm_gmtoff = -1;
        strptime("Z", "%z", address_of broken);
        same_signed((string_address) "the offset that is a letter",
                    (bipolar)broken.tm_gmtoff, 0);

        good((string_address) "sixty minutes is not an offset",
             is_null(strptime("+0060", "%z", address_of broken)));

        /*
                A day of the year on its own derives nothing, because a day of
                the year without a year is not a date. Put a year beside it
                and both the month and the weekday fall out.
        */
        broken.tm_mon = -1;
        broken.tm_mday = -1;
        broken.tm_wday = -1;
        strptime("060", "%j", address_of broken);
        same_signed((string_address) "a day of the year, stored",
                    broken.tm_yday, 59);
        same_signed((string_address) "and nothing derived from it alone",
                    broken.tm_mon, -1);

        strptime("060 2000", "%j %Y", address_of broken);
        same_signed((string_address) "with a year, the month falls out",
                    broken.tm_mon, 1);
        same_signed((string_address) "and the day of the month",
                    broken.tm_mday, 29);
        same_signed((string_address) "and the weekday", broken.tm_wday, 2);

        // %Z steps over a word and does nothing with it, which is why a
        // format that puts %Z in front of a date eats the date.
        rest = strptime("UTC 2000", "%Z %Y", address_of broken);
        same_signed((string_address) "a zone name is stepped over",
                    (bipolar)broken.tm_year + 1900, 2000);
        same_text((string_address) "and consumed to the end", rest,
                  (string_address) "");
        good((string_address) "and it eats a date if one is where it looks",
             is_null(strptime("2000", "%Z %Y", address_of broken)));

        /*
                The four places this does not agree with glibc, checked
                against this answer. See the list at the top of the file for
                why each of them is the answer here.
        */
        broken.tm_year = -1;
        rest = strptime("2000-02-29", "%Ey", address_of broken);
        same_signed((string_address) "%E reads the plain specifier",
                    broken.tm_year, 120);
        same_text((string_address) "and consumes what it read", rest,
                  (string_address) "00-02-29");

        broken.tm_year = -1;
        rest = strptime("2000-02", "%OY-%Om", address_of broken);
        good((string_address) "%O reads the plain specifier too",
             !is_null(rest));
        same_signed((string_address) "and the year with it", broken.tm_year,
                    100);

        broken.tm_hour = -1;
        strptime("11:50 PM", "%l:%M %P", address_of broken);
        same_signed((string_address) "%P is %p", broken.tm_hour, 23);

        broken.tm_wday = -1;
        strptime("0000-01-01", "%Y-%m-%d", address_of broken);
        same_signed((string_address) "the first of January of the year zero",
                    broken.tm_wday, 6);
}

static fn check_asctime(void)
{
        struct tm broken;
        time_t moment = 951825045;
        p8 written[64];

        gmtime_r(address_of moment, address_of broken);
        same_text((string_address) "asctime", asctime_r(address_of broken, written),
                  (string_address) "Tue Feb 29 11:50:45 2000\n");

        same_text((string_address) "ctime", ctime_r(address_of moment, written),
                  (string_address) "Tue Feb 29 11:50:45 2000\n");

        moment = -62167219200LL;                /* 0000-01-01 */
        gmtime_r(address_of moment, address_of broken);
        same_text((string_address) "asctime in the year zero",
                  asctime_r(address_of broken, written),
                  (string_address) "Sat Jan  1 00:00:00 0\n");

        broken.tm_wday = 9;
        good((string_address) "asctime refuses a weekday it cannot name",
             is_null(asctime_r(address_of broken, written)));

        gmtime_r(address_of moment, address_of broken);
        broken.tm_year = 90000;
        good((string_address) "asctime refuses a year it cannot fit",
             is_null(asctime_r(address_of broken, written)));
}

/*
        The four routines that ask the kernel rather than doing arithmetic.
        Nothing here can be compared against a constant, so each is compared
        against the others: they read the same clock and must agree, and the
        two that are supposed to move must move.
*/
static fn check_live(void)
{
        time_t first = time(null);
        time_t written = 0;
        time_t answered = time(address_of written);
        timeval wall = {0, 0};
        timespec real = {0, 0};
        timespec monotonic_first = {0, 0};
        timespec monotonic_second = {0, 0};
        timespec grain = {0, 0};
        timespec nap = {0, 20000000};
        clock_t burned_first;
        clock_t burned_second;
        volatile positive spin = 0;
        positive turn;
        tm address_to broken;
        tm copy;

        good((string_address) "time wrote through its argument",
             written == answered);
        good((string_address) "time is a plausible year", first > 1700000000);

        same((string_address) "gettimeofday answered",
             (positive)gettimeofday(address_of wall, null), 0);
        good((string_address) "gettimeofday agrees with time",
             wall.tv_sec - (b64)first <= 2 && (b64)first - wall.tv_sec <= 2);
        good((string_address) "gettimeofday microseconds are in range",
             wall.tv_usec >= 0 && wall.tv_usec < 1000000);

        same((string_address) "clock_gettime answered",
             (positive)clock_gettime(CLOCK_REALTIME, address_of real), 0);
        good((string_address) "clock_gettime agrees with time",
             (bipolar)real.tv_sec - (bipolar)first <= 2 &&
                     (bipolar)first - (bipolar)real.tv_sec <= 2);
        good((string_address) "clock_gettime nanoseconds are in range",
             real.tv_nsec < 1000000000);

        same((string_address) "monotonic answered",
             (positive)clock_gettime(CLOCK_MONOTONIC,
                                     address_of monotonic_first),
             0);
        sleep(address_of nap);
        same((string_address) "monotonic answered again",
             (positive)clock_gettime(CLOCK_MONOTONIC,
                                     address_of monotonic_second),
             0);
        good((string_address) "monotonic moved forward",
             (bipolar)monotonic_second.tv_sec * 1000000000 +
                             (bipolar)monotonic_second.tv_nsec >
                     (bipolar)monotonic_first.tv_sec * 1000000000 +
                             (bipolar)monotonic_first.tv_nsec);

        same((string_address) "clock_getres answered",
             (positive)clock_getres(CLOCK_REALTIME, address_of grain), 0);
        good((string_address) "clock_getres named a grain",
             grain.tv_sec > 0 || grain.tv_nsec > 0);

        burned_first = clock();
        good((string_address) "clock answered", burned_first >= 0);

        for (turn = 0; turn < 40000000; turn++)
                spin = spin + turn;

        burned_second = clock();
        good((string_address) "clock moved forward", burned_second > burned_first);

        broken = gmtime(address_of first);
        good((string_address) "gmtime answered", !is_null(broken));

        if (!is_null(broken))
        {
                copy = address_to broken;
                good((string_address) "timegm undoes gmtime",
                     timegm(address_of copy) == first);
                good((string_address) "localtime is gmtime here",
                     localtime(address_of first)->tm_hour == copy.tm_hour);
                good((string_address) "and says so",
                     broken->tm_gmtoff == 0 && broken->tm_isdst == 0);
        }

        same_signed((string_address) "difftime",
                    (bipolar)difftime((time_t)1000000, (time_t)3), 999997);
        same_signed((string_address) "difftime backwards",
                    (bipolar)difftime((time_t)3, (time_t)1000000), -999997);
}

b32 main(void)
{
        check_corners();
        check_format_strings();
        check_format_flags();
        check_normalise();
        check_scan();
        check_asctime();
        check_live();

        same((string_address) "every day from 1900 to 2100", clock_test_dense(),
             CLOCK_HASH_DENSE);
        same((string_address) "a coarse sweep of twelve thousand years",
             clock_test_coarse(), CLOCK_HASH_COARSE);
        same((string_address) "mktime and timegm normalising",
             clock_test_normalise(), CLOCK_HASH_NORMALISE);
        same((string_address) "every strftime specifier", clock_test_format(),
             CLOCK_HASH_FORMAT);
        same((string_address) "strftime at every buffer size",
             clock_test_bounds(), CLOCK_HASH_BOUNDS);
        same((string_address) "every specifier under every flag and width",
             clock_test_decorated(), CLOCK_HASH_DECORATED);
        same((string_address) "seventy new years through the week counters",
             clock_test_weeks(), CLOCK_HASH_WEEKS);
        same((string_address) "strptime against what it will and will not take",
             clock_test_scan(), CLOCK_HASH_SCAN);
        same((string_address) "strptime reading back what strftime wrote",
             clock_test_scan_round(), CLOCK_HASH_SCAN_ROUND);
        same((string_address) "asctime", clock_test_asctime(),
             CLOCK_HASH_ASCTIME);

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
