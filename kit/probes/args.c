#include "../../src/library.c"
#include "../../src/spark.c"

// Prints what it was given, which until now nothing could.
b32 main()
{
        b32 count = program_argument_count();

        string_format(log, "%b argument(s)\n", count);

        for (b32 i = 0; i < count; i++)
                string_format(log, "  %b: %s\n", i, program_argument(i));

        log_flush();

        return 0;
}
