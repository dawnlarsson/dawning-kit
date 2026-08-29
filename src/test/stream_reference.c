/*
        Experimental C standard library

        the same FILE trace, over the machine's glibc

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        This one is built by the host compiler in the ordinary way, links
        glibc, and is the answer key. It is the only file in the tree that
        includes a system header on purpose.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define trace_number(label, value) \
        printf("%s %ld\n", (const char *)(label), (long)(value))

#include "stream_body.c"

int main(void)
{
        trace_body();
        fflush(stdout);
        return 0;
}
