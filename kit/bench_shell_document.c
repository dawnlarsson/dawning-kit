#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

/* Isolate the shared here-body expansion path from parsing/forking. Build
   this same probe in both source revisions with src/test/run's freestanding
   flags. Arguments: body bytes (16..1048576), repetitions, optional "dollar".
   Literal runs should go directly into the retained token arena, without a
   second body-sized expansion/mark allocation. The checksum must agree. */
b32 main()
{
        positive bytes = program_argument(1)
                             ? string_digits(program_argument(1), null) : 65536;
        positive repeats = program_argument(2)
                               ? string_digits(program_argument(2), null) : 1000;
        bool dollar = program_argument(3) &&
                      word_is(program_argument(3), "dollar");
        positive sum = 0;
        p8 address_to body;

        if (bytes < 16 || bytes > 1048576 || !repeats || repeats > 1000000)
                return 2;
        body = memory(bytes + 1);
        if (!body || (positive)body >= (positive)-4095)
                return 2;
        memory_fill(body, 'x', bytes);
        body[bytes] = end;
        shell_env_init(environ);
        env_set("word", "abcdefg");
        if (dollar)
                memory_copy(body + bytes - 7, "${word}", 7);

        for (positive at = 0; at < repeats; at++)
        {
                string_address out;
                positive made;
                token_used = 0;
                token_overflow = false;
                made = exec_here_expand(body, bytes, address_of out);
                if (made != bytes || token_overflow || exec_line_aborted() ||
                    out[bytes - 1] != (dollar ? 'g' : 'x'))
                        return 1;
                sum += made;
        }
        memory_free(body, bytes + 1);
        string_format(log, "%p\n", sum);
        log_flush();
        return 0;
}
