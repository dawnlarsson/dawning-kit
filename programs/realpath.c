#include "../src/sh/file.c"

// realpath [-m] [-q] PATH..., every link and every dot resolved away.
b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "meqsLP", address_of first);

        if (first >= count)
        {
                file_fail("realpath: missing operand\n", 0);
                return 1;
        }

        bool allow_missing = (flags & FILE_FLAG('m')) != 0;
        bool quiet = (flags & FILE_FLAG('q')) != 0;
        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];

                // Everything but the last component has to be there, which
                // is what the system's own realpath asks for: naming a file
                // that has yet to be created is the point of the tool.
                p8 above[FILE_PATH_MAX];

                if (!file_real(path, answer))
                {
                        if (!quiet)
                                string_format(file_fail, "realpath: %s: Invalid argument\n", path);

                        status = 1;
                        continue;
                }

                file_head(answer, above);

                if ((!allow_missing && !file_is_directory_through(above)) ||
                    ((flags & FILE_FLAG('e')) && !file_exists(AT_FDCWD, answer)))
                {
                        if (!quiet)
                                string_format(file_fail, "realpath: %s: No such file or directory\n",
                                              path);

                        status = 1;
                        continue;
                }

                file_line(answer);
        }

        log_flush();

        return status;
}
