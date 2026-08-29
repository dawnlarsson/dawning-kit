/*
        Experimental C standard library

        numbers: text into a number, and the decimal one correctly rounded

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_NUMBERS
#define STANDARD_MODERN_C_STANDARD_NUMBERS

/*
        Guarded out of the kernel build and out of a no-platform build, for
        the reason every file in this directory carries the same three lines:
        core.c includes the umbrella, library.c sets KERNEL_MODE from
        __MODULE__, and a kernel that already has its own errno and its own
        idea of a double must not be handed a second one.
*/
#if !defined(KERNEL_MODE) && !defined(STANDARD_NO_PLATFORM)

/*
        THE HALF OF THIS FAMILY THAT WAS ALREADY WRITTEN, AND WHY IT IS ONLY
        DECLARED HERE

        src/platform/standard.inc holds the whole integer half of the number
        conversions as assembly at three-architecture parity, and attaches the
        C names to it with ASM_ALIAS:

              abs labs llabs        absolute_whole, absolute_wide
              atoi atol atoll       string_to_whole, string_to_whole_wide
              strtol strtoll        string_to_number
              strtoul strtoull      string_to_number_unsigned

        ASM_ALIAS emits `.set strtol, string_to_number` into the one assembly
        stream this translation unit becomes. That is a real global symbol
        with no C declaration in front of it, which is exactly the gap the
        tree has been carrying: nm shows "T strtol" in the built object, and
        `strtol(text, &stop, 0)` still fails to compile, because the compiler
        has never been told the name exists. Worse, an undeclared call in
        C99 and later is an error rather than an implicit int, so the failure
        is at the first call site rather than at the link.

        So the block below is declarations and nothing else. Writing a C body
        for any of these names would not override the assembly -- it would be
        a second definition of the same label in the same assembly stream,
        which the assembler rejects outright, not a link-time tie the linker
        could break. The correct fix for a name that already has a symbol is
        to declare it, and that is what this does.

        The string parameters are string_address, which is p8 address_to and
        not char address_to, for the reason strerror gives in error.c: every
        string in this tree is unsigned, and a program that assigns the other
        way makes the same conversion it already makes for string_find. All
        three cross compilers accept the whole block below with no warning at
        all, without -w, which was checked rather than assumed.

        WHAT THE ASSEMBLY DOES NOT DO, WHICH IS ERRNO

        string_to_number saturates on overflow and says nothing, and the
        paragraph above it in standard.inc says so and says why: there was no
        errno when it was written. There is one now, in error.c, and C wants
        strtol to set ERANGE and return the clamped value. That cannot be
        fixed from here. A C wrapper cannot wrap a name the assembly has
        already taken, and a wrapper under a different name that the alias
        then pointed at would have to re-measure the digit run the assembly
        just walked in order to tell a spelled-out limit from a clamped one.
        It is a change to standard.inc, it belongs to whoever owns that file,
        and it is written down here rather than papered over. strtod below
        does set ERANGE, because strtod is new and nothing had taken the name.
*/
/*
        These ten were declared here in the house types, and are not any more.

        declare.c says them first, in C's own spellings -- long labs(long),
        int atoi(const char *) -- and it is right to and this file was not.
        The two disagree about nothing at runtime: every one of them is an
        ASM_ALIAS onto a prose routine, so the address is the same and the
        register is the same, and `positive` and `size_t` are the same width
        in the same register on all three of these targets. They disagree
        about TYPE, which is what a second declaration is checked against,
        and C's spelling is the one that has to win: the whole reason
        declare.c exists is that a program bringing its own <stdlib.h> line
        must compile against these, and it cannot if the tree has already
        said labs takes a bipolar.

        So the declarations are gone and nothing else is. The definitions
        this file does own -- strtod and the <inttypes.h> spellings below --
        are unaffected, and the ERANGE note above still stands.
*/

/*
        The three <inttypes.h> spellings, which are the only integer names in
        this family that had no symbol at all.

        intmax_t and uintmax_t are b64 and p64 in library.c, and they only
        exist when a program has asked for the compatibility spellings, so
        the signatures below are written in the house types those alias --
        bipolar and positive -- and each of these is the wide routine under a
        different name and nothing else. They are
        wrappers rather than macros so that a program can take the address of
        one, which is the same reason math.c gives for its own wrappers, and
        static like everything else here so that an unused one leaves no code
        behind and none of them collides with a program that also links a
        real libc.
*/
static bipolar imaxabs(bipolar value)
{
        return labs(value);
}

static bipolar strtoimax(string_address input, string_address address_to stopped,
                          b32 base)
{
        return strtol((const char address_to)input, (char address_to address_to)stopped, base);
}

static positive strtoumax(string_address input, string_address address_to stopped,
                           b32 base)
{
        return strtoul((const char address_to)input, (char address_to address_to)stopped, base);
}

/*
        AND THE HALF THAT WAS ABSENT: DECIMAL TEXT INTO A BINARY FLOAT

        Everything below is strtod, strtof and strtold, and the whole of the
        work is the word "correctly". A conversion that is merely close is
        four lines -- accumulate the digits into a double and multiply by a
        power of ten -- and it is wrong by up to several units in the last
        place, because each of those operations rounds and the errors
        compound. A conversion that is correct returns, for every input, the
        representable number nearest the exact decimal value the text names,
        with ties going to the even significand, and it has to do that for
        the subnormals, for the boundaries, and for an input with nine
        hundred digits in it.

        THE THREE TIERS, FASTEST FIRST

        1. The exact tier. If the significant digits fit in a p64 with room
           to spare -- nineteen of them or fewer -- and the integer they form
           is small enough to be a double exactly, and the power of ten is
           between minus twenty two and twenty two, where every power of ten
           is itself exactly a double, then the answer is one multiply or one
           divide of two exact operands. IEEE says a single operation is
           correctly rounded, so the answer is correctly rounded, and nothing
           else needs to happen. This is Clinger's fast path from 1990 and it
           takes most of the traffic a real program generates: 1.5, 0.1,
           3.14159, 6.02e23.

        2. The estimating tier, which is Eisel and Lemire's. The significant
           digits go into a p64 and the power of ten comes out of a table of
           128 bit truncated powers; one 64x64 to 128 multiply, and sometimes
           a second, produces the significand together with a proof that the
           bits below it cannot reach the rounding boundary. When the proof
           holds the answer is correctly rounded and the whole conversion was
           two multiplies. When it does not hold -- which is where the value
           sits so near a boundary that 128 bits cannot separate it -- the
           tier declines to answer rather than guessing, and tier three runs.
           That refusal is what makes this safe: an estimator that guessed
           would be wrong rarely, which is the worst frequency to be wrong at.

        3. The exact tier of last resort, which is Clinger's and Gay's idea
           in the shape Go's strconv gives it. The digits go into a register
           of decimal digits, and multiplying or dividing by two is done on
           those decimal digits directly, in long arithmetic, exactly. Scale
           by powers of two until the value sits in [1/2, 1), read off one
           more bit than the significand holds, and round on the decimal
           digits -- where a tie is a real tie, because a decimal register
           that has not overflowed holds the exact value and one that has
           overflowed knows it and says the value is greater than what it
           holds. This is slow -- a few microseconds for a hard input -- and
           it is never wrong.

        The three tiers agree by construction, and the test lane checks that
        rather than trusting it: it runs the same inputs with tiers one and
        two disabled and diffs, so a defect in either fast tier shows up
        without glibc having to be in the loop.

        WHY THE SLOW TIER IS PARAMETERISED AND THE FAST ONES ARE NOT

        binary32, binary64, the eighty bit x87 format and binary128 differ in
        four numbers: how many bits the stored significand has, where the
        exponent field starts, how wide it is, and the bias. Written that way
        the slow tier is one body that serves all four, which is what makes
        strtold cost almost nothing here -- and strtold is the one where
        long double is eighty bit on x86_64 and binary128 on arm64 and
        riscv64, so a body written for one of them would have been wrong on
        the other two. The fast tiers are not parameterised: they are for
        strtod and strtof, the two formats a program actually converts in a
        loop, and strtold goes straight to tier three every time.

        WHAT THE STANDARD ASKS FOR AND IMPLEMENTATIONS FORGET

        Leading whitespace, an optional sign, "inf" and "infinity" and "nan"
        in any mixture of cases, an optional parenthesised sequence after nan,
        hexadecimal floats with an optional binary exponent, an end pointer
        that on failure points at the ORIGINAL string rather than at wherever
        the scan gave up, and ERANGE in errno on overflow and on underflow.
        All of it is here and all of it is in the test lane, because that is
        where implementations diverge from each other far more often than
        they diverge on the arithmetic.

        WHAT THE FLOOR GIVES AND WHAT IT CANNOT

        byte_is_space walks the leading whitespace, byte_is_digit,
        byte_is_hexadecimal and byte_is_alnum classify, byte_to_lower folds
        the exponent letter and the hexadecimal ones, string_length_max gives
        the bounded page-safe length that lets a fixed-width compare run at
        all, string_compare_folded_max is strncasecmp and recognises
        "infinity", "inf" and "nan" without a byte loop of its own, strtoull
        reads the payload inside a NaN's brackets, bits_leading_zeros
        normalises the significand for the estimating tier, and memory_copy
        takes the register aside for the tininess trial.

        What the floor cannot give is the digit walk itself, and the reason is
        worth writing down rather than leaving as an absence. The obvious
        candidate is memory_span_byte, which counts a run of one byte value --
        the leading zeros, say. It does not fit: it wants a size, and a
        NUL-terminated decimal has no size until something has walked it, so
        finding one would cost a string_length over text the parse is about to
        walk anyway. And the run of zeros is not the only thing the pass is
        doing; it is also moving the decimal point, deciding whether the point
        has been seen, and filling the register, all of which have to happen
        in step with the same byte. string_to_positive is the other candidate
        and it answers a different question: it reads backwards from the
        terminator, with no end pointer, no fraction and no exponent. So the
        digit loops are loops, and the accumulation inside them is a serial
        multiply-and-add with a carry that no routine in library.c can hold
        the running value for.
*/
#if decimal_bits == 64

#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")

/*
        A float seen as its bits.

        math.c has unions of the same shape and this does not reach for them:
        they are static to that file, this file may be included without it,
        and six lines duplicated is cheaper than an ordering dependency
        between two families that land in one commit.
*/
typedef union
{
        f32 value;
        p32 bits;
} numbers_narrow_shape;

typedef union
{
        decimal value;
        p64 bits;
} numbers_shape;

typedef union
{
        f128 value;
        p128 bits;
} numbers_extended_shape;

/*
        The four formats, as the four numbers that tell them apart.

        significand_place is where the leading bit of the significand sits,
        which for a format with an implied bit is one past the stored
        fraction and for x87 -- the one format that stores its leading bit --
        is the top of the stored significand. exponent_place is where the
        exponent field begins, and it is the same number except on x87, where
        the explicit bit occupies the position the exponent would otherwise
        start at. Everything else in the slow tier is written in terms of
        those two, so x87 costs one extra field rather than a second body.

        bias is negative, in the sense the exponent field is a biased number:
        the field holds (real exponent - bias), so a bias of -1023 makes the
        field 1023 for an exponent of zero, and a value of exactly `bias`
        means the field is zero, which is what a zero and a subnormal want.

        point_high and point_low are the coarse early outs: a value whose
        decimal point sits above the first or below the second cannot be
        anything but an infinity or a zero, and saying so before the register
        arithmetic starts is what keeps 1e1000000 from being a long loop.
*/
typedef struct
{
        b32 significand_place;
        b32 exponent_place;
        b32 exponent_bits;
        b32 bias;
        b32 point_high;
        b32 point_low;

        //      The window of powers of ten inside which the estimating tier
        //      has to apply round-half-to-even by hand, because the table
        //      entry is exact there and a tie is a real one. Outside it a tie
        //      cannot arise, and the two long double shapes never reach that
        //      tier at all, so they carry a window of nothing.
        b32 round_even_low;
        b32 round_even_high;
} numbers_format;

static const numbers_format numbers_binary32 = {23, 23, 8, -127, 40, -51, -17, 10};
static const numbers_format numbers_binary64 = {52, 52, 11, -1023, 310, -330, -4, 23};

#if __LDBL_MANT_DIG__ == 64
static const numbers_format numbers_extended = {63, 64, 15, -16383, 4934, -4957, 0, 0};
#else
static const numbers_format numbers_extended = {112, 112, 15, -16383, 4934, -4972, 0, 0};
#endif

/*
        The decimal register the slow tier works in.

        Eight hundred and thirty two digits, held as values rather than as
        characters so that nothing subtracts a '0' in the inner loops. The
        number it holds is

              0 . d[0] d[1] ... d[count-1]   times ten to the point

        and `truncated` says that at least one nonzero digit was dropped off
        the end, so the true value is strictly greater than what is held.

        Eight hundred is the number Go arrived at and the reason is worth
        writing down, because the buffer size is a correctness argument and
        not a tuning knob. Dropping digits can only ever change the answer
        when the retained prefix lands exactly on the midpoint between two
        representable numbers, since anywhere else the prefix already decides
        the rounding and a strictly greater value decides it the same way.
        The longest exact midpoint in binary64 is the one nearest the bottom
        of the subnormals and it has 767 significant digits, so a register
        that holds more than that is exact for every input in both binary64
        and binary32 -- and `truncated` makes even a dropped tail correct,
        because a tie plus something is not a tie and rounds up.

        For long double the same argument gives a bigger number: a midpoint
        near the bottom of the binary128 subnormals needs about 11,500
        digits. This does not carry 11,500 digits. So strtold is correctly
        rounded for every input whose significant digits fit here, which is
        every input a program writes, and can round a longer input up where
        round-half-even would have gone down -- one unit in the last place,
        only for a decimal that is exactly a midpoint, only when it is spelled
        with more than 832 significant digits. That is measured in the lane
        and reported rather than claimed away.
*/
#define NUMBERS_DIGIT_MAX 832

//      Sixty is the largest shift the long arithmetic below can take in one
//      pass: the running carry reaches ten times two to the shift, which must
//      stay inside a p64, and ten times two to the sixtieth does.
#define NUMBERS_SHIFT_MAX 60

#define NUMBERS_NONE 0
#define NUMBERS_NUMBER 1
#define NUMBERS_INFINITE 2
#define NUMBERS_NOT_A_NUMBER 3

#define NUMBERS_FINE 0
#define NUMBERS_OVERFLOW 1
#define NUMBERS_UNDERFLOW 2

typedef struct
{
        p8 digits[NUMBERS_DIGIT_MAX];
        b32 count;
        b32 point;
        bool truncated;
        bool negative;

        //      What the text turned out to be, and where it stopped.
        b32 kind;
        string_address stopped;

        //      The first nineteen digits as one integer, which is what the
        //      two fast tiers work from, how many of them there were, and how
        //      many significant digits the text had in total -- the fast
        //      tiers may only run when those last two agree, because a packed
        //      value that is a prefix of the digits is not the number.
        p64 packed;
        b32 packed_count;
        b32 significant;

        //      A hexadecimal float is already binary and skips the decimal
        //      register entirely: the significand goes here with a sticky bit
        //      under it and the exponent counts twos.
        p128 hex_significand;
        b32 hex_exponent;
        bool hex_sticky;
        bool hexadecimal;
} numbers_scan;

/*
        Trimming, which every operation below ends with.

        A register with trailing zeros in it is the same number as one
        without, and the difference matters in exactly one place: the tie
        test asks whether the digit it is looking at is the last one, and a
        trailing zero left behind would make an exact tie look like something
        above a tie. So the zeros come off, and a register that is entirely
        zeros has no point either.
*/
static fn numbers_trim(numbers_scan address_to number)
{
        while (number->count > 0 && number->digits[number->count - 1] == 0)
                number->count--;

        if (number->count == 0)
                number->point = 0;
}

/*
        Halving the register a given number of times, in decimal.

        The digits are walked most significant first with a carry that is the
        remainder of everything above, in a base ten long division by two to
        the shift. The first loop is the one that finds the leading digit of
        the answer: it feeds digits in until the carry is large enough to
        produce a nonzero quotient, and the number of digits it had to eat is
        how far the decimal point moves left. The second loop is the division
        proper, one digit in and one digit out, writing behind the read head
        so nothing needs a second buffer. The third drains the carry, which is
        where a division by two lengthens the number -- and where the register
        can run out and set the truncated flag.

        The carry stays inside a p64 because the loops only ever continue
        while it is below two to the shift, and ten times two to the sixtieth
        is still a p64. That is the whole reason NUMBERS_SHIFT_MAX is sixty.
*/
static fn numbers_shift_right(numbers_scan address_to number, b32 places)
{
        b32 read = 0;
        b32 write = 0;
        positive carry = 0;
        positive mask = ((positive)1 << places) - 1;

        while ((carry >> places) == 0)
        {
                if (read >= number->count)
                {
                        if (carry == 0)
                        {
                                number->count = 0;
                                number->point = 0;
                                return;
                        }

                        while ((carry >> places) == 0)
                        {
                                carry = carry * 10;
                                read++;
                        }
                        break;
                }

                carry = carry * 10 + number->digits[read];
                read++;
        }

        number->point -= read - 1;

        while (read < number->count)
        {
                positive digit = carry >> places;

                carry &= mask;
                number->digits[write] = (p8)digit;
                write++;
                carry = carry * 10 + number->digits[read];
                read++;
        }

        while (carry > 0)
        {
                positive digit = carry >> places;

                carry &= mask;

                if (write < NUMBERS_DIGIT_MAX)
                {
                        number->digits[write] = (p8)digit;
                        write++;
                }
                else if (digit > 0)
                        number->truncated = true;

                carry = carry * 10;
        }

        number->count = write;
        numbers_trim(number);
}

/*
        Doubling the register a given number of times, in decimal.

        Multiplying by a power of two lengthens the number, and the loop that
        does the multiplication has to know by how much before it starts,
        because it writes from the right and needs to know where the right
        end will be. The count of extra digits is the number of digits in two
        to the shift -- except when the leading digits of the register are
        small enough that the product does not reach the next power of ten,
        and "small enough" is decided by comparing them against the decimal
        spelling of five to the shift. That is the classical cheat and it is
        exact: the register holds 0.d and the product 0.d times two to the k
        gains a digit precisely when 0.d is at least five to the k over ten
        to the delta.

        The table below is that pair, delta and the digits of five to the k,
        for every shift up to sixty. It is generated arithmetic rather than
        measured constants -- delta is the digit count of two to the k, and
        the string is five to the k written out -- and the test lane checks
        every entry of it against the register's own long multiplication.
*/
typedef struct
{
        b32 delta;
        const char address_to cutoff;
} numbers_cheat;

static const numbers_cheat numbers_cheats[NUMBERS_SHIFT_MAX + 1] = {
        { 0, ""},
        { 1, "5"},
        { 1, "25"},
        { 1, "125"},
        { 2, "625"},
        { 2, "3125"},
        { 2, "15625"},
        { 3, "78125"},
        { 3, "390625"},
        { 3, "1953125"},
        { 4, "9765625"},
        { 4, "48828125"},
        { 4, "244140625"},
        { 4, "1220703125"},
        { 5, "6103515625"},
        { 5, "30517578125"},
        { 5, "152587890625"},
        { 6, "762939453125"},
        { 6, "3814697265625"},
        { 6, "19073486328125"},
        { 7, "95367431640625"},
        { 7, "476837158203125"},
        { 7, "2384185791015625"},
        { 7, "11920928955078125"},
        { 8, "59604644775390625"},
        { 8, "298023223876953125"},
        { 8, "1490116119384765625"},
        { 9, "7450580596923828125"},
        { 9, "37252902984619140625"},
        { 9, "186264514923095703125"},
        {10, "931322574615478515625"},
        {10, "4656612873077392578125"},
        {10, "23283064365386962890625"},
        {10, "116415321826934814453125"},
        {11, "582076609134674072265625"},
        {11, "2910383045673370361328125"},
        {11, "14551915228366851806640625"},
        {12, "72759576141834259033203125"},
        {12, "363797880709171295166015625"},
        {12, "1818989403545856475830078125"},
        {13, "9094947017729282379150390625"},
        {13, "45474735088646411895751953125"},
        {13, "227373675443232059478759765625"},
        {13, "1136868377216160297393798828125"},
        {14, "5684341886080801486968994140625"},
        {14, "28421709430404007434844970703125"},
        {14, "142108547152020037174224853515625"},
        {15, "710542735760100185871124267578125"},
        {15, "3552713678800500929355621337890625"},
        {15, "17763568394002504646778106689453125"},
        {16, "88817841970012523233890533447265625"},
        {16, "444089209850062616169452667236328125"},
        {16, "2220446049250313080847263336181640625"},
        {16, "11102230246251565404236316680908203125"},
        {17, "55511151231257827021181583404541015625"},
        {17, "277555756156289135105907917022705078125"},
        {17, "1387778780781445675529539585113525390625"},
        {18, "6938893903907228377647697925567626953125"},
        {18, "34694469519536141888238489627838134765625"},
        {18, "173472347597680709441192448139190673828125"},
        {19, "867361737988403547205962240695953369140625"},
};

//      Whether the register's digits, read as the fraction they are, come to
//      less than the cutoff. A register that runs out first is less, because
//      the cutoff has a nonzero digit where the register has nothing.
static bool numbers_below_cutoff(numbers_scan address_to number, const char address_to cutoff)
{
        b32 index;

        for (index = 0; cutoff[index] != 0; index++)
        {
                if (index >= number->count)
                        return true;

                if (number->digits[index] != (p8)(cutoff[index] - '0'))
                        return number->digits[index] < (p8)(cutoff[index] - '0');
        }

        return false;
}

static fn numbers_shift_left(numbers_scan address_to number, b32 places)
{
        b32 delta = numbers_cheats[places].delta;
        b32 read;
        b32 write;
        positive carry = 0;

        if (numbers_below_cutoff(number, numbers_cheats[places].cutoff))
                delta--;

        write = number->count + delta;

        for (read = number->count; read > 0;)
        {
                positive quotient;
                positive remainder;

                read--;
                carry += (positive)number->digits[read] << places;
                quotient = carry / 10;
                remainder = carry - quotient * 10;
                write--;

                if (write >= 0 && write < NUMBERS_DIGIT_MAX)
                        number->digits[write] = (p8)remainder;
                else if (remainder != 0)
                        number->truncated = true;

                carry = quotient;
        }

        while (carry > 0)
        {
                positive quotient = carry / 10;
                positive remainder = carry - quotient * 10;

                write--;

                if (write >= 0 && write < NUMBERS_DIGIT_MAX)
                        number->digits[write] = (p8)remainder;
                else if (remainder != 0)
                        number->truncated = true;

                carry = quotient;
        }

        number->count += delta;

        if (number->count > NUMBERS_DIGIT_MAX)
                number->count = NUMBERS_DIGIT_MAX;

        number->point += delta;
        numbers_trim(number);
}

//      Either direction, in passes of at most sixty, because the shifts this
//      wants are as large as a hundred and fourteen -- one more than the
//      binary128 significand -- and no single pass may exceed the width the
//      carry has room for.
static fn numbers_shift(numbers_scan address_to number, b32 places)
{
        if (number->count == 0)
                return;

        if (places > 0)
        {
                while (places > NUMBERS_SHIFT_MAX)
                {
                        numbers_shift_left(number, NUMBERS_SHIFT_MAX);

                        if (number->count == 0)
                                return;

                        places -= NUMBERS_SHIFT_MAX;
                }

                if (places > 0)
                        numbers_shift_left(number, places);

                return;
        }

        while (places < -NUMBERS_SHIFT_MAX)
        {
                numbers_shift_right(number, NUMBERS_SHIFT_MAX);

                if (number->count == 0)
                        return;

                places += NUMBERS_SHIFT_MAX;
        }

        if (places < 0)
                numbers_shift_right(number, -places);
}

/*
        Whether the digit at a given place rounds the number above it up.

        Three cases and only three. Past the end of the register there is
        nothing to round with, so no. A five that is the very last digit is an
        exact tie, and a tie goes to the even neighbour -- unless the register
        was truncated, in which case the true value is strictly above the
        midpoint and rounds up, which is the whole reason the truncated flag
        is carried. Anything else is decided by the digit alone.
*/
static bool numbers_should_round_up(numbers_scan address_to number, b32 place)
{
        if (place < 0 || place >= number->count)
                return false;

        if (number->digits[place] == 5 && place + 1 == number->count)
        {
                if (number->truncated)
                        return true;

                return place > 0 && (number->digits[place - 1] & 1) != 0;
        }

        return number->digits[place] >= 5;
}

/*
        The register's integer part, rounded.

        The caller has already shifted so that the integer part is the
        significand it wants, which is at most a hundred and thirteen bits and
        so at most thirty five decimal digits. A p128 holds thirty eight, and
        the guard above the loop is there so that a register that somehow
        arrived with a larger point does not wrap silently.
*/
static p128 numbers_rounded_integer(numbers_scan address_to number)
{
        b32 index;
        p128 value = 0;

        if (number->point > 38)
                return ~(p128)0;

        for (index = 0; index < number->point && index < number->count; index++)
                value = value * 10 + number->digits[index];

        for (; index < number->point; index++)
                value = value * 10;

        if (numbers_should_round_up(number, number->point))
                value++;

        return value;
}

/*
        The steps the scaling loop takes, which are how far a decimal point of
        a given size lets the register be shifted in one pass without the
        answer leaving the range the loop is walking it into. Two to the
        twenty seventh is under ten to the ninth, so twenty seven is the step
        once the point is past the end of the table.
*/
static const b32 numbers_step[9] = {1, 3, 6, 9, 13, 16, 19, 23, 26};

static b32 numbers_step_for(b32 point)
{
        if (point >= 9)
                return 27;

        return numbers_step[point];
}

/*
        The slow tier: a decimal register into the bits of a float.

        Scale by powers of two -- exactly, in decimal -- until the value sits
        in [1/2, 1). That is the loop pair: while the point is above zero the
        number is one or more and wants halving, and while the point is at or
        below zero with a leading digit under five the number is under a half
        and wants doubling. Each pass moves by as much as the point allows,
        which is what keeps a value like 1e300 from being three hundred
        separate multiplications.

        Then the exponent is one less than the count of halvings, because a
        significand lives in [1, 2) and the register was walked into [1/2, 1).
        If that exponent is below the smallest the format has, the register is
        halved the rest of the way by hand: that is gradual underflow, and it
        is why the subnormals come out right rather than as a special case.

        One more bit than the stored fraction is then shifted into the integer
        part and read off with rounding, and the only thing that can go wrong
        after that is the rounding carrying into a new bit, which is one
        shift and one more overflow test.

        The last test is the one that separates a normal from a subnormal:
        the leading bit is there or it is not, and if it is not the exponent
        field is zero. For x87, where the leading bit is stored rather than
        implied, that same test reads the stored bit and the same assignment
        is correct, which is the entire reason the format is four numbers.

        WHEN A SMALL ANSWER IS AN UNDERFLOW AND WHEN IT IS ONLY SMALL

        A subnormal result is not by itself an error. IEEE raises underflow
        only when the answer is both tiny AND inexact, and glibc's strtod
        follows that exactly: "0x435p-1073" is a subnormal double and is the
        value written down with nothing lost, so errno stays zero, while
        "5e-324" is a subnormal that had to round and sets ERANGE. That was
        measured rather than assumed -- a first version of this set ERANGE on
        every subnormal and disagreed with glibc on 1,645 of two million
        sweep inputs, every one of them an exactly representable small value,
        and nowhere else.

        Inexactness in the register is one test: once the significand has
        been shifted into the integer part, the conversion lost something if
        and only if a digit is left below the point, or the register was
        truncated on the way in.

        Tininess is decided AFTER rounding, which is one of the two IEEE
        allows and the one x86_64, arm64 and riscv64 all take. The definition
        is precise and it is not the same as "the answer came out subnormal":
        a result is tiny when the value, rounded to the format's FULL
        precision with the exponent range pretended to be unbounded, is still
        below the smallest normal. So a value a hair under the smallest
        normal that would round up to it at full precision was never tiny,
        even though it is below the floor, and no ERANGE is due.

        Four adjacent inputs measured against glibc show every branch of that:

              2.2250738585072011e-308   largest subnormal   ERANGE
              2.2250738585072012e-308   smallest normal     ERANGE
              2.2250738585072013e-308   smallest normal     no ERANGE
              2.2250738585072014e-308   smallest normal     no ERANGE

        The middle two produce the same double and disagree about errno, and
        nothing but a full-precision trial rounding tells them apart: at
        binary64's own spacing the first of them lands one step below the
        smallest normal and the second lands on it.

        That trial can only ever change the answer when the exponent is
        exactly one below the format's floor. Two or more below, the value is
        under half the smallest normal and no rounding at any precision can
        reach it. So the copy of the register the trial needs is taken on one
        narrow band of inputs and nowhere else.

        AND THE ONE PLACE THIS DELIBERATELY DOES NOT MATCH GLIBC

        IEEE 754 permits both answers, and the three machines this builds for
        do not agree about which they give. glibc's strtod follows the local
        hardware: x86_64 and riscv64 detect tininess after rounding, and
        arm64 detects it before. So the four inputs above produce two
        different errno columns on arm64 from the ones they produce on the
        other two, out of the same C library.

        This picks after-rounding on all three, because one answer everywhere
        is what the rest of this project means by parity and because the
        difference is an errno bit rather than a value. It was measured
        rather than waved at: against arm64's own glibc, over two million
        sweep inputs, the disagreement appeared once, and that once was a
        boundary value put in the list on purpose. Defining
        NUMBERS_TININESS_AFTER_ROUNDING to zero takes the other answer, which
        is what a program that wants arm64's glibc bit for bit should do.
*/
#ifndef NUMBERS_TININESS_AFTER_ROUNDING
#define NUMBERS_TININESS_AFTER_ROUNDING 1
#endif
static p128 numbers_assemble(numbers_scan address_to number,
                             const numbers_format address_to shape,
                             b32 address_to condition)
{
        b32 exponent = 0;
        p128 significand = 0;
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 outcome = NUMBERS_FINE;
        bool exact = true;
        bool tiny = false;
        p128 bits;

        if (number->count == 0)
        {
                exponent = shape->bias;
                goto ready;
        }

        if (number->point > shape->point_high)
                goto over;

        if (number->point < shape->point_low)
        {
                exponent = shape->bias;
                outcome = NUMBERS_UNDERFLOW;
                goto ready;
        }

        while (number->point > 0)
        {
                b32 step = numbers_step_for(number->point);

                numbers_shift(number, -step);
                exponent += step;

                if (number->count == 0)
                {
                        exponent = shape->bias;
                        outcome = NUMBERS_UNDERFLOW;
                        goto ready;
                }
        }

        while (number->point < 0 || (number->point == 0 && number->digits[0] < 5))
        {
                b32 step = numbers_step_for(-number->point);

                numbers_shift(number, step);
                exponent -= step;
        }

        exponent--;

        if (exponent < shape->bias + 1)
        {
                b32 back = shape->bias + 1 - exponent;

                tiny = true;

                if (NUMBERS_TININESS_AFTER_ROUNDING && back == 1)
                {
                        numbers_scan trial;

                        memory_copy(address_of trial, number, sizeof trial);
                        numbers_shift(address_of trial, 1 + shape->significand_place);

                        if (numbers_rounded_integer(address_of trial) ==
                            ((p128)2 << shape->significand_place))
                                tiny = false;
                }

                numbers_shift(number, -back);
                exponent += back;
        }

        if (exponent - shape->bias >= limit)
                goto over;

        numbers_shift(number, 1 + shape->significand_place);
        exact = !number->truncated && number->count <= number->point;
        significand = numbers_rounded_integer(number);

        if (significand == ((p128)2 << shape->significand_place))
        {
                significand >>= 1;
                exponent++;

                if (exponent - shape->bias >= limit)
                        goto over;
        }

        if ((significand & ((p128)1 << shape->significand_place)) == 0)
                exponent = shape->bias;

        if (tiny && !exact)
                outcome = NUMBERS_UNDERFLOW;

        goto ready;

over:
        significand = 0;
        exponent = limit + shape->bias;
        outcome = NUMBERS_OVERFLOW;

        //      x87 stores the leading bit of its significand, and an infinity
        //      with that bit clear is a shape the hardware calls invalid.
        if (shape->exponent_place > shape->significand_place)
                significand = (p128)1 << shape->significand_place;

ready:
        bits = significand & ((((p128)1 << shape->exponent_place) - 1));
        bits |= (p128)(p64)((exponent - shape->bias) & limit) << shape->exponent_place;

        if (number->negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        if (condition)
                address_to condition = outcome;

        return bits;
}

/*
        The other rounding, which is binary all the way down.

        A hexadecimal float never becomes a decimal register: the text is
        already a significand and a power of two, so the only work is to move
        the leading bit where the format wants it and round off whatever falls
        below. Sticky is the flag that says something nonzero fell off the
        bottom of the significand while it was being read, which is what turns
        a would-be tie into a value above the midpoint.

        The subnormal case is the same shift with a smaller target: instead of
        putting the leading bit at the top of the significand, put it as far
        down as the smallest exponent forces, and let the rounding happen
        there. A carry out of that lands exactly on the smallest normal, which
        is the right answer and needs no case of its own -- so the carry
        correction below only fires when the significand really has grown past
        the top of the format.
*/
static b32 numbers_top_bit(p128 value)
{
        p64 high = (p64)(value >> 64);

        if (high != 0)
                return 127 - (b32)bits_leading_zeros(high);

        return 63 - (b32)bits_leading_zeros((p64)value);
}

static p128 numbers_round_binary(numbers_scan address_to number,
                                 const numbers_format address_to shape,
                                 b32 address_to condition)
{
        p128 significand = number->hex_significand;
        bool sticky = number->hex_sticky;
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 outcome = NUMBERS_FINE;
        bool exact = true;
        bool tiny = false;
        b32 top;
        b32 leading;
        b32 wanted;
        b32 move;
        b32 scale;
        b32 field;
        p128 bits;

        if (significand == 0)
        {
                scale = 0;
                field = 0;
                goto ready;
        }

        exact = !sticky;

        top = numbers_top_bit(significand);
        leading = number->hex_exponent + top;
        wanted = shape->significand_place;

        if (leading < shape->bias + 1)
        {
                tiny = true;
                wanted = shape->significand_place - (shape->bias + 1 - leading);
        }

        move = wanted - top;
        scale = leading - wanted;

        if (move >= 0)
        {
                significand <<= move;
        }
        else if (-move >= 128)
        {
                //      Every bit of the significand is below the last place
                //      the format has, so the answer is a zero that knows it
                //      was not one.
                significand = 0;
                outcome = NUMBERS_UNDERFLOW;
                scale = 0;
                field = 0;
                goto ready;

        }
        else
        {
                b32 back = -move;
                p128 half = (p128)1 << (back - 1);
                p128 dropped = significand & (((p128)1 << back) - 1);

                significand >>= back;

                if (dropped != 0)
                        exact = false;

                if (dropped > half)
                        significand++;
                else if (dropped == half && (sticky || (significand & 1) != 0))
                        significand++;
        }

        if (significand >= ((p128)2 << shape->significand_place))
        {
                significand >>= 1;
                scale++;
        }

        if ((significand & ((p128)1 << shape->significand_place)) == 0)
        {
                field = 0;
        }
        else
        {
                field = scale + shape->significand_place - shape->bias;

                if (field >= limit)
                {
                        significand = 0;
                        field = limit;

                        if (shape->exponent_place > shape->significand_place)
                                significand = (p128)1 << shape->significand_place;

                        outcome = NUMBERS_OVERFLOW;
                        tiny = false;
                }
        }

        //      Tiny and inexact is an underflow; tiny and exact is only a
        //      subnormal, and a subnormal is a number like any other.
        if (tiny && !exact)
                outcome = NUMBERS_UNDERFLOW;

ready:
        bits = significand & ((((p128)1 << shape->exponent_place) - 1));
        bits |= (p128)(p64)field << shape->exponent_place;

        if (number->negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        if (condition)
                address_to condition = outcome;

        return bits;
}

/*
        Reading the text, which is the half of strtod the standard spends its
        words on.

        Leading whitespace, an optional sign, and then one of four things: a
        name, a hexadecimal float, a decimal float, or nothing at all. The
        three that convert leave the end pointer past what they took; the
        fourth leaves it at the ORIGINAL string, sign and whitespace
        included, which is the corner that separates a library that read the
        standard from one that did not.

        Nothing here reads past the terminator. Every load is either the
        first byte of what is left or a byte whose predecessor has already
        been found not to be the terminator, and the two fixed comparisons
        against "infinity" and "nan" are preceded by a bounded length so that
        a three byte string at the end of a page is never compared eight
        bytes wide. string_length_max with a literal bound folds to
        straight-line code through the umbrella's specializer, so the safety
        costs no call.

        The clamps on the three counters are not decoration. A string may
        carry a billion leading zeros after the point, or an exponent of ten
        to the ninth, and the register's decimal point is a b32. Every clamp
        below saturates far outside the range any format can represent, so
        the coarse early outs in the assembly step answer correctly for
        anything that reaches them.
*/
static p8 numbers_infinity_text[] = "infinity";
static p8 numbers_short_infinity_text[] = "inf";
static p8 numbers_not_a_number_text[] = "nan";

#define NUMBERS_POINT_CLAMP 1000000000
#define NUMBERS_EXPONENT_CLAMP 100000000

static b32 numbers_hex_value(b32 byte)
{
        if (byte_is_digit(byte))
                return byte - '0';

        return byte_to_lower(byte) - 'a' + 10;
}

static fn numbers_read_hexadecimal(numbers_scan address_to number, string_address start)
{
        string_address scan = start + 2;
        p128 significand = 0;
        b32 exponent = 0;
        bool sticky = false;
        bool seen = false;
        bool dotted = false;

        while (true)
        {
                b32 byte = address_to scan;
                b32 value;

                if (byte == '.')
                {
                        if (dotted)
                                break;

                        dotted = true;
                        scan++;
                        continue;
                }

                if (!byte_is_hexadecimal(byte))
                        break;

                value = numbers_hex_value(byte);
                seen = true;

                if (significand < ((p128)1 << 124))
                {
                        significand = (significand << 4) | (p128)(p64)value;

                        if (dotted && exponent > -NUMBERS_POINT_CLAMP)
                                exponent -= 4;
                }
                else
                {
                        if (value != 0)
                                sticky = true;

                        if (!dotted && exponent < NUMBERS_POINT_CLAMP)
                                exponent += 4;
                }

                scan++;
        }

        //      "0x" with nothing usable behind it is not a failed conversion.
        //      The subject sequence is the "0", the answer is zero, and the
        //      end pointer lands on the "x" -- which is what strtol does with
        //      the same text and for the same reason.
        if (!seen)
        {
                number->kind = NUMBERS_NUMBER;
                number->count = 0;
                number->stopped = start + 1;
                return;
        }

        if (byte_to_lower(address_to scan) == 'p')
        {
                string_address digits = scan + 1;
                b32 sign = 1;
                b32 value = 0;

                if (address_to digits == '+')
                        digits++;
                else if (address_to digits == '-')
                {
                        sign = -1;
                        digits++;
                }

                if (byte_is_digit(address_to digits))
                {
                        while (byte_is_digit(address_to digits))
                        {
                                if (value < NUMBERS_EXPONENT_CLAMP)
                                        value = value * 10 + (address_to digits - '0');

                                digits++;
                        }

                        exponent += sign * value;
                        scan = digits;
                }
        }

        number->kind = NUMBERS_NUMBER;
        number->hexadecimal = true;
        number->hex_significand = significand;
        number->hex_exponent = exponent;
        number->hex_sticky = sticky;
        number->stopped = scan;
}

static bool numbers_read(string_address input, numbers_scan address_to number)
{
        string_address scan = input;
        positive available;
        bool seen_digit = false;
        bool dotted = false;
        b32 whole_digits = 0;
        b32 hidden_zeros = 0;
        bool leading = true;

        number->count = 0;
        number->point = 0;
        number->truncated = false;
        number->negative = false;
        number->kind = NUMBERS_NONE;
        number->stopped = input;
        number->packed = 0;
        number->packed_count = 0;
        number->significant = 0;
        number->hex_significand = 0;
        number->hex_exponent = 0;
        number->hex_sticky = false;
        number->hexadecimal = false;

        while (byte_is_space(address_to scan))
                scan++;

        if (address_to scan == '+')
                scan++;
        else if (address_to scan == '-')
        {
                number->negative = true;
                scan++;
        }

        available = string_length_max(scan, 8);

        if (available >= 8 &&
            string_compare_folded_max(scan, numbers_infinity_text, 8) == 0)
        {
                number->kind = NUMBERS_INFINITE;
                number->stopped = scan + 8;
                return true;
        }

        if (available >= 3 &&
            string_compare_folded_max(scan, numbers_short_infinity_text, 3) == 0)
        {
                number->kind = NUMBERS_INFINITE;
                number->stopped = scan + 3;
                return true;
        }

        if (available >= 3 &&
            string_compare_folded_max(scan, numbers_not_a_number_text, 3) == 0)
        {
                string_address after = scan + 3;

                //      The optional n-char-sequence. C spells it as letters,
                //      digits and underscores between brackets, and it is
                //      part of the subject sequence whether or not anything
                //      can be made of it -- so "nan(zz)" converts and stops
                //      after the bracket, which is where glibc stops too.
                //      What it MEANS is implementation defined, and glibc
                //      reads it as a number and puts it in the significand,
                //      so this hands it to strtoull and uses the answer when
                //      the whole sequence was a number.
                if (address_to after == '(')
                {
                        string_address inside = after + 1;
                        string_address walk = inside;

                        while (byte_is_alnum(address_to walk) || address_to walk == '_')
                                walk++;

                        if (address_to walk == ')')
                        {
                                string_address closing = null;
                                positive payload = strtoull((const char address_to)inside, (char address_to address_to)address_of closing, 0);

                                if (closing == walk)
                                        number->packed = payload;

                                after = walk + 1;
                        }
                }

                number->kind = NUMBERS_NOT_A_NUMBER;
                number->stopped = after;
                return true;
        }

        if (address_to scan == '0' && byte_to_lower(scan[1]) == 'x')
        {
                numbers_read_hexadecimal(number, scan);
                return number->kind != NUMBERS_NONE;
        }

        while (true)
        {
                b32 byte = address_to scan;
                b32 value;

                if (byte == '.')
                {
                        if (dotted)
                                break;

                        dotted = true;
                        scan++;
                        continue;
                }

                if (!byte_is_digit(byte))
                        break;

                seen_digit = true;
                value = byte - '0';

                if (leading && value == 0)
                {
                        if (dotted && hidden_zeros < NUMBERS_POINT_CLAMP)
                                hidden_zeros++;

                        scan++;
                        continue;
                }

                leading = false;

                if (number->count < NUMBERS_DIGIT_MAX)
                {
                        number->digits[number->count] = (p8)value;
                        number->count++;
                }
                else if (value != 0)
                        number->truncated = true;

                if (number->packed_count < 19)
                {
                        number->packed = number->packed * 10 + (p64)(p32)value;
                        number->packed_count++;
                }

                if (number->significant < NUMBERS_POINT_CLAMP)
                        number->significant++;

                if (!dotted && whole_digits < NUMBERS_POINT_CLAMP)
                        whole_digits++;

                scan++;
        }

        if (!seen_digit)
        {
                number->kind = NUMBERS_NONE;
                number->stopped = input;
                return false;
        }

        number->point = whole_digits - hidden_zeros;

        if (byte_to_lower(address_to scan) == 'e')
        {
                string_address digits = scan + 1;
                b32 sign = 1;
                b32 value = 0;

                if (address_to digits == '+')
                        digits++;
                else if (address_to digits == '-')
                {
                        sign = -1;
                        digits++;
                }

                if (byte_is_digit(address_to digits))
                {
                        while (byte_is_digit(address_to digits))
                        {
                                if (value < NUMBERS_EXPONENT_CLAMP)
                                        value = value * 10 + (address_to digits - '0');

                                digits++;
                        }

                        number->point += sign * value;
                        scan = digits;
                }
        }

        numbers_trim(number);
        number->kind = NUMBERS_NUMBER;
        number->stopped = scan;

        return true;
}

/*
        THE ESTIMATING TIER, WHICH IS EISEL AND LEMIRE'S

        The exact tier above answers when the significand fits fifty three
        bits and the power of ten is one of the forty five that are exactly
        doubles. Outside that -- "1e-30", or twenty significant digits, or
        anything past ten to the twenty second -- there is still a way to be
        both fast and correct, and it is this one.

        The idea is to hold ten to the q as a 128 bit binary approximation
        rather than as a double, multiply the significand by it in 128 bits,
        and then ask whether the bits that were thrown away could possibly
        have reached the rounding boundary. If they could not, the answer is
        proved correctly rounded and the whole conversion was one or two
        multiplies. If they could, the tier says nothing at all and the
        decimal register runs. That refusal is the load-bearing part: an
        estimator that answered anyway would be wrong on a vanishing fraction
        of inputs, which is the worst possible frequency to be wrong at,
        because no test that samples would ever see it.

        The table is six hundred and fifty one 128 bit values, ten to the
        minus three hundred and forty second through ten to the three hundred
        and eighth, each normalised so its top bit is set. They are
        TRUNCATIONS and not roundings, and the difference matters: the proof
        that the reject test is sufficient assumes the stored value is at or
        below the true one. The generator writes each entry twice by two
        different routes and asserts they agree, and asserts the bracketing
        inequality that says the entry is the truncation, before the entry is
        allowed into the file at all.

        TWO PLACES THIS DELIBERATELY DIFFERS FROM THE PUBLISHED ALGORITHM

        The published version handles subnormal results and overflow inside
        itself. This one refuses them: if the exponent it computes is at or
        below zero, or at or above the saturated field, the tier declines and
        the decimal register runs instead. That is not a shortcut, it is the
        errno contract -- underflow has to answer whether the value was tiny
        AFTER rounding at full precision and whether it was inexact, and
        neither question is one this tier has the information to answer. The
        register does, so the register takes those inputs. They are rare and
        they are the ones where being slow costs nothing.

        And where the digits ran past nineteen, the tier is asked twice --
        once with the truncated significand and once with one more -- and only
        answers if both come out the same float. Rounding is monotonic, the
        true value lies between those two, so two equal answers is a proof.
*/
#define NUMBERS_SMALLEST_POWER (-342)
#define NUMBERS_LARGEST_POWER 308

static const p64 numbers_power_of_five[NUMBERS_LARGEST_POWER -
                                       NUMBERS_SMALLEST_POWER + 1][2] = {
        {0x113faa2906a13b40ULL, 0xeef453d6923bd65aULL},
        {0x4ac7ca59a424c508ULL, 0x9558b4661b6565f8ULL},
        {0x5d79bcf00d2df64aULL, 0xbaaee17fa23ebf76ULL},
        {0xf4d82c2c107973ddULL, 0xe95a99df8ace6f53ULL},
        {0x79071b9b8a4be86aULL, 0x91d8a02bb6c10594ULL},
        {0x9748e2826cdee285ULL, 0xb64ec836a47146f9ULL},
        {0xfd1b1b2308169b26ULL, 0xe3e27a444d8d98b7ULL},
        {0xfe30f0f5e50e20f8ULL, 0x8e6d8c6ab0787f72ULL},
        {0xbdbd2d335e51a936ULL, 0xb208ef855c969f4fULL},
        {0xad2c788035e61383ULL, 0xde8b2b66b3bc4723ULL},
        {0x4c3bcb5021afcc32ULL, 0x8b16fb203055ac76ULL},
        {0xdf4abe242a1bbf3eULL, 0xaddcb9e83c6b1793ULL},
        {0xd71d6dad34a2af0eULL, 0xd953e8624b85dd78ULL},
        {0x8672648c40e5ad69ULL, 0x87d4713d6f33aa6bULL},
        {0x680efdaf511f18c3ULL, 0xa9c98d8ccb009506ULL},
        {0x0212bd1b2566def3ULL, 0xd43bf0effdc0ba48ULL},
        {0x014bb630f7604b58ULL, 0x84a57695fe98746dULL},
        {0x419ea3bd35385e2eULL, 0xa5ced43b7e3e9188ULL},
        {0x52064cac828675baULL, 0xcf42894a5dce35eaULL},
        {0x7343efebd1940994ULL, 0x818995ce7aa0e1b2ULL},
        {0x1014ebe6c5f90bf9ULL, 0xa1ebfb4219491a1fULL},
        {0xd41a26e077774ef7ULL, 0xca66fa129f9b60a6ULL},
        {0x8920b098955522b5ULL, 0xfd00b897478238d0ULL},
        {0x55b46e5f5d5535b1ULL, 0x9e20735e8cb16382ULL},
        {0xeb2189f734aa831eULL, 0xc5a890362fddbc62ULL},
        {0xa5e9ec7501d523e5ULL, 0xf712b443bbd52b7bULL},
        {0x47b233c92125366fULL, 0x9a6bb0aa55653b2dULL},
        {0x999ec0bb696e840bULL, 0xc1069cd4eabe89f8ULL},
        {0xc00670ea43ca250eULL, 0xf148440a256e2c76ULL},
        {0x380406926a5e5729ULL, 0x96cd2a865764dbcaULL},
        {0xc605083704f5ecf3ULL, 0xbc807527ed3e12bcULL},
        {0xf7864a44c633682fULL, 0xeba09271e88d976bULL},
        {0x7ab3ee6afbe0211eULL, 0x93445b8731587ea3ULL},
        {0x5960ea05bad82965ULL, 0xb8157268fdae9e4cULL},
        {0x6fb92487298e33beULL, 0xe61acf033d1a45dfULL},
        {0xa5d3b6d479f8e057ULL, 0x8fd0c16206306babULL},
        {0x8f48a4899877186dULL, 0xb3c4f1ba87bc8696ULL},
        {0x331acdabfe94de88ULL, 0xe0b62e2929aba83cULL},
        {0x9ff0c08b7f1d0b15ULL, 0x8c71dcd9ba0b4925ULL},
        {0x07ecf0ae5ee44ddaULL, 0xaf8e5410288e1b6fULL},
        {0xc9e82cd9f69d6151ULL, 0xdb71e91432b1a24aULL},
        {0xbe311c083a225cd3ULL, 0x892731ac9faf056eULL},
        {0x6dbd630a48aaf407ULL, 0xab70fe17c79ac6caULL},
        {0x092cbbccdad5b109ULL, 0xd64d3d9db981787dULL},
        {0x25bbf56008c58ea6ULL, 0x85f0468293f0eb4eULL},
        {0xaf2af2b80af6f24fULL, 0xa76c582338ed2621ULL},
        {0x1af5af660db4aee2ULL, 0xd1476e2c07286faaULL},
        {0x50d98d9fc890ed4eULL, 0x82cca4db847945caULL},
        {0xe50ff107bab528a1ULL, 0xa37fce126597973cULL},
        {0x1e53ed49a96272c9ULL, 0xcc5fc196fefd7d0cULL},
        {0x25e8e89c13bb0f7bULL, 0xff77b1fcbebcdc4fULL},
        {0x77b191618c54e9adULL, 0x9faacf3df73609b1ULL},
        {0xd59df5b9ef6a2418ULL, 0xc795830d75038c1dULL},
        {0x4b0573286b44ad1eULL, 0xf97ae3d0d2446f25ULL},
        {0x4ee367f9430aec33ULL, 0x9becce62836ac577ULL},
        {0x229c41f793cda740ULL, 0xc2e801fb244576d5ULL},
        {0x6b43527578c11110ULL, 0xf3a20279ed56d48aULL},
        {0x830a13896b78aaaaULL, 0x9845418c345644d6ULL},
        {0x23cc986bc656d554ULL, 0xbe5691ef416bd60cULL},
        {0x2cbfbe86b7ec8aa9ULL, 0xedec366b11c6cb8fULL},
        {0x7bf7d71432f3d6aaULL, 0x94b3a202eb1c3f39ULL},
        {0xdaf5ccd93fb0cc54ULL, 0xb9e08a83a5e34f07ULL},
        {0xd1b3400f8f9cff69ULL, 0xe858ad248f5c22c9ULL},
        {0x23100809b9c21fa2ULL, 0x91376c36d99995beULL},
        {0xabd40a0c2832a78bULL, 0xb58547448ffffb2dULL},
        {0x16c90c8f323f516dULL, 0xe2e69915b3fff9f9ULL},
        {0xae3da7d97f6792e4ULL, 0x8dd01fad907ffc3bULL},
        {0x99cd11cfdf41779dULL, 0xb1442798f49ffb4aULL},
        {0x40405643d711d584ULL, 0xdd95317f31c7fa1dULL},
        {0x482835ea666b2573ULL, 0x8a7d3eef7f1cfc52ULL},
        {0xda3243650005eed0ULL, 0xad1c8eab5ee43b66ULL},
        {0x90bed43e40076a83ULL, 0xd863b256369d4a40ULL},
        {0x5a7744a6e804a292ULL, 0x873e4f75e2224e68ULL},
        {0x711515d0a205cb37ULL, 0xa90de3535aaae202ULL},
        {0x0d5a5b44ca873e04ULL, 0xd3515c2831559a83ULL},
        {0xe858790afe9486c3ULL, 0x8412d9991ed58091ULL},
        {0x626e974dbe39a873ULL, 0xa5178fff668ae0b6ULL},
        {0xfb0a3d212dc81290ULL, 0xce5d73ff402d98e3ULL},
        {0x7ce66634bc9d0b9aULL, 0x80fa687f881c7f8eULL},
        {0x1c1fffc1ebc44e81ULL, 0xa139029f6a239f72ULL},
        {0xa327ffb266b56221ULL, 0xc987434744ac874eULL},
        {0x4bf1ff9f0062baa9ULL, 0xfbe9141915d7a922ULL},
        {0x6f773fc3603db4aaULL, 0x9d71ac8fada6c9b5ULL},
        {0xcb550fb4384d21d4ULL, 0xc4ce17b399107c22ULL},
        {0x7e2a53a146606a49ULL, 0xf6019da07f549b2bULL},
        {0x2eda7444cbfc426eULL, 0x99c102844f94e0fbULL},
        {0xfa911155fefb5309ULL, 0xc0314325637a1939ULL},
        {0x793555ab7eba27cbULL, 0xf03d93eebc589f88ULL},
        {0x4bc1558b2f3458dfULL, 0x96267c7535b763b5ULL},
        {0x9eb1aaedfb016f17ULL, 0xbbb01b9283253ca2ULL},
        {0x465e15a979c1caddULL, 0xea9c227723ee8bcbULL},
        {0x0bfacd89ec191ecaULL, 0x92a1958a7675175fULL},
        {0xcef980ec671f667cULL, 0xb749faed14125d36ULL},
        {0x82b7e12780e7401bULL, 0xe51c79a85916f484ULL},
        {0xd1b2ecb8b0908811ULL, 0x8f31cc0937ae58d2ULL},
        {0x861fa7e6dcb4aa16ULL, 0xb2fe3f0b8599ef07ULL},
        {0x67a791e093e1d49bULL, 0xdfbdcece67006ac9ULL},
        {0xe0c8bb2c5c6d24e1ULL, 0x8bd6a141006042bdULL},
        {0x58fae9f773886e19ULL, 0xaecc49914078536dULL},
        {0xaf39a475506a899fULL, 0xda7f5bf590966848ULL},
        {0x6d8406c952429604ULL, 0x888f99797a5e012dULL},
        {0xc8e5087ba6d33b84ULL, 0xaab37fd7d8f58178ULL},
        {0xfb1e4a9a90880a65ULL, 0xd5605fcdcf32e1d6ULL},
        {0x5cf2eea09a550680ULL, 0x855c3be0a17fcd26ULL},
        {0xf42faa48c0ea481fULL, 0xa6b34ad8c9dfc06fULL},
        {0xf13b94daf124da27ULL, 0xd0601d8efc57b08bULL},
        {0x76c53d08d6b70859ULL, 0x823c12795db6ce57ULL},
        {0x54768c4b0c64ca6fULL, 0xa2cb1717b52481edULL},
        {0xa9942f5dcf7dfd0aULL, 0xcb7ddcdda26da268ULL},
        {0xd3f93b35435d7c4dULL, 0xfe5d54150b090b02ULL},
        {0xc47bc5014a1a6db0ULL, 0x9efa548d26e5a6e1ULL},
        {0x359ab6419ca1091cULL, 0xc6b8e9b0709f109aULL},
        {0xc30163d203c94b63ULL, 0xf867241c8cc6d4c0ULL},
        {0x79e0de63425dcf1eULL, 0x9b407691d7fc44f8ULL},
        {0x985915fc12f542e5ULL, 0xc21094364dfb5636ULL},
        {0x3e6f5b7b17b2939eULL, 0xf294b943e17a2bc4ULL},
        {0xa705992ceecf9c43ULL, 0x979cf3ca6cec5b5aULL},
        {0x50c6ff782a838354ULL, 0xbd8430bd08277231ULL},
        {0xa4f8bf5635246429ULL, 0xece53cec4a314ebdULL},
        {0x871b7795e136be9aULL, 0x940f4613ae5ed136ULL},
        {0x28e2557b59846e40ULL, 0xb913179899f68584ULL},
        {0x331aeada2fe589d0ULL, 0xe757dd7ec07426e5ULL},
        {0x3ff0d2c85def7622ULL, 0x9096ea6f3848984fULL},
        {0x0fed077a756b53aaULL, 0xb4bca50b065abe63ULL},
        {0xd3e8495912c62895ULL, 0xe1ebce4dc7f16dfbULL},
        {0x64712dd7abbbd95dULL, 0x8d3360f09cf6e4bdULL},
        {0xbd8d794d96aacfb4ULL, 0xb080392cc4349decULL},
        {0xecf0d7a0fc5583a1ULL, 0xdca04777f541c567ULL},
        {0xf41686c49db57245ULL, 0x89e42caaf9491b60ULL},
        {0x311c2875c522ced6ULL, 0xac5d37d5b79b6239ULL},
        {0x7d633293366b828cULL, 0xd77485cb25823ac7ULL},
        {0xae5dff9c02033198ULL, 0x86a8d39ef77164bcULL},
        {0xd9f57f830283fdfdULL, 0xa8530886b54dbdebULL},
        {0xd072df63c324fd7cULL, 0xd267caa862a12d66ULL},
        {0x4247cb9e59f71e6eULL, 0x8380dea93da4bc60ULL},
        {0x52d9be85f074e609ULL, 0xa46116538d0deb78ULL},
        {0x67902e276c921f8cULL, 0xcd795be870516656ULL},
        {0x00ba1cd8a3db53b7ULL, 0x806bd9714632dff6ULL},
        {0x80e8a40eccd228a5ULL, 0xa086cfcd97bf97f3ULL},
        {0x6122cd128006b2ceULL, 0xc8a883c0fdaf7df0ULL},
        {0x796b805720085f82ULL, 0xfad2a4b13d1b5d6cULL},
        {0xcbe3303674053bb1ULL, 0x9cc3a6eec6311a63ULL},
        {0xbedbfc4411068a9dULL, 0xc3f490aa77bd60fcULL},
        {0xee92fb5515482d45ULL, 0xf4f1b4d515acb93bULL},
        {0x751bdd152d4d1c4bULL, 0x991711052d8bf3c5ULL},
        {0xd262d45a78a0635eULL, 0xbf5cd54678eef0b6ULL},
        {0x86fb897116c87c35ULL, 0xef340a98172aace4ULL},
        {0xd45d35e6ae3d4da1ULL, 0x9580869f0e7aac0eULL},
        {0x8974836059cca10aULL, 0xbae0a846d2195712ULL},
        {0x2bd1a438703fc94cULL, 0xe998d258869facd7ULL},
        {0x7b6306a34627ddd0ULL, 0x91ff83775423cc06ULL},
        {0x1a3bc84c17b1d543ULL, 0xb67f6455292cbf08ULL},
        {0x20caba5f1d9e4a94ULL, 0xe41f3d6a7377eecaULL},
        {0x547eb47b7282ee9dULL, 0x8e938662882af53eULL},
        {0xe99e619a4f23aa44ULL, 0xb23867fb2a35b28dULL},
        {0x6405fa00e2ec94d5ULL, 0xdec681f9f4c31f31ULL},
        {0xde83bc408dd3dd05ULL, 0x8b3c113c38f9f37eULL},
        {0x9624ab50b148d446ULL, 0xae0b158b4738705eULL},
        {0x3badd624dd9b0958ULL, 0xd98ddaee19068c76ULL},
        {0xe54ca5d70a80e5d7ULL, 0x87f8a8d4cfa417c9ULL},
        {0x5e9fcf4ccd211f4dULL, 0xa9f6d30a038d1dbcULL},
        {0x7647c32000696720ULL, 0xd47487cc8470652bULL},
        {0x29ecd9f40041e074ULL, 0x84c8d4dfd2c63f3bULL},
        {0xf468107100525891ULL, 0xa5fb0a17c777cf09ULL},
        {0x7182148d4066eeb5ULL, 0xcf79cc9db955c2ccULL},
        {0xc6f14cd848405531ULL, 0x81ac1fe293d599bfULL},
        {0xb8ada00e5a506a7dULL, 0xa21727db38cb002fULL},
        {0xa6d90811f0e4851dULL, 0xca9cf1d206fdc03bULL},
        {0x908f4a166d1da664ULL, 0xfd442e4688bd304aULL},
        {0x9a598e4e043287ffULL, 0x9e4a9cec15763e2eULL},
        {0x40eff1e1853f29feULL, 0xc5dd44271ad3cdbaULL},
        {0xd12bee59e68ef47dULL, 0xf7549530e188c128ULL},
        {0x82bb74f8301958cfULL, 0x9a94dd3e8cf578b9ULL},
        {0xe36a52363c1faf02ULL, 0xc13a148e3032d6e7ULL},
        {0xdc44e6c3cb279ac2ULL, 0xf18899b1bc3f8ca1ULL},
        {0x29ab103a5ef8c0baULL, 0x96f5600f15a7b7e5ULL},
        {0x7415d448f6b6f0e8ULL, 0xbcb2b812db11a5deULL},
        {0x111b495b3464ad22ULL, 0xebdf661791d60f56ULL},
        {0xcab10dd900beec35ULL, 0x936b9fcebb25c995ULL},
        {0x3d5d514f40eea743ULL, 0xb84687c269ef3bfbULL},
        {0x0cb4a5a3112a5113ULL, 0xe65829b3046b0afaULL},
        {0x47f0e785eaba72acULL, 0x8ff71a0fe2c2e6dcULL},
        {0x59ed216765690f57ULL, 0xb3f4e093db73a093ULL},
        {0x306869c13ec3532dULL, 0xe0f218b8d25088b8ULL},
        {0x1e414218c73a13fcULL, 0x8c974f7383725573ULL},
        {0xe5d1929ef90898fbULL, 0xafbd2350644eeacfULL},
        {0xdf45f746b74abf3aULL, 0xdbac6c247d62a583ULL},
        {0x6b8bba8c328eb784ULL, 0x894bc396ce5da772ULL},
        {0x066ea92f3f326565ULL, 0xab9eb47c81f5114fULL},
        {0xc80a537b0efefebeULL, 0xd686619ba27255a2ULL},
        {0xbd06742ce95f5f37ULL, 0x8613fd0145877585ULL},
        {0x2c48113823b73705ULL, 0xa798fc4196e952e7ULL},
        {0xf75a15862ca504c6ULL, 0xd17f3b51fca3a7a0ULL},
        {0x9a984d73dbe722fcULL, 0x82ef85133de648c4ULL},
        {0xc13e60d0d2e0ebbbULL, 0xa3ab66580d5fdaf5ULL},
        {0x318df905079926a9ULL, 0xcc963fee10b7d1b3ULL},
        {0xfdf17746497f7053ULL, 0xffbbcfe994e5c61fULL},
        {0xfeb6ea8bedefa634ULL, 0x9fd561f1fd0f9bd3ULL},
        {0xfe64a52ee96b8fc1ULL, 0xc7caba6e7c5382c8ULL},
        {0x3dfdce7aa3c673b1ULL, 0xf9bd690a1b68637bULL},
        {0x06bea10ca65c084fULL, 0x9c1661a651213e2dULL},
        {0x486e494fcff30a63ULL, 0xc31bfa0fe5698db8ULL},
        {0x5a89dba3c3efccfbULL, 0xf3e2f893dec3f126ULL},
        {0xf89629465a75e01dULL, 0x986ddb5c6b3a76b7ULL},
        {0xf6bbb397f1135824ULL, 0xbe89523386091465ULL},
        {0x746aa07ded582e2dULL, 0xee2ba6c0678b597fULL},
        {0xa8c2a44eb4571cddULL, 0x94db483840b717efULL},
        {0x92f34d62616ce414ULL, 0xba121a4650e4ddebULL},
        {0x77b020baf9c81d18ULL, 0xe896a0d7e51e1566ULL},
        {0x0ace1474dc1d122fULL, 0x915e2486ef32cd60ULL},
        {0x0d819992132456bbULL, 0xb5b5ada8aaff80b8ULL},
        {0x10e1fff697ed6c6aULL, 0xe3231912d5bf60e6ULL},
        {0xca8d3ffa1ef463c2ULL, 0x8df5efabc5979c8fULL},
        {0xbd308ff8a6b17cb3ULL, 0xb1736b96b6fd83b3ULL},
        {0xac7cb3f6d05ddbdfULL, 0xddd0467c64bce4a0ULL},
        {0x6bcdf07a423aa96cULL, 0x8aa22c0dbef60ee4ULL},
        {0x86c16c98d2c953c7ULL, 0xad4ab7112eb3929dULL},
        {0xe871c7bf077ba8b8ULL, 0xd89d64d57a607744ULL},
        {0x11471cd764ad4973ULL, 0x87625f056c7c4a8bULL},
        {0xd598e40d3dd89bd0ULL, 0xa93af6c6c79b5d2dULL},
        {0x4aff1d108d4ec2c4ULL, 0xd389b47879823479ULL},
        {0xcedf722a585139bbULL, 0x843610cb4bf160cbULL},
        {0xc2974eb4ee658829ULL, 0xa54394fe1eedb8feULL},
        {0x733d226229feea33ULL, 0xce947a3da6a9273eULL},
        {0x0806357d5a3f5260ULL, 0x811ccc668829b887ULL},
        {0xca07c2dcb0cf26f8ULL, 0xa163ff802a3426a8ULL},
        {0xfc89b393dd02f0b6ULL, 0xc9bcff6034c13052ULL},
        {0xbbac2078d443ace3ULL, 0xfc2c3f3841f17c67ULL},
        {0xd54b944b84aa4c0eULL, 0x9d9ba7832936edc0ULL},
        {0x0a9e795e65d4df12ULL, 0xc5029163f384a931ULL},
        {0x4d4617b5ff4a16d6ULL, 0xf64335bcf065d37dULL},
        {0x504bced1bf8e4e46ULL, 0x99ea0196163fa42eULL},
        {0xe45ec2862f71e1d7ULL, 0xc06481fb9bcf8d39ULL},
        {0x5d767327bb4e5a4dULL, 0xf07da27a82c37088ULL},
        {0x3a6a07f8d510f870ULL, 0x964e858c91ba2655ULL},
        {0x890489f70a55368cULL, 0xbbe226efb628afeaULL},
        {0x2b45ac74ccea842fULL, 0xeadab0aba3b2dbe5ULL},
        {0x3b0b8bc90012929eULL, 0x92c8ae6b464fc96fULL},
        {0x09ce6ebb40173745ULL, 0xb77ada0617e3bbcbULL},
        {0xcc420a6a101d0516ULL, 0xe55990879ddcaabdULL},
        {0x9fa946824a12232eULL, 0x8f57fa54c2a9eab6ULL},
        {0x47939822dc96abfaULL, 0xb32df8e9f3546564ULL},
        {0x59787e2b93bc56f8ULL, 0xdff9772470297ebdULL},
        {0x57eb4edb3c55b65bULL, 0x8bfbea76c619ef36ULL},
        {0xede622920b6b23f2ULL, 0xaefae51477a06b03ULL},
        {0xe95fab368e45eceeULL, 0xdab99e59958885c4ULL},
        {0x11dbcb0218ebb415ULL, 0x88b402f7fd75539bULL},
        {0xd652bdc29f26a11aULL, 0xaae103b5fcd2a881ULL},
        {0x4be76d3346f04960ULL, 0xd59944a37c0752a2ULL},
        {0x6f70a4400c562ddcULL, 0x857fcae62d8493a5ULL},
        {0xcb4ccd500f6bb953ULL, 0xa6dfbd9fb8e5b88eULL},
        {0x7e2000a41346a7a8ULL, 0xd097ad07a71f26b2ULL},
        {0x8ed400668c0c28c9ULL, 0x825ecc24c873782fULL},
        {0x728900802f0f32fbULL, 0xa2f67f2dfa90563bULL},
        {0x4f2b40a03ad2ffbaULL, 0xcbb41ef979346bcaULL},
        {0xe2f610c84987bfa9ULL, 0xfea126b7d78186bcULL},
        {0x0dd9ca7d2df4d7caULL, 0x9f24b832e6b0f436ULL},
        {0x91503d1c79720dbcULL, 0xc6ede63fa05d3143ULL},
        {0x75a44c6397ce912bULL, 0xf8a95fcf88747d94ULL},
        {0xc986afbe3ee11abbULL, 0x9b69dbe1b548ce7cULL},
        {0xfbe85badce996169ULL, 0xc24452da229b021bULL},
        {0xfae27299423fb9c4ULL, 0xf2d56790ab41c2a2ULL},
        {0xdccd879fc967d41bULL, 0x97c560ba6b0919a5ULL},
        {0x5400e987bbc1c921ULL, 0xbdb6b8e905cb600fULL},
        {0x290123e9aab23b69ULL, 0xed246723473e3813ULL},
        {0xf9a0b6720aaf6522ULL, 0x9436c0760c86e30bULL},
        {0xf808e40e8d5b3e6aULL, 0xb94470938fa89bceULL},
        {0xb60b1d1230b20e05ULL, 0xe7958cb87392c2c2ULL},
        {0xb1c6f22b5e6f48c3ULL, 0x90bd77f3483bb9b9ULL},
        {0x1e38aeb6360b1af4ULL, 0xb4ecd5f01a4aa828ULL},
        {0x25c6da63c38de1b1ULL, 0xe2280b6c20dd5232ULL},
        {0x579c487e5a38ad0fULL, 0x8d590723948a535fULL},
        {0x2d835a9df0c6d852ULL, 0xb0af48ec79ace837ULL},
        {0xf8e431456cf88e66ULL, 0xdcdb1b2798182244ULL},
        {0x1b8e9ecb641b5900ULL, 0x8a08f0f8bf0f156bULL},
        {0xe272467e3d222f40ULL, 0xac8b2d36eed2dac5ULL},
        {0x5b0ed81dcc6abb10ULL, 0xd7adf884aa879177ULL},
        {0x98e947129fc2b4eaULL, 0x86ccbb52ea94baeaULL},
        {0x3f2398d747b36225ULL, 0xa87fea27a539e9a5ULL},
        {0x8eec7f0d19a03aaeULL, 0xd29fe4b18e88640eULL},
        {0x1953cf68300424adULL, 0x83a3eeeef9153e89ULL},
        {0x5fa8c3423c052dd8ULL, 0xa48ceaaab75a8e2bULL},
        {0x3792f412cb06794eULL, 0xcdb02555653131b6ULL},
        {0xe2bbd88bbee40bd1ULL, 0x808e17555f3ebf11ULL},
        {0x5b6aceaeae9d0ec5ULL, 0xa0b19d2ab70e6ed6ULL},
        {0xf245825a5a445276ULL, 0xc8de047564d20a8bULL},
        {0xeed6e2f0f0d56713ULL, 0xfb158592be068d2eULL},
        {0x55464dd69685606cULL, 0x9ced737bb6c4183dULL},
        {0xaa97e14c3c26b887ULL, 0xc428d05aa4751e4cULL},
        {0xd53dd99f4b3066a9ULL, 0xf53304714d9265dfULL},
        {0xe546a8038efe402aULL, 0x993fe2c6d07b7fabULL},
        {0xde98520472bdd034ULL, 0xbf8fdb78849a5f96ULL},
        {0x963e66858f6d4441ULL, 0xef73d256a5c0f77cULL},
        {0xdde7001379a44aa9ULL, 0x95a8637627989aadULL},
        {0x5560c018580d5d53ULL, 0xbb127c53b17ec159ULL},
        {0xaab8f01e6e10b4a7ULL, 0xe9d71b689dde71afULL},
        {0xcab3961304ca70e9ULL, 0x9226712162ab070dULL},
        {0x3d607b97c5fd0d23ULL, 0xb6b00d69bb55c8d1ULL},
        {0x8cb89a7db77c506bULL, 0xe45c10c42a2b3b05ULL},
        {0x77f3608e92adb243ULL, 0x8eb98a7a9a5b04e3ULL},
        {0x55f038b237591ed4ULL, 0xb267ed1940f1c61cULL},
        {0x6b6c46dec52f6689ULL, 0xdf01e85f912e37a3ULL},
        {0x2323ac4b3b3da016ULL, 0x8b61313bbabce2c6ULL},
        {0xabec975e0a0d081bULL, 0xae397d8aa96c1b77ULL},
        {0x96e7bd358c904a22ULL, 0xd9c7dced53c72255ULL},
        {0x7e50d64177da2e55ULL, 0x881cea14545c7575ULL},
        {0xdde50bd1d5d0b9eaULL, 0xaa242499697392d2ULL},
        {0x955e4ec64b44e865ULL, 0xd4ad2dbfc3d07787ULL},
        {0xbd5af13bef0b113fULL, 0x84ec3c97da624ab4ULL},
        {0xecb1ad8aeacdd58fULL, 0xa6274bbdd0fadd61ULL},
        {0x67de18eda5814af3ULL, 0xcfb11ead453994baULL},
        {0x80eacf948770ced8ULL, 0x81ceb32c4b43fcf4ULL},
        {0xa1258379a94d028eULL, 0xa2425ff75e14fc31ULL},
        {0x096ee45813a04331ULL, 0xcad2f7f5359a3b3eULL},
        {0x8bca9d6e188853fdULL, 0xfd87b5f28300ca0dULL},
        {0x775ea264cf55347eULL, 0x9e74d1b791e07e48ULL},
        {0x95364afe032a819eULL, 0xc612062576589ddaULL},
        {0x3a83ddbd83f52205ULL, 0xf79687aed3eec551ULL},
        {0xc4926a9672793543ULL, 0x9abe14cd44753b52ULL},
        {0x75b7053c0f178294ULL, 0xc16d9a0095928a27ULL},
        {0x5324c68b12dd6339ULL, 0xf1c90080baf72cb1ULL},
        {0xd3f6fc16ebca5e04ULL, 0x971da05074da7beeULL},
        {0x88f4bb1ca6bcf585ULL, 0xbce5086492111aeaULL},
        {0x2b31e9e3d06c32e6ULL, 0xec1e4a7db69561a5ULL},
        {0x3aff322e62439fd0ULL, 0x9392ee8e921d5d07ULL},
        {0x09befeb9fad487c3ULL, 0xb877aa3236a4b449ULL},
        {0x4c2ebe687989a9b4ULL, 0xe69594bec44de15bULL},
        {0x0f9d37014bf60a11ULL, 0x901d7cf73ab0acd9ULL},
        {0x538484c19ef38c95ULL, 0xb424dc35095cd80fULL},
        {0x2865a5f206b06fbaULL, 0xe12e13424bb40e13ULL},
        {0xf93f87b7442e45d4ULL, 0x8cbccc096f5088cbULL},
        {0xf78f69a51539d749ULL, 0xafebff0bcb24aafeULL},
        {0xb573440e5a884d1cULL, 0xdbe6fecebdedd5beULL},
        {0x31680a88f8953031ULL, 0x89705f4136b4a597ULL},
        {0xfdc20d2b36ba7c3eULL, 0xabcc77118461cefcULL},
        {0x3d32907604691b4dULL, 0xd6bf94d5e57a42bcULL},
        {0xa63f9a49c2c1b110ULL, 0x8637bd05af6c69b5ULL},
        {0x0fcf80dc33721d54ULL, 0xa7c5ac471b478423ULL},
        {0xd3c36113404ea4a9ULL, 0xd1b71758e219652bULL},
        {0x645a1cac083126eaULL, 0x83126e978d4fdf3bULL},
        {0x3d70a3d70a3d70a4ULL, 0xa3d70a3d70a3d70aULL},
        {0xcccccccccccccccdULL, 0xccccccccccccccccULL},
        {0x0000000000000000ULL, 0x8000000000000000ULL},
        {0x0000000000000000ULL, 0xa000000000000000ULL},
        {0x0000000000000000ULL, 0xc800000000000000ULL},
        {0x0000000000000000ULL, 0xfa00000000000000ULL},
        {0x0000000000000000ULL, 0x9c40000000000000ULL},
        {0x0000000000000000ULL, 0xc350000000000000ULL},
        {0x0000000000000000ULL, 0xf424000000000000ULL},
        {0x0000000000000000ULL, 0x9896800000000000ULL},
        {0x0000000000000000ULL, 0xbebc200000000000ULL},
        {0x0000000000000000ULL, 0xee6b280000000000ULL},
        {0x0000000000000000ULL, 0x9502f90000000000ULL},
        {0x0000000000000000ULL, 0xba43b74000000000ULL},
        {0x0000000000000000ULL, 0xe8d4a51000000000ULL},
        {0x0000000000000000ULL, 0x9184e72a00000000ULL},
        {0x0000000000000000ULL, 0xb5e620f480000000ULL},
        {0x0000000000000000ULL, 0xe35fa931a0000000ULL},
        {0x0000000000000000ULL, 0x8e1bc9bf04000000ULL},
        {0x0000000000000000ULL, 0xb1a2bc2ec5000000ULL},
        {0x0000000000000000ULL, 0xde0b6b3a76400000ULL},
        {0x0000000000000000ULL, 0x8ac7230489e80000ULL},
        {0x0000000000000000ULL, 0xad78ebc5ac620000ULL},
        {0x0000000000000000ULL, 0xd8d726b7177a8000ULL},
        {0x0000000000000000ULL, 0x878678326eac9000ULL},
        {0x0000000000000000ULL, 0xa968163f0a57b400ULL},
        {0x0000000000000000ULL, 0xd3c21bcecceda100ULL},
        {0x0000000000000000ULL, 0x84595161401484a0ULL},
        {0x0000000000000000ULL, 0xa56fa5b99019a5c8ULL},
        {0x0000000000000000ULL, 0xcecb8f27f4200f3aULL},
        {0x4000000000000000ULL, 0x813f3978f8940984ULL},
        {0x5000000000000000ULL, 0xa18f07d736b90be5ULL},
        {0xa400000000000000ULL, 0xc9f2c9cd04674edeULL},
        {0x4d00000000000000ULL, 0xfc6f7c4045812296ULL},
        {0xf020000000000000ULL, 0x9dc5ada82b70b59dULL},
        {0x6c28000000000000ULL, 0xc5371912364ce305ULL},
        {0xc732000000000000ULL, 0xf684df56c3e01bc6ULL},
        {0x3c7f400000000000ULL, 0x9a130b963a6c115cULL},
        {0x4b9f100000000000ULL, 0xc097ce7bc90715b3ULL},
        {0x1e86d40000000000ULL, 0xf0bdc21abb48db20ULL},
        {0x1314448000000000ULL, 0x96769950b50d88f4ULL},
        {0x17d955a000000000ULL, 0xbc143fa4e250eb31ULL},
        {0x5dcfab0800000000ULL, 0xeb194f8e1ae525fdULL},
        {0x5aa1cae500000000ULL, 0x92efd1b8d0cf37beULL},
        {0xf14a3d9e40000000ULL, 0xb7abc627050305adULL},
        {0x6d9ccd05d0000000ULL, 0xe596b7b0c643c719ULL},
        {0xe4820023a2000000ULL, 0x8f7e32ce7bea5c6fULL},
        {0xdda2802c8a800000ULL, 0xb35dbf821ae4f38bULL},
        {0xd50b2037ad200000ULL, 0xe0352f62a19e306eULL},
        {0x4526f422cc340000ULL, 0x8c213d9da502de45ULL},
        {0x9670b12b7f410000ULL, 0xaf298d050e4395d6ULL},
        {0x3c0cdd765f114000ULL, 0xdaf3f04651d47b4cULL},
        {0xa5880a69fb6ac800ULL, 0x88d8762bf324cd0fULL},
        {0x8eea0d047a457a00ULL, 0xab0e93b6efee0053ULL},
        {0x72a4904598d6d880ULL, 0xd5d238a4abe98068ULL},
        {0x47a6da2b7f864750ULL, 0x85a36366eb71f041ULL},
        {0x999090b65f67d924ULL, 0xa70c3c40a64e6c51ULL},
        {0xfff4b4e3f741cf6dULL, 0xd0cf4b50cfe20765ULL},
        {0xbff8f10e7a8921a4ULL, 0x82818f1281ed449fULL},
        {0xaff72d52192b6a0dULL, 0xa321f2d7226895c7ULL},
        {0x9bf4f8a69f764490ULL, 0xcbea6f8ceb02bb39ULL},
        {0x02f236d04753d5b4ULL, 0xfee50b7025c36a08ULL},
        {0x01d762422c946590ULL, 0x9f4f2726179a2245ULL},
        {0x424d3ad2b7b97ef5ULL, 0xc722f0ef9d80aad6ULL},
        {0xd2e0898765a7deb2ULL, 0xf8ebad2b84e0d58bULL},
        {0x63cc55f49f88eb2fULL, 0x9b934c3b330c8577ULL},
        {0x3cbf6b71c76b25fbULL, 0xc2781f49ffcfa6d5ULL},
        {0x8bef464e3945ef7aULL, 0xf316271c7fc3908aULL},
        {0x97758bf0e3cbb5acULL, 0x97edd871cfda3a56ULL},
        {0x3d52eeed1cbea317ULL, 0xbde94e8e43d0c8ecULL},
        {0x4ca7aaa863ee4bddULL, 0xed63a231d4c4fb27ULL},
        {0x8fe8caa93e74ef6aULL, 0x945e455f24fb1cf8ULL},
        {0xb3e2fd538e122b44ULL, 0xb975d6b6ee39e436ULL},
        {0x60dbbca87196b616ULL, 0xe7d34c64a9c85d44ULL},
        {0xbc8955e946fe31cdULL, 0x90e40fbeea1d3a4aULL},
        {0x6babab6398bdbe41ULL, 0xb51d13aea4a488ddULL},
        {0xc696963c7eed2dd1ULL, 0xe264589a4dcdab14ULL},
        {0xfc1e1de5cf543ca2ULL, 0x8d7eb76070a08aecULL},
        {0x3b25a55f43294bcbULL, 0xb0de65388cc8ada8ULL},
        {0x49ef0eb713f39ebeULL, 0xdd15fe86affad912ULL},
        {0x6e3569326c784337ULL, 0x8a2dbf142dfcc7abULL},
        {0x49c2c37f07965404ULL, 0xacb92ed9397bf996ULL},
        {0xdc33745ec97be906ULL, 0xd7e77a8f87daf7fbULL},
        {0x69a028bb3ded71a3ULL, 0x86f0ac99b4e8dafdULL},
        {0xc40832ea0d68ce0cULL, 0xa8acd7c0222311bcULL},
        {0xf50a3fa490c30190ULL, 0xd2d80db02aabd62bULL},
        {0x792667c6da79e0faULL, 0x83c7088e1aab65dbULL},
        {0x577001b891185938ULL, 0xa4b8cab1a1563f52ULL},
        {0xed4c0226b55e6f86ULL, 0xcde6fd5e09abcf26ULL},
        {0x544f8158315b05b4ULL, 0x80b05e5ac60b6178ULL},
        {0x696361ae3db1c721ULL, 0xa0dc75f1778e39d6ULL},
        {0x03bc3a19cd1e38e9ULL, 0xc913936dd571c84cULL},
        {0x04ab48a04065c723ULL, 0xfb5878494ace3a5fULL},
        {0x62eb0d64283f9c76ULL, 0x9d174b2dcec0e47bULL},
        {0x3ba5d0bd324f8394ULL, 0xc45d1df942711d9aULL},
        {0xca8f44ec7ee36479ULL, 0xf5746577930d6500ULL},
        {0x7e998b13cf4e1ecbULL, 0x9968bf6abbe85f20ULL},
        {0x9e3fedd8c321a67eULL, 0xbfc2ef456ae276e8ULL},
        {0xc5cfe94ef3ea101eULL, 0xefb3ab16c59b14a2ULL},
        {0xbba1f1d158724a12ULL, 0x95d04aee3b80ece5ULL},
        {0x2a8a6e45ae8edc97ULL, 0xbb445da9ca61281fULL},
        {0xf52d09d71a3293bdULL, 0xea1575143cf97226ULL},
        {0x593c2626705f9c56ULL, 0x924d692ca61be758ULL},
        {0x6f8b2fb00c77836cULL, 0xb6e0c377cfa2e12eULL},
        {0x0b6dfb9c0f956447ULL, 0xe498f455c38b997aULL},
        {0x4724bd4189bd5eacULL, 0x8edf98b59a373fecULL},
        {0x58edec91ec2cb657ULL, 0xb2977ee300c50fe7ULL},
        {0x2f2967b66737e3edULL, 0xdf3d5e9bc0f653e1ULL},
        {0xbd79e0d20082ee74ULL, 0x8b865b215899f46cULL},
        {0xecd8590680a3aa11ULL, 0xae67f1e9aec07187ULL},
        {0xe80e6f4820cc9495ULL, 0xda01ee641a708de9ULL},
        {0x3109058d147fdcddULL, 0x884134fe908658b2ULL},
        {0xbd4b46f0599fd415ULL, 0xaa51823e34a7eedeULL},
        {0x6c9e18ac7007c91aULL, 0xd4e5e2cdc1d1ea96ULL},
        {0x03e2cf6bc604ddb0ULL, 0x850fadc09923329eULL},
        {0x84db8346b786151cULL, 0xa6539930bf6bff45ULL},
        {0xe612641865679a63ULL, 0xcfe87f7cef46ff16ULL},
        {0x4fcb7e8f3f60c07eULL, 0x81f14fae158c5f6eULL},
        {0xe3be5e330f38f09dULL, 0xa26da3999aef7749ULL},
        {0x5cadf5bfd3072cc5ULL, 0xcb090c8001ab551cULL},
        {0x73d9732fc7c8f7f6ULL, 0xfdcb4fa002162a63ULL},
        {0x2867e7fddcdd9afaULL, 0x9e9f11c4014dda7eULL},
        {0xb281e1fd541501b8ULL, 0xc646d63501a1511dULL},
        {0x1f225a7ca91a4226ULL, 0xf7d88bc24209a565ULL},
        {0x3375788de9b06958ULL, 0x9ae757596946075fULL},
        {0x0052d6b1641c83aeULL, 0xc1a12d2fc3978937ULL},
        {0xc0678c5dbd23a49aULL, 0xf209787bb47d6b84ULL},
        {0xf840b7ba963646e0ULL, 0x9745eb4d50ce6332ULL},
        {0xb650e5a93bc3d898ULL, 0xbd176620a501fbffULL},
        {0xa3e51f138ab4cebeULL, 0xec5d3fa8ce427affULL},
        {0xc66f336c36b10137ULL, 0x93ba47c980e98cdfULL},
        {0xb80b0047445d4184ULL, 0xb8a8d9bbe123f017ULL},
        {0xa60dc059157491e5ULL, 0xe6d3102ad96cec1dULL},
        {0x87c89837ad68db2fULL, 0x9043ea1ac7e41392ULL},
        {0x29babe4598c311fbULL, 0xb454e4a179dd1877ULL},
        {0xf4296dd6fef3d67aULL, 0xe16a1dc9d8545e94ULL},
        {0x1899e4a65f58660cULL, 0x8ce2529e2734bb1dULL},
        {0x5ec05dcff72e7f8fULL, 0xb01ae745b101e9e4ULL},
        {0x76707543f4fa1f73ULL, 0xdc21a1171d42645dULL},
        {0x6a06494a791c53a8ULL, 0x899504ae72497ebaULL},
        {0x0487db9d17636892ULL, 0xabfa45da0edbde69ULL},
        {0x45a9d2845d3c42b6ULL, 0xd6f8d7509292d603ULL},
        {0x0b8a2392ba45a9b2ULL, 0x865b86925b9bc5c2ULL},
        {0x8e6cac7768d7141eULL, 0xa7f26836f282b732ULL},
        {0x3207d795430cd926ULL, 0xd1ef0244af2364ffULL},
        {0x7f44e6bd49e807b8ULL, 0x8335616aed761f1fULL},
        {0x5f16206c9c6209a6ULL, 0xa402b9c5a8d3a6e7ULL},
        {0x36dba887c37a8c0fULL, 0xcd036837130890a1ULL},
        {0xc2494954da2c9789ULL, 0x802221226be55a64ULL},
        {0xf2db9baa10b7bd6cULL, 0xa02aa96b06deb0fdULL},
        {0x6f92829494e5acc7ULL, 0xc83553c5c8965d3dULL},
        {0xcb772339ba1f17f9ULL, 0xfa42a8b73abbf48cULL},
        {0xff2a760414536efbULL, 0x9c69a97284b578d7ULL},
        {0xfef5138519684abaULL, 0xc38413cf25e2d70dULL},
        {0x7eb258665fc25d69ULL, 0xf46518c2ef5b8cd1ULL},
        {0xef2f773ffbd97a61ULL, 0x98bf2f79d5993802ULL},
        {0xaafb550ffacfd8faULL, 0xbeeefb584aff8603ULL},
        {0x95ba2a53f983cf38ULL, 0xeeaaba2e5dbf6784ULL},
        {0xdd945a747bf26183ULL, 0x952ab45cfa97a0b2ULL},
        {0x94f971119aeef9e4ULL, 0xba756174393d88dfULL},
        {0x7a37cd5601aab85dULL, 0xe912b9d1478ceb17ULL},
        {0xac62e055c10ab33aULL, 0x91abb422ccb812eeULL},
        {0x577b986b314d6009ULL, 0xb616a12b7fe617aaULL},
        {0xed5a7e85fda0b80bULL, 0xe39c49765fdf9d94ULL},
        {0x14588f13be847307ULL, 0x8e41ade9fbebc27dULL},
        {0x596eb2d8ae258fc8ULL, 0xb1d219647ae6b31cULL},
        {0x6fca5f8ed9aef3bbULL, 0xde469fbd99a05fe3ULL},
        {0x25de7bb9480d5854ULL, 0x8aec23d680043beeULL},
        {0xaf561aa79a10ae6aULL, 0xada72ccc20054ae9ULL},
        {0x1b2ba1518094da04ULL, 0xd910f7ff28069da4ULL},
        {0x90fb44d2f05d0842ULL, 0x87aa9aff79042286ULL},
        {0x353a1607ac744a53ULL, 0xa99541bf57452b28ULL},
        {0x42889b8997915ce8ULL, 0xd3fa922f2d1675f2ULL},
        {0x69956135febada11ULL, 0x847c9b5d7c2e09b7ULL},
        {0x43fab9837e699095ULL, 0xa59bc234db398c25ULL},
        {0x94f967e45e03f4bbULL, 0xcf02b2c21207ef2eULL},
        {0x1d1be0eebac278f5ULL, 0x8161afb94b44f57dULL},
        {0x6462d92a69731732ULL, 0xa1ba1ba79e1632dcULL},
        {0x7d7b8f7503cfdcfeULL, 0xca28a291859bbf93ULL},
        {0x5cda735244c3d43eULL, 0xfcb2cb35e702af78ULL},
        {0x3a0888136afa64a7ULL, 0x9defbf01b061adabULL},
        {0x088aaa1845b8fdd0ULL, 0xc56baec21c7a1916ULL},
        {0x8aad549e57273d45ULL, 0xf6c69a72a3989f5bULL},
        {0x36ac54e2f678864bULL, 0x9a3c2087a63f6399ULL},
        {0x84576a1bb416a7ddULL, 0xc0cb28a98fcf3c7fULL},
        {0x656d44a2a11c51d5ULL, 0xf0fdf2d3f3c30b9fULL},
        {0x9f644ae5a4b1b325ULL, 0x969eb7c47859e743ULL},
        {0x873d5d9f0dde1feeULL, 0xbc4665b596706114ULL},
        {0xa90cb506d155a7eaULL, 0xeb57ff22fc0c7959ULL},
        {0x09a7f12442d588f2ULL, 0x9316ff75dd87cbd8ULL},
        {0x0c11ed6d538aeb2fULL, 0xb7dcbf5354e9beceULL},
        {0x8f1668c8a86da5faULL, 0xe5d3ef282a242e81ULL},
        {0xf96e017d694487bcULL, 0x8fa475791a569d10ULL},
        {0x37c981dcc395a9acULL, 0xb38d92d760ec4455ULL},
        {0x85bbe253f47b1417ULL, 0xe070f78d3927556aULL},
        {0x93956d7478ccec8eULL, 0x8c469ab843b89562ULL},
        {0x387ac8d1970027b2ULL, 0xaf58416654a6babbULL},
        {0x06997b05fcc0319eULL, 0xdb2e51bfe9d0696aULL},
        {0x441fece3bdf81f03ULL, 0x88fcf317f22241e2ULL},
        {0xd527e81cad7626c3ULL, 0xab3c2fddeeaad25aULL},
        {0x8a71e223d8d3b074ULL, 0xd60b3bd56a5586f1ULL},
        {0xf6872d5667844e49ULL, 0x85c7056562757456ULL},
        {0xb428f8ac016561dbULL, 0xa738c6bebb12d16cULL},
        {0xe13336d701beba52ULL, 0xd106f86e69d785c7ULL},
        {0xecc0024661173473ULL, 0x82a45b450226b39cULL},
        {0x27f002d7f95d0190ULL, 0xa34d721642b06084ULL},
        {0x31ec038df7b441f4ULL, 0xcc20ce9bd35c78a5ULL},
        {0x7e67047175a15271ULL, 0xff290242c83396ceULL},
        {0x0f0062c6e984d386ULL, 0x9f79a169bd203e41ULL},
        {0x52c07b78a3e60868ULL, 0xc75809c42c684dd1ULL},
        {0xa7709a56ccdf8a82ULL, 0xf92e0c3537826145ULL},
        {0x88a66076400bb691ULL, 0x9bbcc7a142b17ccbULL},
        {0x6acff893d00ea435ULL, 0xc2abf989935ddbfeULL},
        {0x0583f6b8c4124d43ULL, 0xf356f7ebf83552feULL},
        {0xc3727a337a8b704aULL, 0x98165af37b2153deULL},
        {0x744f18c0592e4c5cULL, 0xbe1bf1b059e9a8d6ULL},
        {0x1162def06f79df73ULL, 0xeda2ee1c7064130cULL},
        {0x8addcb5645ac2ba8ULL, 0x9485d4d1c63e8be7ULL},
        {0x6d953e2bd7173692ULL, 0xb9a74a0637ce2ee1ULL},
        {0xc8fa8db6ccdd0437ULL, 0xe8111c87c5c1ba99ULL},
        {0x1d9c9892400a22a2ULL, 0x910ab1d4db9914a0ULL},
        {0x2503beb6d00cab4bULL, 0xb54d5e4a127f59c8ULL},
        {0x2e44ae64840fd61dULL, 0xe2a0b5dc971f303aULL},
        {0x5ceaecfed289e5d2ULL, 0x8da471a9de737e24ULL},
        {0x7425a83e872c5f47ULL, 0xb10d8e1456105dadULL},
        {0xd12f124e28f77719ULL, 0xdd50f1996b947518ULL},
        {0x82bd6b70d99aaa6fULL, 0x8a5296ffe33cc92fULL},
        {0x636cc64d1001550bULL, 0xace73cbfdc0bfb7bULL},
        {0x3c47f7e05401aa4eULL, 0xd8210befd30efa5aULL},
        {0x65acfaec34810a71ULL, 0x8714a775e3e95c78ULL},
        {0x7f1839a741a14d0dULL, 0xa8d9d1535ce3b396ULL},
        {0x1ede48111209a050ULL, 0xd31045a8341ca07cULL},
        {0x934aed0aab460432ULL, 0x83ea2b892091e44dULL},
        {0xf81da84d5617853fULL, 0xa4e4b66b68b65d60ULL},
        {0x36251260ab9d668eULL, 0xce1de40642e3f4b9ULL},
        {0xc1d72b7c6b426019ULL, 0x80d2ae83e9ce78f3ULL},
        {0xb24cf65b8612f81fULL, 0xa1075a24e4421730ULL},
        {0xdee033f26797b627ULL, 0xc94930ae1d529cfcULL},
        {0x169840ef017da3b1ULL, 0xfb9b7cd9a4a7443cULL},
        {0x8e1f289560ee864eULL, 0x9d412e0806e88aa5ULL},
        {0xf1a6f2bab92a27e2ULL, 0xc491798a08a2ad4eULL},
        {0xae10af696774b1dbULL, 0xf5b5d7ec8acb58a2ULL},
        {0xacca6da1e0a8ef29ULL, 0x9991a6f3d6bf1765ULL},
        {0x17fd090a58d32af3ULL, 0xbff610b0cc6edd3fULL},
        {0xddfc4b4cef07f5b0ULL, 0xeff394dcff8a948eULL},
        {0x4abdaf101564f98eULL, 0x95f83d0a1fb69cd9ULL},
        {0x9d6d1ad41abe37f1ULL, 0xbb764c4ca7a4440fULL},
        {0x84c86189216dc5edULL, 0xea53df5fd18d5513ULL},
        {0x32fd3cf5b4e49bb4ULL, 0x92746b9be2f8552cULL},
        {0x3fbc8c33221dc2a1ULL, 0xb7118682dbb66a77ULL},
        {0x0fabaf3feaa5334aULL, 0xe4d5e82392a40515ULL},
        {0x29cb4d87f2a7400eULL, 0x8f05b1163ba6832dULL},
        {0x743e20e9ef511012ULL, 0xb2c71d5bca9023f8ULL},
        {0x914da9246b255416ULL, 0xdf78e4b2bd342cf6ULL},
        {0x1ad089b6c2f7548eULL, 0x8bab8eefb6409c1aULL},
        {0xa184ac2473b529b1ULL, 0xae9672aba3d0c320ULL},
        {0xc9e5d72d90a2741eULL, 0xda3c0f568cc4f3e8ULL},
        {0x7e2fa67c7a658892ULL, 0x8865899617fb1871ULL},
        {0xddbb901b98feeab7ULL, 0xaa7eebfb9df9de8dULL},
        {0x552a74227f3ea565ULL, 0xd51ea6fa85785631ULL},
        {0xd53a88958f87275fULL, 0x8533285c936b35deULL},
        {0x8a892abaf368f137ULL, 0xa67ff273b8460356ULL},
        {0x2d2b7569b0432d85ULL, 0xd01fef10a657842cULL},
        {0x9c3b29620e29fc73ULL, 0x8213f56a67f6b29bULL},
        {0x8349f3ba91b47b8fULL, 0xa298f2c501f45f42ULL},
        {0x241c70a936219a73ULL, 0xcb3f2f7642717713ULL},
        {0xed238cd383aa0110ULL, 0xfe0efb53d30dd4d7ULL},
        {0xf4363804324a40aaULL, 0x9ec95d1463e8a506ULL},
        {0xb143c6053edcd0d5ULL, 0xc67bb4597ce2ce48ULL},
        {0xdd94b7868e94050aULL, 0xf81aa16fdc1b81daULL},
        {0xca7cf2b4191c8326ULL, 0x9b10a4e5e9913128ULL},
        {0xfd1c2f611f63a3f0ULL, 0xc1d4ce1f63f57d72ULL},
        {0xbc633b39673c8cecULL, 0xf24a01a73cf2dccfULL},
        {0xd5be0503e085d813ULL, 0x976e41088617ca01ULL},
        {0x4b2d8644d8a74e18ULL, 0xbd49d14aa79dbc82ULL},
        {0xddf8e7d60ed1219eULL, 0xec9c459d51852ba2ULL},
        {0xcabb90e5c942b503ULL, 0x93e1ab8252f33b45ULL},
        {0x3d6a751f3b936243ULL, 0xb8da1662e7b00a17ULL},
        {0x0cc512670a783ad4ULL, 0xe7109bfba19c0c9dULL},
        {0x27fb2b80668b24c5ULL, 0x906a617d450187e2ULL},
        {0xb1f9f660802dedf6ULL, 0xb484f9dc9641e9daULL},
        {0x5e7873f8a0396973ULL, 0xe1a63853bbd26451ULL},
        {0xdb0b487b6423e1e8ULL, 0x8d07e33455637eb2ULL},
        {0x91ce1a9a3d2cda62ULL, 0xb049dc016abc5e5fULL},
        {0x7641a140cc7810fbULL, 0xdc5c5301c56b75f7ULL},
        {0xa9e904c87fcb0a9dULL, 0x89b9b3e11b6329baULL},
        {0x546345fa9fbdcd44ULL, 0xac2820d9623bf429ULL},
        {0xa97c177947ad4095ULL, 0xd732290fbacaf133ULL},
        {0x49ed8eabcccc485dULL, 0x867f59a9d4bed6c0ULL},
        {0x5c68f256bfff5a74ULL, 0xa81f301449ee8c70ULL},
        {0x73832eec6fff3111ULL, 0xd226fc195c6a2f8cULL},
        {0xc831fd53c5ff7eabULL, 0x83585d8fd9c25db7ULL},
        {0xba3e7ca8b77f5e55ULL, 0xa42e74f3d032f525ULL},
        {0x28ce1bd2e55f35ebULL, 0xcd3a1230c43fb26fULL},
        {0x7980d163cf5b81b3ULL, 0x80444b5e7aa7cf85ULL},
        {0xd7e105bcc332621fULL, 0xa0555e361951c366ULL},
        {0x8dd9472bf3fefaa7ULL, 0xc86ab5c39fa63440ULL},
        {0xb14f98f6f0feb951ULL, 0xfa856334878fc150ULL},
        {0x6ed1bf9a569f33d3ULL, 0x9c935e00d4b9d8d2ULL},
        {0x0a862f80ec4700c8ULL, 0xc3b8358109e84f07ULL},
        {0xcd27bb612758c0faULL, 0xf4a642e14c6262c8ULL},
        {0x8038d51cb897789cULL, 0x98e7e9cccfbd7dbdULL},
        {0xe0470a63e6bd56c3ULL, 0xbf21e44003acdd2cULL},
        {0x1858ccfce06cac74ULL, 0xeeea5d5004981478ULL},
        {0x0f37801e0c43ebc8ULL, 0x95527a5202df0ccbULL},
        {0xd30560258f54e6baULL, 0xbaa718e68396cffdULL},
        {0x47c6b82ef32a2069ULL, 0xe950df20247c83fdULL},
        {0x4cdc331d57fa5441ULL, 0x91d28b7416cdd27eULL},
        {0xe0133fe4adf8e952ULL, 0xb6472e511c81471dULL},
        {0x58180fddd97723a6ULL, 0xe3d8f9e563a198e5ULL},
        {0x570f09eaa7ea7648ULL, 0x8e679c2f5e44ff8fULL},
};

//      floor(q * log2(10)) + 63, with the logarithm carried as a fixed point
//      fraction: 217706 over 65536 is log2(10) to more places than the range
//      of q here can tell apart, and the multiply cannot overflow because q
//      is between -342 and 308.
//
//      The shift has to be the arithmetic one, because floor and not truncate
//      is what is wanted for a negative q. gcc defines a right shift of a
//      negative signed value that way on every target, which is the same
//      guarantee the rest of this library already builds on.
static b32 numbers_binary_power(b32 power)
{
        return ((217706 * power) >> 16) + 63;
}

static p64 numbers_multiply_high(p64 left, p64 right, p64 address_to low)
{
        p128 product = (p128)left * (p128)right;

        address_to low = (p64)product;

        return (p64)(product >> 64);
}

static bool numbers_estimate(p64 significand, b32 power,
                             const numbers_format address_to shape,
                             p64 address_to answer)
{
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        b32 precision = shape->significand_place + 3;
        b32 lead;
        b32 upper;
        b32 exponent;
        b32 place;
        p64 high;
        p64 low;
        p64 mask;
        p64 mantissa;

        if (significand == 0)
                return false;

        if (power < NUMBERS_SMALLEST_POWER || power > NUMBERS_LARGEST_POWER)
                return false;

        lead = bits_leading_zeros(significand);
        significand <<= lead;

        mask = ~(p64)0 >> precision;
        high = numbers_multiply_high(
                significand,
                numbers_power_of_five[power - NUMBERS_SMALLEST_POWER][1],
                address_of low);

        //      The top of the product is all ones under the mask, so the
        //      bits below it can still carry into the answer and the second
        //      half of the table entry has to be brought in.
        if ((high & mask) == mask)
        {
                p64 second_low;
                p64 second_high = numbers_multiply_high(
                        significand,
                        numbers_power_of_five[power - NUMBERS_SMALLEST_POWER][0],
                        address_of second_low);

                low += second_high;

                if (second_high > low)
                        high++;
        }

        //      Still saturated after both halves: 128 bits could not separate
        //      this value from the boundary. The powers between ten to the
        //      minus twenty seventh and ten to the fifty fifth are exact in
        //      the table, so there is nothing to separate there and the
        //      answer stands; everywhere else the tier declines.
        if (low == ~(p64)0 && (power < -27 || power > 55))
                return false;

        upper = (b32)(high >> 63);
        place = upper + 64 - shape->significand_place - 3;
        mantissa = high >> place;
        exponent = numbers_binary_power(power) + upper - lead - shape->bias;

        //      Subnormal, zero or infinite answers go to the decimal register,
        //      which is the only tier that can decide the errno question.
        if (exponent <= 0 || exponent >= limit)
                return false;

        //      The one case where round-half-to-even has to be applied by
        //      hand: the discarded half is exactly a half, the product was
        //      exact, and the bit above it is odd.
        if (low <= 1 && power >= shape->round_even_low &&
            power <= shape->round_even_high && (mantissa & 3) == 1 &&
            (mantissa << place) == high)
                mantissa &= ~(p64)1;

        mantissa += mantissa & 1;
        mantissa >>= 1;

        if (mantissa >= ((p64)2 << shape->significand_place))
        {
                mantissa = (p64)1 << shape->significand_place;
                exponent++;

                if (exponent >= limit)
                        return false;
        }

        mantissa &= ~((p64)1 << shape->significand_place);

        address_to answer = ((p64)(p32)exponent << shape->exponent_place) | mantissa;

        return true;
}

/*
        The tier as the conversion calls it: one estimate when every digit
        fitted, two when they did not, and a refusal the moment anything is
        not certain.
*/
static bool numbers_estimated(numbers_scan address_to number,
                              const numbers_format address_to shape,
                              p64 address_to answer)
{
        b32 power = number->point - number->packed_count;
        p64 first;

        if (number->count == 0)
                return false;

        if (!numbers_estimate(number->packed, power, shape, address_of first))
                return false;

        if (number->significant != number->packed_count)
        {
                p64 second;

                if (!numbers_estimate(number->packed + 1, power, shape,
                                      address_of second))
                        return false;

                if (first != second)
                        return false;
        }

        if (number->negative)
                first |= (p64)1 << (shape->exponent_place + shape->exponent_bits);

        address_to answer = first;

        return true;
}

/*
        The exact tier, which is Clinger's.

        Two conditions and one operation. The significant digits must all
        have fitted in the p64 -- nineteen of them at most, and none dropped
        -- and the integer they form must be small enough to be a double with
        nothing lost, which is two to the fifty third. The power of ten must
        be one of the ones that is itself exactly a double, which runs from
        ten to the minus twenty second to ten to the twenty second because
        beyond that a power of ten needs more than fifty three bits.

        Given both, the answer is one multiply or one divide of two exact
        operands, and IEEE 754 says a single operation returns the correctly
        rounded result of the exact answer. There is nothing to check
        afterwards and nothing that can be off by a bit.

        The narrow one is the same argument in twenty four bits, and it is
        computed in f32 rather than in double and narrowed: a double result
        rounded again to a float rounds twice, and two roundings are not one.
*/
static const decimal numbers_power_of_ten[23] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static const f32 numbers_narrow_power_of_ten[11] = {
        1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

static bool numbers_exact_double(numbers_scan address_to number, decimal address_to answer)
{
        b32 power;
        decimal value;

        if (number->truncated || number->significant != number->packed_count)
                return false;

        if (number->packed > ((p64)1 << 53))
                return false;

        power = number->point - number->significant;

        if (power > 22 || power < -22)
                return false;

        value = (decimal)number->packed;

        if (power > 0)
                value = value * numbers_power_of_ten[power];
        else if (power < 0)
                value = value / numbers_power_of_ten[-power];

        address_to answer = number->negative ? -value : value;

        return true;
}

static bool numbers_exact_narrow(numbers_scan address_to number, f32 address_to answer)
{
        b32 power;
        f32 value;

        if (number->truncated || number->significant != number->packed_count)
                return false;

        if (number->packed > ((p64)1 << 24))
                return false;

        power = number->point - number->significant;

        if (power > 10 || power < -10)
                return false;

        value = (f32)number->packed;

        if (power > 0)
                value = value * numbers_narrow_power_of_ten[power];
        else if (power < 0)
                value = value / numbers_narrow_power_of_ten[-power];

        address_to answer = number->negative ? -value : value;

        return true;
}

/*
        The two answers that are not numbers, built out of the format rather
        than out of a constant, so that one body serves all four widths.

        An infinity is the exponent field saturated over a zero significand.
        A quiet NaN is that same exponent over a significand whose top bit is
        set, and glibc puts the parenthesised n-char-sequence in the bits
        below it, so this does too -- strtoull read the sequence during the
        scan and left it in `packed`. x87 is the one format where the leading
        bit of the significand is stored rather than implied, and a NaN there
        has to carry it, which is the one line the other three do not need.
*/
static p128 numbers_special(const numbers_format address_to shape, bool negative,
                            b32 kind, p64 payload)
{
        b32 limit = ((b32)1 << shape->exponent_bits) - 1;
        p128 bits = 0;

        if (kind == NUMBERS_NOT_A_NUMBER)
        {
                bits = (p128)1 << (shape->significand_place - 1);
                bits |= (p128)payload &
                        ((((p128)1 << (shape->significand_place - 1)) - 1));
        }

        //      x87 stores the leading bit of its significand rather than
        //      implying it, and an eighty bit infinity or NaN with that bit
        //      clear is one of the shapes the hardware calls invalid. glibc
        //      sets it and so does this; the other three formats have no such
        //      bit to set.
        if (shape->exponent_place > shape->significand_place)
                bits |= (p128)1 << shape->significand_place;

        bits |= (p128)(p64)limit << shape->exponent_place;

        if (negative)
                bits |= (p128)1 << (shape->exponent_place + shape->exponent_bits);

        return bits;
}

/*
        The three conversions themselves, and the one shape they share.

        Read the text; hand back the end pointer whatever happened; answer a
        name or a hexadecimal float or a decimal one; and set ERANGE when the
        answer was pushed to an infinity or squeezed down into the subnormals
        or to zero. errno is only ever written, never cleared -- C says a
        library function may set errno and may not clear one it did not set,
        and a caller that wants to know clears it first.

        NUMBERS_SLOW_TIER_ONLY exists for the test lane and for nothing else.
        Defined, both fast tiers are compiled out and every conversion goes
        through the decimal register, so the lane can run identical inputs
        through both and diff -- which is how a defect in a fast tier gets
        found without a reference library being in the loop.
*/
static decimal string_to_decimal(string_address input, string_address address_to stopped)
{
        numbers_scan number;
        numbers_shape shape;
        b32 condition = NUMBERS_FINE;

        numbers_read(input, address_of number);

        if (stopped)
                address_to stopped = number.stopped;

        if (number.kind == NUMBERS_NONE)
                return 0.0;

        if (number.kind == NUMBERS_INFINITE || number.kind == NUMBERS_NOT_A_NUMBER)
        {
                shape.bits = (p64)numbers_special(address_of numbers_binary64,
                                                  number.negative, number.kind,
                                                  number.packed);
                return shape.value;
        }

        if (number.hexadecimal)
        {
                shape.bits = (p64)numbers_round_binary(address_of number,
                                                       address_of numbers_binary64,
                                                       address_of condition);
        }
        else
        {
#ifndef NUMBERS_SLOW_TIER_ONLY
                decimal quick;
                p64 estimate;

                if (numbers_exact_double(address_of number, address_of quick))
                        return quick;

                if (numbers_estimated(address_of number,
                                      address_of numbers_binary64,
                                      address_of estimate))
                {
                        shape.bits = estimate;
                        return shape.value;
                }
#endif
                shape.bits = (p64)numbers_assemble(address_of number,
                                                   address_of numbers_binary64,
                                                   address_of condition);
        }

        if (condition != NUMBERS_FINE)
                errno = ERANGE;

        return shape.value;
}

static f32 string_to_narrow(string_address input, string_address address_to stopped)
{
        numbers_scan number;
        numbers_narrow_shape shape;
        b32 condition = NUMBERS_FINE;

        numbers_read(input, address_of number);

        if (stopped)
                address_to stopped = number.stopped;

        if (number.kind == NUMBERS_NONE)
                return 0.0f;

        if (number.kind == NUMBERS_INFINITE || number.kind == NUMBERS_NOT_A_NUMBER)
        {
                shape.bits = (p32)numbers_special(address_of numbers_binary32,
                                                  number.negative, number.kind,
                                                  number.packed);
                return shape.value;
        }

        if (number.hexadecimal)
        {
                shape.bits = (p32)numbers_round_binary(address_of number,
                                                       address_of numbers_binary32,
                                                       address_of condition);
        }
        else
        {
#ifndef NUMBERS_SLOW_TIER_ONLY
                f32 quick;
                p64 estimate;

                if (numbers_exact_narrow(address_of number, address_of quick))
                        return quick;

                if (numbers_estimated(address_of number,
                                      address_of numbers_binary32,
                                      address_of estimate))
                {
                        shape.bits = (p32)estimate;
                        return shape.value;
                }
#endif
                shape.bits = (p32)numbers_assemble(address_of number,
                                                   address_of numbers_binary32,
                                                   address_of condition);
        }

        if (condition != NUMBERS_FINE)
                errno = ERANGE;

        return shape.value;
}

/*
        long double, which is eighty bit x87 on x86_64 and binary128 on arm64
        and riscv64, and is the format the fast tiers deliberately skip.

        There is no exact tier here and no estimating one: both are written
        for a significand that fits a p64 with room over it, and neither
        eighty nor a hundred and thirteen bits does. Every long double
        conversion therefore runs the decimal register, which is correct and
        is measured in microseconds rather than nanoseconds. That is the right
        trade for a format a program converts once at startup and never in a
        loop, and it is what let this format cost four numbers rather than a
        second implementation.

        Nothing here does arithmetic on a long double. Every value is
        assembled as bits and read out through a union, which is not only
        faster but necessary: a -nostdlib link has no libgcc under it, and a
        single comparison of two binary128 values on arm64 or riscv64 is a
        call to __letf2 that would not resolve.
*/
static f128 string_to_extended(string_address input, string_address address_to stopped)
{
        numbers_scan number;
        numbers_extended_shape shape;
        b32 condition = NUMBERS_FINE;

        numbers_read(input, address_of number);

        if (stopped)
                address_to stopped = number.stopped;

        if (number.kind == NUMBERS_NONE)
        {
                shape.bits = 0;
                return shape.value;
        }

        if (number.kind == NUMBERS_INFINITE || number.kind == NUMBERS_NOT_A_NUMBER)
        {
                shape.bits = numbers_special(address_of numbers_extended,
                                             number.negative, number.kind,
                                             number.packed);
                return shape.value;
        }

        if (number.hexadecimal)
                shape.bits = numbers_round_binary(address_of number,
                                                  address_of numbers_extended,
                                                  address_of condition);
        else
                shape.bits = numbers_assemble(address_of number,
                                              address_of numbers_extended,
                                              address_of condition);

        if (condition != NUMBERS_FINE)
                errno = ERANGE;

        return shape.value;
}

/*
        The standard names.

        One line wrappers rather than macros, for the reason math.c gives:
        a program can take the address of one, and a local variable called
        strtod does not silently become a call. atof is here rather than in
        standard.inc beside atoi because it is strtod with the end pointer
        thrown away, and strtod is C.
*/
//      The end pointer is a char ** and not a string_address address_to,
//      which is the house spelling everything below the standard names uses.
//      declare.c settled that question for the ninety eight names it covers
//      and these three are the same kind of name reached the same way: a
//      program that brings its own <stdlib.h> line for strtod must compile
//      against this one, and it cannot if the tree says the end pointer is
//      an unsigned char **. The cast inside is the whole of the difference;
//      the two types are one width in one register.
static decimal strtod(const char address_to input, char address_to address_to stopped)
{
        return string_to_decimal((string_address)input, (string_address address_to)stopped);
}

static f32 strtof(const char address_to input, char address_to address_to stopped)
{
        return string_to_narrow((string_address)input, (string_address address_to)stopped);
}

static f128 strtold(const char address_to input, char address_to address_to stopped)
{
        return string_to_extended((string_address)input, (string_address address_to)stopped);
}

static decimal atof(string_address input)
{
        return string_to_decimal(input, null);
}

#pragma GCC pop_options

#endif // decimal_bits == 64

#endif // !KERNEL_MODE && !STANDARD_NO_PLATFORM

#endif // STANDARD_MODERN_C_STANDARD_NUMBERS
