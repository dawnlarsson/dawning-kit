/* dd's nearest human formatter: exact former C body against the ASM leaf. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define ROUNDS (1u << 18)
#define VALUE_COUNT 64
#define TRIES 7

static p8 output[9];
static volatile positive sink;

// Verbatim policy removed from tools.c. Keeping the whole body -- including
// decimal emission and suffix construction -- makes this a body-to-body
// measurement rather than a scaling-loop proxy.
NOT_INLINED positive former_dd_human(p8 address_to into, positive n, bool binary)
{
        positive base = binary ? 1024 : 1000;
        positive amount = n;
        positive tenths = 0;
        positive rounding = 0;
        positive exponent = 0;
        p8 letters[11] = {0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y', 'R', 'Q'};
        p8 digits[24];
        positive have = 0;
        positive used = 0;
        positive fraction = 0;
        bool point = false;

        if (base <= amount)
        {
                do
                {
                        positive ten = (amount % base) * 10 + tenths;
                        positive two = (ten % base) * 2 + (rounding >> 1);

                        amount /= base;
                        tenths = ten / base;
                        rounding = two < base ? ((two + rounding) != 0)
                                              : 2 + (base < two + rounding);
                        exponent++;
                }
                while (base <= amount && exponent < 10);

                if (amount < 10)
                {
                        if (2 < rounding + (tenths & 1))
                        {
                                tenths++;
                                rounding = 0;
                                if (tenths == 10)
                                {
                                        amount++;
                                        tenths = 0;
                                }
                        }

                        if (amount < 10)
                        {
                                point = true;
                                fraction = tenths;
                                tenths = rounding = 0;
                        }
                }
        }

        if (5 < tenths + (0 < rounding + (amount & 1)))
        {
                amount++;
                if (amount == base && exponent < 10)
                {
                        exponent++;
                        point = true;
                        fraction = 0;
                        amount = 1;
                }
        }

        if (!amount)
                digits[have++] = '0';

        while (amount)
        {
                digits[have++] = (p8)('0' + amount % 10);
                amount /= 10;
        }

        while (have)
                into[used++] = digits[--have];

        if (point)
        {
                into[used++] = '.';
                into[used++] = (p8)('0' + fraction);
        }
        into[used++] = ' ';
        if (exponent)
                into[used++] = !binary && exponent == 1 ? 'k' : letters[exponent];
        if (binary && exponent)
                into[used++] = 'i';
        into[used++] = 'B';
        into[used] = end;
        return used;
}

static positive values[2][3][VALUE_COUNT];

static fn make_values()
{
        positive state = 0x9e3779b97f4a7c15ull;

        for (positive binary = 0; binary < 2; binary++)
        {
                positive base = binary ? 1024 : 1000;
                for (positive i = 0; i < VALUE_COUNT; i++)
                {
                        state ^= state << 13;
                        state ^= state >> 7;
                        state ^= state << 17;
                        values[binary][0][i] = state % base;
                        values[binary][1][i] = base + state % (8 * base);
                        values[binary][2][i] = state;
                }

                values[binary][0][0] = 0;
                values[binary][0][1] = base - 1;
                values[binary][1][0] = base;
                values[binary][1][1] = 9 * base - 1;
                values[binary][2][0] = positive_max;
                values[binary][2][1] = binary ? 1023 * 1024 : 999999;
        }
}

static p64 run_once(bool assembly, bool binary, positive shape)
{
        p64 start = get_cpu_time();
        for (positive i = 0; i < ROUNDS; i++)
        {
                positive value = values[binary][shape][i & (VALUE_COUNT - 1)];
                positive length = assembly
                                      ? positive_into_human_nearest_string(output, value,
                                                                            binary)
                                      : former_dd_human(output, value, binary);
                sink += length + output[0] + output[length - 1];
        }
        return get_cpu_time() - start;
}

static fn row(string_address name, bool binary, positive shape)
{
        p64 former = positive_max;
        p64 assembly = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 got_former, got_assembly;
                if (t & 1)
                {
                        got_assembly = run_once(true, binary, shape);
                        got_former = run_once(false, binary, shape);
                }
                else
                {
                        got_former = run_once(false, binary, shape);
                        got_assembly = run_once(true, binary, shape);
                }

                if (got_former < former)
                        former = got_former;
                if (got_assembly < assembly)
                        assembly = got_assembly;
        }

        string_format(log, "  %s: former-C %p  assembly %p  asm/C %p%%\n",
                      name, (positive)former, (positive)assembly,
                      (positive)(assembly * 100 / (former ? former : 1)));
}

b32 main()
{
        make_values();
        moonwater_cpu_detect();

        string_format(log, "nearest human sizes, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"SI plain", false, 0);
        row((string_address)"SI fractional", false, 1);
        row((string_address)"SI mixed u64", false, 2);
        row((string_address)"IEC plain", true, 0);
        row((string_address)"IEC fractional", true, 1);
        row((string_address)"IEC mixed u64", true, 2);
        log_flush();
        return 0;
}
