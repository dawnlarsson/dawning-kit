#include "../compiler_memory.c"

/*
        The standard names, called by their standard names.

        This is the freestanding half of a pair. Everything it checks is in
        src/test/declare_cases.inc, and src/test/declare_reference.c builds
        that same file against the host's real headers and glibc. The pair is
        what makes the lane mean anything: a case file that compiles here and
        not there would mean src/standard/declare.c had declared something in
        a way no real header would recognise, and a case file that passes
        here and fails there is a genuine disagreement about behaviour rather
        than about what to test.

        Four hundred and forty two lines of raw output were compared between
        the two while this was written and one of them differed, which is
        strncpy's padding; it is written out in both directions in the case
        file.

        DECLARE_FREESTANDING switches on the identity checks at the end,
        which ask whether memcpy and memory_copy are one address. glibc has
        no answer to that and no reason to.
*/
#define DECLARE_FREESTANDING 1

#define declare_jump_state jump_state

static positive declare_checks;
static positive declare_failures;

#define declare_check(name, condition)                                        \
        do {                                                                  \
                declare_checks++;                                             \
                if (!(condition)) {                                           \
                        declare_failures++;                                   \
                        string_format(log, "  FAIL " name "\n");              \
                }                                                             \
        } while (0)

#include "declare_cases.inc"

b32 main(void)
{
        declare_cases();

        string_format(log, "%p checks, %p failures\n", declare_checks,
                      declare_failures);
        log_flush();

        return declare_failures != 0;
}
