/*
        Experimental C standard library

        the buffering observation, over this tree's own streams

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/
#include "../compiler_memory.c"

static fn body_pause_now(void)
{
        timespec moment;

        moment.tv_sec = 0;
        moment.tv_nsec = 700000000;
        sleep(address_of moment);
}

#define body_pause() body_pause_now()

#include "stream_buffering_body.c"

b32 main(void)
{
        trace_body();
        return 0;
}
