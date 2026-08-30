#include "../compiler_memory.c"

/*
        The rest of <math.h>, checked without a reference library under it.

        A math test has a problem the rest of the suite does not: there is
        nothing in a freestanding program to compare an answer against. So
        the comparison is baked in. Every table below is a list of arguments
        and the correctly rounded answers to them, and those answers were
        taken from glibc's LONG DOUBLE routines on x86_64 and rounded back to
        a double -- an eighty bit reference with eleven more significant bits
        than the answer it is judging, which makes it a fair one. They are
        written as bit patterns rather than as decimal literals so that
        nothing about them depends on the compiler's own parsing.

        The allowance beside each table is how many units in the last place
        this library is permitted to be away from that answer. It is two for
        the transcendentals, which is the accuracy the family claims, and
        zero for the ones that have exact answers and must produce them.

        The rest of the file is the identities and the special values: the
        classes of every kind of number the format holds, both signed zeros
        where they have to survive, the round trips that must come back
        unchanged, and the handful of arguments where the standard says
        exactly what the answer is.

        This runs on all three architectures. arm64 and riscv64 produce
        byte identical results to x86_64 on everything here.
*/

#if defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic ignored "-Woverflow"
#endif

#include "named_cases.inc"

typedef union
{
        decimal value;
        p64 bits;
} math_test_shape;

typedef struct
{
        p64 argument;
        p64 second;
        p64 answer;
} math_reference;

typedef decimal(address_to math_test_one)(decimal);
typedef decimal(address_to math_test_two)(decimal, decimal);

static decimal math_test_value(p64 bits)
{
        math_test_shape shape;
        shape.bits = bits;
        return shape.value;
}

static p64 math_test_bits(decimal value)
{
        math_test_shape shape;
        shape.value = value;
        return shape.bits;
}

/*
        The distance between two doubles counted in last places.

        Two's complement on the bit pattern does not order negative doubles,
        which are stored sign-and-magnitude, so the negatives are reflected
        about the sign bit first. After that the patterns are in increasing
        numeric order and their difference is the number of representable
        values between them, which is what an ulp count means. Two NaNs are
        the same answer whatever their payloads; a NaN against a number is as
        far away as it is possible to be.
*/
static positive math_test_distance(decimal mine, decimal reference)
{
        p64 left = math_test_bits(mine);
        p64 right = math_test_bits(reference);

        if (decimal_is_nan(mine) && decimal_is_nan(reference))
                return 0;
        if (decimal_is_nan(mine) || decimal_is_nan(reference))
                return ~(p64)0;

        if (left >> 63)
                left = 0x8000000000000000ULL - (left & 0x7fffffffffffffffULL);
        if (right >> 63)
                right = 0x8000000000000000ULL - (right & 0x7fffffffffffffffULL);

        return left > right ? left - right : right - left;
}

static positive math_test_failures = 0;

static bool math_test_one_table(math_test_one routine, const math_reference address_to table,
                                positive count, positive allowance, string_address name)
{
        positive index;
        bool clean = true;

        for (index = 0; index < count; index++)
        {
                decimal argument = math_test_value(table[index].argument);
                decimal expected = math_test_value(table[index].answer);
                decimal answer = routine(argument);
                positive distance = math_test_distance(answer, expected);

                if (distance > allowance)
                {
                        string_format(log_direct,
                                      "\n  [%s] argument %p answered %p, wanted %p, %p ulp out\n",
                                      name, table[index].argument, math_test_bits(answer),
                                      table[index].answer, distance);
                        math_test_failures++;
                        clean = false;
                }
        }
        return clean;
}

static bool math_test_two_table(math_test_two routine, const math_reference address_to table,
                                positive count, positive allowance, string_address name)
{
        positive index;
        bool clean = true;

        for (index = 0; index < count; index++)
        {
                decimal first = math_test_value(table[index].argument);
                decimal second = math_test_value(table[index].second);
                decimal expected = math_test_value(table[index].answer);
                decimal answer = routine(first, second);
                positive distance = math_test_distance(answer, expected);

                if (distance > allowance)
                {
                        string_format(log_direct,
                                      "\n  [%s] arguments %p %p answered %p, wanted %p, %p ulp out\n",
                                      name, table[index].argument, table[index].second,
                                      math_test_bits(answer), table[index].answer, distance);
                        math_test_failures++;
                        clean = false;
                }
        }
        return clean;
}

#define math_table_size(table) (sizeof(table) / sizeof(math_reference))

static const math_reference math_reference_sine[] = {
        {0x00000000000007e8ULL, 0ULL, 0x00000000000007e8ULL},  // sin(9.9998886718268301e-321) = 9.9998886718268301e-321
        {0x3e45798ee2308c3aULL, 0ULL, 0x3e45798ee2308c3aULL},  // sin(1e-08) = 1e-08
        {0x3fb999999999999aULL, 0ULL, 0x3fb98eaecb8bcb2cULL},  // sin(0.10000000000000001) = 0.099833416646828155
        {0x3fe0000000000000ULL, 0ULL, 0x3fdeaee8744b05f0ULL},  // sin(0.5) = 0.47942553860420301
        {0x3ff0000000000000ULL, 0ULL, 0x3feaed548f090ceeULL},  // sin(1) = 0.8414709848078965
        {0x3ff8000000000000ULL, 0ULL, 0x3fefeb7a9b2c6d8bULL},  // sin(1.5) = 0.99749498660405445
        {0x4000000000000000ULL, 0ULL, 0x3fed18f6ead1b446ULL},  // sin(2) = 0.90929742682568171
        {0x4008000000000000ULL, 0ULL, 0x3fc210386db6d55bULL},  // sin(3) = 0.14112000805986721
        {0x4024000000000000ULL, 0ULL, 0xbfe1689ef5f34f52ULL},  // sin(10) = -0.54402111088936977
        {0x4059000000000000ULL, 0ULL, 0xbfe03425b78c4db8ULL},  // sin(100) = -0.50636564110975879
        {0x412e848000000000ULL, 0ULL, 0xbfd6664b2568d867ULL},  // sin(1000000) = -0.34999350217129294
        {0x430c6bf526340000ULL, 0ULL, 0x3feb76f88136cebaULL},  // sin(1000000000000000) = 0.85827279317023586
        {0x4480f0cf064dd592ULL, 0ULL, 0xbfeb453ab76bf397ULL},  // sin(1e+22) = -0.85220084976718879
        {0x54b249ad2594c37dULL, 0ULL, 0xbfd85c5e5b929359ULL},  // sin(1e+100) = -0.38063773100502868
        {0x7e37e43c8800759cULL, 0ULL, 0xbfea2c16b010e385ULL},  // sin(1.0000000000000001e+300) = -0.81788191211590855
};

test(sine)
{
        return math_test_one_table(sine, math_reference_sine,
                                   math_table_size(math_reference_sine), 2, (string_address) "sine");
}

static const math_reference math_reference_cosine[] = {
        {0x00000000000007e8ULL, 0ULL, 0x3ff0000000000000ULL},  // cos(9.9998886718268301e-321) = 1
        {0x3e45798ee2308c3aULL, 0ULL, 0x3ff0000000000000ULL},  // cos(1e-08) = 1
        {0x3fb999999999999aULL, 0ULL, 0x3fefd712f9a817c1ULL},  // cos(0.10000000000000001) = 0.99500416527802582
        {0x3fe0000000000000ULL, 0ULL, 0x3fec1528065b7d50ULL},  // cos(0.5) = 0.87758256189037276
        {0x3ff0000000000000ULL, 0ULL, 0x3fe14a280fb5068cULL},  // cos(1) = 0.54030230586813977
        {0x3ff8000000000000ULL, 0ULL, 0x3fb21bd54fc5f9a7ULL},  // cos(1.5) = 0.070737201667702906
        {0x4000000000000000ULL, 0ULL, 0xbfdaa22657537205ULL},  // cos(2) = -0.41614683654714241
        {0x4008000000000000ULL, 0ULL, 0xbfefae04be85e5d2ULL},  // cos(3) = -0.98999249660044542
        {0x4024000000000000ULL, 0ULL, 0xbfead9ac890c6b1fULL},  // cos(10) = -0.83907152907645244
        {0x4059000000000000ULL, 0ULL, 0x3feb981dbf665fdfULL},  // cos(100) = 0.86231887228768389
        {0x412e848000000000ULL, 0ULL, 0x3fedf9df9906d32cULL},  // cos(1000000) = 0.93675212753314474
        {0x430c6bf526340000ULL, 0ULL, 0xbfe06c154609d33fULL},  // cos(1000000000000000) = -0.51319373778697031
        {0x4480f0cf064dd592ULL, 0ULL, 0x3fe0be2cef01c8f4ULL},  // cos(1e+22) = 0.52321478539513899
        {0x54b249ad2594c37dULL, 0ULL, 0x3fed9757496841f5ULL},  // cos(1e+100) = 0.92472423875193377
        {0x7e37e43c8800759cULL, 0ULL, 0xbfe2699022adc4c1ULL},  // cos(1.0000000000000001e+300) = -0.57538611195754907
};

test(cosine)
{
        return math_test_one_table(cosine, math_reference_cosine,
                                   math_table_size(math_reference_cosine), 2, (string_address) "cosine");
}

static const math_reference math_reference_tangent[] = {
        {0x00000000000007e8ULL, 0ULL, 0x00000000000007e8ULL},  // tan(9.9998886718268301e-321) = 9.9998886718268301e-321
        {0x3e45798ee2308c3aULL, 0ULL, 0x3e45798ee2308c3aULL},  // tan(1e-08) = 1e-08
        {0x3fb999999999999aULL, 0ULL, 0x3fb9af8877430b80ULL},  // tan(0.10000000000000001) = 0.10033467208545055
        {0x3fe0000000000000ULL, 0ULL, 0x3fe17b4f5bf3474aULL},  // tan(0.5) = 0.54630248984379048
        {0x3ff0000000000000ULL, 0ULL, 0x3ff8eb245cbee3a6ULL},  // tan(1) = 1.5574077246549023
        {0x3ff8000000000000ULL, 0ULL, 0x402c33ed50b88777ULL},  // tan(1.5) = 14.101419947171719
        {0x4000000000000000ULL, 0ULL, 0xc0017af62e0950f8ULL},  // tan(2) = -2.1850398632615189
        {0x4008000000000000ULL, 0ULL, 0xbfc23ef71254b86fULL},  // tan(3) = -0.1425465430742778
        {0x4024000000000000ULL, 0ULL, 0x3fe4bf5f34be3782ULL},  // tan(10) = 0.64836082745908663
        {0x4059000000000000ULL, 0ULL, 0xbfe2ca74d62b5d38ULL},  // tan(100) = -0.58721391515692911
        {0x412e848000000000ULL, 0ULL, 0xbfd7e9768ab734c0ULL},  // tan(1000000) = -0.37362445398759903
        {0x430c6bf526340000ULL, 0ULL, 0xbffac23600a95be4ULL},  // tan(1000000000000000) = -1.672414782127583
        {0x4480f0cf064dd592ULL, 0ULL, 0xbffa0f79c1b6b257ULL},  // tan(1e+22) = -1.6287782256068988
        {0x54b249ad2594c37dULL, 0ULL, 0xbfda5807d6f76f7dULL},  // tan(1e+100) = -0.41162296288324979
        {0x7e37e43c8800759cULL, 0ULL, 0x3ff6be411f37ac77ULL},  // tan(1.0000000000000001e+300) = 1.4214488238747245
};

test(tangent)
{
        return math_test_one_table(tangent, math_reference_tangent,
                                   math_table_size(math_reference_tangent), 2, (string_address) "tangent");
}

static const math_reference math_reference_exponential[] = {
        {0xc087480000000000ULL, 0ULL, 0x0000000000000001ULL},  // exp(-745) = 4.9406564584124654e-324
        {0xc059000000000000ULL, 0ULL, 0x36ea8c1f14e2af5dULL},  // exp(-100) = 3.7200759760208361e-44
        {0xbff0000000000000ULL, 0ULL, 0x3fd78b56362cef38ULL},  // exp(-1) = 0.36787944117144233
        {0xbfd0000000000000ULL, 0ULL, 0x3fe8ebef9eac820bULL},  // exp(-0.25) = 0.77880078307140488
        {0x3bc79ca10c924223ULL, 0ULL, 0x3ff0000000000000ULL},  // exp(9.9999999999999995e-21) = 1
        {0x3fd0000000000000ULL, 0ULL, 0x3ff48b5e3c3e8186ULL},  // exp(0.25) = 1.2840254166877414
        {0x3ff0000000000000ULL, 0ULL, 0x4005bf0a8b145769ULL},  // exp(1) = 2.7182818284590451
        {0x4004000000000000ULL, 0ULL, 0x40285d6fd931e0bbULL},  // exp(2.5) = 12.182493960703473
        {0x4059000000000000ULL, 0ULL, 0x48f3494a9b171bf5ULL},  // exp(100) = 2.6881171418161356e+43
        {0x407f400000000000ULL, 0ULL, 0x6d045ba2a9f7e439ULL},  // exp(500) = 1.4035922178528375e+217
        {0x4086280000000000ULL, 0ULL, 0x7fdd422d2be5dc9bULL},  // exp(709) = 8.2184074615549724e+307
};

test(exponential)
{
        return math_test_one_table(exponential, math_reference_exponential,
                                   math_table_size(math_reference_exponential), 2, (string_address) "exponential");
}

static const math_reference math_reference_exponential_two[] = {
        {0xc087480000000000ULL, 0ULL, 0x1160000000000000ULL},  // exp2(-745) = 5.4032272097832669e-225
        {0xc059000000000000ULL, 0ULL, 0x39b0000000000000ULL},  // exp2(-100) = 7.8886090522101181e-31
        {0xbff0000000000000ULL, 0ULL, 0x3fe0000000000000ULL},  // exp2(-1) = 0.5
        {0xbfd0000000000000ULL, 0ULL, 0x3feae89f995ad3adULL},  // exp2(-0.25) = 0.8408964152537145
        {0x3bc79ca10c924223ULL, 0ULL, 0x3ff0000000000000ULL},  // exp2(9.9999999999999995e-21) = 1
        {0x3fd0000000000000ULL, 0ULL, 0x3ff306fe0a31b715ULL},  // exp2(0.25) = 1.189207115002721
        {0x3ff0000000000000ULL, 0ULL, 0x4000000000000000ULL},  // exp2(1) = 2
        {0x4004000000000000ULL, 0ULL, 0x4016a09e667f3bcdULL},  // exp2(2.5) = 5.6568542494923806
        {0x4059000000000000ULL, 0ULL, 0x4630000000000000ULL},  // exp2(100) = 1.2676506002282294e+30
        {0x407f400000000000ULL, 0ULL, 0x5f30000000000000ULL},  // exp2(500) = 3.2733906078961419e+150
        {0x4086280000000000ULL, 0ULL, 0x6c40000000000000ULL},  // exp2(709) = 2.6931895815927672e+213
};

test(exponential_two)
{
        return math_test_one_table(exponential_two, math_reference_exponential_two,
                                   math_table_size(math_reference_exponential_two), 2, (string_address) "exponential_two");
}

static const math_reference math_reference_exponential_minus_one[] = {
        {0xc087480000000000ULL, 0ULL, 0xbff0000000000000ULL},  // expm1(-745) = -1
        {0xc059000000000000ULL, 0ULL, 0xbff0000000000000ULL},  // expm1(-100) = -1
        {0xbff0000000000000ULL, 0ULL, 0xbfe43a54e4e98864ULL},  // expm1(-1) = -0.63212055882855767
        {0xbfd0000000000000ULL, 0ULL, 0xbfcc5041854df7d4ULL},  // expm1(-0.25) = -0.22119921692859512
        {0x3bc79ca10c924223ULL, 0ULL, 0x3bc79ca10c924223ULL},  // expm1(9.9999999999999995e-21) = 9.9999999999999995e-21
        {0x3fd0000000000000ULL, 0ULL, 0x3fd22d78f0fa061aULL},  // expm1(0.25) = 0.28402541668774151
        {0x3ff0000000000000ULL, 0ULL, 0x3ffb7e151628aed3ULL},  // expm1(1) = 1.7182818284590453
        {0x4004000000000000ULL, 0ULL, 0x40265d6fd931e0bbULL},  // expm1(2.5) = 11.182493960703473
        {0x4059000000000000ULL, 0ULL, 0x48f3494a9b171bf5ULL},  // expm1(100) = 2.6881171418161356e+43
        {0x407f400000000000ULL, 0ULL, 0x6d045ba2a9f7e439ULL},  // expm1(500) = 1.4035922178528375e+217
        {0x4086280000000000ULL, 0ULL, 0x7fdd422d2be5dc9bULL},  // expm1(709) = 8.2184074615549724e+307
};

test(exponential_minus_one)
{
        return math_test_one_table(exponential_minus_one, math_reference_exponential_minus_one,
                                   math_table_size(math_reference_exponential_minus_one), 2, (string_address) "exponential_minus_one");
}

static const math_reference math_reference_logarithm[] = {
        {0x0000000000000001ULL, 0ULL, 0xc0874385446d71c3ULL},  // log(4.9406564584124654e-324) = -744.44007192138122
        {0x01a56e1fc2f8f359ULL, 0ULL, 0xc085963447f87fb5ULL},  // log(1e-300) = -690.77552789821368
        {0x3fe0000000000000ULL, 0ULL, 0xbfe62e42fefa39efULL},  // log(0.5) = -0.69314718055994529
        {0x3fefffffca501acbULL, 0ULL, 0xbe7ad7f2b1049b9fULL},  // log(0.99999990000000005) = -1.0000000494736474e-07
        {0x3ff0000000000000ULL, 0ULL, 0x0000000000000000ULL},  // log(1) = 0
        {0x3ff000001ad7f29bULL, 0ULL, 0x3e7ad7f2847b6492ULL},  // log(1.0000001000000001) = 9.9999995058387044e-08
        {0x3ff8000000000000ULL, 0ULL, 0x3fd9f323ecbf984cULL},  // log(1.5) = 0.40546510810816438
        {0x4000000000000000ULL, 0ULL, 0x3fe62e42fefa39efULL},  // log(2) = 0.69314718055994529
        {0x4024000000000000ULL, 0ULL, 0x40026bb1bbb55516ULL},  // log(10) = 2.3025850929940459
        {0x4376345785d8a000ULL, 0ULL, 0x4043926cd770aa67ULL},  // log(1e+17) = 39.143946580898778
        {0x7fefffffffffffffULL, 0ULL, 0x40862e42fefa39efULL},  // log(1.7976931348623157e+308) = 709.78271289338397
};

test(logarithm)
{
        return math_test_one_table(logarithm, math_reference_logarithm,
                                   math_table_size(math_reference_logarithm), 2, (string_address) "logarithm");
}

static const math_reference math_reference_logarithm_two[] = {
        {0x0000000000000001ULL, 0ULL, 0xc090c80000000000ULL},  // log2(4.9406564584124654e-324) = -1074
        {0x01a56e1fc2f8f359ULL, 0ULL, 0xc08f24a09f1a8b89ULL},  // log2(1e-300) = -996.57842846620872
        {0x3fe0000000000000ULL, 0ULL, 0xbff0000000000000ULL},  // log2(0.5) = -1
        {0x3fefffffca501acbULL, 0ULL, 0xbe835d100a8009b2ULL},  // log2(0.99999990000000005) = -1.4426951122643492e-07
        {0x3ff0000000000000ULL, 0ULL, 0x0000000000000000ULL},  // log2(1) = 0
        {0x3ff000001ad7f29bULL, 0ULL, 0x3e835d0fea5fccb7ULL},  // log2(1.0000001000000001) = 1.4426949695965583e-07
        {0x3ff8000000000000ULL, 0ULL, 0x3fe2b803473f7ad1ULL},  // log2(1.5) = 0.58496250072115619
        {0x4000000000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // log2(2) = 1
        {0x4024000000000000ULL, 0ULL, 0x400a934f0979a371ULL},  // log2(10) = 3.3219280948873622
        {0x4376345785d8a000ULL, 0ULL, 0x404c3c83fa113da8ULL},  // log2(1e+17) = 56.472777613085157
        {0x7fefffffffffffffULL, 0ULL, 0x4090000000000000ULL},  // log2(1.7976931348623157e+308) = 1024
};

test(logarithm_two)
{
        return math_test_one_table(logarithm_two, math_reference_logarithm_two,
                                   math_table_size(math_reference_logarithm_two), 2, (string_address) "logarithm_two");
}

static const math_reference math_reference_logarithm_ten[] = {
        {0x0000000000000001ULL, 0ULL, 0xc07434e6420f4374ULL},  // log10(4.9406564584124654e-324) = -323.30621534311581
        {0x01a56e1fc2f8f359ULL, 0ULL, 0xc072c00000000000ULL},  // log10(1e-300) = -300
        {0x3fe0000000000000ULL, 0ULL, 0xbfd34413509f79ffULL},  // log10(0.5) = -0.3010299956639812
        {0x3fefffffca501acbULL, 0ULL, 0xbe6750e5f0ba08cdULL},  // log10(0.99999990000000005) = -4.342945033893839e-08
        {0x3ff0000000000000ULL, 0ULL, 0x0000000000000000ULL},  // log10(1) = 0
        {0x3ff000001ad7f29bULL, 0ULL, 0x3e6750e5ca0b1098ULL},  // log10(1.0000001000000001) = 4.3429446044209946e-08
        {0x3ff8000000000000ULL, 0ULL, 0x3fc68a288b60b7fcULL},  // log10(1.5) = 0.17609125905568124
        {0x4000000000000000ULL, 0ULL, 0x3fd34413509f79ffULL},  // log10(2) = 0.3010299956639812
        {0x4024000000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // log10(10) = 1
        {0x4376345785d8a000ULL, 0ULL, 0x4031000000000000ULL},  // log10(1e+17) = 17
        {0x7fefffffffffffffULL, 0ULL, 0x40734413509f79ffULL},  // log10(1.7976931348623157e+308) = 308.25471555991675
};

test(logarithm_ten)
{
        return math_test_one_table(logarithm_ten, math_reference_logarithm_ten,
                                   math_table_size(math_reference_logarithm_ten), 2, (string_address) "logarithm_ten");
}

static const math_reference math_reference_arc_sine[] = {
        {0xbff0000000000000ULL, 0ULL, 0xbff921fb54442d18ULL},  // asin(-1) = -1.5707963267948966
        {0xbfefff2e48e8a71eULL, 0ULL, 0xbff8e80e1a01556aULL},  // asin(-0.99990000000000001) = -1.5566540733173846
        {0xbfe8000000000000ULL, 0ULL, 0xbfeb235315c680dcULL},  // asin(-0.75) = -0.848062078981481
        {0xbfe0000000000000ULL, 0ULL, 0xbfe0c152382d7366ULL},  // asin(-0.5) = -0.52359877559829893
        {0xbfd0000000000000ULL, 0ULL, 0xbfd02be9ce0b87cdULL},  // asin(-0.25) = -0.25268025514207865
        {0x0000000000000000ULL, 0ULL, 0x0000000000000000ULL},  // asin(0) = 0
        {0x3fd0000000000000ULL, 0ULL, 0x3fd02be9ce0b87cdULL},  // asin(0.25) = 0.25268025514207865
        {0x3fe0000000000000ULL, 0ULL, 0x3fe0c152382d7366ULL},  // asin(0.5) = 0.52359877559829893
        {0x3fe8000000000000ULL, 0ULL, 0x3feb235315c680dcULL},  // asin(0.75) = 0.848062078981481
        {0x3fefff2e48e8a71eULL, 0ULL, 0x3ff8e80e1a01556aULL},  // asin(0.99990000000000001) = 1.5566540733173846
        {0x3ff0000000000000ULL, 0ULL, 0x3ff921fb54442d18ULL},  // asin(1) = 1.5707963267948966
};

test(arc_sine)
{
        return math_test_one_table(arc_sine, math_reference_arc_sine,
                                   math_table_size(math_reference_arc_sine), 2, (string_address) "arc_sine");
}

static const math_reference math_reference_arc_cosine[] = {
        {0xbff0000000000000ULL, 0ULL, 0x400921fb54442d18ULL},  // acos(-1) = 3.1415926535897931
        {0xbfefff2e48e8a71eULL, 0ULL, 0x40090504b722c141ULL},  // acos(-0.99990000000000001) = 3.1274504001122811
        {0xbfe8000000000000ULL, 0ULL, 0x400359d26f93b6c3ULL},  // acos(-0.75) = 2.4188584057763776
        {0xbfe0000000000000ULL, 0ULL, 0x4000c152382d7366ULL},  // acos(-0.5) = 2.0943951023931957
        {0xbfd0000000000000ULL, 0ULL, 0x3ffd2cf5c7c70f0cULL},  // acos(-0.25) = 1.8234765819369754
        {0x0000000000000000ULL, 0ULL, 0x3ff921fb54442d18ULL},  // acos(0) = 1.5707963267948966
        {0x3fd0000000000000ULL, 0ULL, 0x3ff51700e0c14b25ULL},  // acos(0.25) = 1.318116071652818
        {0x3fe0000000000000ULL, 0ULL, 0x3ff0c152382d7366ULL},  // acos(0.5) = 1.0471975511965979
        {0x3fe8000000000000ULL, 0ULL, 0x3fe720a392c1d955ULL},  // acos(0.75) = 0.72273424781341566
        {0x3fefff2e48e8a71eULL, 0ULL, 0x3f8cf69d216bd74bULL},  // acos(0.99990000000000001) = 0.014142253477512098
        {0x3ff0000000000000ULL, 0ULL, 0x0000000000000000ULL},  // acos(1) = 0
};

test(arc_cosine)
{
        return math_test_one_table(arc_cosine, math_reference_arc_cosine,
                                   math_table_size(math_reference_arc_cosine), 2, (string_address) "arc_cosine");
}

static const math_reference math_reference_arc_tangent[] = {
        {0x00000000000007e8ULL, 0ULL, 0x00000000000007e8ULL},  // atan(9.9998886718268301e-321) = 9.9998886718268301e-321
        {0x3e45798ee2308c3aULL, 0ULL, 0x3e45798ee2308c3aULL},  // atan(1e-08) = 1e-08
        {0x3fb999999999999aULL, 0ULL, 0x3fb983e282e2cc4dULL},  // atan(0.10000000000000001) = 0.099668652491162038
        {0x3fe0000000000000ULL, 0ULL, 0x3fddac670561bb4fULL},  // atan(0.5) = 0.46364760900080609
        {0x3ff0000000000000ULL, 0ULL, 0x3fe921fb54442d18ULL},  // atan(1) = 0.78539816339744828
        {0x3ff8000000000000ULL, 0ULL, 0x3fef730bd281f69bULL},  // atan(1.5) = 0.98279372324732905
        {0x4000000000000000ULL, 0ULL, 0x3ff1b6e192ebbe44ULL},  // atan(2) = 1.1071487177940904
        {0x4008000000000000ULL, 0ULL, 0x3ff3fc176b7a8560ULL},  // atan(3) = 1.2490457723982544
        {0x4024000000000000ULL, 0ULL, 0x3ff789bd2c160054ULL},  // atan(10) = 1.4711276743037347
        {0x4059000000000000ULL, 0ULL, 0x3ff8f905eb2def22ULL},  // atan(100) = 1.5607966601082315
        {0x412e848000000000ULL, 0ULL, 0x3ff921fa47d4b30dULL},  // atan(1000000) = 1.5707953267948966
        {0x430c6bf526340000ULL, 0ULL, 0x3ff921fb54442d14ULL},  // atan(1000000000000000) = 1.5707963267948957
        {0x4480f0cf064dd592ULL, 0ULL, 0x3ff921fb54442d18ULL},  // atan(1e+22) = 1.5707963267948966
        {0x54b249ad2594c37dULL, 0ULL, 0x3ff921fb54442d18ULL},  // atan(1e+100) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0ULL, 0x3ff921fb54442d18ULL},  // atan(1.0000000000000001e+300) = 1.5707963267948966
};

test(arc_tangent)
{
        return math_test_one_table(arc_tangent, math_reference_arc_tangent,
                                   math_table_size(math_reference_arc_tangent), 2, (string_address) "arc_tangent");
}

static const math_reference math_reference_hyperbolic_sine[] = {
        {0xc03e000000000000ULL, 0ULL, 0xc29370470aec28edULL},  // sinh(-30) = -5343237290762.2314
        {0xbff0000000000000ULL, 0ULL, 0xbff2cd9fc44eb982ULL},  // sinh(-1) = -1.1752011936438014
        {0xbfd0000000000000ULL, 0ULL, 0xbfd02accd9d08102ULL},  // sinh(-0.25) = -0.25261231680816831
        {0xbddb7cdfd9d7bdbbULL, 0ULL, 0xbddb7cdfd9d7bdbbULL},  // sinh(-1e-10) = -1e-10
        {0x0000000000000000ULL, 0ULL, 0x0000000000000000ULL},  // sinh(0) = 0
        {0x3ddb7cdfd9d7bdbbULL, 0ULL, 0x3ddb7cdfd9d7bdbbULL},  // sinh(1e-10) = 1e-10
        {0x3fd0000000000000ULL, 0ULL, 0x3fd02accd9d08102ULL},  // sinh(0.25) = 0.25261231680816831
        {0x3ff0000000000000ULL, 0ULL, 0x3ff2cd9fc44eb982ULL},  // sinh(1) = 1.1752011936438014
        {0x4036800000000000ULL, 0ULL, 0x41e604b68cf05f66ULL},  // sinh(22.5) = 2955261031.5116453
        {0x4085e00000000000ULL, 0ULL, 0x7efd945df4f8ec8eULL},  // sinh(700) = 5.0711602736750225e+303
        {0x4086300000000000ULL, 0ULL, 0x7fe3e21a464507f9ULL},  // sinh(710) = 1.1169973830808555e+308
};

test(hyperbolic_sine)
{
        return math_test_one_table(hyperbolic_sine, math_reference_hyperbolic_sine,
                                   math_table_size(math_reference_hyperbolic_sine), 2, (string_address) "hyperbolic_sine");
}

static const math_reference math_reference_hyperbolic_cosine[] = {
        {0xc03e000000000000ULL, 0ULL, 0x429370470aec28edULL},  // cosh(-30) = 5343237290762.2314
        {0xbff0000000000000ULL, 0ULL, 0x3ff8b07551d9f550ULL},  // cosh(-1) = 1.5430806348152437
        {0xbfd0000000000000ULL, 0ULL, 0x3ff080ab05ca6146ULL},  // cosh(-0.25) = 1.0314130998795732
        {0xbddb7cdfd9d7bdbbULL, 0ULL, 0x3ff0000000000000ULL},  // cosh(-1e-10) = 1
        {0x0000000000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // cosh(0) = 1
        {0x3ddb7cdfd9d7bdbbULL, 0ULL, 0x3ff0000000000000ULL},  // cosh(1e-10) = 1
        {0x3fd0000000000000ULL, 0ULL, 0x3ff080ab05ca6146ULL},  // cosh(0.25) = 1.0314130998795732
        {0x3ff0000000000000ULL, 0ULL, 0x3ff8b07551d9f550ULL},  // cosh(1) = 1.5430806348152437
        {0x4036800000000000ULL, 0ULL, 0x41e604b68cf05f66ULL},  // cosh(22.5) = 2955261031.5116453
        {0x4085e00000000000ULL, 0ULL, 0x7efd945df4f8ec8eULL},  // cosh(700) = 5.0711602736750225e+303
        {0x4086300000000000ULL, 0ULL, 0x7fe3e21a464507f9ULL},  // cosh(710) = 1.1169973830808555e+308
};

test(hyperbolic_cosine)
{
        return math_test_one_table(hyperbolic_cosine, math_reference_hyperbolic_cosine,
                                   math_table_size(math_reference_hyperbolic_cosine), 2, (string_address) "hyperbolic_cosine");
}

static const math_reference math_reference_hyperbolic_tangent[] = {
        {0xc03e000000000000ULL, 0ULL, 0xbff0000000000000ULL},  // tanh(-30) = -1
        {0xbff0000000000000ULL, 0ULL, 0xbfe85efab514f394ULL},  // tanh(-1) = -0.76159415595576485
        {0xbfd0000000000000ULL, 0ULL, 0xbfcf597ea69a1c86ULL},  // tanh(-0.25) = -0.24491866240370913
        {0xbddb7cdfd9d7bdbbULL, 0ULL, 0xbddb7cdfd9d7bdbbULL},  // tanh(-1e-10) = -1e-10
        {0x0000000000000000ULL, 0ULL, 0x0000000000000000ULL},  // tanh(0) = 0
        {0x3ddb7cdfd9d7bdbbULL, 0ULL, 0x3ddb7cdfd9d7bdbbULL},  // tanh(1e-10) = 1e-10
        {0x3fd0000000000000ULL, 0ULL, 0x3fcf597ea69a1c86ULL},  // tanh(0.25) = 0.24491866240370913
        {0x3ff0000000000000ULL, 0ULL, 0x3fe85efab514f394ULL},  // tanh(1) = 0.76159415595576485
        {0x4036800000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // tanh(22.5) = 1
        {0x4085e00000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // tanh(700) = 1
        {0x4086300000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // tanh(710) = 1
};

test(hyperbolic_tangent)
{
        return math_test_one_table(hyperbolic_tangent, math_reference_hyperbolic_tangent,
                                   math_table_size(math_reference_hyperbolic_tangent), 2, (string_address) "hyperbolic_tangent");
}

static const math_reference math_reference_cube_root[] = {
        {0x00000000000007e8ULL, 0ULL, 0x29c94c7f15ef7ddfULL},  // cbrt(9.9998886718268301e-321) = 2.1544266950262728e-107
        {0x80000000000007e8ULL, 0ULL, 0xa9c94c7f15ef7ddfULL},  // cbrt(-9.9998886718268301e-321) = -2.1544266950262728e-107
        {0x3e45798ee2308c3aULL, 0ULL, 0x3f61a62d511f2b53ULL},  // cbrt(1e-08) = 0.0021544346900318838
        {0xbe45798ee2308c3aULL, 0ULL, 0xbf61a62d511f2b53ULL},  // cbrt(-1e-08) = -0.0021544346900318838
        {0x3fb999999999999aULL, 0ULL, 0x3fddb4c7760bcff3ULL},  // cbrt(0.10000000000000001) = 0.46415888336127792
        {0xbfb999999999999aULL, 0ULL, 0xbfddb4c7760bcff3ULL},  // cbrt(-0.10000000000000001) = -0.46415888336127792
        {0x3fe0000000000000ULL, 0ULL, 0x3fe965fea53d6e3dULL},  // cbrt(0.5) = 0.79370052598409979
        {0xbfe0000000000000ULL, 0ULL, 0xbfe965fea53d6e3dULL},  // cbrt(-0.5) = -0.79370052598409979
        {0x3ff0000000000000ULL, 0ULL, 0x3ff0000000000000ULL},  // cbrt(1) = 1
        {0xbff0000000000000ULL, 0ULL, 0xbff0000000000000ULL},  // cbrt(-1) = -1
        {0x3ff8000000000000ULL, 0ULL, 0x3ff250bfe1b082f5ULL},  // cbrt(1.5) = 1.1447142425533319
        {0xbff8000000000000ULL, 0ULL, 0xbff250bfe1b082f5ULL},  // cbrt(-1.5) = -1.1447142425533319
        {0x4000000000000000ULL, 0ULL, 0x3ff428a2f98d728bULL},  // cbrt(2) = 1.2599210498948732
        {0xc000000000000000ULL, 0ULL, 0xbff428a2f98d728bULL},  // cbrt(-2) = -1.2599210498948732
        {0x4008000000000000ULL, 0ULL, 0x3ff7137449123ef6ULL},  // cbrt(3) = 1.4422495703074083
        {0xc008000000000000ULL, 0ULL, 0xbff7137449123ef6ULL},  // cbrt(-3) = -1.4422495703074083
        {0x4024000000000000ULL, 0ULL, 0x40013c484138704fULL},  // cbrt(10) = 2.1544346900318838
        {0xc024000000000000ULL, 0ULL, 0xc0013c484138704fULL},  // cbrt(-10) = -2.1544346900318838
        {0x4059000000000000ULL, 0ULL, 0x401290fca9c761f8ULL},  // cbrt(100) = 4.6415888336127793
        {0xc059000000000000ULL, 0ULL, 0xc01290fca9c761f8ULL},  // cbrt(-100) = -4.6415888336127793
        {0x412e848000000000ULL, 0ULL, 0x4059000000000000ULL},  // cbrt(1000000) = 100
        {0xc12e848000000000ULL, 0ULL, 0xc059000000000000ULL},  // cbrt(-1000000) = -100
        {0x430c6bf526340000ULL, 0ULL, 0x40f86a0000000000ULL},  // cbrt(1000000000000000) = 100000
        {0xc30c6bf526340000ULL, 0ULL, 0xc0f86a0000000000ULL},  // cbrt(-1000000000000000) = -100000
        {0x4480f0cf064dd592ULL, 0ULL, 0x41748bd9ae67b4baULL},  // cbrt(1e+22) = 21544346.900318839
        {0xc480f0cf064dd592ULL, 0ULL, 0xc1748bd9ae67b4baULL},  // cbrt(-1e+22) = -21544346.900318839
        {0x54b249ad2594c37dULL, 0ULL, 0x46da8e327ba6c9c9ULL},  // cbrt(1e+100) = 2.1544346900318838e+33
        {0xd4b249ad2594c37dULL, 0ULL, 0xc6da8e327ba6c9c9ULL},  // cbrt(-1e+100) = -2.1544346900318838e+33
        {0x7e37e43c8800759cULL, 0ULL, 0x54b249ad2594c37dULL},  // cbrt(1.0000000000000001e+300) = 1e+100
        {0xfe37e43c8800759cULL, 0ULL, 0xd4b249ad2594c37dULL},  // cbrt(-1.0000000000000001e+300) = -1e+100
};

test(cube_root)
{
        return math_test_one_table(cube_root, math_reference_cube_root,
                                   math_table_size(math_reference_cube_root), 2, (string_address) "cube_root");
}

static const math_reference math_reference_power[] = {
        {0x3fe0000000000000ULL, 0xc08f440000000000ULL, 0x7e76a09e667f3bcdULL},  // pow(0.5, -1000.5) = 1.5153420044823246e+301
        {0x3fe0000000000000ULL, 0xc008000000000000ULL, 0x4020000000000000ULL},  // pow(0.5, -3) = 8
        {0x3fe0000000000000ULL, 0x3fe0000000000000ULL, 0x3fe6a09e667f3bcdULL},  // pow(0.5, 0.5) = 0.70710678118654757
        {0x3fe0000000000000ULL, 0x4004000000000000ULL, 0x3fc6a09e667f3bcdULL},  // pow(0.5, 2.5) = 0.17677669529663689
        {0x3fe0000000000000ULL, 0x4008000000000000ULL, 0x3fc0000000000000ULL},  // pow(0.5, 3) = 0.125
        {0x3fe0000000000000ULL, 0x408f440000000000ULL, 0x0166a09e667f3bcdULL},  // pow(0.5, 1000.5) = 6.599170332783212e-302
        {0x3fe0000000000000ULL, 0x408fff3333333333ULL, 0x0004497efb8941acULL},  // pow(0.5, 1023.9) = 5.9619377843282269e-309
        {0x3ff8000000000000ULL, 0xc090c80000000000ULL, 0x18aae9eed50a1cf5ULL},  // pow(1.5, -1074) = 7.5507097140210907e-190
        {0x3ff8000000000000ULL, 0xc08f440000000000ULL, 0x1b5ad0e05b483001ULL},  // pow(1.5, -1000.5) = 6.6175207962444435e-177
        {0x3ff8000000000000ULL, 0xc008000000000000ULL, 0x3fd2f684bda12f68ULL},  // pow(1.5, -3) = 0.29629629629629628
        {0x3ff8000000000000ULL, 0x3fe0000000000000ULL, 0x3ff3988e1409212eULL},  // pow(1.5, 0.5) = 1.2247448713915889
        {0x3ff8000000000000ULL, 0x4004000000000000ULL, 0x40060b9fd68a4554ULL},  // pow(1.5, 2.5) = 2.7556759606310752
        {0x3ff8000000000000ULL, 0x4008000000000000ULL, 0x400b000000000000ULL},  // pow(1.5, 3) = 3.375
        {0x3ff8000000000000ULL, 0x408f440000000000ULL, 0x648317d7955a28ccULL},  // pow(1.5, 1000.5) = 1.5111399431755729e+176
        {0x3ff8000000000000ULL, 0x408fff3333333333ULL, 0x655ec33849aada42ULL},  // pow(1.5, 1023.9) = 1.9945280441794488e+180
        {0x4000000000000000ULL, 0xc090c80000000000ULL, 0x0000000000000001ULL},  // pow(2, -1074) = 4.9406564584124654e-324
        {0x4000000000000000ULL, 0xc08f440000000000ULL, 0x0166a09e667f3bcdULL},  // pow(2, -1000.5) = 6.599170332783212e-302
        {0x4000000000000000ULL, 0xc008000000000000ULL, 0x3fc0000000000000ULL},  // pow(2, -3) = 0.125
        {0x4000000000000000ULL, 0x3fe0000000000000ULL, 0x3ff6a09e667f3bcdULL},  // pow(2, 0.5) = 1.4142135623730951
        {0x4000000000000000ULL, 0x4004000000000000ULL, 0x4016a09e667f3bcdULL},  // pow(2, 2.5) = 5.6568542494923806
        {0x4000000000000000ULL, 0x4008000000000000ULL, 0x4020000000000000ULL},  // pow(2, 3) = 8
        {0x4000000000000000ULL, 0x408f440000000000ULL, 0x7e76a09e667f3bcdULL},  // pow(2, 1000.5) = 1.5153420044823246e+301
        {0x4000000000000000ULL, 0x408fff3333333333ULL, 0x7feddb680117aa8eULL},  // pow(2, 1023.9) = 1.6773070034857416e+308
        {0x4008000000000000ULL, 0xc008000000000000ULL, 0x3fa2f684bda12f68ULL},  // pow(3, -3) = 0.037037037037037035
        {0x4008000000000000ULL, 0x3fe0000000000000ULL, 0x3ffbb67ae8584caaULL},  // pow(3, 0.5) = 1.7320508075688772
        {0x4008000000000000ULL, 0x4004000000000000ULL, 0x402f2d4a45635640ULL},  // pow(3, 2.5) = 15.588457268119896
        {0x4008000000000000ULL, 0x4008000000000000ULL, 0x403b000000000000ULL},  // pow(3, 3) = 27
        {0x4024000000000000ULL, 0xc008000000000000ULL, 0x3f50624dd2f1a9fcULL},  // pow(10, -3) = 0.001
        {0x4024000000000000ULL, 0x3fe0000000000000ULL, 0x40094c583ada5b53ULL},  // pow(10, 0.5) = 3.1622776601683795
        {0x4024000000000000ULL, 0x4004000000000000ULL, 0x4073c3a4edfa9759ULL},  // pow(10, 2.5) = 316.22776601683796
        {0x4024000000000000ULL, 0x4008000000000000ULL, 0x408f400000000000ULL},  // pow(10, 3) = 1000
        {0x54b249ad2594c37dULL, 0xc008000000000000ULL, 0x01a56e1fc2f8f359ULL},  // pow(1e+100, -3) = 1e-300
        {0x54b249ad2594c37dULL, 0x3fe0000000000000ULL, 0x4a511b0ec57e649aULL},  // pow(1e+100, 0.5) = 1.0000000000000001e+50
        {0x54b249ad2594c37dULL, 0x4004000000000000ULL, 0x73d658e3ab795205ULL},  // pow(1e+100, 2.5) = 1.0000000000000001e+250
        {0x54b249ad2594c37dULL, 0x4008000000000000ULL, 0x7e37e43c8800759cULL},  // pow(1e+100, 3) = 1.0000000000000001e+300
        {0x2b2bff2ee48e0530ULL, 0xc008000000000000ULL, 0x7e37e43c8800759bULL},  // pow(1e-100, -3) = 9.999999999999999e+299
        {0x2b2bff2ee48e0530ULL, 0x3fe0000000000000ULL, 0x358dee7a4ad4b81fULL},  // pow(1e-100, 0.5) = 1e-50
        {0x2b2bff2ee48e0530ULL, 0x4004000000000000ULL, 0x0c06e93f5da2824cULL},  // pow(1e-100, 2.5) = 1.0000000000000001e-250
        {0x2b2bff2ee48e0530ULL, 0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL},  // pow(1e-100, 3) = 1e-300
        {0x3fe60755b7093d7fULL, 0xc090c80000000000ULL, 0x64177cd14bb1cb2eULL},  // pow(0.68839536427724102, -1074) = 1.4522974427833062e+174
        {0x3fe60755b7093d7fULL, 0xc08f440000000000ULL, 0x619f204f65179d43ULL},  // pow(0.68839536427724102, -1000.5) = 1.7504267769227017e+162
        {0x3fe60755b7093d7fULL, 0xc008000000000000ULL, 0x400885ecdddcf92eULL},  // pow(0.68839536427724102, -3) = 3.0653931935368268
        {0x3fe60755b7093d7fULL, 0x3fe0000000000000ULL, 0x3fea8cde83afd254ULL},  // pow(0.68839536427724102, 0.5) = 0.82969594688490611
        {0x3fe60755b7093d7fULL, 0x4004000000000000ULL, 0x3fd929e9732bdec1ULL},  // pow(0.68839536427724102, 2.5) = 0.39318310019687536
        {0x3fe60755b7093d7fULL, 0x4008000000000000ULL, 0x3fd4e0d4053cea36ULL},  // pow(0.68839536427724102, 3) = 0.32622242461698947
        {0x3fe60755b7093d7fULL, 0x408f440000000000ULL, 0x1e4072fc14bbbff3ULL},  // pow(0.68839536427724102, 1000.5) = 5.7128924967545768e-163
        {0x3fe60755b7093d7fULL, 0x408fff3333333333ULL, 0x1d759fcfe03d7d95ULL},  // pow(0.68839536427724102, 1023.9) = 9.1677714829885389e-167
};

test(power)
{
        return math_test_two_table(power, math_reference_power,
                                   math_table_size(math_reference_power), 2, (string_address) "power");
}

static const math_reference math_reference_arc_tangent_two[] = {
        {0xfe37e43c8800759cULL, 0xfe37e43c8800759cULL, 0xc002d97c7f3321d2ULL},  // atan2(-1.0000000000000001e+300, -1.0000000000000001e+300) = -2.3561944901923448
        {0xfe37e43c8800759cULL, 0xbff0000000000000ULL, 0xbff921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, -1) = -1.5707963267948966
        {0xfe37e43c8800759cULL, 0x81a56e1fc2f8f359ULL, 0xbff921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, -1e-300) = -1.5707963267948966
        {0xfe37e43c8800759cULL, 0x0000000000000000ULL, 0xbff921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, 0) = -1.5707963267948966
        {0xfe37e43c8800759cULL, 0x01a56e1fc2f8f359ULL, 0xbff921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, 1e-300) = -1.5707963267948966
        {0xfe37e43c8800759cULL, 0x3ff0000000000000ULL, 0xbff921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, 1) = -1.5707963267948966
        {0xfe37e43c8800759cULL, 0x7e37e43c8800759cULL, 0xbfe921fb54442d18ULL},  // atan2(-1.0000000000000001e+300, 1.0000000000000001e+300) = -0.78539816339744828
        {0xbff0000000000000ULL, 0xfe37e43c8800759cULL, 0xc00921fb54442d18ULL},  // atan2(-1, -1.0000000000000001e+300) = -3.1415926535897931
        {0xbff0000000000000ULL, 0xbff0000000000000ULL, 0xc002d97c7f3321d2ULL},  // atan2(-1, -1) = -2.3561944901923448
        {0xbff0000000000000ULL, 0x81a56e1fc2f8f359ULL, 0xbff921fb54442d18ULL},  // atan2(-1, -1e-300) = -1.5707963267948966
        {0xbff0000000000000ULL, 0x0000000000000000ULL, 0xbff921fb54442d18ULL},  // atan2(-1, 0) = -1.5707963267948966
        {0xbff0000000000000ULL, 0x01a56e1fc2f8f359ULL, 0xbff921fb54442d18ULL},  // atan2(-1, 1e-300) = -1.5707963267948966
        {0xbff0000000000000ULL, 0x3ff0000000000000ULL, 0xbfe921fb54442d18ULL},  // atan2(-1, 1) = -0.78539816339744828
        {0xbff0000000000000ULL, 0x7e37e43c8800759cULL, 0x81a56e1fc2f8f359ULL},  // atan2(-1, 1.0000000000000001e+300) = -1e-300
        {0x81a56e1fc2f8f359ULL, 0xfe37e43c8800759cULL, 0xc00921fb54442d18ULL},  // atan2(-1e-300, -1.0000000000000001e+300) = -3.1415926535897931
        {0x81a56e1fc2f8f359ULL, 0xbff0000000000000ULL, 0xc00921fb54442d18ULL},  // atan2(-1e-300, -1) = -3.1415926535897931
        {0x81a56e1fc2f8f359ULL, 0x81a56e1fc2f8f359ULL, 0xc002d97c7f3321d2ULL},  // atan2(-1e-300, -1e-300) = -2.3561944901923448
        {0x81a56e1fc2f8f359ULL, 0x0000000000000000ULL, 0xbff921fb54442d18ULL},  // atan2(-1e-300, 0) = -1.5707963267948966
        {0x81a56e1fc2f8f359ULL, 0x01a56e1fc2f8f359ULL, 0xbfe921fb54442d18ULL},  // atan2(-1e-300, 1e-300) = -0.78539816339744828
        {0x81a56e1fc2f8f359ULL, 0x3ff0000000000000ULL, 0x81a56e1fc2f8f359ULL},  // atan2(-1e-300, 1) = -1e-300
        {0x81a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL, 0x8000000000000000ULL},  // atan2(-1e-300, 1.0000000000000001e+300) = -0
        {0x0000000000000000ULL, 0xfe37e43c8800759cULL, 0x400921fb54442d18ULL},  // atan2(0, -1.0000000000000001e+300) = 3.1415926535897931
        {0x0000000000000000ULL, 0xbff0000000000000ULL, 0x400921fb54442d18ULL},  // atan2(0, -1) = 3.1415926535897931
        {0x0000000000000000ULL, 0x81a56e1fc2f8f359ULL, 0x400921fb54442d18ULL},  // atan2(0, -1e-300) = 3.1415926535897931
        {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL},  // atan2(0, 0) = 0
        {0x0000000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x0000000000000000ULL},  // atan2(0, 1e-300) = 0
        {0x0000000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // atan2(0, 1) = 0
        {0x0000000000000000ULL, 0x7e37e43c8800759cULL, 0x0000000000000000ULL},  // atan2(0, 1.0000000000000001e+300) = 0
        {0x01a56e1fc2f8f359ULL, 0xfe37e43c8800759cULL, 0x400921fb54442d18ULL},  // atan2(1e-300, -1.0000000000000001e+300) = 3.1415926535897931
        {0x01a56e1fc2f8f359ULL, 0xbff0000000000000ULL, 0x400921fb54442d18ULL},  // atan2(1e-300, -1) = 3.1415926535897931
        {0x01a56e1fc2f8f359ULL, 0x81a56e1fc2f8f359ULL, 0x4002d97c7f3321d2ULL},  // atan2(1e-300, -1e-300) = 2.3561944901923448
        {0x01a56e1fc2f8f359ULL, 0x0000000000000000ULL, 0x3ff921fb54442d18ULL},  // atan2(1e-300, 0) = 1.5707963267948966
        {0x01a56e1fc2f8f359ULL, 0x01a56e1fc2f8f359ULL, 0x3fe921fb54442d18ULL},  // atan2(1e-300, 1e-300) = 0.78539816339744828
        {0x01a56e1fc2f8f359ULL, 0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL},  // atan2(1e-300, 1) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL, 0x0000000000000000ULL},  // atan2(1e-300, 1.0000000000000001e+300) = 0
        {0x3ff0000000000000ULL, 0xfe37e43c8800759cULL, 0x400921fb54442d18ULL},  // atan2(1, -1.0000000000000001e+300) = 3.1415926535897931
        {0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x4002d97c7f3321d2ULL},  // atan2(1, -1) = 2.3561944901923448
        {0x3ff0000000000000ULL, 0x81a56e1fc2f8f359ULL, 0x3ff921fb54442d18ULL},  // atan2(1, -1e-300) = 1.5707963267948966
        {0x3ff0000000000000ULL, 0x0000000000000000ULL, 0x3ff921fb54442d18ULL},  // atan2(1, 0) = 1.5707963267948966
        {0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x3ff921fb54442d18ULL},  // atan2(1, 1e-300) = 1.5707963267948966
        {0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x3fe921fb54442d18ULL},  // atan2(1, 1) = 0.78539816339744828
        {0x3ff0000000000000ULL, 0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL},  // atan2(1, 1.0000000000000001e+300) = 1e-300
        {0x7e37e43c8800759cULL, 0xfe37e43c8800759cULL, 0x4002d97c7f3321d2ULL},  // atan2(1.0000000000000001e+300, -1.0000000000000001e+300) = 2.3561944901923448
        {0x7e37e43c8800759cULL, 0xbff0000000000000ULL, 0x3ff921fb54442d18ULL},  // atan2(1.0000000000000001e+300, -1) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0x81a56e1fc2f8f359ULL, 0x3ff921fb54442d18ULL},  // atan2(1.0000000000000001e+300, -1e-300) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0x0000000000000000ULL, 0x3ff921fb54442d18ULL},  // atan2(1.0000000000000001e+300, 0) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL, 0x3ff921fb54442d18ULL},  // atan2(1.0000000000000001e+300, 1e-300) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0x3ff0000000000000ULL, 0x3ff921fb54442d18ULL},  // atan2(1.0000000000000001e+300, 1) = 1.5707963267948966
        {0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL, 0x3fe921fb54442d18ULL},  // atan2(1.0000000000000001e+300, 1.0000000000000001e+300) = 0.78539816339744828
};

test(arc_tangent_two)
{
        return math_test_two_table(arc_tangent_two, math_reference_arc_tangent_two,
                                   math_table_size(math_reference_arc_tangent_two), 2, (string_address) "arc_tangent_two");
}

static const math_reference math_reference_hypotenuse[] = {
        {0x01a56e1fc2f8f359ULL, 0x01a56e1fc2f8f359ULL, 0x01ae4e8d12762225ULL},  // hypot(1e-300, 1e-300) = 1.414213562373095e-300
        {0x01a56e1fc2f8f359ULL, 0x1eb67e9c127b6e74ULL, 0x1eb67e9c127b6e74ULL},  // hypot(1e-300, 9.9999999999999999e-161) = 9.9999999999999999e-161
        {0x01a56e1fc2f8f359ULL, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL},  // hypot(1e-300, 1) = 1
        {0x01a56e1fc2f8f359ULL, 0x4008000000000000ULL, 0x4008000000000000ULL},  // hypot(1e-300, 3) = 3
        {0x01a56e1fc2f8f359ULL, 0x6126c2d4256ffcc3ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1e-300, 1e+160) = 1e+160
        {0x01a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL},  // hypot(1e-300, 1.0000000000000001e+300) = 1.0000000000000001e+300
        {0x1eb67e9c127b6e74ULL, 0x01a56e1fc2f8f359ULL, 0x1eb67e9c127b6e74ULL},  // hypot(9.9999999999999999e-161, 1e-300) = 9.9999999999999999e-161
        {0x1eb67e9c127b6e74ULL, 0x1eb67e9c127b6e74ULL, 0x1ebfcfe76481c4b2ULL},  // hypot(9.9999999999999999e-161, 9.9999999999999999e-161) = 1.414213562373095e-160
        {0x1eb67e9c127b6e74ULL, 0x3ff0000000000000ULL, 0x3ff0000000000000ULL},  // hypot(9.9999999999999999e-161, 1) = 1
        {0x1eb67e9c127b6e74ULL, 0x4008000000000000ULL, 0x4008000000000000ULL},  // hypot(9.9999999999999999e-161, 3) = 3
        {0x1eb67e9c127b6e74ULL, 0x6126c2d4256ffcc3ULL, 0x6126c2d4256ffcc3ULL},  // hypot(9.9999999999999999e-161, 1e+160) = 1e+160
        {0x1eb67e9c127b6e74ULL, 0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL},  // hypot(9.9999999999999999e-161, 1.0000000000000001e+300) = 1.0000000000000001e+300
        {0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x3ff0000000000000ULL},  // hypot(1, 1e-300) = 1
        {0x3ff0000000000000ULL, 0x1eb67e9c127b6e74ULL, 0x3ff0000000000000ULL},  // hypot(1, 9.9999999999999999e-161) = 1
        {0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x3ff6a09e667f3bcdULL},  // hypot(1, 1) = 1.4142135623730951
        {0x3ff0000000000000ULL, 0x4008000000000000ULL, 0x40094c583ada5b53ULL},  // hypot(1, 3) = 3.1622776601683795
        {0x3ff0000000000000ULL, 0x6126c2d4256ffcc3ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1, 1e+160) = 1e+160
        {0x3ff0000000000000ULL, 0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL},  // hypot(1, 1.0000000000000001e+300) = 1.0000000000000001e+300
        {0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x4008000000000000ULL},  // hypot(3, 1e-300) = 3
        {0x4008000000000000ULL, 0x1eb67e9c127b6e74ULL, 0x4008000000000000ULL},  // hypot(3, 9.9999999999999999e-161) = 3
        {0x4008000000000000ULL, 0x3ff0000000000000ULL, 0x40094c583ada5b53ULL},  // hypot(3, 1) = 3.1622776601683795
        {0x4008000000000000ULL, 0x4008000000000000ULL, 0x4010f876ccdf6cd9ULL},  // hypot(3, 3) = 4.2426406871192848
        {0x4008000000000000ULL, 0x6126c2d4256ffcc3ULL, 0x6126c2d4256ffcc3ULL},  // hypot(3, 1e+160) = 1e+160
        {0x4008000000000000ULL, 0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL},  // hypot(3, 1.0000000000000001e+300) = 1.0000000000000001e+300
        {0x6126c2d4256ffcc3ULL, 0x01a56e1fc2f8f359ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1e+160, 1e-300) = 1e+160
        {0x6126c2d4256ffcc3ULL, 0x1eb67e9c127b6e74ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1e+160, 9.9999999999999999e-161) = 1e+160
        {0x6126c2d4256ffcc3ULL, 0x3ff0000000000000ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1e+160, 1) = 1e+160
        {0x6126c2d4256ffcc3ULL, 0x4008000000000000ULL, 0x6126c2d4256ffcc3ULL},  // hypot(1e+160, 3) = 1e+160
        {0x6126c2d4256ffcc3ULL, 0x6126c2d4256ffcc3ULL, 0x61301830a9572a89ULL},  // hypot(1e+160, 1e+160) = 1.4142135623730951e+160
        {0x6126c2d4256ffcc3ULL, 0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL},  // hypot(1e+160, 1.0000000000000001e+300) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL},  // hypot(1.0000000000000001e+300, 1e-300) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x1eb67e9c127b6e74ULL, 0x7e37e43c8800759cULL},  // hypot(1.0000000000000001e+300, 9.9999999999999999e-161) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x3ff0000000000000ULL, 0x7e37e43c8800759cULL},  // hypot(1.0000000000000001e+300, 1) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x4008000000000000ULL, 0x7e37e43c8800759cULL},  // hypot(1.0000000000000001e+300, 3) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x6126c2d4256ffcc3ULL, 0x7e37e43c8800759cULL},  // hypot(1.0000000000000001e+300, 1e+160) = 1.0000000000000001e+300
        {0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL, 0x7e40e4d50f99b211ULL},  // hypot(1.0000000000000001e+300, 1.0000000000000001e+300) = 1.4142135623730952e+300
};

test(hypotenuse)
{
        return math_test_two_table(hypotenuse, math_reference_hypotenuse,
                                   math_table_size(math_reference_hypotenuse), 2, (string_address) "hypotenuse");
}

static const math_reference math_reference_decimal_modulo[] = {
        {0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL, 0x0000000000000000ULL},  // fmod(1.0000000000000001e+300, 1.0000000000000001e+300) = 0
        {0x7e37e43c8800759cULL, 0x54b249ad2594c37dULL, 0x546adf760f4710c0ULL},  // fmod(1.0000000000000001e+300, 1e+100) = 4.5920124608001943e+98
        {0x7e37e43c8800759cULL, 0x401e000000000000ULL, 0x0000000000000000ULL},  // fmod(1.0000000000000001e+300, 7.5) = 0
        {0x7e37e43c8800759cULL, 0x4008000000000000ULL, 0x0000000000000000ULL},  // fmod(1.0000000000000001e+300, 3) = 0
        {0x7e37e43c8800759cULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // fmod(1.0000000000000001e+300, 1) = 0
        {0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL, 0x0194f722a6f79f9cULL},  // fmod(1.0000000000000001e+300, 1e-300) = 4.891554850853602e-301
        {0x7e37e43c8800759cULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(1.0000000000000001e+300, 4.9406564584124654e-324) = 0
        {0x54b249ad2594c37dULL, 0x7e37e43c8800759cULL, 0x54b249ad2594c37dULL},  // fmod(1e+100, 1.0000000000000001e+300) = 1e+100
        {0x54b249ad2594c37dULL, 0x54b249ad2594c37dULL, 0x0000000000000000ULL},  // fmod(1e+100, 1e+100) = 0
        {0x54b249ad2594c37dULL, 0x401e000000000000ULL, 0x4010000000000000ULL},  // fmod(1e+100, 7.5) = 4
        {0x54b249ad2594c37dULL, 0x4008000000000000ULL, 0x3ff0000000000000ULL},  // fmod(1e+100, 3) = 1
        {0x54b249ad2594c37dULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // fmod(1e+100, 1) = 0
        {0x54b249ad2594c37dULL, 0x01a56e1fc2f8f359ULL, 0x01a0a4021e0eec65ULL},  // fmod(1e+100, 1e-300) = 7.7650600273243269e-301
        {0x54b249ad2594c37dULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(1e+100, 4.9406564584124654e-324) = 0
        {0x401e000000000000ULL, 0x7e37e43c8800759cULL, 0x401e000000000000ULL},  // fmod(7.5, 1.0000000000000001e+300) = 7.5
        {0x401e000000000000ULL, 0x54b249ad2594c37dULL, 0x401e000000000000ULL},  // fmod(7.5, 1e+100) = 7.5
        {0x401e000000000000ULL, 0x401e000000000000ULL, 0x0000000000000000ULL},  // fmod(7.5, 7.5) = 0
        {0x401e000000000000ULL, 0x4008000000000000ULL, 0x3ff8000000000000ULL},  // fmod(7.5, 3) = 1.5
        {0x401e000000000000ULL, 0x3ff0000000000000ULL, 0x3fe0000000000000ULL},  // fmod(7.5, 1) = 0.5
        {0x401e000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x019ef63dbeb6f3ceULL},  // fmod(7.5, 1e-300) = 7.2238989602978361e-301
        {0x401e000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(7.5, 4.9406564584124654e-324) = 0
        {0x4008000000000000ULL, 0x7e37e43c8800759cULL, 0x4008000000000000ULL},  // fmod(3, 1.0000000000000001e+300) = 3
        {0x4008000000000000ULL, 0x54b249ad2594c37dULL, 0x4008000000000000ULL},  // fmod(3, 1e+100) = 3
        {0x4008000000000000ULL, 0x401e000000000000ULL, 0x4008000000000000ULL},  // fmod(3, 7.5) = 3
        {0x4008000000000000ULL, 0x4008000000000000ULL, 0x0000000000000000ULL},  // fmod(3, 3) = 0
        {0x4008000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // fmod(3, 1) = 0
        {0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x019d87654ea9f100ULL},  // fmod(3, 1e-300) = 6.8895595841191346e-301
        {0x4008000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(3, 4.9406564584124654e-324) = 0
        {0x3ff0000000000000ULL, 0x7e37e43c8800759cULL, 0x3ff0000000000000ULL},  // fmod(1, 1.0000000000000001e+300) = 1
        {0x3ff0000000000000ULL, 0x54b249ad2594c37dULL, 0x3ff0000000000000ULL},  // fmod(1, 1e+100) = 1
        {0x3ff0000000000000ULL, 0x401e000000000000ULL, 0x3ff0000000000000ULL},  // fmod(1, 7.5) = 1
        {0x3ff0000000000000ULL, 0x4008000000000000ULL, 0x3ff0000000000000ULL},  // fmod(1, 3) = 1
        {0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // fmod(1, 1) = 0
        {0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x01a33550b9c24a66ULL},  // fmod(1, 1e-300) = 8.9631865280397117e-301
        {0x3ff0000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(1, 4.9406564584124654e-324) = 0
        {0x01a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL},  // fmod(1e-300, 1.0000000000000001e+300) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x54b249ad2594c37dULL, 0x01a56e1fc2f8f359ULL},  // fmod(1e-300, 1e+100) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x401e000000000000ULL, 0x01a56e1fc2f8f359ULL},  // fmod(1e-300, 7.5) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL},  // fmod(1e-300, 3) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL},  // fmod(1e-300, 1) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x01a56e1fc2f8f359ULL, 0x0000000000000000ULL},  // fmod(1e-300, 1e-300) = 0
        {0x01a56e1fc2f8f359ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(1e-300, 4.9406564584124654e-324) = 0
        {0x0000000000000001ULL, 0x7e37e43c8800759cULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 1.0000000000000001e+300) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x54b249ad2594c37dULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 1e+100) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x401e000000000000ULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 7.5) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x4008000000000000ULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 3) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x3ff0000000000000ULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 1) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x01a56e1fc2f8f359ULL, 0x0000000000000001ULL},  // fmod(4.9406564584124654e-324, 1e-300) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // fmod(4.9406564584124654e-324, 4.9406564584124654e-324) = 0
};

test(decimal_modulo)
{
        return math_test_two_table(decimal_modulo, math_reference_decimal_modulo,
                                   math_table_size(math_reference_decimal_modulo), 0, (string_address) "decimal_modulo");
}

static const math_reference math_reference_decimal_remainder[] = {
        {0x7e37e43c8800759cULL, 0x7e37e43c8800759cULL, 0x0000000000000000ULL},  // remainder(1.0000000000000001e+300, 1.0000000000000001e+300) = 0
        {0x7e37e43c8800759cULL, 0x54b249ad2594c37dULL, 0x546adf760f4710c0ULL},  // remainder(1.0000000000000001e+300, 1e+100) = 4.5920124608001943e+98
        {0x7e37e43c8800759cULL, 0x401e000000000000ULL, 0x0000000000000000ULL},  // remainder(1.0000000000000001e+300, 7.5) = 0
        {0x7e37e43c8800759cULL, 0x4008000000000000ULL, 0x0000000000000000ULL},  // remainder(1.0000000000000001e+300, 3) = 0
        {0x7e37e43c8800759cULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // remainder(1.0000000000000001e+300, 1) = 0
        {0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL, 0x0194f722a6f79f9cULL},  // remainder(1.0000000000000001e+300, 1e-300) = 4.891554850853602e-301
        {0x7e37e43c8800759cULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(1.0000000000000001e+300, 4.9406564584124654e-324) = 0
        {0x54b249ad2594c37dULL, 0x7e37e43c8800759cULL, 0x54b249ad2594c37dULL},  // remainder(1e+100, 1.0000000000000001e+300) = 1e+100
        {0x54b249ad2594c37dULL, 0x54b249ad2594c37dULL, 0x0000000000000000ULL},  // remainder(1e+100, 1e+100) = 0
        {0x54b249ad2594c37dULL, 0x401e000000000000ULL, 0xc00c000000000000ULL},  // remainder(1e+100, 7.5) = -3.5
        {0x54b249ad2594c37dULL, 0x4008000000000000ULL, 0x3ff0000000000000ULL},  // remainder(1e+100, 3) = 1
        {0x54b249ad2594c37dULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // remainder(1e+100, 1) = 0
        {0x54b249ad2594c37dULL, 0x01a56e1fc2f8f359ULL, 0x8183287693a81bd0ULL},  // remainder(1e+100, 1e-300) = -2.2349399726756733e-301
        {0x54b249ad2594c37dULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(1e+100, 4.9406564584124654e-324) = 0
        {0x401e000000000000ULL, 0x7e37e43c8800759cULL, 0x401e000000000000ULL},  // remainder(7.5, 1.0000000000000001e+300) = 7.5
        {0x401e000000000000ULL, 0x54b249ad2594c37dULL, 0x401e000000000000ULL},  // remainder(7.5, 1e+100) = 7.5
        {0x401e000000000000ULL, 0x401e000000000000ULL, 0x0000000000000000ULL},  // remainder(7.5, 7.5) = 0
        {0x401e000000000000ULL, 0x4008000000000000ULL, 0x3ff8000000000000ULL},  // remainder(7.5, 3) = 1.5
        {0x401e000000000000ULL, 0x3ff0000000000000ULL, 0xbfe0000000000000ULL},  // remainder(7.5, 1) = -0.5
        {0x401e000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x8187cc038e75e5c8ULL},  // remainder(7.5, 1e-300) = -2.7761010397021641e-301
        {0x401e000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(7.5, 4.9406564584124654e-324) = 0
        {0x4008000000000000ULL, 0x7e37e43c8800759cULL, 0x4008000000000000ULL},  // remainder(3, 1.0000000000000001e+300) = 3
        {0x4008000000000000ULL, 0x54b249ad2594c37dULL, 0x4008000000000000ULL},  // remainder(3, 1e+100) = 3
        {0x4008000000000000ULL, 0x401e000000000000ULL, 0x4008000000000000ULL},  // remainder(3, 7.5) = 3
        {0x4008000000000000ULL, 0x4008000000000000ULL, 0x0000000000000000ULL},  // remainder(3, 3) = 0
        {0x4008000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // remainder(3, 1) = 0
        {0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x818aa9b46e8feb64ULL},  // remainder(3, 1e-300) = -3.1104404158808657e-301
        {0x4008000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(3, 4.9406564584124654e-324) = 0
        {0x3ff0000000000000ULL, 0x7e37e43c8800759cULL, 0x3ff0000000000000ULL},  // remainder(1, 1.0000000000000001e+300) = 1
        {0x3ff0000000000000ULL, 0x54b249ad2594c37dULL, 0x3ff0000000000000ULL},  // remainder(1, 1e+100) = 1
        {0x3ff0000000000000ULL, 0x401e000000000000ULL, 0x3ff0000000000000ULL},  // remainder(1, 7.5) = 1
        {0x3ff0000000000000ULL, 0x4008000000000000ULL, 0x3ff0000000000000ULL},  // remainder(1, 3) = 1
        {0x3ff0000000000000ULL, 0x3ff0000000000000ULL, 0x0000000000000000ULL},  // remainder(1, 1) = 0
        {0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL, 0x8171c67849b54798ULL},  // remainder(1, 1e-300) = -1.0368134719602886e-301
        {0x3ff0000000000000ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(1, 4.9406564584124654e-324) = 0
        {0x01a56e1fc2f8f359ULL, 0x7e37e43c8800759cULL, 0x01a56e1fc2f8f359ULL},  // remainder(1e-300, 1.0000000000000001e+300) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x54b249ad2594c37dULL, 0x01a56e1fc2f8f359ULL},  // remainder(1e-300, 1e+100) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x401e000000000000ULL, 0x01a56e1fc2f8f359ULL},  // remainder(1e-300, 7.5) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x4008000000000000ULL, 0x01a56e1fc2f8f359ULL},  // remainder(1e-300, 3) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x3ff0000000000000ULL, 0x01a56e1fc2f8f359ULL},  // remainder(1e-300, 1) = 1e-300
        {0x01a56e1fc2f8f359ULL, 0x01a56e1fc2f8f359ULL, 0x0000000000000000ULL},  // remainder(1e-300, 1e-300) = 0
        {0x01a56e1fc2f8f359ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(1e-300, 4.9406564584124654e-324) = 0
        {0x0000000000000001ULL, 0x7e37e43c8800759cULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 1.0000000000000001e+300) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x54b249ad2594c37dULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 1e+100) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x401e000000000000ULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 7.5) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x4008000000000000ULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 3) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x3ff0000000000000ULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 1) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x01a56e1fc2f8f359ULL, 0x0000000000000001ULL},  // remainder(4.9406564584124654e-324, 1e-300) = 4.9406564584124654e-324
        {0x0000000000000001ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},  // remainder(4.9406564584124654e-324, 4.9406564584124654e-324) = 0
};

test(decimal_remainder)
{
        return math_test_two_table(decimal_remainder, math_reference_decimal_remainder,
                                   math_table_size(math_reference_decimal_remainder), 0, (string_address) "decimal_remainder");
}

/*
        The classes, of one of every kind of number the format holds.

        Six questions about ten values, and every answer is written down
        rather than derived, because a classification routine that agreed
        with a second classification routine written the same way would prove
        nothing about either.
*/
test(classification)
{
        decimal zero = 0.0;
        decimal minus_zero = math_test_value(0x8000000000000000ULL);
        decimal one = 1.0;
        decimal minus_one = -1.0;
        decimal endless = math_test_value(0x7ff0000000000000ULL);
        decimal minus_endless = math_test_value(0xfff0000000000000ULL);
        decimal not_a_number = math_test_value(0x7ff8000000000000ULL);
        decimal smallest = math_test_value(0x0000000000000001ULL);
        decimal largest_subnormal = math_test_value(0x000fffffffffffffULL);
        decimal largest = math_test_value(0x7fefffffffffffffULL);

        fail(decimal_class(zero) == MATH_CLASS_ZERO);
        fail(decimal_class(minus_zero) == MATH_CLASS_ZERO);
        fail(decimal_class(one) == MATH_CLASS_NORMAL);
        fail(decimal_class(minus_one) == MATH_CLASS_NORMAL);
        fail(decimal_class(endless) == MATH_CLASS_INFINITE);
        fail(decimal_class(minus_endless) == MATH_CLASS_INFINITE);
        fail(decimal_class(not_a_number) == MATH_CLASS_NAN);
        fail(decimal_class(smallest) == MATH_CLASS_SUBNORMAL);
        fail(decimal_class(largest_subnormal) == MATH_CLASS_SUBNORMAL);
        fail(decimal_class(largest) == MATH_CLASS_NORMAL);

        fail(!decimal_is_nan(zero) && !decimal_is_nan(endless) && decimal_is_nan(not_a_number));
        fail(!decimal_is_infinite(largest) && decimal_is_infinite(endless) &&
             decimal_is_infinite(minus_endless));
        fail(decimal_is_finite(zero) && decimal_is_finite(smallest) && decimal_is_finite(largest));
        fail(!decimal_is_finite(endless) && !decimal_is_finite(not_a_number));
        fail(decimal_is_normal(one) && decimal_is_normal(largest));
        fail(!decimal_is_normal(zero) && !decimal_is_normal(smallest) && !decimal_is_normal(endless));

        //      The one question a comparison against zero cannot answer.
        fail(!decimal_sign_bit(zero) && decimal_sign_bit(minus_zero));
        fail(!decimal_sign_bit(one) && decimal_sign_bit(minus_one));
        fail(!decimal_sign_bit(endless) && decimal_sign_bit(minus_endless));

        //      And the same six of a float, which are a different set of
        //      masks against a different width.
        fail(!narrow_is_nan(1.0f) && narrow_is_nan(0.0f / 0.0f));
        fail(narrow_is_infinite(1.0f / 0.0f) && !narrow_is_infinite(3.4028234663852886e38f));
        fail(narrow_is_finite(0.0f) && !narrow_is_finite(1.0f / 0.0f));
        fail(narrow_is_normal(1.0f) && !narrow_is_normal(0.0f));
        fail(narrow_sign_bit(-0.0f) && !narrow_sign_bit(0.0f));
        fail(narrow_class(0.0f) == MATH_CLASS_ZERO);
        fail(narrow_class(1.0f) == MATH_CLASS_NORMAL);

        return true;
}

/*
        The zeros that have to stay negative.

        Every one of these is a place where the obvious arithmetic produces a
        positive zero and the standard asks for a negative one, usually
        because a series multiplied a minus zero by a negative coefficient on
        its way past. They are cheap to get right and impossible to notice
        going wrong.
*/
test(signed_zeros)
{
        decimal minus_zero = math_test_value(0x8000000000000000ULL);

        fail(math_test_bits(sine(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(tangent(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(arc_sine(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(arc_tangent(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(exponential_minus_one(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(hyperbolic_sine(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(hyperbolic_tangent(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(cube_root(minus_zero)) == 0x8000000000000000ULL);
        fail(math_test_bits(decimal_scaled(minus_zero, 10)) == 0x8000000000000000ULL);

        //      cos of a minus zero is a positive one, which is the one place
        //      here the sign is supposed to disappear.
        fail(math_test_bits(cosine(minus_zero)) == 0x3ff0000000000000ULL);

        //      modf of a whole negative number leaves a negative zero
        //      behind, and modf of a negative zero leaves two of them.
        {
                decimal whole;
                fail(math_test_bits(decimal_split_whole(-3.0, address_of whole)) ==
                     0x8000000000000000ULL);
                fail(whole == -3.0);
                fail(math_test_bits(decimal_split_whole(minus_zero, address_of whole)) ==
                     0x8000000000000000ULL);
                fail(math_test_bits(whole) == 0x8000000000000000ULL);
        }

        //      fmod keeps the dividend's sign on a zero remainder, and
        //      remainder does the same.
        fail(math_test_bits(decimal_modulo(-6.0, 3.0)) == 0x8000000000000000ULL);
        fail(math_test_bits(decimal_remainder(-6.0, 3.0)) == 0x8000000000000000ULL);

        //      atan2 has four zeros of its own and the standard names all of
        //      them.
        fail(math_test_bits(arc_tangent_two(0.0, 1.0)) == 0);
        fail(math_test_bits(arc_tangent_two(minus_zero, 1.0)) == 0x8000000000000000ULL);
        fail(arc_tangent_two(0.0, -1.0) > 3.14 && arc_tangent_two(0.0, -1.0) < 3.15);
        fail(decimal_sign_bit(arc_tangent_two(minus_zero, -1.0)));

        return true;
}

/*
        The answers the standard states outright, which no approximation is
        allowed to be near rather than equal to.
*/
test(exact_answers)
{
        decimal endless = math_test_value(0x7ff0000000000000ULL);
        decimal minus_endless = math_test_value(0xfff0000000000000ULL);

        fail(exponential(0.0) == 1.0);
        fail(exponential_two(0.0) == 1.0);
        fail(exponential_minus_one(0.0) == 0.0);
        fail(logarithm(1.0) == 0.0);
        fail(logarithm_two(1.0) == 0.0);
        fail(logarithm_ten(1.0) == 0.0);
        fail(sine(0.0) == 0.0);
        fail(cosine(0.0) == 1.0);
        fail(tangent(0.0) == 0.0);
        fail(arc_sine(0.0) == 0.0);
        fail(arc_tangent(0.0) == 0.0);
        fail(arc_cosine(1.0) == 0.0);
        fail(hyperbolic_sine(0.0) == 0.0);
        fail(hyperbolic_cosine(0.0) == 1.0);
        fail(hyperbolic_tangent(0.0) == 0.0);

        //      Every power of two is its own base two logarithm, exactly,
        //      because the exponent field is the answer and nothing is
        //      multiplied into it.
        {
                b32 exponent;
                for (exponent = -1022; exponent <= 1023; exponent++)
                        fail(logarithm_two(decimal_scaled(1.0, exponent)) == (decimal)exponent);
        }

        //      And exp2 of a whole number is that power of two.
        {
                b32 exponent;
                for (exponent = -1022; exponent <= 1023; exponent++)
                        fail(exponential_two((decimal)exponent) == decimal_scaled(1.0, exponent));
        }

        //      Small whole powers are exact until the result stops fitting.
        fail(power(2.0, 10.0) == 1024.0);
        fail(power(3.0, 5.0) == 243.0);
        fail(power(10.0, 22.0) == 1e22);
        fail(power(-2.0, 3.0) == -8.0);
        fail(power(-2.0, 4.0) == 16.0);
        fail(power(0.5, 3.0) == 0.125);

        //      Exact cubes come back exact.
        fail(cube_root(8.0) == 2.0);
        fail(cube_root(-8.0) == -2.0);
        fail(cube_root(27.0) == 3.0);
        fail(cube_root(1000000.0) == 100.0);

        //      hypot of a leg and a zero is the leg, and the three four five
        //      triangle is exact.
        fail(hypotenuse(3.0, 4.0) == 5.0);
        fail(hypotenuse(5.0, 0.0) == 5.0);
        fail(hypotenuse(0.0, -7.0) == 7.0);

        //      The cases C99 lists for pow that do not depend on the base at
        //      all, a NaN included.
        fail(power(math_test_value(0x7ff8000000000000ULL), 0.0) == 1.0);
        fail(power(1.0, math_test_value(0x7ff8000000000000ULL)) == 1.0);
        fail(power(-1.0, endless) == 1.0);
        fail(power(-1.0, minus_endless) == 1.0);
        fail(power(0.5, endless) == 0.0);
        fail(power(2.0, endless) == endless);
        fail(power(2.0, minus_endless) == 0.0);
        fail(power(endless, 2.0) == endless);
        fail(power(minus_endless, 3.0) == minus_endless);
        fail(power(minus_endless, 2.0) == endless);
        fail(power(0.0, -1.0) == endless);
        fail(power(math_test_value(0x8000000000000000ULL), -1.0) == minus_endless);

        //      A negative base and a fractional exponent is not a real
        //      number and has to say so.
        fail(decimal_is_nan(power(-2.0, 0.5)));
        fail(decimal_is_nan(logarithm(-1.0)));
        fail(decimal_is_nan(arc_sine(1.5)));
        fail(decimal_is_nan(arc_cosine(-1.5)));
        fail(decimal_is_nan(sine(endless)));
        fail(decimal_is_nan(decimal_modulo(1.0, 0.0)));

        //      An infinity beats a NaN in hypot, which is the one place C
        //      says it does.
        fail(hypotenuse(endless, math_test_value(0x7ff8000000000000ULL)) == endless);

        //      log of zero is minus infinity and of a negative is a NaN.
        fail(logarithm(0.0) == minus_endless);
        fail(logarithm_two(0.0) == minus_endless);
        fail(logarithm_ten(0.0) == minus_endless);

        return true;
}

/*
        The round trips, which say more about the exact routines than any
        table of answers could: they run over the whole exponent range rather
        than over a handful of points.
*/
test(round_trips)
{
        b32 exponent;
        b32 given;
        decimal value;
        decimal part;
        decimal whole;

        //      frexp hands back a significand in [1/2, 1) and an exponent
        //      that rebuilds the argument exactly, for every exponent the
        //      format has including the subnormal ones.
        for (exponent = -1074; exponent <= 1023; exponent++)
        {
                value = decimal_scaled(1.5, exponent);
                if (value == 0.0 || decimal_is_infinite(value))
                        continue;
                part = decimal_split_exponent(value, address_of given);
                fail(absolute(part) >= 0.5 && absolute(part) < 1.0);
                fail(decimal_scaled(part, given) == value);
        }

        //      ldexp up and back down is the identity wherever nothing
        //      overflowed on the way.
        for (exponent = -500; exponent <= 500; exponent++)
        {
                value = 1.2345678901234567;
                fail(decimal_scaled(decimal_scaled(value, exponent), -exponent) == value);
        }

        //      modf's two halves add back to the argument exactly.
        for (exponent = -60; exponent <= 60; exponent++)
        {
                value = decimal_scaled(1.4142135623730951, exponent);
                part = decimal_split_whole(value, address_of whole);
                fail(whole + part == value);
                fail(whole == decimal_truncated(value));
        }

        //      fmod is exact by construction, so the identity that defines
        //      it holds to the bit: x is a whole number of divisors plus the
        //      remainder, and the remainder is smaller than the divisor.
        for (exponent = -300; exponent <= 300; exponent++)
        {
                decimal left = decimal_scaled(1.7320508075688772, exponent);
                decimal right = 3.25;
                decimal left_over = decimal_modulo(left, right);
                fail(absolute(left_over) < absolute(right));
                fail(decimal_modulo(left_over, right) == left_over);
        }

        //      remainder lands inside half a divisor either way, always.
        for (exponent = -300; exponent <= 300; exponent++)
        {
                decimal left = decimal_scaled(1.7320508075688772, exponent);
                decimal right = 3.25;
                fail(absolute(decimal_remainder(left, right)) <= 0.5 * absolute(right));
        }

        return true;
}

/*
        Payne and Hanek's reduction is what this is really testing.

        An argument reduction that degrades with the size of the argument
        will pass every table above, because the tables cannot cover the
        whole exponent range densely. What catches it is an identity that has
        to hold at every scale: sine squared plus cosine squared is one, and
        the tangent is their quotient, and both of those stop holding the
        moment the reduction loses bits -- long before the answer looks
        obviously wrong.
*/
test(circular_identities)
{
        b32 exponent;
        decimal value;
        decimal opposite;
        decimal adjacent;
        decimal total;

        for (exponent = -60; exponent <= 1023; exponent++)
        {
                value = decimal_scaled(1.4142135623730951, exponent);
                opposite = sine(value);
                adjacent = cosine(value);
                total = opposite * opposite + adjacent * adjacent;

                //      Four ulp of one, which is what two kernels squared
                //      and added can cost and no more.
                fail(math_test_distance(total, 1.0) <= 4);

                //      And the tangent agrees with the quotient it is not
                //      computed as, everywhere the quotient is meaningful.
                if (absolute(adjacent) > 1e-8)
                        fail(math_test_distance(tangent(value), opposite / adjacent) <= 8);
        }

        //      The inverses undo the forward functions across the domain.
        //
        //      The tolerance here is absolute rather than in last places,
        //      and that is not laziness. acos of zero is pi/2, which is not
        //      a double; the cosine of the double it rounds to is 6.1e-17
        //      rather than zero, and 6.1e-17 is an infinite number of last
        //      places away from zero while being a perfectly correct answer.
        //      A round trip through a function with a zero in it can only be
        //      judged on the absolute size of what comes back.
        for (exponent = 0; exponent < 2000; exponent++)
        {
                value = -1.0 + (decimal)exponent * 0.001;
                fail(absolute(sine(arc_sine(value)) - value) <= 1.0e-15);
                fail(absolute(cosine(arc_cosine(value)) - value) <= 1.0e-15);
                fail(absolute(tangent(arc_tangent(value)) - value) <= 1.0e-15);
                fail(absolute(hyperbolic_tangent(value) -
                              hyperbolic_sine(value) / hyperbolic_cosine(value)) <= 1.0e-15);
        }

        //      exp and log undo each other, and pow agrees with repeated
        //      multiplication where the multiplication is exact.
        //
        //      The allowance on the two round trips grows with the size of
        //      the logarithm, and that is arithmetic rather than slack. The
        //      logarithm's error is half an ulp OF THE LOGARITHM, which for
        //      an argument of 2^-300 is an absolute error of 208 times
        //      2^-53; the exponential then turns an absolute error in its
        //      argument into a relative error in its answer of the same
        //      size. A round trip through both is therefore allowed to be
        //      about |log x| units out and cannot be asked for better by any
        //      implementation of either.
        for (exponent = -300; exponent <= 300; exponent++)
        {
                positive allowance;

                value = decimal_scaled(1.7320508075688772, exponent);

                allowance = 8 + 2 * (positive)absolute(logarithm(value));
                fail(math_test_distance(exponential(logarithm(value)), value) <= allowance);

                allowance = 8 + 2 * (positive)absolute(logarithm_two(value));
                fail(math_test_distance(exponential_two(logarithm_two(value)), value) <= allowance);

                fail(math_test_distance(power(value, 2.0), value * value) <= 2);
                fail(math_test_distance(power(value, 0.5), square_root(value)) <= 2);

                //      Only where the cube itself survived the format: a
                //      cube that overflowed or flushed to zero says nothing
                //      about the cube root.
                if (decimal_is_finite(value * value * value) && value * value * value != 0.0)
                        fail(math_test_distance(cube_root(value * value * value), value) <= 4);
        }

        //      cosh squared minus sinh squared is one, which is the
        //      hyperbolic version of the same test and reaches the far end
        //      of the exponential's range.
        //      The two squares grow together and their difference stays
        //      one, so the difference is judged against the last bit of the
        //      squares rather than against the last bit of the one. Past a
        //      cosine of about a hundred million the one is entirely below
        //      that last bit and the subtraction can only answer zero, which
        //      would be a statement about the format rather than about these
        //      two routines, so the sweep stops there.
        for (exponent = 0; exponent < 700; exponent++)
        {
                value = (decimal)exponent * 0.025;
                opposite = hyperbolic_sine(value);
                adjacent = hyperbolic_cosine(value);
                if (adjacent > 1.0e8)
                        break;
                fail(absolute(adjacent * adjacent - opposite * opposite - 1.0) <=
                     8.0 * adjacent * adjacent * 2.220446049250313e-16);
        }

        return true;
}

static test_case test_cases[] = {
        case(sine),
        case(cosine),
        case(tangent),
        case(exponential),
        case(exponential_two),
        case(exponential_minus_one),
        case(logarithm),
        case(logarithm_two),
        case(logarithm_ten),
        case(arc_sine),
        case(arc_cosine),
        case(arc_tangent),
        case(hyperbolic_sine),
        case(hyperbolic_cosine),
        case(hyperbolic_tangent),
        case(cube_root),
        case(power),
        case(arc_tangent_two),
        case(hypotenuse),
        case(decimal_modulo),
        case(decimal_remainder),
        case(classification),
        case(signed_zeros),
        case(exact_answers),
        case(round_trips),
        case(circular_identities),
        {0, 0, 0},
};

b32 main()
{
        log_direct(str("math tests\n\n"));

        test_cases_walk(test_cases);

        return test_report((string_address) "\n");
}
