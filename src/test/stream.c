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

b32 main(void)
{
        trace_body();
        log_flush();
        return 0;
}
