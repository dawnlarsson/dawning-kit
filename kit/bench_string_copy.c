/* string_copy over caller-shaped sizes, against semantic and traffic proxies. */
#include "../src/compiler_memory.c"

#define COPY_BENCH_ROOM ((1u << 20) + 64)
#define COPY_BENCH_BYTES (1u << 26)
#define COPY_BENCH_TRIES 7

typedef fn (*copy_bench_work)(positive, positive);
typedef string_address (*copy_bench_call)(string_address, string_address);
typedef address_any (*copy_bench_traffic_call)(address_any, address_any,
                                               positive);

static volatile positive copy_bench_sink;
static p8 copy_bench_source[COPY_BENCH_ROOM] __attribute__((aligned(64)));
static p8 copy_bench_output[COPY_BENCH_ROOM] __attribute__((aligned(64)));

static __attribute__((noinline, noclone)) string_address
copy_bench_empty(string_address destination, string_address source)
{
        (void)source;
        return destination;
}

/* Exact scalar semantics; a reference candidate, not a performance floor. */
static __attribute__((noinline, noclone)) string_address
copy_bench_scalar(string_address destination, string_address source)
{
        string_address answer = destination;
        p8 byte;

        do
        {
                byte = address_to source++;
                address_to destination++ = byte;
        }
        while (byte);

        return answer;
}

static copy_bench_call volatile copy_bench_empty_call = copy_bench_empty;
static copy_bench_call volatile copy_bench_scalar_call = copy_bench_scalar;
static copy_bench_call volatile copy_bench_subject_call = string_copy;
static copy_bench_traffic_call volatile copy_bench_traffic = memory_copy_apart;

static fn copy_bench_control(positive size, positive rounds)
{
        (void)size;
        for (positive i = 0; i < rounds; i++)
                copy_bench_sink += (positive)copy_bench_empty_call(
                        copy_bench_output, copy_bench_source);
}

static fn copy_bench_floor(positive size, positive rounds)
{
        for (positive i = 0; i < rounds; i++)
                copy_bench_sink += (positive)copy_bench_traffic(
                        copy_bench_output, copy_bench_source, size);
}

static fn copy_bench_scalar_floor(positive size, positive rounds)
{
        (void)size;
        for (positive i = 0; i < rounds; i++)
                copy_bench_sink += (positive)copy_bench_scalar_call(
                        copy_bench_output, copy_bench_source);
}

static fn copy_bench_subject(positive size, positive rounds)
{
        (void)size;
        for (positive i = 0; i < rounds; i++)
                copy_bench_sink += (positive)copy_bench_subject_call(
                        copy_bench_output, copy_bench_source);
}

static p64 copy_bench_best(copy_bench_work work, positive size,
                           positive rounds)
{
        p64 best = 0;

        for (positive which = 0; which < COPY_BENCH_TRIES; which++)
        {
                p64 started = get_cpu_time();
                p64 elapsed;

                work(size, rounds);
                elapsed = get_cpu_time() - started;

                if (!best || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn copy_bench_report(string_address name, copy_bench_work work,
                            positive size, positive rounds)
{
        p64 ticks = copy_bench_best(work, size, rounds);
        positive scaled = (positive)(ticks * 100 / rounds);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "    %s  %p.%s ticks/call\n", name, scaled / 100,
                      fraction);
}

static copy_bench_work copy_bench_named(string_address name)
{
        if (string_compare(name, (string_address)"control") == 0)
                return copy_bench_control;
        if (string_compare(name, (string_address)"floor") == 0)
                return copy_bench_floor;
        if (string_compare(name, (string_address)"scalar") == 0)
                return copy_bench_scalar_floor;
        if (string_compare(name, (string_address)"subject") == 0)
                return copy_bench_subject;
        return null;
}

b32 main(void)
{
        static const positive sizes[] = {5, 23, 128, 4096, 1u << 20};

        for (positive i = 0; i < COPY_BENCH_ROOM - 1; i++)
                copy_bench_source[i] = (p8)('a' + (i * 7) % 23);
        copy_bench_source[COPY_BENCH_ROOM - 1] = end;

        if (program_argument_count() > 2)
        {
                copy_bench_work work = copy_bench_named(program_argument(1));
                positive size = string_to_positive(program_argument(2));

                if (is_null(work) || !size || size >= COPY_BENCH_ROOM)
                        return 2;

                copy_bench_source[size - 1] = end;
                positive rounds = COPY_BENCH_BYTES / size;
                if (!rounds)
                        rounds = 1;
                work(size, rounds);
                return 0;
        }

        string_format(log, "string_copy caller shapes, best of %p (~%p bytes/row)\n",
                      (positive)COPY_BENCH_TRIES,
                      (positive)COPY_BENCH_BYTES);

        for (positive which = 0; which < sizeof(sizes) / sizeof(sizes[0]); which++)
        {
                positive size = sizes[which];
                positive rounds = COPY_BENCH_BYTES / size;

                copy_bench_source[size - 1] = end;
                string_format(log, "  %p bytes (%p calls)\n", size, rounds);
                copy_bench_report((string_address)"empty ABI control",
                                  copy_bench_control, size, rounds);
                copy_bench_report((string_address)"copy-only traffic proxy",
                                  copy_bench_floor, size, rounds);
                copy_bench_report((string_address)"scalar semantic reference",
                                  copy_bench_scalar_floor, size, rounds);
                copy_bench_report((string_address)"string_copy",
                                  copy_bench_subject, size, rounds);
                copy_bench_source[size - 1] = 'q';
        }

        log_flush();
        return 0;
}
