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
        being measured. test/stream.c says why in more words and the
        reason is the same: a trace printed through the thing it is measuring
        moves the evidence rather than showing it. It matters more here than
        there, because half of these entries fork, and a trace sitting in a
        FILE buffer at the moment of a fork is a trace that gets printed
        twice.
*/
#include "../src/compiler_memory.c"

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

static bool line_buffer_fits_one_shelf(void)
{
        stream address_to handle =
                stream_open((string_address)"/dev/null", (string_address)"w");

        if (is_null(handle))
                return false;

        spool_set_line_buffered(handle);
        bool fits = handle->buffer_size == STREAM_DYNAMIC_BUFFER &&
                    memory_usable_size(handle->buffer) ==
                            STREAM_DYNAMIC_BUFFER;

        stream_close(handle);
        return fits;
}

// A pipe may already occupy the child's destination descriptor. Its `e`
// flag belongs to the parent's end; exec must still inherit the child's end.
static bool process_pipe_closed_standard(void)
{
        bool okay = true;
        for (b32 reading = 0; reading < 2; reading++)
        {
                log_flush();
                b32 saved_input = dup(0), saved_output = dup(1);
                if (saved_input < 0 || saved_output < 0)
                        return false;
                close(0);
                if (reading) close(1);
                stream address_to pipe = popen(reading ? "printf ok" :
                        "read code && exit \"$code\"", reading ? "re" : "we");
                if (!pipe)
                {
                dup2(saved_input, 0);
                dup2(saved_output, 1);
                close(saved_input);
                close(saved_output);
                        return false;
                }
                okay &= (fcntl(fileno(pipe), 1, (positive)0) & 1) != 0;
                if (reading)
                        okay &= fgetc(pipe) == 'o' && fgetc(pipe) == 'k' &&
                                fgetc(pipe) == EOF;
                else
                        okay &= fputs("0\n", pipe) >= 0;
                okay &= pclose(pipe) == 0;
                dup2(saved_input, 0);
                dup2(saved_output, 1);
                close(saved_input);
                close(saved_output);
        }
        return okay;
}

b32 main(void)
{
        char bytes[] = "read only";
        errno = 0;
        stream address_to update = fmemopen(bytes, sizeof(bytes), "r+");
        if (update)
                fclose(update);
        if (update || errno != EINVAL)
                return 1;
        trace_body();
        bool fits = line_buffer_fits_one_shelf();

        log_flush();
        return fits && process_pipe_closed_standard() ? 0 : 1;
}
