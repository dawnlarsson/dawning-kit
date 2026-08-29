/* Resident buffered writes against their unavoidable copy/state floor. */
#include "../src/compiler_memory.c"

#define STREAM_WRITE_ROUNDS (1u << 22)
#define STREAM_WRITE_BUFFER 4096
#define STREAM_WRITE_TRIES 7
#define STREAM_WRITE_LENGTH 23

typedef fn (*stream_write_work)();
typedef sized (*stream_write_call)(address_any, sized, sized,
                                   stream address_to);
typedef positive (*stream_put_bytes_call)(stream address_to, address_any,
                                          positive);

static volatile positive stream_write_sink;
static p8 stream_write_input[STREAM_WRITE_LENGTH];
static p8 stream_write_buffer[STREAM_WRITE_BUFFER];
static stream stream_write_handle;

static stream_write_call volatile stream_write_subject_call = stream_write;
static stream_put_bytes_call volatile stream_put_bytes_subject_call =
        stream_put_bytes;

static fn stream_write_reset()
{
        /* Keep every trial on the resident buffered path, with no getpid. */
        stream_write_handle.write_used = 1;
}

/* Minimum resident transition: the bytes move once and become staged. */
static fn stream_write_floor_work()
{
        for (positive i = 0; i < STREAM_WRITE_ROUNDS; i++)
        {
                stream_write_reset();
                memory_copy(stream_write_handle.buffer +
                                    stream_write_handle.write_used,
                            stream_write_input, STREAM_WRITE_LENGTH);
                stream_write_handle.write_used += STREAM_WRITE_LENGTH;
                stream_write_sink += STREAM_WRITE_LENGTH;
        }
}

static fn stream_put_bytes_subject()
{
        for (positive i = 0; i < STREAM_WRITE_ROUNDS; i++)
        {
                stream_write_reset();
                stream_write_sink += stream_put_bytes_subject_call(
                        address_of stream_write_handle, stream_write_input,
                        STREAM_WRITE_LENGTH);
        }
}

static fn stream_write_subject_one()
{
        for (positive i = 0; i < STREAM_WRITE_ROUNDS; i++)
        {
                stream_write_reset();
                stream_write_sink += (positive)stream_write_subject_call(
                        stream_write_input, 1, STREAM_WRITE_LENGTH,
                        address_of stream_write_handle);
        }
}

/* A non-unit item shape catches collateral cost in the general path. */
static fn stream_write_subject_items()
{
        for (positive i = 0; i < STREAM_WRITE_ROUNDS; i++)
        {
                stream_write_reset();
                stream_write_sink += (positive)stream_write_subject_call(
                        stream_write_input, 8, 2,
                        address_of stream_write_handle);
        }
}

/* Line buffering must retain its newline scan and general flush decision. */
static fn stream_put_bytes_line()
{
        for (positive i = 0; i < STREAM_WRITE_ROUNDS; i++)
        {
                stream_write_reset();
                stream_write_handle.flags |= STREAM_LINE_BUFFERED;
                stream_write_sink += stream_put_bytes_subject_call(
                        address_of stream_write_handle, stream_write_input,
                        STREAM_WRITE_LENGTH);
                stream_write_handle.flags &= ~STREAM_LINE_BUFFERED;
        }
}

static p64 stream_write_best(stream_write_work work)
{
        p64 best = 0;

        for (positive which = 0; which < STREAM_WRITE_TRIES; which++)
        {
                p64 started;
                p64 elapsed;

                started = get_cpu_time();
                work();
                elapsed = get_cpu_time() - started;

                if (!best || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn stream_write_report(string_address name, stream_write_work work)
{
        p64 ticks = stream_write_best(work);
        positive scaled = (positive)(ticks * 100 / STREAM_WRITE_ROUNDS);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/call\n", name, scaled / 100,
                      fraction);
}

static stream_write_work stream_write_named(string_address name)
{
        if (string_compare(name, (string_address)"floor-resident") == 0)
                return stream_write_floor_work;
        if (string_compare(name, (string_address)"subject-put") == 0)
                return stream_put_bytes_subject;
        if (string_compare(name, (string_address)"subject-one") == 0)
                return stream_write_subject_one;
        if (string_compare(name, (string_address)"subject-items") == 0)
                return stream_write_subject_items;
        if (string_compare(name, (string_address)"subject-line") == 0)
                return stream_put_bytes_line;
        return null;
}

b32 main(void)
{
        for (positive i = 0; i < STREAM_WRITE_LENGTH; i++)
                stream_write_input[i] = (p8)(i * 13 + 1);

        stream_write_handle.descriptor = -1;
        stream_write_handle.flags = STREAM_WRITABLE | STREAM_MODE_KNOWN;
        stream_write_handle.buffer = stream_write_buffer;
        stream_write_handle.buffer_size = STREAM_WRITE_BUFFER;
        stream_write_handle.write_used = 1;

        if (program_argument_count() > 1)
        {
                stream_write_work work =
                        stream_write_named(program_argument(1));
                if (is_null(work))
                        return 2;
                work();
                return 0;
        }

        string_format(log,
                      "stream_write buffered resident path, best of %p "
                      "(%p calls)\n",
                      (positive)STREAM_WRITE_TRIES,
                      (positive)STREAM_WRITE_ROUNDS);
        stream_write_report(
                (string_address)"resident copy/state floor (state-specific)",
                stream_write_floor_work);
        stream_write_report((string_address)"stream_put_bytes",
                            stream_put_bytes_subject);
        stream_write_report((string_address)"stream_write size=1",
                            stream_write_subject_one);
        stream_write_report((string_address)"stream_write size=8",
                            stream_write_subject_items);
        stream_write_report((string_address)"line-buffered no newline",
                            stream_put_bytes_line);
        log_flush();
        return 0;
}
