/*
        The timing scaffold shared by the small standalone benchmarks.

        A benchmark supplies only the work it means to measure.  Trial
        preparation, best-of selection, fixed-point reporting and the tiny
        insertion sort used by paired medians live here so their policy cannot
        drift between callers.
*/

#ifndef DAWNING_BENCH_MEASURE_C
#define DAWNING_BENCH_MEASURE_C

typedef fn (*bench_work)(void);

/* Optional state reset performed immediately before the clock starts. */
static bench_work bench_prepare;

static p64 bench_best(bench_work work, positive tries)
{
        p64 best = 0;

        for (positive which = 0; which < tries; which++)
        {
                p64 started;
                p64 elapsed;

                if (bench_prepare)
                        bench_prepare();

                started = get_cpu_time();
                work();
                elapsed = get_cpu_time() - started;

                if (!which || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn bench_report(string_address name, bench_work work, positive tries,
                       positive units, string_address unit)
{
        p64 ticks = bench_best(work, tries);
        positive scaled = (positive)(ticks * 100 / units);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/%s\n", name, scaled / 100,
                      fraction, unit);
}

static fn order(positive address_to values, positive count)
{
        for (positive i = 1; i < count; i++)
        {
                positive value = values[i];
                positive at = i;

                while (at && values[at - 1] > value)
                {
                        values[at] = values[at - 1];
                        at--;
                }

                values[at] = value;
        }
}

#endif
