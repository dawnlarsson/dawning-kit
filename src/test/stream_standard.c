/*
        Experimental C standard library

        the standard stream trace, over this tree's own streams

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        The trace cannot go through log here: this test writes to standard
        output on purpose and log writes there too, so the two would
        interleave and the comparison would be measuring flush order rather
        than stream behaviour. Descriptor three is opened by the runner and
        written with the raw retrying primitive, which is outside everything
        under test.
*/
#include "../compiler_memory.c"

static fn standard_trace_writer(address_any data, positive length)
{
        if (length == 0)
                length = string_length((string_address)data);

        system_write_all(3, data, length);
}

#define trace_number(label, value)                                     \
        string_format(standard_trace_writer, "%s %b\n",                \
                      (string_address)(label), (b32)(long)(value))

#include "stream_standard_body.c"

b32 main(void)
{
        trace_body();
        return 0;
}
