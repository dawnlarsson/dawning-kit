/*
        Experimental C standard library

        the trace of <stdio.h>'s remainder, over this tree's own

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        The trace goes through log, which is library.c's own buffered writer
        on descriptor one and has nothing to do with the streams and pipes
        being measured. src/test/stream.c says why in more words and the
        reason is the same: a trace printed through the thing it is measuring
        moves the evidence rather than showing it. It matters more here than
        there, because half of these entries fork, and a trace sitting in a
        FILE buffer at the moment of a fork is a trace that gets printed
        twice.
*/
#include "../compiler_memory.c"

#define trace_number(label, value)                             \
        string_format(log, "%s %b\n", (string_address)(label), \
                      (b32)(long)(value))

//      See the same function in spool_reference.c: the structure differs
//      between the two sides, the number does not.
static long body_file_mode(const char *path)
{
        error_stat info;

        if (stat((string_address)path, address_of info) < 0)
                return -1;

        return (long)(info.st_mode & 0777);
}

#include "spool_body.c"

b32 main(void)
{
        trace_body();
        log_flush();
        return 0;
}
