/*
        awk.

        Its own file because it is its own language: a lexer, a parser, an
        expression evaluator, fields, associative arrays and output formatting.
        Nothing else here needs any of that, and everything else here would
        have to be read around it.

        The reference is the awk on the machine, which is gawk here, and where
        gawk and the standard disagree the tests say which one this followed.
        The regular expression machine is text.c's, above this in the same
        translation unit; there is no second one here.
*/

#define AWK_CHUNK (4u << 20)
#define AWK_CLASSES 22
#define AWK_MIN_CLASS 4

static p8 address_to awk_free_list[AWK_CLASSES];
static p8 address_to awk_bump;
static positive awk_bump_left;

static fn awk_leave(b32 code);

static fn awk_out_of_memory()
{
        text_error(null, "out of memory");
        awk_leave(2);
}

/*
        One allocator, because awk makes garbage.

        Every string here is counted rather than collected, so a size class
        list and a bump pointer are the whole of it: what a block was is
        remembered in the eight bytes before it and freeing pushes it back on
        the list its size came from.
*/
static address_any awk_take(positive bytes)
{
        positive want = bytes + 8;
        b32 class = AWK_MIN_CLASS;

        while (((positive)1 << class) < want && class < AWK_CLASSES - 1)
                class++;

        positive size = (positive)1 << class;

        if (want > size)
        {
                // Bigger than the largest class: its own mapping, and the
                // class recorded as one past the end so it is never reused.
                positive whole = (want + 4095) & ~(positive)4095;
                positive got = (positive)memory(whole);

                if (!got || got >= (positive)-4095)
                        awk_out_of_memory();

                address_to(p32 address_to)(positive)got = AWK_CLASSES;
                address_to((p32 address_to)(got + 4)) = 0;
                return (address_any)(got + 8);
        }

        if (awk_free_list[class])
        {
                p8 address_to block = awk_free_list[class];

                awk_free_list[class] = address_to(p8 address_to address_to)block;
                address_to(p32 address_to)block = (p32)class;
                return block + 8;
        }

        if (awk_bump_left < size)
        {
                positive got = (positive)memory(AWK_CHUNK);

                if (!got || got >= (positive)-4095)
                        awk_out_of_memory();

                awk_bump = (p8 address_to)got;
                awk_bump_left = AWK_CHUNK;
        }

        p8 address_to block = awk_bump;

        awk_bump += size;
        awk_bump_left -= size;
        address_to(p32 address_to)block = (p32)class;
        return block + 8;
}

static fn awk_give(address_any block)
{
        if (!block)
                return;

        p8 address_to base = (p8 address_to)block - 8;
        b32 class = (b32)address_to(p32 address_to)base;

        if (class >= AWK_CLASSES)
                return;

        address_to(p8 address_to address_to)base = awk_free_list[class];
        awk_free_list[class] = base;
}

/*
        Strings, counted.

        A value holds a pointer to one of these rather than a copy, so passing
        a field to a function is a store and an increment. The one that is
        empty is a single object everything shares.
*/
typedef struct
{
        b32 refs;
        positive length;
        p8 text[1];
} awk_text;

static awk_text awk_empty_text = {1000000000, 0, {0}};

static awk_text address_to awk_text_room(positive length)
{
        awk_text address_to made = (awk_text address_to)awk_take(sizeof(awk_text) + length + 8);

        made->refs = 1;
        made->length = length;
        made->text[length] = end;
        return made;
}

static awk_text address_to awk_text_new(string_address from, positive length)
{
        if (!length)
        {
                awk_empty_text.refs++;
                return address_of awk_empty_text;
        }

        awk_text address_to made = awk_text_room(length);

        memory_copy(made->text, from, length);
        return made;
}

static awk_text address_to awk_text_hold(awk_text address_to which)
{
        if (which)
                which->refs++;

        return which;
}

static fn awk_text_drop(awk_text address_to which)
{
        if (!which)
                return;

        if (--which->refs > 0)
                return;

        awk_give(which);
}

/*
        Arithmetic, written out.

        There is no libm under this and no libgcc either, so the seven
        functions POSIX asks awk for are here, along with the two conversions
        between a double and its decimal spelling. The conversions are exact:
        a double is a finite decimal, and printing 1e300 in full is what the
        reference does.
*/
static positive awk_bits_of(decimal value)
{
        positive bits;

        memory_copy(address_of bits, address_of value, 8);
        return bits;
}

static decimal awk_from_bits(positive bits)
{
        decimal value;

        memory_copy(address_of value, address_of bits, 8);
        return value;
}

static decimal awk_infinity = 0;
static decimal awk_not_a_number = 0;

static bool awk_is_nan(decimal value)
{
        return value != value;
}

static bool awk_is_finite(decimal value)
{
        positive bits = awk_bits_of(value);

        return ((bits >> 52) & 0x7ff) != 0x7ff;
}

static bool awk_negative(decimal value)
{
        return (awk_bits_of(value) >> 63) != 0;
}

static decimal awk_scale2(decimal value, b32 power)
{
        while (power > 1000)
        {
                value *= awk_from_bits((positive)(1023 + 1000) << 52);
                power -= 1000;
        }

        while (power < -1000)
        {
                value *= awk_from_bits((positive)(1023 - 1000) << 52);
                power += 1000;
        }

        if (power >= -1022)
                return value * awk_from_bits((positive)(1023 + power) << 52);

        // Subnormal territory, where the exponent field cannot hold it.
        value *= awk_from_bits((positive)(1023 - 1000) << 52);
        power += 1000;
        return value * awk_from_bits((positive)(1023 + power) << 52);
}

static decimal awk_absolute(decimal value)
{
        return value < 0 ? -value : value;
}

static decimal awk_truncate(decimal value)
{
        if (!awk_is_finite(value) || awk_is_nan(value))
                return value;

        if (awk_absolute(value) >= 9223372036854775808.0)
                return value;

        return (decimal)(bipolar)value;
}

static decimal awk_sqrt(decimal value)
{
        if (awk_is_nan(value) || value < 0)
                return awk_not_a_number;

        if (value == 0 || !awk_is_finite(value))
                return value;

        positive bits = awk_bits_of(value);
        b32 exponent = (b32)((bits >> 52) & 0x7ff);
        decimal mantissa;

        if (!exponent)
        {
                // A subnormal has no leading one; lift it into range first.
                value = awk_scale2(value, 200);
                bits = awk_bits_of(value);
                exponent = (b32)((bits >> 52) & 0x7ff);
                exponent -= 200;
        }

        b32 power = exponent - 1023;

        mantissa = awk_from_bits((bits & (((positive)1 << 52) - 1)) |
                                 ((positive)1023 << 52));

        if (power & 1)
        {
                mantissa *= 2;
                power--;
        }

        decimal guess = (mantissa + 1) * 0.5;

        for (b32 i = 0; i < 7; i++)
                guess = 0.5 * (guess + mantissa / guess);

        return awk_scale2(guess, power / 2);
}

#define AWK_LN2_HIGH 0.693147180369123816490
#define AWK_LN2_LOW 1.90821492927058770002e-10
#define AWK_PI 3.14159265358979311600
#define AWK_PI_HALF 1.57079632679489655800

static decimal awk_exp(decimal value)
{
        if (awk_is_nan(value))
                return value;

        if (value > 709.782712893384)
                return awk_infinity;

        if (value < -745.2)
                return 0;

        decimal scaled = value * 1.44269504088896338700;
        b32 k = (b32)(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
        decimal r = value - (decimal)k * AWK_LN2_HIGH - (decimal)k * AWK_LN2_LOW;
        decimal term = 1;
        decimal sum = 1;

        for (b32 i = 1; i <= 15; i++)
        {
                term *= r / (decimal)i;
                sum += term;
        }

        return awk_scale2(sum, k);
}

static decimal awk_log(decimal value)
{
        if (awk_is_nan(value))
                return value;

        if (value < 0)
                return awk_not_a_number;

        if (value == 0)
                return -awk_infinity;

        if (!awk_is_finite(value))
                return value;

        positive bits = awk_bits_of(value);
        b32 exponent = (b32)((bits >> 52) & 0x7ff);

        if (!exponent)
        {
                value = awk_scale2(value, 200);
                bits = awk_bits_of(value);
                exponent = (b32)((bits >> 52) & 0x7ff) - 200;
        }

        b32 power = exponent - 1023;
        decimal mantissa = awk_from_bits((bits & (((positive)1 << 52) - 1)) |
                                         ((positive)1023 << 52));

        if (mantissa > 1.41421356237309514547)
        {
                mantissa *= 0.5;
                power++;
        }

        decimal s = (mantissa - 1) / (mantissa + 1);
        decimal square = s * s;
        decimal term = s;
        decimal sum = s;

        for (b32 i = 3; i <= 27; i += 2)
        {
                term *= square;
                sum += term / (decimal)i;
        }

        return 2 * sum + (decimal)power * AWK_LN2_HIGH + (decimal)power * AWK_LN2_LOW;
}

// Reduced by halves of pi in three pieces, which holds to the last bit out to
// about a billion and drifts slowly after that.
#define AWK_PIO2_1 1.57079632673412561417e+00
#define AWK_PIO2_2 6.07710050650619224932e-11
#define AWK_PIO2_3 2.02226624879595063154e-21

static decimal awk_sin_kernel(decimal r)
{
        decimal square = r * r;
        decimal term = r;
        decimal sum = r;

        for (b32 i = 3; i <= 19; i += 2)
        {
                term *= -square / (decimal)(i * (i - 1));
                sum += term;
        }

        return sum;
}

static decimal awk_cos_kernel(decimal r)
{
        decimal square = r * r;
        decimal term = 1;
        decimal sum = 1;

        for (b32 i = 2; i <= 20; i += 2)
        {
                term *= -square / (decimal)(i * (i - 1));
                sum += term;
        }

        return sum;
}

static b32 awk_reduce_quarter(decimal value, decimal address_to rest)
{
        decimal scaled = value * 0.63661977236758138243;
        decimal rounded = scaled < 0 ? scaled - 0.5 : scaled + 0.5;

        rounded = awk_truncate(rounded);

        decimal r = value - rounded * AWK_PIO2_1;

        r -= rounded * AWK_PIO2_2;
        r -= rounded * AWK_PIO2_3;
        address_to rest = r;

        bipolar quarter = (bipolar)rounded;

        return (b32)(quarter & 3);
}

static decimal awk_sin(decimal value)
{
        if (awk_is_nan(value) || !awk_is_finite(value))
                return awk_not_a_number;

        decimal r;
        b32 quarter = awk_reduce_quarter(value, address_of r);

        switch (quarter)
        {
        case 0: return awk_sin_kernel(r);
        case 1: return awk_cos_kernel(r);
        case 2: return -awk_sin_kernel(r);
        }

        return -awk_cos_kernel(r);
}

static decimal awk_cos(decimal value)
{
        if (awk_is_nan(value) || !awk_is_finite(value))
                return awk_not_a_number;

        decimal r;
        b32 quarter = awk_reduce_quarter(value, address_of r);

        switch (quarter)
        {
        case 0: return awk_cos_kernel(r);
        case 1: return -awk_sin_kernel(r);
        case 2: return -awk_cos_kernel(r);
        }

        return awk_sin_kernel(r);
}

static decimal awk_atan_small(decimal x)
{
        decimal square = x * x;
        decimal term = x;
        decimal sum = x;

        for (b32 i = 3; i <= 61; i += 2)
        {
                term *= -square;
                sum += term / (decimal)i;
        }

        return sum;
}

static decimal awk_atan(decimal x)
{
        bool negative = x < 0;
        decimal value = negative ? -x : x;
        decimal answer;

        if (awk_is_nan(x))
                return x;

        if (!awk_is_finite(value))
                answer = AWK_PI_HALF;
        else if (value > 1)
        {
                decimal inner = 1 / value;

                if (inner > 0.19891236737965800691)
                        answer = AWK_PI_HALF - (AWK_PI / 8 +
                                                awk_atan_small((inner - 0.41421356237309503) /
                                                               (1 + 0.41421356237309503 * inner)));
                else
                        answer = AWK_PI_HALF - awk_atan_small(inner);
        }
        else if (value > 0.19891236737965800691)
                answer = AWK_PI / 8 + awk_atan_small((value - 0.41421356237309503) /
                                                     (1 + 0.41421356237309503 * value));
        else
                answer = awk_atan_small(value);

        return negative ? -answer : answer;
}

static decimal awk_atan2(decimal y, decimal x)
{
        if (awk_is_nan(y) || awk_is_nan(x))
                return awk_not_a_number;

        if (x == 0 && y == 0)
                return awk_negative(x) ? (awk_negative(y) ? -AWK_PI : AWK_PI)
                                       : (awk_negative(y) ? -0.0 : 0.0);

        if (x == 0)
                return y > 0 ? AWK_PI_HALF : -AWK_PI_HALF;

        decimal base = awk_atan(y / x);

        if (x > 0)
                return base;

        return y >= 0 ? base + AWK_PI : base - AWK_PI;
}

/*
        The remainder, taken by subtraction rather than by division.

        Every subtraction below is between two numbers within a factor of two
        of each other, which floating point does exactly, so the answer is the
        one the machine's own instruction would give.
*/
static decimal awk_remainder(decimal x, decimal y)
{
        if (awk_is_nan(x) || awk_is_nan(y) || y == 0 || !awk_is_finite(x))
                return awk_not_a_number;

        bool negative = x < 0;
        decimal left = negative ? -x : x;
        decimal divisor = y < 0 ? -y : y;

        if (!awk_is_finite(divisor))
                return x;

        if (left < divisor)
                return x;

        b32 steps = 0;

        while (awk_scale2(divisor, steps + 1) <= left && steps < 2200)
                steps++;

        for (b32 i = steps; i >= 0; i--)
        {
                decimal piece = awk_scale2(divisor, i);

                if (piece <= left)
                        left -= piece;
        }

        return negative ? -left : left;
}

static decimal awk_power(decimal base, decimal exponent)
{
        if (exponent == 0)
                return 1;

        if (awk_is_nan(base) || awk_is_nan(exponent))
                return awk_not_a_number;

        decimal whole = awk_truncate(exponent);

        if (whole == exponent && awk_absolute(exponent) <= 4096)
        {
                bool invert = exponent < 0;
                positive count = (positive)(invert ? -whole : whole);
                decimal result = 1;
                decimal factor = base;

                while (count)
                {
                        if (count & 1)
                                result *= factor;

                        count >>= 1;

                        if (count)
                                factor *= factor;
                }

                return invert ? 1 / result : result;
        }

        if (base < 0)
                return awk_not_a_number;

        if (base == 0)
                return exponent < 0 ? awk_infinity : 0;

        return awk_exp(exponent * awk_log(base));
}

/*
        A double, spelled out exactly.

        m * 2 ^ e is a finite decimal for every finite double, so the digits
        are generated rather than approximated: the whole part by dividing a
        wide integer down by a billion at a time, the fraction by multiplying
        the leftover bits by ten and taking what rises above the point. That
        is what makes print 1e300 the same three hundred and nine digits the
        reference prints, and it is what makes the rounding at a cut the same
        rounding: to nearest, and to even when the rest is exactly a half.
*/
#define AWK_LIMBS 40
#define AWK_STREAM_MAX 1200

typedef struct
{
        p32 limb[AWK_LIMBS];
        b32 count;
} awk_big;

static fn awk_big_set(awk_big address_to big, positive value)
{
        big->count = 0;

        while (value)
        {
                big->limb[big->count++] = (p32)value;
                value >>= 32;
        }
}

static fn awk_big_shift(awk_big address_to big, b32 bits)
{
        b32 words = bits / 32;
        b32 rest = bits % 32;

        if (words)
        {
                for (b32 i = big->count - 1; i >= 0; i--)
                        if (i + words < AWK_LIMBS)
                                big->limb[i + words] = big->limb[i];

                for (b32 i = 0; i < words && i < AWK_LIMBS; i++)
                        big->limb[i] = 0;

                big->count += words;

                if (big->count > AWK_LIMBS)
                        big->count = AWK_LIMBS;
        }

        if (!rest)
                return;

        p32 carry = 0;

        for (b32 i = 0; i < big->count; i++)
        {
                p64 value = ((p64)big->limb[i] << rest) | carry;

                big->limb[i] = (p32)value;
                carry = (p32)(value >> 32);
        }

        if (carry && big->count < AWK_LIMBS)
                big->limb[big->count++] = carry;
}

static p32 awk_big_divide(awk_big address_to big, p32 by)
{
        p64 rest = 0;

        for (b32 i = big->count - 1; i >= 0; i--)
        {
                p64 current = (rest << 32) | big->limb[i];

                big->limb[i] = (p32)(current / by);
                rest = current % by;
        }

        while (big->count && !big->limb[big->count - 1])
                big->count--;

        return (p32)rest;
}

static fn awk_big_multiply(awk_big address_to big, p32 by)
{
        p64 carry = 0;

        for (b32 i = 0; i < big->count; i++)
        {
                p64 value = (p64)big->limb[i] * by + carry;

                big->limb[i] = (p32)value;
                carry = value >> 32;
        }

        while (carry && big->count < AWK_LIMBS)
        {
                big->limb[big->count++] = (p32)carry;
                carry >>= 32;
        }
}

// What rose above the binary point, taken and cleared in one go.
static p32 awk_big_carry(awk_big address_to big, b32 point)
{
        b32 word = point / 32;
        b32 bit = point % 32;
        p32 high = 0;

        if (word < big->count)
        {
                high = big->limb[word] >> bit;
                big->limb[word] &= bit ? (((p32)1 << bit) - 1) : 0;
        }

        if (bit && word + 1 < big->count)
        {
                high |= big->limb[word + 1] << (32 - bit);
                big->limb[word + 1] = 0;
        }

        for (b32 i = word + 2; i < big->count; i++)
                big->limb[i] = 0;

        while (big->count && !big->limb[big->count - 1])
                big->count--;

        return high;
}

static p8 awk_stream[AWK_STREAM_MAX];
static b32 awk_stream_count;
static b32 awk_stream_point;
static awk_big awk_stream_frac;
static b32 awk_stream_bits;
static bool awk_stream_over;

static fn awk_stream_begin(decimal value)
{
        positive bits = awk_bits_of(value);
        b32 exponent = (b32)((bits >> 52) & 0x7ff);
        positive mantissa = bits & (((positive)1 << 52) - 1);
        b32 power;
        awk_big whole;

        if (value < 0)
                value = -value;

        if (!exponent)
                power = -1074;
        else
        {
                mantissa |= (positive)1 << 52;
                power = exponent - 1075;
        }

        awk_stream_count = 0;
        awk_stream_over = true;
        awk_stream_bits = 0;
        awk_stream_frac.count = 0;

        if (power >= 0)
        {
                awk_big_set(address_of whole, mantissa);
                awk_big_shift(address_of whole, power);
        }
        else
        {
                b32 shift = -power;

                awk_big_set(address_of whole, shift < 64 ? mantissa >> shift : 0);

                positive kept = shift < 64 ? mantissa & (((positive)1 << shift) - 1)
                                           : mantissa;

                if (kept)
                {
                        awk_big_set(address_of awk_stream_frac, kept);
                        awk_stream_bits = shift;
                        awk_stream_over = false;
                }
        }

        p8 groups[400];
        b32 have = 0;

        if (!whole.count)
                groups[have++] = '0';

        while (whole.count)
        {
                p32 rest = awk_big_divide(address_of whole, 1000000000u);

                for (b32 i = 0; i < 9; i++)
                {
                        groups[have++] = (p8)('0' + rest % 10);
                        rest /= 10;
                }
        }

        while (have > 1 && groups[have - 1] == '0')
                have--;

        for (b32 i = 0; i < have; i++)
                awk_stream[i] = groups[have - 1 - i];

        awk_stream_count = have;
        awk_stream_point = have;
}

static fn awk_stream_need(b32 want)
{
        if (want > AWK_STREAM_MAX - 2)
                want = AWK_STREAM_MAX - 2;

        while (awk_stream_count < want)
        {
                if (awk_stream_over)
                {
                        awk_stream[awk_stream_count++] = '0';
                        continue;
                }

                awk_big_multiply(address_of awk_stream_frac, 10);
                awk_stream[awk_stream_count++] =
                    (p8)('0' + awk_big_carry(address_of awk_stream_frac, awk_stream_bits));

                if (!awk_stream_frac.count)
                        awk_stream_over = true;
        }
}

// Keeps cut digits and rounds what follows into them.
static fn awk_stream_round(b32 cut)
{
        if (cut < 0)
                cut = 0;

        awk_stream_need(cut + 1);

        p8 next = awk_stream[cut];
        bool rest = !awk_stream_over;

        for (b32 i = cut + 1; i < awk_stream_count && !rest; i++)
                if (awk_stream[i] != '0')
                        rest = true;

        bool up = next > '5' ||
                  (next == '5' && (rest || (cut > 0 && ((awk_stream[cut - 1] - '0') & 1))));

        awk_stream_count = cut;

        if (!up)
                return;

        for (b32 i = cut - 1; i >= 0; i--)
        {
                if (awk_stream[i] != '9')
                {
                        awk_stream[i]++;
                        return;
                }

                awk_stream[i] = '0';
        }

        for (b32 i = cut; i > 0; i--)
                awk_stream[i] = awk_stream[i - 1];

        awk_stream[0] = '1';
        awk_stream_count = cut + 1;
        awk_stream_point++;
}

// The reference spells these with a sign, always, wherever they are written.
static string_address awk_not_finite_name(decimal value)
{
        if (awk_is_nan(value))
                return awk_negative(value) ? "-nan" : "+nan";

        return value < 0 ? "-inf" : "+inf";
}

static positive awk_write_fixed(decimal value, b32 precision, p8 address_to out, bool point_always)
{
        positive at = 0;

        awk_stream_begin(value);
        awk_stream_round(awk_stream_point + precision);

        b32 whole = awk_stream_point;

        if (whole <= 0)
                out[at++] = '0';
        else
                for (b32 i = 0; i < whole; i++)
                        out[at++] = awk_stream[i];

        if (precision > 0 || point_always)
                out[at++] = '.';

        for (b32 i = 0; i < precision; i++)
        {
                b32 which = whole + i;

                out[at++] = which >= 0 && which < awk_stream_count ? awk_stream[which] : '0';
        }

        out[at] = end;
        return at;
}

static fn awk_write_exponent(p8 address_to out, positive address_to at, b32 exponent, bool upper)
{
        positive where = address_to at;
        b32 magnitude = exponent < 0 ? -exponent : exponent;
        p8 digits[8];
        b32 have = 0;

        out[where++] = upper ? 'E' : 'e';
        out[where++] = exponent < 0 ? '-' : '+';

        while (magnitude)
        {
                digits[have++] = (p8)('0' + magnitude % 10);
                magnitude /= 10;
        }

        while (have < 2)
                digits[have++] = '0';

        while (have)
                out[where++] = digits[--have];

        address_to at = where;
}

// The first digit that is not a zero, and where the point stands relative to
// it. A value of zero has none, and answers with a zero exponent.
static b32 awk_stream_first()
{
        b32 first = 0;

        for (;;)
        {
                awk_stream_need(first + 1);

                if (awk_stream[first] != '0')
                        return first;

                if (awk_stream_over && first >= awk_stream_count - 1)
                        return -1;

                first++;

                if (first > AWK_STREAM_MAX - 8)
                        return -1;
        }
}

static positive awk_write_scientific(decimal value, b32 precision, p8 address_to out,
                                     bool upper, bool point_always)
{
        positive at = 0;

        if (value == 0)
        {
                out[at++] = '0';

                if (precision > 0 || point_always)
                        out[at++] = '.';

                for (b32 i = 0; i < precision; i++)
                        out[at++] = '0';

                awk_write_exponent(out, address_of at, 0, upper);
                out[at] = end;
                return at;
        }

        awk_stream_begin(value);

        b32 first = awk_stream_first();

        awk_stream_round(first + precision + 1);

        for (first = 0; first < awk_stream_count && awk_stream[first] == '0'; first++)
                ;

        b32 exponent = awk_stream_point - 1 - first;

        out[at++] = awk_stream[first];

        if (precision > 0 || point_always)
                out[at++] = '.';

        for (b32 i = 1; i <= precision; i++)
                out[at++] = first + i < awk_stream_count ? awk_stream[first + i] : '0';

        awk_write_exponent(out, address_of at, exponent, upper);
        out[at] = end;
        return at;
}

static positive awk_write_general(decimal value, b32 precision, p8 address_to out,
                                  bool upper, bool keep_zeros)
{
        positive length;
        b32 exponent = 0;

        if (precision < 1)
                precision = 1;

        if (value != 0)
        {
                awk_stream_begin(value);

                b32 first = awk_stream_first();

                awk_stream_round(first + precision);

                for (first = 0; first < awk_stream_count && awk_stream[first] == '0'; first++)
                        ;

                exponent = awk_stream_point - 1 - first;
        }

        if (exponent < -4 || exponent >= precision)
                length = awk_write_scientific(value, precision - 1, out, upper, keep_zeros);
        else
                length = awk_write_fixed(value, precision - 1 - exponent, out, keep_zeros);

        if (keep_zeros)
                return length;

        // Trailing zeros go, and the point with them if nothing is left after
        // it, but only in the part before any exponent.
        positive stop = 0;
        bool dotted = false;

        while (stop < length && out[stop] != 'e' && out[stop] != 'E')
        {
                if (out[stop] == '.')
                        dotted = true;

                stop++;
        }

        if (!dotted)
                return length;

        positive cut = stop;

        while (cut > 0 && out[cut - 1] == '0')
                cut--;

        if (cut > 0 && out[cut - 1] == '.')
                cut--;

        if (cut == stop)
                return length;

        for (positive i = stop; i < length; i++)
                out[cut++] = out[i];

        out[cut] = end;
        return cut;
}

/*
        Ten to a power, kept to twice a double's precision.

        A mantissa multiplied by a power of ten in plain double arithmetic is
        a few units in the last place off by the time the power reaches a
        hundred, and 1e300 then reads as a different double than the one the
        reference read -- visible, because an integral value is printed in
        full. Two doubles carrying one number is enough precision that the
        rounding back down to one lands where it should.
*/
typedef struct
{
        decimal high;
        decimal low;
} awk_wide;

static awk_wide awk_wide_of(decimal value)
{
        awk_wide made;

        made.high = value;
        made.low = 0;
        return made;
}

// The exact sum of two doubles, given the first is the larger.
static awk_wide awk_wide_join(decimal a, decimal b)
{
        awk_wide made;

        made.high = a + b;
        made.low = b - (made.high - a);
        return made;
}

static awk_wide awk_wide_product(decimal a, decimal b)
{
        decimal cut = 134217729.0;
        decimal ca = cut * a;
        decimal ah = ca - (ca - a);
        decimal al = a - ah;
        decimal cb = cut * b;
        decimal bh = cb - (cb - b);
        decimal bl = b - bh;
        awk_wide made;

        made.high = a * b;
        made.low = ((ah * bh - made.high) + ah * bl + al * bh) + al * bl;
        return made;
}

static awk_wide awk_wide_add(awk_wide a, awk_wide b)
{
        decimal sum = a.high + b.high;
        decimal part = sum - a.high;
        decimal error = (a.high - (sum - part)) + (b.high - part);

        return awk_wide_join(sum, error + a.low + b.low);
}

static awk_wide awk_wide_multiply(awk_wide a, awk_wide b)
{
        awk_wide made = awk_wide_product(a.high, b.high);

        made.low += a.high * b.low + a.low * b.high;
        return awk_wide_join(made.high, made.low);
}

static awk_wide awk_wide_negate(awk_wide a)
{
        a.high = -a.high;
        a.low = -a.low;
        return a;
}

static awk_wide awk_wide_divide(awk_wide a, awk_wide b)
{
        decimal first = a.high / b.high;
        awk_wide rest = awk_wide_add(a, awk_wide_negate(awk_wide_multiply(b,
                                                                          awk_wide_of(first))));
        decimal second = rest.high / b.high;

        rest = awk_wide_add(rest, awk_wide_negate(awk_wide_multiply(b, awk_wide_of(second))));

        decimal third = rest.high / b.high;

        return awk_wide_add(awk_wide_join(first, second), awk_wide_of(third));
}

static awk_wide awk_wide_ten(b32 power)
{
        awk_wide answer = awk_wide_of(1);
        awk_wide factor = awk_wide_of(10);

        while (power)
        {
                if (power & 1)
                        answer = awk_wide_multiply(answer, factor);

                power >>= 1;

                if (power)
                        factor = awk_wide_multiply(factor, factor);
        }

        return answer;
}

static decimal awk_scale_ten(positive mantissa, b32 power)
{
        awk_wide value = awk_wide_add(awk_wide_product((decimal)(mantissa >> 32), 4294967296.0),
                                      awk_wide_of((decimal)(mantissa & 0xffffffffu)));

        if (!power)
                return value.high + value.low;

        if (power > 340)
                return mantissa ? awk_infinity : 0;

        if (power < -400)
                return 0;

        if (power > 0)
        {
                value = awk_wide_multiply(value, awk_wide_ten(power));
                return value.high + value.low;
        }

        b32 want = -power;

        // Ten to the four hundredth is not a double, so a division that far
        // down is done in two halves.
        if (want > 300)
        {
                value = awk_wide_divide(value, awk_wide_ten(150));
                want -= 150;
        }

        value = awk_wide_divide(value, awk_wide_ten(want));
        return value.high + value.low;
}

/*
        A number read out of a string, and how much of the string it took.

        Every rule about what counts as a number here is asked through this
        one: a field is a number to compare against a number only when this
        reaches the end of it.
*/
static decimal awk_scan_number(string_address text, positive length, positive address_to used)
{
        positive at = 0;
        bool negative = false;
        positive mantissa = 0;
        b32 digits = 0;
        b32 exponent = 0;
        bool any = false;

        address_to used = 0;

        while (at < length && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'))
                at++;

        if (at < length && (text[at] == '+' || text[at] == '-'))
        {
                negative = text[at] == '-';
                at++;
        }

        while (at < length && text[at] >= '0' && text[at] <= '9')
        {
                any = true;

                if (digits < 19)
                {
                        mantissa = mantissa * 10 + (positive)(text[at] - '0');
                        digits++;
                }
                else
                        exponent++;

                at++;
        }

        if (at < length && text[at] == '.')
        {
                at++;

                while (at < length && text[at] >= '0' && text[at] <= '9')
                {
                        any = true;

                        if (digits < 19)
                        {
                                mantissa = mantissa * 10 + (positive)(text[at] - '0');
                                digits++;
                                exponent--;
                        }

                        at++;
                }
        }

        if (!any)
                return 0;

        positive after = at;

        if (at < length && (text[at] == 'e' || text[at] == 'E'))
        {
                positive step = at + 1;
                bool minus = false;

                if (step < length && (text[step] == '+' || text[step] == '-'))
                {
                        minus = text[step] == '-';
                        step++;
                }

                if (step < length && text[step] >= '0' && text[step] <= '9')
                {
                        b32 value = 0;

                        while (step < length && text[step] >= '0' && text[step] <= '9')
                        {
                                if (value < 100000)
                                        value = value * 10 + (text[step] - '0');

                                step++;
                        }

                        exponent += minus ? -value : value;
                        after = step;
                }
        }

        address_to used = after;

        decimal value = awk_scale_ten(mantissa, exponent);

        return negative ? -value : value;
}

static bool awk_looks_numeric(string_address text, positive length)
{
        positive used;

        awk_scan_number(text, length, address_of used);

        if (!used)
                return false;

        while (used < length &&
               (text[used] == ' ' || text[used] == '\t' || text[used] == '\n'))
                used++;

        return used == length;
}

static decimal awk_number_of(string_address text, positive length)
{
        positive used;

        return awk_scan_number(text, length, address_of used);
}

/*
        Values.

        A value is a number, a string, or both, and which of those it is
        decides what a comparison means. The awkward one is the third state:
        something that came in from outside -- a field, a getline, an argument
        assignment -- and happens to look like a number. Those compare as
        numbers against numbers and as strings against string constants, and
        the difference is visible: a field holding 10.0 equals 10 and does not
        equal "10".
*/
enum
{
        AWK_HAS_NUMBER = 1,
        AWK_HAS_TEXT = 2,
        AWK_STRNUM = 4,
        AWK_UNSET = 8
};

typedef struct
{
        decimal number;
        awk_text address_to text;
        p8 state;
} awk_value;

static string_address awk_convfmt();
static string_address awk_ofmt();
static awk_text address_to awk_sprintf(string_address format, positive length,
                                       awk_value address_to arguments, b32 count);

static fn awk_value_clear(awk_value address_to which)
{
        awk_text_drop(which->text);
        which->text = null;
        which->number = 0;
        which->state = AWK_UNSET;
}

static fn awk_set_number(awk_value address_to which, decimal number)
{
        awk_text_drop(which->text);
        which->text = null;
        which->number = number;
        which->state = AWK_HAS_NUMBER;
}

// Takes the reference the caller was holding.
static fn awk_set_text(awk_value address_to which, awk_text address_to text)
{
        awk_text_drop(which->text);
        which->text = text;
        which->number = 0;
        which->state = AWK_HAS_TEXT;
}

static fn awk_set_bytes(awk_value address_to which, string_address from, positive length)
{
        awk_set_text(which, awk_text_new(from, length));
}

// What came from outside, which is a string that may also be a number.
static fn awk_set_input(awk_value address_to which, awk_text address_to text)
{
        awk_text_drop(which->text);
        which->text = text;
        which->number = 0;
        which->state = AWK_HAS_TEXT;

        if (awk_looks_numeric(text->text, text->length))
                which->state |= AWK_STRNUM;
}

static fn awk_set_input_bytes(awk_value address_to which, string_address from, positive length)
{
        awk_set_input(which, awk_text_new(from, length));
}

static fn awk_value_copy(awk_value address_to to, awk_value address_to from)
{
        if (to == from)
                return;

        awk_text address_to kept = awk_text_hold(from->text);

        awk_text_drop(to->text);
        to->text = kept;
        to->number = from->number;
        to->state = from->state;
}

static decimal awk_to_number(awk_value address_to which)
{
        if (which->state & AWK_HAS_NUMBER)
                return which->number;

        if (which->state & AWK_UNSET)
                return 0;

        which->number = awk_number_of(which->text->text, which->text->length);
        which->state |= AWK_HAS_NUMBER;
        return which->number;
}

/*
        A number spelled the way awk spells one.

        An integral value is written out in full whatever the format says --
        which is why 2^53 and 1e300 come out as digits rather than as 9e+15 --
        and everything else goes through CONVFMT or OFMT.
*/
static awk_text address_to awk_text_of_number(decimal number, string_address format)
{
        p8 room[512];

        if (!awk_is_finite(number) || awk_is_nan(number))
        {
                string_address name = awk_not_finite_name(number);

                return awk_text_new(name, string_length(name));
        }

        if (number == awk_truncate(number))
        {
                if (number > -1e18 && number < 1e18)
                {
                        bipolar whole = (bipolar)number;
                        positive magnitude = whole < 0 ? (positive)-whole : (positive)whole;
                        p8 digits[24];
                        b32 have = 0;
                        positive at = 0;

                        if (!magnitude)
                                digits[have++] = '0';

                        while (magnitude)
                        {
                                digits[have++] = (p8)('0' + magnitude % 10);
                                magnitude /= 10;
                        }

                        if (whole < 0)
                                room[at++] = '-';

                        while (have)
                                room[at++] = digits[--have];

                        return awk_text_new(room, at);
                }

                positive at = 0;

                if (number < 0)
                        room[at++] = '-';

                at += awk_write_fixed(number, 0, room + at, false);
                return awk_text_new(room, at);
        }

        awk_value one;

        one.text = null;
        one.number = number;
        one.state = AWK_HAS_NUMBER;

        awk_text address_to made = awk_sprintf(format, string_length(format), address_of one, 1);

        awk_text_drop(one.text);
        return made;
}

static awk_text address_to awk_to_text(awk_value address_to which)
{
        if (which->state & AWK_HAS_TEXT)
                return which->text;

        if (which->state & AWK_UNSET)
        {
                awk_empty_text.refs++;
                which->text = address_of awk_empty_text;
                which->state |= AWK_HAS_TEXT;
                return which->text;
        }

        which->text = awk_text_of_number(which->number, awk_convfmt());
        which->state |= AWK_HAS_TEXT;
        return which->text;
}

// What print writes, which differs from the above in one variable's name.
static awk_text address_to awk_to_output_text(awk_value address_to which)
{
        if (which->state & AWK_HAS_TEXT)
                return awk_text_hold(which->text);

        if (which->state & AWK_UNSET)
                return awk_text_hold(address_of awk_empty_text);

        return awk_text_of_number(which->number, awk_ofmt());
}

static bool awk_truth(awk_value address_to which)
{
        if (which->state & AWK_UNSET)
                return false;

        if (which->state & AWK_HAS_TEXT)
        {
                if (which->state & AWK_STRNUM)
                        return awk_to_number(which) != 0;

                return which->text->length != 0;
        }

        return which->number != 0;
}

static bool awk_numeric_side(awk_value address_to which)
{
        if (which->state & (AWK_UNSET | AWK_STRNUM))
                return true;

        return (which->state & AWK_HAS_NUMBER) && !(which->state & AWK_HAS_TEXT);
}

static b32 awk_compare(awk_value address_to left, awk_value address_to right)
{
        if (awk_numeric_side(left) && awk_numeric_side(right))
        {
                decimal a = awk_to_number(left);
                decimal b = awk_to_number(right);

                return a < b ? -1 : (a > b ? 1 : 0);
        }

        awk_text address_to a = awk_to_text(left);
        awk_text address_to b = awk_to_text(right);
        positive shortest = a->length < b->length ? a->length : b->length;

        for (positive i = 0; i < shortest; i++)
                if (a->text[i] != b->text[i])
                        return a->text[i] < b->text[i] ? -1 : 1;

        return a->length == b->length ? 0 : (a->length < b->length ? -1 : 1);
}

/*
        Arrays, which are the only kind awk has.

        Keys are strings even when they were written as numbers, and the
        spelling a number takes as a key is CONVFMT's -- except for an
        integral one, which is its digits. a[1] and a["1"] are one element.
*/
typedef struct awk_slot
{
        struct awk_slot address_to next;
        awk_text address_to key;
        awk_value value;
} awk_slot;

typedef struct
{
        awk_slot address_to address_to buckets;
        positive width;
        positive count;
} awk_array;

static positive awk_hash(string_address text, positive length)
{
        positive value = 5381;

        for (positive i = 0; i < length; i++)
                value = value * 33 + text[i];

        return value;
}

static awk_array address_to awk_array_new()
{
        awk_array address_to made = (awk_array address_to)awk_take(sizeof(awk_array));

        made->width = 16;
        made->count = 0;
        made->buckets = (awk_slot address_to address_to)awk_take(made->width * sizeof(address_any));

        for (positive i = 0; i < made->width; i++)
                made->buckets[i] = null;

        return made;
}

static fn awk_array_grow(awk_array address_to which)
{
        positive width = which->width * 4;
        awk_slot address_to address_to buckets =
            (awk_slot address_to address_to)awk_take(width * sizeof(address_any));

        for (positive i = 0; i < width; i++)
                buckets[i] = null;

        for (positive i = 0; i < which->width; i++)
        {
                awk_slot address_to slot = which->buckets[i];

                while (slot)
                {
                        awk_slot address_to next = slot->next;
                        positive where = awk_hash(slot->key->text, slot->key->length) & (width - 1);

                        slot->next = buckets[where];
                        buckets[where] = slot;
                        slot = next;
                }
        }

        awk_give(which->buckets);
        which->buckets = buckets;
        which->width = width;
}

static awk_slot address_to awk_array_find(awk_array address_to which, string_address key,
                                          positive length)
{
        positive where = awk_hash(key, length) & (which->width - 1);

        for (awk_slot address_to slot = which->buckets[where]; slot; slot = slot->next)
                if (slot->key->length == length &&
                    !memory_compare(slot->key->text, key, length))
                        return slot;

        return null;
}

static awk_slot address_to awk_array_place(awk_array address_to which, string_address key,
                                           positive length)
{
        awk_slot address_to found = awk_array_find(which, key, length);

        if (found)
                return found;

        if (which->count >= which->width * 2)
                awk_array_grow(which);

        positive where = awk_hash(key, length) & (which->width - 1);
        awk_slot address_to slot = (awk_slot address_to)awk_take(sizeof(awk_slot));

        slot->key = awk_text_new(key, length);
        slot->value.text = null;
        slot->value.number = 0;
        slot->value.state = AWK_UNSET;
        slot->next = which->buckets[where];
        which->buckets[where] = slot;
        which->count++;
        return slot;
}

static fn awk_array_remove(awk_array address_to which, string_address key, positive length)
{
        positive where = awk_hash(key, length) & (which->width - 1);
        awk_slot address_to address_to link = address_of which->buckets[where];

        while (address_to link)
        {
                awk_slot address_to slot = address_to link;

                if (slot->key->length == length && !memory_compare(slot->key->text, key, length))
                {
                        address_to link = slot->next;
                        awk_text_drop(slot->key);
                        awk_text_drop(slot->value.text);
                        awk_give(slot);
                        which->count--;
                        return;
                }

                link = address_of slot->next;
        }
}

static fn awk_array_empty(awk_array address_to which)
{
        for (positive i = 0; i < which->width; i++)
        {
                awk_slot address_to slot = which->buckets[i];

                while (slot)
                {
                        awk_slot address_to next = slot->next;

                        awk_text_drop(slot->key);
                        awk_text_drop(slot->value.text);
                        awk_give(slot);
                        slot = next;
                }

                which->buckets[i] = null;
        }

        which->count = 0;
}

/*
        Variables.

        A name is resolved once, where the program is parsed, into either a
        slot in the current function's frame or a slot in the one global
        table. Nothing looks a name up while the program runs.

        Thirteen of the globals mean something to the machinery underneath,
        and assigning to those has to be noticed: NF rebuilds the record, RS
        changes what a record is, FS changes what a field is.
*/
enum
{
        AWK_CELL_UNKNOWN = 0,
        AWK_CELL_SCALAR,
        AWK_CELL_ARRAY
};

typedef struct awk_cell
{
        p8 kind;
        bool owned;
        struct awk_cell address_to link;
        awk_value value;
        awk_array address_to array;
} awk_cell;

enum
{
        AWK_PLAIN = 0,
        AWK_NR,
        AWK_NF,
        AWK_FNR,
        AWK_FS,
        AWK_OFS,
        AWK_ORS,
        AWK_RS,
        AWK_FILENAME,
        AWK_SUBSEP,
        AWK_RSTART,
        AWK_RLENGTH,
        AWK_CONVFMT,
        AWK_OFMT
};

#define AWK_GLOBALS_MAX 1024
#define AWK_FRAME_MAX 8192
#define AWK_LOCALS_MAX 128

static awk_cell awk_globals[AWK_GLOBALS_MAX];
static awk_text address_to awk_global_names[AWK_GLOBALS_MAX];
static p8 awk_global_meaning[AWK_GLOBALS_MAX];
static b32 awk_global_count;

static awk_cell awk_stack[AWK_FRAME_MAX];
static b32 awk_frame;
static b32 awk_frame_size;

static b32 awk_where_environ;
static b32 awk_where_argv;
static b32 awk_where_argc;

static fn awk_fatal(string_address about, string_address reason);

static b32 awk_global_find(string_address name, positive length)
{
        for (b32 i = 0; i < awk_global_count; i++)
                if (awk_global_names[i]->length == length &&
                    !memory_compare(awk_global_names[i]->text, name, length))
                        return i;

        if (awk_global_count == AWK_GLOBALS_MAX)
                awk_fatal(null, "too many variables");

        b32 which = awk_global_count++;

        awk_global_names[which] = awk_text_new(name, length);
        awk_globals[which].kind = AWK_CELL_UNKNOWN;
        awk_globals[which].value.state = AWK_UNSET;
        return which;
}

static awk_cell address_to awk_cell_of(b32 index)
{
        return index < 0 ? address_of awk_stack[awk_frame + (-index - 1)]
                         : address_of awk_globals[index];
}

static awk_array address_to awk_cell_array(awk_cell address_to cell)
{
        while (cell->link)
                cell = cell->link;

        if (!cell->array)
        {
                cell->array = awk_array_new();
                cell->owned = true;
        }

        cell->kind = AWK_CELL_ARRAY;
        return cell->array;
}

static string_address awk_global_string(b32 which)
{
        return awk_to_text(address_of awk_globals[which].value)->text;
}

static b32 awk_where_fs, awk_where_ofs, awk_where_ors, awk_where_rs;
static b32 awk_where_nr, awk_where_nf, awk_where_fnr, awk_where_filename;
static b32 awk_where_subsep, awk_where_rstart, awk_where_rlength;
static b32 awk_where_convfmt, awk_where_ofmt;

static string_address awk_convfmt()
{
        return awk_global_string(awk_where_convfmt);
}

static string_address awk_ofmt()
{
        return awk_global_string(awk_where_ofmt);
}

static awk_text address_to awk_special_text(b32 which)
{
        return awk_to_text(address_of awk_globals[which].value);
}

static fn awk_set_global_number(b32 which, decimal value)
{
        awk_set_number(address_of awk_globals[which].value, value);
}

static decimal awk_global_number(b32 which)
{
        return awk_to_number(address_of awk_globals[which].value);
}

/*
        Regular expressions, out of text.c's machine.

        The ones written in the program are compiled once and kept forever.
        The ones built at run time -- a string used where a pattern goes --
        share a small cache in the pool above the kept ones, and when it fills
        the pool is wound back to where the kept ones end rather than to zero,
        which is what keeps a program compiled at parse time valid for the
        whole run.
*/
#define AWK_REGEX_KEPT 32
#define AWK_REGEX_CACHED 6

static regex_program awk_regex_kept[AWK_REGEX_KEPT];
static b32 awk_regex_kept_count;

static regex_program awk_regex_cache[AWK_REGEX_CACHED];
static awk_text address_to awk_regex_cache_key[AWK_REGEX_CACHED];
static b32 awk_regex_cache_count;

static b32 awk_regex_mark_code;
static b32 awk_regex_mark_sets;
static b32 awk_regex_mark_first;

static fn awk_regex_build(regex_program address_to into, string_address pattern)
{
        if (!regex_compile(pattern, true, false, true))
                awk_fatal(pattern, "invalid regular expression");

        regex_keep(into);
}

static regex_program address_to awk_regex_keep(string_address pattern)
{
        if (awk_regex_kept_count == AWK_REGEX_KEPT)
                return null;

        regex_program address_to into = address_of awk_regex_kept[awk_regex_kept_count++];

        awk_regex_build(into, pattern);
        return into;
}

static fn awk_regex_mark()
{
        awk_regex_mark_code = regex_pool_used;
        awk_regex_mark_sets = regex_pool_sets;
        awk_regex_mark_first = regex_first_used;
}

static regex_program address_to awk_regex_dynamic(awk_text address_to pattern)
{
        for (b32 i = 0; i < awk_regex_cache_count; i++)
                if (awk_regex_cache_key[i]->length == pattern->length &&
                    !memory_compare(awk_regex_cache_key[i]->text, pattern->text,
                                    pattern->length))
                        return address_of awk_regex_cache[i];

        if (awk_regex_cache_count == AWK_REGEX_CACHED)
        {
                for (b32 i = 0; i < awk_regex_cache_count; i++)
                        awk_text_drop(awk_regex_cache_key[i]);

                awk_regex_cache_count = 0;
                regex_pool_used = awk_regex_mark_code;
                regex_pool_sets = awk_regex_mark_sets;
                regex_first_used = awk_regex_mark_first;
        }

        b32 which = awk_regex_cache_count++;

        awk_regex_cache_key[which] = awk_text_hold(pattern);
        awk_regex_build(address_of awk_regex_cache[which], pattern->text);
        return address_of awk_regex_cache[which];
}

/*
        The record and its fields.

        $0 and $1..$NF are one array of values with the record at zero, and
        either side can be the stale one: assigning to a field marks the
        record for rebuilding with OFS, assigning to the record splits it
        again. Everything a field holds arrived from outside, so every field
        is a value that may compare as a number.
*/
static awk_value address_to awk_fields;
static positive awk_fields_room;
static b32 awk_nf;
static bool awk_record_stale;
static awk_value awk_field_nothing;

static positive address_to awk_piece_start;
static positive address_to awk_piece_length;
static positive awk_piece_count;
static positive awk_piece_room;

static fn awk_pieces_room(positive want)
{
        if (want <= awk_piece_room)
                return;

        positive room = awk_piece_room ? awk_piece_room : 64;

        while (room < want)
                room *= 2;

        positive address_to starts = (positive address_to)awk_take(room * sizeof(positive));
        positive address_to lengths = (positive address_to)awk_take(room * sizeof(positive));

        for (positive i = 0; i < awk_piece_count; i++)
        {
                starts[i] = awk_piece_start[i];
                lengths[i] = awk_piece_length[i];
        }

        awk_give(awk_piece_start);
        awk_give(awk_piece_length);
        awk_piece_start = starts;
        awk_piece_length = lengths;
        awk_piece_room = room;
}

static fn awk_piece_add(positive start, positive length)
{
        awk_pieces_room(awk_piece_count + 1);
        awk_piece_start[awk_piece_count] = start;
        awk_piece_length[awk_piece_count] = length;
        awk_piece_count++;
}

static bool awk_is_blank(p8 character)
{
        return character == ' ' || character == '\t' || character == '\n';
}

/*
        One splitter, for fields and for split().

        A separator of one space is the rule nobody writes down the same way
        twice: leading and trailing blanks are not separators at all, and a
        run of them is one. Any other single character is itself and not a
        pattern -- -F. cuts on dots, not on everything.
*/
static fn awk_split_pieces(string_address text, positive length, string_address separator,
                           positive separator_length, bool paragraph, bool as_pattern)
{
        awk_piece_count = 0;

        if (separator_length == 1 && separator[0] == ' ')
        {
                positive at = 0;

                while (at < length)
                {
                        while (at < length && awk_is_blank(text[at]))
                                at++;

                        if (at == length)
                                break;

                        positive start = at;

                        while (at < length && !awk_is_blank(text[at]))
                                at++;

                        awk_piece_add(start, at - start);
                }

                return;
        }

        if (!length)
                return;

        if (!separator_length)
        {
                for (positive i = 0; i < length; i++)
                        awk_piece_add(i, 1);

                return;
        }

        if (separator_length == 1 && !as_pattern)
        {
                positive start = 0;

                for (positive at = 0; at < length; at++)
                        if (text[at] == separator[0] || (paragraph && text[at] == '\n'))
                        {
                                awk_piece_add(start, at - start);
                                start = at + 1;
                        }

                awk_piece_add(start, length - start);
                return;
        }

        awk_text address_to pattern = awk_text_new(separator, separator_length);
        regex_program address_to program = awk_regex_dynamic(pattern);
        positive start = 0;
        positive at = 0;

        awk_text_drop(pattern);
        regex_select(program);

        while (at < length)
        {
                positive cut = TEXT_UNSET;
                positive stop = 0;

                if (regex_search_longest(text, length, at))
                {
                        cut = regex_slots[0];
                        stop = regex_slots[1];

                        // An empty match separates nothing; it would cut the
                        // record at every byte and never move forward.
                        if (stop == cut)
                        {
                                cut = TEXT_UNSET;

                                for (positive scan = at; scan < length && cut == TEXT_UNSET; scan++)
                                        if (regex_search_longest(text, length, scan) &&
                                            regex_slots[1] > regex_slots[0])
                                        {
                                                cut = regex_slots[0];
                                                stop = regex_slots[1];
                                        }
                        }
                }

                if (paragraph)
                {
                        for (positive scan = at; scan < length; scan++)
                                if (text[scan] == '\n')
                                {
                                        if (cut == TEXT_UNSET || scan < cut)
                                        {
                                                cut = scan;
                                                stop = scan + 1;
                                        }

                                        break;
                                }
                }

                if (cut == TEXT_UNSET)
                        break;

                awk_piece_add(start, cut - start);
                start = stop;
                at = stop;
        }

        awk_piece_add(start, length - start);
}

static fn awk_fields_reserve(positive want)
{
        if (want < awk_fields_room)
                return;

        positive room = awk_fields_room ? awk_fields_room : 64;

        while (room <= want)
                room *= 2;

        awk_value address_to made = (awk_value address_to)awk_take(room * sizeof(awk_value));

        for (positive i = 0; i < room; i++)
        {
                made[i].text = null;
                made[i].number = 0;
                made[i].state = AWK_UNSET;
        }

        for (positive i = 0; i < awk_fields_room; i++)
                made[i] = awk_fields[i];

        awk_give(awk_fields);
        awk_fields = made;
        awk_fields_room = room;
}

static string_address awk_separator(b32 which, positive address_to length)
{
        awk_text address_to text = awk_special_text(which);

        address_to length = text->length;
        return text->text;
}

static bool awk_paragraph_mode()
{
        positive length;

        awk_separator(awk_where_rs, address_of length);
        return length == 0;
}

static fn awk_split_record()
{
        positive separator_length;
        string_address separator = awk_separator(awk_where_fs, address_of separator_length);
        awk_text address_to record = awk_to_text(address_of awk_fields[0]);

        awk_split_pieces(record->text, record->length, separator, separator_length,
                         awk_paragraph_mode(), false);

        awk_fields_reserve(awk_piece_count + 1);

        for (positive i = 0; i < awk_piece_count; i++)
                awk_set_input_bytes(address_of awk_fields[i + 1],
                                    record->text + awk_piece_start[i], awk_piece_length[i]);

        for (positive i = awk_piece_count + 1; i <= (positive)awk_nf; i++)
                awk_value_clear(address_of awk_fields[i]);

        awk_nf = (b32)awk_piece_count;
        awk_set_global_number(awk_where_nf, (decimal)awk_nf);
        awk_record_stale = false;
}

static fn awk_record_rebuild()
{
        positive length;
        string_address separator = awk_separator(awk_where_ofs, address_of length);
        positive total = 0;

        for (b32 i = 1; i <= awk_nf; i++)
                total += awk_to_text(address_of awk_fields[i])->length + length;

        awk_text address_to made = awk_text_room(total ? total : 1);
        positive at = 0;

        for (b32 i = 1; i <= awk_nf; i++)
        {
                awk_text address_to piece = awk_to_text(address_of awk_fields[i]);

                if (i > 1)
                {
                        memory_copy(made->text + at, separator, length);
                        at += length;
                }

                memory_copy(made->text + at, piece->text, piece->length);
                at += piece->length;
        }

        made->length = at;
        made->text[at] = end;
        awk_set_input(address_of awk_fields[0], made);
        awk_record_stale = false;
}

static fn awk_record_set(string_address text, positive length)
{
        awk_fields_reserve(1);
        awk_set_input_bytes(address_of awk_fields[0], text, length);
        awk_split_record();
}

static awk_value address_to awk_field(b32 which)
{
        if (which < 0)
                awk_fatal(null, "attempt to access a field before the first");

        if (!which)
        {
                if (awk_record_stale)
                        awk_record_rebuild();

                awk_fields_reserve(1);
                return address_of awk_fields[0];
        }

        if (which > awk_nf)
        {
                awk_value_clear(address_of awk_field_nothing);
                awk_set_bytes(address_of awk_field_nothing, "", 0);
                return address_of awk_field_nothing;
        }

        return address_of awk_fields[which];
}

static fn awk_field_grow(b32 want)
{
        awk_fields_reserve((positive)want + 1);

        for (b32 i = awk_nf + 1; i <= want; i++)
                awk_set_bytes(address_of awk_fields[i], "", 0);

        if (want > awk_nf)
        {
                awk_nf = want;
                awk_set_global_number(awk_where_nf, (decimal)awk_nf);
        }
}

static fn awk_field_written(b32 which)
{
        if (!which)
        {
                awk_split_record();
                return;
        }

        if (which > awk_nf)
                awk_field_grow(which);

        awk_record_stale = true;
}

static fn awk_nf_written(b32 want)
{
        if (want < 0)
                want = 0;

        awk_fields_reserve((positive)want + 1);

        for (b32 i = want + 1; i <= awk_nf; i++)
                awk_value_clear(address_of awk_fields[i]);

        for (b32 i = awk_nf + 1; i <= want; i++)
                awk_set_bytes(address_of awk_fields[i], "", 0);

        awk_nf = want;
        awk_set_global_number(awk_where_nf, (decimal)awk_nf);
        awk_record_rebuild();
}

/*
        Where output goes, and where getline reads from.

        Both are tables keyed by the string that named them, because that is
        what close() is given: print > "out" and getline < "out" are two
        entries under one name and close("out") ends both. A name that starts
        with | is a command, which means a pipe and a child.
*/
#define AWK_STREAMS_MAX 32
#define AWK_READ_CHUNK 65536

enum
{
        AWK_TO_FILE = 0,
        AWK_TO_APPEND,
        AWK_TO_PIPE
};

typedef struct
{
        awk_text address_to name;
        b32 handle;
        p8 kind;
        bool live;
        bipolar child;
        positive used;
        p8 buffer[8192];
} awk_writer;

typedef struct
{
        awk_text address_to name;
        b32 handle;
        bool live;
        bool pipe;
        bool ended;
        bipolar child;
        p8 address_to data;
        positive room;
        positive filled;
        positive at;
} awk_reader;

static awk_writer awk_writers[AWK_STREAMS_MAX];
static awk_reader awk_readers[AWK_STREAMS_MAX];
static awk_writer awk_standard_out;

static fn awk_writer_flush(awk_writer address_to which)
{
        if (!which->used)
                return;

        text_write_raw((positive)which->handle, which->buffer, which->used);
        which->used = 0;
}

static fn awk_writer_put(awk_writer address_to which, string_address data, positive length)
{
        if (length >= sizeof(which->buffer))
        {
                awk_writer_flush(which);
                text_write_raw((positive)which->handle, (address_any)data, length);
                return;
        }

        if (which->used + length > sizeof(which->buffer))
                awk_writer_flush(which);

        memory_copy(which->buffer + which->used, data, length);
        which->used += length;
}

static fn awk_flush_everything()
{
        awk_writer_flush(address_of awk_standard_out);

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
                if (awk_writers[i].live)
                        awk_writer_flush(address_of awk_writers[i]);
}

/*
        A command, in a child, with one end of a pipe.

        There is no exec.c under this -- awk.c is included before it -- so the
        three syscalls are made here. /bin/sh -c is what the standard says the
        command is handed to.
*/
static string_address awk_child_environment[1024];

static bipolar awk_spawn(string_address command, b32 into, b32 out_of)
{
        string_address words[4];
        bipolar child;

        words[0] = "/bin/sh";
        words[1] = "-c";
        words[2] = (string_address)command;
        words[3] = null;

        awk_flush_everything();

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child)
                return child;

        if (into >= 0)
        {
                system_call_3(syscall(dup3), (positive)into, 0, 0);
                system_call_1(syscall(close), (positive)into);
        }

        if (out_of >= 0)
        {
                system_call_3(syscall(dup3), (positive)out_of, 1, 0);
                system_call_1(syscall(close), (positive)out_of);
        }

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                if (awk_writers[i].live && awk_writers[i].handle > 2)
                        system_call_1(syscall(close), (positive)awk_writers[i].handle);

                if (awk_readers[i].live && awk_readers[i].handle > 2)
                        system_call_1(syscall(close), (positive)awk_readers[i].handle);
        }

        system_call_3(syscall(execve), (positive) "/bin/sh", (positive)words,
                      (positive)awk_child_environment);
        exit(127);
        return -1;
}

static b32 awk_wait_for(bipolar child)
{
        positive status = 0;

        if (child <= 0)
                return 0;

        system_call_4(syscall(wait4), (positive)child, (positive)address_of status, 0, 0);
        return (b32)((status >> 8) & 0xff);
}

static bool awk_name_is(awk_text address_to name, string_address what)
{
        positive length = string_length(what);

        return name->length == length && !memory_compare(name->text, what, length);
}

static awk_writer address_to awk_writer_for(awk_text address_to name, p8 kind)
{
        b32 free_slot = -1;

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                if (awk_writers[i].live)
                {
                        if (awk_writers[i].name->length == name->length &&
                            !memory_compare(awk_writers[i].name->text, name->text, name->length))
                                return address_of awk_writers[i];
                }
                else if (free_slot < 0)
                        free_slot = i;
        }

        if (free_slot < 0)
                awk_fatal(null, "too many open files");

        awk_writer address_to made = address_of awk_writers[free_slot];

        made->name = awk_text_hold(name);
        made->used = 0;
        made->kind = kind;
        made->child = 0;
        made->live = true;

        if (kind == AWK_TO_PIPE)
        {
                b32 ends[2];

                if (system_call_2(syscall(pipe2), (positive)ends, 0) < 0)
                        awk_fatal(null, "cannot open pipe");

                // The child has to be told about its own end of the pipe
                // before it is made, or it inherits the writing end and the
                // command never sees the input stop.
                made->handle = ends[1];
                made->child = awk_spawn(name->text, ends[0], -1);
                system_call_1(syscall(close), (positive)ends[0]);
                return made;
        }

        if (awk_name_is(name, "/dev/stdout") || awk_name_is(name, "-"))
        {
                made->handle = 1;
                return made;
        }

        if (awk_name_is(name, "/dev/stderr"))
        {
                made->handle = 2;
                return made;
        }

        bipolar handle = text_open_handle(name->text,
                                          kind == AWK_TO_APPEND ? TEXT_APPEND : TEXT_WRITE, 0666);

        if (handle < 0)
                awk_fatal(name->text, "cannot open for writing");

        made->handle = (b32)handle;
        return made;
}

static fn awk_reader_room(awk_reader address_to which, positive want)
{
        if (want <= which->room)
                return;

        positive room = which->room ? which->room : AWK_READ_CHUNK * 2;

        while (room < want)
                room *= 2;

        p8 address_to made = (p8 address_to)awk_take(room);

        if (which->filled > which->at)
                memory_copy(made, which->data + which->at, which->filled - which->at);

        which->filled -= which->at;
        which->at = 0;
        awk_give(which->data);
        which->data = made;
        which->room = room;
}

static bool awk_reader_fill(awk_reader address_to which)
{
        if (which->ended)
                return false;

        if (which->at && which->at == which->filled)
        {
                which->at = 0;
                which->filled = 0;
        }

        awk_reader_room(which, which->filled + AWK_READ_CHUNK + 1);

        bipolar got = system_call_3(syscall(read), (positive)which->handle,
                                    (positive)(which->data + which->filled), AWK_READ_CHUNK);

        if (got <= 0)
        {
                which->ended = true;
                return false;
        }

        which->filled += (positive)got;
        return true;
}

static awk_reader address_to awk_reader_for(awk_text address_to name, bool pipe)
{
        b32 free_slot = -1;

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                if (awk_readers[i].live)
                {
                        if (awk_readers[i].pipe == pipe &&
                            awk_readers[i].name->length == name->length &&
                            !memory_compare(awk_readers[i].name->text, name->text, name->length))
                                return address_of awk_readers[i];
                }
                else if (free_slot < 0)
                        free_slot = i;
        }

        if (free_slot < 0)
                return null;

        awk_reader address_to made = address_of awk_readers[free_slot];

        made->name = awk_text_hold(name);
        made->at = 0;
        made->filled = 0;
        made->ended = false;
        made->pipe = pipe;
        made->child = 0;

        if (pipe)
        {
                b32 ends[2];

                if (system_call_2(syscall(pipe2), (positive)ends, 0) < 0)
                        return null;

                made->handle = ends[0];
                made->live = true;
                made->child = awk_spawn(name->text, -1, ends[1]);
                system_call_1(syscall(close), (positive)ends[1]);
                return made;
        }

        if (awk_name_is(name, "-") || awk_name_is(name, "/dev/stdin"))
        {
                made->handle = 0;
                made->live = true;
                return made;
        }

        bipolar handle = text_open_handle(name->text, FILE_READ, 0);

        if (handle < 0)
        {
                awk_text_drop(made->name);
                made->name = null;
                return null;
        }

        made->handle = (b32)handle;
        made->live = true;
        return made;
}

static b32 awk_close_named(awk_text address_to name)
{
        b32 answer = -1;

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                awk_writer address_to which = address_of awk_writers[i];

                if (!which->live || which->name->length != name->length ||
                    memory_compare(which->name->text, name->text, name->length))
                        continue;

                awk_writer_flush(which);

                if (which->handle > 2)
                        system_call_1(syscall(close), (positive)which->handle);

                answer = which->kind == AWK_TO_PIPE ? awk_wait_for(which->child) : 0;
                awk_text_drop(which->name);
                which->name = null;
                which->live = false;
        }

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                awk_reader address_to which = address_of awk_readers[i];

                if (!which->live || which->name->length != name->length ||
                    memory_compare(which->name->text, name->text, name->length))
                        continue;

                if (which->handle > 2)
                        system_call_1(syscall(close), (positive)which->handle);

                answer = which->pipe ? awk_wait_for(which->child) : 0;
                awk_text_drop(which->name);
                which->name = null;
                which->live = false;
                which->at = 0;
                which->filled = 0;
                which->ended = true;
        }

        return answer;
}

static fn awk_close_everything()
{
        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                awk_writer address_to which = address_of awk_writers[i];

                if (!which->live)
                        continue;

                awk_writer_flush(which);

                if (which->handle > 2)
                        system_call_1(syscall(close), (positive)which->handle);

                if (which->kind == AWK_TO_PIPE)
                        awk_wait_for(which->child);

                which->live = false;
        }

        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
        {
                awk_reader address_to which = address_of awk_readers[i];

                if (!which->live)
                        continue;

                if (which->handle > 2)
                        system_call_1(syscall(close), (positive)which->handle);

                if (which->pipe)
                        awk_wait_for(which->child);

                which->live = false;
        }
}

/*
        One record, however RS says a record ends.

        An empty RS is paragraph mode: blank lines separate, leading ones are
        skipped, and a newline becomes a field separator as well. A single
        character is itself. Anything longer is a pattern, which is not what
        POSIX says and is what the awk on this machine does.
*/
static bool awk_read_record(awk_reader address_to which, awk_text address_to address_to into)
{
        positive length;
        string_address separator = awk_separator(awk_where_rs, address_of length);

        if (!length)
        {
                for (;;)
                {
                        while (which->at < which->filled && which->data[which->at] == '\n')
                                which->at++;

                        if (which->at < which->filled || !awk_reader_fill(which))
                                break;
                }

                if (which->at >= which->filled)
                        return false;

                positive scan = 0;
                bool found = false;

                for (;;)
                {
                        while (which->at + scan + 1 < which->filled)
                        {
                                if (which->data[which->at + scan] == '\n' &&
                                    which->data[which->at + scan + 1] == '\n')
                                {
                                        found = true;
                                        break;
                                }

                                scan++;
                        }

                        if (found || !awk_reader_fill(which))
                                break;
                }

                if (!found)
                        scan = which->filled - which->at;

                positive stop = scan;

                while (stop > 0 && which->data[which->at + stop - 1] == '\n')
                        stop--;

                address_to into = awk_text_new(which->data + which->at, stop);
                which->at += scan;
                return true;
        }

        if (length == 1)
        {
                positive scan = 0;

                for (;;)
                {
                        while (which->at + scan < which->filled &&
                               which->data[which->at + scan] != separator[0])
                                scan++;

                        if (which->at + scan < which->filled || !awk_reader_fill(which))
                                break;
                }

                positive here = which->at + scan;

                if (!scan && here >= which->filled)
                        return false;

                address_to into = awk_text_new(which->data + which->at, scan);
                which->at = here < which->filled ? here + 1 : here;
                return true;
        }

        awk_text address_to pattern = awk_text_new(separator, length);
        positive cut = TEXT_UNSET;
        positive stop = 0;

        for (;;)
        {
                regex_select(awk_regex_dynamic(pattern));

                bool got = regex_search_longest(which->data + which->at,
                                                which->filled - which->at, 0) &&
                           regex_slots[1] > regex_slots[0];

                cut = got ? regex_slots[0] : TEXT_UNSET;
                stop = got ? regex_slots[1] : 0;

                if (which->ended)
                        break;

                // A match that runs to the end of what has been read may
                // still be growing, so read more before believing it.
                if (got && stop < which->filled - which->at)
                        break;

                awk_reader_fill(which);
        }

        awk_text_drop(pattern);

        if (cut == TEXT_UNSET)
        {
                if (which->at >= which->filled)
                        return false;

                address_to into = awk_text_new(which->data + which->at,
                                               which->filled - which->at);
                which->at = which->filled;
                return true;
        }

        address_to into = awk_text_new(which->data + which->at, cut);
        which->at += stop;
        return true;
}

/*
        Somewhere to build a string that does not know its length yet.

        Small ones never leave the stack; the arena is only reached for when
        one outgrows the thousand bytes it starts with.
*/
typedef struct
{
        p8 address_to data;
        positive room;
        positive used;
        bool heap;
        p8 fixed[1024];
} awk_builder;

static fn awk_builder_start(awk_builder address_to build)
{
        build->data = build->fixed;
        build->room = sizeof(build->fixed);
        build->used = 0;
        build->heap = false;
}

static fn awk_builder_room(awk_builder address_to build, positive want)
{
        if (want <= build->room)
                return;

        positive room = build->room * 2;

        while (room < want)
                room *= 2;

        p8 address_to made = (p8 address_to)awk_take(room);

        memory_copy(made, build->data, build->used);

        if (build->heap)
                awk_give(build->data);

        build->data = made;
        build->room = room;
        build->heap = true;
}

static fn awk_builder_put(awk_builder address_to build, string_address data, positive length)
{
        awk_builder_room(build, build->used + length + 1);
        memory_copy(build->data + build->used, data, length);
        build->used += length;
}

static fn awk_builder_char(awk_builder address_to build, p8 character)
{
        awk_builder_room(build, build->used + 2);
        build->data[build->used++] = character;
}

static fn awk_builder_fill(awk_builder address_to build, p8 character, b32 count)
{
        for (b32 i = 0; i < count; i++)
                awk_builder_char(build, character);
}

static awk_text address_to awk_builder_text(awk_builder address_to build)
{
        awk_text address_to made = awk_text_new(build->data, build->used);

        if (build->heap)
                awk_give(build->data);

        return made;
}

/*
        printf.

        The conversions are C's, because that is what awk's are: the same
        flags, the same star for a width taken from the arguments, and the
        same six digits when no precision is given. %c is the one that is not
        C's -- a number is a character code and a string is its first byte,
        and which one a value is follows the same rule comparison follows.
*/
static b32 awk_integer_digits(decimal value, p8 address_to out, bool address_to negative)
{
        address_to negative = value < 0;

        if (!awk_is_finite(value) || awk_is_nan(value))
        {
                string_address name = awk_not_finite_name(value);
                b32 at = (b32)string_length(name);

                address_to negative = false;
                memory_copy(out, name, (positive)at + 1);
                return at;
        }

        decimal magnitude = awk_truncate(value < 0 ? -value : value);

        if (magnitude < 1e18)
        {
                positive whole = (positive)magnitude;
                p8 digits[24];
                b32 have = 0;
                b32 at = 0;

                if (!whole)
                        digits[have++] = '0';

                while (whole)
                {
                        digits[have++] = (p8)('0' + whole % 10);
                        whole /= 10;
                }

                while (have)
                        out[at++] = digits[--have];

                out[at] = end;

                if (at == 1 && out[0] == '0')
                        address_to negative = false;

                return at;
        }

        return (b32)awk_write_fixed(magnitude, 0, out, false);
}

static awk_text address_to awk_sprintf(string_address format, positive length,
                                       awk_value address_to arguments, b32 count)
{
        awk_builder build;
        awk_value nothing;
        b32 taken = 0;
        positive at = 0;

        nothing.text = null;
        nothing.number = 0;
        nothing.state = AWK_UNSET;
        awk_builder_start(address_of build);

        while (at < length)
        {
                if (format[at] != '%')
                {
                        awk_builder_char(address_of build, format[at++]);
                        continue;
                }

                at++;

                if (at < length && format[at] == '%')
                {
                        awk_builder_char(address_of build, '%');
                        at++;
                        continue;
                }

                bool left = false;
                bool sign = false;
                bool space = false;
                bool zero = false;
                bool alternate = false;
                b32 width = 0;
                b32 precision = -1;

                for (; at < length; at++)
                {
                        if (format[at] == '-')
                                left = true;
                        else if (format[at] == '+')
                                sign = true;
                        else if (format[at] == ' ')
                                space = true;
                        else if (format[at] == '0')
                                zero = true;
                        else if (format[at] == '#')
                                alternate = true;
                        else
                                break;
                }

                if (at < length && format[at] == '*')
                {
                        decimal value = taken < count
                                            ? awk_to_number(address_of arguments[taken++])
                                            : 0;

                        width = (b32)value;
                        at++;

                        if (width < 0)
                        {
                                left = true;
                                width = -width;
                        }
                }
                else
                        while (at < length && format[at] >= '0' && format[at] <= '9')
                                width = width * 10 + (format[at++] - '0');

                if (at < length && format[at] == '.')
                {
                        at++;
                        precision = 0;

                        if (at < length && format[at] == '*')
                        {
                                decimal value = taken < count
                                                    ? awk_to_number(address_of arguments[taken++])
                                                    : 0;

                                precision = (b32)value;
                                at++;

                                if (precision < 0)
                                        precision = -1;
                        }
                        else
                                while (at < length && format[at] >= '0' && format[at] <= '9')
                                        precision = precision * 10 + (format[at++] - '0');
                }

                // The length modifiers C needs and awk has no use for.
                while (at < length && (format[at] == 'l' || format[at] == 'h' ||
                                       format[at] == 'L' || format[at] == 'q' ||
                                       format[at] == 'j' || format[at] == 'z' ||
                                       format[at] == 't'))
                        at++;

                if (at >= length)
                {
                        awk_builder_char(address_of build, '%');
                        break;
                }

                p8 conversion = format[at++];
                awk_value address_to argument = taken < count ? address_of arguments[taken++]
                                                              : address_of nothing;
                p8 room[2048];
                p8 prefix[4];
                b32 prefixed = 0;
                b32 body = 0;
                bool from_string = false;
                string_address body_at = room;

                switch (conversion)
                {
                case 'd':
                case 'i':
                {
                        bool negative;

                        body = awk_integer_digits(awk_to_number(argument), room,
                                                  address_of negative);

                        // A precision of zero on a value of zero writes no
                        // digits at all, which is C's rule and awk's.
                        if (!precision && body == 1 && room[0] == '0')
                                body = 0;

                        if (negative)
                                prefix[prefixed++] = '-';
                        else if (sign)
                                prefix[prefixed++] = '+';
                        else if (space)
                                prefix[prefixed++] = ' ';

                        break;
                }

                case 'o':
                case 'u':
                case 'x':
                case 'X':
                {
                        decimal exact = awk_to_number(argument);
                        decimal value = awk_truncate(exact);
                        positive whole;

                        /*
                                A value with no place in sixty four bits is
                                written the way the reference writes it,
                                which is not as a number in that base at all.
                        */
                        if (value >= 18446744073709551616.0 ||
                            value < -9223372036854775808.0 || awk_is_nan(value) ||
                            !awk_is_finite(value))
                        {
                                if (!awk_is_finite(exact) || awk_is_nan(exact))
                                {
                                        string_address name = awk_not_finite_name(exact);

                                        body = (b32)string_length(name);
                                        memory_copy(room, name, (positive)body);
                                        break;
                                }

                                body = (b32)awk_write_general(exact, 6, room, false, alternate);

                                if (room[0] == '-')
                                {
                                        prefix[prefixed++] = '-';
                                        body--;

                                        for (b32 i = 0; i < body; i++)
                                                room[i] = room[i + 1];
                                }

                                break;
                        }

                        whole = value < 0 ? (positive)(bipolar)value : (positive)value;

                        b32 base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);
                        string_address letters = conversion == 'X' ? "0123456789ABCDEF"
                                                                   : "0123456789abcdef";
                        p8 digits[32];
                        b32 have = 0;

                        if (!whole && precision != 0)
                                digits[have++] = '0';

                        while (whole)
                        {
                                digits[have++] = letters[whole % base];
                                whole /= base;
                        }

                        if (alternate && conversion == 'o' && digits[have - 1] != '0')
                                digits[have++] = '0';

                        while (have)
                                room[body++] = digits[--have];

                        if (alternate && (conversion == 'x' || conversion == 'X') && exact != 0)
                        {
                                prefix[prefixed++] = '0';
                                prefix[prefixed++] = conversion;
                        }

                        break;
                }

                case 'c':
                {
                        if (awk_numeric_side(argument))
                                room[body++] = (p8)((positive)(bipolar)awk_to_number(argument) & 0xff);
                        else
                        {
                                awk_text address_to text = awk_to_text(argument);

                                room[body++] = text->length ? text->text[0] : (p8)0;
                        }

                        precision = -1;
                        break;
                }

                case 's':
                {
                        awk_text address_to text = awk_to_text(argument);

                        body_at = text->text;
                        body = (b32)text->length;
                        from_string = true;

                        if (precision >= 0 && precision < body)
                                body = precision;

                        break;
                }

                case 'e':
                case 'E':
                case 'f':
                case 'F':
                case 'g':
                case 'G':
                {
                        decimal value = awk_to_number(argument);
                        b32 places = precision < 0 ? 6 : precision;

                        if (!awk_is_finite(value) || awk_is_nan(value))
                        {
                                string_address name = awk_not_finite_name(value);

                                body = (b32)string_length(name);
                                memory_copy(room, name, (positive)body);
                                zero = false;
                                break;
                        }

                        if (value < 0 || awk_negative(value))
                                prefix[prefixed++] = '-';
                        else if (sign)
                                prefix[prefixed++] = '+';
                        else if (space)
                                prefix[prefixed++] = ' ';

                        if (places > 1000)
                                places = 1000;

                        if (conversion == 'f' || conversion == 'F')
                                body = (b32)awk_write_fixed(value, places, room, alternate);
                        else if (conversion == 'e' || conversion == 'E')
                                body = (b32)awk_write_scientific(value, places, room,
                                                                 conversion == 'E', alternate);
                        else
                                body = (b32)awk_write_general(value, places, room,
                                                              conversion == 'G', alternate);

                        break;
                }

                default:
                        awk_builder_char(address_of build, '%');
                        awk_builder_char(address_of build, conversion);
                        taken -= taken > 0 ? 1 : 0;
                        continue;
                }

                // A precision on an integer is a minimum number of digits,
                // and it takes the zero flag out of the argument.
                b32 zeros = 0;

                if (!from_string && precision >= 0 &&
                    (conversion == 'd' || conversion == 'i' || conversion == 'o' ||
                     conversion == 'u' || conversion == 'x' || conversion == 'X'))
                {
                        if (precision > body)
                                zeros = precision - body;

                        zero = false;
                }

                b32 total = prefixed + zeros + body;
                b32 padding = width > total ? width - total : 0;

                if (padding && !left && !zero)
                        awk_builder_fill(address_of build, ' ', padding);

                for (b32 i = 0; i < prefixed; i++)
                        awk_builder_char(address_of build, prefix[i]);

                if (padding && !left && zero)
                        awk_builder_fill(address_of build, '0', padding);

                awk_builder_fill(address_of build, '0', zeros);
                awk_builder_put(address_of build, body_at, (positive)body);

                if (padding && left)
                        awk_builder_fill(address_of build, ' ', padding);
        }

        return awk_builder_text(address_of build);
}

/*
        The lexer.

        One thing here is not decidable from the character: a slash is either
        a division or the start of a pattern, and which one depends on whether
        the parser is standing where an operand would go. That is what the
        previous token says, so it is kept.
*/
enum
{
        T_END = 0,
        T_NEWLINE,
        T_OPEN_BRACE,
        T_CLOSE_BRACE,
        T_OPEN,
        T_CLOSE,
        T_OPEN_SQUARE,
        T_CLOSE_SQUARE,
        T_SEMICOLON,
        T_COMMA,
        T_NUMBER,
        T_STRING,
        T_ERE,
        T_NAME,
        T_CALL_NAME,
        T_BUILTIN,
        T_GETLINE,
        T_BEGIN,
        T_FINISH,
        T_FUNCTION,
        T_IF,
        T_ELSE,
        T_WHILE,
        T_FOR,
        T_DO,
        T_BREAK,
        T_CONTINUE,
        T_NEXT,
        T_NEXTFILE,
        T_EXIT,
        T_RETURN,
        T_DELETE,
        T_IN,
        T_PRINT,
        T_PRINTF,
        T_ASSIGN,
        T_ASSIGN_ADD,
        T_ASSIGN_SUB,
        T_ASSIGN_MUL,
        T_ASSIGN_DIV,
        T_ASSIGN_MOD,
        T_ASSIGN_POWER,
        T_OR,
        T_AND,
        T_NOT,
        T_LESS,
        T_LESS_EQUAL,
        T_GREATER,
        T_GREATER_EQUAL,
        T_EQUAL,
        T_UNEQUAL,
        T_MATCH,
        T_UNMATCH,
        T_PLUS,
        T_MINUS,
        T_TIMES,
        T_DIVIDE,
        T_MODULO,
        T_POWER,
        T_QUESTION,
        T_COLON,
        T_PLUS_PLUS,
        T_MINUS_MINUS,
        T_DOLLAR,
        T_APPEND,
        T_PIPE
};

enum
{
        B_LENGTH = 0,
        B_SUBSTR,
        B_INDEX,
        B_SPLIT,
        B_SUB,
        B_GSUB,
        B_MATCH,
        B_SPRINTF,
        B_SIN,
        B_COS,
        B_ATAN2,
        B_EXP,
        B_LOG,
        B_SQRT,
        B_INT,
        B_RAND,
        B_SRAND,
        B_TOLOWER,
        B_TOUPPER,
        B_SYSTEM,
        B_CLOSE,
        B_FFLUSH
};

typedef struct
{
        string_address name;
        b32 token;
        b32 value;
} awk_word;

static awk_word awk_words[] = {
    {"BEGIN", T_BEGIN, 0},
    {"END", T_FINISH, 0},
    {"function", T_FUNCTION, 0},
    {"func", T_FUNCTION, 0},
    {"if", T_IF, 0},
    {"else", T_ELSE, 0},
    {"while", T_WHILE, 0},
    {"for", T_FOR, 0},
    {"do", T_DO, 0},
    {"break", T_BREAK, 0},
    {"continue", T_CONTINUE, 0},
    {"next", T_NEXT, 0},
    {"nextfile", T_NEXTFILE, 0},
    {"exit", T_EXIT, 0},
    {"return", T_RETURN, 0},
    {"delete", T_DELETE, 0},
    {"in", T_IN, 0},
    {"print", T_PRINT, 0},
    {"printf", T_PRINTF, 0},
    {"getline", T_GETLINE, 0},
    {"length", T_BUILTIN, B_LENGTH},
    {"substr", T_BUILTIN, B_SUBSTR},
    {"index", T_BUILTIN, B_INDEX},
    {"split", T_BUILTIN, B_SPLIT},
    {"sub", T_BUILTIN, B_SUB},
    {"gsub", T_BUILTIN, B_GSUB},
    {"match", T_BUILTIN, B_MATCH},
    {"sprintf", T_BUILTIN, B_SPRINTF},
    {"sin", T_BUILTIN, B_SIN},
    {"cos", T_BUILTIN, B_COS},
    {"atan2", T_BUILTIN, B_ATAN2},
    {"exp", T_BUILTIN, B_EXP},
    {"log", T_BUILTIN, B_LOG},
    {"sqrt", T_BUILTIN, B_SQRT},
    {"int", T_BUILTIN, B_INT},
    {"rand", T_BUILTIN, B_RAND},
    {"srand", T_BUILTIN, B_SRAND},
    {"tolower", T_BUILTIN, B_TOLOWER},
    {"toupper", T_BUILTIN, B_TOUPPER},
    {"system", T_BUILTIN, B_SYSTEM},
    {"close", T_BUILTIN, B_CLOSE},
    {"fflush", T_BUILTIN, B_FFLUSH},
    {null, 0, 0}};

static string_address awk_source;
static positive awk_source_length;
static positive awk_source_at;
static b32 awk_token;
static b32 awk_token_value;
static b32 awk_last_token = T_NEWLINE;
static decimal awk_token_number;
static awk_text address_to awk_token_text;
static b32 awk_line_number = 1;

static fn awk_fatal(string_address about, string_address reason)
{
        awk_flush_everything();
        text_error(about, reason);
        awk_leave(2);
}

static fn awk_syntax(string_address reason)
{
        p8 where[64];
        positive at = 0;
        b32 line = awk_line_number;
        p8 digits[16];
        b32 have = 0;

        memory_copy(where, "line ", 5);
        at = 5;

        if (!line)
                digits[have++] = '0';

        while (line)
        {
                digits[have++] = (p8)('0' + line % 10);
                line /= 10;
        }

        while (have)
                where[at++] = digits[--have];

        where[at] = end;
        awk_flush_everything();
        text_error(where, reason);
        awk_leave(1);
}

static bool awk_name_start(p8 character)
{
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') || character == '_';
}

static bool awk_name_part(p8 character)
{
        return awk_name_start(character) || text_digit(character);
}

static bool awk_operand_before()
{
        switch (awk_last_token)
        {
        case T_NUMBER:
        case T_STRING:
        case T_NAME:
        case T_ERE:
        case T_CLOSE:
        case T_CLOSE_SQUARE:
        case T_PLUS_PLUS:
        case T_MINUS_MINUS:
        case T_BUILTIN:
                return true;
        }

        return false;
}

static b32 awk_hex_of(p8 character)
{
        if (character >= '0' && character <= '9')
                return character - '0';

        if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;

        if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;

        return -1;
}

static b32 awk_escape(positive address_to at, positive stop)
{
        p8 character = awk_source[address_to at];

        address_to at += 1;

        switch (character)
        {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '\\': return '\\';
        case '"': return '"';
        case '/': return '/';
        case 'a': return 7;
        case 'b': return 8;
        case 'f': return 12;
        case 'v': return 11;
        }

        if (character >= '0' && character <= '7')
        {
                b32 value = character - '0';

                for (b32 i = 0; i < 2 && address_to at < stop; i++)
                {
                        p8 next = awk_source[address_to at];

                        if (next < '0' || next > '7')
                                break;

                        value = value * 8 + (next - '0');
                        address_to at += 1;
                }

                return value & 0xff;
        }

        if (character == 'x' && address_to at < stop && awk_hex_of(awk_source[address_to at]) >= 0)
        {
                b32 value = 0;

                for (b32 i = 0; i < 2 && address_to at < stop; i++)
                {
                        b32 digit = awk_hex_of(awk_source[address_to at]);

                        if (digit < 0)
                                break;

                        value = value * 16 + digit;
                        address_to at += 1;
                }

                return value & 0xff;
        }

        return character;
}

static fn awk_next_token()
{
        awk_last_token = awk_token;

        for (;;)
        {
                while (awk_source_at < awk_source_length &&
                       (awk_source[awk_source_at] == ' ' || awk_source[awk_source_at] == '\t' ||
                        awk_source[awk_source_at] == '\r'))
                        awk_source_at++;

                if (awk_source_at + 1 < awk_source_length && awk_source[awk_source_at] == '\\' &&
                    awk_source[awk_source_at + 1] == '\n')
                {
                        awk_source_at += 2;
                        awk_line_number++;
                        continue;
                }

                if (awk_source_at < awk_source_length && awk_source[awk_source_at] == '#')
                {
                        while (awk_source_at < awk_source_length &&
                               awk_source[awk_source_at] != '\n')
                                awk_source_at++;

                        continue;
                }

                break;
        }

        if (awk_source_at >= awk_source_length)
        {
                awk_token = T_END;
                return;
        }

        p8 character = awk_source[awk_source_at];

        if (character == '\n')
        {
                awk_source_at++;
                awk_line_number++;
                awk_token = T_NEWLINE;
                return;
        }

        if (text_digit(character) ||
            (character == '.' && awk_source_at + 1 < awk_source_length &&
             text_digit(awk_source[awk_source_at + 1])))
        {
                positive used;

                awk_token_number = awk_scan_number(awk_source + awk_source_at,
                                                   awk_source_length - awk_source_at,
                                                   address_of used);
                awk_source_at += used;
                awk_token = T_NUMBER;
                return;
        }

        if (awk_name_start(character))
        {
                positive start = awk_source_at;

                while (awk_source_at < awk_source_length &&
                       awk_name_part(awk_source[awk_source_at]))
                        awk_source_at++;

                positive length = awk_source_at - start;

                for (b32 i = 0; awk_words[i].name; i++)
                        if (string_length(awk_words[i].name) == length &&
                            !memory_compare(awk_words[i].name, awk_source + start, length))
                        {
                                awk_token = awk_words[i].token;
                                awk_token_value = awk_words[i].value;
                                return;
                        }

                awk_text_drop(awk_token_text);
                awk_token_text = awk_text_new(awk_source + start, length);
                awk_token = awk_source_at < awk_source_length &&
                                    awk_source[awk_source_at] == '('
                                ? T_CALL_NAME
                                : T_NAME;
                return;
        }

        if (character == '"')
        {
                awk_builder build;

                awk_builder_start(address_of build);
                awk_source_at++;

                while (awk_source_at < awk_source_length && awk_source[awk_source_at] != '"')
                {
                        if (awk_source[awk_source_at] == '\n')
                                awk_syntax("newline in string");

                        if (awk_source[awk_source_at] == '\\' &&
                            awk_source_at + 1 < awk_source_length)
                        {
                                awk_source_at++;
                                awk_builder_char(address_of build,
                                                 (p8)awk_escape(address_of awk_source_at,
                                                                awk_source_length));
                                continue;
                        }

                        awk_builder_char(address_of build, awk_source[awk_source_at++]);
                }

                if (awk_source_at >= awk_source_length)
                        awk_syntax("unterminated string");

                awk_source_at++;
                awk_text_drop(awk_token_text);
                awk_token_text = awk_builder_text(address_of build);
                awk_token = T_STRING;
                return;
        }

        if (character == '/' && !awk_operand_before())
        {
                awk_builder build;
                bool inside = false;

                awk_builder_start(address_of build);
                awk_source_at++;

                while (awk_source_at < awk_source_length)
                {
                        p8 here = awk_source[awk_source_at];

                        if (here == '\n')
                                awk_syntax("newline in regular expression");

                        if (here == '\\' && awk_source_at + 1 < awk_source_length)
                        {
                                // The engine below wants the backslash kept,
                                // except before the slash that would have
                                // ended the pattern.
                                if (awk_source[awk_source_at + 1] == '/')
                                {
                                        awk_builder_char(address_of build, '/');
                                        awk_source_at += 2;
                                        continue;
                                }

                                awk_builder_char(address_of build, here);
                                awk_builder_char(address_of build,
                                                 awk_source[awk_source_at + 1]);
                                awk_source_at += 2;
                                continue;
                        }

                        if (here == '[')
                                inside = true;
                        else if (here == ']')
                                inside = false;
                        else if (here == '/' && !inside)
                                break;

                        awk_builder_char(address_of build, here);
                        awk_source_at++;
                }

                if (awk_source_at >= awk_source_length)
                        awk_syntax("unterminated regular expression");

                awk_source_at++;
                awk_text_drop(awk_token_text);
                awk_token_text = awk_builder_text(address_of build);
                awk_token = T_ERE;
                return;
        }

        awk_source_at++;

        p8 next = awk_source_at < awk_source_length ? awk_source[awk_source_at] : 0;

        switch (character)
        {
        case '{': awk_token = T_OPEN_BRACE; return;
        case '}': awk_token = T_CLOSE_BRACE; return;
        case '(': awk_token = T_OPEN; return;
        case ')': awk_token = T_CLOSE; return;
        case '[': awk_token = T_OPEN_SQUARE; return;
        case ']': awk_token = T_CLOSE_SQUARE; return;
        case ';': awk_token = T_SEMICOLON; return;
        case ',': awk_token = T_COMMA; return;
        case '?': awk_token = T_QUESTION; return;
        case ':': awk_token = T_COLON; return;
        case '$': awk_token = T_DOLLAR; return;
        case '~': awk_token = T_MATCH; return;

        case '+':
                if (next == '+') { awk_source_at++; awk_token = T_PLUS_PLUS; return; }
                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_ADD; return; }
                awk_token = T_PLUS;
                return;

        case '-':
                if (next == '-') { awk_source_at++; awk_token = T_MINUS_MINUS; return; }
                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_SUB; return; }
                awk_token = T_MINUS;
                return;

        case '*':
                if (next == '*')
                {
                        awk_source_at++;

                        if (awk_source_at < awk_source_length && awk_source[awk_source_at] == '=')
                        {
                                awk_source_at++;
                                awk_token = T_ASSIGN_POWER;
                                return;
                        }

                        awk_token = T_POWER;
                        return;
                }

                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_MUL; return; }
                awk_token = T_TIMES;
                return;

        case '/':
                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_DIV; return; }
                awk_token = T_DIVIDE;
                return;

        case '%':
                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_MOD; return; }
                awk_token = T_MODULO;
                return;

        case '^':
                if (next == '=') { awk_source_at++; awk_token = T_ASSIGN_POWER; return; }
                awk_token = T_POWER;
                return;

        case '=':
                if (next == '=') { awk_source_at++; awk_token = T_EQUAL; return; }
                awk_token = T_ASSIGN;
                return;

        case '!':
                if (next == '=') { awk_source_at++; awk_token = T_UNEQUAL; return; }
                if (next == '~') { awk_source_at++; awk_token = T_UNMATCH; return; }
                awk_token = T_NOT;
                return;

        case '<':
                if (next == '=') { awk_source_at++; awk_token = T_LESS_EQUAL; return; }
                awk_token = T_LESS;
                return;

        case '>':
                if (next == '=') { awk_source_at++; awk_token = T_GREATER_EQUAL; return; }
                if (next == '>') { awk_source_at++; awk_token = T_APPEND; return; }
                awk_token = T_GREATER;
                return;

        case '&':
                if (next == '&') { awk_source_at++; awk_token = T_AND; return; }
                break;

        case '|':
                if (next == '|') { awk_source_at++; awk_token = T_OR; return; }
                awk_token = T_PIPE;
                return;
        }

        awk_syntax("unexpected character");
}

/*
        The parser.

        Recursive descent, one function per level of precedence, in the order
        the grammar in the standard gives them. Two of those levels are the
        ones every awk gets wrong at least once: concatenation, which binds
        tighter than a comparison and looser than a minus -- so 1 " " -1 is
        one string and a subtraction -- and the greater-than after print,
        which is a redirection and not a comparison until a parenthesis says
        otherwise.
*/
enum
{
        N_NUMBER = 0,
        N_STRING,
        N_REGEX,
        N_VARIABLE,
        N_FIELD,
        N_SUBSCRIPT,
        N_GROUP,
        N_ASSIGN,
        N_COND,
        N_OR,
        N_AND,
        N_NOT,
        N_IN,
        N_MATCH,
        N_COMPARE,
        N_CONCAT,
        N_ARITH,
        N_NEGATE,
        N_AFFIRM,
        N_STEP,
        N_CALL,
        N_BUILTIN,
        N_GETLINE,
        S_PRINT,
        S_PRINTF,
        S_EXPRESSION,
        S_IF,
        S_WHILE,
        S_DO,
        S_FOR,
        S_FORIN,
        S_BLOCK,
        S_NEXT,
        S_NEXTFILE,
        S_EXIT,
        S_RETURN,
        S_BREAK,
        S_CONTINUE,
        S_DELETE
};

enum
{
        G_MAIN = 0,
        G_FILE,
        G_COMMAND
};

enum
{
        R_NONE = 0,
        R_FILE,
        R_APPEND,
        R_PIPE
};

typedef struct awk_node
{
        p8 kind;
        p8 sub;
        b32 index;
        b32 count;
        struct awk_node address_to a;
        struct awk_node address_to b;
        struct awk_node address_to c;
        struct awk_node address_to d;
        struct awk_node address_to next;
        awk_text address_to text;
        decimal number;
        regex_program address_to program;
} awk_node;

typedef struct
{
        awk_text address_to name;
        b32 parameters;
        awk_node address_to body;
        bool defined;
} awk_function;

#define AWK_FUNCTIONS_MAX 256

static awk_function awk_functions[AWK_FUNCTIONS_MAX];
static b32 awk_function_count;

static awk_text address_to awk_local_names[AWK_LOCALS_MAX];
static b32 awk_local_count;
static bool awk_inside_function;
static b32 awk_print_depth;

typedef struct
{
        p8 kind;
        awk_node address_to first;
        awk_node address_to second;
        awk_node address_to action;
        bool running;
} awk_rule;

enum
{
        RULE_BEGIN = 0,
        RULE_END,
        RULE_PLAIN
};

#define AWK_RULES_MAX 512

static awk_rule awk_rules[AWK_RULES_MAX];
static b32 awk_rule_count;

static awk_node address_to awk_node_new(p8 kind)
{
        awk_node address_to made = (awk_node address_to)awk_take(sizeof(awk_node));

        memory_fill(made, 0, sizeof(awk_node));
        made->kind = kind;
        return made;
}

static b32 awk_resolve(awk_text address_to name)
{
        if (awk_inside_function)
                for (b32 i = 0; i < awk_local_count; i++)
                        if (awk_local_names[i]->length == name->length &&
                            !memory_compare(awk_local_names[i]->text, name->text, name->length))
                                return -(i + 1);

        return awk_global_find(name->text, name->length);
}

static b32 awk_function_named(awk_text address_to name)
{
        for (b32 i = 0; i < awk_function_count; i++)
                if (awk_functions[i].name->length == name->length &&
                    !memory_compare(awk_functions[i].name->text, name->text, name->length))
                        return i;

        if (awk_function_count == AWK_FUNCTIONS_MAX)
                awk_syntax("too many functions");

        b32 which = awk_function_count++;

        awk_functions[which].name = awk_text_hold(name);
        awk_functions[which].parameters = 0;
        awk_functions[which].body = null;
        awk_functions[which].defined = false;
        return which;
}

typedef struct
{
        positive at;
        b32 token;
        b32 value;
        b32 last;
        b32 line;
        decimal number;
        awk_text address_to text;
} awk_place;

static fn awk_mark(awk_place address_to place)
{
        place->at = awk_source_at;
        place->token = awk_token;
        place->value = awk_token_value;
        place->last = awk_last_token;
        place->line = awk_line_number;
        place->number = awk_token_number;
        place->text = awk_text_hold(awk_token_text);
}

static fn awk_reset(awk_place address_to place)
{
        awk_source_at = place->at;
        awk_token = place->token;
        awk_token_value = place->value;
        awk_last_token = place->last;
        awk_line_number = place->line;
        awk_token_number = place->number;
        awk_text_drop(awk_token_text);
        awk_token_text = place->text;
}

static fn awk_forget(awk_place address_to place)
{
        awk_text_drop(place->text);
}

static fn awk_expect(b32 what, string_address complaint)
{
        if (awk_token != what)
                awk_syntax(complaint);

        awk_next_token();
}

static fn awk_skip_newlines()
{
        while (awk_token == T_NEWLINE)
                awk_next_token();
}

static fn awk_skip_terminators()
{
        while (awk_token == T_NEWLINE || awk_token == T_SEMICOLON)
                awk_next_token();
}

static awk_node address_to awk_expression();
static awk_node address_to awk_concat_level();
static awk_node address_to awk_statement();
static awk_node address_to awk_statement_list(b32 stop);

static bool awk_is_lvalue(awk_node address_to node)
{
        return node && (node->kind == N_VARIABLE || node->kind == N_FIELD ||
                        node->kind == N_SUBSCRIPT);
}

static awk_node address_to awk_subscript_list(b32 array)
{
        awk_node address_to node = awk_node_new(N_SUBSCRIPT);
        awk_node address_to last = null;

        node->index = array;

        for (;;)
        {
                awk_node address_to one = awk_expression();

                if (last)
                        last->next = one;
                else
                        node->a = one;

                last = one;
                node->count++;

                if (awk_token != T_COMMA)
                        break;

                awk_next_token();
                awk_skip_newlines();
        }

        return node;
}

static awk_node address_to awk_primary()
{
        awk_node address_to node;

        switch (awk_token)
        {
        case T_NUMBER:
                node = awk_node_new(N_NUMBER);
                node->number = awk_token_number;
                awk_next_token();
                return node;

        case T_STRING:
                node = awk_node_new(N_STRING);
                node->text = awk_text_hold(awk_token_text);
                awk_next_token();
                return node;

        case T_ERE:
                node = awk_node_new(N_REGEX);
                node->text = awk_text_hold(awk_token_text);
                node->program = awk_regex_keep(awk_token_text->text);
                awk_next_token();
                return node;

        case T_DOLLAR:
                awk_next_token();
                node = awk_node_new(N_FIELD);
                node->a = awk_primary();
                return node;

        case T_PLUS_PLUS:
        case T_MINUS_MINUS:
        {
                b32 which = awk_token;

                awk_next_token();
                node = awk_node_new(N_STEP);
                node->sub = (p8)(which == T_PLUS_PLUS ? 1 : 0);
                node->count = 1;
                node->a = awk_primary();

                if (!awk_is_lvalue(node->a))
                        awk_syntax("++ wants a variable");

                return node;
        }

        case T_NOT:
                awk_next_token();
                node = awk_node_new(N_NOT);
                node->a = awk_primary();
                return node;

        case T_MINUS:
                awk_next_token();
                node = awk_node_new(N_NEGATE);
                node->a = awk_primary();
                return node;

        case T_PLUS:
                awk_next_token();
                node = awk_node_new(N_AFFIRM);
                node->a = awk_primary();
                return node;

        case T_OPEN:
        {
                b32 kept = awk_print_depth;
                awk_node address_to first;
                awk_node address_to last;
                b32 count = 1;

                awk_print_depth = 0;
                awk_next_token();
                awk_skip_newlines();
                first = awk_expression();
                last = first;

                while (awk_token == T_COMMA)
                {
                        awk_next_token();
                        awk_skip_newlines();
                        last->next = awk_expression();
                        last = last->next;
                        count++;
                }

                awk_print_depth = kept;
                awk_expect(T_CLOSE, "expected )");

                if (count == 1)
                        return first;

                node = awk_node_new(N_GROUP);
                node->a = first;
                node->count = count;
                return node;
        }

        case T_NAME:
        {
                awk_text address_to name = awk_text_hold(awk_token_text);
                b32 where = awk_resolve(name);

                awk_next_token();

                if (awk_token == T_OPEN_SQUARE)
                {
                        awk_next_token();
                        node = awk_subscript_list(where);
                        awk_expect(T_CLOSE_SQUARE, "expected ]");
                        awk_text_drop(name);
                        return node;
                }

                node = awk_node_new(N_VARIABLE);
                node->index = where;
                node->text = name;
                return node;
        }

        case T_CALL_NAME:
        {
                awk_text address_to name = awk_text_hold(awk_token_text);
                b32 which = awk_function_named(name);
                b32 kept = awk_print_depth;
                awk_node address_to last = null;

                awk_text_drop(name);
                awk_next_token();
                awk_print_depth = 0;
                awk_expect(T_OPEN, "expected (");
                node = awk_node_new(N_CALL);
                node->index = which;
                awk_skip_newlines();

                while (awk_token != T_CLOSE)
                {
                        awk_node address_to one = awk_expression();

                        if (last)
                                last->next = one;
                        else
                                node->a = one;

                        last = one;
                        node->count++;

                        if (awk_token != T_COMMA)
                                break;

                        awk_next_token();
                        awk_skip_newlines();
                }

                awk_print_depth = kept;
                awk_expect(T_CLOSE, "expected ) after arguments");
                return node;
        }

        case T_BUILTIN:
        {
                b32 which = awk_token_value;
                b32 kept = awk_print_depth;
                awk_node address_to last = null;

                awk_next_token();
                node = awk_node_new(N_BUILTIN);
                node->index = which;

                if (awk_token != T_OPEN)
                {
                        if (which != B_LENGTH)
                                awk_syntax("expected ( after a function name");

                        return node;
                }

                awk_print_depth = 0;
                awk_next_token();
                awk_skip_newlines();

                while (awk_token != T_CLOSE)
                {
                        awk_node address_to one = awk_expression();

                        if (last)
                                last->next = one;
                        else
                                node->a = one;

                        last = one;
                        node->count++;

                        if (awk_token != T_COMMA)
                                break;

                        awk_next_token();
                        awk_skip_newlines();
                }

                awk_print_depth = kept;
                awk_expect(T_CLOSE, "expected ) after arguments");
                return node;
        }

        case T_GETLINE:
        {
                awk_next_token();
                node = awk_node_new(N_GETLINE);
                node->sub = G_MAIN;

                if (awk_token == T_NAME || awk_token == T_DOLLAR)
                {
                        node->a = awk_primary();

                        if (!awk_is_lvalue(node->a))
                                awk_syntax("getline wants a variable");
                }

                if (awk_token == T_LESS)
                {
                        awk_next_token();
                        node->sub = G_FILE;
                        node->b = awk_concat_level();
                }

                return node;
        }
        }

        awk_syntax("unexpected token");
        return null;
}

static awk_node address_to awk_postfix()
{
        awk_node address_to node = awk_primary();

        while ((awk_token == T_PLUS_PLUS || awk_token == T_MINUS_MINUS) &&
               awk_is_lvalue(node))
        {
                awk_node address_to made = awk_node_new(N_STEP);

                made->sub = (p8)(awk_token == T_PLUS_PLUS ? 1 : 0);
                made->count = 0;
                made->a = node;
                node = made;
                awk_next_token();
        }

        return node;
}

static awk_node address_to awk_unary();

static awk_node address_to awk_power_level()
{
        awk_node address_to node = awk_postfix();

        if (awk_token == T_POWER)
        {
                awk_node address_to made = awk_node_new(N_ARITH);

                awk_next_token();
                made->sub = '^';
                made->a = node;
                made->b = awk_unary();
                return made;
        }

        return node;
}

static awk_node address_to awk_unary()
{
        if (awk_token == T_MINUS || awk_token == T_PLUS || awk_token == T_NOT)
        {
                b32 which = awk_token;
                awk_node address_to made =
                    awk_node_new(which == T_MINUS ? N_NEGATE
                                                  : (which == T_PLUS ? N_AFFIRM : N_NOT));

                awk_next_token();
                made->a = awk_unary();
                return made;
        }

        return awk_power_level();
}

static awk_node address_to awk_multiply_level()
{
        awk_node address_to node = awk_unary();

        while (awk_token == T_TIMES || awk_token == T_DIVIDE || awk_token == T_MODULO)
        {
                awk_node address_to made = awk_node_new(N_ARITH);

                made->sub = (p8)(awk_token == T_TIMES ? '*'
                                                      : (awk_token == T_DIVIDE ? '/' : '%'));
                awk_next_token();
                made->a = node;
                made->b = awk_unary();
                node = made;
        }

        return node;
}

static awk_node address_to awk_add_level()
{
        awk_node address_to node = awk_multiply_level();

        while (awk_token == T_PLUS || awk_token == T_MINUS)
        {
                awk_node address_to made = awk_node_new(N_ARITH);

                made->sub = (p8)(awk_token == T_PLUS ? '+' : '-');
                awk_next_token();
                made->a = node;
                made->b = awk_multiply_level();
                node = made;
        }

        return node;
}

static bool awk_starts_operand()
{
        switch (awk_token)
        {
        case T_NUMBER:
        case T_STRING:
        case T_ERE:
        case T_NAME:
        case T_CALL_NAME:
        case T_BUILTIN:
        case T_DOLLAR:
        case T_OPEN:
        case T_NOT:
        case T_PLUS_PLUS:
        case T_MINUS_MINUS:
                return true;
        }

        return false;
}

static awk_node address_to awk_concat_level()
{
        awk_node address_to node = awk_add_level();

        while (awk_starts_operand())
        {
                awk_node address_to made = awk_node_new(N_CONCAT);

                made->a = node;
                made->b = awk_add_level();
                node = made;
        }

        return node;
}

static awk_node address_to awk_in_level()
{
        awk_node address_to node = awk_concat_level();

        while (awk_token == T_IN)
        {
                awk_node address_to made = awk_node_new(N_IN);

                awk_next_token();

                if (awk_token != T_NAME)
                        awk_syntax("in wants an array");

                made->index = awk_resolve(awk_token_text);
                made->a = node;
                awk_next_token();
                node = made;
        }

        return node;
}

static awk_node address_to awk_compare_level()
{
        awk_node address_to node = awk_in_level();

        for (;;)
        {
                b32 which = awk_token;

                if (which == T_GREATER && awk_print_depth)
                        return node;

                switch (which)
                {
                case T_LESS:
                case T_LESS_EQUAL:
                case T_GREATER:
                case T_GREATER_EQUAL:
                case T_EQUAL:
                case T_UNEQUAL:
                {
                        awk_node address_to made = awk_node_new(N_COMPARE);

                        made->sub = (p8)which;
                        awk_next_token();
                        made->a = node;
                        made->b = awk_in_level();
                        node = made;
                        continue;
                }

                case T_MATCH:
                case T_UNMATCH:
                {
                        awk_node address_to made = awk_node_new(N_MATCH);

                        made->sub = (p8)(which == T_UNMATCH);
                        awk_next_token();
                        made->a = node;
                        made->b = awk_in_level();
                        node = made;
                        continue;
                }
                }

                return node;
        }
}

// "command" | getline sits between && and a comparison, which is where the
// grammar puts it and not where anybody would guess.
static awk_node address_to awk_pipe_level()
{
        awk_node address_to node = awk_compare_level();

        while (awk_token == T_PIPE)
        {
                awk_place place;

                awk_mark(address_of place);
                awk_next_token();

                if (awk_token != T_GETLINE)
                {
                        awk_reset(address_of place);
                        break;
                }

                awk_forget(address_of place);
                awk_next_token();

                awk_node address_to made = awk_node_new(N_GETLINE);

                made->sub = G_COMMAND;
                made->b = node;

                if (awk_token == T_NAME || awk_token == T_DOLLAR)
                {
                        made->a = awk_primary();

                        if (!awk_is_lvalue(made->a))
                                awk_syntax("getline wants a variable");
                }

                node = made;
        }

        return node;
}

static awk_node address_to awk_and_level()
{
        awk_node address_to node = awk_pipe_level();

        while (awk_token == T_AND)
        {
                awk_node address_to made = awk_node_new(N_AND);

                awk_next_token();
                awk_skip_newlines();
                made->a = node;
                made->b = awk_pipe_level();
                node = made;
        }

        return node;
}

static awk_node address_to awk_or_level()
{
        awk_node address_to node = awk_and_level();

        while (awk_token == T_OR)
        {
                awk_node address_to made = awk_node_new(N_OR);

                awk_next_token();
                awk_skip_newlines();
                made->a = node;
                made->b = awk_and_level();
                node = made;
        }

        return node;
}

static awk_node address_to awk_expression()
{
        awk_node address_to node = awk_or_level();

        if (awk_token == T_QUESTION)
        {
                awk_node address_to made = awk_node_new(N_COND);

                awk_next_token();
                awk_skip_newlines();
                made->a = node;
                made->b = awk_expression();
                awk_skip_newlines();
                awk_expect(T_COLON, "expected : in ?:");
                awk_skip_newlines();
                made->c = awk_expression();
                return made;
        }

        switch (awk_token)
        {
        case T_ASSIGN:
        case T_ASSIGN_ADD:
        case T_ASSIGN_SUB:
        case T_ASSIGN_MUL:
        case T_ASSIGN_DIV:
        case T_ASSIGN_MOD:
        case T_ASSIGN_POWER:
        {
                if (!awk_is_lvalue(node))
                        awk_syntax("assignment wants a variable on the left");

                awk_node address_to made = awk_node_new(N_ASSIGN);

                made->sub = (p8)awk_token;
                awk_next_token();
                awk_skip_newlines();
                made->a = node;
                made->b = awk_expression();
                return made;
        }
        }

        return node;
}

static bool awk_statement_ends()
{
        return awk_token == T_SEMICOLON || awk_token == T_NEWLINE ||
               awk_token == T_CLOSE_BRACE || awk_token == T_END;
}

static fn awk_finish_statement()
{
        if (awk_token == T_SEMICOLON || awk_token == T_NEWLINE)
        {
                awk_next_token();
                awk_skip_terminators();
                return;
        }

        if (awk_token == T_CLOSE_BRACE || awk_token == T_END)
                return;

        awk_syntax("expected ; or a newline");
}

static awk_node address_to awk_print_statement(bool formatted)
{
        awk_node address_to node = awk_node_new(formatted ? S_PRINTF : S_PRINT);
        awk_node address_to last = null;

        awk_next_token();
        awk_print_depth++;

        if (!awk_statement_ends() && awk_token != T_GREATER && awk_token != T_APPEND &&
            awk_token != T_PIPE)
                for (;;)
                {
                        awk_node address_to one = awk_expression();

                        if (last)
                                last->next = one;
                        else
                                node->a = one;

                        last = one;
                        node->count++;

                        if (awk_token != T_COMMA)
                                break;

                        awk_next_token();
                        awk_skip_newlines();
                }

        // print (a, b) is the list in parentheses, not one expression.
        if (node->count == 1 && node->a->kind == N_GROUP)
        {
                node->count = node->a->count;
                node->a = node->a->a;
        }

        if (awk_token == T_GREATER || awk_token == T_APPEND || awk_token == T_PIPE)
        {
                node->sub = (p8)(awk_token == T_GREATER ? R_FILE
                                                        : (awk_token == T_APPEND ? R_APPEND
                                                                                 : R_PIPE));
                awk_next_token();
                node->b = awk_concat_level();
        }

        awk_print_depth--;
        return node;
}

static awk_node address_to awk_simple_statement()
{
        awk_node address_to node;

        switch (awk_token)
        {
        case T_PRINT: return awk_print_statement(false);
        case T_PRINTF: return awk_print_statement(true);

        case T_DELETE:
        {
                awk_next_token();

                if (awk_token != T_NAME)
                        awk_syntax("delete wants an array");

                node = awk_node_new(S_DELETE);
                node->index = awk_resolve(awk_token_text);
                awk_next_token();

                if (awk_token == T_OPEN_SQUARE)
                {
                        awk_next_token();
                        node->a = awk_subscript_list(node->index);
                        awk_expect(T_CLOSE_SQUARE, "expected ]");
                }

                return node;
        }

        case T_NEXT:
                awk_next_token();
                return awk_node_new(S_NEXT);

        case T_NEXTFILE:
                awk_next_token();
                return awk_node_new(S_NEXTFILE);

        case T_BREAK:
                awk_next_token();
                return awk_node_new(S_BREAK);

        case T_CONTINUE:
                awk_next_token();
                return awk_node_new(S_CONTINUE);

        case T_EXIT:
                awk_next_token();
                node = awk_node_new(S_EXIT);

                if (!awk_statement_ends())
                        node->a = awk_expression();

                return node;

        case T_RETURN:
                awk_next_token();
                node = awk_node_new(S_RETURN);

                if (!awk_statement_ends())
                        node->a = awk_expression();

                return node;
        }

        node = awk_node_new(S_EXPRESSION);
        node->a = awk_expression();
        return node;
}

static awk_node address_to awk_statement()
{
        awk_node address_to node;

        switch (awk_token)
        {
        case T_SEMICOLON:
                awk_next_token();
                awk_skip_terminators();
                return awk_node_new(S_BLOCK);

        case T_OPEN_BRACE:
                awk_next_token();
                node = awk_node_new(S_BLOCK);
                node->a = awk_statement_list(T_CLOSE_BRACE);
                awk_expect(T_CLOSE_BRACE, "expected }");

                if (awk_token == T_SEMICOLON)
                        awk_next_token();

                awk_skip_terminators();
                return node;

        case T_IF:
                awk_next_token();
                awk_expect(T_OPEN, "expected ( after if");
                node = awk_node_new(S_IF);
                node->a = awk_expression();
                awk_expect(T_CLOSE, "expected ) after the condition");
                awk_skip_newlines();
                node->b = awk_statement();
                awk_skip_terminators();

                if (awk_token == T_ELSE)
                {
                        awk_next_token();
                        awk_skip_newlines();
                        node->c = awk_statement();
                }

                return node;

        case T_WHILE:
                awk_next_token();
                awk_expect(T_OPEN, "expected ( after while");
                node = awk_node_new(S_WHILE);
                node->a = awk_expression();
                awk_expect(T_CLOSE, "expected ) after the condition");
                awk_skip_newlines();

                if (awk_token == T_SEMICOLON)
                {
                        awk_next_token();
                        awk_skip_terminators();
                        return node;
                }

                node->b = awk_statement();
                return node;

        case T_DO:
                awk_next_token();
                awk_skip_newlines();
                node = awk_node_new(S_DO);
                node->b = awk_statement();
                awk_skip_terminators();
                awk_expect(T_WHILE, "expected while after do");
                awk_expect(T_OPEN, "expected ( after while");
                node->a = awk_expression();
                awk_expect(T_CLOSE, "expected ) after the condition");
                return node;

        case T_FOR:
        {
                awk_place place;

                awk_next_token();
                awk_expect(T_OPEN, "expected ( after for");

                if (awk_token == T_NAME)
                {
                        awk_text address_to name = awk_text_hold(awk_token_text);

                        awk_mark(address_of place);
                        awk_next_token();

                        if (awk_token == T_IN)
                        {
                                awk_forget(address_of place);
                                awk_next_token();

                                if (awk_token != T_NAME)
                                        awk_syntax("for wants an array after in");

                                node = awk_node_new(S_FORIN);
                                node->index = awk_resolve(name);
                                node->count = awk_resolve(awk_token_text);
                                awk_text_drop(name);
                                awk_next_token();
                                awk_expect(T_CLOSE, "expected ) after in");
                                awk_skip_newlines();
                                node->b = awk_statement();
                                return node;
                        }

                        awk_reset(address_of place);
                        awk_text_drop(name);
                }

                node = awk_node_new(S_FOR);

                if (awk_token != T_SEMICOLON)
                        node->a = awk_simple_statement();

                awk_expect(T_SEMICOLON, "expected ; in for");
                awk_skip_newlines();

                if (awk_token != T_SEMICOLON)
                        node->b = awk_expression();

                awk_expect(T_SEMICOLON, "expected ; in for");
                awk_skip_newlines();

                if (awk_token != T_CLOSE)
                        node->c = awk_simple_statement();

                awk_expect(T_CLOSE, "expected ) after for");
                awk_skip_newlines();

                if (awk_token == T_SEMICOLON)
                {
                        awk_next_token();
                        awk_skip_terminators();
                        return node;
                }

                node->d = awk_statement();
                return node;
        }
        }

        node = awk_simple_statement();
        awk_finish_statement();
        return node;
}

static awk_node address_to awk_statement_list(b32 stop)
{
        awk_node address_to first = null;
        awk_node address_to last = null;

        awk_skip_terminators();

        while (awk_token != stop && awk_token != T_END)
        {
                awk_node address_to one = awk_statement();

                if (last)
                        last->next = one;
                else
                        first = one;

                last = one;
                awk_skip_terminators();
        }

        return first;
}

static fn awk_parse_function()
{
        awk_next_token();

        if (awk_token != T_NAME && awk_token != T_CALL_NAME)
                awk_syntax("function wants a name");

        b32 which = awk_function_named(awk_token_text);

        if (awk_functions[which].defined)
                awk_syntax("function defined twice");

        awk_next_token();
        awk_expect(T_OPEN, "expected ( after a function name");
        awk_local_count = 0;
        awk_skip_newlines();

        while (awk_token != T_CLOSE)
        {
                if (awk_token != T_NAME)
                        awk_syntax("expected a parameter name");

                if (awk_local_count == AWK_LOCALS_MAX)
                        awk_syntax("too many parameters");

                awk_local_names[awk_local_count++] = awk_text_hold(awk_token_text);
                awk_next_token();

                if (awk_token != T_COMMA)
                        break;

                awk_next_token();
                awk_skip_newlines();
        }

        awk_expect(T_CLOSE, "expected ) after the parameters");
        awk_skip_newlines();

        awk_functions[which].parameters = awk_local_count;
        awk_functions[which].defined = true;
        awk_inside_function = true;

        awk_expect(T_OPEN_BRACE, "expected { after a function header");
        awk_functions[which].body = awk_statement_list(T_CLOSE_BRACE);
        awk_expect(T_CLOSE_BRACE, "expected } after a function body");

        awk_inside_function = false;
        awk_local_count = 0;
}

static fn awk_parse_program()
{
        awk_next_token();

        for (;;)
        {
                awk_skip_terminators();

                if (awk_token == T_END)
                        break;

                if (awk_token == T_FUNCTION)
                {
                        awk_parse_function();
                        continue;
                }

                if (awk_rule_count == AWK_RULES_MAX)
                        awk_syntax("too many rules");

                awk_rule address_to rule = address_of awk_rules[awk_rule_count++];

                rule->kind = RULE_PLAIN;
                rule->first = null;
                rule->second = null;
                rule->action = null;
                rule->running = false;

                if (awk_token == T_BEGIN || awk_token == T_FINISH)
                {
                        rule->kind = (p8)(awk_token == T_BEGIN ? RULE_BEGIN : RULE_END);
                        awk_next_token();
                        awk_skip_newlines();

                        if (awk_token != T_OPEN_BRACE)
                                awk_syntax("BEGIN and END want an action");
                }
                else if (awk_token != T_OPEN_BRACE)
                {
                        rule->first = awk_expression();

                        if (awk_token == T_COMMA)
                        {
                                awk_next_token();
                                awk_skip_newlines();
                                rule->second = awk_expression();
                        }
                }

                if (awk_token == T_OPEN_BRACE)
                {
                        awk_next_token();
                        rule->action = awk_statement_list(T_CLOSE_BRACE);
                        awk_expect(T_CLOSE_BRACE, "expected } after an action");
                }
                else if (!awk_statement_ends())
                        awk_syntax("expected an action or the end of the rule");
        }
}

/*
        Running it.

        A tree walk. Statements answer with what should happen next -- keep
        going, leave the loop, leave the record, leave the program -- and that
        answer travels back up through everything that called them, which is
        how next inside a function inside an if gets out to the record loop.
*/
enum
{
        RUN_ON = 0,
        RUN_BREAK,
        RUN_CONTINUE,
        RUN_NEXT,
        RUN_NEXTFILE,
        RUN_EXIT,
        RUN_RETURN
};

enum
{
        LV_CELL = 0,
        LV_FIELD,
        LV_ELEMENT
};

typedef struct
{
        p8 kind;
        b32 index;
        b32 field;
        awk_cell address_to cell;
        awk_slot address_to entry;
} awk_target;

static b32 awk_exit_code;
static bool awk_exiting;
static awk_value awk_returned;
static positive awk_seed = 1;
static positive awk_seed_state = 1;

static fn awk_eval(awk_node address_to node, awk_value address_to out);
static b32 awk_run(awk_node address_to node);
static bool awk_main_next_record(awk_text address_to address_to into);

#define awk_value_start(which)          \
        do                              \
        {                               \
                (which).text = null;    \
                (which).number = 0;     \
                (which).state = AWK_UNSET; \
        } while (0)

static fn awk_value_done(awk_value address_to which)
{
        awk_text_drop(which->text);
        which->text = null;
        which->state = AWK_UNSET;
}

static fn awk_leave(b32 code)
{
        awk_flush_everything();
        awk_close_everything();
        exit(code & 0xff);
}

static awk_text address_to awk_eval_text(awk_node address_to node)
{
        awk_value one;
        awk_text address_to made;

        awk_value_start(one);
        awk_eval(node, address_of one);
        made = awk_text_hold(awk_to_text(address_of one));
        awk_value_done(address_of one);
        return made;
}

static decimal awk_eval_number(awk_node address_to node)
{
        awk_value one;
        decimal value;

        awk_value_start(one);
        awk_eval(node, address_of one);
        value = awk_to_number(address_of one);
        awk_value_done(address_of one);
        return value;
}

static bool awk_eval_truth(awk_node address_to node)
{
        awk_value one;
        bool answer;

        awk_value_start(one);
        awk_eval(node, address_of one);
        answer = awk_truth(address_of one);
        awk_value_done(address_of one);
        return answer;
}

// The key a subscript list makes, which is the pieces joined by SUBSEP.
static awk_text address_to awk_subscript_key(awk_node address_to list, b32 count)
{
        if (count == 1)
                return awk_eval_text(list);

        awk_builder build;
        positive length;
        string_address separator = awk_separator(awk_where_subsep, address_of length);

        awk_builder_start(address_of build);

        for (awk_node address_to one = list; one; one = one->next)
        {
                awk_text address_to piece = awk_eval_text(one);

                if (one != list)
                        awk_builder_put(address_of build, separator, length);

                awk_builder_put(address_of build, piece->text, piece->length);
                awk_text_drop(piece);
        }

        return awk_builder_text(address_of build);
}

static fn awk_target_of(awk_node address_to node, awk_target address_to into)
{
        into->field = -1;
        into->index = 0;
        into->cell = null;
        into->entry = null;

        switch (node->kind)
        {
        case N_VARIABLE:
                into->kind = LV_CELL;
                into->index = node->index;
                into->cell = awk_cell_of(node->index);
                return;

        case N_FIELD:
        {
                decimal which = awk_eval_number(node->a);

                into->kind = LV_FIELD;
                into->field = (b32)which;

                if (into->field < 0)
                        awk_fatal(null, "attempt to assign to a field before the first");

                if (into->field > awk_nf)
                        awk_field_grow(into->field);

                return;
        }

        case N_SUBSCRIPT:
        {
                awk_text address_to key = awk_subscript_key(node->a, node->count);

                into->kind = LV_ELEMENT;
                into->entry = awk_array_place(awk_cell_array(awk_cell_of(node->index)),
                                              key->text, key->length);
                awk_text_drop(key);
                return;
        }
        }

        awk_fatal(null, "not something that can be assigned to");
}

static awk_value address_to awk_target_slot(awk_target address_to which)
{
        switch (which->kind)
        {
        case LV_FIELD:
                awk_fields_reserve((positive)which->field + 1);
                return address_of awk_fields[which->field];

        case LV_ELEMENT:
                return address_of which->entry->value;
        }

        return address_of which->cell->value;
}

static fn awk_target_written(awk_target address_to which)
{
        if (which->kind == LV_FIELD)
        {
                awk_field_written(which->field);
                return;
        }

        if (which->kind != LV_CELL || which->index < 0)
                return;

        if (awk_global_meaning[which->index] == AWK_NF)
                awk_nf_written((b32)awk_to_number(address_of which->cell->value));
}

static fn awk_do_assign(awk_node address_to node, awk_value address_to out)
{
        awk_target target;
        awk_value right;

        awk_value_start(right);

        if (node->sub == T_ASSIGN)
        {
                awk_eval(node->b, address_of right);
                awk_target_of(node->a, address_of target);
                awk_value_copy(awk_target_slot(address_of target), address_of right);
                awk_target_written(address_of target);
                awk_value_copy(out, address_of right);
                awk_value_done(address_of right);
                return;
        }

        awk_target_of(node->a, address_of target);

        decimal left = awk_to_number(awk_target_slot(address_of target));

        awk_eval(node->b, address_of right);

        decimal value = awk_to_number(address_of right);

        awk_value_done(address_of right);

        switch (node->sub)
        {
        case T_ASSIGN_ADD: left = left + value; break;
        case T_ASSIGN_SUB: left = left - value; break;
        case T_ASSIGN_MUL: left = left * value; break;

        case T_ASSIGN_DIV:
                if (value == 0)
                        awk_fatal(null, "division by zero attempted");

                left = left / value;
                break;

        case T_ASSIGN_MOD:
                if (value == 0)
                        awk_fatal(null, "division by zero attempted in %");

                left = awk_remainder(left, value);
                break;

        case T_ASSIGN_POWER: left = awk_power(left, value); break;
        }

        awk_set_number(awk_target_slot(address_of target), left);
        awk_target_written(address_of target);
        awk_set_number(out, left);
}

static regex_program address_to awk_program_of(awk_node address_to node)
{
        if (node->kind == N_REGEX)
        {
                if (node->program)
                        return node->program;

                return awk_regex_dynamic(node->text);
        }

        awk_text address_to pattern = awk_eval_text(node);
        regex_program address_to made = awk_regex_dynamic(pattern);

        awk_text_drop(pattern);
        return made;
}

static bool awk_matches(awk_node address_to pattern, awk_text address_to subject)
{
        regex_select(awk_program_of(pattern));
        return regex_search(subject->text, subject->length, 0);
}

static fn awk_call(awk_node address_to node, awk_value address_to out);

static fn awk_builtin(awk_node address_to node, awk_value address_to out);

static fn awk_getline(awk_node address_to node, awk_value address_to out);

static fn awk_eval(awk_node address_to node, awk_value address_to out)
{
        switch (node->kind)
        {
        case N_NUMBER:
                awk_set_number(out, node->number);
                return;

        case N_STRING:
                awk_set_text(out, awk_text_hold(node->text));
                return;

        case N_REGEX:
        {
                awk_value address_to record = awk_field(0);

                awk_set_number(out, awk_matches(node, awk_to_text(record)) ? 1 : 0);
                return;
        }

        case N_VARIABLE:
                awk_value_copy(out, address_of awk_cell_of(node->index)->value);
                return;

        case N_FIELD:
                awk_value_copy(out, awk_field((b32)awk_eval_number(node->a)));
                return;

        case N_SUBSCRIPT:
        {
                awk_text address_to key = awk_subscript_key(node->a, node->count);
                awk_slot address_to slot =
                    awk_array_place(awk_cell_array(awk_cell_of(node->index)), key->text,
                                    key->length);

                awk_text_drop(key);
                awk_value_copy(out, address_of slot->value);
                return;
        }

        case N_GROUP:
                awk_eval(node->a, out);
                return;

        case N_ASSIGN:
                awk_do_assign(node, out);
                return;

        case N_COND:
                awk_eval(awk_eval_truth(node->a) ? node->b : node->c, out);
                return;

        case N_OR:
                awk_set_number(out, awk_eval_truth(node->a) || awk_eval_truth(node->b) ? 1 : 0);
                return;

        case N_AND:
                awk_set_number(out, awk_eval_truth(node->a) && awk_eval_truth(node->b) ? 1 : 0);
                return;

        case N_NOT:
                awk_set_number(out, awk_eval_truth(node->a) ? 0 : 1);
                return;

        case N_IN:
        {
                awk_text address_to key;

                if (node->a->kind == N_GROUP)
                        key = awk_subscript_key(node->a->a, node->a->count);
                else
                        key = awk_eval_text(node->a);

                awk_array address_to array = awk_cell_array(awk_cell_of(node->index));

                awk_set_number(out, awk_array_find(array, key->text, key->length) ? 1 : 0);
                awk_text_drop(key);
                return;
        }

        case N_MATCH:
        {
                awk_text address_to subject = awk_eval_text(node->a);
                bool got = awk_matches(node->b, subject);

                awk_text_drop(subject);
                awk_set_number(out, (got != (node->sub != 0)) ? 1 : 0);
                return;
        }

        case N_COMPARE:
        {
                awk_value left;
                awk_value right;
                b32 order;

                awk_value_start(left);
                awk_value_start(right);
                awk_eval(node->a, address_of left);
                awk_eval(node->b, address_of right);
                order = awk_compare(address_of left, address_of right);
                awk_value_done(address_of left);
                awk_value_done(address_of right);

                switch (node->sub)
                {
                case T_LESS: order = order < 0; break;
                case T_LESS_EQUAL: order = order <= 0; break;
                case T_GREATER: order = order > 0; break;
                case T_GREATER_EQUAL: order = order >= 0; break;
                case T_EQUAL: order = order == 0; break;
                default: order = order != 0; break;
                }

                awk_set_number(out, (decimal)order);
                return;
        }

        case N_CONCAT:
        {
                awk_text address_to left = awk_eval_text(node->a);
                awk_text address_to right = awk_eval_text(node->b);
                awk_text address_to made = awk_text_room(left->length + right->length);

                memory_copy(made->text, left->text, left->length);
                memory_copy(made->text + left->length, right->text, right->length);
                awk_text_drop(left);
                awk_text_drop(right);
                awk_set_text(out, made);
                return;
        }

        case N_ARITH:
        {
                decimal left = awk_eval_number(node->a);
                decimal right = awk_eval_number(node->b);

                switch (node->sub)
                {
                case '+': awk_set_number(out, left + right); return;
                case '-': awk_set_number(out, left - right); return;
                case '*': awk_set_number(out, left * right); return;

                case '/':
                        if (right == 0)
                                awk_fatal(null, "division by zero attempted");

                        awk_set_number(out, left / right);
                        return;

                case '%':
                        if (right == 0)
                                awk_fatal(null, "division by zero attempted in %");

                        awk_set_number(out, awk_remainder(left, right));
                        return;
                }

                awk_set_number(out, awk_power(left, right));
                return;
        }

        case N_NEGATE:
                awk_set_number(out, -awk_eval_number(node->a));
                return;

        case N_AFFIRM:
                awk_set_number(out, awk_eval_number(node->a));
                return;

        case N_STEP:
        {
                awk_target target;

                awk_target_of(node->a, address_of target);

                decimal before = awk_to_number(awk_target_slot(address_of target));
                decimal after = node->sub ? before + 1 : before - 1;

                awk_set_number(awk_target_slot(address_of target), after);
                awk_target_written(address_of target);
                awk_set_number(out, node->count ? after : before);
                return;
        }

        case N_CALL:
                awk_call(node, out);
                return;

        case N_BUILTIN:
                awk_builtin(node, out);
                return;

        case N_GETLINE:
                awk_getline(node, out);
                return;
        }

        awk_fatal(null, "cannot evaluate this");
}

static fn awk_call(awk_node address_to node, awk_value address_to out)
{
        awk_function address_to which = address_of awk_functions[node->index];

        if (!which->defined)
                awk_fatal(which->name->text, "calling a function that is not defined");

        b32 base = awk_frame + awk_frame_size;
        b32 count = which->parameters;
        b32 kept_frame = awk_frame;
        b32 kept_size = awk_frame_size;
        awk_node address_to argument = node->a;

        if (base + count >= AWK_FRAME_MAX)
                awk_fatal(which->name->text, "too deep");

        awk_frame_size += count;

        for (b32 i = 0; i < count; i++)
        {
                awk_cell address_to cell = address_of awk_stack[base + i];

                cell->kind = AWK_CELL_UNKNOWN;
                cell->owned = false;
                cell->link = null;
                cell->array = null;
                cell->value.text = null;
                cell->value.number = 0;
                cell->value.state = AWK_UNSET;

                if (!argument)
                        continue;

                if (argument->kind == N_VARIABLE)
                {
                        awk_cell address_to source = awk_cell_of(argument->index);

                        if (source->kind != AWK_CELL_SCALAR)
                                cell->link = source;

                        if (source->kind != AWK_CELL_ARRAY)
                                awk_value_copy(address_of cell->value, address_of source->value);
                }
                else
                        awk_eval(argument, address_of cell->value);

                argument = argument->next;
        }

        awk_frame = base;
        awk_frame_size = count;

        b32 answer = which->body ? awk_run(which->body) : RUN_ON;

        if (answer == RUN_RETURN)
        {
                awk_value_copy(out, address_of awk_returned);
                awk_value_done(address_of awk_returned);
        }
        else
                awk_value_clear(out);

        for (b32 i = 0; i < count; i++)
        {
                awk_cell address_to cell = address_of awk_stack[base + i];

                awk_value_done(address_of cell->value);

                if (cell->owned && cell->array)
                {
                        awk_array_empty(cell->array);
                        awk_give(cell->array->buckets);
                        awk_give(cell->array);
                }

                cell->array = null;
                cell->owned = false;
                cell->link = null;
        }

        awk_frame = kept_frame;
        awk_frame_size = kept_size;

        // exit inside a function still has to leave, and the answer above
        // told the caller nothing about it.
        if (answer == RUN_EXIT)
                awk_exiting = true;
}

static awk_text address_to awk_replace(awk_text address_to subject, regex_program address_to program,
                                       awk_text address_to with, bool every, b32 address_to made)
{
        awk_builder build;
        positive done = 0;
        positive at = 0;
        positive last = TEXT_UNSET;

        address_to made = 0;
        awk_builder_start(address_of build);

        while (at <= subject->length)
        {
                regex_select(program);

                if (!regex_search_longest(subject->text, subject->length, at))
                        break;

                positive start = regex_slots[0];
                positive stop = regex_slots[1];

                // An empty match where the last one ended would put the same
                // replacement in twice.
                if (start == stop && start == last)
                {
                        if (start >= subject->length)
                                break;

                        at = start + 1;
                        continue;
                }

                awk_builder_put(address_of build, subject->text + done, start - done);

                /*
                        A backslash before an ampersand takes its meaning
                        away. A backslash before anything else is a
                        backslash, including before another backslash --
                        which is not what the standard says and is what the
                        awk this is measured against does. The one place two
                        of them collapse is directly before an ampersand that
                        is meant to be the match.
                */
                for (positive i = 0; i < with->length; i++)
                {
                        if (with->text[i] == '\\' && i + 1 < with->length)
                        {
                                if (with->text[i + 1] == '&')
                                {
                                        awk_builder_char(address_of build, '&');
                                        i++;
                                        continue;
                                }

                                if (with->text[i + 1] == '\\' && i + 2 < with->length &&
                                    with->text[i + 2] == '&')
                                {
                                        awk_builder_char(address_of build, '\\');
                                        i++;
                                        continue;
                                }

                                awk_builder_char(address_of build, '\\');
                                continue;
                        }

                        if (with->text[i] == '&')
                        {
                                awk_builder_put(address_of build, subject->text + start,
                                                stop - start);
                                continue;
                        }

                        awk_builder_char(address_of build, with->text[i]);
                }

                address_to made += 1;
                done = stop;
                last = stop;
                at = start == stop ? start + 1 : stop;

                if (!every)
                        break;
        }

        if (!address_to made)
                return awk_text_hold(subject);

        awk_builder_put(address_of build, subject->text + done, subject->length - done);
        return awk_builder_text(address_of build);
}

static fn awk_builtin(awk_node address_to node, awk_value address_to out)
{
        awk_node address_to first = node->a;
        awk_node address_to second = first ? first->next : null;
        awk_node address_to third = second ? second->next : null;

        switch (node->index)
        {
        case B_LENGTH:
        {
                if (!node->count)
                {
                        awk_set_number(out, (decimal)awk_to_text(awk_field(0))->length);
                        return;
                }

                if (first->kind == N_VARIABLE)
                {
                        awk_cell address_to cell = awk_cell_of(first->index);

                        while (cell->link)
                                cell = cell->link;

                        if (cell->kind == AWK_CELL_ARRAY && cell->array)
                        {
                                awk_set_number(out, (decimal)cell->array->count);
                                return;
                        }
                }

                awk_text address_to text = awk_eval_text(first);

                awk_set_number(out, (decimal)text->length);
                awk_text_drop(text);
                return;
        }

        case B_SUBSTR:
        {
                awk_text address_to text = awk_eval_text(first);
                decimal start = awk_truncate(awk_eval_number(second));
                positive from;
                positive want;

                if (start < 1)
                        start = 1;

                from = start > (decimal)text->length ? text->length : (positive)start - 1;

                if (third)
                {
                        decimal length = awk_truncate(awk_eval_number(third));

                        if (length < 0)
                                length = 0;

                        want = length > (decimal)text->length ? text->length : (positive)length;
                }
                else
                        want = text->length;

                if (from + want > text->length)
                        want = text->length - from;

                awk_set_text(out, awk_text_new(text->text + from, want));
                awk_text_drop(text);
                return;
        }

        case B_INDEX:
        {
                awk_text address_to text = awk_eval_text(first);
                awk_text address_to want = awk_eval_text(second);
                positive answer = 0;

                if (want->length <= text->length)
                        for (positive i = 0; i + want->length <= text->length; i++)
                                if (!memory_compare(text->text + i, want->text, want->length))
                                {
                                        answer = i + 1;
                                        break;
                                }

                awk_text_drop(text);
                awk_text_drop(want);
                awk_set_number(out, (decimal)answer);
                return;
        }

        case B_SPLIT:
        {
                awk_text address_to text = awk_eval_text(first);
                awk_array address_to array = awk_cell_array(awk_cell_of(second->index));
                string_address separator;
                positive separator_length;
                awk_text address_to held = null;
                bool pattern = false;

                if (second->kind != N_VARIABLE)
                        awk_fatal(null, "split wants an array");

                if (!third)
                        separator = awk_separator(awk_where_fs, address_of separator_length);
                else if (third->kind == N_REGEX)
                {
                        held = awk_text_hold(third->text);
                        separator = held->text;
                        separator_length = held->length;
                        pattern = true;
                }
                else
                {
                        held = awk_eval_text(third);
                        separator = held->text;
                        separator_length = held->length;
                }

                awk_array_empty(array);
                awk_split_pieces(text->text, text->length, separator, separator_length, false,
                                 pattern);

                for (positive i = 0; i < awk_piece_count; i++)
                {
                        p8 name[24];
                        positive value = i + 1;
                        b32 have = 0;
                        p8 digits[24];

                        while (value)
                        {
                                digits[have++] = (p8)('0' + value % 10);
                                value /= 10;
                        }

                        positive at = 0;

                        while (have)
                                name[at++] = digits[--have];

                        awk_slot address_to slot = awk_array_place(array, name, at);

                        awk_set_input_bytes(address_of slot->value,
                                            text->text + awk_piece_start[i],
                                            awk_piece_length[i]);
                }

                awk_set_number(out, (decimal)awk_piece_count);
                awk_text_drop(text);
                awk_text_drop(held);
                return;
        }

        case B_SUB:
        case B_GSUB:
        {
                awk_text address_to with = awk_eval_text(second);
                awk_target target;
                awk_node address_to where = third;
                awk_node holder;
                b32 count = 0;

                if (!where)
                {
                        memory_fill(address_of holder, 0, sizeof(awk_node));
                        holder.kind = N_FIELD;
                        holder.a = awk_node_new(N_NUMBER);
                        where = address_of holder;
                }

                awk_target_of(where, address_of target);

                // Last, because everything above can evaluate an expression
                // and an expression can compile a pattern of its own.
                regex_program address_to program = awk_program_of(first);

                awk_text address_to subject = awk_text_hold(awk_to_text(awk_target_slot(address_of target)));
                awk_text address_to made = awk_replace(subject, program, with, node->index == B_GSUB,
                                                       address_of count);

                if (count)
                {
                        awk_set_text(awk_target_slot(address_of target), made);
                        awk_target_written(address_of target);
                }
                else
                        awk_text_drop(made);

                awk_text_drop(subject);
                awk_text_drop(with);
                awk_set_number(out, (decimal)count);
                return;
        }

        case B_MATCH:
        {
                awk_text address_to text = awk_eval_text(first);

                regex_select(awk_program_of(second));

                if (regex_search_longest(text->text, text->length, 0))
                {
                        awk_set_global_number(awk_where_rstart, (decimal)(regex_slots[0] + 1));
                        awk_set_global_number(awk_where_rlength,
                                              (decimal)(regex_slots[1] - regex_slots[0]));
                        awk_set_number(out, (decimal)(regex_slots[0] + 1));
                }
                else
                {
                        awk_set_global_number(awk_where_rstart, 0);
                        awk_set_global_number(awk_where_rlength, -1);
                        awk_set_number(out, 0);
                }

                awk_text_drop(text);
                return;
        }

        case B_SPRINTF:
        {
                awk_text address_to format = awk_eval_text(first);
                awk_value room[64];
                b32 have = 0;

                for (awk_node address_to one = second; one && have < 64; one = one->next)
                {
                        awk_value_start(room[have]);
                        awk_eval(one, address_of room[have]);
                        have++;
                }

                awk_set_text(out, awk_sprintf(format->text, format->length, room, have));

                for (b32 i = 0; i < have; i++)
                        awk_value_done(address_of room[i]);

                awk_text_drop(format);
                return;
        }

        case B_SIN: awk_set_number(out, awk_sin(awk_eval_number(first))); return;
        case B_COS: awk_set_number(out, awk_cos(awk_eval_number(first))); return;

        case B_ATAN2:
                awk_set_number(out, awk_atan2(awk_eval_number(first), awk_eval_number(second)));
                return;

        case B_EXP: awk_set_number(out, awk_exp(awk_eval_number(first))); return;
        case B_LOG: awk_set_number(out, awk_log(awk_eval_number(first))); return;
        case B_SQRT: awk_set_number(out, awk_sqrt(awk_eval_number(first))); return;

        case B_INT:
                awk_set_number(out, awk_truncate(node->count ? awk_eval_number(first) : 0));
                return;

        case B_RAND:
        {
                awk_seed_state = awk_seed_state * 6364136223846793005ull + 1442695040888963407ull;

                positive value = (awk_seed_state >> 11) & (((positive)1 << 53) - 1);

                awk_set_number(out, (decimal)value / 9007199254740992.0);
                return;
        }

        case B_SRAND:
        {
                positive before = awk_seed;

                if (node->count)
                        awk_seed = (positive)(bipolar)awk_eval_number(first);
                else
                {
                        positive when[2] = {0, 0};

                        system_call_2(syscall(clock_gettime), 0, (positive)when);
                        awk_seed = when[0];
                }

                awk_seed_state = awk_seed + 0x9e3779b97f4a7c15ull;
                awk_set_number(out, (decimal)before);
                return;
        }

        case B_TOLOWER:
        case B_TOUPPER:
        {
                awk_text address_to text = awk_eval_text(first);
                awk_text address_to made = awk_text_room(text->length);

                for (positive i = 0; i < text->length; i++)
                {
                        p8 character = text->text[i];

                        if (node->index == B_TOLOWER)
                                made->text[i] = character >= 'A' && character <= 'Z'
                                                    ? (p8)(character + 32)
                                                    : character;
                        else
                                made->text[i] = character >= 'a' && character <= 'z'
                                                    ? (p8)(character - 32)
                                                    : character;
                }

                awk_text_drop(text);
                awk_set_text(out, made);
                return;
        }

        case B_SYSTEM:
        {
                awk_text address_to command = awk_eval_text(first);
                bipolar child = awk_spawn(command->text, -1, -1);

                awk_text_drop(command);
                awk_set_number(out, (decimal)awk_wait_for(child));
                return;
        }

        case B_CLOSE:
        {
                awk_text address_to name = awk_eval_text(first);

                awk_set_number(out, (decimal)awk_close_named(name));
                awk_text_drop(name);
                return;
        }

        case B_FFLUSH:
                if (node->count)
                {
                        awk_text address_to name = awk_eval_text(first);

                        for (b32 i = 0; i < AWK_STREAMS_MAX; i++)
                                if (awk_writers[i].live &&
                                    awk_writers[i].name->length == name->length &&
                                    !memory_compare(awk_writers[i].name->text, name->text,
                                                    name->length))
                                        awk_writer_flush(address_of awk_writers[i]);

                        awk_text_drop(name);
                }
                else
                        awk_flush_everything();

                awk_set_number(out, 0);
                return;
        }

        awk_fatal(null, "no such function");
}

static fn awk_getline_store(awk_node address_to node, awk_text address_to record)
{
        if (!node->a)
        {
                awk_record_set(record->text, record->length);
                return;
        }

        awk_target target;

        awk_target_of(node->a, address_of target);
        awk_set_input(awk_target_slot(address_of target), awk_text_hold(record));
        awk_target_written(address_of target);
}

static fn awk_getline(awk_node address_to node, awk_value address_to out)
{
        awk_text address_to record = null;
        b32 answer = 0;

        if (node->sub == G_MAIN)
        {
                if (awk_main_next_record(address_of record))
                {
                        awk_getline_store(node, record);
                        awk_text_drop(record);
                        answer = 1;
                }
        }
        else
        {
                awk_text address_to name = awk_eval_text(node->b);
                awk_reader address_to from = awk_reader_for(name, node->sub == G_COMMAND);

                awk_text_drop(name);

                if (!from)
                        answer = -1;
                else if (awk_read_record(from, address_of record))
                {
                        awk_getline_store(node, record);
                        awk_text_drop(record);
                        answer = 1;
                }
        }

        awk_set_number(out, (decimal)answer);
}

static awk_writer address_to awk_output_of(awk_node address_to node)
{
        if (!node->sub)
                return address_of awk_standard_out;

        awk_text address_to name = awk_eval_text(node->b);
        awk_writer address_to where =
            awk_writer_for(name, (p8)(node->sub == R_FILE ? AWK_TO_FILE
                                                          : (node->sub == R_APPEND ? AWK_TO_APPEND
                                                                                   : AWK_TO_PIPE)));

        awk_text_drop(name);
        return where;
}

static fn awk_do_print(awk_node address_to node)
{
        awk_builder build;
        positive ofs_length;
        positive ors_length;
        string_address ofs = awk_separator(awk_where_ofs, address_of ofs_length);
        string_address ors;
        awk_writer address_to where;

        awk_builder_start(address_of build);

        if (!node->a)
        {
                awk_text address_to record = awk_to_text(awk_field(0));

                awk_builder_put(address_of build, record->text, record->length);
        }
        else
                for (awk_node address_to one = node->a; one; one = one->next)
                {
                        awk_value value;

                        awk_value_start(value);
                        awk_eval(one, address_of value);

                        awk_text address_to piece = awk_to_output_text(address_of value);

                        if (one != node->a)
                                awk_builder_put(address_of build, ofs, ofs_length);

                        awk_builder_put(address_of build, piece->text, piece->length);
                        awk_text_drop(piece);
                        awk_value_done(address_of value);
                }

        ors = awk_separator(awk_where_ors, address_of ors_length);
        awk_builder_put(address_of build, ors, ors_length);
        where = awk_output_of(node);
        awk_writer_put(where, build.data, build.used);

        if (build.heap)
                awk_give(build.data);
}

static fn awk_do_printf(awk_node address_to node)
{
        awk_value room[64];
        b32 have = 0;
        awk_writer address_to where;

        if (!node->a)
                awk_fatal(null, "printf wants a format");

        awk_text address_to format = awk_eval_text(node->a);

        for (awk_node address_to one = node->a->next; one && have < 64; one = one->next)
        {
                awk_value_start(room[have]);
                awk_eval(one, address_of room[have]);
                have++;
        }

        awk_text address_to made = awk_sprintf(format->text, format->length, room, have);

        for (b32 i = 0; i < have; i++)
                awk_value_done(address_of room[i]);

        where = awk_output_of(node);
        awk_writer_put(where, made->text, made->length);
        awk_text_drop(made);
        awk_text_drop(format);
}

static b32 awk_run(awk_node address_to node)
{
        for (; node; node = node->next)
        {
                if (awk_exiting)
                        return RUN_EXIT;

                switch (node->kind)
                {
                case S_BLOCK:
                {
                        b32 answer = awk_run(node->a);

                        if (answer != RUN_ON)
                                return answer;

                        break;
                }

                case S_EXPRESSION:
                {
                        awk_value value;

                        awk_value_start(value);
                        awk_eval(node->a, address_of value);
                        awk_value_done(address_of value);
                        break;
                }

                case S_PRINT:
                        awk_do_print(node);
                        break;

                case S_PRINTF:
                        awk_do_printf(node);
                        break;

                case S_IF:
                {
                        b32 answer = RUN_ON;

                        if (awk_eval_truth(node->a))
                                answer = node->b ? awk_run(node->b) : RUN_ON;
                        else if (node->c)
                                answer = awk_run(node->c);

                        if (answer != RUN_ON)
                                return answer;

                        break;
                }

                case S_WHILE:
                        while (!awk_exiting && awk_eval_truth(node->a))
                        {
                                b32 answer = node->b ? awk_run(node->b) : RUN_ON;

                                if (answer == RUN_BREAK)
                                        break;

                                if (answer != RUN_ON && answer != RUN_CONTINUE)
                                        return answer;
                        }

                        break;

                case S_DO:
                        for (;;)
                        {
                                b32 answer = node->b ? awk_run(node->b) : RUN_ON;

                                if (answer == RUN_BREAK)
                                        break;

                                if (answer != RUN_ON && answer != RUN_CONTINUE)
                                        return answer;

                                if (awk_exiting || !awk_eval_truth(node->a))
                                        break;
                        }

                        break;

                case S_FOR:
                {
                        if (node->a)
                        {
                                b32 answer = awk_run(node->a);

                                if (answer != RUN_ON)
                                        return answer;
                        }

                        while (!awk_exiting && (!node->b || awk_eval_truth(node->b)))
                        {
                                b32 answer = node->d ? awk_run(node->d) : RUN_ON;

                                if (answer == RUN_BREAK)
                                        break;

                                if (answer != RUN_ON && answer != RUN_CONTINUE)
                                        return answer;

                                if (node->c)
                                {
                                        answer = awk_run(node->c);

                                        if (answer != RUN_ON)
                                                return answer;
                                }
                        }

                        break;
                }

                case S_FORIN:
                {
                        awk_array address_to array = awk_cell_array(awk_cell_of(node->count));
                        awk_target target;
                        positive have = 0;
                        awk_text address_to address_to keys;
                        b32 answer = RUN_ON;

                        if (!array->count)
                                break;

                        keys = (awk_text address_to address_to)awk_take(array->count *
                                                                        sizeof(address_any));

                        for (positive i = 0; i < array->width; i++)
                                for (awk_slot address_to slot = array->buckets[i]; slot;
                                     slot = slot->next)
                                        keys[have++] = awk_text_hold(slot->key);

                        awk_node holder;

                        memory_fill(address_of holder, 0, sizeof(awk_node));
                        holder.kind = N_VARIABLE;
                        holder.index = node->index;

                        for (positive i = 0; i < have && answer == RUN_ON; i++)
                        {
                                if (awk_exiting)
                                {
                                        answer = RUN_EXIT;
                                        break;
                                }

                                awk_target_of(address_of holder, address_of target);
                                awk_set_input(awk_target_slot(address_of target),
                                              awk_text_hold(keys[i]));
                                awk_target_written(address_of target);

                                b32 got = node->b ? awk_run(node->b) : RUN_ON;

                                if (got == RUN_BREAK)
                                        break;

                                if (got != RUN_ON && got != RUN_CONTINUE)
                                        answer = got;
                        }

                        for (positive i = 0; i < have; i++)
                                awk_text_drop(keys[i]);

                        awk_give(keys);

                        if (answer != RUN_ON)
                                return answer;

                        break;
                }

                case S_DELETE:
                {
                        awk_array address_to array = awk_cell_array(awk_cell_of(node->index));

                        if (!node->a)
                        {
                                awk_array_empty(array);
                                break;
                        }

                        awk_text address_to key = awk_subscript_key(node->a->a, node->a->count);

                        awk_array_remove(array, key->text, key->length);
                        awk_text_drop(key);
                        break;
                }

                case S_NEXT:
                        return RUN_NEXT;

                case S_NEXTFILE:
                        return RUN_NEXTFILE;

                case S_BREAK:
                        return RUN_BREAK;

                case S_CONTINUE:
                        return RUN_CONTINUE;

                case S_EXIT:
                        if (node->a)
                                awk_exit_code = (b32)awk_eval_number(node->a);

                        awk_exiting = true;
                        return RUN_EXIT;

                case S_RETURN:
                        awk_value_done(address_of awk_returned);

                        if (node->a)
                                awk_eval(node->a, address_of awk_returned);

                        return RUN_RETURN;
                }
        }

        return RUN_ON;
}

/*
        The input, in the order the arguments give it.

        ARGV is walked rather than copied, because a program is allowed to
        change it while it runs: an element that is a name=value assignment is
        performed where it stands rather than opened, which is how awk lets a
        variable change between one file and the next.
*/
static awk_reader awk_main;
static bool awk_main_live;
static b32 awk_argv_at = 1;
static bool awk_input_used;

static awk_text address_to awk_unescape(string_address text, positive length)
{
        awk_builder build;

        awk_builder_start(address_of build);

        for (positive i = 0; i < length; i++)
        {
                if (text[i] != '\\' || i + 1 >= length)
                {
                        awk_builder_char(address_of build, text[i]);
                        continue;
                }

                i++;

                p8 character = text[i];

                switch (character)
                {
                case 'n': awk_builder_char(address_of build, '\n'); continue;
                case 't': awk_builder_char(address_of build, '\t'); continue;
                case 'r': awk_builder_char(address_of build, '\r'); continue;
                case 'a': awk_builder_char(address_of build, 7); continue;
                case 'b': awk_builder_char(address_of build, 8); continue;
                case 'f': awk_builder_char(address_of build, 12); continue;
                case 'v': awk_builder_char(address_of build, 11); continue;
                case '\\': awk_builder_char(address_of build, '\\'); continue;
                case '"': awk_builder_char(address_of build, '"'); continue;
                case '/': awk_builder_char(address_of build, '/'); continue;
                }

                if (character >= '0' && character <= '7')
                {
                        b32 value = character - '0';

                        for (b32 step = 0; step < 2 && i + 1 < length; step++)
                        {
                                p8 next = text[i + 1];

                                if (next < '0' || next > '7')
                                        break;

                                value = value * 8 + (next - '0');
                                i++;
                        }

                        awk_builder_char(address_of build, (p8)(value & 0xff));
                        continue;
                }

                awk_builder_char(address_of build, character);
        }

        return awk_builder_text(address_of build);
}

static awk_text address_to awk_number_key(positive value)
{
        p8 digits[24];
        p8 room[24];
        b32 have = 0;
        positive at = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (have)
                room[at++] = digits[--have];

        return awk_text_new(room, at);
}

static bool awk_assignment(string_address text, positive length)
{
        positive at = 0;

        if (!length || !awk_name_start(text[0]))
                return false;

        while (at < length && awk_name_part(text[at]))
                at++;

        if (at >= length || text[at] != '=')
                return false;

        awk_text address_to name = awk_text_new(text, at);
        awk_text address_to value = awk_unescape(text + at + 1, length - at - 1);
        b32 where = awk_global_find(name->text, name->length);

        awk_set_input(address_of awk_globals[where].value, value);
        awk_globals[where].kind = AWK_CELL_SCALAR;

        if (awk_global_meaning[where] == AWK_NF)
                awk_nf_written((b32)awk_global_number(where));

        awk_text_drop(name);
        return true;
}

static bool awk_open_next_input()
{
        awk_array address_to argv = awk_cell_array(address_of awk_globals[awk_where_argv]);

        for (;;)
        {
                b32 count = (b32)awk_global_number(awk_where_argc);

                if (awk_argv_at >= count)
                {
                        if (awk_input_used)
                                return false;

                        awk_input_used = true;
                        awk_main.handle = 0;
                        awk_main.live = true;
                        awk_main.pipe = false;
                        awk_main.ended = false;
                        awk_main.at = 0;
                        awk_main.filled = 0;
                        awk_main.name = awk_text_hold(address_of awk_empty_text);
                        awk_set_global_number(awk_where_fnr, 0);
                        awk_main_live = true;
                        return true;
                }

                awk_text address_to key = awk_number_key((positive)awk_argv_at++);
                awk_slot address_to slot = awk_array_find(argv, key->text, key->length);

                awk_text_drop(key);

                if (!slot)
                        continue;

                awk_text address_to name = awk_text_hold(awk_to_text(address_of slot->value));

                if (!name->length)
                {
                        awk_text_drop(name);
                        continue;
                }

                if (awk_assignment(name->text, name->length))
                {
                        awk_text_drop(name);
                        continue;
                }

                awk_input_used = true;
                awk_main.pipe = false;
                awk_main.ended = false;
                awk_main.at = 0;
                awk_main.filled = 0;
                awk_main.live = true;
                awk_main.name = awk_text_hold(name);

                if (name->length == 1 && name->text[0] == '-')
                        awk_main.handle = 0;
                else
                {
                        bipolar handle = text_open_handle(name->text, FILE_READ, 0);

                        if (handle < 0)
                        {
                                awk_text_drop(name);
                                awk_fatal(awk_main.name->text, "cannot open file for reading");
                        }

                        awk_main.handle = (b32)handle;
                }

                awk_set_text(address_of awk_globals[awk_where_filename].value,
                             awk_text_hold(name));
                awk_globals[awk_where_filename].value.state |= AWK_STRNUM;
                awk_set_global_number(awk_where_fnr, 0);
                awk_text_drop(name);
                awk_main_live = true;
                return true;
        }
}

static fn awk_close_main()
{
        if (!awk_main_live)
                return;

        if (awk_main.handle > 2)
                system_call_1(syscall(close), (positive)awk_main.handle);

        awk_text_drop(awk_main.name);
        awk_main.name = null;
        awk_main.live = false;
        awk_main_live = false;
}

static bool awk_main_next_record(awk_text address_to address_to into)
{
        for (;;)
        {
                if (!awk_main_live && !awk_open_next_input())
                        return false;

                if (awk_read_record(address_of awk_main, into))
                {
                        awk_set_global_number(awk_where_nr, awk_global_number(awk_where_nr) + 1);
                        awk_set_global_number(awk_where_fnr,
                                              awk_global_number(awk_where_fnr) + 1);
                        return true;
                }

                awk_close_main();
        }
}

/*
        The rules, in the order they were written.

        A pattern with a comma is a range, and whether it is open is kept on
        the rule itself: the line that starts one is tested against the line
        that ends it before the next line is read, so /a/,/a/ is one line and
        not every line to the end.
*/
static b32 awk_run_rules()
{
        b32 answer = RUN_ON;
        bool wanted = false;

        for (b32 i = 0; i < awk_rule_count; i++)
                if (awk_rules[i].kind == RULE_BEGIN)
                {
                        answer = awk_run(awk_rules[i].action);

                        if (answer == RUN_EXIT)
                                break;
                }

        for (b32 i = 0; i < awk_rule_count; i++)
                if (awk_rules[i].kind != RULE_BEGIN)
                        wanted = true;

        while (answer != RUN_EXIT && wanted)
        {
                awk_text address_to record;

                if (!awk_main_next_record(address_of record))
                        break;

                awk_record_set(record->text, record->length);
                awk_text_drop(record);

                for (b32 i = 0; i < awk_rule_count; i++)
                {
                        awk_rule address_to rule = address_of awk_rules[i];
                        bool matched;

                        if (rule->kind != RULE_PLAIN)
                                continue;

                        if (!rule->first)
                                matched = true;
                        else if (!rule->second)
                                matched = awk_eval_truth(rule->first);
                        else if (rule->running)
                        {
                                matched = true;

                                if (awk_eval_truth(rule->second))
                                        rule->running = false;
                        }
                        else if (awk_eval_truth(rule->first))
                        {
                                matched = true;
                                rule->running = !awk_eval_truth(rule->second);
                        }
                        else
                                matched = false;

                        if (!matched)
                                continue;

                        if (!rule->action)
                        {
                                awk_text address_to line = awk_to_text(awk_field(0));
                                positive length;
                                string_address ors = awk_separator(awk_where_ors,
                                                                   address_of length);

                                awk_writer_put(address_of awk_standard_out, line->text,
                                               line->length);
                                awk_writer_put(address_of awk_standard_out, ors, length);
                                continue;
                        }

                        answer = awk_run(rule->action);

                        if (answer == RUN_NEXT || answer == RUN_EXIT)
                                break;

                        if (answer == RUN_NEXTFILE)
                        {
                                awk_close_main();
                                break;
                        }

                        answer = RUN_ON;
                }

                if (answer == RUN_EXIT)
                        break;

                answer = RUN_ON;
        }

        awk_exiting = false;

        for (b32 i = 0; i < awk_rule_count; i++)
                if (awk_rules[i].kind == RULE_END)
                {
                        if (awk_run(awk_rules[i].action) == RUN_EXIT)
                                break;
                }

        return awk_exit_code;
}

static fn awk_start()
{
        awk_infinity = awk_from_bits((positive)0x7ff0000000000000ull);
        awk_not_a_number = awk_from_bits((positive)0x7ff8000000000000ull);
        awk_returned.state = AWK_UNSET;
        awk_field_nothing.state = AWK_UNSET;
        awk_standard_out.handle = 1;
        awk_standard_out.used = 0;
        awk_standard_out.live = true;
        awk_standard_out.kind = AWK_TO_FILE;

        for (b32 i = 0; i < 1023 && program_environment(i); i++)
                awk_child_environment[i] = program_environment(i);

        awk_where_fs = awk_global_find("FS", 2);
        awk_where_ofs = awk_global_find("OFS", 3);
        awk_where_ors = awk_global_find("ORS", 3);
        awk_where_rs = awk_global_find("RS", 2);
        awk_where_nr = awk_global_find("NR", 2);
        awk_where_nf = awk_global_find("NF", 2);
        awk_where_fnr = awk_global_find("FNR", 3);
        awk_where_filename = awk_global_find("FILENAME", 8);
        awk_where_subsep = awk_global_find("SUBSEP", 6);
        awk_where_rstart = awk_global_find("RSTART", 6);
        awk_where_rlength = awk_global_find("RLENGTH", 7);
        awk_where_convfmt = awk_global_find("CONVFMT", 7);
        awk_where_ofmt = awk_global_find("OFMT", 4);
        awk_where_environ = awk_global_find("ENVIRON", 7);
        awk_where_argv = awk_global_find("ARGV", 4);
        awk_where_argc = awk_global_find("ARGC", 4);

        awk_global_meaning[awk_where_nf] = AWK_NF;

        awk_set_bytes(address_of awk_globals[awk_where_fs].value, " ", 1);
        awk_set_bytes(address_of awk_globals[awk_where_ofs].value, " ", 1);
        awk_set_bytes(address_of awk_globals[awk_where_ors].value, "\n", 1);
        awk_set_bytes(address_of awk_globals[awk_where_rs].value, "\n", 1);
        awk_set_bytes(address_of awk_globals[awk_where_subsep].value, "\034", 1);
        awk_set_bytes(address_of awk_globals[awk_where_convfmt].value, "%.6g", 4);
        awk_set_bytes(address_of awk_globals[awk_where_ofmt].value, "%.6g", 4);
        awk_set_bytes(address_of awk_globals[awk_where_filename].value, "", 0);
        awk_set_global_number(awk_where_nr, 0);
        awk_set_global_number(awk_where_nf, 0);
        awk_set_global_number(awk_where_fnr, 0);
        awk_set_global_number(awk_where_rstart, 0);
        awk_set_global_number(awk_where_rlength, -1);

        awk_array address_to environ = awk_cell_array(address_of awk_globals[awk_where_environ]);

        for (b32 i = 0; program_environment(i); i++)
        {
                string_address entry = program_environment(i);
                positive at = 0;

                while (entry[at] && entry[at] != '=')
                        at++;

                if (!entry[at])
                        continue;

                awk_slot address_to slot = awk_array_place(environ, entry, at);

                awk_set_input_bytes(address_of slot->value, entry + at + 1,
                                    string_length(entry + at + 1));
        }

        awk_fields_reserve(1);
        awk_set_bytes(address_of awk_fields[0], "", 0);
}

static fn awk_usage()
{
        text_error_raw("usage: awk [-F sepstring] [-v assignment]... program"
                       " [argument...]\n");
        text_error_raw("       awk [-F sepstring] -f progfile [-f progfile]..."
                       " [-v assignment]... [argument...]\n");
        awk_leave(1);
}

static b32 text_awk()
{
        awk_builder program;
        bool have_program = false;
        b32 at = 1;
        awk_text address_to separator = null;
        awk_text address_to pending[64];
        b32 pending_count = 0;

        text_begin("awk");
        awk_builder_start(address_of program);
        awk_start();

        while (at < text_argument_count)
        {
                string_address argument = text_argument(at);
                positive length = string_length(argument);

                if (argument[0] != '-' || length == 1)
                        break;

                if (argument[1] == '-' && length == 2)
                {
                        at++;
                        break;
                }

                p8 letter = argument[1];
                string_address value = null;

                if (letter != 'f' && letter != 'v' && letter != 'F')
                {
                        text_error(argument, "unknown option");
                        awk_usage();
                }

                if (length > 2)
                        value = argument + 2;
                else if (at + 1 < text_argument_count)
                        value = text_argument(++at);
                else
                {
                        text_error(argument, "wants an argument");
                        awk_usage();
                }

                at++;

                if (letter == 'F')
                {
                        awk_text_drop(separator);
                        separator = awk_unescape(value, string_length(value));
                        continue;
                }

                if (letter == 'v')
                {
                        if (pending_count < 64)
                                pending[pending_count++] = awk_text_new(value,
                                                                        string_length(value));

                        continue;
                }

                bipolar handle = value[0] == '-' && !value[1]
                                     ? 0
                                     : text_open_handle(value, FILE_READ, 0);

                if (handle < 0)
                        awk_fatal(value, "cannot open the program file");

                for (;;)
                {
                        p8 room[65536];
                        bipolar got = system_call_3(syscall(read), (positive)handle,
                                                    (positive)room, sizeof(room));

                        if (got <= 0)
                                break;

                        awk_builder_put(address_of program, room, (positive)got);
                }

                if (handle > 0)
                        system_call_1(syscall(close), (positive)handle);

                awk_builder_char(address_of program, '\n');
                have_program = true;
        }

        if (!have_program)
        {
                if (at >= text_argument_count)
                        awk_usage();

                string_address text = text_argument(at++);

                awk_builder_put(address_of program, text, string_length(text));
        }

        awk_array address_to argv = awk_cell_array(address_of awk_globals[awk_where_argv]);
        awk_text address_to zero = awk_number_key(0);
        awk_slot address_to slot = awk_array_place(argv, zero->text, zero->length);
        b32 count = 1;

        awk_set_input_bytes(address_of slot->value, "awk", 3);
        awk_text_drop(zero);

        for (b32 i = at; i < text_argument_count; i++)
        {
                awk_text address_to key = awk_number_key((positive)count++);
                string_address one = text_argument(i);

                slot = awk_array_place(argv, key->text, key->length);
                awk_set_input_bytes(address_of slot->value, one, string_length(one));
                awk_text_drop(key);
        }

        awk_set_global_number(awk_where_argc, (decimal)count);

        awk_source = program.data;
        awk_source_length = program.used;
        awk_source_at = 0;
        awk_parse_program();
        awk_regex_mark();

        if (separator)
        {
                awk_set_text(address_of awk_globals[awk_where_fs].value, separator);
                separator = null;
        }

        for (b32 i = 0; i < pending_count; i++)
        {
                awk_assignment(pending[i]->text, pending[i]->length);
                awk_text_drop(pending[i]);
        }

        b32 answer = awk_run_rules();

        awk_leave(answer);
        return answer;
}
