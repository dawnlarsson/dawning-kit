/*
        Contiguous fixed-width decimal fields against the C bodies they fold:
        a direct two-digit pair and the former scale/divide loops for six and
        nine places. Values stay inside each caller's proven field domain.

        Native results are hardware time. Qemu results are emulator work only.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define VALUE_COUNT 64
#define ROUNDS (1u << 17)
#define TRIES 7

static p8 output[24];
static positive values[VALUE_COUNT];
static volatile positive sink;

NOT_INLINED positive former_pair(p8 address_to into, positive value)
{
        into[0] = (p8)('0' + (value / 10) % 10);
        into[1] = (p8)('0' + value % 10);
        return 2;
}

NOT_INLINED positive former_scaled(p8 address_to into, positive value,
                                   positive scale)
{
        positive length = 0;

        while (scale)
        {
                into[length++] = (p8)('0' + (value / scale) % 10);
                scale /= 10;
        }

        return length;
}

static fn make_values()
{
        positive state = 0x9e3779b97f4a7c15ull;

        for (positive i = 0; i < VALUE_COUNT; i++)
        {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                values[i] = state;
        }

        values[0] = 0;
        values[1] = 1;
        values[2] = 9;
        values[3] = 10;
        values[4] = 99;
        values[5] = 999999;
        values[6] = 999999999;
}

static p64 former_once(positive width)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
        {
                positive value = values[r & (VALUE_COUNT - 1)];
                positive length;

                if (width == 2)
                        length = former_pair(output, value % 100);
                else if (width == 6)
                        length = former_scaled(output, value % 1000000, 100000);
                else
                        length = former_scaled(output, value % 1000000000, 100000000);

                sink += length + output[0] + output[width - 1];
        }

        return get_cpu_time() - start;
}

static p64 assembly_once(positive width)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
        {
                positive value = values[r & (VALUE_COUNT - 1)];
                positive length;

                if (width == 2)
                {
                        value %= 100;
                        length = positive_into_pair(output, value);
                }
                else if (width == 6)
                {
                        value %= 1000000;
                        length = positive_into_padded(output, value, width, '0');
                }
                else
                {
                        value %= 1000000000;
                        length = positive_into_padded(output, value, width, '0');
                }

                sink += length + output[0] + output[width - 1];
        }

        return get_cpu_time() - start;
}

static fn row(string_address name, positive width)
{
        p64 former = positive_max;
        p64 assembly = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 got_former;
                p64 got_assembly;

                if (t & 1)
                {
                        got_assembly = assembly_once(width);
                        got_former = former_once(width);
                }
                else
                {
                        got_former = former_once(width);
                        got_assembly = assembly_once(width);
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

        string_format(log, "contiguous padded decimal, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"width 2", 2);
        row((string_address)"width 6", 6);
        row((string_address)"width 9", 9);
        log_flush();
        return 0;
}
