/*
        Experimental C standard library

        <time.h>: a clock read from the kernel, and a calendar with no tables

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_CLOCK
#define STANDARD_MODERN_C_STANDARD_CLOCK

/*
        Two builds this family deliberately does not join.

        A kernel build already has a calendar and already has these names:
        <linux/time.h> declares struct tm and time64_to_tm and mktime64, and a
        second struct tm in the same translation unit is not a conflict of
        opinion, it is a compile error. A module also has no business reaching
        for clock_gettime through a syscall trap -- ktime_get is what it wants
        -- so there is nothing here it could use even if the names were free.

        A no-platform build has had platform/syscall.inc compiled out from
        under it, so syscall(clock_gettime) is not a number and there is no
        kernel to ask the time of. The calendar arithmetic would still be
        valid there, but half a <time.h> that cannot say what o'clock it is
        would be a worse thing to offer than none.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        This is ordinary C on purpose.

        library.c and everything it includes holds declarations and assembly
        and nothing else, which is checked. Almost nothing here is a floor a
        machine could do better: the whole of <time.h> below the two syscalls
        is integer arithmetic over a calendar that Rome and then Pope Gregory
        chose, and no processor has an instruction for either of them. Writing
        it once as C is what lets the same bytes be tested on all three
        machines instead of being written three times and drifting.

        What it does need from the library is small and already there:
        system_call_2 for the two clock traps, memory_copy_apart for the
        pieces strftime assembles, positive_into_padded for a zero-filled
        field, and string_length. Nothing here calls printf or snprintf, so
        this file can be merged before or after the formatting family and in
        either order with the rest.

        Timezones, said plainly: this system has no zoneinfo, nothing reads
        /etc/localtime, and localtime is gmtime. Every broken-down time this
        file produces carries tm_isdst zero, tm_gmtoff zero, and tm_zone
        "UTC", because that is what it actually computed -- not because the
        machine happens to be in London. A program that needs a real local
        time will need a TZif parser, and when one exists the routine to
        change is clock_local_offset and the two that call it are mktime and
        localtime_r.
*/

/*
        The clock identifiers the kernel takes in its first argument. These
        are the same three numbers on x86_64, arm64 and riscv64 because they
        are not a syscall number, they are a kernel-wide enumeration.

        Guarded one at a time because src/sh/system.c and src/sh/net.c each
        define a CLOCK_MONOTONIC for themselves and include this umbrella; an
        identical redefinition is legal but only if the spelling matches, and
        a guard is cheaper than requiring it to.
*/
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID 2
#endif

#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

/*
        clock() counts in these, and the number is a fiction the standard
        fixed at a million on POSIX. It is not the resolution of anything.
*/
#define CLOCKS_PER_SEC ((clock_t)1000000)

#define CLOCK_SECONDS_PER_DAY 86400

typedef bipolar time_t;
typedef bipolar clock_t;
typedef bipolar suseconds_t;
typedef b32 clockid_t;

/*
        gettimeofday's pair, which is the older of the two shapes and the one
        BSD left behind. Signed, both fields, unlike library.c's timespec:
        that one is a duration handed to nanosleep and never negative, this
        one is a point on a line that started in 1970 and has an outside.
*/
typedef struct timeval
{
        b64 tv_sec;
        b64 tv_usec;
} timeval;

/*
        The broken-down time, in the layout every program that has ever read
        a struct tm expects, including the two fields that are not in C but
        are in glibc and in BSD and are therefore in practice mandatory.

        The field names are not prose and will not become prose. They are the
        names a caller writes, they are fixed by thirty years of source, and
        renaming them would mean this structure is not struct tm any more.

        tm_year counts from 1900 and tm_mon from zero, which are the two
        traps in the whole of <time.h>. tm_yday is zero for the first of
        January. tm_isdst is always zero here; see the note about zoneinfo.
*/
typedef struct tm
{
        b32 tm_sec;
        b32 tm_min;
        b32 tm_hour;
        b32 tm_mday;
        b32 tm_mon;
        b32 tm_year;
        b32 tm_wday;
        b32 tm_yday;
        b32 tm_isdst;
        b64 tm_gmtoff;
        const char address_to tm_zone;
} tm;

static const char address_to clock_zone_name = "UTC";

static const char address_to clock_weekday_short[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static const char address_to clock_weekday_long[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"};

static const char address_to clock_month_short[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char address_to clock_month_long[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};

/*
        What glibc prints where the index is out of range, rather than reading
        past the end of its own table. A hand-filled struct tm with tm_wday of
        nine is not an error strftime is allowed to report, so it prints this.
*/
static const char address_to clock_unknown_name = "?";

/*
        Division that rounds toward minus infinity, which is what a calendar
        means by "which day is this second in" and is not what C's / does.

        C truncates toward zero, so -1 / 86400 is 0 and the second before the
        epoch lands in the same day as the second after it. Every date before
        1970 depends on this correction and nothing after 1970 ever reaches
        it, which is exactly the shape of bug that survives a test suite that
        only looks at the present.
*/
static bipolar clock_floor_divide(bipolar value, bipolar divisor)
{
        bipolar quotient = value / divisor;

        if (value % divisor != 0 && (value < 0) != (divisor < 0))
                quotient--;

        return quotient;
}

/*
        The calendar, closed form, no table of month lengths and no loop over
        years. This is Howard Hinnant's days_from_civil and civil_from_days,
        from "chrono-Compatible Low-Level Date Algorithms",
        howardhinnant.github.io/date_algorithms.html, which he placed in the
        public domain and which is the arithmetic underneath C++'s
        <chrono> calendar. It is transcribed here rather than reinvented
        because a proof exists for it over the whole range of a 64-bit day
        count and would not exist for a second attempt at the same idea.

        The trick, and the only thing worth understanding to read the rest:
        the internal year begins on the first of March, not the first of
        January. That moves the leap day to the very end of the year, where
        it stops being an insertion the arithmetic has to step over, and it
        makes the twelve month lengths from March round to February a single
        almost regular sequence -- 31 30 31 30 31 31 30 31 30 31 31
        28 -- whose running total is exactly (153 * month + 2) / 5. That one
        expression is the whole of the month table. The +-3/+9 and the two
        `month <= 2` adjustments are just the shift into and out of that
        March-first year.

        The constants:

            146097  days in four hundred years, which is the length of the
                    Gregorian cycle: 400 * 365 + 97 leap days, since a year
                    divisible by 4 is a leap year unless it is divisible by
                    100 unless it is divisible by 400. An "era" below is one
                    of these four-century blocks.
             36524  days in one hundred years, 100 * 365 + 24
              1460  days in four years without the century rule, 4 * 365
            719468  days from 0000-03-01 to 1970-01-01. The epoch is not the
                    origin of the arithmetic; the start of an era is, and
                    this is the offset between them.

        Both directions are branch-free apart from the sign guards, which
        exist for the same reason clock_floor_divide does: era is a floored
        quotient and C's / is not, so the negative side is nudged down by one
        divisor's worth before the truncation happens.

        Preconditions, from the source: month is 1 through 12, day is 1
        through the length of that month, and year is the proleptic Gregorian
        year -- the Gregorian rules run backwards through 1582 and through
        zero, with year 0 being 1 BC and being a leap year. Both directions
        are exact for any year that fits in a signed 64-bit value, which is
        very much more than any caller here will ask for.
*/
static bipolar clock_days_from_civil(bipolar year, bipolar month, bipolar day)
{
        bipolar era;
        bipolar year_of_era;
        bipolar day_of_year;
        bipolar day_of_era;

        year -= month <= 2;

        era = (year >= 0 ? year : year - 399) / 400;
        year_of_era = year - era * 400;
        day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                     day_of_year;

        return era * 146097 + day_of_era - 719468;
}

static fn clock_civil_from_days(bipolar days, bipolar address_to year,
                                bipolar address_to month,
                                bipolar address_to day)
{
        bipolar era;
        bipolar day_of_era;
        bipolar year_of_era;
        bipolar shifted_year;
        bipolar day_of_year;
        bipolar shifted_month;
        bipolar civil_month;

        days += 719468;

        era = (days >= 0 ? days : days - 146096) / 146097;
        day_of_era = days - era * 146097;
        year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                       day_of_era / 146096) /
                      365;
        shifted_year = year_of_era + era * 400;
        day_of_year = day_of_era -
                      (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        shifted_month = (5 * day_of_year + 2) / 153;

        civil_month = shifted_month + (shifted_month < 10 ? 3 : -9);

        address_to day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
        address_to month = civil_month;
        address_to year = shifted_year + (civil_month <= 2);
}

/*
        The day of the week, straight from the day count and with no calendar
        in the way. 1970-01-01 was a Thursday, so day zero is weekday four,
        and the second form is C's truncating remainder corrected for the days
        before day minus four rather than a separate floored modulo.
*/
static bipolar clock_weekday_from_days(bipolar days)
{
        return days >= -4 ? (days + 4) % 7 : (days + 5) % 7 + 6;
}

/*
        The whole of gmtime, once the calendar exists.

        Returns false when the year does not fit in tm_year, which is an int
        and therefore runs out around the year two billion while a 64-bit
        time_t does not run out until the year three hundred billion. glibc
        answers a null pointer and EOVERFLOW there; this answers false and its
        callers turn that into the null pointer. There is no errno in this
        tree to set.
*/
static bool clock_break_down(bipolar seconds, tm address_to broken)
{
        bipolar days = clock_floor_divide(seconds, CLOCK_SECONDS_PER_DAY);
        bipolar rest = seconds - days * CLOCK_SECONDS_PER_DAY;
        bipolar year;
        bipolar month;
        bipolar day;
        bipolar year_field;

        clock_civil_from_days(days, address_of year, address_of month,
                              address_of day);

        year_field = year - 1900;

        if (year_field > 2147483647 || year_field < -2147483647 - 1)
                return false;

        broken->tm_sec = (b32)(rest % 60);
        broken->tm_min = (b32)((rest / 60) % 60);
        broken->tm_hour = (b32)(rest / 3600);
        broken->tm_mday = (b32)day;
        broken->tm_mon = (b32)(month - 1);
        broken->tm_year = (b32)year_field;
        broken->tm_wday = (b32)clock_weekday_from_days(days);
        broken->tm_yday = (b32)(days - clock_days_from_civil(year, 1, 1));
        broken->tm_isdst = 0;
        broken->tm_gmtoff = 0;
        broken->tm_zone = clock_zone_name;

        return true;
}

/*
        The offset from UTC that local time is, in seconds, at a given moment.

        It is zero. It is zero because nothing in this tree reads
        /etc/localtime and there is no zoneinfo on the system to read, and it
        will keep being zero until a TZif parser exists. The routine is here
        anyway so that when one does exist there is exactly one place to
        change and the two callers already going through it, rather than two
        copies of gmtime wearing a different name.
*/
static bipolar clock_local_offset(bipolar seconds)
{
        (void)seconds;

        return 0;
}

/*
        The two traps. Both take a pointer the kernel fills in, both answer
        zero or a negative errno, and neither exists under a different number
        on the three machines: riscv64 shares arm64's asm-generic table, which
        is why syscall(clock_gettime) resolves everywhere.

        There is deliberately no use of a `time` syscall. x86_64 has one,
        number 201, and arm64 and riscv64 have never had one -- it is one of
        the calls the asm-generic ABI dropped because clock_gettime already
        answers it. Building time() on the trap that exists on one machine out
        of three is how a family passes its own test and fails on the others.
*/
static bipolar clock_read(b32 which, timespec address_to into)
{
        return (bipolar)system_call_2(syscall(clock_gettime), (positive)which,
                                      (positive)into);
}

/*
        The seconds field of library.c's timespec is p64 and therefore
        unsigned, because that structure's job in this tree so far has been to
        carry a sleep duration into nanosleep and a duration is never
        negative. A wall clock reading is not a duration: it has an outside,
        and every second before 1970 is negative in it. So every read is cast
        to bipolar at the boundary, here, once, rather than by each caller --
        an unsigned second sliding into clock_break_down would make the
        `days >= 0` guard vacuously true and quietly hand back a date in the
        year 584 billion for every timestamp before the epoch.
*/
static bipolar clock_now(b32 which)
{
        timespec stamp = {0, 0};

        if (clock_read(which, address_of stamp) < 0)
                return -1;

        return (bipolar)stamp.tv_sec;
}

// The number of seconds since 1970-01-01 00:00:00 UTC, ignoring leap seconds
// as POSIX requires. The argument may be null, and is written when it is not.
time_t time(time_t address_to into)
{
        time_t now = (time_t)clock_now(CLOCK_REALTIME);

        if (!is_null(into))
                address_to into = now;

        return now;
}

/*
        Processor time this process has burned, in CLOCKS_PER_SEC units.

        Not get_cpu_time, which is the machine's free-running cycle counter
        and answers a different question: that one counts wall time in units
        nobody has calibrated, this one counts the time the scheduler actually
        gave this process and stops while it is blocked. A program measuring
        itself wants this; a program measuring an instruction sequence wants
        get_cpu_time.
*/
clock_t clock(void)
{
        timespec stamp = {0, 0};

        if (clock_read(CLOCK_PROCESS_CPUTIME_ID, address_of stamp) < 0)
                return (clock_t)-1;

        return (clock_t)stamp.tv_sec * CLOCKS_PER_SEC +
               (clock_t)(stamp.tv_nsec / 1000);
}

// Straight through to the kernel, with the errno convention flipped from
// Linux's negative return to the standard's minus one.
b32 clock_gettime(clockid_t which, timespec address_to into)
{
        return clock_read((b32)which, into) < 0 ? -1 : 0;
}

b32 clock_getres(clockid_t which, timespec address_to into)
{
        return (bipolar)system_call_2(syscall(clock_getres), (positive)which,
                                      (positive)into) < 0
                       ? -1
                       : 0;
}

/*
        The older shape, built on the newer trap rather than on its own.

        gettimeofday has its own syscall on all three machines, but it answers
        a coarser reading of the same clock and needs its own structure laid
        out for it; going through clock_gettime and dividing is one division
        and keeps one path. The second argument is the vestigial timezone
        pointer, which POSIX marked obsolete and which is ignored here as it
        is everywhere else.
*/
b32 gettimeofday(timeval address_to into, address_any zone)
{
        timespec stamp = {0, 0};

        (void)zone;

        if (clock_read(CLOCK_REALTIME, address_of stamp) < 0)
                return -1;

        if (!is_null(into))
        {
                into->tv_sec = (b64)stamp.tv_sec;
                into->tv_usec = (b64)(stamp.tv_nsec / 1000);
        }

        return 0;
}

// Seconds between two points, as a decimal because the standard says so and
// because the difference of two 64-bit times does not always fit in one.
decimal difftime(time_t later, time_t earlier)
{
        return (decimal)later - (decimal)earlier;
}

/*
        The way back: a broken-down time to a count of seconds, and the
        broken-down time normalised in place while we are there.

        Normalising is not a nicety, it is most of what mktime is for. A
        caller that wants "thirty days from now" adds thirty to tm_mday and
        calls this, and a caller reading a date out of a file hands over
        whatever was in the file. So nothing here checks a range: the month is
        floored into a year and a remainder, the day of the month is added to
        the first of that month as a plain offset and is allowed to be zero or
        negative or four hundred, and the hours, minutes and seconds are
        multiplied out and added without a care for whether any of them is
        under sixty. Every one of those out-of-range values lands on the right
        second, and clock_break_down then writes the in-range spelling of that
        second back through the caller's pointer along with tm_wday and
        tm_yday, which is what the standard requires and what a round-trip
        test through gmtime can never see, because gmtime only ever hands back
        a structure that was already normal.

        tm_isdst is ignored rather than consulted. There is no daylight saving
        without a timezone database, so there is no ambiguous hour to resolve.
*/
time_t timegm(tm address_to broken)
{
        bipolar year;
        bipolar month;
        bipolar carried;
        bipolar days;
        bipolar seconds;

        if (is_null(broken))
                return (time_t)-1;

        year = (bipolar)broken->tm_year + 1900;
        month = (bipolar)broken->tm_mon;
        carried = clock_floor_divide(month, 12);
        year += carried;
        month -= carried * 12;

        days = clock_days_from_civil(year, month + 1, 1) +
               (bipolar)broken->tm_mday - 1;

        seconds = days * CLOCK_SECONDS_PER_DAY +
                  (bipolar)broken->tm_hour * 3600 +
                  (bipolar)broken->tm_min * 60 + (bipolar)broken->tm_sec;

        if (!clock_break_down(seconds, broken))
                return (time_t)-1;

        return (time_t)seconds;
}

// Local time is UTC here, so this is timegm with the offset subtracted, and
// the offset is zero. See clock_local_offset for why the call is written out
// rather than folded away.
time_t mktime(tm address_to broken)
{
        time_t answer = timegm(broken);

        if (answer == (time_t)-1)
                return answer;

        return answer - (time_t)clock_local_offset((bipolar)answer);
}

tm address_to gmtime_r(const time_t address_to stamp, tm address_to into)
{
        if (is_null(stamp) || is_null(into))
                return null;

        if (!clock_break_down((bipolar)(address_to stamp), into))
                return null;

        return into;
}

/*
        The static one structure that gmtime and localtime share, which is
        what glibc does too and what makes both of them unusable from two
        threads at once. The _r forms above and below exist for that reason
        and are the ones anything long-lived should call.
*/
static tm clock_broken_shared;

tm address_to gmtime(const time_t address_to stamp)
{
        return gmtime_r(stamp, address_of clock_broken_shared);
}

tm address_to localtime_r(const time_t address_to stamp, tm address_to into)
{
        time_t shifted;

        if (is_null(stamp) || is_null(into))
                return null;

        shifted = address_to stamp + (time_t)clock_local_offset((bipolar)(address_to stamp));

        return gmtime_r(address_of shifted, into);
}

tm address_to localtime(const time_t address_to stamp)
{
        return localtime_r(stamp, address_of clock_broken_shared);
}

/*
        Two shapes of decimal field, which between them are every number this
        file prints.

        clock_number_precision is printf's "%.Nd": the sign first, then the
        magnitude zero-filled to at least N digits, so -5 at two digits is
        "-05". clock_number_field is printf's "%Nd": the number as it is, then
        spaces on the left until it is N wide, so -5 at three wide is " -5".
        Neither ever truncates. Both write no terminator and return the count,
        which is what positive_into_padded underneath them does.
*/
static positive clock_number_precision(p8 address_to into, bipolar value,
                                       positive least)
{
        positive length = 0;
        positive magnitude;

        if (value < 0)
        {
                into[0] = '-';
                length = 1;
                magnitude = (positive)(-(value + 1)) + 1;
        }
        else
                magnitude = (positive)value;

        return length +
               positive_into_padded(into + length, magnitude, least, '0');
}

static positive clock_number_field(p8 address_to into, bipolar value,
                                   positive width)
{
        p8 body[32];
        positive length = clock_number_precision(body, value, 1);
        positive at = 0;

        while (at + length < width)
                into[at++] = ' ';

        memory_copy_apart(into + at, body, length);

        return at + length;
}

/*
        asctime, which is the oldest thing in <time.h> and the only one with a
        buffer size written into its contract: twenty six bytes, of which
        twenty five are the line

            Thu Jan  1 00:00:00 1970\n

        and the twenty sixth is the terminator. Nothing about that is
        adjustable, and a caller cannot say the buffer is bigger, so the only
        thing to do with a broken-down time that does not fit in twenty five
        bytes is refuse it. A year of five digits does that, and so does a day
        of the month in the hundreds; a year before the common era does not,
        because "Fri Dec 31 23:59:59 -1" is twenty two bytes and glibc prints
        it, so this prints it too.

        The line is assembled somewhere else first and only copied over once
        its length is known, which is what makes the refusal safe: measuring
        it in the caller's twenty six bytes would mean having already written
        past them.
*/
p8 address_to asctime_r(const tm address_to broken, p8 address_to into)
{
        p8 line[128];
        positive at = 0;

        if (is_null(broken) || is_null(into))
                return null;

        /*
                The two fields that index a table are the two that are checked,
                because a wrong one there is a read past the end of the table
                rather than a wrong answer. Everything else is printed as it
                stands and is caught, if it is absurd, by the length test at
                the bottom -- which is exactly the order glibc does it in.
        */
        if (broken->tm_wday < 0 || broken->tm_wday > 6 || broken->tm_mon < 0 ||
            broken->tm_mon > 11)
                return null;

        memory_copy_apart(line + at,
                          (address_any)clock_weekday_short[broken->tm_wday], 3);
        at += 3;
        line[at++] = ' ';
        memory_copy_apart(line + at,
                          (address_any)clock_month_short[broken->tm_mon], 3);
        at += 3;
        at += clock_number_field(line + at, broken->tm_mday, 3);
        line[at++] = ' ';
        at += clock_number_precision(line + at, broken->tm_hour, 2);
        line[at++] = ':';
        at += clock_number_precision(line + at, broken->tm_min, 2);
        line[at++] = ':';
        at += clock_number_precision(line + at, broken->tm_sec, 2);
        line[at++] = ' ';
        at += clock_number_precision(line + at,
                                     (bipolar)broken->tm_year + 1900, 1);
        line[at++] = '\n';

        /*
                Twenty five bytes and a terminator is the whole of the buffer
                this routine is allowed, and a year of five digits or a day of
                the month in the hundreds is how a caller runs out of it.
                Answering a null pointer is what glibc does there, and it is
                the only answer available: the contract names the size.
        */
        if (at > 25)
                return null;

        memory_copy_apart(into, line, at);
        into[at] = end;

        return into;
}

static p8 clock_asctime_shared[32];

p8 address_to asctime(const tm address_to broken)
{
        return asctime_r(broken, clock_asctime_shared);
}

p8 address_to ctime_r(const time_t address_to stamp, p8 address_to into)
{
        tm broken;

        if (is_null(localtime_r(stamp, address_of broken)))
                return null;

        return asctime_r(address_of broken, into);
}

p8 address_to ctime(const time_t address_to stamp)
{
        return ctime_r(stamp, clock_asctime_shared);
}

/*
        strftime.

        The awkward half is not the specifiers, it is the return value, and it
        is the half every hand-written strftime gets wrong. The contract:

          - the answer is the number of bytes written, not counting the
            terminator, which is written;
          - if the whole result plus its terminator will not fit in max, the
            answer is zero and the contents of the buffer are unspecified;
          - max of zero writes nothing at all, not even a terminator, and the
            pointer is allowed to be null.

        Which means the size check cannot be "did I run out while copying",
        because a result that exactly fills max has run out -- there is no room
        for the terminator -- and must still not write a byte past max. So the
        cursor below never advances past max, every append refuses rather than
        truncates, and the terminator is written once at the end only after
        used + 1 has been checked against max. A test that only feeds it a
        generous buffer reaches none of this, so the suite sweeps max from zero
        to past the answer for a format containing every specifier.

        The compound specifiers -- %c %x %X %D %F %R %T %r -- are their own
        format string handed back to the same loop, which is what glibc does
        and is why they cannot drift from the pieces they are made of. None of
        them contains a compound specifier, so the recursion is one deep.

        The C locale is the only locale. There is no LC_TIME here to read, so
        the four name tables above are the answer and %E and %O, which ask for
        a locale's alternative era and alternative digits, are accepted and
        ignored exactly as glibc does when the locale has neither.
*/
typedef struct clock_format_state
{
        p8 address_to into;
        positive max;
        positive used;
        bool failed;
} clock_format_state;

static fn clock_format_append(clock_format_state address_to state,
                              address_any bytes, positive length)
{
        if (state->failed)
                return;

        if (length > state->max || state->used > state->max - length)
        {
                state->failed = true;
                return;
        }

        if (length)
                memory_copy_apart(state->into + state->used, bytes, length);

        state->used += length;
}

static fn clock_format_byte(clock_format_state address_to state, p8 byte)
{
        clock_format_append(state, address_of byte, 1);
}

static fn clock_format_text(clock_format_state address_to state,
                            const char address_to text)
{
        clock_format_append(state, (address_any)text,
                            string_length((string_address)text));
}

/*
        The two field shapes again, this time through the writer.

        The zero-filled one carries every specifier that has a fixed width --
        %d %H %j and the rest -- and the spaced one carries %e alone. The
        three that name a year, %Y %G and %C, ask for a least of zero and are
        therefore printed exactly as wide as they are: glibc 2.44 does not pad
        them, "%F" of the year one is "1-01-01" and not "0001-01-01", and this
        matches it. That is a real fork in the road -- musl pads %Y to four --
        and neither C nor POSIX says which is right, so the answer here is the
        one the reference on the build machine gives.
*/
static fn clock_format_number(clock_format_state address_to state,
                              bipolar value, positive least)
{
        p8 digits[32];

        clock_format_append(state, digits,
                            clock_number_precision(digits, value, least));
}

static fn clock_format_number_spaced(clock_format_state address_to state,
                                     bipolar value, positive width)
{
        p8 digits[64];

        clock_format_append(state, digits,
                            clock_number_field(digits, value, width));
}

// The name tables, guarded, answering what glibc answers for an index that is
// outside them rather than reading past the end of the table.
static const char address_to clock_weekday_name(b32 weekday, bool full)
{
        if (weekday < 0 || weekday > 6)
                return clock_unknown_name;

        return full ? clock_weekday_long[weekday] : clock_weekday_short[weekday];
}

static const char address_to clock_month_name(b32 month, bool full)
{
        if (month < 0 || month > 11)
                return clock_unknown_name;

        return full ? clock_month_long[month] : clock_month_short[month];
}

/*
        The ISO 8601 week-based year and week number, which are a different
        calendar sharing the same days: a week belongs to whichever year its
        Thursday is in, so the first days of January can be week 52 or 53 of
        the year before, and the last days of December can be week 1 of the
        year after.

        Written through the day count rather than through a table of cases,
        because with clock_days_from_civil already here the definition is
        directly executable: find the Thursday of this week, ask which year it
        is in, and count weeks from the first of January of that year.
*/
static fn clock_iso_week(const tm address_to broken, bipolar address_to year,
                         bipolar address_to week)
{
        bipolar days = clock_days_from_civil((bipolar)broken->tm_year + 1900,
                                             (bipolar)broken->tm_mon + 1,
                                             (bipolar)broken->tm_mday);
        bipolar weekday = clock_weekday_from_days(days);
        bipolar thursday = days - (weekday == 0 ? 7 : weekday) + 4;
        bipolar thursday_year;
        bipolar thursday_month;
        bipolar thursday_day;

        clock_civil_from_days(thursday, address_of thursday_year,
                              address_of thursday_month,
                              address_of thursday_day);

        address_to year = thursday_year;
        address_to week =
                (thursday - clock_days_from_civil(thursday_year, 1, 1)) / 7 + 1;
}

static fn clock_format_core(clock_format_state address_to state,
                            const char address_to format,
                            const tm address_to broken);

static fn clock_format_nested(clock_format_state address_to state,
                              const char address_to format,
                              const tm address_to broken)
{
        clock_format_core(state, format, broken);
}

static fn clock_format_core(clock_format_state address_to state,
                            const char address_to format,
                            const tm address_to broken)
{
        const char address_to cursor = format;

        while (address_to cursor != end)
        {
                p8 which;
                p8 modifier = 0;

                if (address_to cursor != '%')
                {
                        clock_format_byte(state, (p8)(address_to cursor));
                        cursor++;
                        continue;
                }

                cursor++;

                if (address_to cursor == 'E' || address_to cursor == 'O')
                {
                        modifier = (p8)(address_to cursor);
                        cursor++;
                }

                which = (p8)(address_to cursor);

                if (which == end)
                {
                        /*
                                A format that stops in the middle of a
                                specifier. glibc emits the percent and
                                whatever it had already consumed, and does not
                                walk off the end looking for more.
                        */
                        clock_format_byte(state, '%');

                        if (modifier)
                                clock_format_byte(state, modifier);

                        break;
                }

                cursor++;

                switch (which)
                {
                case 'a':
                        clock_format_text(state,
                                          clock_weekday_name(broken->tm_wday,
                                                             false));
                        break;

                case 'A':
                        clock_format_text(state,
                                          clock_weekday_name(broken->tm_wday,
                                                             true));
                        break;

                case 'b':
                case 'h':
                        clock_format_text(
                                state, clock_month_name(broken->tm_mon, false));
                        break;

                case 'B':
                        clock_format_text(
                                state, clock_month_name(broken->tm_mon, true));
                        break;

                case 'c':
                        clock_format_nested(state, "%a %b %e %H:%M:%S %Y",
                                            broken);
                        break;

                case 'C':
                        clock_format_number(
                                state,
                                clock_floor_divide((bipolar)broken->tm_year +
                                                           1900,
                                                   100),
                                0);
                        break;

                case 'd':
                        clock_format_number(state, broken->tm_mday, 2);
                        break;

                case 'D':
                        clock_format_nested(state, "%m/%d/%y", broken);
                        break;

                case 'e':
                        clock_format_number_spaced(state, broken->tm_mday, 2);
                        break;

                case 'F':
                        clock_format_nested(state, "%Y-%m-%d", broken);
                        break;

                case 'g':
                {
                        bipolar week_year;
                        bipolar week;

                        clock_iso_week(broken, address_of week_year,
                                       address_of week);
                        clock_format_number(
                                state,
                                week_year - clock_floor_divide(week_year, 100) *
                                                    100,
                                2);
                        break;
                }

                case 'G':
                {
                        bipolar week_year;
                        bipolar week;

                        clock_iso_week(broken, address_of week_year,
                                       address_of week);
                        clock_format_number(state, week_year, 0);
                        break;
                }

                case 'H':
                        clock_format_number(state, broken->tm_hour, 2);
                        break;

                case 'I':
                {
                        bipolar hour = broken->tm_hour % 12;

                        clock_format_number(state, hour == 0 ? 12 : hour, 2);
                        break;
                }

                case 'j':
                        clock_format_number(state, (bipolar)broken->tm_yday + 1,
                                            3);
                        break;

                case 'm':
                        clock_format_number(state, (bipolar)broken->tm_mon + 1,
                                            2);
                        break;

                case 'M':
                        clock_format_number(state, broken->tm_min, 2);
                        break;

                case 'n':
                        clock_format_byte(state, '\n');
                        break;

                case 'p':
                        clock_format_text(state, broken->tm_hour < 12 ? "AM"
                                                                     : "PM");
                        break;

                case 'P':
                        clock_format_text(state, broken->tm_hour < 12 ? "am"
                                                                     : "pm");
                        break;

                case 'r':
                        clock_format_nested(state, "%I:%M:%S %p", broken);
                        break;

                case 'R':
                        clock_format_nested(state, "%H:%M", broken);
                        break;

                case 's':
                {
                        tm copy = address_to broken;

                        clock_format_number(state, (bipolar)timegm(address_of copy),
                                            0);
                        break;
                }

                case 'S':
                        clock_format_number(state, broken->tm_sec, 2);
                        break;

                case 't':
                        clock_format_byte(state, '\t');
                        break;

                case 'T':
                        clock_format_nested(state, "%H:%M:%S", broken);
                        break;

                case 'u':
                        clock_format_number(state,
                                            broken->tm_wday == 0
                                                    ? 7
                                                    : broken->tm_wday,
                                            1);
                        break;

                case 'U':
                        clock_format_number(state,
                                            ((bipolar)broken->tm_yday + 7 -
                                             (bipolar)broken->tm_wday) /
                                                    7,
                                            2);
                        break;

                case 'V':
                {
                        bipolar week_year;
                        bipolar week;

                        clock_iso_week(broken, address_of week_year,
                                       address_of week);
                        clock_format_number(state, week, 2);
                        break;
                }

                case 'w':
                        clock_format_number(state, broken->tm_wday, 1);
                        break;

                case 'W':
                        clock_format_number(
                                state,
                                ((bipolar)broken->tm_yday + 7 -
                                 (broken->tm_wday == 0
                                          ? 6
                                          : (bipolar)broken->tm_wday - 1)) /
                                        7,
                                2);
                        break;

                case 'x':
                        clock_format_nested(state, "%m/%d/%y", broken);
                        break;

                case 'X':
                        clock_format_nested(state, "%H:%M:%S", broken);
                        break;

                case 'y':
                {
                        bipolar year = (bipolar)broken->tm_year + 1900;

                        clock_format_number(
                                state,
                                year - clock_floor_divide(year, 100) * 100,
                                2);
                        break;
                }

                case 'Y':
                        clock_format_number(
                                state, (bipolar)broken->tm_year + 1900, 0);
                        break;

                case 'z':
                {
                        bipolar offset = broken->tm_gmtoff;
                        bipolar magnitude = offset < 0 ? -offset : offset;

                        clock_format_byte(state, offset < 0 ? '-' : '+');
                        clock_format_number(state, magnitude / 3600, 2);
                        clock_format_number(state, magnitude / 60 % 60, 2);
                        break;
                }

                case 'Z':
                        clock_format_text(state, is_null(broken->tm_zone)
                                                         ? clock_zone_name
                                                         : broken->tm_zone);
                        break;

                case '%':
                        clock_format_byte(state, '%');
                        break;

                default:
                        /*
                                Anything nobody knows comes back out exactly as
                                it went in, percent and modifier and all, which
                                is what glibc does and what lets a format
                                string carrying a specifier from a newer
                                standard degrade into visible text rather than
                                vanish.
                        */
                        clock_format_byte(state, '%');

                        if (modifier)
                                clock_format_byte(state, modifier);

                        clock_format_byte(state, which);
                        break;
                }
        }
}

positive strftime(p8 address_to into, positive max, const char address_to format,
                  const tm address_to broken)
{
        clock_format_state state;

        if (is_null(format) || is_null(broken))
                return 0;

        state.into = into;
        state.max = max;
        state.used = 0;
        state.failed = false;

        clock_format_core(address_of state, format, broken);

        if (state.failed || state.used + 1 > max)
                return 0;

        into[state.used] = end;

        return state.used;
}

/*
        The three globals tzset is supposed to set, set to what they actually
        are here. tzset itself is a call that does nothing, which is honest:
        there is no zone to load, and a program that calls it is asking for
        the timezone to be re-read rather than for anything to change.
*/
static const char address_to clock_zone_names[2] = {"UTC", "UTC"};

const char address_to address_to tzname = clock_zone_names;
b64 timezone = 0;
b32 daylight = 0;

fn tzset(void)
{
}

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_CLOCK
