/* Buffered stream_get_byte hits against ABI and state-machine floors. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define STREAM_GET_ROUNDS (1u << 24)
#define STREAM_GET_BUFFER 4096
#define STREAM_GET_TRIES 7

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

//      Every try begins at the front of the resident buffer, and the
//      rewind is not part of what the clock sees.
static fn stream_get_rewind()
{
        stream_get_handle.read_head = 0;
}

static fn stream_get_report(string_address name, bench_work work)
{
        bench_report(name, work, STREAM_GET_TRIES,
                     STREAM_GET_ROUNDS, (string_address)"byte");
}

static bench_work stream_get_named(string_address name)
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
        bench_prepare = stream_get_rewind;

        if (program_argument_count() > 1)
        {
                bench_work work = stream_get_named(program_argument(1));
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
