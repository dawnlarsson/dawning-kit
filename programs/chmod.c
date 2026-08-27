#include "../src/sh/file.c"

// chmod [-R] MODE FILE..., with MODE octal or symbolic.
static string_address chmod_specification;
static b32 chmod_status;

static bool chmod_one(bipolar directory, string_address name, string_address shown)
{
        file_facts facts;

        // Following the link, because Linux has no mode on a symlink of its
        // own to change and chmod has always meant the thing pointed at.
        if (!file_look(directory, name, 0, address_of facts))
        {
                file_facts itself;

                // A link with nothing at the end of it is not an error worth
                // a word, but there is nothing to change either.
                if (file_look(directory, name, AT_SYMLINK_NOFOLLOW, address_of itself))
                        return true;

                string_format(file_fail, "chmod: cannot access '%s': No such file or directory\n",
                              shown);
                chmod_status = 1;
                return false;
        }

        positive wanted = 0;

        if (!file_mode_of(chmod_specification, facts.mode,
                          (facts.mode & MODE_FORMAT) == MODE_DIRECTORY, address_of wanted))
        {
                string_format(file_fail, "chmod: invalid mode: %s\n", chmod_specification);
                chmod_status = 1;
                return false;
        }

        bipolar done = system_call_4(syscall(fchmodat), directory, (positive)name, wanted, 0);

        if (done < 0)
        {
                string_format(file_fail, "chmod: changing permissions of '%s': %s\n",
                              shown, file_reason(done));
                chmod_status = 1;
                return false;
        }

        return true;
}

static fn chmod_walk(bipolar directory, string_address name, string_address shown,
                     positive depth)
{
        chmod_one(directory, name, shown);

        // is_directory here asks about the link itself, so a link to a
        // directory is changed and not walked into.
        if (depth == 0 || !file_is_directory(directory, name))
                return;

        file_walk walk;

        if (!file_walk_open(address_of walk, directory, name))
                return;

        struct linux_dirent64 address_to entry;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 below[FILE_PATH_MAX];

                file_join(below, FILE_PATH_MAX, shown, entry->d_name);
                chmod_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "Rfvc", address_of first);

        if (first + 1 >= count)
        {
                file_fail("chmod: missing operand\n", 0);
                return 1;
        }

        chmod_specification = program_argument((b32)first++);

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (flags & FILE_FLAG('R'))
                        chmod_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chmod_one(AT_FDCWD, path, path);
        }

        log_flush();

        return chmod_status;
}
