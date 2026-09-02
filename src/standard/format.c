/*
        Experimental C standard library

        printf: a format string, its arguments, and the bytes they mean

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_FORMAT
#define STANDARD_MODERN_C_FORMAT

/*
        This is ordinary C on purpose, for the reason netlink.c gives.

        library.c holds declarations and assembly and nothing else, and that is
        checked. printf is not a floor. It is a small language -- five flags, a
        width, a precision, eight length modifiers, twenty conversions -- and
        the part of it a machine could do differently from another machine is
        already downstairs: positive_into_base turns an integer into digits,
        positive_into does the decimal case through the pair table, memory_copy
        moves the run of literal text between two specifiers. What is left up
        here is the grammar, the argument fetch, and the decision about which
        byte goes where, and that is the same reasoning on every architecture.

        It is built beside string_format rather than on top of it. They are
        different languages that happen to share a percent sign: string_format
        reads %p as an unsigned integer where printf reads it as a pointer, it
        has no width, no precision and no flags, and it hands each piece to a
        writer the moment it has one. printf cannot do that. A right-aligned
        field pads before it emits, so the length has to be known before the
        first byte leaves, and snprintf has to keep counting after the buffer
        has stopped accepting. What the two do share is the number engine
        underneath, and both call the same assembly for it.

        Nothing here reaches for positive_to_base_field, which is the library's
        complete integer field and looks like exactly the right thing to call.
        Three of C's rules are not its rules. A zero value at a precision of
        zero prints nothing in C and prints one digit there. The # flag on
        octal is a precision bump in C and a two-byte prefix there. An explicit
        precision turns the 0 flag off in C and leaves it on there unless the
        caller has already cleared it. Pre-correcting for three of those at
        every call site is more code than assembling the field, so the field is
        assembled here and only the digits are borrowed.
*/

/*
        WHY THE WHOLE FILE IS BEHIND KERNEL_MODE

        src/core.c includes compiler_memory.c, so everything here is compiled
        into the kernel build as well as into every program. The kernel build
        refuses a decimal in a signature on arm64 whether or not anything calls
        it, which is the same reason decimal_to_string and fast_sin are guarded
        one floor down, and a kernel has its own logging and no business
        carrying a float formatter. So the file is one guarded block.
*/

#ifndef KERNEL_MODE

/*
        WHAT THIS FILE TAKES FROM THE STREAM FAMILY AND THE ERROR FAMILY

        Both were written beside this one and both are deferred to by name.
        Nothing here redefines anything either of them owns.

        From the stream family, guarded on STANDARD_MODERN_C_STANDARD_STREAM:

                FILE / stream           the handle, and stdout and stderr
                stream_put_bytes        bytes to a handle, count accepted
                fputc, fputs            already there as aliases

        From the error family, guarded on STANDARD_MODERN_C_STANDARD_ERROR:

                errno                   an lvalue through __errno_location
                strerror                the sentence for a number
                perror                  the whole line, written without a
                                        stream, which is the right way round

        Each guard has a fallback below it so this file builds and runs alone,
        which is how it was tested: a handle is then the descriptor itself and
        the write is one trap. When a family is present its version wins and
        the fallback is not compiled, so the merge is include order and nothing
        else -- stream, then error, then this.

        THE HANDLE GOES THROUGH A TYPEDEF ON PURPOSE. library.c defines stdin,
        stdout and stderr as the integers 0, 1 and 2 at library.c:12577, and
        src/sh/shell.c:899 uses stdout as a descriptor in a dup3. Naming FILE
        directly in these signatures would tie them to whichever of the two
        meanings happened to be current. format_stream is whichever one is
        real, so fprintf(stdout, ...) compiles and means the right thing in
        both worlds, and the entries below never have to be edited again.

        (The stream family undefines those three and redefines them as
        pointers. That is its business and its risk -- programs/shell.c
        includes compiler_memory.c before src/sh/shell.c, so the shell sees
        whatever compiler_memory.c last said -- but whoever merges the two
        should build programs/shell.c and run it before believing it worked.
        This file was built and run against the shell for exactly that reason
        and does not move any of the three.)
*/

#ifndef STANDARD_MODERN_C_STANDARD_STREAM

typedef b32 format_stream;

#define format_output ((format_stream)stdout)
#define format_error ((format_stream)stderr)
#define FORMAT_OWNS_BYTE_ENTRIES 1

/*
        The placeholder writes straight through. It has no buffer, so it also
        has no flush and no ordering question against the library's own log
        buffer, and a caller that prints one line per check pays one write call
        per line. The stream family replaces this with something that buffers,
        which is the only reason the placeholder is allowed to be this slow.
*/
static positive format_stream_write(format_stream stream, address_any data,
                                    positive length)
{
        bipolar written;

        if (length == 0)
                return 0;

        written = (bipolar)system_write_all((positive)stream, data, length);

        return written < 0 ? 0 : (positive)written;
}

#else

typedef FILE address_to format_stream;

#define format_output stdout
#define format_error stderr

//      stream_put_bytes already has this shape, so this is a second name.
static positive format_stream_write(format_stream, address_any, positive)
        __attribute__((alias("stream_put_bytes")));

#endif // STANDARD_MODERN_C_STANDARD_STREAM

#ifndef STANDARD_MODERN_C_STANDARD_ERROR

static bipolar errno = 0;

/*
        Enough of the table to make perror useful on its own. The error family
        carries the whole of it, checked against glibc number by number, and
        replaces this outright; what is here is the codes this tree already
        names in src/sh/file.c and the handful beside them.
*/
static string_address strerror(b32 code)
{
        static const char address_to messages[40] = {
                [0] = "Success",
                [1] = "Operation not permitted",
                [2] = "No such file or directory",
                [3] = "No such process",
                [4] = "Interrupted system call",
                [5] = "Input/output error",
                [9] = "Bad file descriptor",
                [11] = "Resource temporarily unavailable",
                [12] = "Cannot allocate memory",
                [13] = "Permission denied",
                [14] = "Bad address",
                [17] = "File exists",
                [18] = "Invalid cross-device link",
                [20] = "Not a directory",
                [21] = "Is a directory",
                [22] = "Invalid argument",
                [24] = "Too many open files",
                [25] = "Inappropriate ioctl for device",
                [28] = "No space left on device",
                [32] = "Broken pipe",
                [39] = "Directory not empty",
        };

        if (code < 0)
                code = -code;

        return (string_address)(code < 40 && messages[code]
                                    ? messages[code]
                                    : "Unknown error");
}

#define FORMAT_OWNS_PERROR 1

#endif // STANDARD_MODERN_C_STANDARD_ERROR

/*
        Where the bytes go.

        One structure covers every destination the family has. A buffer with a
        capacity is sprintf and snprintf. A buffer with a capacity of zero is
        the snprintf call that must not touch the pointer at all and still has
        to answer with a length. A stream is printf and fprintf. A downstream
        writer is the entry this tree's own code wants, so that a caller
        already holding a writer can format into it without inventing a FILE.

        counted is the answer, and it is deliberately not used. That difference
        is the single most commonly broken rule in a hand-written printf:
        snprintf returns the length it WOULD have written, so the count keeps
        rising long after the buffer has stopped accepting, and a caller sizing
        a second allocation off the return value gets the size it needs rather
        than the size it already had.
*/
typedef struct
{
        p8 address_to buffer;
        positive capacity;
        positive used;
        positive counted;
        writer downstream;
        format_stream stream;
        bool streaming;
} format_sink;

static fn format_emit(format_sink address_to sink, address_any data,
                      positive length)
{
        positive room;

        if (length == 0)
                return;

        sink->counted += length;

        if (sink->streaming)
        {
                format_stream_write(sink->stream, data, length);
                return;
        }

        if (sink->downstream)
        {
                sink->downstream(data, length);
                return;
        }

        if (is_null(sink->buffer) || sink->used >= sink->capacity)
                return;

        room = sink->capacity - sink->used;

        if (length < room)
                room = length;

        memory_copy(sink->buffer + sink->used, data, room);
        sink->used += room;
}

/*
        Padding is a run of one repeated byte and there is never a buffer of it
        lying around, so it comes out of a small stack block a chunk at a time.
        Sixty-four bytes means the widths that actually occur -- a column in a
        table, an eight place zero fill -- are one pass, while a pathological
        %2000d is thirty-two rather than two thousand.
*/
static fn format_fill(format_sink address_to sink, p8 byte, positive count)
{
        p8 block[64];
        positive part;

        if (count == 0)
                return;

        //      The whole block, not the part this call needs: sizeof is a
        //      literal, so the umbrella's specializer folds it to straight
        //      line stores, while a variable part is a call into the general
        //      routine and padding is nearly always a handful of bytes.
        memory_fill(block, byte, sizeof(block));

        while (count)
        {
                part = count < sizeof(block) ? count : sizeof(block);
                format_emit(sink, block, part);
                count -= part;
        }
}

//      The five flags, packed so a whole specifier fits in registers.

#define FORMAT_FLAG_LEFT CONVERSION_FLAG_LEFT
#define FORMAT_FLAG_PLUS CONVERSION_FLAG_PLUS
#define FORMAT_FLAG_SPACE CONVERSION_FLAG_SPACE
#define FORMAT_FLAG_ALTERNATE CONVERSION_FLAG_ALTERNATE
#define FORMAT_FLAG_ZERO CONVERSION_FLAG_ZERO

//      The length modifiers, one code each rather than the one or two bytes
//      they are spelled with.

#define FORMAT_LENGTH_INT CONVERSION_LENGTH_INT
#define FORMAT_LENGTH_CHAR CONVERSION_LENGTH_CHAR
#define FORMAT_LENGTH_SHORT CONVERSION_LENGTH_SHORT
#define FORMAT_LENGTH_LONG CONVERSION_LENGTH_LONG
#define FORMAT_LENGTH_LONG_LONG CONVERSION_LENGTH_LONG_LONG
#define FORMAT_LENGTH_SIZE CONVERSION_LENGTH_SIZE
#define FORMAT_LENGTH_DIFFERENCE CONVERSION_LENGTH_DIFFERENCE
#define FORMAT_LENGTH_WIDEST CONVERSION_LENGTH_WIDEST
/*
        L is parsed and then ignored, which is not the same as supported.

        A %Lf reads a double out of the place a double would have been, and on
        x86_64 a long double was not put there -- it arrives on the stack with
        sixteen byte alignment while a double arrives in a vector register --
        so the number printed is whatever was in that register instead. The
        code is here so that the conversion after it is still found and the
        rest of the format still comes out; nobody should write %Lf against
        this until somebody implements it.

        Implementing it means three different types: eighty bits on x86_64 and
        a hundred and twenty eight on arm64 and riscv64, and converting between
        them calls into libgcc, which a -nostdlib link does not have. The
        exact-decimal engine below would take a wider mantissa and a wider
        exponent without complaint; it is the argument fetch and the libgcc
        dependency that are the work.
*/
#define FORMAT_LENGTH_WIDE_DECIMAL CONVERSION_LENGTH_WIDE_DECIMAL

typedef struct
{
        positive flags;
        positive width;
        bipolar precision;
        positive length;
        p8 conversion;
} format_spec;

/*
        The integer field, assembled in the order C says it appears.

        Everything that is not a digit is decided here: the sign byte, the 0x
        or 0b or 0 that the # flag asks for, the zeros an explicit precision
        demands, and the spaces the width demands on whichever side the - flag
        did not claim. Only the digits come out of the library.

        Three rules are worth stating because each is a corner every
        implementation gets wrong exactly once.

        A precision turns the 0 flag off. "%08.3d" of 42 is "     042" and not
        "00000042": the zeros a precision asks for are part of the number, the
        zeros the flag asks for are padding, and C says the padding loses.

        A zero value at a precision of zero is no characters at all. "%.0d" of
        0 is the empty string. A sign or a prefix still appears, so "%+.0d" of
        0 is "+", which is why the emptiness here is a digit length of zero
        rather than an early return.

        The # flag on octal is not a prefix. C says it raises the precision far
        enough to force a leading zero and no further, which is why "%#.5o" of
        8 is "00010" in five characters and not six: the zero the flag wants is
        already there, so the flag adds nothing.
*/
static fn format_integer(format_sink address_to sink, positive value,
                         positive base, bool negative,
                         format_spec address_to spec)
{
        p8 digits[72];
        p8 prefix[2];
        p8 sign = 0;
        positive prefix_length = 0;
        positive length;
        positive zeros = 0;
        positive forced = 0;
        positive body;
        positive spaces = 0;
        bool upper = spec->conversion == 'X' || spec->conversion == 'B';
        bool has_sign = spec->conversion == 'd' || spec->conversion == 'i';

        length = positive_into_base(digits, value, base, upper);

        //      A zero at an explicit precision of zero occupies no columns.
        if (value == 0 && spec->precision == 0)
                length = 0;

        //      The + and space flags belong to the signed conversions alone.
        //      An unsigned value has no sign to make explicit, so glibc drops
        //      both flags there and so does this.
        if (negative)
                sign = '-';
        else if (!has_sign)
                sign = 0;
        else if (spec->flags & FORMAT_FLAG_PLUS)
                sign = '+';
        else if (spec->flags & FORMAT_FLAG_SPACE)
                sign = ' ';

        if (spec->conversion == 'p')
        {
                prefix[0] = '0';
                prefix[1] = 'x';
                prefix_length = 2;
        }

        if (spec->precision >= 0 && (positive)spec->precision > length)
                zeros = (positive)spec->precision - length;

        if ((spec->flags & FORMAT_FLAG_ALTERNATE) && spec->conversion != 'p')
        {
                if (base == 16 && value != 0)
                {
                        prefix[0] = '0';
                        prefix[1] = upper ? 'X' : 'x';
                        prefix_length = 2;
                }
                else if (base == 2 && value != 0)
                {
                        prefix[0] = '0';
                        prefix[1] = upper ? 'B' : 'b';
                        prefix_length = 2;
                }
                else if (base == 8 && zeros == 0 && !(value == 0 && length == 1))
                {
                        //      One zero, and only where the body does not
                        //      already begin with one. It is counted apart
                        //      from the precision zeros on purpose: a
                        //      precision turns the 0 flag off and this does
                        //      not, so "%#05o" of 8 is "00010" and not
                        //      "  010".
                        forced = 1;
                }
        }

        body = (sign != 0) + prefix_length + forced + zeros + length;

        //      The 0 flag pads between the prefix and the digits, and only
        //      where nothing else has already claimed that job.
        if ((spec->flags & FORMAT_FLAG_ZERO) &&
            !(spec->flags & FORMAT_FLAG_LEFT) && spec->precision < 0 &&
            spec->width > body)
        {
                zeros += spec->width - body;
                body = spec->width;
        }

        if (spec->width > body)
                spaces = spec->width - body;

        if (!(spec->flags & FORMAT_FLAG_LEFT))
                format_fill(sink, ' ', spaces);

        if (sign)
                format_emit(sink, address_of sign, 1);

        if (prefix_length)
                format_emit(sink, prefix, prefix_length);

        format_fill(sink, '0', zeros + forced);
        format_emit(sink, digits, length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        A counted run of bytes inside a width, which is %s and %c both.

        The name carries the word field only because src/test/verify.c already
        has a static array called format_text, and one translation unit cannot
        hold both.

        writer_field downstairs does exactly this and is not called, for the
        same reason positive_to_base_field is not: it takes a writer, and a
        writer has no way to tell snprintf how many bytes it refused. The three
        lines saved are not worth a second path through the sink.
*/
static fn format_text_field(format_sink address_to sink, address_any data,
                            positive length, format_spec address_to spec)
{
        positive spaces = spec->width > length ? spec->width - length : 0;

        if (!(spec->flags & FORMAT_FLAG_LEFT))
                format_fill(sink, ' ', spaces);

        format_emit(sink, data, length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        EVERY DOUBLE IS A TERMINATING DECIMAL, AND THAT IS THE WHOLE TRICK

        A finite double is m times 2^E with m below 2^53. When E is not
        negative that is an integer and it has an exact decimal spelling. When
        E is negative, write k for -E and split m at the binary point:

                m * 2^-k  =  (m >> k)  +  (m & (2^k - 1)) / 2^k

        and multiply the fraction on the right, top and bottom, by 5^k. It
        becomes (m_low * 5^k) / 10^k -- which is to say the exact decimal
        digits after the point are the digits of m_low * 5^k written out in
        exactly k places with leading zeros. No approximation enters anywhere.
        One tenth really does print as 0.1000000000000000055511151231257827
        021181583404541015625 when a caller asks for fifty-five places, and a
        real glibc agrees digit for digit, because there is exactly one right
        answer and both of us are computing it rather than estimating it.

        The cost is a big integer, but a small and one-directional one. The
        widest value that ever appears is 5^1074 times a fifty-three bit
        mantissa, which is 767 decimal digits. Holding it in limbs of 10^9
        rather than 2^64 means every operation needed is a multiply by
        something below 10^9, whose product is below 10^18 and fits a register
        without help, and the digits fall straight out of the limbs at the end.
        Nothing here divides by anything but a compile time constant and
        nothing calls libgcc, which matters because a -nostdlib link does not
        have libgcc to call.

        Rounding then happens on decimal digits, where a tie is a real tie
        rather than an artifact of the estimate, and it is round half to even
        because that is what a glibc in its default rounding mode does: "%.0f"
        of 0.5 is "0", of 1.5 is "2", and of 2.5 is "2".
*/

#define FORMAT_LIMB_BASE 1000000000
#define FORMAT_LIMB_DIGITS 9
#define FORMAT_LIMBS 90
#define FORMAT_DIGITS 1120

typedef struct
{
        positive limb[FORMAT_LIMBS];
        positive count;
} format_bignum;

static fn format_bignum_set(format_bignum address_to number, positive value)
{
        number->count = 0;

        while (value)
        {
                number->limb[number->count++] = value % FORMAT_LIMB_BASE;
                value /= FORMAT_LIMB_BASE;
        }
}

//      Multiply by anything below the limb base. The product of two such
//      values is below 10^18 and the carry never reaches the base, so the
//      whole operation stays inside a sixty-four bit register.
static fn format_bignum_scale(format_bignum address_to number, positive factor)
{
        positive carry = 0;
        positive index;

        for (index = 0; index < number->count; index++)
        {
                positive product = number->limb[index] * factor + carry;

                number->limb[index] = product % FORMAT_LIMB_BASE;
                carry = product / FORMAT_LIMB_BASE;
        }

        while (carry && number->count < FORMAT_LIMBS)
        {
                number->limb[number->count++] = carry % FORMAT_LIMB_BASE;
                carry /= FORMAT_LIMB_BASE;
        }
}

static fn format_bignum_add(format_bignum address_to into,
                            format_bignum address_to from)
{
        positive carry = 0;
        positive index;
        positive reach = into->count > from->count ? into->count : from->count;

        for (index = 0; (index < reach || carry) && index < FORMAT_LIMBS; index++)
        {
                positive total = carry;

                if (index < into->count)
                        total += into->limb[index];

                if (index < from->count)
                        total += from->limb[index];

                into->limb[index] = total % FORMAT_LIMB_BASE;
                carry = total / FORMAT_LIMB_BASE;

                if (index >= into->count)
                        into->count = index + 1;
        }
}

/*
        Multiply by a mantissa, which is too wide for one scale.

        Splitting at bit twenty-six leaves two halves each below 2^27, both of
        them under the limb base, and the shift that puts the high half back is
        itself a multiply by 2^26 which is also under the base. Three scales
        and one add, and no operation anywhere widens past sixty-four bits.
*/
static fn format_bignum_multiply(format_bignum address_to number,
                                 positive factor)
{
        format_bignum low;

        if (factor < FORMAT_LIMB_BASE)
        {
                format_bignum_scale(number, factor);
                return;
        }

        low.count = number->count;
        memory_copy(low.limb, number->limb, number->count * sizeof(positive));

        format_bignum_scale(number, factor >> 26);
        format_bignum_scale(number, (positive)1 << 26);
        format_bignum_scale(address_of low, factor & (((positive)1 << 26) - 1));
        format_bignum_add(number, address_of low);
}

/*
        The digits of the big integer, most significant first.

        The top limb loses its leading zeros and every limb below it keeps all
        nine of its own, which is what makes the concatenation the number
        rather than a list of limbs. A zero big integer writes nothing, and
        both callers want that: as an integer part it is replaced by a single
        "0", and as a fraction it is entirely the leading zero run the caller
        pads with anyway.
*/
static positive format_bignum_digits(format_bignum address_to number,
                                     p8 address_to into)
{
        positive length;
        positive index;

        if (number->count == 0)
                return 0;

        length = positive_into(into, number->limb[number->count - 1]);

        //      A limb below the base is a nine digit zero padded field, and
        //      positive_into_padded is that field: its three architectures
        //      each carry a fast path gated on exactly this shape -- pad
        //      zero, width nine, value below ten to the ninth -- which walks
        //      the digit pair table with no division in it at all.
        for (index = number->count - 1; index > 0; index--)
                length += positive_into_padded(into + length,
                                               number->limb[index - 1],
                                               FORMAT_LIMB_DIGITS, '0');

        return length;
}

/*
        A finite double, taken apart into the digits it exactly is.

        The result is a run of decimal digits with no leading and no trailing
        zero, and an exponent that says where the point sits. The value is

                0.d[0] d[1] ... d[count-1]  times ten to the exponent

        which is the one shape that serves %f, %e and %g without any of them
        needing to know how the other two work. A zero value is the empty digit
        run with an exponent of one, chosen so that %e writes e+00 rather than
        e-01 and %f writes a bare 0. A digit read past either end of the run
        reads as zero, which is true of the value and is what lets every emit
        loop below run without a bound.
*/
typedef struct
{
        p8 digit[FORMAT_DIGITS];
        positive count;
        bipolar exponent;
} format_number;

static fn format_expand(decimal value, format_number address_to number)
{
        union
        {
                decimal value;
                p64 bits;
        } view;
        p64 bits;
        positive mantissa;
        bipolar raw;
        bipolar power;
        p8 whole[400];
        positive whole_length = 0;
        positive first;

        view.value = value;
        bits = view.bits;

        number->count = 0;
        number->exponent = 1;

        mantissa = (positive)(bits & (((p64)1 << 52) - 1));
        raw = (bipolar)((bits >> 52) & 0x7ff);

        if (raw == 0)
        {
                power = -1074;
        }
        else
        {
                mantissa |= (positive)1 << 52;
                power = raw - 1075;
        }

        if (mantissa == 0)
                return;

        if (power >= 0)
        {
                format_bignum big;

                format_bignum_set(address_of big, mantissa);

                while (power >= 29)
                {
                        format_bignum_scale(address_of big, (positive)1 << 29);
                        power -= 29;
                }

                format_bignum_scale(address_of big, (positive)1 << power);

                whole_length = format_bignum_digits(address_of big, whole);

                memory_copy(number->digit, whole, whole_length);

                number->count = whole_length;
                number->exponent = (bipolar)whole_length;
        }
        else
        {
                static const positive powers_of_five[12] = {
                    1,      5,       25,      125,     625,      3125,
                    15625,  78125,   390625,  1953125, 9765625,  48828125};
                positive shift = (positive)(-power);
                positive above = shift >= 64 ? 0 : mantissa >> shift;
                positive below =
                    shift >= 64 ? mantissa
                                : mantissa & ((((positive)1) << shift) - 1);
                positive left = shift;
                format_bignum big;
                p8 fraction[FORMAT_DIGITS];
                positive fraction_length;
                positive pad;
                positive room;
                positive take;
                positive count = 0;

                if (above)
                        whole_length = positive_into(whole, above);

                format_bignum_set(address_of big, 1);

                //      Five to the shift, twelve powers at a time, because
                //      5^12 is the largest power of five below the limb base.
                while (left >= 12)
                {
                        format_bignum_scale(address_of big, 244140625);
                        left -= 12;
                }

                if (left)
                        format_bignum_scale(address_of big, powers_of_five[left]);

                format_bignum_multiply(address_of big, below);

                fraction_length = format_bignum_digits(address_of big, fraction);

                //      The fraction is exactly shift places wide. Whatever the
                //      big integer is short by is its leading zero run.
                pad = shift > fraction_length ? shift - fraction_length : 0;

                //      Three runs, each clipped to what is left of the digit
                //      array exactly as the byte loops that were here clipped
                //      it: the integer part, the fraction's leading zeros,
                //      and the fraction's own digits. The pad alone reaches a
                //      thousand places on a subnormal, which is a thousand
                //      loop iterations where memory_fill is one call.
                room = FORMAT_DIGITS - count;
                take = whole_length < room ? whole_length : room;
                memory_copy(number->digit + count, whole, take);
                count += take;

                room = FORMAT_DIGITS - count;
                take = pad < room ? pad : room;
                memory_fill(number->digit + count, '0', take);
                count += take;

                room = FORMAT_DIGITS - count;
                take = fraction_length < room ? fraction_length : room;
                memory_copy(number->digit + count, fraction, take);
                count += take;

                number->count = count;
                number->exponent = (bipolar)whole_length;
        }

        //      Leading zeros are not digits of the value. They are a statement
        //      about where the point is, so they move into the exponent.
        //      The run of leading zeros is a span of one byte value, which
        //      is what memory_span_byte answers. On a small number it is a
        //      few bytes; on 1e-300 it is three hundred.
        first = memory_span_byte(number->digit, '0', number->count);

        if (first == number->count)
        {
                number->count = 0;
                number->exponent = 1;
                return;
        }

        if (first)
        {
                //      The regions overlap and memory_copy is the overlap
                //      aware one, which is what its contract in library.c
                //      says and is why memory_copy_apart is not named here.
                memory_copy(number->digit, number->digit + first,
                            number->count - first);

                number->count -= first;
                number->exponent -= (bipolar)first;
        }

        //      Trailing zeros are real digits but nothing ever asks for them:
        //      a read past the end already answers zero. Dropping them makes
        //      every later loop shorter and changes no answer.
        while (number->count && number->digit[number->count - 1] == '0')
                number->count--;
}

/*
        Keep the leading `keep` digits and round the rest away, half to even.

        A carry that runs off the front is the 999 becoming 1000 case. The
        digits become a single one and the exponent goes up, and that is the
        only place in the whole conversion where the exponent moves after
        expansion. It is also why %g picks between its two shapes after
        rounding rather than before: 9.99e-5 at three significant digits is
        still 9.99e-5, but at two it is 1.0e-4, and the two print differently.
*/
static fn format_round(format_number address_to number, bipolar keep)
{
        positive index;
        bool up;

        if (keep < 0)
        {
                number->count = 0;
                number->exponent = 1;
                return;
        }

        if ((positive)keep >= number->count)
                return;

        index = (positive)keep;

        if (number->digit[index] > '5')
        {
                up = true;
        }
        else if (number->digit[index] < '5')
        {
                up = false;
        }
        else
        {
                //      MEASURED, AND LEFT AS A BYTE LOOP ON PURPOSE.
                //
                //      "Are the digits after the tie all zeros" is a span of
                //      one byte value and memory_span_byte answers exactly
                //      that. It was called, and it lost 2.1% of the whole
                //      %.6f benchmark: this loop is only entered when the
                //      first discarded digit is a five, and the byte after a
                //      five is not a zero about nine times in ten, so the
                //      loop almost always stops on its first compare and the
                //      call's setup is pure loss.
                positive after = index + 1;

                up = false;

                while (after < number->count)
                {
                        if (number->digit[after] != '0')
                        {
                                up = true;
                                break;
                        }

                        after++;
                }

                //      An exact tie goes to the even neighbour, and the digit
                //      before the first kept one, when there is none, is a
                //      zero and zero is even.
                if (!up)
                        up = index > 0 &&
                             ((number->digit[index - 1] - '0') & 1) != 0;
        }

        number->count = index;

        if (!up)
        {
                while (number->count && number->digit[number->count - 1] == '0')
                        number->count--;

                if (number->count == 0)
                        number->exponent = 1;

                return;
        }

        while (number->count)
        {
                if (number->digit[number->count - 1] != '9')
                {
                        number->digit[number->count - 1]++;
                        return;
                }

                number->count--;
        }

        number->digit[0] = '1';
        number->count = 1;
        number->exponent++;
}

static inline INLINE p8 format_number_sign(p64 bits,
                                            format_spec address_to spec)
{
        return bits >> 63 ? '-' : spec->flags & FORMAT_FLAG_PLUS ? '+' :
               spec->flags & FORMAT_FLAG_SPACE ? ' ' : 0;
}

/*
        The three decimal float shapes, and the field around them.

        The body's length is computed before a byte is written, because a right
        aligned field pads first and no streaming emitter can know how long a
        float body is halfway through it. Everything after that is the pieces
        in the order C lists them.

        The 0 flag goes after the sign and before the digits, which is why the
        two padding decisions are separated rather than written once: a space
        pad precedes the sign and a zero pad follows it.
*/
static fn format_decimal_field(format_sink address_to sink, decimal value,
                               format_spec address_to spec)
{
        format_number number;
        union
        {
                decimal value;
                p64 bits;
        } view;
        p8 sign = 0;
        p8 style = spec->conversion;
        bipolar precision = spec->precision;
        positive integer_length;
        positive fraction_length;
        positive exponent_length = 0;
        p8 exponent_digits[8];
        positive body;
        positive spaces = 0;
        positive run;
        bool point;
        bool upper = style == 'E' || style == 'F' || style == 'G' || style == 'A';

        view.value = value;

        sign = format_number_sign(view.bits, spec);

        //      Infinities and not-a-numbers are words, and a word is never
        //      zero padded however loudly the flag asks.
        if (((view.bits >> 52) & 0x7ff) == 0x7ff)
        {
                string_address word;
                positive length;

                if (view.bits & (((p64)1 << 52) - 1))
                        word = (string_address)(upper ? "NAN" : "nan");
                else
                        word = (string_address)(upper ? "INF" : "inf");

                length = 3 + (sign != 0);
                spaces = spec->width > length ? spec->width - length : 0;

                if (!(spec->flags & FORMAT_FLAG_LEFT))
                        format_fill(sink, ' ', spaces);

                if (sign)
                        format_emit(sink, address_of sign, 1);

                format_emit(sink, (address_any)word, 3);

                if (spec->flags & FORMAT_FLAG_LEFT)
                        format_fill(sink, ' ', spaces);

                return;
        }

        if (precision < 0)
                precision = 6;

        format_expand(value, address_of number);

        if (style == 'F')
                style = 'f';

        if (style == 'g' || style == 'G')
        {
                bipolar significant = precision == 0 ? 1 : precision;
                bipolar shown;

                format_round(address_of number, significant);

                shown = number.exponent - 1;

                if (shown < -4 || shown >= significant)
                {
                        style = (style == 'G') ? 'E' : 'e';
                        precision = significant - 1;
                }
                else
                {
                        style = 'f';
                        precision = significant - 1 - shown;
                }

                //      A %g that was not asked to keep its trailing zeros
                //      keeps only the places a digit actually reaches.
                if (!(spec->flags & FORMAT_FLAG_ALTERNATE))
                {
                        bipolar reach;

                        if (style == 'f')
                                reach = (bipolar)number.count - number.exponent;
                        else
                                reach = (bipolar)number.count - 1;

                        if (reach < 0)
                                reach = 0;

                        if (reach < precision)
                                precision = reach;
                }
        }
        else if (style == 'e' || style == 'E')
        {
                format_round(address_of number, precision + 1);
        }
        else
        {
                format_round(address_of number, number.exponent + precision);
        }

        point = precision > 0 || (spec->flags & FORMAT_FLAG_ALTERNATE);

        if (style == 'e' || style == 'E')
        {
                bipolar shown = number.count ? number.exponent - 1 : 0;
                positive magnitude = (positive)(shown < 0 ? -shown : shown);

                integer_length = 1;
                fraction_length = (positive)precision;

                exponent_digits[0] = (p8)style;
                exponent_digits[1] = shown < 0 ? '-' : '+';

                //      Two exponent digits at least, which is the one place
                //      printf pads without being asked.
                if (magnitude < 10)
                {
                        exponent_digits[2] = '0';
                        exponent_digits[3] = (p8)('0' + magnitude);
                        exponent_length = 4;
                }
                else
                {
                        exponent_length =
                            2 + positive_into(exponent_digits + 2, magnitude);
                }
        }
        else
        {
                integer_length =
                    number.exponent > 0 ? (positive)number.exponent : 1;
                fraction_length = (positive)precision;
        }

        body = (sign != 0) + integer_length + (point ? 1 : 0) + fraction_length +
               exponent_length;

        if (spec->width > body)
                spaces = spec->width - body;

        if (!(spec->flags & FORMAT_FLAG_LEFT) && !(spec->flags & FORMAT_FLAG_ZERO))
                format_fill(sink, ' ', spaces);

        if (sign)
                format_emit(sink, address_of sign, 1);

        if (!(spec->flags & FORMAT_FLAG_LEFT) && (spec->flags & FORMAT_FLAG_ZERO))
        {
                format_fill(sink, '0', spaces);
                spaces = 0;
        }

        if (style == 'e' || style == 'E')
        {
                p8 lead = number.count ? number.digit[0] : '0';

                format_emit(sink, address_of lead, 1);

                if (point)
                        format_emit(sink, (address_any) ".", 1);

                //      Digits one upward, which are contiguous in the run for
                //      as far as the run reaches and are zeros after that.
                run = number.count > 1 ? number.count - 1 : 0;

                if (run > fraction_length)
                        run = fraction_length;

                format_emit(sink, number.digit + 1, run);
                format_fill(sink, '0', fraction_length - run);

                format_emit(sink, exponent_digits, exponent_length);
        }
        else
        {
                if (number.exponent > 0)
                {
                        positive whole = (positive)number.exponent;

                        run = number.count < whole ? number.count : whole;

                        format_emit(sink, number.digit, run);
                        format_fill(sink, '0', whole - run);
                }
                else
                {
                        format_emit(sink, (address_any) "0", 1);
                }

                if (point)
                        format_emit(sink, (address_any) ".", 1);

                //      The fraction reads digits from number.exponent upward
                //      for fraction_length places. Anything below zero and
                //      anything at or past the end of the run is a zero, so
                //      the whole field is at most three pieces: a leading
                //      zero run, the part of the digit run that falls inside
                //      the field, and a trailing zero run.
                {
                        bipolar reach = number.exponent + (bipolar)fraction_length;
                        bipolar begin = number.exponent < 0 ? 0 : number.exponent;
                        bipolar stop = reach > (bipolar)number.count
                                               ? (bipolar)number.count
                                               : reach;
                        bipolar cut = begin > reach ? reach : begin;
                        positive lead_zeros = (positive)(cut - number.exponent);

                        run = stop > begin ? (positive)(stop - begin) : 0;

                        format_fill(sink, '0', lead_zeros);
                        format_emit(sink, number.digit + begin, run);
                        format_fill(sink, '0',
                                    fraction_length - lead_zeros - run);
                }
        }

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        The hexadecimal float, which is the one shape that needs no arithmetic.

        A normal double is written with its implicit leading one before the
        point and its fifty-two stored bits after it, and the exponent is the
        unbiased power of two. A subnormal keeps the stored leading zero and
        prints the same -1022 every subnormal has, which is why nothing here
        renormalises: matching means printing what is stored.

        With no precision the fraction is as short as it can be without losing
        a bit, so trailing zero nibbles go. With a precision the nibbles round
        half to even at that place, and a carry can reach the leading digit and
        turn its 1 into a 2. glibc does the same and does not renormalise
        either.

        Nibbles are held as values rather than as characters so the carry is
        arithmetic; the alphabet is applied once, on the way out.
*/
static fn format_hex_field(format_sink address_to sink, decimal value,
                           format_spec address_to spec)
{
        union
        {
                decimal value;
                p64 bits;
        } view;
        const p8 address_to alphabet;
        p8 sign = 0;
        p8 nibble[16];
        p8 text[16];
        p8 exponent_digits[8];
        positive nibble_count = 13;
        positive exponent_length;
        positive index;
        positive extra = 0;
        positive body;
        positive spaces = 0;
        positive mantissa;
        bipolar raw;
        bipolar power;
        p8 lead;
        bool point;
        bool upper = spec->conversion == 'A';

        alphabet = (const p8 address_to)(upper ? "0123456789ABCDEF"
                                               : "0123456789abcdef");
        view.value = value;

        if (((view.bits >> 52) & 0x7ff) == 0x7ff)
        {
                format_spec word = address_to spec;

                word.conversion = upper ? 'E' : 'e';
                format_decimal_field(sink, value, address_of word);
                return;
        }

        sign = format_number_sign(view.bits, spec);

        mantissa = (positive)(view.bits & (((p64)1 << 52) - 1));
        raw = (bipolar)((view.bits >> 52) & 0x7ff);

        if (raw == 0)
        {
                lead = 0;
                power = mantissa ? -1022 : 0;
        }
        else
        {
                lead = 1;
                power = raw - 1023;
        }

        for (index = 0; index < 13; index++)
                nibble[index] = (p8)((mantissa >> (48 - 4 * index)) & 15);

        if (spec->precision < 0)
        {
                while (nibble_count && nibble[nibble_count - 1] == 0)
                        nibble_count--;
        }
        else if ((positive)spec->precision < nibble_count)
        {
                positive keep = (positive)spec->precision;
                bool up;

                if (nibble[keep] > 8)
                {
                        up = true;
                }
                else if (nibble[keep] < 8)
                {
                        up = false;
                }
                else
                {
                        positive after = keep + 1;

                        up = false;

                        while (after < nibble_count)
                        {
                                if (nibble[after])
                                {
                                        up = true;
                                        break;
                                }

                                after++;
                        }

                        if (!up)
                                up = ((keep ? nibble[keep - 1] : lead) & 1) != 0;
                }

                nibble_count = keep;

                while (up && nibble_count)
                {
                        if (nibble[nibble_count - 1] == 15)
                        {
                                nibble[nibble_count - 1] = 0;
                                nibble_count--;
                                continue;
                        }

                        nibble[nibble_count - 1]++;
                        up = false;
                }

                //      The carry ran out of nibbles, so it lands on the digit
                //      before the point. Nothing renormalises afterwards.
                if (up)
                        lead++;

                nibble_count = keep;
        }
        else
        {
                extra = (positive)spec->precision - nibble_count;
        }

        for (index = 0; index < nibble_count; index++)
                text[index] = alphabet[nibble[index]];

        point = nibble_count + extra > 0 || (spec->flags & FORMAT_FLAG_ALTERNATE);

        exponent_digits[0] = upper ? 'P' : 'p';
        exponent_digits[1] = power < 0 ? '-' : '+';
        exponent_length =
            2 + positive_into(exponent_digits + 2,
                              (positive)(power < 0 ? -power : power));

        body = (sign != 0) + 2 + 1 + (point ? 1 : 0) + nibble_count + extra +
               exponent_length;

        if (spec->width > body)
                spaces = spec->width - body;

        if (!(spec->flags & FORMAT_FLAG_LEFT) && !(spec->flags & FORMAT_FLAG_ZERO))
                format_fill(sink, ' ', spaces);

        if (sign)
                format_emit(sink, address_of sign, 1);

        format_emit(sink, (address_any)(upper ? "0X" : "0x"), 2);

        if (!(spec->flags & FORMAT_FLAG_LEFT) && (spec->flags & FORMAT_FLAG_ZERO))
        {
                format_fill(sink, '0', spaces);
                spaces = 0;
        }

        {
                p8 first = alphabet[lead & 15];

                format_emit(sink, address_of first, 1);
        }

        if (point)
                format_emit(sink, (address_any) ".", 1);

        format_emit(sink, text, nibble_count);
        format_fill(sink, '0', extra);
        format_emit(sink, exponent_digits, exponent_length);

        if (spec->flags & FORMAT_FLAG_LEFT)
                format_fill(sink, ' ', spaces);
}

/*
        The engine.

        One pass over the format string. A run of plain text is found whole and
        emitted in one call, because that is where the bytes are: a message
        with one specifier in forty characters should cross the sink twice and
        not forty times.

        A specifier nobody knows is written out complete, percent and all,
        which makes a stray percent in a message survive rather than silently
        eat the character after it. glibc agrees on "%y" and disagrees on
        "%zz", where it drops the length modifier and prints "%z"; both are
        undefined behaviour and printing the bytes back is the more useful of
        the two. A percent that is the last byte of the format writes nothing,
        which is string_format's rule one floor down, where glibc instead
        abandons the call and returns minus one. Those two and one glibc bug in
        %#g are the only places in 45,225 compared pairs where this and a real
        glibc do not produce the same bytes.
*/
static fn format_run(format_sink address_to sink, string_address format,
                     var_args list)
{
        string_address at = format;

        if (is_null(format))
        {
                format_emit(sink, (address_any) "(null)", 6);
                return;
        }

        while (string_get(at))
        {
                string_address start = at;
                format_spec spec;
                positive base = 10;

                //      MEASURED, AND LEFT AS A BYTE LOOP ON PURPOSE.
                //
                //      This is exactly strchrnul and string_first_of_or_end
                //      is exactly that routine, so the house rule says call
                //      it. It was called, and it lost. The vectorised scan
                //      costs a flat forty-eight cycles a call whatever the
                //      run length; this loop costs about 1.3 cycles a byte
                //      on top of a smaller fixed cost, so the crossover is
                //      near ten bytes -- and 65% of the literal runs in this
                //      tree's own format strings are shorter than that, mean
                //      length 8.9. Whole formats measured both ways on
                //      x86_64: "%d\n" 11.8% slower with the call, "connecting
                //      to %s on port %d\n" 4.5% slower, a sixty-two byte
                //      three-conversion line 3.5% slower, and a two-line
                //      usage message 26% faster. A format string is short
                //      text between conversions, which is the shape the byte
                //      loop is better at.
                while (string_get(at) && string_get(at) != '%')
                        at++;

                if (at != start)
                        format_emit(sink, (address_any)start,
                                    (positive)(at - start));

                if (!string_get(at))
                        break;

                start = at;
                at++;

                spec.flags = conversion_flags_take(address_of at);
                spec.width = 0;
                spec.precision = -1;
                spec.length = FORMAT_LENGTH_INT;

                if (string_get(at) == '*')
                {
                        b32 given = var_list_get(list, b32);

                        at++;

                        //      A negative star width is a left flag and a
                        //      positive width, which is the one place a flag
                        //      can arrive after the flags have been read.
                        if (given < 0)
                        {
                                spec.flags |= FORMAT_FLAG_LEFT;
                                spec.width = (positive)(0 - (bipolar)given);
                        }
                        else
                        {
                                spec.width = (positive)given;
                        }
                }
                else
                {
                        while (byte_is_digit(string_get(at)))
                        {
                                spec.width = spec.width * 10 +
                                             (positive)(string_get(at) - '0');
                                at++;
                        }
                }

                if (string_get(at) == '.')
                {
                        at++;
                        spec.precision = 0;

                        if (string_get(at) == '*')
                        {
                                b32 given = var_list_get(list, b32);

                                at++;

                                //      A negative star precision is no
                                //      precision at all, and not a precision
                                //      of zero.
                                spec.precision = given < 0 ? -1 : (bipolar)given;
                        }
                        else
                        {
                                while (byte_is_digit(string_get(at)))
                                {
                                        spec.precision =
                                            spec.precision * 10 +
                                            (bipolar)(string_get(at) - '0');
                                        at++;
                                }
                        }
                }

                spec.length = conversion_length_take(address_of at);

                spec.conversion = string_get(at);

                switch (spec.conversion)
                {
                case 'd':
                case 'i':
                {
                        bipolar signed_value;
                        bool negative;
                        positive value;

                        at++;

                        switch (spec.length)
                        {
                        case FORMAT_LENGTH_CHAR:
                                signed_value = (b8)var_list_get(list, b32);
                                break;
                        case FORMAT_LENGTH_SHORT:
                                signed_value = (b16)var_list_get(list, b32);
                                break;
                        case FORMAT_LENGTH_LONG:
                        case FORMAT_LENGTH_LONG_LONG:
                        case FORMAT_LENGTH_SIZE:
                        case FORMAT_LENGTH_DIFFERENCE:
                        case FORMAT_LENGTH_WIDEST:
                                signed_value = var_list_get(list, bipolar);
                                break;
                        default:
                                signed_value = var_list_get(list, b32);
                                break;
                        }

                        negative = signed_value < 0;

                        //      Negating the most negative value is undefined
                        //      as a signed operation and exact as an unsigned
                        //      one, which is why the cast happens first.
                        value = negative ? (positive)0 - (positive)signed_value
                                         : (positive)signed_value;

                        format_integer(sink, value, 10, negative,
                                       address_of spec);
                        continue;
                }
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                case 'b':
                case 'B':
                {
                        positive value;

                        at++;

                        switch (spec.length)
                        {
                        case FORMAT_LENGTH_CHAR:
                                value = (p8)var_list_get(list, p32);
                                break;
                        case FORMAT_LENGTH_SHORT:
                                value = (p16)var_list_get(list, p32);
                                break;
                        case FORMAT_LENGTH_LONG:
                        case FORMAT_LENGTH_LONG_LONG:
                        case FORMAT_LENGTH_SIZE:
                        case FORMAT_LENGTH_DIFFERENCE:
                        case FORMAT_LENGTH_WIDEST:
                                value = var_list_get(list, positive);
                                break;
                        default:
                                value = var_list_get(list, p32);
                                break;
                        }

                        if (spec.conversion == 'o')
                                base = 8;
                        else if (spec.conversion == 'x' || spec.conversion == 'X')
                                base = 16;
                        else if (spec.conversion == 'b' || spec.conversion == 'B')
                                base = 2;

                        format_integer(sink, value, base, false, address_of spec);
                        continue;
                }
                case 'c':
                {
                        p8 byte = (p8)var_list_get(list, b32);

                        at++;
                        format_text_field(sink, address_of byte, 1, address_of spec);
                        continue;
                }
                case 's':
                {
                        string_address text = var_list_get(list, string_address);
                        positive length;

                        at++;

                        if (is_null(text))
                        {
                                //      A null pointer prints the word, but a
                                //      precision that cannot hold the whole
                                //      word prints nothing at all rather than
                                //      a piece of it. glibc draws the line
                                //      there and a truncated "(nu" would be
                                //      worse than silence anyway.
                                length = 6;
                                text = (string_address) "(null)";

                                if (spec.precision >= 0 &&
                                    (positive)spec.precision < length)
                                        length = 0;
                        }
                        else if (spec.precision >= 0)
                        {
                                length = string_length_max(
                                    text, (positive)spec.precision);
                        }
                        else
                        {
                                length = string_length(text);
                        }

                        format_text_field(sink, (address_any)text, length,
                                          address_of spec);
                        continue;
                }
                case 'p':
                {
                        address_any pointer = var_list_get(list, address_any);

                        at++;
                        spec.precision = -1;
                        spec.flags &= ~FORMAT_FLAG_ALTERNATE;

                        if (is_null(pointer))
                        {
                                spec.conversion = 's';
                                format_text_field(sink,
                                                  (address_any) "(nil)", 5,
                                                  address_of spec);
                                continue;
                        }

                        format_integer(sink, (positive)pointer, 16, false,
                                       address_of spec);
                        continue;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                {
                        decimal given = var_list_get(list, decimal);

                        at++;
                        format_decimal_field(sink, given, address_of spec);
                        continue;
                }
                case 'a':
                case 'A':
                {
                        decimal given = var_list_get(list, decimal);

                        at++;
                        format_hex_field(sink, given, address_of spec);
                        continue;
                }
                case '%':
                        at++;
                        format_emit(sink, (address_any) "%", 1);
                        continue;
                default:
                        //      %n is not here and is not an oversight. It is
                        //      the primitive that turns a format string a
                        //      program did not control into a write anywhere
                        //      in memory, nothing in this tree needs it, and
                        //      it falls through to the line below and prints
                        //      itself back like any other conversion nobody
                        //      knows.
                        //
                        //      A percent that is the last byte of the format
                        //      writes nothing, which is string_format's rule
                        //      one floor down and is what callers in this
                        //      tree already expect. glibc calls it an error
                        //      and returns minus one instead.
                        if (!string_get(at))
                                continue;

                        //      Anything else that is not a conversion is
                        //      text, and the whole run of it including the
                        //      percent comes out unchanged.
                        at++;
                        format_emit(sink, (address_any)start,
                                    (positive)(at - start));
                        continue;
                }
        }
}

/*
        THE STANDARD NAMES

        These carry the standard spellings rather than prose ones. printf is
        not a description of an operation the way memory_copy is; it is a fixed
        interface with a fixed signature that thirty years of C expects to find
        under that exact name, and a prose alias would only be a second name
        for the same thing. The prose names in this family are the ones nobody
        else fixed: format_run is the engine, format_sink is the destination,
        and format_to_writer below is the entry this tree's own code should
        prefer, since a writer is what everything here already passes around.

        Every entry returns the length the format WANTED, which for printf and
        fprintf is also the length written, and for snprintf deliberately is
        not.
*/

static bipolar format_to_writer(writer write, string_address format, ...)
{
        format_sink sink = {0};
        var_args list;

        sink.downstream = write;

        var_list(list, format);
        format_run(address_of sink, format, list);
        var_list_end(list);

        return (bipolar)sink.counted;
}

static bipolar vfprintf(format_stream stream, string_address format,
                        var_args list)
{
        format_sink sink = {0};

        sink.stream = stream;
        sink.streaming = true;
        format_run(address_of sink, format, list);

        return (bipolar)sink.counted;
}

static bipolar vprintf(string_address format, var_args list)
{
        return vfprintf(format_output, format, list);
}

/*
        The bounded buffer entries.

        Capacity here is the room for bytes, not the room for the buffer: one
        byte of the caller's size is always the terminator. A size of zero
        leaves the buffer alone entirely -- no terminator, no read, and the
        pointer is allowed to be null -- and still returns the length, which is
        the call a caller makes on purpose to find out how much to allocate.
*/
static bipolar vsnprintf(p8 address_to into, positive size, string_address format,
                         var_args list)
{
        format_sink sink = {0};

        sink.buffer = size ? into : null;
        sink.capacity = size ? size - 1 : 0;

        format_run(address_of sink, format, list);

        if (size)
                into[sink.used] = end;

        return (bipolar)sink.counted;
}

static bipolar vsprintf(p8 address_to into, string_address format, var_args list)
{
        format_sink sink = {0};

        sink.buffer = into;
        sink.capacity = ~(positive)0 >> 1;

        format_run(address_of sink, format, list);
        into[sink.used] = end;

        return (bipolar)sink.counted;
}

var_list_entry(printf, bipolar, (string_address format, ...), format,
               vprintf(format, _variadic_list))
var_list_entry(fprintf, bipolar,
               (format_stream stream, string_address format, ...), format,
               vfprintf(stream, format, _variadic_list))
var_list_entry(snprintf, bipolar,
               (p8 address_to into, positive size, string_address format, ...),
               format, vsnprintf(into, size, format, _variadic_list))
var_list_entry(sprintf, bipolar,
               (p8 address_to into, string_address format, ...), format,
               vsprintf(into, format, _variadic_list))

/*
        The entries that write bytes without reading a format.

        fputc and fputs are here only when the stream family is not, because a
        byte to a stream is that family's operation and it already exports both
        as aliases onto its own put_byte and put_string. puts and putchar are
        always here: nothing else in the tree defines them, and puts is not
        fputs with a newline glued on -- the asymmetry is real and is what C
        says, and it is the single most common surprise in the whole header.
*/
#ifdef FORMAT_OWNS_BYTE_ENTRIES

static b32 fputc(b32 byte, format_stream stream)
{
        p8 value = (p8)byte;

        if (format_stream_write(stream, address_of value, 1) != 1)
                return -1;

        return (b32)value;
}

static b32 fputs(string_address text, format_stream stream)
{
        positive length;

        if (is_null(text))
                return -1;

        length = string_length(text);

        if (length &&
            format_stream_write(stream, (address_any)text, length) != length)
                return -1;

        return 1;
}

#endif // FORMAT_OWNS_BYTE_ENTRIES

static b32 putchar(b32 byte)
{
        return fputc(byte, format_output);
}

static b32 puts(string_address text)
{
        positive length;

        if (is_null(text))
                return -1;

        length = string_length(text);

        if (length &&
            format_stream_write(format_output, (address_any)text, length) !=
                length)
                return -1;

        if (format_stream_write(format_output, (address_any) "\n", 1) != 1)
                return -1;

        return 1;
}

/*
        perror writes the prefix, a colon, a space, the sentence and a newline;
        with an empty or absent prefix it writes the sentence and the newline
        alone. It is one call per piece rather than one formatted line because
        the sentence is the last thing a failing program gets to say, and a
        formatter that itself buffered would be the wrong thing to depend on
        there.

        Only when the error family is absent. That family builds the same line
        into a stack buffer and writes it with one trap, without going through
        a stream at all, which is a better answer for the same reason: a
        diagnostic wants to be independent of everything that might be what
        failed. Its version wins wherever it is present.
*/
#ifdef FORMAT_OWNS_PERROR

static fn perror(string_address prefix)
{
        string_address reason = strerror((b32)errno);

        if (prefix && string_get(prefix))
        {
                format_stream_write(format_error, (address_any)prefix,
                                    string_length(prefix));
                format_stream_write(format_error, (address_any) ": ", 2);
        }

        format_stream_write(format_error, (address_any)reason,
                            string_length(reason));
        format_stream_write(format_error, (address_any) "\n", 1);
}

#endif // FORMAT_OWNS_PERROR

#endif // KERNEL_MODE

#endif // STANDARD_MODERN_C_FORMAT
