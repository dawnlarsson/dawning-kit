/*
        Experimental C standard library

        scanf: a format string, an input, and the values it takes out of it

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_SCAN
#define STANDARD_MODERN_C_STANDARD_SCAN

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason text.c gives beside the same two words. core.c includes
        this umbrella, library.c sets KERNEL_MODE from __MODULE__, and a
        kernel has no business carrying a float parser -- arm64 refuses a
        decimal in a signature there whether or not anything calls it, which
        is why math.c and format.c are each one guarded block as well.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        THIS IS PRINTF READ BACKWARDS, AND IT IS NOT SYMMETRIC

        format.c decides which byte goes where. This decides how far to read
        before it knows, which is the harder half: a formatter can measure its
        answer and then emit it, while a parser has to commit to consuming a
        byte before it can tell whether the byte belongs to the thing it is
        reading. Every corner of scanf is that one problem wearing a different
        hat, and C answers it with one sentence in 7.21.6.2 paragraph 9 that
        the whole of this file is an implementation of:

            the input item is the longest sequence of input characters which
            does not exceed any specified field width and which is, or is a
            prefix of, a matching input sequence

        Read that twice. It does not say "read while the bytes are valid". It
        says read while what you have could still GROW into something valid,
        and only then ask whether what you have is itself valid. The two are
        different at exactly the places a hand-written scanf is wrong:

            "0x"    with %x   -- a prefix of "0x1", so both bytes are eaten,
                                 and then it is not a number: matching failure
            "0x1f"  with %2x  -- the width stops it at "0x", same answer
            "1e+"   with %f   -- a prefix of "1e+5", eaten, matching failure
            "infinit" with %f -- a prefix of "infinity", eaten, and "inf" is
                                 NOT the answer even though it was passed
            "nan("  with %f   -- a prefix of "nan(1)", eaten, no fallback
            "- 5"   with %d   -- "-" is a prefix of "-5", eaten, then a space
            "0xz"   with %f   -- "0x" is a prefix, "0xz" is not, so the item
                                 is "0x" and the answer is a matching failure
                                 rather than the zero that strtod would give

        A real glibc answers every one of those the way this file does, and
        every one of them was measured before a line here was written rather
        than reasoned about afterwards. The last of them is the reason this
        file does its own syntax scan for %f instead of handing the rest of
        the input to strtod and believing the pointer it returns: strtod is
        specified to take the longest sequence that IS valid, which for "0xz"
        is "0", and scanf is specified to take the longest sequence that MAY
        BECOME valid, which is "0x". They differ on the input and they differ
        on the answer, so the split here is scan the shape in this file, and
        convert the value with the one float parser the tree has.

        The other thing worth writing down before the code: because the rule
        is "read while it could still grow", nothing here ever backtracks.
        The scan stops at the first byte that cannot extend a prefix and that
        one byte goes back. One byte, never two, which is exactly what the C
        standard promises a stream can take and exactly what stream.c's
        ungetc guarantees -- its array holds eight and this needs one of them.
        An implementation that tried "inf" after "infinity" failed would need
        five, and would be wrong as well as greedy.
*/

/*
        THE SEAM ONTO THE FLOAT PARSER

        There is no float parser here and there should not be. This file
        decides where a number ends -- which is a different question from
        which digits are valid, and the note above says why -- and hands the
        bytes it decided on to the family that turns digits into values.

        That family is numbers.c and it exports the three C spells for the
        three widths: strtof for a float, strtod for a double, strtold for a
        long double, each taking a string and the address of a pointer that
        comes back saying where the parse stopped. Nothing here reads that
        pointer. The scan above already decided where the number ends and the
        buffer handed over holds exactly that and no more, so the answer is
        always its terminator; a parser that ignores a null ending is fine.

        All three are used, one per width, which is not a nicety. Parsing to a
        double and narrowing to a float is wrong twice: it rounds twice, which
        can land a value one place off at the bottom of the float, and it
        throws away a not-a-number's payload, which "%f" against "nan(1)"
        shows immediately. glibc calls strtof there and so does this.

        Where numbers.c is absent the seam still closes, on the one entry that
        cannot be done without: strtod is declared and the other two widths
        are the double narrowed and the double widened. That configuration is
        what the whole family was developed and diffed against, before
        numbers.c landed, and the two rounding divergences above are its and
        not scanf's. Either way the merge is include order and nothing else --
        numbers.c, then stream.c, then format.c, then this.
*/
#if defined(STANDARD_MODERN_C_STANDARD_NUMBERS) && decimal_bits == 64

#define SCAN_DECIMAL_DECLARED 1
//      The three take a const char * and everything here carries a
//      string_address, which is the same pointer wearing the other
//      signedness. The cast is in the macro so the twenty odd call sites
//      below do not each have to say it.
#define SCAN_DECIMAL_NARROW(where, stop) \
        strtof((const char address_to)(where), (char address_to address_to)(stop))
#define SCAN_DECIMAL_WIDE(where, stop) \
        strtold((const char address_to)(where), (char address_to address_to)(stop))

#endif

#ifndef SCAN_DECIMAL
#define SCAN_DECIMAL(where, stop) \
        strtod((const char address_to)(where), (char address_to address_to)(stop))
#endif

#ifndef SCAN_DECIMAL_DECLARED
decimal SCAN_DECIMAL(string_address text, string_address address_to ending);
#endif

/*
        Whether the stream family is under this one.

        fscanf, scanf and their var_args forms need a FILE to read from and a
        byte of pushback to give back, and both are stream.c's. Without it the
        string entries still build and still work, which is how the sscanf
        half was first tested; there is deliberately no descriptor fallback
        the way format.c has one for writing, because a raw descriptor has no
        end-of-file indicator, no error indicator and no pushback, and three
        of scanf's answers are about exactly those.
*/
#ifdef STANDARD_MODERN_C_STANDARD_STREAM
#define SCAN_STREAMS 1
typedef FILE address_to scan_stream;
#else
#define SCAN_STREAMS 0
typedef address_any scan_stream;
#endif

/*
        Out of range, reported the way strtol reports it.

        glibc's scanf converts through strtol and strtoul and lets their
        ERANGE stand, so a %d that overflows sets errno and stores the
        saturated value rather than failing the conversion. That is measured
        behaviour and is matched below. Where the error family is absent there
        is no errno to set and the value is still saturated, which is the only
        part a caller without errno could have observed anyway.
*/
#ifdef STANDARD_MODERN_C_STANDARD_ERROR
#define scan_out_of_range() (errno = ERANGE)
#define scan_errno_save(into) ((into) = errno, errno = 0)
#define scan_errno_keep(from) ((fn)(errno == 0 ? (errno = (from)) : errno))
#define scan_errno_clear() (errno = 0)
#else
#define scan_out_of_range() ((fn)0)
#define scan_errno_save(into) ((into) = 0)
#define scan_errno_keep(from) ((fn)(from))
#define scan_errno_clear() ((fn)0)
#endif

/*
        The six bytes isspace calls white, written once. The umbrella folds a
        literal set into a 256 byte table and a jump into string_span, so
        every use of this below is a table lookup per byte and no set build.
*/
#define scan_white " \t\n\v\f\r"

/*
        How much of one number is kept.

        The scan consumes every byte the input item has, however many that is,
        because the position it leaves behind is part of the answer. What it
        STORES is capped, and the cap is here. An integer needs at most twenty
        significant digits before the answer is "too big" and nothing past
        that can change it, so the cap is unreachable there by construction --
        the leading zeros are not stored at all and a run of more than a
        thousand significant digits has overflowed a hundred times over.

        A float is the one place the cap can be reached and change an answer,
        and it takes a literal with more than a thousand characters in its
        mantissa to do it. Past that the stored text is truncated, the
        conversion is of the truncated text, and the consumption and the
        return value are still exact. It is written down rather than hidden
        because it is a real divergence from a glibc that allocates.
*/
#define scan_stage_bytes 1024

/*
        WHERE THE BYTES COME FROM

        One structure for both halves, because every directive below would
        otherwise be written twice. A string source walks a pointer; a stream
        source calls stream.c. The difference shows up in three places and
        they are all marked: pushback, the whole-run scans, and the end.

        held is the stream's pushback and the string source never uses it. A
        string can put a byte back by walking backwards, which costs nothing
        and has no depth limit, and keeping the string source's pushback empty
        is what lets the run scans below hand a whole run to the library in
        one call instead of a byte at a time. A stream cannot walk backwards,
        so its returned byte is kept here until the scan is over and then
        pushed into stream.c's array -- once, at the end, rather than through
        ungetc in the middle, so that nothing here disturbs the end-of-file
        indicator that feof is about to be asked for.

        consumed is what %n reports. It counts a byte when the byte is handed
        out and uncounts it when the byte goes back, so a lookahead that was
        rejected is not in the number, which is the whole of what %n is for.

        ran_out is not about the answer either; it is about errno, and the
        note beside scan_leave is what it is for. A white space skip whose
        FIRST read is the end of the source is what glibc treats as an input
        failure even on a call that then succeeds, and telling that apart
        from a skip that read a space and then the end is one bit of state
        that nothing else in the file can see.

        ended is this file's own memory of end-of-file and is not the stream's
        indicator. The two are deliberately separate: the stream's is what
        feof answers and must not be cleared by anything here, and this one is
        cleared whenever a byte goes back, because a source holding a byte is
        not at its end.
*/
typedef struct scan_source scan_source;

struct scan_source
{
        string_address text;
        positive place;

        scan_stream handle;

        positive consumed;
        positive held;
        p8 holding[8];

        bipolar entered_errno;

        bool ended;
        bool ran_out;
};

#define scan_is_text(source) ((source)->handle == null)

static fn scan_text_take(scan_source address_to source, p8 address_to into,
                         positive count)
{
        if (into != null && count != 0)
                memory_copy(into, source->text + source->place, count);

        source->place += count;
        source->consumed += count;
}

/*
        One byte out, and one byte back.

        EOF is never put into the holding array and never given to stream.c's
        ungetc, which refuses it: the end of a source is a fact about the
        source and not a byte it is holding. scan_unget of EOF is therefore a
        deliberate no-op, so that every caller can push back whatever stopped
        it without first asking whether anything did.
*/
static b32 scan_get(scan_source address_to source)
{
        b32 byte;

        if (source->held != 0)
        {
                source->held--;
                source->consumed++;

                return (b32)source->holding[source->held];
        }

#if SCAN_STREAMS
        if (source->handle != null)
        {
                byte = stream_get_byte(source->handle);

                if (byte == EOF)
                {
                        source->ended = true;

                        return EOF;
                }

                source->consumed++;

                return byte;
        }
#endif

        if (string_get(source->text + source->place) == end)
        {
                source->ended = true;

                return EOF;
        }

        byte = (b32)source->text[source->place];
        source->place++;
        source->consumed++;

        return byte;
}

static fn scan_unget(scan_source address_to source, b32 byte)
{
        if (byte == EOF)
                return;

        source->consumed--;
        source->ended = false;

#if SCAN_STREAMS
        if (source->handle != null)
        {
                source->holding[source->held] = (p8)byte;
                source->held++;

                return;
        }
#endif

        source->place--;
}

/*
        The end of the call, which is where a stream gets its byte back.

        stream.c's pushback is a stack, so a byte pushed last comes out first,
        and the array here is a stack for the same reason. Handing one to the
        other therefore walks this one from the bottom: the byte at index zero
        was returned first, so it is the one that must end up deepest.

        In practice there is one byte here at most, because the prefix rule at
        the top of this file never needs two. The loop is written for the
        array's size rather than for one because a routine that is correct
        only for the case that happens to occur is a routine that is wrong the
        first time the case changes.
*/
static fn scan_finish(scan_source address_to source)
{
#if SCAN_STREAMS
        positive index;

        if (source->handle == null)
                return;

        for (index = 0; index < source->held; index++)
                stream_unget_byte((b32)source->holding[index], source->handle);
#endif

        source->held = 0;
}

/*
        A run of white space, skipped.

        This is the one scan where the two sources differ in shape rather than
        only in mechanism. A string has the whole run in front of it and hands
        it to string_span, which the umbrella has already folded into a table
        lookup per byte over the six white bytes; a stream has one byte at a
        time and no buffer this file is allowed to reach into, so it asks
        byte_is_space per byte -- the library's classifier, not a comparison
        chain written here.

        Reaching into stream.c's buffer would make this one call as well, and
        it is deliberately not done: format.c writes through stream_put_bytes
        for the same reason. The buffer, its direction and its refill are that
        file's, and a second file that knew where the bytes were would have to
        be right about all three forever.
*/
static fn scan_skip_white(scan_source address_to source)
{
        b32 byte;

        if (scan_is_text(source))
        {
                positive run = string_span_of_set(source->text + source->place,
                                                  (string_address)scan_white);

                if (run == 0 &&
                    string_get(source->text + source->place) == end)
                        source->ran_out = true;

                source->place += run;
                source->consumed += run;

                return;
        }

        byte = scan_get(source);

        if (byte == EOF)
        {
                source->ran_out = true;

                return;
        }

        while (byte != EOF && byte_is_space(byte))
                byte = scan_get(source);

        scan_unget(source, byte);
}

/*
        WHAT ONE DIRECTIVE ASKS FOR

        The star is assignment suppression, the width is a byte count and zero
        means none -- a written "%0d" is a width of zero, which C leaves
        undefined and glibc reads as no width at all, measured and matched.
        The length is the width of the object being written and not the width
        of the value: every one of long, long long, size_t, ptrdiff_t and
        intmax_t is eight bytes on all three machines this builds for, so the
        eight distinct modifiers land on four distinct stores.
*/
#define SCAN_LENGTH_CHAR 1
#define SCAN_LENGTH_SHORT 2
#define SCAN_LENGTH_INT 4
#define SCAN_LENGTH_WIDE 8
#define SCAN_LENGTH_DECIMAL 9
#define SCAN_LENGTH_WIDE_DECIMAL 10

/*
        The staging buffer and what is in it.

        Two very different fills share it, which is why the counts are named
        rather than one length. A float stages the raw text it matched, ready
        for strtod, and staged is how much of it fits. An integer stages only
        its SIGNIFICANT digits -- the leading zeros are dropped on the way in,
        because the only two questions asked of that run afterwards are how
        many digits it has and whether they are above the largest value the
        machine holds, and a leading zero answers neither.

        significant is the true count and staged is how much of it was kept.
        They differ only past the cap, and past the cap the answer is already
        "too big" whatever the digits are.
*/
typedef struct scan_stage scan_stage;

struct scan_stage
{
        p8 bytes[scan_stage_bytes + 1];
        positive staged;
        positive significant;
};

static fn scan_stage_clear(scan_stage address_to stage)
{
        stage->staged = 0;
        stage->significant = 0;
}

//      A raw byte, for the float scan, kept while there is room.
static fn scan_stage_byte(scan_stage address_to stage, b32 byte)
{
        if (stage->staged < scan_stage_bytes)
        {
                stage->bytes[stage->staged] = (p8)byte;
                stage->staged++;
        }
}

//      A digit, for the integer scan, with a leading zero counted by neither.
static fn scan_stage_digit(scan_stage address_to stage, b32 byte)
{
        if (stage->significant == 0 && byte == '0')
                return;

        stage->significant++;

        if (stage->staged < scan_stage_bytes)
        {
                stage->bytes[stage->staged] = (p8)byte;
                stage->staged++;
        }
}

/*
        Is this byte a digit in this base?

        Four bases, because %i can read a base out of its own input and one
        of the four it can find is two. %o is eight, %x and %p are sixteen,
        %i is whichever of two, eight, ten and sixteen its prefix said, and
        everything else is ten. The two that a classifier covers are the
        library's branchless ones rather than comparisons written here; octal
        is the decimal classifier with its top two values removed, which is
        the shape the classifier already has, and binary is two values and
        has nothing worth borrowing.
*/
static bool scan_digit_of_base(b32 byte, positive base)
{
        if (base == 16)
                return byte_is_hexadecimal(byte) != 0;

        if (base == 8)
                return byte_is_digit(byte) != 0 && byte <= '7';

        if (base == 2)
                return byte == '0' || byte == '1';

        return byte_is_digit(byte) != 0;
}

/*
        THE MAGNITUDE, AND WHETHER IT FIT

        string_digits_base_max is the library's bounded digit run and it is
        the whole of the arithmetic here: a slice of digits, a bound, a base,
        and the number they spell. It wraps silently, which is the right
        contract for the thirty three parsers it was written for and the wrong
        one for scanf, because glibc converts through strtoul and saturates.

        So overflow is decided before the routine is called, by counting
        rather than by arithmetic. The largest value the machine holds has a
        fixed number of digits in any base -- twenty in ten, sixteen in
        sixteen, twenty two in eight -- and a run of significant digits that
        is shorter than that cannot overflow, a run that is longer always
        does, and a run of exactly that length is decided by comparing the two
        strings. That comparison is a plain memcmp because the digits of every
        base up to thirty six rise through ASCII in the same order as their
        values once the letters are folded down: '0' through '9' at 0x30, then
        'a' through 'z' at 0x61, with nothing out of order in between. The
        fold is memory_to_lower_ascii over the slice, which is assembly, and
        the largest value's own digits come from positive_into_base, which is
        the same assembly the formatter uses.

        A leading zero is not a significant digit and never reaches this,
        because the scan above declined to store it: "0000000000000000000005"
        is twenty two digits and one significant one, and a count that took
        the leading zeros in would call every one of those an overflow. That
        is why the staging buffer keeps two counts rather than a length.

        Nothing here looks at a digit one at a time. The comparison is
        memory_compare, the fold is memory_to_lower_ascii, the limit's digits
        are positive_into_base and the value is string_digits_base_max, and
        each of those four is assembly at three-architecture parity.
*/
static positive scan_magnitude(scan_stage address_to stage, positive base,
                               bool address_to overflowed)
{
        p8 largest[72];
        positive largest_length;

        address_to overflowed = false;

        if (stage->significant == 0)
                return 0;

        if (stage->significant > stage->staged)
        {
                //      More digits than the buffer kept, and the buffer holds
                //      a thousand. Nothing that long is representable.
                address_to overflowed = true;

                return 0;
        }

        largest_length = positive_into_base(largest, ~(positive)0, base, false);

        if (stage->significant > largest_length)
        {
                address_to overflowed = true;

                return 0;
        }

        if (stage->significant == largest_length)
        {
                memory_to_lower_ascii(stage->bytes, stage->staged);

                if (memory_compare(stage->bytes, largest, largest_length) > 0)
                {
                        address_to overflowed = true;

                        return 0;
                }
        }

        return string_digits_base_max(stage->bytes, stage->staged, base, null);
}

/*
        THE INTEGER SCAN

        One routine for %d %i %u %o %x %X and %p's hexadecimal half, because
        they differ only in which base they start from and whether a "0x" is
        allowed to change it. base is the base to read in, or zero for %i,
        which reads the base out of the input the way C source does.

        The order below is the grammar and the grammar is the prefix rule:
        the sign is taken before anything says a digit follows it, the "0x" is
        taken before anything says a hexadecimal digit follows it, and both
        are what makes "-" and "0x" matching failures rather than pushed-back
        bytes. Only ONE byte is ever pushed back, and it is the byte that
        could not extend the prefix.

        digits is how many digits were seen at all and decides whether this
        matched. The stage's significant count is how many of them were not
        leading zeros and decides the value. "0" has one of the first and none
        of the second, which is exactly the distinction the two counts exist
        for.
*/
static bool scan_integer(scan_source address_to source, scan_stage address_to stage,
                         positive width, positive base, bool address_to negative,
                         positive address_to base_used)
{
        positive limit = width != 0 ? width : ~(positive)0;
        positive taken = 0;
        positive digits = 0;
        bool letter_last = false;
        b32 byte;

        scan_stage_clear(stage);
        address_to negative = false;

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == '+' || byte == '-')
                {
                        address_to negative = byte == '-';
                        taken++;
                }
                else
                {
                        scan_unget(source, byte);
                }
        }

        //      The base prefix, which only two of the conversions have. %i
        //      has no base yet and reads one here; %x and %p have sixteen
        //      already and allow the prefix to be written anyway.
        if ((base == 0 || base == 16) && taken < limit)
        {
                byte = scan_get(source);

                if (byte == '0')
                {
                        taken++;
                        digits++;
                        scan_stage_digit(stage, byte);

                        if (taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == 'x' || byte == 'X')
                                {
                                        //      The prefix is taken and the
                                        //      digit it promised has not
                                        //      arrived yet. If none does,
                                        //      this is a matching failure
                                        //      with both bytes eaten.
                                        taken++;
                                        base = 16;
                                        digits = 0;
                                        letter_last = true;
                                        scan_stage_clear(stage);
                                }
                                else if (base == 0 &&
                                         (byte == 'b' || byte == 'B'))
                                {
                                        //      "0b" is C23's binary literal
                                        //      and glibc reads it here, in
                                        //      %i and nowhere else -- an
                                        //      explicit %x never takes it,
                                        //      because sixteen was not asked
                                        //      to go looking for a base.
                                        taken++;
                                        base = 2;
                                        digits = 0;
                                        letter_last = true;
                                        scan_stage_clear(stage);
                                }
                                else
                                {
                                        scan_unget(source, byte);

                                        if (base == 0)
                                                base = 8;
                                }
                        }
                        else if (base == 0)
                        {
                                base = 8;
                        }
                }
                else
                {
                        scan_unget(source, byte);

                        if (base == 0)
                                base = 10;
                }
        }

        while (taken < limit)
        {
                byte = scan_get(source);

                if (byte == EOF)
                        break;

                if (!scan_digit_of_base(byte, base))
                {
                        scan_unget(source, byte);
                        break;
                }

                taken++;
                digits++;
                letter_last = false;
                scan_stage_digit(stage, byte);
        }

        /*
                One byte past the end, looked at and put straight back.

                A run that stopped because it ran out of width has not yet
                touched the byte after it, and glibc has: its digit loop
                reads the next byte before it tests the width, so a "%3d"
                that ends exactly at the end of a file leaves feof set and
                one that ends anywhere else leaves the byte pushed back. The
                position is the same either way and the indicator is not, so
                the look is taken here too rather than leaving feof to
                disagree after every bounded conversion.

                Not when the width ran out on the "x" of a "0x" or the "b"
                of a "0b" itself, which is the one place glibc stops without
                looking. "%2x" against "0x" consumes both bytes, answers a
                matching failure and leaves the end-of-file indicator alone,
                while "%1i" against "0" consumes its one byte, answers zero
                and DOES set it. Both are measured, both are one line apart,
                and there is no reading of the standard that predicts either.
        */
        if (taken == limit && !letter_last)
                scan_unget(source, scan_get(source));

        address_to base_used = base;

        return digits != 0;
}

/*
        THE FLOAT SCAN

        The shape only. The value is strtod's, and the text handed to it is
        exactly the bytes matched here and nothing after them.

        Four alternatives share one sign: a hexadecimal significand with an
        optional binary exponent, a decimal significand with an optional
        decimal exponent, an infinity spelled either "inf" or "infinity", and
        a not-a-number optionally carrying a parenthesised tag. All four are
        scanned as prefixes and each says separately whether what was taken is
        also complete, which is the difference between "inf" -- complete at
        three bytes -- and "infinit", which is a longer prefix of the same
        word and complete at nothing.

        The numeric arm is written as flags rather than as states because the
        two bases differ only in which byte introduces the exponent and which
        classifier says what a digit is. Everything else -- one point at most,
        an exponent only after a digit, a sign only immediately after the
        exponent letter, at least one digit in the exponent once the letter is
        there -- is the same sentence for both, and writing it twice would be
        writing the same four rules twice.
*/
static bool scan_decimal_shape(scan_source address_to source,
                               scan_stage address_to stage, positive width)
{
        positive limit = width != 0 ? width : ~(positive)0;
        positive taken = 0;
        b32 byte;

        scan_stage_clear(stage);

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == '+' || byte == '-')
                {
                        scan_stage_byte(stage, byte);
                        taken++;
                }
                else
                {
                        scan_unget(source, byte);
                }
        }

        if (taken < limit)
        {
                byte = scan_get(source);

                if (byte == EOF)
                        return false;

                if (byte_to_lower(byte) == 'i' || byte_to_lower(byte) == 'n')
                {
                        string_address word = byte_to_lower(byte) == 'i'
                                                      ? (string_address) "infinity"
                                                      : (string_address) "nan";
                        positive full = byte_to_lower(byte) == 'i' ? 8 : 3;
                        positive matched = 1;
                        bool tagged = false;
                        bool closed = false;

                        scan_stage_byte(stage, byte);
                        taken++;

                        while (matched < full && taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == EOF)
                                        break;

                                if ((b32)(p8)byte_to_lower(byte) !=
                                    (b32)word[matched])
                                {
                                        scan_unget(source, byte);
                                        break;
                                }

                                scan_stage_byte(stage, byte);
                                taken++;
                                matched++;
                        }

                        //      "nan" may carry a tag, and the tag is a prefix
                        //      the same way the word is: an open bracket with
                        //      no close is a longer prefix of something valid
                        //      and therefore not an answer.
                        if (full == 3 && matched == 3 && taken < limit)
                        {
                                byte = scan_get(source);

                                if (byte == '(')
                                {
                                        scan_stage_byte(stage, byte);
                                        taken++;
                                        tagged = true;

                                        while (taken < limit)
                                        {
                                                byte = scan_get(source);

                                                if (byte == EOF)
                                                        break;

                                                if (byte == ')')
                                                {
                                                        scan_stage_byte(stage,
                                                                        byte);
                                                        taken++;
                                                        closed = true;
                                                        break;
                                                }

                                                if (!byte_is_alnum(byte) &&
                                                    byte != '_')
                                                {
                                                        scan_unget(source, byte);
                                                        break;
                                                }

                                                scan_stage_byte(stage, byte);
                                                taken++;
                                        }
                                }
                                else
                                {
                                        scan_unget(source, byte);
                                }
                        }

                        if (tagged)
                                return closed;

                        //      Three bytes is "inf" and eight is "infinity",
                        //      and the five lengths between them are prefixes
                        //      of the longer word and answers to nothing.
                        return matched == 3 || matched == full;
                }

                scan_unget(source, byte);
        }

        {
                bool hexadecimal = false;
                bool pointed = false;
                bool exponent = false;
                bool exponent_signed = false;
                positive significand = 0;
                positive exponent_digits = 0;

                if (taken < limit)
                {
                        byte = scan_get(source);

                        if (byte == '0')
                        {
                                scan_stage_byte(stage, byte);
                                taken++;
                                significand++;

                                if (taken < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == 'x' || byte == 'X')
                                        {
                                                scan_stage_byte(stage, byte);
                                                taken++;
                                                hexadecimal = true;
                                                significand = 0;
                                        }
                                        else
                                        {
                                                scan_unget(source, byte);
                                        }
                                }
                        }
                        else
                        {
                                scan_unget(source, byte);
                        }
                }

                while (taken < limit)
                {
                        byte = scan_get(source);

                        if (byte == EOF)
                                break;

                        if (!exponent)
                        {
                                if (scan_digit_of_base(byte,
                                                       hexadecimal ? 16 : 10))
                                {
                                        significand++;
                                }
                                else if (byte == '.' && !pointed)
                                {
                                        pointed = true;
                                }
                                else if (significand != 0 &&
                                         byte_to_lower(byte) ==
                                             (hexadecimal ? 'p' : 'e'))
                                {
                                        exponent = true;
                                }
                                else
                                {
                                        scan_unget(source, byte);
                                        break;
                                }
                        }
                        else if (exponent_digits == 0 && !exponent_signed &&
                                 (byte == '+' || byte == '-'))
                        {
                                exponent_signed = true;
                        }
                        else if (byte_is_digit(byte))
                        {
                                exponent_digits++;
                        }
                        else
                        {
                                scan_unget(source, byte);
                                break;
                        }

                        scan_stage_byte(stage, byte);
                        taken++;
                }

                return significand != 0 &&
                       (!exponent || exponent_digits != 0);
        }
}

/*
        THE SCAN SET

        %[ builds a table of two hundred and fifty six answers on the stack
        and then hands it to string_span, which is the library's table-driven
        run scanner and the same routine the umbrella's folded literal sets
        jump into. The table cannot be folded here because the members are
        read at run time out of the format, but the scan over the input is the
        same assembly either way.

        The four rules that every implementation gets at least one of wrong,
        each measured against glibc rather than reasoned about:

            a ']' immediately after the '[' or after the '^' is a MEMBER and
            not the close, so "%[]]" is the set holding one close bracket

            a '-' that is first, or last before the ']', is a member, so
            "%[a-]" is 'a' and a dash and "%[-a]" is a dash and 'a'

            a '-' between two members is a range, and the range runs from the
            member most recently added, so "%[a-c-e]" is 'a' through 'e'

            a range whose ends are the wrong way round is not an empty range
            and not an error: glibc adds the two ends and the dash as three
            plain members, so "%[z-a]" matches "z-a" and nothing else

        A format that runs out before its ']' is not a scan set at all. glibc
        stops the whole call there and returns what it had already assigned,
        which is zero rather than EOF even on empty input -- a broken format
        is not an input failure -- and that is what the caller does with the
        null this returns.
*/
static string_address scan_build_set(string_address at, b8 address_to set,
                                     bool address_to negated)
{
        b32 previous = -1;

        memory_zero(set, 256);
        address_to negated = false;

        if (string_get(at) == '^')
        {
                address_to negated = true;
                at++;
        }

        if (string_get(at) == ']')
        {
                set[']'] = 1;
                previous = ']';
                at++;
        }

        while (string_get(at) != end && string_get(at) != ']')
        {
                b32 member = (b32)string_get(at);

                if (member == '-' && previous >= 0 && at[1] != end &&
                    at[1] != ']')
                {
                        b32 last = (b32)at[1];

                        if (previous <= last)
                        {
                                b32 value;

                                for (value = previous; value <= last; value++)
                                        set[value] = 1;
                        }
                        else
                        {
                                set['-'] = 1;
                                set[last] = 1;
                        }

                        previous = last;
                        at += 2;

                        continue;
                }

                set[member] = 1;
                previous = member;
                at++;
        }

        if (string_get(at) != ']')
                return null;

        if (address_to negated)
        {
                positive value;

                for (value = 0; value < 256; value++)
                        set[value] = !set[value];
        }

        return at + 1;
}

/*
        THE STORE

        One place where a converted value becomes bytes in the caller's
        object, so that the eight length modifiers are read once rather than
        at every conversion. long, long long, size_t, ptrdiff_t and intmax_t
        are all eight bytes on x86_64, arm64 and riscv64, so there are four
        integer widths here and not eight, and the narrowing is the store
        itself: glibc converts at the full width of the machine word, saturates
        there, and then truncates into whatever the caller pointed at, which
        is why "%hhd" of 300 stores 44 and "%d" of a number past a signed long
        stores minus one.
*/
static fn scan_store_whole(address_any into, positive length, positive value)
{
        switch (length)
        {
        case SCAN_LENGTH_CHAR:
                address_to (p8 address_to)into = (p8)value;
                break;
        case SCAN_LENGTH_SHORT:
                address_to (p16 address_to)into = (p16)value;
                break;
        case SCAN_LENGTH_INT:
                address_to (p32 address_to)into = (p32)value;
                break;
        default:
                address_to (positive address_to)into = value;
                break;
        }
}

/*
        A DOUBLE, WRITTEN INTO A LONG DOUBLE, WITHOUT ASKING THE COMPILER

        %Lf points at an object this tree has no arithmetic for. Writing to it
        with a cast -- "(f128)value" -- compiles on x86_64, where the widening
        is one x87 instruction, and does not link on arm64 or riscv64, where
        long double is IEEE binary128 and the widening is a call into libgcc
        that a -nostdlib link has no copy of. Writing eight bytes of double
        into a sixteen byte object instead is not a precision divergence, it
        is a wrong object, so neither of those is the answer.

        The answer is that both formats are the same three fields in different
        places, and moving a field is a shift. x86_64's long double is the x87
        eighty bit extended: fifteen bits of exponent, then an EXPLICIT
        integer bit that IEEE's other formats leave implied, then sixty three
        bits of fraction, in a sixteen byte object with six bytes of nothing
        on the end. arm64 and riscv64 use binary128: fifteen bits of exponent
        and a hundred and twelve of fraction with the integer bit implied as
        usual. Every double is exactly representable in both, so the move is
        exact and there is no rounding to get wrong.

        A subnormal double is a NORMAL number in either wide format, because
        both have room for exponents a double cannot reach, so its leading
        zeros have to come off the fraction and go onto the exponent. Counting
        them and shifting is the obvious way; multiplying by two to the sixty
        four first is exact, is one instruction, lands the value in the normal
        range in one step whatever it was, and takes the sixty four back off
        the exponent afterwards.

        The value itself is still only as good as a double, because the parse
        was strtod's. That is the divergence worth naming and it is named in
        the file that ships beside this one: "%Lf" here reads a long double's
        worth of object and a double's worth of digits, and printf in this
        tree does not read the L at all.
*/
#ifndef SCAN_DECIMAL_WIDE
static fn scan_store_wide_decimal(address_any into, decimal value)
{
        union {
                decimal value;
                p64 bits;
        } narrow;

        p64 sign;
        p64 exponent;
        p64 fraction;
        p64 wide;
        positive lowered = 0;

        narrow.value = value;

        if (((narrow.bits >> 52) & 0x7FF) == 0 &&
            (narrow.bits & 0x000FFFFFFFFFFFFFull) != 0)
        {
                narrow.value = value * 18446744073709551616.0;
                lowered = 64;
        }

        sign = narrow.bits >> 63;
        exponent = (narrow.bits >> 52) & 0x7FF;
        fraction = narrow.bits & 0x000FFFFFFFFFFFFFull;

        if (exponent == 0x7FF)
                wide = 0x7FFF;
        else if (exponent == 0 && fraction == 0)
                wide = 0;
        else
                wide = exponent + 16383 - 1023 - lowered;

#if __LDBL_MANT_DIG__ == 64
        {
                //      The x87 extended, whose integer bit is written down.
                //      It is set for everything except a true zero, which
                //      leaves both infinities and every not-a-number with it
                //      set, as that format requires.
                p64 piece[2];

                piece[0] = fraction << 11;

                if (!(exponent == 0 && fraction == 0))
                        piece[0] |= 0x8000000000000000ull;

                piece[1] = (sign << 15) | wide;

                //      Ten bytes and not sixteen. The object is sixteen for
                //      alignment and the format is eighty bits, and the
                //      instruction that stores one writes exactly the ten it
                //      needs: a caller that had something in the padding
                //      still has it afterwards, which is measurable against
                //      glibc and is what it leaves behind.
                memory_copy(into, piece, 10);
        }
#elif __LDBL_MANT_DIG__ == 113
        {
                //      binary128, whose integer bit is implied as usual, so
                //      the fraction simply moves sixty places up and splits
                //      across the two halves of the object.
                p64 piece[2];

                piece[0] = (fraction & 0xF) << 60;
                piece[1] = (sign << 63) | (wide << 48) | (fraction >> 4);

                memory_copy(into, piece, 16);
        }
#else
        /*
                Neither of the two formats this tree builds for. A target
                whose long double is its double needs no move at all, and one
                with a third format is a target this has never seen, so both
                take the compiler's own widening -- which is free in the
                first case and a call into libgcc in the second. Naming that
                here rather than assembling bytes for a layout nobody has
                looked at is the honest answer; a link that then asks for
                __extenddftf2 is asking a question that has to be answered
                with a measurement and not with a guess.
        */
        (void)sign;
        (void)exponent;
        (void)fraction;
        (void)wide;
        address_to (f128 address_to)into = (f128)value;
#endif
}
#endif // !SCAN_DECIMAL_WIDE

/*
        The three widths, each asking the parser that owns it.

        Every one of these runs even when the conversion was suppressed,
        because a suppressed conversion still reports a range error through
        errno and glibc's does: "%*f" against 1e400 answers zero assignments
        and ERANGE. So the parse happens and only the store is conditional.
*/
static f32 scan_narrow_decimal(string_address text, address_any into)
{
#ifdef SCAN_DECIMAL_NARROW
        f32 value = SCAN_DECIMAL_NARROW(text, null);
#else
        f32 value = (f32)SCAN_DECIMAL(text, null);
#endif

        if (into != null)
                address_to (f32 address_to)into = value;

        return value;
}

static fn scan_extended_decimal(string_address text, address_any into)
{
#ifdef SCAN_DECIMAL_WIDE

        //      A long double out and a long double in, which is a copy and
        //      not a conversion, so nothing here asks libgcc for anything.
        f128 value = SCAN_DECIMAL_WIDE(text, null);

        if (into != null)
                address_to (f128 address_to)into = value;
#else
        decimal value = SCAN_DECIMAL(text, null);

        if (into != null)
                scan_store_wide_decimal(into, value);
#endif
}

//      A whole number that came out of the input's sign, saturated the way
//      strtol saturates: the positive limit is one below the negative one,
//      so the two ends are asked about separately.
static positive scan_signed_value(positive magnitude, bool negative,
                                  bool overflowed)
{
        positive limit_positive = ~(positive)0 >> 1;
        positive limit_negative = (~(positive)0 >> 1) + 1;

        if (overflowed)
        {
                scan_out_of_range();

                return negative ? limit_negative : limit_positive;
        }

        if (!negative && magnitude > limit_positive)
        {
                scan_out_of_range();

                return limit_positive;
        }

        if (negative && magnitude > limit_negative)
        {
                scan_out_of_range();

                return limit_negative;
        }

        return negative ? (positive)(0 - (bipolar)magnitude) : magnitude;
}

//      And the unsigned one, where strtoul saturates at the top and a minus
//      sign on a value that fit is a wrap and not an error at all: "-1" into
//      %u is every bit set, and glibc does not call that out of range.
static positive scan_unsigned_value(positive magnitude, bool negative,
                                    bool overflowed)
{
        if (overflowed)
        {
                scan_out_of_range();

                return ~(positive)0;
        }

        return negative ? (positive)(0 - (bipolar)magnitude) : magnitude;
}

/*
        WHICH FAILURE THIS WAS, AND WHAT THE CALL ANSWERS BECAUSE OF IT

        mark is what the source had already handed out when this directive's
        own reading began -- after its white space skip, because the skip is
        not part of the item. If the directive read nothing past that and the
        source is at its end, nothing was there and this is an INPUT failure,
        which answers EOF when nothing has been assigned yet. If it read
        something and that something was wrong, it is a MATCHING failure and
        answers the count, which may be zero.

        The two are one line apart and are the whole of what separates "the
        file ended" from "the file said something else", which is the answer
        every caller of scanf actually wants. "%d" against "" is EOF, "%d"
        against "+" is zero because the sign was read, and "%d" against " "
        is EOF again because the space was skipped rather than read.
*/
static b32 scan_leave(scan_source address_to source, b32 assigned,
                      bool starved)
{
        scan_finish(source);

        /*
                AND WHAT ERRNO SAYS ABOUT IT AFTERWARDS

                glibc puts errno aside on the way in and sets it to zero, so
                that a conversion which overflows can report ERANGE through
                it and one that does not cannot leave a stale number behind.
                On the way out it puts the caller's value back, unless
                something inside actually set one. That much is ordinary.

                The part that is not ordinary, and is measured rather than
                reasoned: a call that ends on an INPUT failure puts nothing
                back and clears what was set. "%ldx" against a number too
                large to hold, with nothing after it, answers one and leaves
                errno at zero -- the range error the conversion really had is
                thrown away by the literal that then ran out of input. The
                same call with a byte after the number keeps the ERANGE. A
                program that reads errno after a scanf is reading a value
                that depends on whether the LAST directive found anything,
                which is worth knowing before depending on it.

                The caller's own value does not survive that either: an entry
                errno of seven and an input failure leaves zero and not seven.
                Both halves were measured against glibc 2.44 rather than read
                out of it, and both are matched here because a program that
                tests errno after a failed scanf is testing this and not the
                sentence in the standard, which says nothing at all.
        */
        if (starved || source->ran_out)
                scan_errno_clear();
        else
                scan_errno_keep(source->entered_errno);

        if (starved && assigned == 0)
                return EOF;

        return assigned;
}

static b32 scan_stop(scan_source address_to source, positive mark, b32 assigned)
{
        return scan_leave(source, assigned,
                          source->ended && source->consumed == mark);
}

/*
        THE ENGINE

        A walk over the format with three kinds of thing in it: white space,
        which matches a run of white space including none; an ordinary byte,
        which must be there exactly; and a directive, which is everything
        else in this file.

        The two failures are not the same failure and the difference is the
        single most-missed thing in scanf. An INPUT failure is the source
        running out before a directive had anything to work with, and it makes
        the whole call answer EOF if nothing has been assigned yet. A MATCHING
        failure is bytes that were there and were wrong, and it answers with
        the count so far however small, including zero. "" against "%d" is EOF
        and "abc" against "%d" is zero, and a program that tells them apart is
        a program that can tell a short file from a bad one.

        A directive that read at least one byte and then ran out is a MATCHING
        failure and not an input one -- "1e+" at the end of a file answers
        zero, not EOF -- because the end of the file is what stopped a prefix
        from growing, and a prefix that stopped is a thing that failed to
        match. %c is the exception and is glibc's: a width it could not fill
        answers EOF even though it stored what it read.
*/
static b32 scan_run(scan_source address_to source, string_address format,
                    var_args list)
{
        scan_stage stage;
        b32 assigned = 0;
        string_address at = format;

        if (is_null(format))
                return scan_leave(source, 0, true);

        while (string_get(at) != end)
        {
                positive width = 0;
                positive length = SCAN_LENGTH_INT;
                positive mark = 0;
                bool suppress = false;
                bool literal = false;
                bool literal_skips = false;
                b32 byte;

                /* A string source exposes the whole ordinary literal run.
                   The common-prefix floor gives both the match and the exact
                   mismatch position, so successful bytes remain consumed and
                   the rejected byte remains untouched just as scan_get plus
                   scan_unget would leave them.  Streams retain the byte path:
                   their refill buffer belongs to stream.c. */
                if (scan_is_text(source) &&
                    string_get(at) != '%' &&
                    !byte_is_space(string_get(at)))
                {
                        string_address input =
                                source->text + source->place;
                        positive run = string_span_without_set(
                                at, (string_address)"% \t\n\r\v\f");
                        positive available = string_length_max(input, run);
                        positive matched = memory_common_prefix(
                                (address_any)at, (address_any)input, available);

                        source->place += matched;
                        source->consumed += matched;
                        at += matched;

                        if (matched != run)
                        {
                                if (matched == available)
                                {
                                        source->ended = true;
                                        return scan_leave(source, assigned,
                                                          true);
                                }

                                return scan_leave(source, assigned, false);
                        }

                        continue;
                }

                if (byte_is_space(string_get(at)))
                {
                        while (byte_is_space(string_get(at)))
                                at++;

                        scan_skip_white(source);

                        continue;
                }

                if (string_get(at) != '%')
                {
                        literal = true;
                }
                else
                {
                        at++;

                        if (string_get(at) == '*')
                        {
                                suppress = true;
                                at++;
                        }

                        while (byte_is_digit(string_get(at)))
                        {
                                width = width * 10 +
                                        (positive)(string_get(at) - '0');
                                at++;
                        }

                        switch (string_get(at))
                        {
                        case 'h':
                                at++;

                                if (string_get(at) == 'h')
                                {
                                        at++;
                                        length = SCAN_LENGTH_CHAR;
                                }
                                else
                                {
                                        length = SCAN_LENGTH_SHORT;
                                }

                                break;
                        case 'l':
                                at++;

                                if (string_get(at) == 'l')
                                {
                                        at++;
                                        length = SCAN_LENGTH_WIDE;
                                }
                                else
                                {
                                        length = SCAN_LENGTH_DECIMAL;
                                }

                                break;
                        case 'q':
                        case 'z':
                        case 't':
                        case 'j':
                                at++;
                                length = SCAN_LENGTH_WIDE;
                                break;
                        case 'L':
                                at++;
                                length = SCAN_LENGTH_WIDE_DECIMAL;
                                break;
                        default:
                                break;
                        }

                        //      "%%" is a percent in the input, and it is
                        //      NOT the same thing as writing a percent as an
                        //      ordinary character: glibc skips the white
                        //      space in front of the one and not in front of
                        //      the other, so " %" matches "%%" and does not
                        //      match a bare percent. Measured, and the sort
                        //      of asymmetry only a diff finds.
                        if (string_get(at) == '%')
                        {
                                literal = true;
                                literal_skips = true;
                        }
                }

                if (literal)
                {
                        if (literal_skips)
                                scan_skip_white(source);

                        mark = source->consumed;
                        byte = scan_get(source);

                        if (byte == EOF)
                                return scan_leave(source, assigned, true);

                        if (byte != (b32)string_get(at))
                        {
                                scan_unget(source, byte);

                                return scan_leave(source, assigned, false);
                        }

                        at++;

                        continue;
                }

                switch (string_get(at))
                {
                case 'd':
                case 'i':
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                {
                        b32 conversion = (b32)string_get(at);
                        positive base = conversion == 'i'   ? 0
                                        : conversion == 'o' ? 8
                                        : conversion == 'x' || conversion == 'X'
                                            ? 16
                                            : 10;
                        bool negative;
                        bool overflowed;
                        positive magnitude;
                        positive value;
                        positive base_used;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        if (!scan_integer(source, address_of stage, width, base,
                                          address_of negative,
                                          address_of base_used))
                                return scan_stop(source, mark, assigned);

                        magnitude = scan_magnitude(address_of stage, base_used,
                                                   address_of overflowed);

                        if (conversion == 'd' || conversion == 'i')
                                value = scan_signed_value(magnitude, negative,
                                                          overflowed);
                        else
                                value = scan_unsigned_value(magnitude, negative,
                                                            overflowed);

                        if (!suppress)
                        {
                                scan_store_whole(var_list_get(list, address_any),
                                                 length, value);
                                assigned++;
                        }

                        break;
                }
                case 'p':
                {
                        bool negative;
                        bool overflowed;
                        positive magnitude;
                        positive value;
                        positive base_used;
                        bool matched;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        byte = scan_get(source);

                        //      "(nil)" is five bytes and a width that cannot
                        //      hold five is not offered it at all: glibc
                        //      pushes the bracket back untouched rather than
                        //      starting a word it has no room to finish, so
                        //      "%4p" against "(nil)" leaves the position at
                        //      nothing and answers a matching failure.
                        if (byte == '(' && (width == 0 || width >= 5))
                        {
                                //      glibc prints a null pointer as "(nil)"
                                //      and reads it back, so this does too.
                                //      It is a prefix like every other word
                                //      here: five bytes or nothing.
                                string_address word = (string_address) "(nil)";
                                positive matched_bytes = 1;
                                positive limit = width != 0 ? width
                                                            : ~(positive)0;

                                while (matched_bytes < 5 &&
                                       matched_bytes < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if ((b32)(p8)byte_to_lower(byte) !=
                                            (b32)word[matched_bytes])
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        matched_bytes++;
                                }

                                if (matched_bytes != 5)
                                        return scan_stop(source, mark,
                                                         assigned);

                                if (!suppress)
                                {
                                        address_to (address_any address_to)
                                            var_list_get(list, address_any) =
                                                null;
                                        assigned++;
                                }

                                break;
                        }

                        scan_unget(source, byte);

                        matched = scan_integer(source, address_of stage, width,
                                               16, address_of negative,
                                               address_of base_used);

                        if (!matched)
                                return scan_stop(source, mark, assigned);

                        magnitude = scan_magnitude(address_of stage, base_used,
                                                   address_of overflowed);
                        value = scan_unsigned_value(magnitude, negative,
                                                    overflowed);

                        if (!suppress)
                        {
                                address_to (address_any address_to)
                                    var_list_get(list, address_any) =
                                        (address_any)value;
                                assigned++;
                        }

                        break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'a':
                case 'A':
                {
                        address_any into = null;

                        at++;
                        scan_skip_white(source);
                        mark = source->consumed;

                        if (!scan_decimal_shape(source, address_of stage, width))
                                return scan_stop(source, mark, assigned);

                        stage.bytes[stage.staged] = end;

                        if (!suppress)
                                into = var_list_get(list, address_any);

                        if (length == SCAN_LENGTH_WIDE_DECIMAL)
                        {
                                scan_extended_decimal(
                                    (string_address)stage.bytes, into);
                        }
                        else if (length == SCAN_LENGTH_DECIMAL ||
                                 length == SCAN_LENGTH_WIDE)
                        {
                                decimal value = SCAN_DECIMAL(
                                    (string_address)stage.bytes, null);

                                if (into != null)
                                        address_to (f64 address_to)into = value;
                        }
                        else
                        {
                                scan_narrow_decimal(
                                    (string_address)stage.bytes, into);
                        }

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 'c':
                {
                        positive want = width != 0 ? width : 1;
                        positive got = 0;
                        p8 address_to into = null;

                        at++;

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        //      A string source hands the whole run over: how
                        //      much is there is one bounded string_length and
                        //      the move is one memory_copy.
                        if (scan_is_text(source))
                        {
                                got = string_length_max(source->text +
                                                            source->place,
                                                        want);

                                scan_text_take(source, into, got);

                                if (got < want)
                                        source->ended = true;
                        }
                        else
                        {
                                while (got < want)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got < want)
                                //      glibc keeps the bytes it did read and
                                //      still calls the short count an input
                                //      failure, which is measured.
                                return scan_leave(source, assigned, true);

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 's':
                {
                        positive limit = width != 0 ? width : ~(positive)0;
                        positive got = 0;
                        p8 address_to into = null;

                        at++;
                        scan_skip_white(source);

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        if (scan_is_text(source))
                        {
                                got = string_span_without_set(
                                    source->text + source->place,
                                    (string_address)scan_white);

                                if (got > limit)
                                        got = limit;

                                scan_text_take(source, into, got);
                        }
                        else
                        {
                                while (got < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (byte_is_space(byte))
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got == 0)
                                return scan_leave(source, assigned, true);

                        if (into != null)
                                into[got] = end;

                        if (!suppress)
                                assigned++;

                        break;
                }
                case '[':
                {
                        b8 set[256];
                        bool negated;
                        positive limit = width != 0 ? width : ~(positive)0;
                        positive got = 0;
                        p8 address_to into = null;
                        string_address after;

                        after = scan_build_set(at + 1, set, address_of negated);

                        if (is_null(after))
                                //      A format with no close bracket in it.
                                //      glibc stops and reports what it had,
                                //      and does not call it an input failure.
                                return scan_leave(source, assigned, false);

                        at = after;

                        if (!suppress)
                                into = (p8 address_to)var_list_get(list,
                                                                   address_any);

                        if (scan_is_text(source))
                        {
                                //      The terminator ends a string source
                                //      whatever the set says, which is what
                                //      clearing this one entry buys: a
                                //      negated set holds the zero byte, and a
                                //      string has no zero byte in it to hold.
                                set[0] = 0;

                                got = string_span(source->text + source->place,
                                                  set);

                                if (got > limit)
                                        got = limit;

                                scan_text_take(source, into, got);

                                if (got == 0 &&
                                    string_get(source->text + source->place) ==
                                        end)
                                        source->ended = true;
                        }
                        else
                        {
                                while (got < limit)
                                {
                                        byte = scan_get(source);

                                        if (byte == EOF)
                                                break;

                                        if (!set[(p8)byte])
                                        {
                                                scan_unget(source, byte);
                                                break;
                                        }

                                        if (into != null)
                                                into[got] = (p8)byte;

                                        got++;
                                }
                        }

                        if (got == 0)
                                return scan_leave(source, assigned,
                                                  source->ended);

                        if (into != null)
                                into[got] = end;

                        if (!suppress)
                                assigned++;

                        break;
                }
                case 'n':
                        //      Not a conversion. It reads nothing, it cannot
                        //      fail, it never reaches the end of the input,
                        //      and it is not counted in the answer -- all
                        //      four of which are things a plausible scanf
                        //      gets wrong by treating it like the others.
                        at++;

                        if (!suppress)
                                scan_store_whole(var_list_get(list, address_any),
                                                 length, source->consumed);

                        break;
                default:
                        /*
                                An unknown conversion, or a percent that was
                                the last byte of the format. Either way the
                                call stops and answers with what it had
                                already assigned, and NOT with EOF even when
                                the input was empty: a format nobody can read
                                is not the input running out.

                                An unknown conversion still skips the white
                                space in front of it before giving up, which
                                is glibc's, and is visible: "%y" against three
                                spaces leaves the position at three and the
                                end-of-file indicator set. A percent at the
                                very end of the format skips nothing, because
                                there is no conversion there to have skipped
                                for.
                        */
                        if (string_get(at) != end)
                                scan_skip_white(source);

                        return scan_leave(source, assigned, false);
                }
        }

        return scan_leave(source, assigned, false);
}

/*
        THE STANDARD NAMES

        The same argument format.c makes for printf's: scanf is a fixed
        interface with a fixed signature that thirty years of C expects under
        that exact name, and a prose alias would be a second name for one
        thing. The prose name in this family is scan_run, which is the engine,
        and scan_source, which is where the bytes come from.
*/
static b32 vsscanf(string_address text, string_address format, var_args list)
{
        scan_source source;

        memory_zero(address_of source, sizeof(source));
        scan_errno_save(source.entered_errno);
        source.text = is_null(text) ? (string_address) "" : text;

        return scan_run(address_of source, format, list);
}

static b32 sscanf(string_address text, string_address format, ...)
{
        var_args list;
        b32 answer;

        var_list(list, format);
        answer = vsscanf(text, format, list);
        var_list_end(list);

        return answer;
}

#if SCAN_STREAMS

static b32 vfscanf(scan_stream handle, string_address format, var_args list)
{
        scan_source source;

        if (is_null(handle))
                return EOF;

        memory_zero(address_of source, sizeof(source));
        scan_errno_save(source.entered_errno);
        source.handle = handle;

        return scan_run(address_of source, format, list);
}

static b32 vscanf(string_address format, var_args list)
{
        return vfscanf(stdin, format, list);
}

static b32 fscanf(scan_stream handle, string_address format, ...)
{
        var_args list;
        b32 answer;

        var_list(list, format);
        answer = vfscanf(handle, format, list);
        var_list_end(list);

        return answer;
}

static b32 scanf(string_address format, ...)
{
        var_args list;
        b32 answer;

        var_list(list, format);
        answer = vfscanf(stdin, format, list);
        var_list_end(list);

        return answer;
}

#endif // SCAN_STREAMS

#endif // KERNEL_MODE / STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_SCAN
