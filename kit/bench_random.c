/* random() draw cost against call/control and additive-ring floors. */
#include "../src/compiler_memory.c"

#define RANDOM_BENCH_ROUNDS (1u << 22)
#define RANDOM_BENCH_TRIES 7
#define RANDOM_BENCH_DEGREE 31

typedef fn (*random_bench_work)();

static volatile positive random_bench_sink;
static p32 random_bench_state[RANDOM_BENCH_DEGREE];
static positive random_bench_front = 3;
static positive random_bench_rear;

static b32 (*volatile random_bench_call)() = random;

static __attribute__((noinline, noclone)) b32 random_bench_empty()
{
        return 1;
}

static b32 (*volatile random_bench_control_call)() = random_bench_empty;

static __attribute__((noinline, noclone)) b32 random_bench_floor_draw()
{
        p32 sum = random_bench_state[random_bench_front] +
                  random_bench_state[random_bench_rear];

        random_bench_state[random_bench_front] = sum;
        random_bench_front++;
        random_bench_rear++;

        if (random_bench_front == RANDOM_BENCH_DEGREE)
                random_bench_front = 0;
        if (random_bench_rear == RANDOM_BENCH_DEGREE)
                random_bench_rear = 0;

        return (b32)(sum >> 1);
}

static b32 (*volatile random_bench_floor_call)() = random_bench_floor_draw;

static fn random_bench_control()
{
        for (positive i = 0; i < RANDOM_BENCH_ROUNDS; i++)
                random_bench_sink += (positive)random_bench_control_call();
}

static fn random_bench_floor()
{
        for (positive i = 0; i < RANDOM_BENCH_ROUNDS; i++)
                random_bench_sink += (positive)random_bench_floor_call();
}

static fn random_bench_subject()
{
        for (positive i = 0; i < RANDOM_BENCH_ROUNDS; i++)
                random_bench_sink += (positive)random_bench_call();
}

static p64 random_bench_best(random_bench_work work)
{
        p64 best = 0;

        for (positive which = 0; which < RANDOM_BENCH_TRIES; which++)
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

static fn random_bench_report(string_address name, random_bench_work work)
{
        p64 ticks = random_bench_best(work);
        positive scaled =
                (positive)(ticks * 100 / RANDOM_BENCH_ROUNDS);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/draw\n", name, scaled / 100,
                      fraction);
}

static random_bench_work random_bench_named(string_address name)
{
        if (string_compare(name, (string_address)"control") == 0)
                return random_bench_control;
        if (string_compare(name, (string_address)"floor") == 0)
                return random_bench_floor;
        if (string_compare(name, (string_address)"subject") == 0)
                return random_bench_subject;

        return null;
}

b32 main(void)
{
        for (positive i = 0; i < RANDOM_BENCH_DEGREE; i++)
                random_bench_state[i] = (p32)(i * 1103515245u + 12345u);

        srandom(1);

        if (program_argument_count() > 1)
        {
                random_bench_work work = random_bench_named(program_argument(1));

                if (is_null(work))
                        return 2;

                work();
                return 0;
        }

        string_format(log, "random draw, best of %p (%p rounds)\n",
                      (positive)RANDOM_BENCH_TRIES,
                      (positive)RANDOM_BENCH_ROUNDS);
        random_bench_report((string_address)"empty ABI control",
                            random_bench_control);
        random_bench_report((string_address)"additive ring floor",
                            random_bench_floor);
        random_bench_report((string_address)"random", random_bench_subject);
        log_flush();

        return 0;
}
