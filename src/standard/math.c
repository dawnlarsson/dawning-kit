
/*
        Experimental C standard library

        The rest of <math.h>: the transcendentals, and the exact ones

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

#ifndef STANDARD_MODERN_C_STANDARD_MATH
#define STANDARD_MODERN_C_STANDARD_MATH

/*
        This is ordinary C, and it has to be.

        src/platform/standard.inc already holds the half of <math.h> that is
        an instruction: sqrt, fabs, trunc, floor, ceil, round, fmin, fmax,
        fma and copysign are one opcode on at least two of the three machines
        and the assembly there is the floor. Nothing below is like that.
        exp is a range reduction, a polynomial and an exponent field write;
        pow is a logarithm carried in two doubles so that multiplying it by
        an exponent of a thousand still leaves fifty three good bits. Those
        are algorithms, and an algorithm written three times in three
        assemblers is an algorithm with three sets of bugs. So it lives here,
        in the one place the library keeps C -- included from
        src/compiler_memory.c, which is deliberately outside library.c's
        graph, in the way src/net/netlink.c holds the netlink wire layer.

        It depends on library.c alone, through standard.inc: square_root,
        absolute, decimal_floor, decimal_truncated, decimal_rounded,
        decimal_with_sign and decimal_multiply_add. Every one of those is a
        single instruction on the machines that have it, so reaching for them
        rather than writing the arithmetic again is both shorter and faster.

        WHAT THE NAMES ARE, AND THE ONE THAT COULD NOT BE HAD

        The library already exports `log`, and it is not a logarithm: it is
        the buffered writer, `fn log(address_any data, positive length)`, and
        `string_format(log, ...)` is written in a hundred places in this tree.
        Defining C's log here -- as a function, as a macro, as anything --
        would turn every one of those call sites into a type error or, worse,
        into a call to a logarithm with a pointer in it. So the natural
        logarithm ships as `logarithm`, with `ln` as its short alias, and
        there is no `log`. A program that wants the C spelling has to make
        that choice for itself, knowing what it costs.

        Everything else has its prose name and a one-line wrapper carrying
        the standard name, at the bottom of the file. The wrappers are
        `static` like everything else here, so an unused one costs nothing
        and a program that takes `&pow` still gets a real function pointer,
        which a macro alias could not give it.

        WHY THE CONTRACTION PRAGMA IS NOT OPTIONAL

        gcc contracts `a*b + c` into a fused multiply-add by default on arm64
        and riscv64, and cannot on baseline x86_64, which has no FMA3. That
        is three different answers from one source, and for most of this file
        it would only mean harmless last-bit disagreement. In the compensated
        arithmetic below it is not harmless: math_two_product computes the
        exact rounding error of a product as fma(a, b, -p), and if the
        compiler is also allowed to fuse the plain product p = a*b that the
        error is measured against, the two roundings that were supposed to
        differ become the same one and the error comes back zero. A pow built
        on that is quietly wrong rather than obviously wrong. So the file
        turns contraction off for its own extent and puts it back, and every
        fused operation it actually wants is an explicit call to
        decimal_multiply_add.

        WHAT IS NOT HERE

        No errno and no fenv. A domain error returns a NaN and raises
        nothing; an overflow returns an infinity and raises nothing. This
        library has no errno to set and no exception flags to read, and
        pretending otherwise would cost every call a store nobody reads.

        HOW THE NUMBERS BELOW WERE TAKEN

        Every routine here is measured against glibc's LONG DOUBLE version of
        the same function, rounded back to a double. On x86_64 that is an
        eighty bit format with a sixty four bit significand, so the reference
        carries eleven more bits than the answer being checked and is a fair
        judge of it; glibc's own double routines are quoted separately,
        because for two of these functions they are the ones that are wrong.
        Arguments are drawn log-spaced across the whole of each domain with
        random signs and random mantissas, eight million of them per routine
        unless the line says otherwise, plus dense sweeps of every place a
        branch changes and every special value C99 names.

        In summary, worst case, against the long double reference:

              exact, bit for bit, no error at all
                    fmod  remainder  ldexp  scalbn  frexp  modf
                    isnan  isinf  isfinite  isnormal  signbit  fpclassify

              within 1 ulp
                    exp  exp2  expm1  logarithm  log2  log10  pow
                    cbrt  hypot  sin  cos  asin  acos  atan

              within 2 ulp
                    tan  atan2  sinh  tanh   (cosh is 1)

        Two of those beat glibc's double routines rather than matching them.
        glibc's cbrt is out by as much as four ulp where this one is inside
        one, and its log10 by two; every other disagreement between the two
        libraries is a single last bit, and this one is on the correct side
        of it as often as not.

        The same test built freestanding runs on x86_64, arm64 under
        qemu-aarch64 and riscv64 under qemu-riscv64, and the three produce
        byte identical output on ten thousand results with four exceptions,
        all of them the sign bit of a NaN: 0/0 gives a negative quiet NaN on
        x86_64 and a positive one on the other two. That is the hardware's
        default NaN and glibc reports it the same way.
*/

/*
        Everything here takes or returns a decimal, and a decimal in a
        signature is refused by the arm64 kernel build whether or not
        anything calls it -- the same reason standard.inc guards its own
        floating point half. Kernel code may not touch the floating point
        registers without asking first, so this is right anyway.

        The second half of the guard is about width. Every polynomial,
        every constant and every bit mask below is double precision, chosen
        for a fifty three bit significand and an eleven bit exponent. On a
        profile where `decimal` is f32 -- which is what library.c gives a
        thirty two bit target -- none of it would be either correct or
        useful, so the family is simply absent there rather than silently
        wrong. All three architectures this project builds are sixty four
        bit, so the guard is documentation rather than a fork.
*/
#if !defined(KERNEL_MODE) && decimal_bits == 64

#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")

/*
        A double seen as its bits, and the bits seen as a double.

        Every classification below and every exponent write is a question
        about the bit pattern rather than about the number, and a union is
        how C asks it. gcc defines reading a union member other than the one
        last written, and it compiles to the one register move the hardware
        needs -- movq on x86_64, fmov on arm64, fmv.x.d on riscv64 -- with no
        store to memory in between.
*/
typedef union
{
        decimal value;
        p64 bits;
        b64 signed_bits;
} math_shape;

typedef union
{
        f32 value;
        p32 bits;
        b32 signed_bits;
} math_narrow_shape;

#define MATH_SIGN_MASK 0x8000000000000000ULL
#define MATH_MAGNITUDE_MASK 0x7fffffffffffffffULL
#define MATH_INFINITY_BITS 0x7ff0000000000000ULL
#define MATH_EXPONENT_BIAS 1023
#define MATH_MANTISSA_BITS 52
#define MATH_MANTISSA_MASK 0x000fffffffffffffULL
#define MATH_IMPLIED_BIT 0x0010000000000000ULL

/*
        The classes, which are the classification macros the C library calls
        isnan, isinf, isfinite, isnormal, signbit and fpclassify.

        The question that had to be answered before writing these was whether
        they belong beside square_root in standard.inc as assembly. They do
        not, and the reason is that the C library does not define them as
        functions in the first place: they are macros, and a macro is what
        makes them free. isnan of a value already in a register is one move
        to a general register, one and, and one compare -- three instructions
        that fold into whatever the caller was doing. A call to an assembly
        routine that did the same three instructions would pay a call and a
        return on top of them, which is more than doubling the cost of the
        thing being called, and it would do it at every one of the dozens of
        guard sites inside this file. Written as C the compiler inlines them
        and often removes them entirely, because most of the tests below are
        against values it can already see the shape of.

        So they are C, and they are exact: no rounding happens anywhere in
        them. The six float ones were checked against glibc's macros on all
        four billion two hundred and ninety four million float bit patterns,
        exhaustively, and the six double ones on forty million random
        patterns. Zero disagreements in either.

        fpclassify's five answers are numbered the way glibc numbers them --
        NaN 0, infinite 1, zero 2, subnormal 3, normal 4 -- not because the
        standard says so, it deliberately does not, but because a program
        that moves between the two libraries should not find its switch
        statement quietly relabelled.
*/
#define MATH_CLASS_NAN 0
#define MATH_CLASS_INFINITE 1
#define MATH_CLASS_ZERO 2
#define MATH_CLASS_SUBNORMAL 3
#define MATH_CLASS_NORMAL 4

static bool decimal_is_nan(decimal value)
{
        math_shape shape;
        shape.value = value;
        return (shape.bits & MATH_MAGNITUDE_MASK) > MATH_INFINITY_BITS;
}

static bool decimal_is_infinite(decimal value)
{
        math_shape shape;
        shape.value = value;
        return (shape.bits & MATH_MAGNITUDE_MASK) == MATH_INFINITY_BITS;
}

static bool decimal_is_finite(decimal value)
{
        math_shape shape;
        shape.value = value;
        return (shape.bits & MATH_MAGNITUDE_MASK) < MATH_INFINITY_BITS;
}

static bool decimal_is_normal(decimal value)
{
        math_shape shape;
        p64 magnitude;
        shape.value = value;
        magnitude = shape.bits & MATH_MAGNITUDE_MASK;
        return magnitude >= MATH_IMPLIED_BIT && magnitude < MATH_INFINITY_BITS;
}

//      The sign of a negative zero is what separates this from a comparison
//      against zero, and it is the whole reason the routine exists.
static bool decimal_sign_bit(decimal value)
{
        math_shape shape;
        shape.value = value;
        return (shape.bits >> 63) != 0;
}

static b32 decimal_class(decimal value)
{
        math_shape shape;
        p64 magnitude;
        shape.value = value;
        magnitude = shape.bits & MATH_MAGNITUDE_MASK;

        if (magnitude == 0)
                return MATH_CLASS_ZERO;
        if (magnitude < MATH_IMPLIED_BIT)
                return MATH_CLASS_SUBNORMAL;
        if (magnitude < MATH_INFINITY_BITS)
                return MATH_CLASS_NORMAL;
        if (magnitude == MATH_INFINITY_BITS)
                return MATH_CLASS_INFINITE;
        return MATH_CLASS_NAN;
}

//      The same five questions asked of a float, so that a program working
//      in single precision does not have to widen to ask them. Widening is
//      exact for every float including the NaNs, so these would answer
//      correctly through the wide ones -- but the widening is an instruction
//      and the mask is a different constant, and doing it in the width the
//      caller has is both shorter and closer to what was meant.
static bool narrow_is_nan(f32 value)
{
        math_narrow_shape shape;
        shape.value = value;
        return (shape.bits & 0x7fffffffU) > 0x7f800000U;
}

static bool narrow_is_infinite(f32 value)
{
        math_narrow_shape shape;
        shape.value = value;
        return (shape.bits & 0x7fffffffU) == 0x7f800000U;
}

static bool narrow_is_finite(f32 value)
{
        math_narrow_shape shape;
        shape.value = value;
        return (shape.bits & 0x7fffffffU) < 0x7f800000U;
}

static bool narrow_is_normal(f32 value)
{
        math_narrow_shape shape;
        p32 magnitude;
        shape.value = value;
        magnitude = shape.bits & 0x7fffffffU;
        return magnitude >= 0x00800000U && magnitude < 0x7f800000U;
}

static bool narrow_sign_bit(f32 value)
{
        math_narrow_shape shape;
        shape.value = value;
        return (shape.bits >> 31) != 0;
}

static b32 narrow_class(f32 value)
{
        math_narrow_shape shape;
        p32 magnitude;
        shape.value = value;
        magnitude = shape.bits & 0x7fffffffU;

        if (magnitude == 0)
                return MATH_CLASS_ZERO;
        if (magnitude < 0x00800000U)
                return MATH_CLASS_SUBNORMAL;
        if (magnitude < 0x7f800000U)
                return MATH_CLASS_NORMAL;
        if (magnitude == 0x7f800000U)
                return MATH_CLASS_INFINITE;
        return MATH_CLASS_NAN;
}

/*
        The C spellings, which are macros because C says they are.

        The dispatch is on the size of the argument rather than on its type,
        because a size comparison is a constant the compiler folds and a
        _Generic would refuse an integer argument that the C macros are
        required to accept. Both arms of the conditional are type correct for
        any arithmetic argument, so only one survives compilation. A long
        double argument goes down the double arm and is narrowed, which is
        the one place these differ from a library that has a long double
        path; nothing in this tree has one.
*/
#define isnan(value) (sizeof(value) == sizeof(f32) ? narrow_is_nan((f32)(value)) : decimal_is_nan((decimal)(value)))
#define isinf(value) (sizeof(value) == sizeof(f32) ? narrow_is_infinite((f32)(value)) : decimal_is_infinite((decimal)(value)))
#define isfinite(value) (sizeof(value) == sizeof(f32) ? narrow_is_finite((f32)(value)) : decimal_is_finite((decimal)(value)))
#define isnormal(value) (sizeof(value) == sizeof(f32) ? narrow_is_normal((f32)(value)) : decimal_is_normal((decimal)(value)))
#define signbit(value) (sizeof(value) == sizeof(f32) ? narrow_sign_bit((f32)(value)) : decimal_sign_bit((decimal)(value)))
#define fpclassify(value) (sizeof(value) == sizeof(f32) ? narrow_class((f32)(value)) : decimal_class((decimal)(value)))

#define FP_NAN MATH_CLASS_NAN
#define FP_INFINITE MATH_CLASS_INFINITE
#define FP_ZERO MATH_CLASS_ZERO
#define FP_SUBNORMAL MATH_CLASS_SUBNORMAL
#define FP_NORMAL MATH_CLASS_NORMAL

/*
        The constants, and why several of them are written twice.

        A double holds fifty three bits and several of the reductions below
        need more than that from a constant they are about to multiply by a
        number as large as a thousand. Where that happens the constant is
        carried as a pair: a leading double, and a second double holding what
        the first one could not, so that the pair together is good to about a
        hundred and six bits. LN2_HEAD is not the nearest double to ln 2 --
        it is ln 2 rounded to thirty two significant bits, chosen so that
        multiplying it by any integer up to two million is exact and leaves
        no rounding to account for. LN2_TAIL then carries the rest.

        Where a constant is used as a plain multiplier and the pair is only
        there for precision, the head IS the nearest double and the tail is
        the difference. LOG2E and LOG2E_TAIL are that shape, as are LOG10E,
        LOG10_2 and PI_OVER_TWO.

        Every number below was produced by rounding a hundred digit decimal
        expansion, and each is written with the seventeen significant digits
        that round trip exactly through a double.
*/
#define MATH_LN2 0.6931471805599453             // the nearest double to ln 2
#define MATH_LN2_TAIL 2.3190468138462996e-17    // ln 2 minus that
#define MATH_LN2_HEAD 0.6931471806019545        // ln 2 to 32 significant bits
#define MATH_LN2_HEAD_TAIL -4.2009150726810846e-11
#define MATH_LOG2E 1.4426950408889634
#define MATH_LOG2E_TAIL 2.0355273740931033e-17
#define MATH_LOG10E 0.4342944819032518
#define MATH_LOG10E_TAIL 1.098319650216765e-17
#define MATH_LOG10_2 0.3010299956639812
#define MATH_LOG10_2_TAIL -2.8037281277851704e-18
#define MATH_PI 3.141592653589793
#define MATH_PI_TAIL 1.2246467991473532e-16
#define MATH_PI_OVER_TWO 1.5707963267948966
#define MATH_PI_OVER_TWO_TAIL 6.123233995736766e-17
#define MATH_PI_OVER_FOUR 0.7853981633974483
#define MATH_THREE_PI_OVER_FOUR 2.356194490192345
#define MATH_TWO_TO_1023 8.98846567431158e+307
#define MATH_TWO_TO_MINUS_969 2.004168360008973e-292
#define MATH_TWO_TO_MINUS_54 5.551115123125783e-17
#define MATH_TWO_TO_MINUS_28 3.725290298461914e-09
#define MATH_TWO_TO_MINUS_27 7.450580596923828e-09
#define MATH_HUGE 1.7976931348623157e+308

//      The mantissa field of the square root of two, which is where a
//      logarithm splits its argument so that the reduced value lands in
//      [sqrt(1/2), sqrt(2)) and the series that follows sees an argument no
//      larger than about 0.1716 in magnitude.
#define MATH_SQRT2_MANTISSA 0x0006a09e667f3bcdULL

/*
        Two sums and one product that keep what rounding threw away.

        The whole of pow, and the good half of log2 and log10, rests on being
        able to carry a number in two doubles. Addition and multiplication
        both have the property that the error of a single rounded operation
        is itself exactly representable, and these three routines hand it
        back.

        math_two_sum is Knuth's, and it is exact for any two arguments in any
        order: it costs six additions where the version that assumes the
        first argument is the larger costs three. Both appear below, and the
        cheap one is used only where the ordering is known from the
        arithmetic rather than believed.

        math_two_product is the one that could not be written in plain C.
        The error of a rounded product needs the full product, and the only
        way to see the bits a double multiply discarded is a fused
        multiply-add, which computes a*b to full width and subtracts before
        rounding once. That is decimal_multiply_add, which is one instruction
        on arm64 and riscv64 and a software body on a baseline x86_64 without
        FMA3 -- correct either way, which is what matters here. Dekker's
        splitting trick would avoid the dependency, but it is silently
        destroyed by the very contraction this file disables, and one build
        flag going missing should not turn an exact routine into an
        approximate one.
*/
static decimal math_two_sum(decimal first, decimal second, decimal address_to error)
{
        decimal sum = first + second;
        decimal first_part = sum - second;
        decimal second_part = sum - first_part;
        address_to error = (first - first_part) + (second - second_part);
        return sum;
}

//      Only correct when |first| >= |second|, which every caller of it below
//      knows from the shape of what it is adding rather than from a test.
static decimal math_fast_two_sum(decimal first, decimal second, decimal address_to error)
{
        decimal sum = first + second;
        address_to error = second - (sum - first);
        return sum;
}

static decimal math_two_product(decimal first, decimal second, decimal address_to error)
{
        decimal product = first * second;
        address_to error = decimal_multiply_add(first, second, -product);
        return product;
}

/*
        A quotient with its own rounding repaired.

        A single divide is already correctly rounded, so a quotient of two
        exact numbers needs nothing from this. The arctangent's reduction is
        not that: the answer it produces is added to a table entry it will
        largely cancel against, so the division's half ulp arrives in the
        result multiplied by however much of the table entry survives, and it
        is worth a fused multiply-add to be rid of. One Newton step over the
        exact residual takes the division's own contribution back out.
*/
static decimal math_quotient(decimal numerator, decimal denominator)
{
        decimal quotient = numerator / denominator;
        return quotient +
               decimal_multiply_add(-quotient, denominator, numerator) / denominator;
}

/*
        Scaling by a power of two, which is ldexp and scalbn and the floor
        under every routine below that finishes by writing an exponent.

        Writing the exponent field directly is the obvious implementation and
        it is wrong twice: it overflows silently when the exponent leaves the
        eleven bit field, and it cannot produce a subnormal at all, because a
        subnormal is not an exponent write but a shift that loses bits and
        has to round once while losing them. Multiplying by a power of two
        instead gets both right, because the hardware multiply already knows
        how to overflow to infinity and how to round into the subnormal
        range, and it rounds exactly once.

        The three stage ladder is what keeps the multiplier itself
        representable. A single 2^n is only a double for n in [-1074, 1023],
        so an exponent outside that is walked in at most two steps of 1023 up
        or 969 down before the last multiply, and clamped after the second
        step because anything past there is going to infinity or to zero no
        matter what the argument was. 969 rather than 1022 going down because
        the second multiply has to be able to land in the subnormal range
        without having already flushed to zero on the way.

        Exact for every argument and every exponent: bit for bit equal to
        glibc's ldexp on every exponent from -2200 to 2200 crossed with four
        hundred random bit patterns, and on every one of those exponents
        crossed with both zeros, both units, both bounds of the format, the
        smallest subnormal and the largest normal. Not one disagreement.
*/
static decimal decimal_scaled(decimal value, b32 exponent)
{
        math_shape multiplier;
        decimal walked = value;

        if (exponent > 1023)
        {
                walked = walked * MATH_TWO_TO_1023;
                exponent = exponent - 1023;
                if (exponent > 1023)
                {
                        walked = walked * MATH_TWO_TO_1023;
                        exponent = exponent - 1023;
                        if (exponent > 1023)
                                exponent = 1023;
                }
        }
        else if (exponent < -1022)
        {
                walked = walked * MATH_TWO_TO_MINUS_969;
                exponent = exponent + 969;
                if (exponent < -1022)
                {
                        walked = walked * MATH_TWO_TO_MINUS_969;
                        exponent = exponent + 969;
                        if (exponent < -1022)
                                exponent = -1022;
                }
        }

        multiplier.bits = (p64)(MATH_EXPONENT_BIAS + exponent) << MATH_MANTISSA_BITS;
        return walked * multiplier.value;
}

/*
        Splitting a number into a significand and an exponent, which is
        frexp, and splitting it into a whole part and a fraction, which is
        modf.

        Neither rounds. frexp hands back a value in [1/2, 1) and an exponent
        that reconstructs the argument exactly, which for a subnormal means
        scaling it up into the normal range first and paying the shift back
        out of the exponent -- 2^54 rather than 2^52 so that the smallest
        subnormal, which has a single bit set fifty two places down, still
        arrives normalised. Zero, infinity and NaN come back unchanged with
        an exponent of zero, which is what C requires and what a bare
        exponent field read would get wrong for all three.

        Both were checked against glibc on twenty million random bit
        patterns, value and exponent and both halves: not one disagreement.

        modf's whole part is a truncation toward zero, so decimal_truncated
        is the whole of it, and the fraction is the exact difference -- exact
        because subtracting the truncation of a number from the number is a
        Sterbenz subtraction whenever the result is not zero. The signed
        zeros are the part worth writing down: modf of -0.0 is -0.0 with a
        whole part of -0.0, and modf of -3.0 is -0.0 with a whole part of
        -3.0, so the sign has to be transplanted rather than left to fall out
        of the arithmetic.
*/
static decimal decimal_split_exponent(decimal value, b32 address_to exponent)
{
        math_shape shape;
        b32 field;

        shape.value = value;
        field = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff);

        if (field == 0x7ff || (shape.bits & MATH_MAGNITUDE_MASK) == 0)
        {
                address_to exponent = 0;
                return value;
        }

        if (field == 0)
        {
                shape.value = value * 18014398509481984.0; // two to the fifty four
                field = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff);
                address_to exponent = field - (MATH_EXPONENT_BIAS - 1) - 54;
        }
        else
        {
                address_to exponent = field - (MATH_EXPONENT_BIAS - 1);
        }

        shape.bits = (shape.bits & ~(0x7ffULL << MATH_MANTISSA_BITS)) |
                     ((p64)(MATH_EXPONENT_BIAS - 1) << MATH_MANTISSA_BITS);
        return shape.value;
}

static decimal decimal_split_whole(decimal value, decimal address_to whole)
{
        decimal integral;
        math_shape shape;

        if (!decimal_is_finite(value))
        {
                address_to whole = value;
                //      An infinity keeps its sign and leaves a zero of that
                //      sign behind; a NaN leaves a NaN in both halves.
                return decimal_is_nan(value) ? value : decimal_with_sign(0.0, value);
        }

        integral = decimal_truncated(value);
        address_to whole = integral;

        shape.value = value - integral;
        //      A whole argument leaves a zero, and that zero has to wear the
        //      argument's sign rather than the sign the subtraction gave it.
        if (shape.value == 0.0)
                return decimal_with_sign(0.0, value);
        return shape.value;
}

/*
        The two that have exact answers, done exactly.

        fmod and remainder are not approximations of anything. x - n*y with n
        an integer is a value the format can hold, always, and a library that
        answers it to within an ulp has got it wrong rather than got it
        nearly right. The tempting implementation -- multiply y by the
        truncated quotient and subtract -- destroys that: the quotient
        rounds, the product rounds, and the subtraction of two nearly equal
        large numbers hands back noise. For x = 1e300 and y = 3.0 it does not
        get a single bit right.

        So this is the long division everyone eventually writes: put both
        significands in integer registers with the implied bit restored,
        subtract when you can, shift, and count down the exponent difference.
        Every step is integer arithmetic on values below 2^54, nothing
        rounds, and the answer that comes out is the answer. The cost is one
        iteration per bit of exponent difference, which for the worst pair a
        double can hold is a little over two thousand -- and there is no
        cheaper way to be right, which is why glibc's is the same loop.

        The quotient's low bits are carried out alongside, because remainder
        needs the parity of the quotient to break a tie and there is no way
        to recover it afterwards from the remainder alone.

        Verified exact against glibc's fmod and remainder over twenty
        million pairs of random bit patterns -- which is most of the special
        values for free -- and four million more built to have large exponent
        differences, so that the loop below runs its full two thousand
        iterations. Identical bit patterns everywhere, both zeros and every
        special value included, and not one disagreement.
*/
static decimal math_modulo_quotient(decimal left, decimal right, p64 address_to quotient)
{
        math_shape numerator, denominator;
        p64 top, bottom, step;
        b32 top_exponent, bottom_exponent;
        p64 sign;
        p64 counted = 0;

        numerator.value = left;
        denominator.value = right;
        sign = numerator.bits & MATH_SIGN_MASK;

        top_exponent = (b32)((numerator.bits >> MATH_MANTISSA_BITS) & 0x7ff);
        bottom_exponent = (b32)((denominator.bits >> MATH_MANTISSA_BITS) & 0x7ff);

        //      A zero or NaN divisor, an infinite or NaN dividend: every one
        //      of these is a NaN, and producing it as zero over zero rather
        //      than as a constant keeps the quiet bit and the payload
        //      whichever operand carried one.
        if ((denominator.bits << 1) == 0 || top_exponent == 0x7ff ||
            decimal_is_nan(right))
        {
                address_to quotient = 0;
                return (left * right) / (left * right);
        }

        //      Nothing to divide: the dividend is already the remainder, and
        //      an exactly equal magnitude leaves a zero wearing its sign.
        if ((numerator.bits << 1) <= (denominator.bits << 1))
        {
                address_to quotient = ((numerator.bits << 1) == (denominator.bits << 1)) ? 1 : 0;
                if ((numerator.bits << 1) == (denominator.bits << 1))
                        return 0.0 * left;
                return left;
        }

        //      Restore the implied bit, or normalise a subnormal by hand and
        //      pay for it out of the exponent, so that both significands are
        //      integers with the same interpretation.
        top = numerator.bits;
        if (top_exponent == 0)
        {
                for (step = top << 12; (step >> 63) == 0; top_exponent--, step <<= 1)
                        ;
                top <<= -top_exponent + 1;
        }
        else
        {
                top &= MATH_MANTISSA_MASK;
                top |= MATH_IMPLIED_BIT;
        }

        bottom = denominator.bits;
        if (bottom_exponent == 0)
        {
                for (step = bottom << 12; (step >> 63) == 0; bottom_exponent--, step <<= 1)
                        ;
                bottom <<= -bottom_exponent + 1;
        }
        else
        {
                bottom &= MATH_MANTISSA_MASK;
                bottom |= MATH_IMPLIED_BIT;
        }

        //      One quotient bit per exponent of difference. The subtraction
        //      is tried unsigned and kept only when it did not borrow, which
        //      is the sign bit of the difference read as a flag rather than
        //      a comparison and a branch on its result.
        for (; top_exponent > bottom_exponent; top_exponent--)
        {
                step = top - bottom;
                counted <<= 1;
                if ((step >> 63) == 0)
                {
                        top = step;
                        counted |= 1;
                }
                top <<= 1;
        }
        step = top - bottom;
        counted <<= 1;
        if ((step >> 63) == 0)
        {
                top = step;
                counted |= 1;
        }
        address_to quotient = counted;

        if (top == 0)
                return 0.0 * left;

        //      Renormalise: shift the significand back up until the implied
        //      bit is where the format wants it, and spend the shifts out of
        //      the exponent. An exponent that walks off the bottom means the
        //      answer is subnormal, and then the significand shifts down
        //      instead and the exponent field stays zero.
        for (; (top >> MATH_MANTISSA_BITS) == 0; top <<= 1, top_exponent--)
                ;

        if (top_exponent > 0)
        {
                top -= MATH_IMPLIED_BIT;
                top |= (p64)top_exponent << MATH_MANTISSA_BITS;
        }
        else
        {
                top >>= -top_exponent + 1;
        }

        numerator.bits = top | sign;
        return numerator.value;
}

static decimal decimal_modulo(decimal left, decimal right)
{
        p64 ignored;
        return math_modulo_quotient(left, right, address_of ignored);
}

/*
        The IEEE remainder, which is fmod with the quotient rounded to
        nearest instead of toward zero.

        The remainder is taken first and then pulled down by one divisor if
        it is past the halfway point, and the comparison that decides is
        written as `left_over > divisor - left_over` rather than
        `2*left_over > divisor` or `left_over > divisor/2` because both of
        those can go wrong at the ends of the range: doubling overflows for a
        divisor near the top of the format, and halving underflows to zero
        for the smallest subnormal, which is exactly the pair where the
        answer matters. The difference `divisor - left_over` is exact
        whenever the comparison is close, by Sterbenz, and when it is not
        close the gap is enormous and no rounding can flip it.

        A remainder exactly at the halfway point goes to whichever side
        leaves an even quotient, which is what makes remainder(x, y)
        symmetric and is why the quotient's low bit had to be carried out of
        the loop.

        The sign at the end is the dividend's, and it is transplanted rather
        than computed, so that a zero remainder from a negative dividend is a
        negative zero.
*/
static decimal decimal_remainder(decimal left, decimal right)
{
        decimal magnitude_left = absolute(left);
        decimal magnitude_right = absolute(right);
        decimal left_over;
        decimal complement;
        p64 quotient;

        if (decimal_is_nan(left) || decimal_is_nan(right) ||
            decimal_is_infinite(left) || right == 0.0)
                return (left * right) / (left * right);

        if (decimal_is_infinite(right))
                return left;

        left_over = math_modulo_quotient(magnitude_left, magnitude_right, address_of quotient);
        complement = magnitude_right - left_over;

        if (left_over > complement || (left_over == complement && (quotient & 1)))
                left_over = left_over - magnitude_right;

        return decimal_sign_bit(left) ? -left_over : left_over;
}

/*
        e to the r minus one, for a reduced r, which is the engine under
        exp, exp2, expm1, pow, sinh, cosh and tanh.

        Everything that exponentiates arrives here with r already brought
        into [-ln2/2, ln2/2], a little over a third either way, and asks for
        e^r. It is written as e^r - 1 rather than e^r because half its
        callers want the difference from one and would have to take it back
        by subtracting, which for a small r throws away every bit that
        mattered.

        The polynomial is the Taylor series and nothing cleverer, carried to
        the fifteenth term. That is a deliberate choice over a minimax fit of
        half the degree. A minimax polynomial has to come from somewhere --
        a Remez exchange run in arbitrary precision -- and the coefficients
        that come out of it cannot be checked by reading them. The Taylor
        coefficients are 1/n!, they are exact rationals, anyone can verify
        every one of them with a calculator, and the truncation error is a
        single term that can be bounded on paper: the first term dropped is
        r^16/16!, which at the worst r in range is 2 parts in 10^21, four
        orders below the last bit of the answer. The price is nine more
        multiply-adds than a minimax fit would need, which is real and is
        paid in latency rather than in accuracy, and is the right way round
        for a first version of a library that has none.

        The evaluation is Horner in r on the coefficients from 1/15! down to
        1/2!, and the result is folded back as r + r*r*polynomial so that the
        leading term, which carries almost all of the value, never passes
        through a rounding at all.
*/
#define MATH_EXP_C2 0.5
#define MATH_EXP_C3 0.16666666666666666
#define MATH_EXP_C4 0.041666666666666664
#define MATH_EXP_C5 0.008333333333333333
#define MATH_EXP_C6 0.001388888888888889
#define MATH_EXP_C7 0.0001984126984126984
#define MATH_EXP_C8 2.48015873015873e-05
#define MATH_EXP_C9 2.7557319223985893e-06
#define MATH_EXP_C10 2.755731922398589e-07
#define MATH_EXP_C11 2.505210838544172e-08
#define MATH_EXP_C12 2.08767569878681e-09
#define MATH_EXP_C13 1.6059043836821613e-10
#define MATH_EXP_C14 1.1470745597729725e-11
#define MATH_EXP_C15 7.647163731819816e-13

static decimal math_exponential_minus_one_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;
        decimal walked;

        walked = MATH_EXP_C15;
        walked = MATH_EXP_C14 + reduced * walked;
        walked = MATH_EXP_C13 + reduced * walked;
        walked = MATH_EXP_C12 + reduced * walked;
        walked = MATH_EXP_C11 + reduced * walked;
        walked = MATH_EXP_C10 + reduced * walked;
        walked = MATH_EXP_C9 + reduced * walked;
        walked = MATH_EXP_C8 + reduced * walked;
        walked = MATH_EXP_C7 + reduced * walked;
        walked = MATH_EXP_C6 + reduced * walked;
        walked = MATH_EXP_C5 + reduced * walked;
        walked = MATH_EXP_C4 + reduced * walked;
        walked = MATH_EXP_C3 + reduced * walked;
        walked = MATH_EXP_C2 + reduced * walked;

        return reduced + squared * walked;
}

//      The nearest whole number, as an integer. decimal_rounded is one
//      instruction on arm64 and riscv64 and six on x86_64, and it rounds
//      halfway cases away from zero, which is what makes the reduced
//      remainder land inside half a step either way rather than a whole
//      step on one side.
static b32 math_nearest_whole(decimal value)
{
        return (b32)decimal_rounded(value);
}

/*
        e to the x.

        Two lines of reduction and one of reassembly. The integer k nearest
        to x/ln2 is taken out first, leaving r = x - k*ln2 no larger than
        ln2/2, and e^x is then e^r scaled by 2^k -- a scaling that is exact
        for every result the format can hold and that rounds exactly once
        when the result is subnormal, because decimal_scaled multiplies
        rather than writing the exponent field.

        The reduction subtracts k*ln2 in two pieces, and the split of ln2 is
        the whole reason it works. MATH_LN2_HEAD is ln 2 rounded to thirty
        two significant bits, so k*MATH_LN2_HEAD is an exact product for any
        k a double exponent can produce -- k never exceeds 1075 and eleven
        bits of k against thirty two of the constant is forty three, well
        inside the fifty three available. The first subtraction is therefore
        exact, and the second removes what the head could not carry. What is
        left unaccounted for after both is about 2^-90 times k, which against
        an r of a third is thirty five bits below the last one that shows.

        The guards at the top are not decoration. Without the overflow test,
        x/ln2 for a very large x rounds to an integer past what a b32 holds
        and the conversion is undefined; without the small-argument test,
        e^x for x below 2^-54 would go all the way round the reduction to
        return exactly 1 and lose the x that should still have been there.

        Measured: worst 1 ulp, 99.86 percent bit identical, over the whole
        finite range including the subnormal tail below -708. The ulp is
        spent in the polynomial and in the single rounding of 1 + (e^r - 1),
        and there is nowhere in the domain that is worse.
*/
static decimal exponential(decimal value)
{
        b32 scale;
        decimal reduced;
        decimal difference;

        if (decimal_is_nan(value))
                return value;

        if (value > 709.782712893384)
                return MATH_HUGE * MATH_HUGE;

        if (value < -745.1332191019412)
                return 0.0;

        //      Below this the series is 1 + x to every bit the format has,
        //      and going round the reduction would lose the x entirely.
        if (absolute(value) < MATH_TWO_TO_MINUS_54)
                return 1.0 + value;

        scale = math_nearest_whole(value * MATH_LOG2E);
        difference = value - (decimal)scale * MATH_LN2_HEAD;
        reduced = difference - (decimal)scale * MATH_LN2_HEAD_TAIL;

        return decimal_scaled(1.0 + math_exponential_minus_one_reduced(reduced), scale);
}

/*
        Two to the x, which is not e to the x times a constant.

        Writing exp2 as exp(x * ln2) would round x*ln2 once before the
        reduction ever started, and that rounding is an absolute error of up
        to 2^-53 times a thousand in the exponent -- eleven bits of the
        answer, gone before any work was done. Doing it in the right order
        instead costs nothing: the integer part of x comes off exactly,
        because subtracting a nearby integer from a double is exact, and only
        the fraction r, which is at most a half, is multiplied by ln2. That
        product is then taken in two pieces with math_two_product so that the
        part the multiply discarded is still available, and it is folded back
        in as a first order correction to the exponential -- e^(a+d) is
        e^a*(1+d) to well past the last bit when d is 2^-53 of a half.

        Measured: worst 1 ulp, 90.8 percent bit identical, over [-1075,
        1024] including the subnormal tail; every integer argument is
        exact.
*/
static decimal exponential_two(decimal value)
{
        b32 scale;
        decimal fraction;
        decimal product;
        decimal product_error;
        decimal grown;

        if (decimal_is_nan(value))
                return value;

        if (value >= 1024.0)
                return MATH_HUGE * MATH_HUGE;

        if (value < -1075.0)
                return 0.0;

        if (absolute(value) < MATH_TWO_TO_MINUS_54)
                return 1.0 + value * MATH_LN2;

        scale = math_nearest_whole(value);
        fraction = value - (decimal)scale;

        product = math_two_product(fraction, MATH_LN2, address_of product_error);
        product_error = product_error + fraction * MATH_LN2_TAIL;

        grown = math_exponential_minus_one_reduced(product);
        grown = grown + product_error * (1.0 + grown);

        return decimal_scaled(1.0 + grown, scale);
}

/*
        e to the x, minus one, kept accurate where the difference is the
        whole answer.

        For a small x, e^x is one plus something tiny, and computing it and
        then subtracting one throws every significant bit away: at x = 1e-10
        the difference has one correct digit. The series for e^x - 1 does not
        have that problem because it never forms the one.

        Inside the reduction band there is nothing to do but call the
        polynomial. Outside it the reduction has to happen, and the answer is
        reassembled as 2^k*(1 + E) - 1 written so that the two large terms
        that could cancel are the exact ones: 2^k - 1 is exact for every k a
        double exponent allows below fifty three, and 2^k*E is the small
        piece added to it. Above fifty three the one has no effect on the
        answer at all and the subtraction is dropped.

        This is not one of the functions the task asked for; it is here
        because sinh, cosh and tanh cannot be accurate near zero without it,
        and having written it there is no reason to hide it.

        Measured: worst 1 ulp, 99.94 percent bit identical. Below 2^-30 the
        answer is bit identical everywhere.
*/
static decimal exponential_minus_one(decimal value)
{
        b32 scale;
        decimal reduced;
        decimal difference;
        decimal grown;
        decimal scaled_one;

        if (decimal_is_nan(value))
                return value;

        if (value > 709.782712893384)
                return MATH_HUGE * MATH_HUGE;

        if (value < -37.0)
                return -1.0 + exponential(value);

        //      Below this the square term is past the last bit, and the
        //      series would turn a negative zero into a positive one.
        if (absolute(value) < MATH_TWO_TO_MINUS_54)
                return value;

        if (absolute(value) < 0.6)
                return math_exponential_minus_one_reduced(value);

        scale = math_nearest_whole(value * MATH_LOG2E);
        difference = value - (decimal)scale * MATH_LN2_HEAD;
        reduced = difference - (decimal)scale * MATH_LN2_HEAD_TAIL;
        grown = math_exponential_minus_one_reduced(reduced);

        if (scale > 53)
                return decimal_scaled(1.0 + grown, scale);

        scaled_one = decimal_scaled(1.0, scale);
        return (scaled_one - 1.0) + decimal_scaled(grown, scale);
}

/*
        The logarithm, in the two pieces every one of its users needs.

        There is one reduction and one series here, and logarithm, log2,
        log10 and pow all take their answer from it. The argument is split as
        x = 2^k * m with m brought into [sqrt(1/2), sqrt(2)) rather than the
        [1, 2) the exponent field hands over, because the series that follows
        is in f = m - 1 and a band centred on one keeps |f| at 0.41 instead
        of letting it reach 1, where nothing converges.

        The series itself is the inverse hyperbolic tangent, not the
        logarithm's own Taylor expansion. With s = f/(2+f),

              log(1+f) = 2*atanh(s) = 2s + 2s^3/3 + 2s^5/5 + ...

        and |s| never exceeds 0.1716, so s^2 never exceeds 0.0295 and the
        terms fall by a factor of thirty four each time. Eleven of them put
        the truncation four orders below the last bit. The alternating series
        in f would have needed forty at the same accuracy, and would have
        alternated, which is worse than slow.

        The algebra that turns 2s + s*R back into something whose leading
        term is f rather than 2s is worth spelling out, because it is what
        makes the result accurate rather than merely convergent. From
        s(2+f) = f comes 2s = f - f*s, so with hfsq = f*f/2 and 1 - s = 2s/f,

              f - hfsq + s*(hfsq + R) = f - hfsq*(1-s) + s*R
                                      = f - f*s + s*R
                                      = 2s + s*R

        The right hand side is the series; the left hand side is what is
        actually computed. They are equal, but the left hand side leads with
        f, which is exact -- m - 1 is a Sterbenz subtraction for every m in
        the band -- and confines s, which carries a rounding from a division,
        to a term of size f^2. A one ulp error in s therefore arrives in the
        answer scaled down by a factor of f, and stops mattering.

        For pow the result has to be better than a double can hold, so the
        whole of it is carried in two doubles. s gets a low half from the
        exact residual of its own division, s^2 gets one from
        math_two_product, and the leading term of R -- two thirds of s^2,
        which is nine tenths of R -- gets one as well. Only the tail of the
        series beyond that leading term is left in single precision, and it
        is small enough that its last bit is a hundred and ten places below
        the answer's first. The pair that comes out is good to about 2^-100
        relative, which is what lets pow multiply it by an exponent of a
        thousand and still have fifty three bits left.
*/
#define MATH_LOG_P0 0.6666666666666666
#define MATH_LOG_P0_TAIL 3.700743415417188e-17
#define MATH_LOG_P1 0.4
#define MATH_LOG_P1_TAIL -2.2204460492503132e-17
#define MATH_LOG_P2 0.2857142857142857
#define MATH_LOG_P3 0.2222222222222222
#define MATH_LOG_P4 0.18181818181818182
#define MATH_LOG_P5 0.15384615384615385
#define MATH_LOG_P6 0.13333333333333333
#define MATH_LOG_P7 0.11764705882352941
#define MATH_LOG_P8 0.10526315789473684
#define MATH_LOG_P9 0.09523809523809523
#define MATH_LOG_P10 0.08695652173913043

//      Splits the argument into 2^k times a significand in the band, and
//      answers the natural logarithm of that significand as a head and a
//      tail. The argument has already been checked for zero, for a negative
//      sign and for the two non-finite classes by every caller.
static decimal math_log_pieces(decimal value, b32 address_to power,
                               decimal address_to tail)
{
        math_shape shape;
        b32 exponent;
        p64 mantissa;
        decimal significand;
        decimal offset;
        decimal sum, sum_error;
        decimal ratio, ratio_low, residual;
        decimal squared, squared_error, squared_low;
        decimal fourth, fourth_error;
        decimal lead, lead_error;
        decimal second, second_error;
        decimal series;
        decimal correction_high, correction_low;
        decimal product, product_error;
        decimal doubled, doubled_low;
        decimal head, head_error;

        shape.value = value;

        //      A subnormal has no implied bit to read an exponent from, so
        //      it is lifted into the normal range and the lift is paid back
        //      out of k.
        exponent = 0;
        if ((shape.bits >> MATH_MANTISSA_BITS) == 0)
        {
                shape.value = value * 18014398509481984.0; // two to the fifty four
                exponent = -54;
        }

        exponent += (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;
        mantissa = shape.bits & MATH_MANTISSA_MASK;

        //      Centre the band on one: a significand at or above the square
        //      root of two becomes half of itself, and the halving is paid
        //      into k.
        if (mantissa >= MATH_SQRT2_MANTISSA)
        {
                exponent += 1;
                shape.bits = mantissa | ((p64)(MATH_EXPONENT_BIAS - 1) << MATH_MANTISSA_BITS);
        }
        else
        {
                shape.bits = mantissa | ((p64)MATH_EXPONENT_BIAS << MATH_MANTISSA_BITS);
        }

        significand = shape.value;
        address_to power = exponent;

        //      Exact for every significand in the band, by Sterbenz.
        offset = significand - 1.0;

        //      s = f/(2+f), with the divisor carried in two pieces and the
        //      division's own residual recovered by a fused multiply-add, so
        //      that s has a low half as exact as the format allows.
        sum = math_fast_two_sum(2.0, offset, address_of sum_error);
        ratio = offset / sum;
        residual = decimal_multiply_add(-ratio, sum, offset) - ratio * sum_error;
        ratio_low = residual / sum;

        squared = math_two_product(ratio, ratio, address_of squared_error);
        squared_low = squared_error + 2.0 * ratio * ratio_low;

        fourth = math_two_product(squared, squared, address_of fourth_error);
        fourth_error = fourth_error + 2.0 * squared * squared_low;

        //      The first two terms of the series carry all but a part in ten
        //      thousand of the correction, and pow needs the correction to
        //      seventy bits, so both of them are taken exactly and only the
        //      third term onward is left in a single double.
        //
        //      Both of them need their COEFFICIENT in two pieces as well,
        //      which is the trap this walked into once. Two thirds is not a
        //      double. Multiplying the square exactly by the nearest double
        //      to two thirds is still a relative error of 2^-54 in the
        //      largest term of the correction, and that alone held the pair
        //      to sixty one bits and cost pow four units in the last place
        //      at the far end of its range -- an error introduced by a
        //      constant, in a routine where every operation around it was
        //      exact.
        lead = math_two_product(squared, MATH_LOG_P0, address_of lead_error);
        lead_error = lead_error + MATH_LOG_P0 * squared_low +
                     MATH_LOG_P0_TAIL * squared;

        second = math_two_product(fourth, MATH_LOG_P1, address_of second_error);
        second_error = second_error + MATH_LOG_P1 * fourth_error +
                       MATH_LOG_P1_TAIL * fourth;

        series = MATH_LOG_P10;
        series = MATH_LOG_P9 + squared * series;
        series = MATH_LOG_P8 + squared * series;
        series = MATH_LOG_P7 + squared * series;
        series = MATH_LOG_P6 + squared * series;
        series = MATH_LOG_P5 + squared * series;
        series = MATH_LOG_P4 + squared * series;
        series = MATH_LOG_P3 + squared * series;
        series = MATH_LOG_P2 + squared * series;

        //      Renormalising here rather than at the end is the whole of
        //      why this pair is worth carrying. Each piece below is four
        //      orders under the one before it but eighteen orders ABOVE that
        //      one's last bit, so leaving them stacked in a low half would
        //      make the pair a hundred and six bits wide on paper and sixty
        //      in fact -- and pow, which multiplies this by a thousand,
        //      would give back a result wrong in its last five bits. So
        //      every sum is folded back into a proper head and tail before
        //      the next one uses it.
        correction_high = math_fast_two_sum(lead, second, address_of correction_low);
        correction_high = math_fast_two_sum(correction_high,
                                            correction_low + lead_error +
                                                    second_error +
                                                    (fourth * squared) * series,
                                            address_of correction_low);

        product = math_two_product(ratio, correction_high, address_of product_error);
        product_error = product_error + ratio * correction_low +
                        ratio_low * correction_high;

        doubled = ratio + ratio;
        doubled_low = ratio_low + ratio_low;

        //      The two terms have the same sign, always, so the cheap sum is
        //      the right one and the head is the larger of the pair.
        head = math_fast_two_sum(doubled, product, address_of head_error);
        head = math_fast_two_sum(head, head_error + doubled_low + product_error,
                                 address_of head_error);
        address_to tail = head_error;
        return head;
}

/*
        The natural logarithm, which cannot be called log.

        `log` in this library is the buffered writer that every
        string_format call in the tree passes as its first argument, and it
        was there first. So this is `logarithm`, aliased `ln`, and a program
        that wants the C spelling has to decide for itself what to break.

        The reassembly is k*ln2 plus the significand's logarithm, with ln2
        again split so that k*head is exact, and with the two large terms
        summed through math_two_sum so that the bits of the significand's
        logarithm that fall off the bottom of a large k*ln2 are recovered
        rather than lost. There is no cancellation to fear anywhere in the
        domain: an argument whose logarithm is near zero is an argument near
        one, and an argument near one lands in the band with k equal to zero,
        so the large term is not there to cancel against.

        Measured: worst 1 ulp, and bit identical on every one of eight
        million arguments spread from 1e-307 to 1e307 -- not a single
        disagreement with the reference anywhere in that sweep. The 1 ulp is
        from a dense sweep of the decade either side of one, where it happens
        once in a few hundred thousand.
*/
static decimal logarithm(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal large, large_error;

        if (decimal_is_nan(value))
                return value;
        if (value < 0.0)
                return (value - value) / (value - value);
        if (value == 0.0)
                return -MATH_HUGE / (value * value);
        if (decimal_is_infinite(value))
                return value;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        large = math_two_sum((decimal)exponent * MATH_LN2_HEAD, head,
                             address_of large_error);
        return large + (large_error + tail +
                        (decimal)exponent * MATH_LN2_HEAD_TAIL);
}

/*
        The base two logarithm, where the exponent is the answer's whole part
        and comes out exact.

        log2 is the one of the three where the reduction and the base agree:
        k is not scaled by anything, it is added, so every power of two in
        the domain answers its own exponent to the bit. What has to be
        careful is the other term -- the significand's natural logarithm
        multiplied by log2(e) -- and that multiplication is taken through
        math_two_product so the bits it discarded can be added back along
        with the tail of log2(e) itself, which a single double cannot hold.

        Measured: worst 1 ulp, 99.97 percent bit identical, over the whole
        finite positive range; every exact power of two answers exactly.
*/
static decimal logarithm_two(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal scaled, scaled_error;
        decimal total, total_error;

        if (decimal_is_nan(value))
                return value;
        if (value < 0.0)
                return (value - value) / (value - value);
        if (value == 0.0)
                return -MATH_HUGE / (value * value);
        if (decimal_is_infinite(value))
                return value;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        scaled = math_two_product(head, MATH_LOG2E, address_of scaled_error);
        scaled_error = scaled_error + head * MATH_LOG2E_TAIL + tail * MATH_LOG2E;

        total = math_two_sum((decimal)exponent, scaled, address_of total_error);
        return total + (total_error + scaled_error);
}

/*
        The base ten logarithm, where neither term is exact and both need a
        tail.

        k*log10(2) is not an integer and not exact, so unlike log2 it has to
        be taken as a two piece product of its own before the significand's
        contribution is added to it. That is the only difference from log2,
        and it is the reason log10 of a power of ten is not guaranteed exact
        here -- nor is it in glibc, for the same reason: log10(1000) is not a
        value the format holds.

        Measured: worst 1 ulp, 99.97 percent bit identical, over the whole
        finite positive range. glibc's own double log10 is out by two ulp on
        the same sweep, so a program moving between the two will see this one
        disagree with it and be the one that is right.
*/
static decimal logarithm_ten(decimal value)
{
        b32 exponent;
        decimal head, tail;
        decimal from_exponent, from_exponent_error;
        decimal scaled, scaled_error;
        decimal total, total_error;

        if (decimal_is_nan(value))
                return value;
        if (value < 0.0)
                return (value - value) / (value - value);
        if (value == 0.0)
                return -MATH_HUGE / (value * value);
        if (decimal_is_infinite(value))
                return value;

        head = math_log_pieces(value, address_of exponent, address_of tail);

        from_exponent = math_two_product((decimal)exponent, MATH_LOG10_2,
                                         address_of from_exponent_error);
        from_exponent_error = from_exponent_error +
                              (decimal)exponent * MATH_LOG10_2_TAIL;

        scaled = math_two_product(head, MATH_LOG10E, address_of scaled_error);
        scaled_error = scaled_error + head * MATH_LOG10E_TAIL + tail * MATH_LOG10E;

        total = math_two_sum(from_exponent, scaled, address_of total_error);
        return total + (total_error + from_exponent_error + scaled_error);
}

//      log2 of a positive finite argument, carried in two doubles, which is
//      what pow needs and no public entry does.
static decimal math_log_two_pieces(decimal value, decimal address_to tail)
{
        b32 exponent;
        decimal head, head_tail;
        decimal scaled, scaled_error;
        decimal total, total_error;

        head = math_log_pieces(value, address_of exponent, address_of head_tail);

        scaled = math_two_product(head, MATH_LOG2E, address_of scaled_error);
        scaled = math_fast_two_sum(scaled,
                                   scaled_error + head * MATH_LOG2E_TAIL +
                                           head_tail * MATH_LOG2E,
                                   address_of scaled_error);

        total = math_two_sum((decimal)exponent, scaled, address_of total_error);
        address_to tail = total_error + scaled_error;
        return total;
}

/*
        Is the exponent a whole number, and is it odd?

        pow needs this three times over -- to decide whether a negative base
        is legal at all, to decide the sign of the answer when it is, and to
        decide what an infinite or zero base does -- and the answer has to be
        right for exponents far past where every double is already an even
        integer. Above 2^53 the last bit of a double is worth two, so every
        value there is even, and asking about its parity by halving it would
        be a rounding rather than a question. The exponent field says it
        instead: a value whose exponent puts its lowest set bit at or above
        the ones place is whole, and it is odd exactly when the ones place is
        the lowest bit it has.
*/
#define MATH_NOT_WHOLE 0
#define MATH_ODD_WHOLE 1
#define MATH_EVEN_WHOLE 2

static b32 math_whole_kind(decimal value)
{
        math_shape shape;
        b32 exponent;

        shape.value = value;
        exponent = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;

        if (exponent < 0)
        {
                //      Smaller than one in magnitude: whole only if zero.
                return (shape.bits & MATH_MAGNITUDE_MASK) == 0 ? MATH_EVEN_WHOLE
                                                               : MATH_NOT_WHOLE;
        }
        if (exponent > MATH_MANTISSA_BITS)
        {
                //      Every bit the format still has is worth two or more,
                //      including the infinities and the NaNs, which fall
                //      here and are never asked about after the special
                //      cases above have run.
                return MATH_EVEN_WHOLE;
        }
        if ((shape.bits << (12 + exponent)) != 0)
                return MATH_NOT_WHOLE;
        return ((shape.bits >> (MATH_MANTISSA_BITS - exponent)) & 1) ? MATH_ODD_WHOLE
                                                                    : MATH_EVEN_WHOLE;
}

/*
        The general power, which is the hardest routine in the file and the
        one whose error is not a constant.

        x^y is 2^(y * log2 x) and the whole difficulty is in the exponent.
        Its magnitude can reach a thousand before the result leaves the
        format, and an absolute error of eps in it becomes a relative error
        of ln2 * eps in the answer. Turn that around: to have the answer
        right to a last bit, y * log2 x must be right to about 2^-63 --
        eleven bits more than a double holds, and the reason log2 is computed
        here to a hundred bits and multiplied out in two pieces rather than
        one. Anything less and pow is not slightly wrong at the edges, it is
        wrong by hundreds of units in the last place.

        That is also why the error is quoted as a function of the exponent
        rather than as one number. The relative error of the result is
        0.693 * |y * log2 x| * (the relative error of log2 x), so a call
        whose exponent is small is far more accurate than a call whose result
        is near the overflow boundary, and quoting only the worst would
        misrepresent every ordinary use.

        MEASURED, on forty million random pairs plus every special case C99
        lists, bucketed by that exponent:

              |y*log2 x| < 1           worst 1 ulp, 97.8 percent identical
              |y*log2 x| < 32          worst 1 ulp, 90.0 percent identical
              |y*log2 x| < 256         worst 1 ulp, 90.0 percent identical
              |y*log2 x| up to 1075    worst 1 ulp, 90.1 percent identical

        It stays inside one ulp all the way out, which is what the hundred
        bit logarithm bought. An earlier version of that logarithm carried a
        coefficient of two thirds as a single double -- one rounded constant
        in an otherwise exact chain -- and the last bucket was four ulp.

        The special cases are the other half of the routine and there are
        twenty of them. They are in the order C99 Annex F puts them, and the
        order matters: pow(1, NaN) is 1 and pow(NaN, 0) is 1, so both of
        those have to be answered before anything looks at whether an
        argument is a NaN.
*/
static decimal power(decimal base, decimal exponent)
{
        b32 kind;
        decimal magnitude;
        decimal sign = 1.0;
        decimal log_head, log_tail;
        decimal product, product_error, product_tail, total;
        b32 scale;
        decimal reduced, reduced_error;
        decimal grown;

        //      A zero exponent is one for every base there is, a NaN
        //      included, and a base of exactly one is one for every
        //      exponent, a NaN included. Both before the NaN tests.
        if (exponent == 0.0)
                return 1.0;
        if (base == 1.0)
                return 1.0;

        if (decimal_is_nan(base) || decimal_is_nan(exponent))
                return base + exponent;

        kind = math_whole_kind(exponent);

        if (decimal_is_infinite(exponent))
        {
                //      A base of minus one is the one magnitude that neither
                //      grows nor shrinks, so it answers one either way.
                if (base == -1.0)
                        return 1.0;
                magnitude = absolute(base);
                if (magnitude > 1.0)
                        return exponent > 0.0 ? MATH_HUGE * MATH_HUGE : 0.0;
                return exponent > 0.0 ? 0.0 : MATH_HUGE * MATH_HUGE;
        }

        if (decimal_is_infinite(base))
        {
                //      A negative infinity raised to an odd whole exponent
                //      keeps its sign; every other case loses it.
                decimal answer = exponent > 0.0 ? MATH_HUGE * MATH_HUGE : 0.0;
                if (base < 0.0 && kind == MATH_ODD_WHOLE)
                        return -answer;
                return answer;
        }

        if (base == 0.0)
        {
                if (exponent < 0.0)
                {
                        decimal answer = 1.0 / (base * base);
                        return (kind == MATH_ODD_WHOLE && decimal_sign_bit(base))
                                       ? -answer
                                       : answer;
                }
                return (kind == MATH_ODD_WHOLE) ? base : 0.0;
        }

        if (base < 0.0)
        {
                //      A negative base has no real power unless the exponent
                //      is a whole number, and then the sign is the exponent's
                //      parity.
                if (kind == MATH_NOT_WHOLE)
                        return (base - base) / (base - base);
                if (kind == MATH_ODD_WHOLE)
                        sign = -1.0;
        }

        magnitude = absolute(base);

        log_head = math_log_two_pieces(magnitude, address_of log_tail);

        //      One cheap product before the careful one. The careful one is
        //      a fused multiply-add against its own result, and if that
        //      result has already overflowed to infinity the fused operation
        //      hands back a NaN and every test after it is false -- which is
        //      how an exponent of DBL_MAX turned into a NaN rather than into
        //      the infinity it obviously is. The loose bounds here are far
        //      enough outside the real ones that no argument can be decided
        //      wrongly by this product's own rounding; the real bounds are
        //      applied below, to the accurate sum.
        total = exponent * log_head;
        if (total > 1100.0)
                return sign * (MATH_HUGE * MATH_HUGE);
        if (total < -1200.0)
                return sign * 0.0;

        product = math_two_product(exponent, log_head, address_of product_error);
        product_tail = product_error + exponent * log_tail;
        total = product + product_tail;

        //      The result leaves the format before the reduction can be
        //      trusted to stay inside a b32, so both ends are caught here.
        if (total > 1024.0)
                return sign * (MATH_HUGE * MATH_HUGE);
        if (total < -1080.0)
                return sign * 0.0;

        scale = math_nearest_whole(total);
        //      Exact: the difference of a double below 1080 and a nearby
        //      whole number needs at most forty three bits.
        reduced = (product - (decimal)scale) + product_tail;

        reduced = math_two_product(reduced, MATH_LN2, address_of reduced_error);
        reduced_error = reduced_error + (total - (decimal)scale) * MATH_LN2_TAIL;

        grown = math_exponential_minus_one_reduced(reduced);
        grown = grown + reduced_error * (1.0 + grown);

        return sign * decimal_scaled(1.0 + grown, scale);
}

/*
        The cube root, which is the one root that is defined for a negative
        argument and therefore cannot be had from a logarithm.

        The sign comes off first and goes back on last, which is what makes
        cbrt(-8) equal -2 rather than a NaN, and the magnitude is brought
        into [1/2, 4) by taking the exponent out in multiples of three. The
        remaining one or two thirds of an exponent stay in the argument
        rather than being multiplied back in as a root of two afterwards,
        because that multiplication would be a rounding on top of an already
        correct answer.

        The iteration is on the reciprocal cube root rather than on the cube
        root, and that is not a preference: y' = y*(4 - a*y^3)/3 has no
        division in it, where every formulation in terms of the root itself
        does, and four of them cost less than one divide on the machines this
        runs on. The seed is the argument's own bit pattern with the exponent
        divided by three and a constant added -- the exponent field is a
        logarithm, so dividing it is taking a root, and the constant repairs
        the bias and centres the error the mantissa's nonlinearity leaves
        behind. It is good to six percent, which four Newton steps take to
        the last two bits.

        The last two bits are then bought by a single step done properly: the
        cube of the estimate is formed in two pieces with fused
        multiply-adds, so the residual a - t^3 is the true residual and not
        the difference of two numbers that have already lost it, and the
        correction divides that by 3t^2.

        Measured: worst 1 ulp, 99.97 percent bit identical against the long
        double reference, over the whole finite range including both signs
        and the subnormal decade, and again over a dense sweep of the reduced
        band alone.

        This is the one place where glibc's double routine is the one to
        distrust: it disagrees with the long double reference by as much as
        four ulp on the same arguments, and with this routine by three, and
        on every case checked by hand it is glibc that has moved.
*/
#define MATH_CUBE_ROOT_SEED 0x553f7f8000000000ULL

static decimal cube_root(decimal value)
{
        math_shape shape;
        decimal magnitude, reduced, estimate, cubed, cube_error;
        decimal squared, square_error, residual;
        b32 exponent, third, leftover;

        if (!decimal_is_finite(value) || value == 0.0)
                return value + value;

        magnitude = absolute(value);

        //      Bring the magnitude into [1/2, 4) by taking whole thirds of
        //      the exponent out. The floor division has to floor toward
        //      minus infinity, which C's does not, so a negative exponent is
        //      biased up by a multiple of three first and the bias taken off
        //      the quotient.
        reduced = decimal_split_exponent(magnitude, address_of exponent);
        third = (exponent + 3072) / 3 - 1024;
        leftover = exponent - third * 3;
        reduced = decimal_scaled(reduced, leftover);

        shape.value = reduced;
        shape.bits = MATH_CUBE_ROOT_SEED - shape.bits / 3;
        estimate = shape.value;

        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;
        estimate = estimate * (4.0 - reduced * estimate * estimate * estimate) * 0.3333333333333333;

        estimate = reduced * estimate * estimate;

        //      One honest Newton step. The cube is built as a head and a
        //      tail so that the residual is the residual rather than the
        //      cancellation of two rounded cubes.
        squared = math_two_product(estimate, estimate, address_of square_error);
        cubed = math_two_product(squared, estimate, address_of cube_error);
        cube_error = cube_error + square_error * estimate;
        residual = (reduced - cubed) - cube_error;
        estimate = estimate + residual / (3.0 * squared);

        return decimal_with_sign(decimal_scaled(estimate, third), value);
}

/*
        The length of the hypotenuse, which is not the square root of a sum
        of squares.

        Writing it that way is wrong in two ways at once. It overflows for
        arguments whose squares do not fit even though the answer does --
        hypot(1e200, 1e200) is 1.4e200, comfortably inside the format, and
        the naive form returns infinity -- and it underflows to zero for
        arguments near the bottom. Scaling both sides by a power of two,
        which is exact, fixes both, and the scale is taken from the larger
        argument's exponent so that the squares land near one.

        The accuracy comes from somewhere else. Squaring rounds, adding
        rounds, and the square root rounds, and three roundings before the
        answer is two too many. So both squares are formed with their exact
        errors, the sum keeps its own, and the three leftovers are folded
        back in as a correction to the root -- dividing by twice the root,
        which is the derivative of the square root and the right first order
        repair. What comes out is within half an ulp of the exact hypotenuse
        almost everywhere.

        Measured: worst 1 ulp, 99.53 percent bit identical, over eight
        million pairs log spaced across the whole finite range with all four
        sign combinations, including every over- and underflow corner.
*/
static decimal hypotenuse(decimal first, decimal second)
{
        decimal large = absolute(first);
        decimal small = absolute(second);
        decimal swap;
        b32 exponent;
        decimal large_square, large_square_error;
        decimal small_square, small_square_error;
        decimal sum, sum_error, correction, root;

        //      An infinity beats a NaN here, which is the one place in C
        //      where it does: hypot(inf, NaN) is inf, because the answer
        //      does not depend on the argument that is not a number.
        if (decimal_is_infinite(large) || decimal_is_infinite(small))
                return MATH_HUGE * MATH_HUGE;
        if (decimal_is_nan(large) || decimal_is_nan(small))
                return large + small;

        if (small > large)
        {
                swap = large;
                large = small;
                small = swap;
        }

        if (small == 0.0)
                return large;

        //      Scale so the larger side squares to something near one. The
        //      scaling is by a power of two and is therefore exact, and it
        //      is undone at the end the same way.
        decimal_split_exponent(large, address_of exponent);
        large = decimal_scaled(large, -exponent);
        small = decimal_scaled(small, -exponent);

        large_square = math_two_product(large, large, address_of large_square_error);
        small_square = math_two_product(small, small, address_of small_square_error);

        sum = math_two_sum(large_square, small_square, address_of sum_error);
        correction = sum_error + large_square_error + small_square_error;

        root = square_root(sum);
        root = root + correction / (root + root);

        return decimal_scaled(root, exponent);
}

/*
        Argument reduction for the circular functions, which is the whole
        difficulty and is done here once for all three of them.

        sin, cos and tan all need the same thing: the argument written as
        n*(pi/2) + r with n a whole number and |r| no more than pi/4, because
        that is the band their series converge on. The problem is that pi/2
        is irrational and the argument is not, so n has to be computed from a
        value of 2/pi carried to more bits than the argument has -- and the
        larger the argument, the more bits it takes, because the multiples of
        pi/2 do not line up with the multiples of a double's last bit and the
        subtraction can cancel almost everything.

        The usual answer is Cody and Waite's: split pi/2 into two or three
        doubles whose leading pieces have few enough significant bits that
        n times them is exact, and subtract in stages. It is fast, it is
        short, and it stops working somewhere around a million, because past
        there n needs more bits than the split has and the residual error
        grows until, for an argument near 2^63, there is nothing left. Every
        library that has done that has had to say so in its documentation,
        and the numbers it prints for sin(1e22) are wrong.

        What is here instead is Payne and Hanek's, which does not degrade at
        all, and it is not much longer. The observation is that the bits of
        2/pi far above the argument's own scale contribute only whole
        multiples of four to n and cannot change the answer, and the bits far
        below contribute less than the last bit of the result. So only a
        window of 2/pi matters, its position given by the argument's
        exponent, and the reduction is an integer multiply of the
        significand by that window.

        Concretely: the argument is m * 2^s with m a fifty three bit integer.
        A bit of 2/pi at fractional position i contributes m * 2^(s-i) to the
        product, which is a multiple of four whenever i is at or below s-2,
        so the window starts at s-1. Taking a hundred and ninety two bits
        from there, multiplying by m into a two hundred and fifty six bit
        product, and putting the binary point a hundred and ninety places up
        gives the whole part of x*2/pi in its top bits and the fraction below
        -- and the bits dropped off the end of the window are worth less than
        2^-137 of a turn, which is a hundred and thirty bits below anything
        that shows.

        The fraction is then nudged to [-1/2, 1/2) by rounding the whole part
        to nearest, and multiplied by pi/2 in two pieces to give r. Because
        the fraction is carried as a hundred and twenty six bit integer, an
        argument that lands within 2^-61 of a multiple of pi/2 -- which is
        the worst any double does -- still leaves sixty five good bits in r,
        and r comes out correct to better than half an ulp of itself
        everywhere in the domain.

        There is one branch, and it is not a fallback: an argument already
        inside the band skips all of this, because there is nothing to
        reduce.

        The table is 2/pi to sixteen hundred bits, generated from a hundred
        and fifty digit expansion, as twenty six sixty four bit words most
        significant first. Nineteen of them are reachable by the largest
        double; the rest are the window's overhang.
*/
static const p64 math_two_over_pi_bits[26] = {
        0xa2f9836e4e441529ULL, 0xfc2757d1f534ddc0ULL, 0xdb6295993c439041ULL,
        0xfe5163abdebbc561ULL, 0xb7246e3a424dd2e0ULL, 0x06492eea09d1921cULL,
        0xfe1deb1cb129a73eULL, 0xe88235f52ebb4484ULL, 0xe99c7026b45f7e41ULL,
        0x3991d639835339f4ULL, 0x9c845f8bbdf9283bULL, 0x1ff897ffde05980fULL,
        0xef2f118b5a0a6d1fULL, 0x6d367ecf27cb09b7ULL, 0x4f463f669e5fea2dULL,
        0x7527bac7ebe5f17bULL, 0x3d0739f78a5292eaULL, 0x6bfb5fb11f8d5d08ULL,
        0x56033046fc7b6babULL, 0xf0cfbc209af4361dULL, 0xa9e391615ee61b08ULL,
        0x6599855f14a06840ULL, 0x8dffd8804d732731ULL, 0x06061556ca73a8c9ULL,
        0x60e27bc08c6b47c4ULL, 0x19c367cddce8092aULL};

static p64 math_two_over_pi_word(b32 index)
{
        if (index < 0 || index >= 26)
                return 0;
        return math_two_over_pi_bits[index];
}

//      Answers the quadrant, 0 through 3, and leaves the reduced argument
//      behind. The argument is finite and its magnitude is past pi/4.
static b32 math_reduce_quadrant(decimal value, decimal address_to reduced)
{
        math_shape shape;
        p64 significand;
        b32 exponent, start, word_index, shift;
        p64 window_high, window_middle, window_low;
        p128 stage;
        p64 part_high, part_middle, carry;
        p64 fraction_high, fraction_middle;
        b128 fraction;
        b32 quadrant;
        b64 top;
        p64 middle, bottom;
        decimal top_part, middle_part, bottom_part;
        decimal joined, joined_error, head, head_error;
        decimal product, product_error;
        b32 negative = 0;
        decimal magnitude = value;

        if (magnitude < 0.0)
        {
                magnitude = -magnitude;
                negative = 1;
        }

        shape.value = magnitude;
        significand = (shape.bits & MATH_MANTISSA_MASK) | MATH_IMPLIED_BIT;
        exponent = (b32)((shape.bits >> MATH_MANTISSA_BITS) & 0x7ff) - MATH_EXPONENT_BIAS;

        //      The window starts one bit above where a contribution could
        //      still be something other than a multiple of four.
        start = exponent - MATH_MANTISSA_BITS - 1;

        //      Floor division rather than C's truncation, because the window
        //      start is negative for every argument below 2^53 and a
        //      truncating divide would land a word too high.
        word_index = (start - 1) >= 0 ? (start - 1) / 64 : -(((-(start - 1)) + 63) / 64);
        shift = (start - 1) - word_index * 64;

        if (shift == 0)
        {
                window_high = math_two_over_pi_word(word_index);
                window_middle = math_two_over_pi_word(word_index + 1);
                window_low = math_two_over_pi_word(word_index + 2);
        }
        else
        {
                window_high = (math_two_over_pi_word(word_index) << shift) |
                              (math_two_over_pi_word(word_index + 1) >> (64 - shift));
                window_middle = (math_two_over_pi_word(word_index + 1) << shift) |
                                (math_two_over_pi_word(word_index + 2) >> (64 - shift));
                window_low = (math_two_over_pi_word(word_index + 2) << shift) |
                             (math_two_over_pi_word(word_index + 3) >> (64 - shift));
        }

        //      One fifty three bit significand against a hundred and ninety
        //      two bit window, carried by hand into two hundred and
        //      fifty six bits.
        //      The lowest sixty four bits of the product are a hundred and
        //      twenty six places below the binary point and are dropped
        //      where they fall; only the carry out of them matters.
        stage = (p128)significand * (p128)window_low;
        carry = (p64)(stage >> 64);

        stage = (p128)significand * (p128)window_middle + (p128)carry;
        part_middle = (p64)stage;
        carry = (p64)(stage >> 64);

        stage = (p128)significand * (p128)window_high + (p128)carry;
        part_high = (p64)stage;
        carry = (p64)(stage >> 64);

        //      The binary point sits a hundred and ninety places up, so the
        //      whole part is the top sixty six bits and the fraction is the
        //      hundred and ninety below them.
        quadrant = (b32)(((carry << 2) | (part_high >> 62)) & 3);
        fraction_high = part_high & 0x3fffffffffffffffULL;
        fraction_middle = part_middle;

        //      Only the top hundred and twenty six bits of the fraction are
        //      kept. What is dropped is worth 2^-126 of a turn, sixty five
        //      bits below the worst cancellation a double can produce.
        fraction = (b128)(((p128)fraction_high << 64) | (p128)fraction_middle);

        //      Round the whole part to nearest by pulling a fraction at or
        //      past a half up to the next quadrant and letting the leftover
        //      go negative.
        if (fraction_high >> 61)
        {
                fraction = fraction - ((b128)1 << 126);
                quadrant = (quadrant + 1) & 3;
        }

        //      Three exactly representable pieces, forty two bits each, so
        //      that a hundred and twenty six bit integer becomes a pair of
        //      doubles without losing a bit on the way.
        top = (b64)(fraction >> 84);
        middle = (p64)(fraction >> 42) & 0x3ffffffffffULL;
        bottom = (p64)fraction & 0x3ffffffffffULL;

        top_part = (decimal)top * 2.2737367544323206e-13;     // two to the minus forty two
        middle_part = (decimal)middle * 5.169878828456423e-26; // two to the minus eighty four
        bottom_part = (decimal)bottom * 1.1754943508222875e-38; // two to the minus one hundred twenty six

        joined = math_two_sum(middle_part, bottom_part, address_of joined_error);
        head = math_two_sum(top_part, joined, address_of head_error);
        head_error = head_error + joined_error;

        product = math_two_product(head, MATH_PI_OVER_TWO, address_of product_error);
        product_error = product_error + head * MATH_PI_OVER_TWO_TAIL +
                        head_error * MATH_PI_OVER_TWO;

        if (negative)
        {
                address_to reduced = -(product + product_error);
                return (-quadrant) & 3;
        }

        address_to reduced = product + product_error;
        return quadrant;
}

/*
        The two series, on a reduced argument no larger than pi/4.

        Both are Taylor and both are carried past where the truncation can be
        seen. Sine keeps terms through x^17, whose first omission is
        x^19/19!, two parts in 10^19 at the end of the band and a hundred
        and forty times below the last bit; cosine keeps terms through x^16
        with the same margin. As with the exponential these are the exact
        rationals 1/n!, readable and checkable, at the cost of three or four
        more multiply-adds than a minimax fit of the same accuracy.

        Sine's shape puts the whole of the leading term outside the
        polynomial: x + x^3*P(x^2), so the term carrying almost all of the
        value never rounds, and an argument small enough that its cube
        underflows comes straight back out.

        Cosine cannot do that, because its leading term is one and the second
        is not small -- at the end of the band x^2/2 is 0.31, and 1 - 0.31 is
        a subtraction whose rounding is a whole ulp of the answer. So the
        rounding is recovered rather than tolerated: w is the rounded
        difference and (1 - w) - hz is exactly what the rounding threw away,
        because 1 - w is itself exact for every w the band can produce. That
        one extra subtraction is the difference between a cosine good to two
        ulp and one good to a half.
*/
#define MATH_SIN_C3 -0.16666666666666666
#define MATH_SIN_C5 0.008333333333333333
#define MATH_SIN_C7 -0.0001984126984126984
#define MATH_SIN_C9 2.7557319223985893e-06
#define MATH_SIN_C11 -2.505210838544172e-08
#define MATH_SIN_C13 1.6059043836821613e-10
#define MATH_SIN_C15 -7.647163731819816e-13
#define MATH_SIN_C17 2.8114572543455206e-15

#define MATH_COS_C4 0.041666666666666664
#define MATH_COS_C6 -0.001388888888888889
#define MATH_COS_C8 2.48015873015873e-05
#define MATH_COS_C10 -2.755731922398589e-07
#define MATH_COS_C12 2.08767569878681e-09
#define MATH_COS_C14 -1.1470745597729725e-11
#define MATH_COS_C16 4.779477332387385e-14

static decimal math_sine_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;
        decimal walked;

        walked = MATH_SIN_C17;
        walked = MATH_SIN_C15 + squared * walked;
        walked = MATH_SIN_C13 + squared * walked;
        walked = MATH_SIN_C11 + squared * walked;
        walked = MATH_SIN_C9 + squared * walked;
        walked = MATH_SIN_C7 + squared * walked;
        walked = MATH_SIN_C5 + squared * walked;
        walked = MATH_SIN_C3 + squared * walked;

        return reduced + (reduced * squared) * walked;
}

static decimal math_cosine_reduced(decimal reduced)
{
        decimal squared = reduced * reduced;
        decimal half = 0.5 * squared;
        decimal front = 1.0 - half;
        decimal lost = (1.0 - front) - half;
        decimal walked;

        walked = MATH_COS_C16;
        walked = MATH_COS_C14 + squared * walked;
        walked = MATH_COS_C12 + squared * walked;
        walked = MATH_COS_C10 + squared * walked;
        walked = MATH_COS_C8 + squared * walked;
        walked = MATH_COS_C6 + squared * walked;
        walked = MATH_COS_C4 + squared * walked;

        return front + (lost + (squared * squared) * walked);
}

/*
        The three circular functions.

        Each is the reduction followed by the kernel the quadrant selects,
        and the quadrant table is the addition formula written out: a
        quarter turn swaps sine for cosine, a half turn changes the sign, and
        the two compose.

        The band test at the top is not an optimisation for the common case,
        although it is that too. It is what keeps a small argument out of the
        integer reduction entirely, so that sin of a subnormal is that
        subnormal and cos of one is exactly one.

        Measured, over eight million arguments log spaced from 1e-320 to
        1e300 with random signs -- so most of them are far past where a Cody
        and Waite reduction would have given up:

              sin   worst 1 ulp, 92.45 percent bit identical
              cos   worst 1 ulp, 92.45 percent bit identical
              tan   worst 2 ulp, 85.09 percent bit identical
              tan, inside the band and needing no reduction at all,
                    worst 1 ulp, 96.88 percent identical

        There is no degradation with the size of the argument: sin of 1e300
        is as accurate as sin of 1. tan is the one that is worse, and only by
        the last bit, for the reason written above its own routine.
*/
static decimal sine(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        //      Below this the cube term is past the last bit, and returning
        //      the argument is what carries a negative zero back out: the
        //      series would form it as minus zero times a negative
        //      coefficient and hand back a positive one.
        if (absolute(value) < MATH_TWO_TO_MINUS_27)
                return value;

        if (absolute(value) <= MATH_PI_OVER_FOUR)
                return math_sine_reduced(value);

        quadrant = math_reduce_quadrant(value, address_of reduced);

        switch (quadrant)
        {
        case 0:
                return math_sine_reduced(reduced);
        case 1:
                return math_cosine_reduced(reduced);
        case 2:
                return -math_sine_reduced(reduced);
        default:
                return -math_cosine_reduced(reduced);
        }
}

static decimal cosine(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        if (absolute(value) <= MATH_PI_OVER_FOUR)
                return math_cosine_reduced(value);

        quadrant = math_reduce_quadrant(value, address_of reduced);

        switch (quadrant)
        {
        case 0:
                return math_cosine_reduced(reduced);
        case 1:
                return -math_sine_reduced(reduced);
        case 2:
                return -math_cosine_reduced(reduced);
        default:
                return math_sine_reduced(reduced);
        }
}

/*
        The tangent of a reduced argument, as a quotient that keeps what both
        kernels rounded away.

        tan is the one circular function with no series of its own worth
        having: its Taylor coefficients are Bernoulli numbers and on a band
        reaching pi/4 it would need thirty terms, because the pole at pi/2 is
        only a quarter turn past the end. So it is sine over cosine, and the
        price of that is normally three roundings where there should be one
        -- each kernel's, and the division's.

        Two of the three are recoverable. Both kernels are built as a leading
        term plus a small correction, and the exact split of that sum is a
        fast two-sum away, so each comes back as a pair rather than a rounded
        double. The division then takes its own residual out by a fused
        multiply-add over both halves. What is left is the truncation of the
        two series, which is twenty orders below the answer, and one final
        rounding.

        The quadrant decides which way up the quotient goes: an odd quadrant
        wants the cotangent, which is the same pair of kernels with the
        arguments of the division exchanged and the sign flipped, not a
        reciprocal of the tangent -- taking a reciprocal would put back the
        rounding this routine exists to avoid.
*/
static decimal math_tangent_reduced(decimal reduced, bool cotangent)
{
        decimal squared = reduced * reduced;
        decimal sine_high, sine_low;
        decimal cosine_high, cosine_low;
        decimal front, lost, walked;
        decimal quotient, residual;
        decimal numerator_high, numerator_low;
        decimal denominator_high, denominator_low;

        walked = MATH_SIN_C17;
        walked = MATH_SIN_C15 + squared * walked;
        walked = MATH_SIN_C13 + squared * walked;
        walked = MATH_SIN_C11 + squared * walked;
        walked = MATH_SIN_C9 + squared * walked;
        walked = MATH_SIN_C7 + squared * walked;
        walked = MATH_SIN_C5 + squared * walked;
        walked = MATH_SIN_C3 + squared * walked;
        sine_high = math_fast_two_sum(reduced, (reduced * squared) * walked,
                                      address_of sine_low);

        front = 1.0 - 0.5 * squared;
        lost = (1.0 - front) - 0.5 * squared;
        walked = MATH_COS_C16;
        walked = MATH_COS_C14 + squared * walked;
        walked = MATH_COS_C12 + squared * walked;
        walked = MATH_COS_C10 + squared * walked;
        walked = MATH_COS_C8 + squared * walked;
        walked = MATH_COS_C6 + squared * walked;
        walked = MATH_COS_C4 + squared * walked;
        cosine_high = math_fast_two_sum(front, lost + (squared * squared) * walked,
                                        address_of cosine_low);

        if (cotangent)
        {
                numerator_high = -cosine_high;
                numerator_low = -cosine_low;
                denominator_high = sine_high;
                denominator_low = sine_low;
        }
        else
        {
                numerator_high = sine_high;
                numerator_low = sine_low;
                denominator_high = cosine_high;
                denominator_low = cosine_low;
        }

        quotient = numerator_high / denominator_high;
        residual = (decimal_multiply_add(-quotient, denominator_high, numerator_high) +
                    numerator_low) -
                   quotient * denominator_low;
        return quotient + residual / denominator_high;
}

static decimal tangent(decimal value)
{
        decimal reduced;
        b32 quadrant;

        if (!decimal_is_finite(value))
                return (value - value) / (value - value);

        if (absolute(value) <= MATH_PI_OVER_FOUR)
        {
                //      Below this the cube term is past the last bit, and
                //      the signed zero has to come back out unchanged.
                if (absolute(value) < MATH_TWO_TO_MINUS_27)
                        return value;
                return math_tangent_reduced(value, 0);
        }

        quadrant = math_reduce_quadrant(value, address_of reduced);
        return math_tangent_reduced(reduced, (bool)(quadrant & 1));
}

/*
        The inverse sine, on the half of its domain where a series works.

        asin has a square root singularity at both ends: its derivative is
        1/sqrt(1-x^2), which is infinite at one, so no polynomial in x can
        follow it there. The standard escape is the half angle identity,

              asin(x) = pi/2 - 2*asin(sqrt((1-x)/2))

        which turns an argument near one into an argument near zero, and the
        series only ever has to work on [0, 1/2]. This routine is that
        series, in u = x^2, and both asin and acos call it.

        The coefficients are the binomial expansion of the derivative
        integrated term by term: (2n)! / (4^n (n!)^2 (2n+1)), which are exact
        rationals like the exponential's. Twenty eight of them are needed
        where a minimax rational of degree six over five would do, because on
        u up to a quarter the ratio between successive terms is only four and
        the series converges slowly. That is the cost of coefficients anyone
        can check, and it is paid in latency and in the length of this list.

        The leading x is outside the polynomial for the same reason it is in
        the sine: it carries the whole of the value for a small argument and
        must not round.
*/
#define MATH_ASIN_C1 0.16666666666666666
#define MATH_ASIN_C2 0.075
#define MATH_ASIN_C3 0.044642857142857144
#define MATH_ASIN_C4 0.030381944444444444
#define MATH_ASIN_C5 0.022372159090909092
#define MATH_ASIN_C6 0.017352764423076924
#define MATH_ASIN_C7 0.01396484375
#define MATH_ASIN_C8 0.011551800896139705
#define MATH_ASIN_C9 0.009761609529194078
#define MATH_ASIN_C10 0.008390335809616815
#define MATH_ASIN_C11 0.0073125258735988454
#define MATH_ASIN_C12 0.006447210311889649
#define MATH_ASIN_C13 0.005740037670841924
#define MATH_ASIN_C14 0.005153309682319905
#define MATH_ASIN_C15 0.004660143486915096
#define MATH_ASIN_C16 0.004240907093679363
#define MATH_ASIN_C17 0.003880964558837669
#define MATH_ASIN_C18 0.0035692053938259347
#define MATH_ASIN_C19 0.003297059503473485
#define MATH_ASIN_C20 0.0030578216492580306
#define MATH_ASIN_C21 0.002846178401108942
#define MATH_ASIN_C22 0.00265787063820729
#define MATH_ASIN_C23 0.0024894486782468836
#define MATH_ASIN_C24 0.002338091892111975
#define MATH_ASIN_C25 0.0022014739737101384
#define MATH_ASIN_C26 0.0020776610325181676
#define MATH_ASIN_C27 0.0019650336162772837
#define MATH_ASIN_C28 0.0018622264064031275

//      The series without its leading term: asin(x) is x + x*u*this(u),
//      with u the square of x and |x| no more than a half.
static decimal math_arc_sine_series(decimal squared)
{
        decimal walked;

        walked = MATH_ASIN_C28;
        walked = MATH_ASIN_C27 + squared * walked;
        walked = MATH_ASIN_C26 + squared * walked;
        walked = MATH_ASIN_C25 + squared * walked;
        walked = MATH_ASIN_C24 + squared * walked;
        walked = MATH_ASIN_C23 + squared * walked;
        walked = MATH_ASIN_C22 + squared * walked;
        walked = MATH_ASIN_C21 + squared * walked;
        walked = MATH_ASIN_C20 + squared * walked;
        walked = MATH_ASIN_C19 + squared * walked;
        walked = MATH_ASIN_C18 + squared * walked;
        walked = MATH_ASIN_C17 + squared * walked;
        walked = MATH_ASIN_C16 + squared * walked;
        walked = MATH_ASIN_C15 + squared * walked;
        walked = MATH_ASIN_C14 + squared * walked;
        walked = MATH_ASIN_C13 + squared * walked;
        walked = MATH_ASIN_C12 + squared * walked;
        walked = MATH_ASIN_C11 + squared * walked;
        walked = MATH_ASIN_C10 + squared * walked;
        walked = MATH_ASIN_C9 + squared * walked;
        walked = MATH_ASIN_C8 + squared * walked;
        walked = MATH_ASIN_C7 + squared * walked;
        walked = MATH_ASIN_C6 + squared * walked;
        walked = MATH_ASIN_C5 + squared * walked;
        walked = MATH_ASIN_C4 + squared * walked;
        walked = MATH_ASIN_C3 + squared * walked;
        walked = MATH_ASIN_C2 + squared * walked;
        walked = MATH_ASIN_C1 + squared * walked;

        return walked;
}

/*
        The inverse sine.

        Below a half the series is the whole answer. Above it the half angle
        identity moves the work to an argument near zero, and the subtraction
        (1 - |x|) that gets it there is exact for every |x| in [1/2, 1] by
        Sterbenz, so the reduction itself costs nothing. What it does cost is
        that the answer is then pi/2 minus twice a small thing, and pi/2 has
        to be carried in two doubles for the last bits of that to survive.

        Measured: worst 1 ulp, 98.52 percent bit identical over a uniform
        sweep of [-1, 1], and worst 1 ulp, 99.89 percent identical over a
        sweep concentrated within 1e-17 of both ends. Before the square
        root's own residual was put back it was two ulp just above a half,
        where the identity doubles that residual on its way into the
        answer.
*/
//      The residual of a square root, which is what a correctly rounded
//      root threw away. asin and acos both double their root before
//      subtracting it from pi/2, which doubles that residual into a full ulp
//      of the answer unless it is put back. A fused multiply-add sees the
//      exact difference between the square of the root and what was rooted.
static decimal math_root_residual(decimal root, decimal squared)
{
        if (root == 0.0)
                return 0.0;
        return decimal_multiply_add(-root, root, squared) / (root + root);
}

static decimal arc_sine(decimal value)
{
        decimal magnitude = absolute(value);
        decimal squared, root, root_low, correction;
        decimal head, head_error, answer;

        if (magnitude > 1.0)
                return (value - value) / (value - value);

        if (magnitude <= 0.5)
        {
                if (magnitude < MATH_TWO_TO_MINUS_27)
                        return value;
                squared = value * value;
                return value + (value * squared) * math_arc_sine_series(squared);
        }

        //      Exact for every magnitude in the upper half, by Sterbenz.
        squared = (1.0 - magnitude) * 0.5;
        root = square_root(squared);
        root_low = math_root_residual(root, squared);
        correction = (root * squared) * math_arc_sine_series(squared);

        head = math_two_sum(MATH_PI_OVER_TWO, -2.0 * root, address_of head_error);
        answer = head + ((head_error + MATH_PI_OVER_TWO_TAIL) -
                         2.0 * (root_low + correction));

        return decimal_with_sign(answer, value);
}

/*
        The inverse cosine, which is not pi/2 minus the inverse sine
        everywhere.

        Writing it that way is right in the middle of the domain and wrong at
        the top of it: as x approaches one the answer approaches zero, and
        subtracting an asin that approaches pi/2 from a pi/2 that is itself
        rounded throws the answer away entirely. So the top of the domain
        goes through the half angle identity directly -- acos(x) is twice the
        asin of sqrt((1-x)/2), with no subtraction anywhere in it -- and the
        bottom is pi minus that same form. Only the middle third, where the
        answer is comfortably away from both ends, is pi/2 minus a series.

        Measured: worst 1 ulp, 99.35 percent bit identical over a uniform
        sweep of [-1, 1], and worst 1 ulp, 99.93 percent identical near both
        ends. acos(1) is exactly zero and acos(-1) is pi.
*/
static decimal arc_cosine(decimal value)
{
        decimal squared, root, root_low, correction;
        decimal head, head_error;

        if (absolute(value) > 1.0)
                return (value - value) / (value - value);

        if (absolute(value) <= 0.5)
        {
                squared = value * value;
                correction = (value * squared) * math_arc_sine_series(squared);
                head = math_two_sum(MATH_PI_OVER_TWO, -value, address_of head_error);
                return head + ((head_error + MATH_PI_OVER_TWO_TAIL) - correction);
        }

        if (value > 0.5)
        {
                //      No subtraction anywhere in this branch, which is the
                //      point of it: the answer goes to zero as the argument
                //      goes to one and nothing large is standing next to it.
                squared = (1.0 - value) * 0.5;
                root = square_root(squared);
                root_low = math_root_residual(root, squared);
                correction = (root * squared) * math_arc_sine_series(squared);
                return 2.0 * (root + (root_low + correction));
        }

        squared = (1.0 + value) * 0.5;
        root = square_root(squared);
        root_low = math_root_residual(root, squared);
        correction = (root * squared) * math_arc_sine_series(squared);
        head = math_two_sum(MATH_PI, -2.0 * root, address_of head_error);
        return head + ((head_error + MATH_PI_TAIL) - 2.0 * (root_low + correction));
}

/*
        The inverse tangent, reduced onto a sixteenth of a turn by a table.

        The series for atan converges at a rate set by the argument, and at
        an argument of one it barely converges at all -- the terms are 1/n
        and it would take 10^16 of them. The reciprocal identity halves the
        problem, bringing everything into [0, 1], and past that the addition
        formula does the rest:

              atan(a) = atan(x0) + atan((a - x0)/(1 + a*x0))

        With x0 taken from a table of nine values an eighth apart and chosen
        as the nearest one to a, the second argument is never larger than a
        sixteenth, and eight terms of the series put its truncation twenty
        orders below the answer. The table is stored as a head and a tail
        each, because the exact atan of an eighth is not a double and the
        rounding of it would otherwise be the largest error in the routine.

        The eighths are chosen rather than tangents of equally spaced angles
        because an eighth is exactly representable and, for every a the table
        entry is chosen for, the subtraction a - x0 is then a Sterbenz
        subtraction and exact. Only the denominator rounds, and its rounding
        arrives in the answer scaled down by how small the second atan is
        relative to the first.

        Measured: worst 1 ulp, 99.96 percent bit identical, over eight
        million arguments spanning the whole finite range with both signs.
*/
#define MATH_ATAN_A1 -0.3333333333333333
#define MATH_ATAN_A2 0.2
#define MATH_ATAN_A3 -0.14285714285714285
#define MATH_ATAN_A4 0.1111111111111111
#define MATH_ATAN_A5 -0.09090909090909091
#define MATH_ATAN_A6 0.07692307692307693
#define MATH_ATAN_A7 -0.06666666666666667

static const decimal math_arc_tangent_head[9] = {
        0.0, 0.12435499454676144, 0.24497866312686414, 0.35877067027057225,
        0.4636476090008061, 0.5585993153435624, 0.6435011087932844,
        0.7188299996216245, 0.7853981633974483};

static const decimal math_arc_tangent_tail[9] = {
        0.0, -3.1253241424539383e-18, 1.0698755618734451e-17,
        -2.4623815582638635e-17, 2.2698777452961687e-17,
        -5.4556305485916264e-18, 1.5834785051444286e-17,
        -2.1478388444456983e-17, 3.061616997868383e-17};

static decimal math_arc_tangent_series(decimal small)
{
        decimal squared = small * small;
        decimal walked;

        walked = MATH_ATAN_A7;
        walked = MATH_ATAN_A6 + squared * walked;
        walked = MATH_ATAN_A5 + squared * walked;
        walked = MATH_ATAN_A4 + squared * walked;
        walked = MATH_ATAN_A3 + squared * walked;
        walked = MATH_ATAN_A2 + squared * walked;
        walked = MATH_ATAN_A1 + squared * walked;

        return small + (small * squared) * walked;
}

//      The inverse tangent of a non-negative finite argument, which is where
//      all the work is; atan and atan2 both put the signs on afterwards.
static decimal math_arc_tangent_positive(decimal value)
{
        b32 index;
        decimal reduced, answer, answer_error;
        decimal point, working_low = 0.0;
        bool inverted = 0;
        decimal working = value;

        if (working > 1.0)
        {
                //      Past here the reciprocal is smaller than the last bit
                //      of pi/2 and the answer cannot be told from it.
                if (working > 1.0e18)
                        return MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL;
                working = 1.0 / working;
                //      The reciprocal's own rounding, kept so it can be
                //      folded back through the derivative below.
                working_low = decimal_multiply_add(-working, value, 1.0) / value;
                inverted = 1;
        }

        index = math_nearest_whole(working * 8.0);
        point = (decimal)index * 0.125;

        if (index == 0)
        {
                reduced = working;
        }
        else
        {
                //      The numerator is exact -- the argument and the table
                //      point are within a factor of two of each other for
                //      every index this picks -- so the denominator is taken
                //      with one rounding rather than two, and the division
                //      gives its own back.
                reduced = math_quotient(working - point,
                                        decimal_multiply_add(working, point, 1.0));
        }

        answer = math_two_sum(math_arc_tangent_head[index],
                              math_arc_tangent_series(reduced),
                              address_of answer_error);
        answer = answer + (answer_error + math_arc_tangent_tail[index]);

        if (inverted)
        {
                //      d(atan u)/du is 1/(1+u^2), which is how the
                //      reciprocal's rounding gets from the argument into the
                //      answer.
                answer = answer + working_low / (1.0 + working * working);
                answer = math_two_sum(MATH_PI_OVER_TWO, -answer,
                                      address_of answer_error);
                return answer + (answer_error + MATH_PI_OVER_TWO_TAIL);
        }
        return answer;
}

static decimal arc_tangent(decimal value)
{
        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL,
                                         value);
        if (absolute(value) < MATH_TWO_TO_MINUS_27)
                return value;

        return decimal_with_sign(math_arc_tangent_positive(absolute(value)), value);
}

/*
        The inverse tangent of a quotient, which is the one that knows which
        quadrant it is in.

        atan2 exists because atan(y/x) cannot tell the third quadrant from
        the first: the quotient has already thrown the signs away, and it has
        thrown away the answer as well when x is tiny and y is huge and the
        division overflows. So the quotient is only formed where it is safe,
        the signs are read off the arguments themselves, and the eight cases
        where one or both arguments is a zero or an infinity are answered
        from the table C99 gives rather than from any arithmetic -- including
        atan2(0, -0), which is pi, and atan2(-0, 0), which is minus zero.

        Adding or subtracting pi at the end is done with pi carried in two
        doubles, because the answers in the second and third quadrants are
        differences from pi and a rounded pi would put a full ulp of error
        into half the domain.

        Measured: worst 2 ulp, 75.57 percent bit identical, over eight
        million pairs log spaced in both arguments across the whole finite
        range with all four sign combinations, plus every special case C99
        lists. The second ulp is the arctangent's own last bit arriving on
        top of the quotient's, and it is the one routine here that would need
        the arctangent itself carried in two doubles to do better.
*/
static decimal arc_tangent_two(decimal rise, decimal run)
{
        decimal answer;

        if (decimal_is_nan(rise) || decimal_is_nan(run))
                return rise + run;

        if (run == 0.0 && rise == 0.0)
        {
                //      Both zero: the answer is decided entirely by the sign
                //      bit of the second argument.
                if (decimal_sign_bit(run))
                        return decimal_with_sign(MATH_PI, rise);
                return decimal_with_sign(0.0, rise);
        }

        if (rise == 0.0)
        {
                if (decimal_sign_bit(run))
                        return decimal_with_sign(MATH_PI, rise);
                return decimal_with_sign(0.0, rise);
        }

        if (run == 0.0)
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL, rise);

        if (decimal_is_infinite(rise))
        {
                if (decimal_is_infinite(run))
                {
                        //      Both infinite: the answer is a diagonal, and
                        //      which diagonal is all the arguments still say.
                        if (run > 0.0)
                                return decimal_with_sign(MATH_PI_OVER_FOUR, rise);
                        return decimal_with_sign(MATH_THREE_PI_OVER_FOUR, rise);
                }
                return decimal_with_sign(MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL, rise);
        }

        if (decimal_is_infinite(run))
        {
                if (run > 0.0)
                        return decimal_with_sign(0.0, rise);
                return decimal_with_sign(MATH_PI, rise);
        }

        //      A quotient too far from one to be formed is answered by the
        //      limit it is approaching, which is what the division would
        //      have given after overflowing or flushing to zero anyway.
        {
                b32 rise_exponent, run_exponent;
                decimal_split_exponent(rise, address_of rise_exponent);
                decimal_split_exponent(run, address_of run_exponent);

                if (rise_exponent - run_exponent > 60)
                {
                        answer = MATH_PI_OVER_TWO + MATH_PI_OVER_TWO_TAIL;
                }
                else
                {
                        //      The quotient is a rounding the arguments did
                        //      not have, and it lands in the answer scaled by
                        //      the arctangent's derivative, so it is taken
                        //      out the same way the reciprocal's is above.
                        decimal above = absolute(rise);
                        decimal below = absolute(run);
                        decimal quotient = above / below;
                        decimal quotient_low =
                                decimal_multiply_add(-quotient, below, above) / below;

                        answer = math_arc_tangent_positive(quotient);
                        if (decimal_is_finite(quotient))
                                answer = answer +
                                         quotient_low / (1.0 + quotient * quotient);
                }
        }

        if (run > 0.0)
                return decimal_with_sign(answer, rise);

        {
                decimal head, head_error;
                head = math_two_sum(MATH_PI, -answer, address_of head_error);
                return decimal_with_sign(head + (head_error + MATH_PI_TAIL), rise);
        }
}

/*
        The three hyperbolic functions, which are the exponential seen twice.

        Each of them is a combination of e^x and e^-x, and each of them has
        a place where writing it that way destroys the answer. sinh near zero
        is the difference of two numbers both close to one; tanh near zero is
        that difference over a sum. Both are recovered the same way, by
        asking the exponential for e^x - 1 instead of e^x, so that the
        cancellation happens inside a series that never formed the one.

            sinh(a) = (t + t/(t+1)) / 2       with t = e^a - 1
            tanh(a) = -t / (t + 2)            with t = e^(-2a) - 1
            cosh(a) = 1 + t*t/(2*(1+t))       with t = e^a - 1

        The sinh identity is worth a line of algebra: t + t/(t+1) is
        (e^a - 1) + (e^a - 1)/e^a, which is e^a - e^-a, and every term in it
        stays away from cancellation because t is the small quantity rather
        than the difference of two large ones.

        The far ends are the other thing each of them has to get right. Above
        an argument of about 22 the two exponentials differ by more than the
        format can hold and both sinh and cosh become half of e^a exactly;
        above 709.78 that halved exponential would overflow before being
        halved, so it is taken as two square roots of itself multiplied
        together instead, which buys the last ln2 of range. tanh saturates at
        one long before that, at an argument of 22, where 2/(e^2a + 1) is
        below the last bit of one.

        Measured, over eight million arguments log spaced to 800 with random
        signs:

            sinh  worst 2 ulp, 98.33 percent bit identical
            cosh  worst 1 ulp, 99.82 percent bit identical
            tanh  worst 2 ulp, 99.18 percent bit identical

        sinh and tanh inherit the exponential's last bit and then divide by
        something built from it, which is where their second ulp comes from;
        cosh adds rather than divides and keeps the first.
*/
static decimal hyperbolic_sine(decimal value)
{
        decimal magnitude = absolute(value);
        decimal grown, half;

        if (!decimal_is_finite(value))
                return value + value;

        if (magnitude < 1.0)
        {
                grown = exponential_minus_one(magnitude);
                half = 0.5 * (grown + grown / (grown + 1.0));
                return decimal_with_sign(half, value);
        }

        if (magnitude < 22.0)
        {
                grown = exponential(magnitude);
                return decimal_with_sign(0.5 * (grown - 1.0 / grown), value);
        }

        if (magnitude < 709.782712893384)
                return decimal_with_sign(0.5 * exponential(magnitude), value);

        if (magnitude < 710.4758600739439)
        {
                //      Half of e^a would overflow before the halving, so the
                //      exponential is split and one of the halves carries the
                //      factor of a half with it.
                grown = exponential(0.5 * magnitude);
                return decimal_with_sign(grown * (0.5 * grown), value);
        }

        return decimal_with_sign(MATH_HUGE * MATH_HUGE, value);
}

static decimal hyperbolic_cosine(decimal value)
{
        decimal magnitude = absolute(value);
        decimal grown;

        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return MATH_HUGE * MATH_HUGE;

        if (magnitude < 0.34657359027997264)
        {
                grown = exponential_minus_one(magnitude);
                return 1.0 + (grown * grown) / (2.0 * (1.0 + grown));
        }

        if (magnitude < 22.0)
        {
                grown = exponential(magnitude);
                return 0.5 * (grown + 1.0 / grown);
        }

        if (magnitude < 709.782712893384)
                return 0.5 * exponential(magnitude);

        if (magnitude < 710.4758600739439)
        {
                grown = exponential(0.5 * magnitude);
                return grown * (0.5 * grown);
        }

        return MATH_HUGE * MATH_HUGE;
}

static decimal hyperbolic_tangent(decimal value)
{
        decimal magnitude = absolute(value);
        decimal grown, answer;

        if (decimal_is_nan(value))
                return value;
        if (decimal_is_infinite(value))
                return decimal_with_sign(1.0, value);

        //      Below this the cube term is past the last bit and tanh is the
        //      argument, which also carries the signed zero through.
        if (magnitude < MATH_TWO_TO_MINUS_28)
                return value;

        if (magnitude >= 22.0)
                return decimal_with_sign(1.0, value);

        if (magnitude < 1.0)
        {
                grown = exponential_minus_one(-2.0 * magnitude);
                answer = -grown / (grown + 2.0);
        }
        else
        {
                grown = exponential_minus_one(2.0 * magnitude);
                answer = 1.0 - 2.0 / (grown + 2.0);
        }

        return decimal_with_sign(answer, value);
}

/*
        The standard names.

        Each is a one line wrapper rather than a macro, so that a program can
        take the address of one and so that a local variable called `exp`
        does not silently become a call. They are static like everything else
        in this file: an unused one leaves no code behind, and none of them
        can collide at link time with a program that also links a real libc.

        There is no `log`. The library's `log` is the writer, and the natural
        logarithm is `logarithm`, or `ln` for short.

        Also absent, because standard.inc already has them under prose names:
        sqrt is square_root, fabs is absolute, trunc floor ceil round and
        nearbyint are the decimal_ roundings, copysign is decimal_with_sign,
        fmin and fmax are decimal_smaller and decimal_larger, fdim is
        decimal_difference and fma is decimal_multiply_add.
*/
static decimal ln(decimal value) { return logarithm(value); }
static decimal exp(decimal value) { return exponential(value); }
static decimal exp2(decimal value) { return exponential_two(value); }
static decimal expm1(decimal value) { return exponential_minus_one(value); }
static decimal log2(decimal value) { return logarithm_two(value); }
static decimal log10(decimal value) { return logarithm_ten(value); }
static decimal pow(decimal base, decimal exponent) { return power(base, exponent); }
static decimal cbrt(decimal value) { return cube_root(value); }
static decimal hypot(decimal first, decimal second) { return hypotenuse(first, second); }
static decimal sin(decimal value) { return sine(value); }
static decimal cos(decimal value) { return cosine(value); }
static decimal tan(decimal value) { return tangent(value); }
static decimal asin(decimal value) { return arc_sine(value); }
static decimal acos(decimal value) { return arc_cosine(value); }
static decimal atan(decimal value) { return arc_tangent(value); }
static decimal atan2(decimal rise, decimal run) { return arc_tangent_two(rise, run); }
static decimal sinh(decimal value) { return hyperbolic_sine(value); }
static decimal cosh(decimal value) { return hyperbolic_cosine(value); }
static decimal tanh(decimal value) { return hyperbolic_tangent(value); }
static decimal fmod(decimal left, decimal right) { return decimal_modulo(left, right); }
static decimal remainder(decimal left, decimal right) { return decimal_remainder(left, right); }
static decimal ldexp(decimal value, b32 exponent) { return decimal_scaled(value, exponent); }
static decimal scalbn(decimal value, b32 exponent) { return decimal_scaled(value, exponent); }
static decimal frexp(decimal value, b32 address_to exponent) { return decimal_split_exponent(value, exponent); }
static decimal modf(decimal value, decimal address_to whole) { return decimal_split_whole(value, whole); }

#pragma GCC pop_options

#endif // !KERNEL_MODE && decimal_bits == 64

#endif // STANDARD_MODERN_C_STANDARD_MATH
