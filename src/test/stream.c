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

b32 main(void)
{
        trace_body();
        bool fits = dynamic_buffer_fits_one_shelf();

        log_flush();
        return fits ? 0 : 1;
}
