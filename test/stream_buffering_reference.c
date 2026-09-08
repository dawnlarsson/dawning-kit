/*
        Experimental C standard library

        the same buffering observation, over the machine's glibc

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/
#include <stdio.h>
#include <time.h>

static void body_pause_now(void)
{
        struct timespec moment = {0, 700000000};

        nanosleep(&moment, 0);
}

#define body_pause() body_pause_now()

#include "stream_buffering_body.c"

int main(void)
{
        trace_body();
        return 0;
}
