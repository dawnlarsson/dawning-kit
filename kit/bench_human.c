/*
        Compact binary-size formatting at both public call shapes.

        The former column is the overflow-safe C policy removed from file.c:
        a power-of-1024 walk and upward rounding around the existing decimal
        primitives. The assembly column is the shared buffer or writer API.
        Both writer forms preserve the same callback boundaries.

        Native runs are hardware ticks. Qemu ratios are emulator work only.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define VALUE_COUNT 64
#define ROUNDS (1u << 16)
#define TRIES 7

static p8 output[8];
static positive values[4][VALUE_COUNT];
static volatile positive sink;

NOT_INLINED positive former_human_buffer(p8 address_to into, positive value)
{
        p8 units[7] = "BKMGTPE";
        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 6)
        {
                divisor *= 1024;
                unit++;
        }

        if (!unit)
                return positive_into_string(into, value);

        positive quotient = value / divisor;
        positive remainder = value % divisor;
        positive length;

        if (quotient >= 10)
                length = positive_into(into, quotient + (remainder != 0));
        else
        {
                positive fraction = (remainder * 10 + divisor - 1) / divisor;

                if (fraction == 10)
                {
                        quotient++;
                        fraction = 0;
                }

                if (quotient >= 10)
                        length = positive_into(into, quotient);
                else
                {
                        length = positive_into(into, quotient);
                        into[length++] = '.';
                        length += positive_into(into + length, fraction);
                }
        }

        into[length++] = units[unit];
        into[length] = end;
        return length;
}

NOT_INLINED fn former_human_writer(writer write, positive value)
{
        p8 units[7] = "BKMGTPE";
        positive divisor = 1;
        positive unit = 0;

        while (value / divisor >= 1024 && unit < 6)
        {
                divisor *= 1024;
                unit++;
        }

        if (!unit)
                return positive_to_string(write, value);

        positive quotient = value / divisor;
        positive remainder = value % divisor;
        if (quotient >= 10)
        {
                positive_to_string(write, quotient + (remainder != 0));
                write(units + unit, 1);
                return;
        }

        positive fraction = (remainder * 10 + divisor - 1) / divisor;

        if (fraction == 10)
        {
                quotient++;
                fraction = 0;
        }

        positive_to_string(write, quotient);

        if (quotient < 10)
        {
                write(".", 1);
                positive_to_string(write, fraction);
        }

        write(units + unit, 1);
}

NOT_INLINED fn discard_writer(address_any data, positive length)
{
        volatile p8 address_to bytes = (volatile p8 address_to)data;

        sink += length + bytes[0] + bytes[length - 1];
}

static fn make_values()
{
        positive state = 0x9e3779b97f4a7c15ull;

        for (positive i = 0; i < VALUE_COUNT; i++)
        {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;

                values[0][i] = state & 1023;
                values[1][i] = 1024 + state % (8 * 1024);
                values[2][i] = 10 * 1024 + state % (1014 * 1024);
                values[3][i] = state;
        }

        values[0][0] = 0;
        values[0][1] = 1023;
        values[1][0] = 1024;
        values[1][1] = 9 * 1024;
        values[1][2] = 9 * 1024 + 1;
        values[2][0] = 1024 * 1024 - 1;
        values[2][1] = 1024 * 1024;
        values[3][0] = positive_max;
        values[3][1] = ((positive)15 << 60) + 1;
}

static p64 buffer_once(positive shape, bool assembly)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
                sink += assembly
                            ? positive_into_human_1024_string(
                                  output, values[shape][r & (VALUE_COUNT - 1)])
                            : former_human_buffer(
                                  output, values[shape][r & (VALUE_COUNT - 1)]);

        return get_cpu_time() - start;
}

static p64 writer_once(positive shape, bool assembly)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
                if (assembly)
                        positive_to_human_1024(
                            discard_writer, values[shape][r & (VALUE_COUNT - 1)]);
                else
                        former_human_writer(
                            discard_writer, values[shape][r & (VALUE_COUNT - 1)]);

        return get_cpu_time() - start;
}

static fn row(string_address name, positive shape, bool writer_form)
{
        p64 former = positive_max;
        p64 assembly = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 got_former;
                p64 got_assembly;

                if (t & 1)
                {
                        got_assembly = writer_form ? writer_once(shape, true)
                                                   : buffer_once(shape, true);
                        got_former = writer_form ? writer_once(shape, false)
                                                 : buffer_once(shape, false);
                }
                else
                {
                        got_former = writer_form ? writer_once(shape, false)
                                                 : buffer_once(shape, false);
                        got_assembly = writer_form ? writer_once(shape, true)
                                                   : buffer_once(shape, true);
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

        string_format(log, "compact binary sizes, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"buffer plain", 0, false);
        row((string_address)"buffer fractional", 1, false);
        row((string_address)"buffer integer", 2, false);
        row((string_address)"buffer mixed u64", 3, false);
        row((string_address)"writer plain", 0, true);
        row((string_address)"writer fractional", 1, true);
        row((string_address)"writer integer", 2, true);
        row((string_address)"writer mixed u64", 3, true);
        log_flush();
        return 0;
}
