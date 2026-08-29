#define _GNU_SOURCE

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
        The same cases, against the host's glibc.

        Nothing the distribution ships is linked here and nothing here is
        built by the distribution. It exists so that src/test/declare_cases.inc
        is put through the library those names are specified by, on the
        machine the tests run on, and so that the case file has to compile
        against real headers -- which is the actual assertion about
        src/standard/declare.c. A prototype there that disagreed with
        <string.h> by a const or by a size_t would make this file fail to
        compile, and no amount of agreement at run time would make up for it.

        Build it beside the freestanding one and compare the verdicts:

            gcc -O2 -w -o /tmp/declare.reference src/test/declare_reference.c -lm

        strlcpy and strlcat arrived in glibc 2.38 and are the only two cases
        that need a library that new.

        DECLARE_FREESTANDING is not defined here, which takes away the
        identity checks -- glibc's memcpy is not anybody's memory_copy -- and
        switches strncpy's case over to the padding C describes.
*/

static unsigned long long declare_checks;
static unsigned long long declare_failures;

#define declare_jump_state jmp_buf

#define declare_check(name, condition)                                        \
        do {                                                                  \
                declare_checks++;                                             \
                if (!(condition)) {                                           \
                        declare_failures++;                                   \
                        printf("  FAIL " name "\n");                          \
                }                                                             \
        } while (0)

#include "declare_cases.inc"

int main(void)
{
        declare_cases();

        printf("%llu checks, %llu failures\n", declare_checks, declare_failures);

        return declare_failures != 0;
}
