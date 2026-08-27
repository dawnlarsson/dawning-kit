#include "../src/sh/file.c"

// rm [-r] [-f] FILE...
static bool rm_force;
static b32 rm_status;

static bool rm_tree(bipolar directory, string_address name, string_address shown,
                    positive depth);

static bool rm_contents(bipolar directory, string_address shown, positive depth)
{
        bool complete = true;

        /*
                Removing an entry while a getdents block is being walked moves
                the ones behind it, so the block is refilled from the start
                after each pass and the directory is read again until a pass
                finds nothing left to take.
        */
        while (1)
        {
                file_walk walk;

                walk.handle = directory;
                walk.have = 0;
                walk.at = 0;

                system_call_3(syscall(lseek), directory, 0, FILE_SEEK_SET);

                struct linux_dirent64 address_to entry;
                positive removed = 0;
                positive seen = 0;

                while ((entry = file_walk_next(address_of walk)))
                {
                        if (file_is_dot(entry->d_name))
                                continue;

                        seen++;

                        p8 below[FILE_PATH_MAX];

                        file_join(below, FILE_PATH_MAX, shown, entry->d_name);

                        if (rm_tree(directory, entry->d_name, below, depth))
                                removed++;
                        else
                                complete = false;
                }

                if (seen == 0 || removed == 0)
                        break;
        }

        return complete;
}

static bool rm_tree(bipolar directory, string_address name, string_address shown,
                    positive depth)
{
        if (system_call_3(syscall(unlinkat), directory, (positive)name, 0) == 0)
                return true;

        if (!file_is_directory(directory, name))
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(-ERROR_NO_ENTRY));
                        rm_status = 1;
                }

                return false;
        }

        if (depth == 0)
        {
                string_format(file_fail, "rm: '%s' is nested too deep\n", shown);
                rm_status = 1;
                return false;
        }

        bipolar inside = system_call_3(syscall(openat), directory, (positive)name,
                                       FILE_READ | O_DIRECTORY);

        if (inside < 0)
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot read '%s': %s\n", shown,
                                      file_reason(inside));
                        rm_status = 1;
                }

                return false;
        }

        bool complete = rm_contents(inside, shown, depth - 1);

        system_call_1(syscall(close), inside);

        bipolar gone = system_call_3(syscall(unlinkat), directory, (positive)name,
                                     AT_REMOVEDIR);

        if (gone < 0)
        {
                if (!rm_force)
                {
                        string_format(file_fail, "rm: cannot remove '%s': %s\n", shown,
                                      file_reason(gone));
                        rm_status = 1;
                }

                return false;
        }

        return complete;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "rRfivd", address_of first);

        rm_force = (flags & FILE_FLAG('f')) != 0;

        bool recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R'))) != 0;

        if (first >= count)
        {
                if (rm_force)
                        return 0;

                file_fail("rm: missing operand\n", 0);
                return 1;
        }

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (!file_exists(AT_FDCWD, path))
                {
                        if (!rm_force)
                        {
                                string_format(file_fail,
                                              "rm: cannot remove '%s': No such file or directory\n",
                                              path);
                                rm_status = 1;
                        }

                        continue;
                }

                if (file_is_directory(AT_FDCWD, path) && !recursive)
                {
                        string_format(file_fail, "rm: cannot remove '%s': Is a directory\n",
                                      path);
                        rm_status = 1;
                        continue;
                }

                rm_tree(AT_FDCWD, path, path, FILE_MAX_DEPTH);
        }

        log_flush();

        return rm_status;
}
