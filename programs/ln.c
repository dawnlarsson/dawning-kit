#include "../src/sh/file.c"

// ln [-s] [-f] TARGET [NAME], and ln [-s] [-f] TARGET... DIRECTORY.
static bool ln_make(string_address target, string_address name, bool symbolic, bool force)
{
        if (force)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)name, 0);

        bipolar done;

        if (symbolic)
                done = system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                     (positive)name);
        else
                done = system_call_5(syscall(linkat), AT_FDCWD, (positive)target,
                                     AT_FDCWD, (positive)name, 0);

        if (done < 0)
        {
                string_format(file_fail, "ln: failed to create link '%s': %s\n",
                              name, file_reason(done));
                return false;
        }

        return true;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "sfnvTP", address_of first);

        bool symbolic = (flags & FILE_FLAG('s')) != 0;
        bool force = (flags & FILE_FLAG('f')) != 0;

        if (first >= count)
        {
                file_fail("ln: missing operand\n", 0);
                return 1;
        }

        positive given = count - first;

        if (given == 1)
        {
                // One operand links into the working directory under the
                // target's own last component.
                string_address target = program_argument((b32)first);
                p8 name[FILE_PATH_MAX];

                file_tail(target, name);

                return ln_make(target, name, symbolic, force) ? 0 : 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (given == 2 && !file_is_directory_through(last))
        {
                return ln_make(program_argument((b32)first), last, symbolic, force) ? 0 : 1;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "ln: target '%s' is not a directory\n", last);
                return 1;
        }

        b32 status = 0;

        while (first < count - 1)
        {
                string_address target = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 name[FILE_PATH_MAX];

                file_tail(target, tail);
                file_join(name, FILE_PATH_MAX, last, tail);

                if (!ln_make(target, name, symbolic, force))
                        status = 1;
        }

        log_flush();

        return status;
}
