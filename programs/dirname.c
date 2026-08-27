#include "../src/sh/file.c"

// dirname NAME..., the directory part of every name given.
b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();

        file_take_options((string_address) "", address_of first);

        if (first >= count)
        {
                file_fail("dirname: missing operand\n", 0);
                return 1;
        }

        while (first < count)
        {
                p8 answer[FILE_PATH_MAX];

                file_head(program_argument((b32)first++), answer);
                file_line(answer);
        }

        log_flush();

        return 0;
}
