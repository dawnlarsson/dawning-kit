/* Calendar text traffic and directive-parser costs. */
#include "../src/compiler_memory.c"

#define CLOCK_BENCH_ROUNDS (1u << 20)
#define CLOCK_BENCH_TRIES 5
#define CLOCK_FORMAT_DIRECTIVES 10
#define CLOCK_SCAN_DIRECTIVES 9

typedef fn (*clock_bench_work)();

static p8 clock_bench_output[256];
static p8 clock_bench_input[256];
static tm clock_bench_broken;
static volatile positive clock_bench_sink;

static const char address_to clock_bench_literal =
        "ordinary calendar text has no directives; this deliberately makes "
        "the byte traffic, rather than date arithmetic, the measured work.";

static const char address_to clock_bench_format =
        "%Y-%m-%dT%H:%M:%S %A %B %p %z";
static const char address_to clock_bench_scan_format =
        "%Y-%m-%dT%H:%M:%S %A %B %p";
static const char address_to clock_bench_scan_input =
        "2024-08-17T13:42:51 Saturday August PM";

static positive (*volatile clock_bench_strftime)(
        p8 address_to, positive, const char address_to, const tm address_to) =
        strftime;
static p8 address_to (*volatile clock_bench_strptime)(
        const char address_to, const char address_to, tm address_to) = strptime;
static p8 address_to (*volatile clock_bench_asctime)(
        const tm address_to, p8 address_to) = asctime_r;

static fn clock_bench_copy()
{
        positive length = string_length((string_address)clock_bench_literal);

        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
        {
                memory_copy_apart(clock_bench_output,
                                  (address_any)clock_bench_literal, length);
                clock_bench_sink += clock_bench_output[i & 63];
        }
}

static fn clock_bench_compare()
{
        positive length = string_length((string_address)clock_bench_literal);

        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink +=
                        (positive)memory_compare(clock_bench_literal,
                                                 clock_bench_input, length);
}

static fn clock_bench_scan_floor()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
        {
                const char address_to format = clock_bench_literal;
                const char address_to input = (const char address_to)clock_bench_input;

                while (address_to format != end)
                {
                        positive run;

                        if (byte_is_space((p8)(address_to format)))
                        {
                                format += string_span_of_set(
                                        (string_address)format, " \t\n\r\v\f");
                                input += string_span_of_set(
                                        (string_address)input, " \t\n\r\v\f");
                                continue;
                        }

                        run = string_span_without_set(
                                (string_address)format, "% \t\n\r\v\f");
                        clock_bench_sink += (positive)memory_compare(
                                (address_any)format, (address_any)input, run);
                        format += run;
                        input += run;
                }

                clock_bench_sink += (positive)input;
        }
}

static fn clock_bench_format_literal()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink += clock_bench_strftime(
                        clock_bench_output, sizeof(clock_bench_output),
                        clock_bench_literal, address_of clock_bench_broken);
}

static fn clock_bench_format_directives()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink += clock_bench_strftime(
                        clock_bench_output, sizeof(clock_bench_output),
                        clock_bench_format, address_of clock_bench_broken);
}

static fn clock_bench_scan_literal()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink += (positive)clock_bench_strptime(
                        (const char address_to)clock_bench_input,
                        clock_bench_literal,
                        address_of clock_bench_broken);
}

static fn clock_bench_scan_directives()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink += (positive)clock_bench_strptime(
                        clock_bench_scan_input, clock_bench_scan_format,
                        address_of clock_bench_broken);
}

static fn clock_bench_asctime_work()
{
        for (positive i = 0; i < CLOCK_BENCH_ROUNDS; i++)
                clock_bench_sink += (positive)clock_bench_asctime(
                        address_of clock_bench_broken, clock_bench_output);
}

static p64 clock_bench_best(clock_bench_work work)
{
        p64 best = 0;

        for (positive which = 0; which < CLOCK_BENCH_TRIES; which++)
        {
                p64 started = get_cpu_time();
                p64 elapsed;

                work();
                elapsed = get_cpu_time() - started;

                if (!best || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn clock_bench_report(string_address name, clock_bench_work work,
                             positive units, string_address unit)
{
        p64 ticks = clock_bench_best(work);
        positive scaled =
                (positive)(ticks * 100 / (CLOCK_BENCH_ROUNDS * units));
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;

        string_format(log, "  %s  %p.%s ticks/%s\n", name, scaled / 100,
                      fraction, unit);
}

static clock_bench_work clock_bench_named(string_address name)
{
        if (string_compare(name, (string_address)"copy-floor") == 0)
                return clock_bench_copy;
        if (string_compare(name, (string_address)"compare-floor") == 0)
                return clock_bench_compare;
        if (string_compare(name, (string_address)"scan-floor") == 0)
                return clock_bench_scan_floor;
        if (string_compare(name, (string_address)"format-literal") == 0)
                return clock_bench_format_literal;
        if (string_compare(name, (string_address)"format-directives") == 0)
                return clock_bench_format_directives;
        if (string_compare(name, (string_address)"scan-literal") == 0)
                return clock_bench_scan_literal;
        if (string_compare(name, (string_address)"scan-directives") == 0)
                return clock_bench_scan_directives;
        if (string_compare(name, (string_address)"asctime") == 0)
                return clock_bench_asctime_work;

        return null;
}

b32 main(void)
{
        positive literal_length =
                string_length((string_address)clock_bench_literal);

        memory_copy_apart(clock_bench_input,
                          (address_any)clock_bench_literal,
                          literal_length + 1);

        clock_bench_broken.tm_sec = 51;
        clock_bench_broken.tm_min = 42;
        clock_bench_broken.tm_hour = 13;
        clock_bench_broken.tm_mday = 17;
        clock_bench_broken.tm_mon = 7;
        clock_bench_broken.tm_year = 124;
        clock_bench_broken.tm_wday = 6;
        clock_bench_broken.tm_yday = 229;
        clock_bench_broken.tm_zone = "UTC";

        if (program_argument_count() > 1)
        {
                clock_bench_work work =
                        clock_bench_named(program_argument(1));

                if (is_null(work))
                        return 2;

                work();
                return 0;
        }

        string_format(log, "clock text, best of %p (%p rounds)\n",
                      (positive)CLOCK_BENCH_TRIES,
                      (positive)CLOCK_BENCH_ROUNDS);
        clock_bench_report((string_address)"copy floor", clock_bench_copy,
                           literal_length, (string_address)"byte");
        clock_bench_report((string_address)"strftime literal",
                           clock_bench_format_literal, literal_length,
                           (string_address)"byte");
        clock_bench_report((string_address)"strftime directives",
                           clock_bench_format_directives,
                           CLOCK_FORMAT_DIRECTIVES,
                           (string_address)"directive");
        clock_bench_report((string_address)"compare floor",
                           clock_bench_compare, literal_length,
                           (string_address)"byte");
        clock_bench_report((string_address)"parser floor",
                           clock_bench_scan_floor, literal_length,
                           (string_address)"byte");
        clock_bench_report((string_address)"strptime literal",
                           clock_bench_scan_literal, literal_length,
                           (string_address)"byte");
        clock_bench_report((string_address)"strptime directives",
                           clock_bench_scan_directives, CLOCK_SCAN_DIRECTIVES,
                           (string_address)"directive");
        clock_bench_report((string_address)"asctime", clock_bench_asctime_work,
                           25, (string_address)"byte");
        log_flush();

        return 0;
}
