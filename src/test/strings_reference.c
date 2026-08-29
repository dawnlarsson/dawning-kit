#define _GNU_SOURCE

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
        The same cases, against the host's glibc.

        This is not built by the distribution and it links nothing the
        distribution ships. It exists so that every expectation in
        src/test/strings_cases.inc is checked against the implementation those
        routines are specified by, on the machine the tests run on -- an
        expectation that only src/standard/text.c satisfies is an expectation
        that agrees with a bug.

        Build it beside the freestanding one and compare the verdicts:

            gcc -O2 -no-pie -w -o /tmp/text.reference src/test/strings_reference.c

        -no-pie because the register probe in the case file addresses
        text_case_state and calls setjmp directly, and a position independent
        executable would want those through the GOT and the PLT.

        strlcpy and strlcat arrived in glibc 2.38. On an older library this
        will not link, and the two of them are the only cases that need it.
*/

static unsigned long long checks;
static unsigned long long failures;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        printf("  FAIL " name "\n");                    \
                }                                                       \
        } while (0)

typedef char *text_string;
typedef size_t text_size;
typedef char text_byte;

#define TX(literal) ((text_string)(literal))

#define text_length(source)              strlen((const char *)(source))
#define text_equal(one, two)             (strcmp((const char *)(one),          \
                                                 (const char *)(two)) == 0)
#define text_fill(block, value, size)    memset((block), (value), (size))

#define text_duplicate(source)           strdup((const char *)(source))
#define text_duplicate_max(source, n)    strndup((const char *)(source), (n))
#define text_token(source, delims)       strtok((source), (delims))
#define text_token_next(source, delims, place)                                \
        strtok_r((source), (delims), (place))
#define text_split_next(holder, delims)  strsep((holder), (delims))
#define text_search_folded(hay, needle)  strcasestr((hay), (needle))
#define text_copy_bounded(dest, source, n)                                    \
        strlcpy((dest), (source), (n))
#define text_append_bounded(dest, source, n)                                  \
        strlcat((dest), (source), (n))
#define text_frob(block, size)           memfrob((block), (size))

#define text_jump_state                  jmp_buf
#define text_jump_mark(state)            setjmp(state)
#define text_jump_to_mark(state, value)  longjmp((state), (value))

#include "strings_cases.inc"

int main(void)
{
        text_case_duplicate();
        text_case_duplicate_max();
        text_case_token_next();
        text_case_token();
        text_case_split();
        text_case_search_folded();
        text_case_copy_bounded();
        text_case_append_bounded();
        text_case_frob();
        text_case_jump();

        /*
                One check to stand where the freestanding build asks strtok_r
                what it does with no place at all. glibc faults on that
                question, so it is not asked here and this keeps the two
                verdict lines comparable.
        */
        check("strtok_r with no place at all is not asked of glibc", 1);

        printf("%llu checks, %llu failures\n", checks, failures);

        return failures ? 1 : 0;
}
