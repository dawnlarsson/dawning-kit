#include "../compiler_memory.c"

/*
        The rest of <string.h>, and setjmp, on three machines.

        src/standard/text.c is included directly when the umbrella has not
        already pulled it in, so this test builds and runs before the merge
        that adds that line to src/compiler_memory.c and stays correct after
        it: the file guards itself, and the guard is what this asks about.

        Every case is in src/test/strings_cases.inc, which
        src/test/strings_reference.c builds against the host's glibc. The two
        are supposed to print the same verdict, and a difference is a real
        disagreement about behaviour rather than a difference of opinion about
        what to test.
*/
#ifndef STANDARD_MODERN_C_STANDARD_TEXT
#include "../standard/text.c"
#endif

static positive checks;
static positive failures;

#define check(name, condition)                                          \
        do {                                                            \
                checks++;                                               \
                if (!(condition)) {                                     \
                        failures++;                                     \
                        string_format(log, "  FAIL " name "\n");        \
                }                                                       \
        } while (0)

/*
        The allocator strdup needs, until there is a real one.

        A bump over a fixed block: enough for the handful of small
        duplications below and nothing else. It is defined here rather than
        borrowed so this lane runs before the allocator family lands, and it
        stands down of its own accord once that family is in the tree -- the
        second guard is the house spelling of its include guard, so a build
        that has a real malloc never sees this one. -DTEXT_TEST_HAS_ALLOCATOR
        turns it off by hand if that family names its guard something else.

        There is no free. Nothing here frees, the block is sized for the
        whole run, and a bump allocator that pretended to have one would be
        the wrong thing to test the string routines against anyway.
*/
#if !defined(TEXT_TEST_HAS_ALLOCATOR) && \
    !defined(STANDARD_MODERN_C_STANDARD_ALLOCATOR)

static p8 text_test_block[8192];
static positive text_test_used;

address_any malloc(positive size)
{
        positive at = (text_test_used + 15) & ~(positive)15;

        if (at + size > sizeof text_test_block)
                return null;

        text_test_used = at + size;

        return text_test_block + at;
}

#endif // no allocator in the build

/*
        What the cases are written against.

        One name per routine, so the case list mentions neither the library's
        spelling nor glibc's and can be compiled against either.
*/
typedef string_address text_string;
typedef positive text_size;
typedef p8 text_byte;

#define TX(literal) ((text_string)(literal))

#define text_length(source)              string_length(source)
#define text_equal(one, two)             (string_compare((text_string)(one),   \
                                                         (text_string)(two)) == 0)
#define text_fill(block, value, size)    memory_fill((block), (value), (size))

#define text_duplicate(source)           string_duplicate(source)
#define text_duplicate_max(source, n)    string_duplicate_max((source), (n))
#define text_token(source, delims)       string_token((source), (delims))
#define text_token_next(source, delims, place)                                \
        string_token_next((source), (delims), (place))
#define text_split_next(holder, delims)  string_split_next((holder), (delims))
#define text_search_folded(hay, needle)  string_search_folded((hay), (needle))
#define text_copy_bounded(dest, source, n)                                    \
        string_copy_bounded((dest), (source), (n))
#define text_append_bounded(dest, source, n)                                  \
        string_append_bounded((dest), (source), (n))
#define text_frob(block, size)           memory_frob((block), (size))

#define text_jump_state                  jump_state
#define text_jump_mark(state)            jump_mark(state)
#define text_jump_to_mark(state, value)  jump_to_mark((state), (value))

#include "strings_cases.inc"

b32 main(void)
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
                The one question glibc cannot be asked.

                strtok_r with no source and nothing saved dereferences the
                null in glibc and the program stops there, so this case is
                outside the shared list. Ours answers nothing, which is the
                only answer a routine with no starting point has.
        */
        {
                text_string place = 0;

                check("strtok_r with no place at all",
                      string_token_next(null, TX(","), &place) == null);
        }

        string_format(log, "%p checks, %p failures\n", checks, failures);
        log_flush();

        return failures ? 1 : 0;
}
