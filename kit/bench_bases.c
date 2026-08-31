/*
        Buffer conversion in non-decimal bases.

        The reference is printf_render's former C body: a runtime-base divide
        loop into a scratch buffer, followed by a backwards copy. The assembly
        is positive_into_base. Decimal has its own deeper benchmark in
        bench_numbers.c; this one measures every power-of-two lane from two
        through thirty two and the generic base-36 lane.

        Native runs are hardware ticks. Qemu runs are emulator work and only
        useful as regression ratios, never as hardware-floor measurements.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define VALUE_COUNT 64
#define ROUNDS (1u << 17)
#define TRIES 7

static p8 output[64];
static positive values[VALUE_COUNT];
static volatile positive sink;

NOT_INLINED positive scalar_base(p8 address_to into, positive value,
                                 positive base, bool upper)
{
        p8 scratch[64];
        positive have = 0;

        do
        {
                positive digit = value % base;

                scratch[have++] =
                    (p8)(digit < 10 ? '0' + digit
                                    : (upper ? 'A' : 'a') + digit - 10);
                value /= base;
        } while (value);

        for (positive i = 0; i < have; i++)
                into[i] = scratch[have - i - 1];

        return have;
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
        values[1] = 7;
        values[2] = 8;
        values[3] = 15;
        values[4] = 16;
        values[5] = 07777;
        values[6] = ~(positive)0;
}

static p64 scalar_run(positive base, bool upper)
{
        p64 best = ~(positive)0;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 start = get_cpu_time();
                for (positive r = 0; r < ROUNDS; r++)
                        sink += scalar_base(output,
                                            values[r & (VALUE_COUNT - 1)],
                                            base, upper);
                p64 took = get_cpu_time() - start;
                if (took < best)
                        best = took;
        }

        return best;
}

static p64 assembly_run(positive base, bool upper)
{
        p64 best = ~(positive)0;

        for (positive t = 0; t < TRIES; t++)
        {
                p64 start = get_cpu_time();
                for (positive r = 0; r < ROUNDS; r++)
                        sink += positive_into_base(
                            output, values[r & (VALUE_COUNT - 1)], base, upper);
                p64 took = get_cpu_time() - start;
                if (took < best)
                        best = took;
        }

        return best;
}

static fn row(string_address name, positive base, bool upper)
{
        p64 scalar = scalar_run(base, upper);
        p64 assembly = assembly_run(base, upper);

        string_format(log, "  %s: scalar %p  assembly %p  assembly/scalar %p%%\n",
                      name, (positive)scalar, (positive)assembly,
                      (positive)(assembly * 100 / (scalar ? scalar : 1)));
}

b32 main()
{
        make_values();
        moonwater_cpu_detect();

        string_format(log, "base conversion, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"binary", 2, false);
        row((string_address)"base 4", 4, false);
        row((string_address)"octal", 8, false);
        row((string_address)"hexadecimal lower", 16, false);
        row((string_address)"hexadecimal upper", 16, true);
        row((string_address)"base 32 lower", 32, false);
        row((string_address)"base 32 upper", 32, true);
        row((string_address)"base 36 lower", 36, false);
        log_flush();
        return 0;
}
