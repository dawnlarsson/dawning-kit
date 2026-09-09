#include "src/library.c"
#include "src/library.common.c"

b32 main()
{
        string_format(log, "hello %s %p\n", "world", (positive)42);
        log_flush();
        return 0;
}
