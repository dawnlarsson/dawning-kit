#include "../compiler_memory.c"

/* String and jump cases share strings_cases.inc with the system-libc
   reference. The umbrella supplies the actual text and allocator families. */
#include "counted.inc"

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
        text_case_all();

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

        return test_report(null);
}
