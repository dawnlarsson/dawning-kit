#include "../compiler_memory.c"

/*
        The number conversions: text into an integer, and text into a float
        that is the nearest one.

        Six things are checked here and they are checked in six different
        ways, because the failure modes are not alike.

        THE INTEGER NAMES ARE CHECKED BY BEING CALLED. abs, labs, llabs, atoi,
        atol, atoll, strtol, strtoll, strtoul, strtoull, imaxabs, strtoimax
        and strtoumax are assembly in src/platform/standard.inc with no C
        declaration in front of them, which meant that until numbers.c every
        one of those call sites was a compile error in a file that linked
        perfectly. So the first group is a call to each of them with an answer
        beside it: it is a test of the arithmetic, and it is also the only
        thing that can prove the declarations are right, because a wrong one
        does not link and a missing one does not compile.

        THE FLOAT ANSWERS ARE BAKED, AND THEY CAME FROM GLIBC. A freestanding
        program has nothing to compare a conversion against, so the table
        below carries the bit pattern, the end pointer and the errno that
        glibc 2.44 produced for each of a hundred and fifty five inputs, taken
        on x86_64 and written down. They are bit patterns rather than decimal
        literals so that nothing about the expected answer depends on the
        compiler's own parser. The list is not a sample: it is the boundaries,
        the subnormals, the classic hard cases from the literature, the
        hexadecimal corners, and the two dozen malformed strings where an end
        pointer has to come back pointing at the ORIGINAL text.

        THE THREE TIERS ARE CHECKED AGAINST EACH OTHER. numbers.c answers a
        conversion three ways depending on the input -- an exact multiply, a
        128 bit estimate, or a decimal register that is exact by construction
        -- and the last of those is reachable on its own by calling
        numbers_read and numbers_assemble, which are static in the same
        translation unit this file is. So every generated input is converted
        twice, once by strtod and once by the register alone, and they have to
        agree. That finds a defect in either fast tier without a reference
        library being anywhere near it.

        THE SHIFT TABLE IS CHECKED AGAINST ITSELF. The decimal register
        multiplies by two to the k in one pass using a table that says how
        many digits the answer gains. Shifting by one, k times, uses only the
        first entry of that table and reaches the same number, so the two
        routes are compared for every k up to sixty on a dozen registers.

        MONOTONICITY, which is the property a conversion can lose without any
        single answer looking wrong: a larger decimal must never convert to a
        smaller float.

        AND THE SMALL INTEGERS EXHAUSTIVELY, in three spellings each, against
        the compiler's own conversion of the same number.
*/

#define number_case(name) bool number_test_##name(void)

static positive number_checks = 0;
static positive number_failures = 0;
static positive number_reported = 0;

static fn number_note(bool held, string_address what, positive got, positive wanted)
{
        number_checks++;

        if (held)
                return;

        number_failures++;

        if (number_reported < 40)
        {
                number_reported++;
                string_format(log, "FAIL %s: got %p wanted %p\n", what, got, wanted);
        }
}

static fn number_say(bool held, string_address what)
{
        number_checks++;

        if (held)
                return;

        number_failures++;

        if (number_reported < 40)
        {
                number_reported++;
                string_format(log, "FAIL %s\n", what);
        }
}

#define text(literal) ((string_address)(literal))

typedef union
{
        decimal value;
        p64 bits;
} number_wide_shape;

typedef union
{
        f32 value;
        p32 bits;
} number_narrow_shape;

typedef union
{
        f128 value;
        p128 bits;
} number_extended_shape;

typedef struct
{
        const char address_to text;
        p64 wide_bits;
        positive wide_end;
        b32 wide_errno;
        p32 narrow_bits;
        positive narrow_end;
        b32 narrow_errno;
} number_float_case;

static const number_float_case number_float_cases[] = {
        {"0",
         0x0000000000000000ULL, 1, 0, 0x00000000U, 1, 0},
        {"-0",
         0x8000000000000000ULL, 2, 0, 0x80000000U, 2, 0},
        {"0.0",
         0x0000000000000000ULL, 3, 0, 0x00000000U, 3, 0},
        {"-0.0",
         0x8000000000000000ULL, 4, 0, 0x80000000U, 4, 0},
        {"1",
         0x3ff0000000000000ULL, 1, 0, 0x3f800000U, 1, 0},
        {"-1",
         0xbff0000000000000ULL, 2, 0, 0xbf800000U, 2, 0},
        {"1.5",
         0x3ff8000000000000ULL, 3, 0, 0x3fc00000U, 3, 0},
        {"-1.5",
         0xbff8000000000000ULL, 4, 0, 0xbfc00000U, 4, 0},
        {"0.1",
         0x3fb999999999999aULL, 3, 0, 0x3dcccccdU, 3, 0},
        {"-0.1",
         0xbfb999999999999aULL, 4, 0, 0xbdcccccdU, 4, 0},
        {"0.2",
         0x3fc999999999999aULL, 3, 0, 0x3e4ccccdU, 3, 0},
        {"0.3",
         0x3fd3333333333333ULL, 3, 0, 0x3e99999aU, 3, 0},
        {"3.14159265358979323846",
         0x400921fb54442d18ULL, 22, 0, 0x40490fdbU, 22, 0},
        {"2.718281828459045235360287471352662497757",
         0x4005bf0a8b145769ULL, 41, 0, 0x402df854U, 41, 0},
        {"1e0",
         0x3ff0000000000000ULL, 3, 0, 0x3f800000U, 3, 0},
        {"1e1",
         0x4024000000000000ULL, 3, 0, 0x41200000U, 3, 0},
        {"1e-1",
         0x3fb999999999999aULL, 4, 0, 0x3dcccccdU, 4, 0},
        {"1e10",
         0x4202a05f20000000ULL, 4, 0, 0x501502f9U, 4, 0},
        {"1e-10",
         0x3ddb7cdfd9d7bdbbULL, 5, 0, 0x2edbe6ffU, 5, 0},
        {"1e22",
         0x4480f0cf064dd592ULL, 4, 0, 0x64078678U, 4, 0},
        {"1e23",
         0x44b52d02c7e14af6ULL, 4, 0, 0x65a96816U, 4, 0},
        {"1e-22",
         0x3b5e392010175ee6ULL, 5, 0, 0x1af1c901U, 5, 0},
        {"1e-23",
         0x3b282db34012b251ULL, 5, 0, 0x19416d9aU, 5, 0},
        {"1e100",
         0x54b249ad2594c37dULL, 5, 0, 0x7f800000U, 5, 34},
        {"1e-100",
         0x2b2bff2ee48e0530ULL, 6, 0, 0x00000000U, 6, 34},
        {"1e300",
         0x7e37e43c8800759cULL, 5, 0, 0x7f800000U, 5, 34},
        {"1e-300",
         0x01a56e1fc2f8f359ULL, 6, 0, 0x00000000U, 6, 34},
        {"1e308",
         0x7fe1ccf385ebc8a0ULL, 5, 0, 0x7f800000U, 5, 34},
        {"1e-308",
         0x000730d67819e8d2ULL, 6, 34, 0x00000000U, 6, 34},
        {"1e309",
         0x7ff0000000000000ULL, 5, 34, 0x7f800000U, 5, 34},
        {"1e-309",
         0x0000b8157268fdafULL, 6, 34, 0x00000000U, 6, 34},
        {"1e-320",
         0x00000000000007e8ULL, 6, 34, 0x00000000U, 6, 34},
        {"1e-323",
         0x0000000000000002ULL, 6, 34, 0x00000000U, 6, 34},
        {"1e-324",
         0x0000000000000000ULL, 6, 34, 0x00000000U, 6, 34},
        {"1e-325",
         0x0000000000000000ULL, 6, 34, 0x00000000U, 6, 34},
        {"1e400",
         0x7ff0000000000000ULL, 5, 34, 0x7f800000U, 5, 34},
        {"1e-400",
         0x0000000000000000ULL, 6, 34, 0x00000000U, 6, 34},
        {"4.9406564584124654e-324",
         0x0000000000000001ULL, 23, 34, 0x00000000U, 23, 34},
        {"4.9406564584124655e-324",
         0x0000000000000001ULL, 23, 34, 0x00000000U, 23, 34},
        {"2.4703282292062327e-324",
         0x0000000000000000ULL, 23, 34, 0x00000000U, 23, 34},
        {"2.4703282292062328e-324",
         0x0000000000000001ULL, 23, 34, 0x00000000U, 23, 34},
        {"2.2250738585072011e-308",
         0x000fffffffffffffULL, 23, 34, 0x00000000U, 23, 34},
        {"2.2250738585072012e-308",
         0x0010000000000000ULL, 23, 34, 0x00000000U, 23, 34},
        {"2.2250738585072013e-308",
         0x0010000000000000ULL, 23, 0, 0x00000000U, 23, 34},
        {"2.2250738585072014e-308",
         0x0010000000000000ULL, 23, 0, 0x00000000U, 23, 34},
        {"1.7976931348623157e308",
         0x7fefffffffffffffULL, 22, 0, 0x7f800000U, 22, 34},
        {"1.7976931348623158e308",
         0x7fefffffffffffffULL, 22, 0, 0x7f800000U, 22, 34},
        {"1.7976931348623159e308",
         0x7ff0000000000000ULL, 22, 34, 0x7f800000U, 22, 34},
        {"9007199254740992",
         0x4340000000000000ULL, 16, 0, 0x5a000000U, 16, 0},
        {"9007199254740993",
         0x4340000000000000ULL, 16, 0, 0x5a000000U, 16, 0},
        {"9007199254740994",
         0x4340000000000001ULL, 16, 0, 0x5a000000U, 16, 0},
        {"9007199254740995",
         0x4340000000000002ULL, 16, 0, 0x5a000000U, 16, 0},
        {"18446744073709551615",
         0x43f0000000000000ULL, 20, 0, 0x5f800000U, 20, 0},
        {"18446744073709551616",
         0x43f0000000000000ULL, 20, 0, 0x5f800000U, 20, 0},
        {"340282366920938463463374607431768211456",
         0x47f0000000000000ULL, 39, 0, 0x7f800000U, 39, 34},
        {"123456789012345678901234567890",
         0x45f8ee90ff6c373eULL, 30, 0, 0x6fc77488U, 30, 0},
        {"0.000000000000000000000000000001",
         0x39b4484bfeebc2a0ULL, 32, 0, 0x0da24260U, 32, 0},
        {"1234567890123456789012345678901234567890e-40",
         0x3fbf9add3746f65fULL, 44, 0, 0x3dfcd6eaU, 44, 0},
        {"7.8459735791271921e65",
         0x4d9dcd0089c1314eULL, 21, 0, 0x7f800000U, 21, 34},
        {"3.571e266",
         0x77462644c61d41aaULL, 9, 0, 0x7f800000U, 9, 34},
        {"3.08984926168550152811e-32",
         0x39640de48676653bULL, 26, 0, 0x0b206f24U, 26, 0},
        {"8.98846567431158e307",
         0x7fe0000000000000ULL, 20, 0, 0x7f800000U, 20, 34},
        {"1.00000000000000005",
         0x3ff0000000000000ULL, 19, 0, 0x3f800000U, 19, 0},
        {"0.5000000000000000166533453693773481063544750213623046875",
         0x3fe0000000000000ULL, 57, 0, 0x3f000000U, 57, 0},
        {"3.518437208883201171875e13",
         0x42c0000000000002ULL, 26, 0, 0x56000000U, 26, 0},
        {"62.5364939768271845828",
         0x404f44abd5aa7ca4ULL, 22, 0, 0x427a255fU, 22, 0},
        {"8.10109172351e-33",
         0x394508195549f5feULL, 17, 0, 0x0a2840cbU, 17, 0},
        {"1.50000000000000011102230246251565404236316680908203125",
         0x3ff8000000000000ULL, 55, 0, 0x3fc00000U, 55, 0},
        {"9007199254740991.4999999999999999999999999999999999",
         0x433fffffffffffffULL, 51, 0, 0x5a000000U, 51, 0},
        {"5e-324",
         0x0000000000000001ULL, 6, 34, 0x00000000U, 6, 34},
        {"7.4109846876186981626485318930233205854758970392148714663837852375101326090531312779794975454245398856969484704316857659638998506553390969459816219401617281718945106978546710679176872575177347315553307795408549809608457500958111373034747658096871009590975442271004757307809711118935784838675653998783503015228055934046593739791790738723868299395818481660169122019456499931289798411362062484498678713572180352209017023903285791732520220528974020802906854021606612375549983402671300035812486479041385743401875520901590172592547146296175134159774938718574737870961645638908718119841271673056017045493004705269590165763776884908267986972573366521765567941072508764337560846003984904972149117463085539556354188641513168478436313080237596295773983001708984374999e-324",
         0x0000000000000001ULL, 761, 34, 0x00000000U, 761, 34},
        {"5e-324",
         0x0000000000000001ULL, 6, 34, 0x00000000U, 6, 34},
        {"inf",
         0x7ff0000000000000ULL, 3, 0, 0x7f800000U, 3, 0},
        {"-inf",
         0xfff0000000000000ULL, 4, 0, 0xff800000U, 4, 0},
        {"INFINITY",
         0x7ff0000000000000ULL, 8, 0, 0x7f800000U, 8, 0},
        {"-INFINITY",
         0xfff0000000000000ULL, 9, 0, 0xff800000U, 9, 0},
        {"InF",
         0x7ff0000000000000ULL, 3, 0, 0x7f800000U, 3, 0},
        {"infi",
         0x7ff0000000000000ULL, 3, 0, 0x7f800000U, 3, 0},
        {"nan",
         0x7ff8000000000000ULL, 3, 0, 0x7fc00000U, 3, 0},
        {"-nan",
         0xfff8000000000000ULL, 4, 0, 0xffc00000U, 4, 0},
        {"NaN",
         0x7ff8000000000000ULL, 3, 0, 0x7fc00000U, 3, 0},
        {"nan(1)",
         0x7ff8000000000001ULL, 6, 0, 0x7fc00001U, 6, 0},
        {"nan(0x7)",
         0x7ff8000000000007ULL, 8, 0, 0x7fc00007U, 8, 0},
        {"nan()",
         0x7ff8000000000000ULL, 5, 0, 0x7fc00000U, 5, 0},
        {"nan(zz)",
         0x7ff8000000000000ULL, 7, 0, 0x7fc00000U, 7, 0},
        {"0x1p0",
         0x3ff0000000000000ULL, 5, 0, 0x3f800000U, 5, 0},
        {"0x1p1",
         0x4000000000000000ULL, 5, 0, 0x40000000U, 5, 0},
        {"-0x1p-1",
         0xbfe0000000000000ULL, 7, 0, 0xbf000000U, 7, 0},
        {"0x1.8p3",
         0x4028000000000000ULL, 7, 0, 0x41400000U, 7, 0},
        {"0x1.fffffffffffffp1023",
         0x7fefffffffffffffULL, 22, 0, 0x7f800000U, 22, 34},
        {"0x1.fffffffffffff8p0",
         0x4000000000000000ULL, 20, 0, 0x40000000U, 20, 0},
        {"0x1.0000000000001p0",
         0x3ff0000000000001ULL, 19, 0, 0x3f800000U, 19, 0},
        {"0x1p-1074",
         0x0000000000000001ULL, 9, 0, 0x00000000U, 9, 34},
        {"0x1p-1075",
         0x0000000000000000ULL, 9, 34, 0x00000000U, 9, 34},
        {"0x1p-1076",
         0x0000000000000000ULL, 9, 34, 0x00000000U, 9, 34},
        {"0x1p1024",
         0x7ff0000000000000ULL, 8, 34, 0x7f800000U, 8, 34},
        {"0x0p0",
         0x0000000000000000ULL, 5, 0, 0x00000000U, 5, 0},
        {"-0x0p0",
         0x8000000000000000ULL, 6, 0, 0x80000000U, 6, 0},
        {"0x.8p1",
         0x3ff0000000000000ULL, 6, 0, 0x3f800000U, 6, 0},
        {"0x8.p-1",
         0x4010000000000000ULL, 7, 0, 0x40800000U, 7, 0},
        {"0X1P+2",
         0x4010000000000000ULL, 6, 0, 0x40800000U, 6, 0},
        {"0xabcdefp-20",
         0x402579bde0000000ULL, 12, 0, 0x412bcdefU, 12, 0},
        {"0x1.00000000000008p0",
         0x3ff0000000000000ULL, 20, 0, 0x3f800000U, 20, 0},
        {"0x1.00000000000018p0",
         0x3ff0000000000002ULL, 20, 0, 0x3f800000U, 20, 0},
        {"0x435p-1073",
         0x000000000000086aULL, 11, 0, 0x00000000U, 11, 34},
        {"0x",
         0x0000000000000000ULL, 1, 0, 0x00000000U, 1, 0},
        {"0X",
         0x0000000000000000ULL, 1, 0, 0x00000000U, 1, 0},
        {"0xp3",
         0x0000000000000000ULL, 1, 0, 0x00000000U, 1, 0},
        {"0x.p1",
         0x0000000000000000ULL, 1, 0, 0x00000000U, 1, 0},
        {"0x1",
         0x3ff0000000000000ULL, 3, 0, 0x3f800000U, 3, 0},
        {"0x1.8",
         0x3ff8000000000000ULL, 5, 0, 0x3fc00000U, 5, 0},
        {"",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {" ",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"+",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"-",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {".",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"e5",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {".e3",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"+.e-3",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"1e",
         0x3ff0000000000000ULL, 1, 0, 0x3f800000U, 1, 0},
        {"1e+",
         0x3ff0000000000000ULL, 1, 0, 0x3f800000U, 1, 0},
        {"1e-",
         0x3ff0000000000000ULL, 1, 0, 0x3f800000U, 1, 0},
        {"1.5e",
         0x3ff8000000000000ULL, 3, 0, 0x3fc00000U, 3, 0},
        {"1.5e+x",
         0x3ff8000000000000ULL, 3, 0, 0x3fc00000U, 3, 0},
        {"  \t\n\r\f\v12.5xyz",
         0x4029000000000000ULL, 11, 0, 0x41480000U, 11, 0},
        {"--1",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {"+-1",
         0x0000000000000000ULL, 0, 0, 0x00000000U, 0, 0},
        {".5",
         0x3fe0000000000000ULL, 2, 0, 0x3f000000U, 2, 0},
        {"5.",
         0x4014000000000000ULL, 2, 0, 0x40a00000U, 2, 0},
        {"+.5",
         0x3fe0000000000000ULL, 3, 0, 0x3f000000U, 3, 0},
        {"-.5",
         0xbfe0000000000000ULL, 3, 0, 0xbf000000U, 3, 0},
        {"00000000000000000000000000000000000001",
         0x3ff0000000000000ULL, 38, 0, 0x3f800000U, 38, 0},
        {"0.00000000000000000000000000000000000001",
         0x380b38fb9daa78e4ULL, 40, 0, 0x006ce3eeU, 40, 34},
        {"000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001",
         0x3ff0000000000000ULL, 351, 0, 0x3f800000U, 351, 0},
        {"1.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001",
         0x3ff0000000000000ULL, 353, 0, 0x3f800000U, 353, 0},
        {"1e1000000000",
         0x7ff0000000000000ULL, 12, 34, 0x7f800000U, 12, 34},
        {"1e-1000000000",
         0x0000000000000000ULL, 13, 34, 0x00000000U, 13, 34},
        {"1e2147483647",
         0x7ff0000000000000ULL, 12, 34, 0x7f800000U, 12, 34},
        {"1e-2147483647",
         0x0000000000000000ULL, 13, 34, 0x00000000U, 13, 34},
        {"1e99999999999999999999",
         0x7ff0000000000000ULL, 22, 34, 0x7f800000U, 22, 34},
        {"1e-99999999999999999999",
         0x0000000000000000ULL, 23, 34, 0x00000000U, 23, 34},
        {"2.2250738585072012e-308",
         0x0010000000000000ULL, 23, 34, 0x00000000U, 23, 34},
        {"1e-45",
         0x3696d601ad376ab9ULL, 5, 0, 0x00000001U, 5, 34},
        {"1.4e-45",
         0x369ff868bf4d956aULL, 7, 0, 0x00000001U, 7, 34},
        {"3.4028235e38",
         0x47efffffe54daff8ULL, 12, 0, 0x7f7fffffU, 12, 0},
        {"3.4028236e38",
         0x47effffff514a7bcULL, 12, 0, 0x7f800000U, 12, 34},
        {"1.1754944e-38",
         0x381000000b3aeeabULL, 13, 0, 0x00800000U, 13, 0},
        {"1.1754942e-38",
         0x380fffffbb1dd6a1ULL, 13, 0, 0x007fffffU, 13, 34},
        {"5.877472e-39",
         0x380000000b3aeeabULL, 12, 0, 0x00400000U, 12, 34},
        {"16777217",
         0x4170000010000000ULL, 8, 0, 0x4b800000U, 8, 0},
        {"16777216",
         0x4170000000000000ULL, 8, 0, 0x4b800000U, 8, 0},
        {"33554433",
         0x4180000008000000ULL, 8, 0, 0x4c000000U, 8, 0},
        {"2.49113510e7",
         0x4177c1df70000000ULL, 12, 0, 0x4bbe0efcU, 12, 0},
        {"3.99578460e7",
         0x41830daab0000000ULL, 12, 0, 0x4c186d56U, 12, 0},
        {"1.19209289550781250e-7",
         0x3e80000000000000ULL, 22, 0, 0x34000000U, 22, 0},
};

/*
        The integer half, which is the half that had no declarations.

        Every call below was a compile error in this tree until numbers.c
        declared the name, and the symbol each one resolves to has been in the
        built object all along.
*/
number_case(integers)
{
        string_address stop;

        number_note(abs(-5) == 5, text("abs"), (positive)abs(-5), 5);
        number_note(abs(0) == 0, text("abs zero"), (positive)abs(0), 0);
        number_note(labs(-9223372036854775807LL) == 9223372036854775807LL,
                    text("labs"), (positive)labs(-9223372036854775807LL),
                    9223372036854775807ULL);
        number_note(llabs(-1) == 1, text("llabs"), (positive)llabs(-1), 1);
        number_note(imaxabs(-42) == 42, text("imaxabs"), (positive)imaxabs(-42), 42);

        number_note(atoi(text("42")) == 42, text("atoi"),
                    (positive)atoi(text("42")), 42);
        number_note(atoi(text("  -17abc")) == -17, text("atoi signed"),
                    (positive)(bipolar)atoi(text("  -17abc")), (positive)(bipolar)-17);
        number_note(atoi(text("xyz")) == 0, text("atoi none"),
                    (positive)atoi(text("xyz")), 0);
        number_note(atol(text("9223372036854775807")) == 9223372036854775807LL,
                    text("atol"), (positive)atol(text("9223372036854775807")),
                    9223372036854775807ULL);
        number_note(atoll(text("-1")) == -1, text("atoll"),
                    (positive)atoll(text("-1")), (positive)-1);

        stop = null;
        number_note(strtol(text("  -0x1fzz"), (char address_to address_to)address_of stop, 0) == -31,
                    text("strtol base zero"),
                    (positive)(bipolar)strtol(text("  -0x1fzz"), null, 0),
                    (positive)(bipolar)-31);
        number_say(stop != null && address_to stop == 'z', text("strtol endptr"));

        stop = null;
        number_note(strtol(text("0x"), (char address_to address_to)address_of stop, 0) == 0,
                    text("strtol bare prefix"), 0, 0);
        number_say(stop != null && address_to stop == 'x',
                   text("strtol bare prefix endptr"));

        stop = null;
        number_note(strtol(text("zzz"), (char address_to address_to)address_of stop, 10) == 0,
                    text("strtol no conversion"), 0, 0);
        number_say(stop != null && address_to stop == 'z',
                   text("strtol no conversion endptr"));

        number_note(strtoll(text("777"), null, 8) == 511, text("strtoll octal"),
                    (positive)strtoll(text("777"), null, 8), 511);
        number_note(strtoul(text("-1"), null, 10) == 18446744073709551615ULL,
                    text("strtoul wrap"), strtoul(text("-1"), null, 10),
                    18446744073709551615ULL);
        number_note(strtoull(text("ffffffffffffffff"), null, 16) ==
                            18446744073709551615ULL,
                    text("strtoull hex"),
                    strtoull(text("ffffffffffffffff"), null, 16),
                    18446744073709551615ULL);
        number_note(strtoimax(text("-1234"), null, 10) == -1234,
                    text("strtoimax"),
                    (positive)strtoimax(text("-1234"), null, 10),
                    (positive)(bipolar)-1234);
        number_note(strtoumax(text("1234"), null, 10) == 1234, text("strtoumax"),
                    strtoumax(text("1234"), null, 10), 1234);

        //      The addresses have to be takeable, which a macro alias could
        //      not give and an undeclared name could not give either.
        number_say((address_any)address_of strtol != null &&
                           (address_any)address_of strtod != null,
                   text("addresses of the names"));

        return number_failures == 0;
}

/*
        The baked table, three answers to each line: the bits, where the scan
        stopped, and what errno the call left behind.
*/
number_case(against_glibc)
{
        positive index;
        positive total = sizeof number_float_cases / sizeof number_float_cases[0];

        for (index = 0; index < total; index++)
        {
                const number_float_case address_to entry = address_of number_float_cases[index];
                string_address input = (string_address)entry->text;
                string_address stop;
                number_wide_shape wide;
                number_narrow_shape narrow;
                b32 seen;

                errno = 0;
                stop = null;
                wide.value = strtod(input, (char address_to address_to)address_of stop);
                seen = errno;

                number_note(wide.bits == entry->wide_bits, input, wide.bits,
                            entry->wide_bits);
                number_note((positive)(stop - input) == entry->wide_end, input,
                            (positive)(stop - input), entry->wide_end);
                number_note(seen == entry->wide_errno, input, (positive)seen,
                            (positive)entry->wide_errno);

                errno = 0;
                stop = null;
                narrow.value = strtof(input, (char address_to address_to)address_of stop);
                seen = errno;

                number_note(narrow.bits == entry->narrow_bits, input,
                            narrow.bits, entry->narrow_bits);
                number_note((positive)(stop - input) == entry->narrow_end, input,
                            (positive)(stop - input), entry->narrow_end);
                number_note(seen == entry->narrow_errno, input, (positive)seen,
                            (positive)entry->narrow_errno);

                //      The three conversions share one parser, so wherever
                //      they stop they stop together.
                stop = null;
                strtold(input, (char address_to address_to)address_of stop);
                number_note((positive)(stop - input) == entry->wide_end, input,
                            (positive)(stop - input), entry->wide_end);
        }

        return number_failures == 0;
}

/*
        long double, without a reference and without arithmetic.

        There is no baked table here because there could not be one that ran
        everywhere: long double is the eighty bit x87 format on x86_64 and
        binary128 on arm64 and riscv64, so the bits are different answers to
        the same question. What is architecture independent is that a decimal
        which is exactly a double must be exactly the same number as a long
        double, and the bits of that wider number can be built out of the bits
        of the narrower one -- shift the significand up, rebias the exponent,
        carry the sign. So the check is a conversion this file computes
        against a conversion the library computed, in whichever of the two
        formats the machine has.

        Nothing here compares two long doubles with an operator. A -nostdlib
        link has no libgcc under it and a single binary128 comparison is a
        call to __letf2 that would not resolve; every comparison below is
        between integers taken out of a union.

        And on x86_64 only ten of the sixteen bytes of a long double mean
        anything. The other six are padding the ABI never defines -- a long
        double comes back from a call in an x87 register and is stored into
        whatever the caller's stack slot held -- so the comparison masks down
        to the eighty bits that exist. Comparing all sixteen bytes was the
        first version of this and it failed on every line, including on zero.
*/
static bool number_extended_same(p128 left, p128 right)
{
#if __LDBL_MANT_DIG__ == 64
        return (p64)left == (p64)right &&
               (((p64)(left >> 64)) & 0xffff) == (((p64)(right >> 64)) & 0xffff);
#else
        return left == right;
#endif
}

static p128 number_widened(p64 wide_bits)
{
        p64 sign = wide_bits >> 63;
        p64 field = (wide_bits >> 52) & 0x7ff;
        p64 fraction = wide_bits & 0xfffffffffffffULL;
        p128 bits;

        if (field == 0 && fraction == 0)
#if __LDBL_MANT_DIG__ == 64
                return (p128)sign << 79;
#else
                return (p128)sign << 127;
#endif

        //      A saturated exponent stays saturated rather than being
        //      rebiased: an infinity is an infinity in both formats and its
        //      field is all ones in both.
        {
                p64 wider = field == 0x7ff ? 0x7fff : (field - 1023 + 16383) & 0x7fff;

#if __LDBL_MANT_DIG__ == 64
                bits = (p128)(((p64)1 << 63) | (fraction << 11));
                bits |= (p128)wider << 64;
                bits |= (p128)sign << 79;
#else
                bits = (p128)fraction << 60;
                bits |= (p128)wider << 112;
                bits |= (p128)sign << 127;
#endif
        }

        return bits;
}

number_case(extended)
{
        //      Every one of these is a dyadic rational small enough to be a
        //      double exactly, which is what makes the widening the right
        //      answer. A decimal that is only NEAR a double -- 0.1, or
        //      1e100 -- is a different number in the two formats, and
        //      comparing them would be comparing two correct answers.
        static const char address_to exactly[] = {
                "0", "-0", "1", "-1", "2", "0.5", "-0.5", "1.5", "0.25",
                "1024", "-4096.125", "3", "7.75", "1e10", "-1e10", "65536",
                "9007199254740992", "-9007199254740992", "0.00390625",
                "1e15", "-1e15", "1048576.5", "255.9375", "1e0", "8388608"};
        positive index;
        positive total = sizeof exactly / sizeof exactly[0];
        number_extended_shape extended;
        number_wide_shape wide;

        for (index = 0; index < total; index++)
        {
                string_address input = (string_address)exactly[index];

                wide.value = strtod(input, null);
                extended.value = strtold(input, null);

                number_say(number_extended_same(extended.bits,
                                                number_widened(wide.bits)),
                           input);
        }

        //      The specials, in whichever format the machine has.
        extended.value = strtold(text("inf"), null);
        number_say(number_extended_same(extended.bits,
                                        number_widened(0x7ff0000000000000ULL)),
                   text("strtold inf"));

        extended.value = strtold(text("-inf"), null);
        number_say(number_extended_same(extended.bits,
                                        number_widened(0xfff0000000000000ULL)),
                   text("strtold -inf"));

        //      A NaN is not the widening of a NaN, so this asks the two
        //      questions a NaN has to answer: saturated exponent, and a
        //      significand that is not zero.
        extended.value = strtold(text("nan"), null);
#if __LDBL_MANT_DIG__ == 64
        number_say(((p64)(extended.bits >> 64) & 0x7fff) == 0x7fff &&
                           (p64)extended.bits != 0,
                   text("strtold nan"));
#else
        number_say(((p64)(extended.bits >> 112) & 0x7fff) == 0x7fff &&
                           (extended.bits & ((((p128)1 << 112) - 1))) != 0,
                   text("strtold nan"));
#endif

        return number_failures == 0;
}

/*
        The shift table, checked against the one entry of it that cannot be
        wrong.

        numbers_shift_left multiplies the decimal register by two to the k in
        a single pass, and to write from the right it must know beforehand how
        many digits the answer will gain. That count comes from a table, and a
        table is exactly the kind of thing that is right for fifty nine
        entries and wrong for one. Shifting by one, sixty times, uses only the
        first entry and reaches the same number by a different road, so the
        two roads are compared -- for every k, on registers built from real
        digits rather than from a single one.
*/
number_case(shift_table)
{
        static const char address_to seeds[] = {
                "1", "9", "1234567890", "5", "4999999999", "5000000001",
                "999999999999999999999", "1000000000000000000001",
                "31415926535897932384626433832795", "7", "12", "8888888888"};
        positive which;
        positive total = sizeof seeds / sizeof seeds[0];
        b32 places;

        for (which = 0; which < total; which++)
        {
                for (places = 1; places <= NUMBERS_SHIFT_MAX; places++)
                {
                        numbers_scan once;
                        numbers_scan step;
                        b32 count;

                        numbers_read((string_address)seeds[which], address_of once);
                        memory_copy(address_of step, address_of once, sizeof step);

                        numbers_shift_left(address_of once, places);

                        for (count = 0; count < places; count++)
                                numbers_shift_left(address_of step, 1);

                        number_say(once.count == step.count &&
                                           once.point == step.point &&
                                           memory_compare(once.digits, step.digits,
                                                          (positive)once.count) == 0,
                                   (string_address)seeds[which]);
                }
        }

        return number_failures == 0;
}

/*
        A generator, and the three tiers against each other.

        numbers_read and numbers_assemble are static in the translation unit
        this file compiles into, so the decimal register can be driven
        directly and its answer compared against whatever tier strtod chose.
        The register is exact by construction; a disagreement is a defect in
        the exact tier or in the estimating one, and it is found here rather
        than by a reference library that this program does not have.
*/
static p64 number_seed = 0x243f6a8885a308d3ULL;

static p64 number_random(void)
{
        p64 mixed = (number_seed += 0x9e3779b97f4a7c15ULL);

        mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;

        return mixed ^ (mixed >> 31);
}

static p8 number_text[128];

//      A decimal with the given number of significant digits and the given
//      power of ten, written the way a program writes one. positive_into and
//      bipolar_into do the two numeric parts; only the random digits in the
//      middle are a loop, because there is no routine that invents digits.
static string_address number_compose(b32 digits, b32 exponent)
{
        positive at = 0;
        b32 index;

        for (index = 0; index < digits; index++)
        {
                p64 digit = number_random() % 10;

                if (index == 0)
                        digit = 1 + number_random() % 9;

                number_text[at] = (p8)('0' + digit);
                at++;

                if (index == 0)
                {
                        number_text[at] = '.';
                        at++;
                }
        }

        number_text[at] = 'e';
        at++;
        at += bipolar_into_string(number_text + at, exponent);

        return number_text;
}

static bool number_register_answer(string_address input, p64 address_to bits)
{
        numbers_scan scan;
        b32 condition = 0;

        numbers_read(input, address_of scan);

        if (scan.kind != NUMBERS_NUMBER || scan.hexadecimal)
                return false;

        address_to bits = (p64)numbers_assemble(address_of scan,
                                                address_of numbers_binary64,
                                                address_of condition);

        return true;
}

static bool number_register_narrow(string_address input, p32 address_to bits)
{
        numbers_scan scan;
        b32 condition = 0;

        numbers_read(input, address_of scan);

        if (scan.kind != NUMBERS_NUMBER || scan.hexadecimal)
                return false;

        address_to bits = (p32)numbers_assemble(address_of scan,
                                                address_of numbers_binary32,
                                                address_of condition);

        return true;
}

#ifndef NUMBER_SWEEP
#define NUMBER_SWEEP 120000
#endif

number_case(tiers_agree)
{
        b32 round;

        for (round = 0; round < NUMBER_SWEEP; round++)
        {
                b32 digits = 1 + (b32)(number_random() % 25);
                b32 exponent = (b32)(number_random() % 700) - 350;
                string_address input = number_compose(digits, exponent);
                number_wide_shape wide;
                number_narrow_shape narrow;
                p64 slow_wide;
                p32 slow_narrow;

                wide.value = strtod(input, null);
                narrow.value = strtof(input, null);

                if (number_register_answer(input, address_of slow_wide))
                        number_note(wide.bits == slow_wide, input, wide.bits,
                                    slow_wide);

                if (number_register_narrow(input, address_of slow_narrow))
                        number_note(narrow.bits == slow_narrow, input,
                                    narrow.bits, slow_narrow);
        }

        return number_failures == 0;
}

/*
        Monotonicity, which is the property a conversion can lose one input at
        a time without any single answer looking wrong.

        A decimal that is larger must never convert to a float that is
        smaller. The pairs are a significand and the same significand plus
        one, at the same power of ten, across the whole exponent range
        including both ends of it, so the pairs that straddle a rounding
        boundary are the common case rather than the rare one.
*/
number_case(monotone)
{
        b32 round;

        for (round = 0; round < NUMBER_SWEEP / 2; round++)
        {
                p64 significand = number_random() % 10000000000000000ULL;
                b32 exponent = (b32)(number_random() % 700) - 350;
                positive at;
                number_wide_shape low;
                number_wide_shape high;

                at = positive_into(number_text, significand);
                number_text[at] = 'e';
                at++;
                at += bipolar_into_string(number_text + at, exponent);
                low.value = strtod(number_text, null);

                at = positive_into(number_text, significand + 1);
                number_text[at] = 'e';
                at++;
                at += bipolar_into_string(number_text + at, exponent);
                high.value = strtod(number_text, null);

                number_note(low.bits <= high.bits, text("monotone"), low.bits,
                            high.bits);
        }

        return number_failures == 0;
}

/*
        The small integers, exhaustively, in three spellings each.

        The compiler converted the same numbers when it built this file, so
        the comparison is against a second implementation of the same
        conversion rather than against a table -- and the three spellings put
        the same value through the integer path, the fraction path and the
        leading-zero path.
*/
number_case(small_integers)
{
        p64 value;

        for (value = 0; value < 100000; value++)
        {
                positive at = positive_into_string(number_text, value);
                number_wide_shape shape;
                number_narrow_shape narrow;

                shape.value = strtod(number_text, null);
                narrow.value = strtof(number_text, null);

                number_note(shape.value == (decimal)value, number_text,
                            (positive)shape.bits, value);
                number_note(narrow.value == (f32)value, number_text,
                            (positive)narrow.bits, value);

                number_text[at] = '.';
                number_text[at + 1] = '0';
                number_text[at + 2] = 0;
                shape.value = strtod(number_text, null);
                number_note(shape.value == (decimal)value, number_text,
                            (positive)shape.bits, value);

                //      The same number written as a fraction of ten, which
                //      takes the scaling path rather than the integer one.
                at = positive_into(number_text + 1, value);
                number_text[0] = '.';
                number_text[at + 1] = 'e';
                number_text[at + 2] = 0;
                at = positive_into_string(number_text + at + 2, at);
                shape.value = strtod(number_text, null);
                number_note(shape.value == (decimal)value, number_text,
                            (positive)shape.bits, value);
        }

        return number_failures == 0;
}

typedef struct
{
        const char address_to name;
        bool(address_to run)(void);
} number_entry;

static const number_entry number_entries[] = {
        {"integers", number_test_integers},
        {"against glibc", number_test_against_glibc},
        {"extended", number_test_extended},
        {"shift table", number_test_shift_table},
        {"tiers agree", number_test_tiers_agree},
        {"monotone", number_test_monotone},
        {"small integers", number_test_small_integers},
        {null, null}};

b32 main(void)
{
        const number_entry address_to entry = number_entries;

        log_direct(str("number tests\n\n"));

        while (entry->name)
        {
                positive before = number_failures;

                log_direct((string_address)entry->name,
                           string_length((string_address)entry->name));
                entry->run();

                if (number_failures == before)
                        log_direct(str(" PASSED\n"));
                else
                        log_direct(str(" ----- FAILED\n"));

                entry++;
        }

        string_format(log, "\n%p checks, %p failures\n", number_checks,
                      number_failures);
        log_flush();

        return number_failures > 0 ? 1 : 0;
}
