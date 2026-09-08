#include <stdio.h>
#include <stdio_ext.h>

/* libstdbuf runs before main.  Reading glibc's public stdio inspection
   surface here proves that the launcher changed the streams themselves,
   rather than merely publishing plausible environment variables. */
int main(void)
{
        printf("%d:%zu %d:%zu %d:%zu\n",
               __flbf(stdin) != 0, __fbufsize(stdin),
               __flbf(stdout) != 0, __fbufsize(stdout),
               __flbf(stderr) != 0, __fbufsize(stderr));
        return 0;
}
