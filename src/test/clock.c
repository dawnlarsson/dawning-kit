#include "../compiler_memory.c"

/*
        <time.h> against the answers glibc gives.

        The calendar has one property that makes it awkward to test the way
        the rest of this tree is tested: there is no second implementation to
        keep beside it, because the C it replaced never existed. What exists
        instead is glibc, on the machine this is built on, and the way to use
        it is to run the same sweep through both and compare -- 73,000 days
        for the dense one, another 71,000 timestamps for the coarse one,
        thousands of deliberately out-of-range struct tm for mktime, and every
        specifier of strftime at every buffer size from zero to past the
        answer.

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

        Four places where this deliberately does not match glibc 2.44, all
        four checked by hand and all four believed to be the better answer:

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
        "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%H|%I|%j|%m|%M|%n|%p|%P|%r|%R|" \
        "%s|%S|%t|%T|%u|%U|%V|%w|%W|%x|%X|%y|%Y|%z|%%|%Q|%Ey|%OS|%h|"

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
        };
        static const bipolar moments[] = {0LL, 1234567890LL, -1234567890LL,
                                          951782400LL};
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

                        for (max = 0; max < 220; max++)
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
#define CLOCK_HASH_FORMAT 2258117547500551994ULL
#define CLOCK_HASH_BOUNDS 15613544422311794872ULL
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
        same((string_address) "asctime", clock_test_asctime(),
             CLOCK_HASH_ASCTIME);

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
