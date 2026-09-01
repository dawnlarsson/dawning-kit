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
        time will need a TZif parser; until one exists the local spellings
        share the UTC entries below.
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
static const p8 clock_weekday_long_length[7] = {6, 6, 7, 9, 8, 6, 8};

static const char address_to clock_month_short[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char address_to clock_month_long[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
static const p8 clock_month_long_length[12] = {
        7, 8, 5, 5, 3, 4, 4, 6, 9, 7, 8, 8};

/*
        The two halves of a twelve hour clock. A table rather than a pair of
        literals inside %p, because strptime has to match the same two words
        the way it matches a weekday name -- longest first and without regard
        to case -- and two spellings of the same two words would be two places
        to get them wrong. %P prints these folded to lower case rather than
        keeping a second table beside this one.
*/
static const char address_to clock_half_day[2] = {"AM", "PM"};

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
CONST decimal difftime(time_t later, time_t earlier)
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

time_t mktime(tm address_to broken) __attribute__((alias("timegm")));

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

//      Without a timezone database, the local reentrant spelling is UTC too.
tm address_to localtime_r(const time_t address_to stamp, tm address_to into)
        __attribute__((alias("gmtime_r")));

tm address_to localtime(const time_t address_to stamp)
        __attribute__((alias("gmtime")));

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
        the four name tables above are the answer, and %E and %O -- which ask
        a locale for an alternative era and for alternative digits -- have
        nothing to select and print what the unmodified specifier prints.

        A directive is

            %  flags  width  modifier  specifier

        in that order and in no other. Everything but the last is optional,
        and the order is not advice: %E5Y is not %5EY. It is a directive whose
        specifier is '5', which nobody knows, so "%E5" comes back out as text
        and the Y behind it is an ordinary letter. glibc does exactly that,
        and so does this.

        The flags, which may repeat and may come in any order:

            -   do not pad the number at all
            _   pad the number with spaces
            0   pad the number with zeros
            ^   upper case the letters of the answer
            #   the other case from the one the answer usually has

        The width is decimal, is a minimum, and never truncates. It is a GNU
        extension that neither C nor POSIX has and that every strftime worth
        calling implements, so a format written for one of those is a format
        this has to read.

        What follows is glibc's algorithm rather than an independent one, down
        to two behaviours surprising enough to name. A differential test
        against glibc is the only reference this family has, and matching it
        on the ordinary cases while inventing an answer for the odd ones would
        make that test worth nothing:

          - a width applies to each piece a directive emits, separately, and
            the number path spends the width it consumed. That is why "%5z" is
            "    +00000" rather than "+0000": the sign is one piece widened to
            five, and the four digits of the offset are a second piece widened
            to five again.
          - a directive nobody knows comes back out as the bytes that were
            consumed -- percent, flags, width, modifier and all -- and it comes
            back out through the same path a weekday name takes, so it is
            widened and case folded on the way. "%5Q" is "  %5Q" and "%^q" is
            "%^Q".

        Which specifiers take which modifier is glibc's table and there is no
        principle in it to derive it from: %Ec is a directive and %Ed is not,
        %OS is one and %ES is not. It is transcribed below rather than
        reasoned about. A modifier the specifier does not take is not an
        error, it is a directive nobody knows, and it comes back out as text.
*/

/*
        The writer, and the decoration the directive being read asked for.

        into may be null only when max is zero, which is the one call the
        standard allows to measure nothing at all. width is -1 when the format
        named none, which is not the same as a named zero: the number path
        tells them apart, and a named zero still spends itself. pad holds the
        flag byte, or zero when no flag was given, because for %e %k and %l
        "no flag" and "the 0 flag" mean opposite things.

        to_upper and to_lower are what ^ resolved to and what # resolved to.
        They cannot both be set: # decides its direction inside the specifier
        that reads it -- upper for the four name tables, lower for %p %P and
        %Z -- and whichever it picks it clears the other. That is why # is
        carried as change_case until then rather than as a fold of its own.
*/
typedef struct clock_format_state
{
        p8 address_to into;
        positive max;
        positive used;
        bool failed;
        bipolar width;
        p8 pad;
        bool to_upper;
        bool to_lower;
        bool change_case;
} clock_format_state;

/*
        The floor under all three appends: these bytes, as they are, with no
        width and no fold. It is the only place that decides there is no room,
        so every other writer here is bounded by having gone through it.
*/
static fn clock_format_raw(clock_format_state address_to state,
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

/*
        The same, for one byte repeated, which is what every kind of padding
        in this file is. It is a separate routine and not a loop at the four
        call sites because memory_fill is the assembly: a width of two hundred
        is two hundred bytes of one store, not two hundred stores.
*/
static fn clock_format_run(clock_format_state address_to state, p8 byte,
                           positive count)
{
        if (state->failed)
                return;

        if (count > state->max || state->used > state->max - count)
        {
                state->failed = true;
                return;
        }

        if (count)
                memory_fill(state->into + state->used, (b8)byte, count);

        state->used += count;
}

/*
        The width, in front of whatever is about to be written.

        Zeros under the 0 flag and spaces under everything else, the - flag
        included: - turns off the number path's own padding and leaves this
        one running, which is why "%-10d" is eight spaces and then "29" while
        "%10d" is eight zeros and then "29".
*/
static fn clock_format_widen(clock_format_state address_to state,
                             positive length)
{
        if (state->width > 0 && (positive)state->width > length)
                clock_format_run(state, state->pad == '0' ? '0' : ' ',
                                 (positive)state->width - length);
}

/*
        One byte, widened and not folded. The ordinary characters of the
        format take this path with no width set; %n, %t, %% and the sign in
        front of %z take it with whatever width the directive named.
*/
static fn clock_format_byte(clock_format_state address_to state, p8 byte)
{
        clock_format_widen(state, 1);

        /*
                One byte, stored rather than copied.

                clock_format_raw takes its length as a variable, so the
                known-size expansion of memory_copy_apart cannot fold there
                and every literal byte of a format string -- the colons in
                %H:%M:%S, the spaces, every character that is not a directive
                -- paid a call into the general routine to move one byte. The
                bounds test below is exactly raw's, written out because after
                folding the length to one there is nothing else left of it:
                `1 > max || used > max - 1` is `used >= max` for every max,
                including zero. Measured on a pure-literal format, 158 cycles
                to 122.
        */
        if (state->failed)
                return;

        if (state->used >= state->max)
        {
                state->failed = true;
                return;
        }

        state->into[state->used] = byte;
        state->used++;
}

/*
        Bytes, widened and then folded to the case the flags asked for.

        The fold happens in place over the range just written rather than over
        a copy on the way in, which is what lets memory_to_upper_ascii do it
        sixteen or thirty two bytes at a time. Nothing else has that range
        yet, so writing it twice is free and correct. The padding in front is
        deliberately outside the fold: a space is a space in both cases, and
        glibc folds only the body.

        Lower is tested before upper because a specifier that forces lower --
        %P does, always -- has to win over a ^ in the flags, which is the
        order glibc's copy does it in.
*/
static fn clock_format_append(clock_format_state address_to state,
                              address_any bytes, positive length)
{
        positive where;

        clock_format_widen(state, length);

        where = state->used;

        clock_format_raw(state, bytes, length);

        if (state->failed || length == 0)
                return;

        if (state->to_lower)
                memory_to_lower_ascii(state->into + where, length);
        else if (state->to_upper)
                memory_to_upper_ascii(state->into + where, length);
}

static fn clock_format_text(clock_format_state address_to state,
                            const char address_to text)
{
        clock_format_append(state, (address_any)text,
                            string_length((string_address)text));
}

/*
        A number: its sign, then its own padding, then the width.

        least is how many digits the specifier has when the format said
        nothing -- two for %d, three for %j, one for the three that name a
        year -- and a wider width replaces it rather than adding to it. The
        padding is the difference between that and the digits there actually
        are, and where it goes is the whole of what the three padding flags
        mean:

            -   nowhere. The width is left unspent for the append below.
            _   spaces, in front of the sign, and they spend that much width.
            0   zeros, behind the sign, and they spend all of it.

        so that -1 at five digits is "-0001" and never "000-1", and the same
        number under _ is "   -1". No flag at all is the third row, which is
        why %d pads with zeros without being asked to.

        positive_into_padded writes the digits and there is no loop here that
        divides by ten; the sign is one byte in front of them, in a buffer
        that starts one byte early so that dropping the sign back off under
        the 0 flag is an index and not a move.
*/
static positive clock_number_text(p8 address_to body, bipolar value,
                                  positive address_to at)
{
        bool negative = value < 0;
        positive magnitude = negative ? (positive)(-(value + 1)) + 1
                                      : (positive)value;
        positive length = positive_into(body + 1, magnitude);

        address_to at = 1;

        if (negative)
        {
                body[0] = '-';
                address_to at = 0;
                length++;
        }

        return length;
}

static fn clock_format_number(clock_format_state address_to state,
                              bipolar value, positive least)
{
        p8 body[40];
        bool negative = value < 0;
        positive at;
        positive length = clock_number_text(body, value, address_of at);

        if (state->width > (bipolar)least)
                least = (positive)state->width;

        if (state->pad != '-' && least > length)
        {
                positive padding = least - length;

                if (state->pad == '_')
                {
                        clock_format_run(state, ' ', padding);

                        state->width = state->width > (bipolar)padding
                                               ? state->width - (bipolar)padding
                                               : 0;
                }
                else
                {
                        if (negative)
                        {
                                clock_format_run(state, '-', 1);
                                at++;
                                length--;
                        }

                        clock_format_run(state, '0', padding);
                        state->width = 0;
                }
        }

        clock_format_append(state, body + at, length);
}

/*
        %e, %k and %l, which pad with spaces rather than zeros unless the
        format said otherwise, and that is the whole of what makes them
        different from %d, %H and %I. "Otherwise" is the 0 flag or the - flag;
        ^ and # do not enter into it.
*/
static fn clock_format_number_spaced(clock_format_state address_to state,
                                     bipolar value, positive least)
{
        if (state->pad != '0' && state->pad != '-')
                state->pad = '_';

        clock_format_number(state, value, least);
}

// Guard and append either calendar-name table with the same out-of-range rule.
static fn clock_format_name(clock_format_state address_to state, b32 index,
                            bool full, b32 count,
                            const char address_to short_names[],
                            const char address_to long_names[],
                            const p8 address_to long_lengths)
{
        if (index < 0 || index >= count)
                return clock_format_append(state, (address_any)clock_unknown_name,
                                           1);

        clock_format_append(state,
                            (address_any)(full ? long_names[index]
                                              : short_names[index]),
                            full ? long_lengths[index] : 3);
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

/*
        A compound specifier is its own format string, and the decoration
        belongs to the compound and not to each of its pieces: "%^c" upper
        cases the finished line once, and "%40c" pads the finished line once,
        rather than doing either to the weekday and then again to the month
        and again to every number between them. So the pieces are assembled
        undecorated somewhere else first and handed over as one run of bytes,
        which is what glibc does and is the only reading of "%40c" that is not
        absurd.

        Sixty four bytes is the longest any of the seven can be, with a year
        of ten digits in it. The buffer is four times that, and running out of
        it fails the whole call rather than truncating: a truncated compound
        would be a wrong answer wearing the shape of a right one.
*/
static fn clock_format_nested(clock_format_state address_to state,
                              const char address_to format,
                              const tm address_to broken)
{
        p8 body[256];
        clock_format_state inner;

        if (state->failed)
                return;

        inner.into = body;
        inner.max = sizeof(body);
        inner.used = 0;
        inner.failed = false;
        inner.width = -1;
        inner.pad = 0;
        inner.to_upper = false;
        inner.to_lower = false;
        inner.change_case = false;

        clock_format_core(address_of inner, format, broken);

        if (inner.failed)
        {
                state->failed = true;
                return;
        }

        clock_format_append(state, body, inner.used);
}

/*
        The specifiers that refuse a modifier, which is glibc's table with the
        sense turned round: the lists of refusals are shorter than the lists
        of acceptances, and a specifier this file has never heard of refuses
        both by falling off the end of the same test.

        There is nothing underneath either list. E asks for an era and O asks
        for a locale's own digits, and which specifiers a locale was allowed
        to have an opinion about was settled one case at a time over thirty
        years. %Ou is a directive and %Oa is not; %OS is and %ES is not.
*/
static const char address_to clock_refuses_era = "aAbBdDeFgGhHIjklmMSUVwW";
static const char address_to clock_refuses_digits = "aAcDFxXY";

static bool clock_format_takes_modifier(p8 modifier, p8 which)
{
        if (modifier == 'E')
                return is_null(string_first_of(
                        (string_address)clock_refuses_era, which));

        if (modifier == 'O')
                return is_null(string_first_of(
                        (string_address)clock_refuses_digits, which));

        return true;
}

/*
        Everything from the percent to the specifier, back out as text.

        Three different failures share it -- a format that stopped in the
        middle of a directive, a modifier the specifier will not take, and a
        specifier nobody knows -- and all three are the same answer: the bytes
        that were consumed, through the ordinary widened and folded path, so
        that a format string carrying a specifier from a newer standard
        degrades into something visible rather than vanishing.
*/
static fn clock_format_verbatim(clock_format_state address_to state,
                                const char address_to opened,
                                const char address_to cursor)
{
        clock_format_append(state, (address_any)opened,
                            (positive)(cursor - opened));
}

static fn clock_format_core(clock_format_state address_to state,
                            const char address_to format,
                            const tm address_to broken)
{
        const char address_to cursor = format;

        while (address_to cursor != end)
        {
                const char address_to opened = cursor;
                p8 which;
                p8 modifier = 0;

                /*
                        The decoration is per directive and nothing carries
                        over, so it is cleared here and not after use: an
                        ordinary character of the format is a directive with
                        no decoration at all and must not inherit the width of
                        the one before it.
                */
                state->width = -1;
                state->pad = 0;
                state->to_upper = false;
                state->to_lower = false;
                state->change_case = false;

                if (address_to cursor != '%')
                {
                        clock_format_byte(state, (p8)(address_to cursor));
                        cursor++;
                        continue;
                }

                cursor++;

                /*
                        The flags, repeatable and in any order. The last of
                        -, _ and 0 wins, because the three of them are one
                        question answered three ways; ^ and # are each their
                        own question and stay set once they are.
                */
                while (true)
                {
                        p8 flag = (p8)(address_to cursor);

                        if (flag == '-' || flag == '_' || flag == '0')
                                state->pad = flag;
                        else if (flag == '^')
                                state->to_upper = true;
                        else if (flag == '#')
                                state->change_case = true;
                        else
                                break;

                        cursor++;
                }

                /*
                        The width. Saturating and not wrapping: a width no
                        buffer could satisfy has to end as an answer of zero
                        for want of room, and never as a negative width that
                        would pad backwards through the caller's memory. The
                        ceiling is the same signed thirty two bit one glibc
                        stops at, so the two agree on where absurd begins.
                */
                if (byte_is_digit((p8)(address_to cursor)))
                {
                        state->width = 0;

                        while (byte_is_digit((p8)(address_to cursor)))
                        {
                                bipolar digit = (p8)(address_to cursor) - '0';

                                if (state->width > 214748364 ||
                                    (state->width == 214748364 && digit > 7))
                                        state->width = 2147483647;
                                else
                                        state->width =
                                                state->width * 10 + digit;

                                cursor++;
                        }
                }

                if (address_to cursor == 'E' || address_to cursor == 'O')
                {
                        modifier = (p8)(address_to cursor);
                        cursor++;
                }

                which = (p8)(address_to cursor);

                if (which != end)
                        cursor++;

                /*
                        # decides which way to fold inside the specifier that
                        reads it, and glibc reads it in a different order for
                        %b and %h than for the other three name specifiers:
                        those two fold before they notice that the modifier in
                        front of them is one they do not take, and %a, %A and
                        %B notice first and never fold. Which is why "%#Eb" is
                        "%#EB" and "%#Ea" is "%#Ea". There is no reason for the
                        difference and nobody meant it; it is transcribed here
                        so that the differential test against glibc needs no
                        exception carved out of it.
                */
                if (state->change_case && (which == 'b' || which == 'h'))
                {
                        state->to_upper = true;
                        state->to_lower = false;
                }

                if (which == end ||
                    (modifier && !clock_format_takes_modifier(modifier, which)))
                {
                        clock_format_verbatim(state, opened, cursor);
                        continue;
                }

                switch (which)
                {
                case 'a':
                        if (state->change_case)
                        {
                                state->to_upper = true;
                                state->to_lower = false;
                        }

                        clock_format_name(state, broken->tm_wday, false, 7,
                                          clock_weekday_short,
                                          clock_weekday_long,
                                          clock_weekday_long_length);
                        break;

                case 'A':
                        if (state->change_case)
                        {
                                state->to_upper = true;
                                state->to_lower = false;
                        }

                        clock_format_name(state, broken->tm_wday, true, 7,
                                          clock_weekday_short,
                                          clock_weekday_long,
                                          clock_weekday_long_length);
                        break;

                case 'b':
                case 'h':
                        clock_format_name(state, broken->tm_mon, false, 12,
                                          clock_month_short, clock_month_long,
                                          clock_month_long_length);
                        break;

                case 'B':
                        if (state->change_case)
                        {
                                state->to_upper = true;
                                state->to_lower = false;
                        }

                        clock_format_name(state, broken->tm_mon, true, 12,
                                          clock_month_short, clock_month_long,
                                          clock_month_long_length);
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
                                1);
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
                        clock_format_number(state, week_year, 1);
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

                /*
                        %k and %l are %H and %I written with a leading space
                        instead of a leading zero, and they are the two
                        specifiers the first draft of this file did not have.
                        They are not in C and not in POSIX; they are in glibc,
                        in BSD, and in every date(1) anybody has typed.
                */
                case 'k':
                        clock_format_number_spaced(state, broken->tm_hour, 2);
                        break;

                case 'l':
                {
                        bipolar hour = broken->tm_hour % 12;

                        clock_format_number_spaced(state,
                                                   hour == 0 ? 12 : hour, 2);
                        break;
                }

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
                        if (state->change_case)
                        {
                                state->to_upper = false;
                                state->to_lower = true;
                        }

                        clock_format_append(
                                state,
                                (address_any)clock_half_day[broken->tm_hour < 12
                                                                   ? 0
                                                                   : 1],
                                2);
                        break;

                /*
                        %P is %p in lower case, and it is lower case whatever
                        the flags say: "%^P" is "am" and the upper case flag
                        loses. Which is why the fold is forced here and why
                        there is no second table of lower case names -- the
                        fold over "AM" is "am", and one table that cannot
                        disagree with itself is worth more than two that can.
                */
                case 'P':
                        state->to_upper = false;
                        state->to_lower = true;

                        clock_format_append(
                                state,
                                (address_any)clock_half_day[broken->tm_hour < 12
                                                                   ? 0
                                                                   : 1],
                                2);
                        break;

                case 'r':
                        clock_format_nested(state, "%I:%M:%S %p", broken);
                        break;

                case 'R':
                        clock_format_nested(state, "%H:%M", broken);
                        break;

                /*
                        %s is the one number here that does not go through
                        the padding above. glibc copies its digits the way it
                        copies a weekday name, so a width in front of it pads
                        with spaces unless the 0 flag asked otherwise, and the
                        zeros of "%020s" land in front of the minus sign
                        rather than behind it. Every other number in this
                        switch does the opposite. It is not a distinction
                        anybody designed; it is where the GNU extension was
                        bolted on, and it is visible enough to match.
                */
                case 's':
                {
                        tm copy = address_to broken;
                        p8 body[40];
                        positive at;
                        positive length = clock_number_text(
                                body, (bipolar)timegm(address_of copy),
                                address_of at);

                        clock_format_append(state, body + at, length);
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
                                state, (bipolar)broken->tm_year + 1900, 1);
                        break;

                /*
                        One number and not two: the offset is printed as
                        hours times a hundred plus minutes, four digits wide,
                        which is the same bytes as two two-digit fields right
                        up until a width is named and then is not. "%6z" is
                        "     +001800" under glibc because the six applies to
                        the sign and then to the whole four-digit number, and
                        that only comes out if the four digits are one field.
                */
                case 'z':
                {
                        bipolar offset = broken->tm_gmtoff;
                        bipolar magnitude = offset < 0 ? -offset : offset;

                        clock_format_byte(state, offset < 0 ? '-' : '+');
                        clock_format_number(state,
                                            magnitude / 3600 * 100 +
                                                    magnitude / 60 % 60,
                                            4);
                        break;
                }

                case 'Z':
                        if (state->change_case)
                        {
                                state->to_upper = false;
                                state->to_lower = true;
                        }

                        if (is_null(broken->tm_zone))
                                clock_format_append(
                                        state, (address_any)clock_zone_name, 3);
                        else
                                clock_format_text(state, broken->tm_zone);
                        break;

                case '%':
                        clock_format_byte(state, '%');
                        break;

                default:
                        clock_format_verbatim(state, opened, cursor);
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

        /* A literal format is already the finished answer.  This common
           logging shape needs one terminated scan and one hardware-floor
           copy, not the directive engine once per byte. */
        if (address_to format != '%')
        {
                const char address_to directive =
                        (const char address_to)string_first_of_or_end(
                                (string_address)format, '%');

                if (address_to directive == end)
                {
                        positive length = (positive)(directive - format);

                        if (length >= max)
                                return 0;

                        if (length)
                                memory_copy_apart(into, (address_any)format,
                                                  length);

                        into[length] = end;
                        return length;
                }
        }

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
        strptime, which reads a date back out of the text strftime wrote and
        is not, whatever the symmetry of the names suggests, its inverse.

        It is not an inverse for three reasons, all of them in the standard
        and all of them surprising the first time:

          - it does not clear the structure. Fields the format never mentions
            keep whatever the caller left in them, which is what lets two
            calls with two formats fill one struct tm between them, and which
            means a caller who does not clear it first gets stack rubbish in
            the fields nobody parsed. That is the contract, not a defect.
          - it does not have to consume the whole input. It answers a pointer
            to the first byte it did not use, and a caller who cares that the
            input ended checks that byte itself. It answers null, and writes
            nothing more, when the input stopped matching the format.
          - it derives what it can and no more. Give it a year, a month and a
            day and it works out the weekday and the day of the year; give it
            a week number and it throws the week number away, because %U and
            %W do not pin a date down and glibc does not pretend they do.

        The scanning rules, which are as much of the specification as the
        specifier list is:

          - a run of whitespace in the format matches any run of whitespace in
            the input, the empty run included;
          - any other ordinary byte in the format must be that byte in the
            input;
          - a number skips leading whitespace whether the format asked for
            any or not, then takes up to as many digits as the field allows,
            and stops early rather than overflowing the field's range --
            which is why "%m%d" reads "0102" as January the second and not as
            month one thousand and two;
          - a name is matched without regard to case, and the longest name
            that matches wins, so that "Sunday" is never read as "Sun" with
            "day" left over.

        E and O are accepted in front of every specifier and mean the plain
        specifier, because in the C locale there is no era to name and no
        alternative digit to read. That is wider than glibc, which keeps a
        list of the specifiers each modifier may precede and refuses the rest
        -- and which, on the ones it does take, is wrong twice: %Ey reads its
        number, then falls into the plain %y arm without winding the input
        back and reads a second one on top of it, and the second %O in a
        format returns null where the first one fell back. Both are in the
        list of disagreements at the top of src/test/clock.c, with what glibc
        answers and what this answers instead.
*/
typedef struct clock_scan_state
{
        const char address_to at;
        const char address_to last;
        tm address_to broken;
        bipolar century;
        bool have_mday;
        bool have_wday;
        bool have_yday;
        bool have_mon;
        bool have_hour12;
        bool is_afternoon;
        bool want_century;
        bool want_day;
} clock_scan_state;

/*
        A decimal field: whitespace, then digits, then a range test.

        The whitespace is skipped whether or not the format asked for it,
        which is not symmetrical with strftime and is what every caller
        expects: "%H:%M" against " 5:06" reads five o'clock, and a format
        would have to say " %H" to get that if the skip were not here.

        The early stop is the subtle part and it is glibc's. Reading stops
        when one more digit would carry the value past the top of the field's
        range even though the field is allowed more digits -- so "%d" against
        "312" takes "31" and leaves the "2", rather than taking "312" and
        failing. A parser that took the digits first and range-checked after
        would refuse a string that glibc accepts, and "%m%d%y" against a run
        of six digits is exactly the case that breaks.
*/
static bool clock_scan_number(clock_scan_state address_to state, bipolar least,
                              bipolar most, positive digits,
                              bipolar address_to into)
{
        bipolar value = 0;

        while (byte_is_space((p8)(address_to state->at)))
                state->at++;

        if (!byte_is_digit((p8)(address_to state->at)))
                return false;

        do
        {
                value = value * 10 + ((p8)(address_to state->at) - '0');
                state->at++;
        } while (--digits > 0 && value * 10 <= most &&
                 byte_is_digit((p8)(address_to state->at)));

        if (value < least || value > most)
                return false;

        address_to into = value;

        return true;
}

/*
        One of a table of names, without regard to case, longest first.

        strptime records the input end once.  The candidates longer than what
        remains are never compared, which keeps memory_compare_ascii_case --
        counted, not terminated -- from reading past the caller's string
        without rescanning that string for every name directive. Answers
        which one, or -1.
*/
static b32 clock_scan_name(clock_scan_state address_to state,
                           const char address_to address_to table,
                           const p8 address_to lengths, positive fixed_length,
                           positive count)
{
        positive left = (positive)(state->last - state->at);
        positive best_length = 0;
        b32 best = -1;
        positive which;

        for (which = 0; which < count; which++)
        {
                positive length = fixed_length ? fixed_length : lengths[which];

                if (length <= left && length > best_length &&
                    memory_compare_ascii_case((address_any)state->at,
                                              (address_any)table[which],
                                              length) == 0)
                {
                        best = (b32)which;
                        best_length = length;
                }
        }

        if (best < 0)
                return -1;

        state->at += best_length;

        return best;
}

static bool clock_scan_core(clock_scan_state address_to state,
                            const char address_to format);

// The compound specifiers, read back through the same loop that wrote them,
// against the same format strings strftime expands them to.
static bool clock_scan_nested(clock_scan_state address_to state,
                              const char address_to format)
{
        return clock_scan_core(state, format);
}

static bool clock_scan_core(clock_scan_state address_to state,
                            const char address_to format)
{
        const char address_to cursor = format;
        tm address_to broken = state->broken;

        while (address_to cursor != end)
        {
                p8 which;
                bipolar value = 0;

                if (byte_is_space((p8)(address_to cursor)))
                {
                        cursor += string_span_of_set(
                                (string_address)cursor, " \t\n\r\v\f");
                        state->at += string_span_of_set(
                                (string_address)state->at, " \t\n\r\v\f");
                        continue;
                }

                if (address_to cursor != '%')
                {
                        positive run = string_span_without_set(
                                (string_address)cursor, "% \t\n\r\v\f");

                        if (run > (positive)(state->last - state->at) ||
                            memory_compare((address_any)cursor,
                                           (address_any)state->at, run) != 0)
                                return false;

                        state->at += run;
                        cursor += run;
                        continue;
                }

                cursor++;

                // Accepted and meaningless, both of them, in the only locale
                // there is.
                if (address_to cursor == 'E' || address_to cursor == 'O')
                        cursor++;

                which = (p8)(address_to cursor);

                if (which == end)
                        return false;

                cursor++;

                switch (which)
                {
                case 'a':
                case 'A':
                {
                        b32 found = clock_scan_name(
                                state, clock_weekday_long,
                                clock_weekday_long_length, 0, 7);

                        if (found < 0)
                                found = clock_scan_name(
                                        state, clock_weekday_short, null, 3, 7);

                        if (found < 0)
                                return false;

                        broken->tm_wday = found;
                        state->have_wday = true;
                        break;
                }

                case 'b':
                case 'B':
                case 'h':
                {
                        b32 found = clock_scan_name(
                                state, clock_month_long,
                                clock_month_long_length, 0, 12);

                        if (found < 0)
                                found = clock_scan_name(
                                        state, clock_month_short, null, 3, 12);

                        if (found < 0)
                                return false;

                        broken->tm_mon = found;
                        state->have_mon = true;
                        state->want_day = true;
                        break;
                }

                case 'c':
                        if (!clock_scan_nested(state, "%a %b %e %H:%M:%S %Y"))
                                return false;
                        break;

                case 'C':
                        if (!clock_scan_number(state, 0, 99, 2, address_of value))
                                return false;

                        state->century = value;
                        state->want_day = true;
                        break;

                case 'd':
                case 'e':
                        if (!clock_scan_number(state, 1, 31, 2, address_of value))
                                return false;

                        broken->tm_mday = (b32)value;
                        state->have_mday = true;
                        state->want_day = true;
                        break;

                case 'D':
                        if (!clock_scan_nested(state, "%m/%d/%y"))
                                return false;
                        break;

                case 'F':
                        if (!clock_scan_nested(state, "%Y-%m-%d"))
                                return false;
                        break;

                case 'H':
                case 'k':
                        if (!clock_scan_number(state, 0, 23, 2, address_of value))
                                return false;

                        broken->tm_hour = (b32)value;
                        state->have_hour12 = false;
                        break;

                case 'I':
                case 'l':
                        if (!clock_scan_number(state, 1, 12, 2, address_of value))
                                return false;

                        broken->tm_hour = (b32)(value % 12);
                        state->have_hour12 = true;
                        break;

                /*
                        The day of the year is stored and nothing is derived
                        from it on its own, which is glibc's choice and is the
                        right one: a day of the year without a year is not a
                        date, so "%j" by itself fills tm_yday and leaves the
                        month, the day and the weekday exactly as the caller
                        left them. Put a year beside it and the year turns the
                        derivation on.
                */
                case 'j':
                        if (!clock_scan_number(state, 1, 366, 3, address_of value))
                                return false;

                        broken->tm_yday = (b32)(value - 1);
                        state->have_yday = true;
                        break;

                case 'm':
                        if (!clock_scan_number(state, 1, 12, 2, address_of value))
                                return false;

                        broken->tm_mon = (b32)(value - 1);
                        state->have_mon = true;
                        state->want_day = true;
                        break;

                case 'M':
                        if (!clock_scan_number(state, 0, 59, 2, address_of value))
                                return false;

                        broken->tm_min = (b32)value;
                        break;

                case 'n':
                case 't':
                        while (byte_is_space((p8)(address_to state->at)))
                                state->at++;
                        break;

                case 'p':
                case 'P':
                {
                        b32 found = clock_scan_name(state, clock_half_day, null,
                                                   2, 2);

                        if (found < 0)
                                return false;

                        state->is_afternoon = found == 1;
                        break;
                }

                case 'r':
                        if (!clock_scan_nested(state, "%I:%M:%S %p"))
                                return false;
                        break;

                case 'R':
                        if (!clock_scan_nested(state, "%H:%M"))
                                return false;
                        break;

                /*
                        %s is the one specifier that does not fill a field, it
                        fills the whole structure: the seconds are read and
                        then broken down, and everything the format said
                        before it is overwritten. Read digit by digit rather
                        than through the field reader above, because there is
                        no range to stop at and no width to stop at either.
                */
                case 's':
                {
                        bipolar seconds = 0;
                        time_t moment;

                        if (!byte_is_digit((p8)(address_to state->at)))
                                return false;

                        do
                        {
                                seconds = seconds * 10 +
                                          ((p8)(address_to state->at) - '0');
                                state->at++;
                        } while (byte_is_digit((p8)(address_to state->at)));

                        moment = (time_t)seconds;

                        if (is_null(localtime_r(address_of moment, broken)))
                                return false;

                        break;
                }

                /*
                        Sixty one and not fifty nine: a minute with two leap
                        seconds in it was legal in the C standard long after
                        it stopped being possible in the world, and a parser
                        that refuses "23:59:60" refuses text that exists.
                */
                case 'S':
                        if (!clock_scan_number(state, 0, 61, 2, address_of value))
                                return false;

                        broken->tm_sec = (b32)value;
                        break;

                case 'T':
                        if (!clock_scan_nested(state, "%H:%M:%S"))
                                return false;
                        break;

                case 'u':
                        if (!clock_scan_number(state, 1, 7, 1, address_of value))
                                return false;

                        broken->tm_wday = (b32)(value % 7);
                        state->have_wday = true;
                        break;

                case 'w':
                        if (!clock_scan_number(state, 0, 6, 1, address_of value))
                                return false;

                        broken->tm_wday = (b32)value;
                        state->have_wday = true;
                        break;

                /*
                        Read and thrown away, all five of them. A week number
                        does not name a day, and the week-based year does not
                        name the ordinary one; taken with the rest of a
                        format they would be redundant and taken alone they
                        would be ambiguous, so glibc consumes the digits to
                        keep the format in step with the input and stores
                        nothing. This does the same rather than inventing an
                        answer glibc does not have.
                */
                case 'g':
                        if (!clock_scan_number(state, 0, 99, 2, address_of value))
                                return false;
                        break;

                /*
                        %G takes every digit it can see and not four of them,
                        which is glibc's answer to a year that has no width:
                        a week-based year is not stored, so there is no field
                        for a fifth digit to overflow and no reason to stop.
                        It means "%G%j" cannot work -- the %G swallows the day
                        of the year too -- and that is true in glibc as well.
                */
                case 'G':
                        if (!byte_is_digit((p8)(address_to state->at)))
                                return false;

                        do
                                state->at++;
                        while (byte_is_digit((p8)(address_to state->at)));
                        break;

                case 'U':
                case 'V':
                case 'W':
                        if (!clock_scan_number(state, 0, 53, 2, address_of value))
                                return false;
                        break;

                case 'x':
                        if (!clock_scan_nested(state, "%m/%d/%y"))
                                return false;
                        break;

                case 'X':
                        if (!clock_scan_nested(state, "%H:%M:%S"))
                                return false;
                        break;

                /*
                        Two digits of year, and the window the paper called
                        "Year 2000: The Millennium Rollover" chose: sixty nine
                        and up is the nineteen hundreds, sixty eight and down
                        is the two thousands. It is arbitrary and it is what
                        every other strptime does.
                */
                case 'y':
                        if (!clock_scan_number(state, 0, 99, 2, address_of value))
                                return false;

                        broken->tm_year = (b32)(value >= 69 ? value
                                                            : value + 100);
                        state->want_century = true;
                        state->want_day = true;
                        break;

                case 'Y':
                        if (!clock_scan_number(state, 0, 9999, 4, address_of value))
                                return false;

                        broken->tm_year = (b32)(value - 1900);
                        state->want_century = false;
                        state->want_day = true;
                        break;

                /*
                        A zone offset in the four spellings anybody writes:
                        a Z on its own, two digits of hours, four digits of
                        hours and minutes, and the same four with a colon in
                        the middle. The colon is only allowed where it belongs
                        and only when a digit follows it.
                */
                case 'z':
                {
                        bool behind;
                        positive taken = 0;

                        while (byte_is_space((p8)(address_to state->at)))
                                state->at++;

                        if (address_to state->at == 'Z')
                        {
                                state->at++;
                                broken->tm_gmtoff = 0;
                                break;
                        }

                        if (address_to state->at != '+' &&
                            address_to state->at != '-')
                                return false;

                        behind = address_to state->at == '-';
                        state->at++;

                        while (taken < 4 &&
                               byte_is_digit((p8)(address_to state->at)))
                        {
                                value = value * 10 +
                                        ((p8)(address_to state->at) - '0');
                                state->at++;
                                taken++;

                                if (address_to state->at == ':' &&
                                    taken == 2 &&
                                    byte_is_digit((p8)(state->at[1])))
                                        state->at++;
                        }

                        if (taken == 2)
                                value *= 100;
                        else if (taken != 4)
                                return false;
                        else if (value % 100 >= 60)
                                return false;

                        /*
                                The minutes are checked and the hours are not,
                                which is glibc's asymmetry: "+9959" is an
                                offset of ninety nine hours and it is taken,
                                "+0060" is sixty minutes and it is refused.
                                Nothing on Earth is more than fourteen hours
                                from Greenwich, but a parser is not the place
                                to have opinions about that.
                        */
                        broken->tm_gmtoff =
                                value / 100 * 3600 + value % 100 * 60;

                        if (behind)
                                broken->tm_gmtoff = -broken->tm_gmtoff;

                        break;
                }

                /*
                        %Z takes a word and does nothing with it.

                        There is no table of zone abbreviations to match
                        against, and there could not be a right one: three
                        letters name two different zones often enough that
                        any table would be wrong somewhere. So the word is
                        stepped over so that the format stays in step with the
                        input, and the caller who cares which zone it was
                        reads it out of the input itself. "Word" is literally
                        that -- whitespace, then everything up to the next
                        whitespace -- so "%Z %Y" against "2000-02-29" fails,
                        because the zone ate the date.
                */
                case 'Z':
                        while (byte_is_space((p8)(address_to state->at)))
                                state->at++;

                        while (address_to state->at != end &&
                               !byte_is_space((p8)(address_to state->at)))
                                state->at++;
                        break;

                case '%':
                        if (address_to state->at != '%')
                                return false;

                        state->at++;
                        break;

                default:
                        return false;
                }
        }

        return true;
}

/*
        What the fields imply, worked out once at the end rather than as each
        one arrives, because the order they arrive in is the format's business
        and not this routine's: "%d %m %Y" has to reach the same answer as
        "%Y %m %d", so nothing is derived until there is nothing left to read.

        Three derivations, in the order they depend on each other. The century
        adjusts the year. A day of the year, with no month or day of the month
        beside it, becomes both. And a year, a month and a day become the
        weekday and, if it was not given, the day of the year.
*/
static fn clock_scan_settle(clock_scan_state address_to state)
{
        tm address_to broken = state->broken;
        bipolar year;

        if (state->have_hour12 && state->is_afternoon)
                broken->tm_hour += 12;

        if (state->century >= 0)
        {
                if (state->want_century)
                        broken->tm_year = (b32)(broken->tm_year % 100 +
                                                (state->century - 19) * 100);
                else
                        broken->tm_year = (b32)((state->century - 19) * 100);
        }

        if (!state->want_day)
                return;

        year = (bipolar)broken->tm_year + 1900;

        if (!state->have_wday)
        {
                if (!(state->have_mon && state->have_mday) && state->have_yday)
                {
                        bipolar found_year;
                        bipolar found_month;
                        bipolar found_day;

                        clock_civil_from_days(
                                clock_days_from_civil(year, 1, 1) +
                                        (bipolar)broken->tm_yday,
                                address_of found_year, address_of found_month,
                                address_of found_day);

                        if (!state->have_mon)
                                broken->tm_mon = (b32)(found_month - 1);

                        if (!state->have_mday)
                                broken->tm_mday = (b32)found_day;

                        state->have_mon = true;
                        state->have_mday = true;
                }

                if (state->have_mon ||
                    (broken->tm_mon >= 0 && broken->tm_mon <= 11))
                        broken->tm_wday = (b32)clock_weekday_from_days(
                                clock_days_from_civil(
                                        year, (bipolar)broken->tm_mon + 1,
                                        (bipolar)broken->tm_mday));
        }

        if (!state->have_yday &&
            (state->have_mon || (broken->tm_mon >= 0 && broken->tm_mon <= 11)))
                broken->tm_yday = (b32)(clock_days_from_civil(
                                                year,
                                                (bipolar)broken->tm_mon + 1,
                                                (bipolar)broken->tm_mday) -
                                        clock_days_from_civil(year, 1, 1));
}

p8 address_to strptime(const char address_to input, const char address_to format,
                       tm address_to broken)
{
        clock_scan_state state;

        if (is_null(input) || is_null(format) || is_null(broken))
                return null;

        state.at = input;
        state.last = (const char address_to)string_first_of_or_end(
                (string_address)input, end);
        state.broken = broken;
        state.century = -1;
        state.have_mday = false;
        state.have_wday = false;
        state.have_yday = false;
        state.have_mon = false;
        state.have_hour12 = false;
        state.is_afternoon = false;
        state.want_century = false;
        state.want_day = false;

        if (!clock_scan_core(address_of state, format))
                return null;

        clock_scan_settle(address_of state);

        return (p8 address_to)state.at;
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
