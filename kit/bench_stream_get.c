/* Buffered stream_get_byte hits against ABI and state-machine floors. */
#include "../src/compiler_memory.c"

#define STREAM_GET_ROUNDS (1u << 24)
#define STREAM_GET_BUFFER 4096
#define STREAM_GET_TRIES 7

typedef fn (*stream_get_work)();
typedef b32 (*stream_get_call)(stream address_to);

static volatile positive stream_get_sink;
static p8 stream_get_bytes[STREAM_GET_BUFFER];
static stream stream_get_handle;

static __attribute__((noinline, noclone)) b32
stream_get_empty(stream address_to handle)
{
        (void)handle;
        return 1;
}

/* Minimum buffered-hit state transition, with the same EOF answer. */
static __attribute__((noinline, noclone)) b32
stream_get_floor(stream address_to handle)
{
        if (handle->read_head == handle->read_tail)
                return EOF;
        return (b32)handle->buffer[handle->read_head++];
}

static stream_get_call volatile stream_get_empty_call = stream_get_empty;
static stream_get_call volatile stream_get_floor_call = stream_get_floor;
static stream_get_call volatile stream_get_subject_call = stream_get_byte;

static fn stream_get_reset_if_needed()
{
        if (stream_get_handle.read_head == stream_get_handle.read_tail)
                stream_get_handle.read_head = 0;
}

static fn stream_get_control()
{
        for (positive i = 0; i < STREAM_GET_ROUNDS; i++)
        {
                stream_get_reset_if_needed();
                stream_get_sink += (positive)stream_get_empty_call(
                        address_of stream_get_handle);
        }
}

static fn stream_get_floor_work()
{
        for (positive i = 0; i < STREAM_GET_ROUNDS; i++)
        {
                stream_get_reset_if_needed();
                stream_get_sink += (positive)stream_get_floor_call(
                        address_of stream_get_handle);
        }
}

static fn stream_get_subject()
{
        for (positive i = 0; i < STREAM_GET_ROUNDS; i++)
        {
                stream_get_reset_if_needed();
                stream_get_sink += (positive)stream_get_subject_call(
                        address_of stream_get_handle);
        }
}

static p64 stream_get_best(stream_get_work work)
{
        p64 best = 0;

        for (positive which = 0; which < STREAM_GET_TRIES; which++)
        {
                p64 started;
                p64 elapsed;

                stream_get_handle.read_head = 0;
                started = get_cpu_time();
                work();
                elapsed = get_cpu_time() - started;

                if (!best || elapsed < best)
                        best = elapsed;
        }

        return best;
}

static fn stream_get_report(string_address name, stream_get_work work)
{
        p64 ticks = stream_get_best(work);
        positive scaled = (positive)(ticks * 100 / STREAM_GET_ROUNDS);
        p8 fraction[3];

        positive_into_padded(fraction, scaled % 100, 2, '0');
        fraction[2] = end;
        string_format(log, "  %s  %p.%s ticks/byte\n", name, scaled / 100,
                      fraction);
}

static stream_get_work stream_get_named(string_address name)
{
        if (string_compare(name, (string_address)"control") == 0)
                return stream_get_control;
        if (string_compare(name, (string_address)"floor") == 0)
                return stream_get_floor_work;
        if (string_compare(name, (string_address)"subject") == 0)
                return stream_get_subject;
        return null;
}

b32 main(void)
{
        for (positive i = 0; i < STREAM_GET_BUFFER; i++)
                stream_get_bytes[i] = (p8)(i * 13 + 1);

        stream_get_handle.descriptor = -1;
        stream_get_handle.flags = STREAM_READABLE | STREAM_MODE_KNOWN;
        stream_get_handle.buffer = stream_get_bytes;
        stream_get_handle.buffer_size = STREAM_GET_BUFFER;
        stream_get_handle.read_head = 0;
        stream_get_handle.read_tail = STREAM_GET_BUFFER;

        if (program_argument_count() > 1)
        {
                stream_get_work work = stream_get_named(program_argument(1));
                if (is_null(work))
                        return 2;
                work();
                return 0;
        }

        string_format(log, "stream_get_byte buffered hit, best of %p (%p bytes)\n",
                      (positive)STREAM_GET_TRIES,
                      (positive)STREAM_GET_ROUNDS);
        stream_get_report((string_address)"empty ABI control", stream_get_control);
        stream_get_report((string_address)"buffered state floor",
                          stream_get_floor_work);
        stream_get_report((string_address)"stream_get_byte", stream_get_subject);
        log_flush();
        return 0;
}
