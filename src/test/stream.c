/*
        Experimental C standard library

        the FILE trace, over this tree's own streams

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        The trace goes through log, which is the library's own buffered writer
        on descriptor one and has nothing to do with the FILE streams being
        measured. A trace printed through the thing it is measuring cannot
        report on it: a stream that flushed at the wrong moment would move the
        evidence rather than show it.
*/
#include "../compiler_memory.c"

#define trace_number(label, value)                                    \
        string_format(log, "%s %b\n", (string_address)(label),        \
                      (b32)(long)(value))

#define body_limit_memory() \
        (system_call_4(syscall(prlimit64), 0, 9, \
                        (positive)(p64[]){1, 1}, 0) == 0)
#include "stream_body.c"

static bool dynamic_buffer_fits_one_shelf(void)
{
        stream address_to handle =
                stream_open((string_address)"/dev/null", (string_address)"w");

        if (is_null(handle))
                return false;

        stream_ready(handle);
        bool fits = handle->buffer_size == STREAM_DYNAMIC_BUFFER &&
                    memory_usable_size(handle->buffer) ==
                            STREAM_DYNAMIC_BUFFER;

        stream_close(handle);
        return fits;
}

static bool empty_mode_stays_bounded(void)
{
        // 64 KiB is a multiple of every supported Linux base page size.
        positive page = 64 * 1024;
        p8 address_to bytes = memory(page * 2);
        if (!bytes || (positive)bytes >= ERROR_WINDOW)
                return false;
        if (system_call_3(syscall(mprotect), (positive)(bytes + page), page, 0))
        {
                memory_free(bytes, page * 2);
                return false;
        }

        p8 address_to mode = bytes + page - 1;
        *mode = end;
        errno = 0;
        stream address_to opened = fopen("/dev/null", mode);
        bool bounded = !opened && errno == EINVAL;
        if (opened)
                fclose(opened);
        memory_free(bytes, page * 2);
        return bounded;
}

b32 main(void)
{
        if (program_argument_count() > 1)
                return body_allocation_failure();
        if (!empty_mode_stays_bounded())
                return 1;
        trace_body();
        bool fits = dynamic_buffer_fits_one_shelf();

        log_flush();
        return fits ? 0 : 1;
}
