/*
        Experimental C standard library

        the same standard stream trace, over the machine's glibc

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void standard_trace_number(const char *label, long value)
{
        char text[160];
        int length = snprintf(text, sizeof(text), "%s %ld\n", label, value);
        ssize_t ignored = write(3, text, (size_t)length);
        (void)ignored;
}

#define trace_number(label, value) standard_trace_number((label), (long)(value))

#include "stream_standard_body.c"

int main(void)
{
        trace_body();
        return 0;
}
