/*
        Experimental C standard library

        the same trace of <stdio.h>'s remainder, over the machine's glibc

        Dawn Larsson - Apache-2.0 license
        github.com/dawnlarsson/dawning-kit

        www.dawning.dev
*/

/*
        Built by the host compiler in the ordinary way, links glibc, and is
        the answer key. It and src/test/stream_reference.c are the only files
        in the tree that include a system header on purpose.

        _GNU_SOURCE is what puts fread_unlocked, fputs_unlocked, setbuffer and
        tempnam into <stdio.h>; __fpurge lives in <stdio_ext.h> on its own.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define trace_number(label, value) \
        printf("%s %ld\n", (const char *)(label), (long)(value))

/*
        The one thing the body cannot write once. Both sides have a stat, and
        both spell st_mode the same way, but the structure's tag and the
        header it comes from are different objects, so the call is made here
        and the body only ever sees a number.
*/
static long body_file_mode(const char *path)
{
        struct stat info;

        if (stat(path, &info) < 0)
                return -1;

        return (long)(info.st_mode & 0777);
}

#include "spool_body.c"

int main(void)
{
        trace_body();
        fflush(stdout);
        return 0;
}
