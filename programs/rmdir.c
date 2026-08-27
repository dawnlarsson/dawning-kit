#include "../src/sh/file.c"

// rmdir [-p] DIRECTORY..., where -p goes on removing the parents while they
// are empty too.
b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "p", address_of first);

        if (first >= count)
        {
                file_fail("rmdir: missing operand\n", 0);
                return 1;
        }

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);
                bipolar gone = system_call_3(syscall(unlinkat), AT_FDCWD, (positive)path,
                                             AT_REMOVEDIR);

                if (gone < 0)
                {
                        string_format(file_fail, "rmdir: failed to remove '%s': %s\n",
                                      path, file_reason(gone));
                        status = 1;
                        continue;
                }

                if (!(flags & FILE_FLAG('p')))
                        continue;

                p8 parent[FILE_PATH_MAX];

                string_copy_max(parent, path, FILE_PATH_MAX - 1);
                parent[FILE_PATH_MAX - 1] = end;

                while (1)
                {
                        p8 above[FILE_PATH_MAX];

                        file_head(parent, above);

                        if (string_is(above, '.') && string_is(above + 1, end))
                                break;

                        if (string_is(above, '/') && string_is(above + 1, end))
                                break;

                        if (system_call_3(syscall(unlinkat), AT_FDCWD, (positive)above,
                                          AT_REMOVEDIR) < 0)
                                break;

                        string_copy_max(parent, above, FILE_PATH_MAX - 1);
                }
        }

        log_flush();

        return status;
}
