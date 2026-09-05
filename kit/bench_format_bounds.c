/*
        Bounded printf padding: retained bytes versus requested bytes.

        The wide case is deliberately a count-only snprintf.  Its answer is
        one hundred million, but its destination has no resident byte to
        fill; work proportional to the field width is therefore formatter
        overhead rather than useful output.  The ordinary mixed-format row
        keeps the optimization honest against the short widths programs use.
*/
#include "../src/compiler_memory.c"

#define FORMAT_BOUNDS_TRIALS 9
#define FORMAT_BOUNDS_NORMAL_ROUNDS (1u << 17)

static volatile bipolar format_bounds_sink;
static p8 format_bounds_room[64];

static __attribute__((noinline, noclone)) bipolar format_bounds_normal(
    positive value)
{
        return snprintf(format_bounds_room, sizeof(format_bounds_room),
                        (string_address) "%08d:%-12s:%6.3f", (b32)value,
                        (string_address) "moon", (decimal)1.25);
}

static p64 format_bounds_wide_once(void)
{
        p64 began = get_cpu_time();

        format_bounds_sink +=
            snprintf(null, 0, (string_address) "%100000000d", 7);

        return get_cpu_time() - began;
}

static p64 format_bounds_normal_once(void)
{
        p64 began = get_cpu_time();

        for (positive round = 0; round < FORMAT_BOUNDS_NORMAL_ROUNDS; round++)
                format_bounds_sink += format_bounds_normal(round);

        return get_cpu_time() - began;
}

static fn format_bounds_order(p64 address_to values)
{
        for (positive at = 1; at < FORMAT_BOUNDS_TRIALS; at++)
        {
                p64 value = values[at];
                positive place = at;

                while (place && values[place - 1] > value)
                {
                        values[place] = values[place - 1];
                        place--;
                }

                values[place] = value;
        }
}

b32 main(void)
{
        p64 wide[FORMAT_BOUNDS_TRIALS];
        p64 normal[FORMAT_BOUNDS_TRIALS];
        bool run_wide = true;
        bool run_normal = true;

        if (program_argument_count() > 1)
        {
                run_wide = !string_compare(program_argument(1),
                                           (string_address) "wide");
                run_normal = !string_compare(program_argument(1),
                                             (string_address) "normal");

                if (!run_wide && !run_normal)
                        return 2;
        }

        for (positive trial = 0; trial < FORMAT_BOUNDS_TRIALS; trial++)
        {
                if (run_wide)
                        wide[trial] = format_bounds_wide_once();
                if (run_normal)
                        normal[trial] = format_bounds_normal_once();
        }

        if (run_wide)
        {
                format_bounds_order(wide);
                string_format(
                    log,
                    "  count-only width 100000000  %p ticks/call median of %p\n",
                    wide[FORMAT_BOUNDS_TRIALS / 2], FORMAT_BOUNDS_TRIALS);
        }

        if (run_normal)
        {
                format_bounds_order(normal);
                string_format(
                    log,
                    "  ordinary mixed format      %p ticks/%p calls median of %p\n",
                    normal[FORMAT_BOUNDS_TRIALS / 2],
                    FORMAT_BOUNDS_NORMAL_ROUNDS, FORMAT_BOUNDS_TRIALS);
        }

        string_format(log, "  retained sink              %b\n",
                      (b32)format_bounds_sink);
        log_flush();
        return 0;
}
