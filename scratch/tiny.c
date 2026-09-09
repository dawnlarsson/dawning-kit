#include "src/compiler_memory.c"

b32 main()
{
        string_format(log_output, "hello %d\n", 42);
        log_flush();
        return 0;
}
