/*
        Decimal conversion at the call shapes the library actually exposes.

        The reference is the scalar divide-by-ten/scratch/copy loop that the
        callers used before positive_into folded it.  The assembly column is
        positive_into itself.  positive_to_string is shown separately because
        its writer call is part of that API and therefore not directly
        comparable with a buffer writer, but it is the existing chunked
        implementation against which a new shared digit core must be checked.

        A native run reports hardware ticks.  A qemu run reports emulator
        work: useful as a regression ratio, never as a hardware-floor claim.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define VALUE_COUNT 64
#define ROUNDS (1u << 17)
#define TRIES 7

static p8 output[64];
static volatile positive sink;

NOT_INLINED positive scalar_into(p8 address_to into, positive value)
{
        p8 scratch[24];
        positive have = 0;

        do
        {
                scratch[have++] = (p8)('0' + value % 10);
                value /= 10;
        } while (value);

        for (positive i = 0; i < have; i++)
                into[i] = scratch[have - i - 1];

        return have;
}

NOT_INLINED fn discard_writer(address_any data, positive length)
{
        volatile p8 address_to bytes = (volatile p8 address_to)data;

        sink += length + bytes[0] + bytes[length - 1];
}

static positive small_values[VALUE_COUNT];
static positive mixed_values[VALUE_COUNT];
static positive wide_values[VALUE_COUNT];

static fn make_values()
{
        positive state = 0x9e3779b97f4a7c15ull;

        for (positive i = 0; i < VALUE_COUNT; i++)
        {
                small_values[i] = i < 32 ? i : (i * 313u) % 10000u;

                positive digits = i % 20 + 1;
                positive base = 1;
                for (positive d = 1; d < digits; d++)
                        base *= 10;
                mixed_values[i] = base + (i * 7919u) % base;

                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                wide_values[i] = state | (1ull << 63);
        }

        /* Exact boundaries and both ends keep branch wins honest. */
        small_values[0] = 0;
        small_values[1] = 9;
        small_values[2] = 10;
        small_values[3] = 99;
        small_values[4] = 100;
        small_values[5] = 9999;
        mixed_values[0] = 99999999ull;
        mixed_values[1] = 100000000ull;
        mixed_values[2] = 9999999999999999ull;
        mixed_values[3] = 10000000000000000ull;
        wide_values[0] = positive_max;
        wide_values[1] = 10000000000000000000ull;
}

static p64 scalar_run(positive address_to values)
{
        p64 best = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 start = get_cpu_time();
                for (positive r = 0; r < ROUNDS; r++)
                        sink += scalar_into(output, values[r & (VALUE_COUNT - 1)]);
                p64 took = get_cpu_time() - start;
                if (took < best)
                        best = took;
        }

        return best;
}

static p64 assembly_run(positive address_to values)
{
        p64 best = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 start = get_cpu_time();
                for (positive r = 0; r < ROUNDS; r++)
                        sink += positive_into(output,
                                              values[r & (VALUE_COUNT - 1)]);
                p64 took = get_cpu_time() - start;
                if (took < best)
                        best = took;
        }

        return best;
}

static p64 writer_run(positive address_to values)
{
        p64 best = positive_max;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 start = get_cpu_time();
                for (positive r = 0; r < ROUNDS; r++)
                        positive_to_string(discard_writer,
                                           values[r & (VALUE_COUNT - 1)]);
                p64 took = get_cpu_time() - start;
                if (took < best)
                        best = took;
        }

        return best;
}

static fn row(string_address name, positive address_to values)
{
        p64 scalar = scalar_run(values);
        p64 assembly = assembly_run(values);
        p64 writer = writer_run(values);

        string_format(log, "  %s: scalar %p  into %p  chunked-writer %p"
                           "  into/scalar %p%%\n",
                      name, (positive)scalar, (positive)assembly,
                      (positive)writer,
                      (positive)(assembly * 100 / (scalar ? scalar : 1)));
}

b32 main()
{
        make_values();
        moonwater_cpu_detect();

        string_format(log, "decimal conversion, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"small 1-4 digit", small_values);
        row((string_address)"mixed 1-20 digit", mixed_values);
        row((string_address)"wide 19-20 digit", wide_values);
        log_flush();
        return 0;
}
