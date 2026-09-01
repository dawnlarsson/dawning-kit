#include "../src/compiler_memory.c"
#include "../src/spark.c"
#define SHELL_UTILITY_PROGRAM
#include "../src/sh/shell.c"

/* Utility-only multicall entry: never turn an unknown argv[0] into a shell. */
b32 main()
{
        b32 answered = shell_tool_as_called();

        if (answered >= 0)
        {
                log_flush();
                return answered;
        }

        string_format(file_fail, "%s: utility not found\n", program_argument(0));
        return 127;
}
