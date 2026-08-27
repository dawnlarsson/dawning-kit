#include "../src/sh/file.c"

// readlink [-f] [-n] FILE..., where -f resolves the whole path rather than
// reading one link.
b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "fneqsvm", address_of first);

        if (first >= count)
        {
                file_fail("readlink: missing operand\n", 0);
                return 1;
        }

        bool resolve = (flags & (FILE_FLAG('f') | FILE_FLAG('e') | FILE_FLAG('m'))) != 0;
        bool no_newline = (flags & FILE_FLAG('n')) != 0;
        bool quiet = (flags & (FILE_FLAG('q') | FILE_FLAG('s'))) != 0;
        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                p8 answer[FILE_PATH_MAX];

                if (resolve)
                {
                        if (!file_real(path, answer))
                        {
                                status = 1;
                                continue;
                        }

                        p8 above[FILE_PATH_MAX];

                        file_head(answer, above);

                        // -f wants the parent to be real, -e wants the whole
                        // path to be, -m wants neither.
                        if (!(flags & FILE_FLAG('m')) && !file_is_directory_through(above))
                        {
                                status = 1;
                                continue;
                        }

                        if ((flags & FILE_FLAG('e')) && !file_exists(AT_FDCWD, answer))
                        {
                                status = 1;
                                continue;
                        }
                }
                else if (file_link_text(path, answer, FILE_PATH_MAX) < 0)
                {
                        if (!quiet)
                                string_format(file_fail, "readlink: %s: Invalid argument\n", path);

                        status = 1;
                        continue;
                }

                log(answer, 0);

                if (!no_newline)
                        log("\n", 1);
        }

        log_flush();

        return status;
}
