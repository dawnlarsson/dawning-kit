/* Dynamic-bound string_compare_max against semantic and traffic floors. */
#include "../src/compiler_memory.c"

#define COMPARE_MAX_ROUNDS (1u << 22)
#define COMPARE_MAX_TRIES 7
#define COMPARE_MAX_SHORT 23
#define COMPARE_MAX_LONG 256

typedef fn (*compare_max_work)();
typedef b32 (*compare_max_call)(string_address, string_address, positive);
typedef b32 (*compare_max_traffic)(const address_any, const address_any,
                                   positive);

static volatile bipolar compare_max_sink;
static p8 compare_max_left[COMPARE_MAX_LONG + 1] __attribute__((aligned(64)));
static p8 compare_max_right[COMPARE_MAX_LONG + 1] __attribute__((aligned(64)));

static __attribute__((noinline, noclone)) b32
compare_max_empty(string_address left, string_address right, positive size)
{
        (void)left;
        (void)right;
        (void)size;
        return 1;
}

/* Exact minimum semantics for the mismatch-at-byte-zero corpus. */
static __attribute__((noinline, noclone)) b32
compare_max_first_floor(string_address left, string_address right, positive size)
{
        if (!size)
                return 0;
        return (b32)left[0] - (b32)right[0];
}

static compare_max_call volatile compare_max_empty_call = compare_max_empty;
static compare_max_call volatile compare_max_first_floor_call =
        compare_max_first_floor;
static compare_max_call volatile compare_max_subject_call = string_compare_max;
static compare_max_traffic volatile compare_max_traffic_call = memory_compare;

static fn compare_max_control_first()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_empty_call(
                        compare_max_left, compare_max_right, COMPARE_MAX_SHORT);
}

static fn compare_max_floor_first()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_first_floor_call(
                        compare_max_left, compare_max_right, COMPARE_MAX_SHORT);
}

static fn compare_max_subject_first()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_subject_call(
                        compare_max_left, compare_max_right, COMPARE_MAX_SHORT);
}

static fn compare_max_subject_equal()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_subject_call(
                        compare_max_left, compare_max_left, COMPARE_MAX_LONG);
}

static fn compare_max_subject_short_equal()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_subject_call(
                        compare_max_left, compare_max_left, COMPARE_MAX_SHORT);
}

static fn compare_max_floor_short_equal()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_traffic_call(
                        compare_max_left, compare_max_left, COMPARE_MAX_SHORT);
}

/*
        No byte is zero inside this corpus, so memory_compare performs the
        unavoidable two-stream read and equality decision. It is a traffic
        proxy lower bound, not a full string semantic implementation.
*/
static fn compare_max_floor_equal()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_traffic_call(
                        compare_max_left, compare_max_left, COMPARE_MAX_LONG);
}

static fn compare_max_subject_late()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_subject_call(
                        compare_max_left, compare_max_right, COMPARE_MAX_LONG);
}

static fn compare_max_floor_late()
{
        for (positive i = 0; i < COMPARE_MAX_ROUNDS; i++)
                compare_max_sink += compare_max_traffic_call(
                        compare_max_left, compare_max_right, COMPARE_MAX_LONG);
}

static p64 compare_max_best(compare_max_work work)
{
        p64 best = 0;

        for (positive which = 0; which < COMPARE_MAX_TRIES; which++)
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

static fn compare_max_report(string_address name, compare_max_work work)
{
        p64 ticks = compare_max_best(work);
        positive scaled = (positive)(ticks * 100 / COMPARE_MAX_ROUNDS);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/call\n", name, scaled / 100,
                      fraction);
}

static compare_max_work compare_max_named(string_address name)
{
        if (string_compare(name, (string_address)"control-first") == 0)
                return compare_max_control_first;
        if (string_compare(name, (string_address)"floor-first") == 0)
                return compare_max_floor_first;
        if (string_compare(name, (string_address)"subject-first") == 0)
                return compare_max_subject_first;
        if (string_compare(name, (string_address)"floor-equal") == 0)
                return compare_max_floor_equal;
        if (string_compare(name, (string_address)"subject-equal") == 0)
                return compare_max_subject_equal;
        if (string_compare(name, (string_address)"floor-short-equal") == 0)
                return compare_max_floor_short_equal;
        if (string_compare(name, (string_address)"subject-short-equal") == 0)
                return compare_max_subject_short_equal;
        if (string_compare(name, (string_address)"floor-late") == 0)
                return compare_max_floor_late;
        if (string_compare(name, (string_address)"subject-late") == 0)
                return compare_max_subject_late;
        return null;
}

b32 main(void)
{
        for (positive i = 0; i < COMPARE_MAX_LONG; i++)
                compare_max_left[i] = compare_max_right[i] =
                        (p8)('a' + (i * 7) % 23);

        compare_max_left[COMPARE_MAX_LONG] = end;
        compare_max_right[COMPARE_MAX_LONG] = end;

        if (program_argument_count() > 1)
        {
                compare_max_work work = compare_max_named(program_argument(1));
                if (is_null(work))
                        return 2;

                /* First and late mismatch use the same buffers, selected here. */
                if (string_compare(program_argument(1),
                                   (string_address)"subject-first") == 0 ||
                    string_compare(program_argument(1),
                                   (string_address)"floor-first") == 0)
                        compare_max_right[0]++;
                else if (string_compare(program_argument(1),
                                        (string_address)"subject-late") == 0 ||
                         string_compare(program_argument(1),
                                        (string_address)"floor-late") == 0)
                        compare_max_right[COMPARE_MAX_LONG - 1]++;

                work();
                return 0;
        }

        string_format(log, "string_compare_max dynamic bound, best of %p (%p calls)\n",
                      (positive)COMPARE_MAX_TRIES,
                      (positive)COMPARE_MAX_ROUNDS);

        compare_max_right[0]++;
        compare_max_report((string_address)"empty ABI control",
                           compare_max_control_first);
        compare_max_report((string_address)"first-byte semantic floor",
                           compare_max_floor_first);
        compare_max_report((string_address)"first-byte mismatch",
                           compare_max_subject_first);
        compare_max_right[0]--;

        compare_max_report((string_address)"short equal traffic proxy",
                           compare_max_floor_short_equal);
        compare_max_report((string_address)"equal 23 bytes",
                           compare_max_subject_short_equal);

        compare_max_report((string_address)"equal traffic proxy",
                           compare_max_floor_equal);
        compare_max_report((string_address)"equal 256 bytes",
                           compare_max_subject_equal);

        compare_max_right[COMPARE_MAX_LONG - 1]++;
        compare_max_report((string_address)"late traffic proxy",
                           compare_max_floor_late);
        compare_max_report((string_address)"late mismatch",
                           compare_max_subject_late);

        log_flush();
        return 0;
}
