#include "../src/sh/file.c"

// mkdir [-p] [-m MODE] DIRECTORY...
b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool parents = false;
        positive mode = 0777;
        bool given_mode = false;

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, '-') &&
                    string_is(argument + 2, end))
                {
                        index++;
                        break;
                }

                if (!string_is(argument, '-') || string_is(argument + 1, end))
                        break;

                if (string_is(argument + 1, 'm') && string_is(argument + 2, end) &&
                    index + 1 < count)
                {
                        if (!file_mode_of(program_argument((b32)(index + 1)), 0777, true,
                                          address_of mode))
                        {
                                file_fail("mkdir: bad mode\n", 0);
                                return 1;
                        }

                        given_mode = true;
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'p') && string_is(argument + 2, end))
                {
                        parents = true;
                        index++;
                        continue;
                }

                string_format(file_fail, "mkdir: unknown option: %s\n", argument);
                return 1;
        }

        if (index >= count)
        {
                file_fail("mkdir: missing operand\n", 0);
                return 1;
        }

        b32 status = 0;

        while (index < count)
        {
                string_address path = program_argument((b32)index++);

                if (parents)
                {
                        // The parents are made with the default, and only the
                        // directory that was named gets the mode asked for.
                        if (!file_make_parents(path, 0777))
                        {
                                file_complain((string_address) "mkdir",
                                              (string_address) "Cannot create directory",
                                              path);
                                status = 1;
                                continue;
                        }

                        if (given_mode)
                                system_call_4(syscall(fchmodat), AT_FDCWD, (positive)path,
                                              mode, 0);

                        continue;
                }

                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)path, mode);

                if (made < 0)
                {
                        string_format(file_fail, "mkdir: cannot create directory '%s': %s\n",
                                      path, file_reason(made));
                        status = 1;
                }
        }

        log_flush();

        return status;
}
