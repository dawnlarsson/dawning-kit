#include "../src/sh/file.c"

// chown [-R] [-h] USER[:GROUP] FILE..., and the same program answers to a
// USER of nothing at all so that ":group" changes only the group.
static bipolar chown_user = -1;
static bipolar chown_group = -1;
static b32 chown_status;
static positive chown_flags;

static fn chown_one(bipolar directory, string_address name, string_address shown)
{
        bipolar done = system_call_5(syscall(fchownat), directory, (positive)name,
                                     (positive)chown_user, (positive)chown_group,
                                     (chown_flags & FILE_FLAG('h')) ? AT_SYMLINK_NOFOLLOW : 0);

        if (done < 0)
        {
                string_format(file_fail, "chown: changing ownership of '%s': %s\n",
                              shown, file_reason(done));
                chown_status = 1;
        }
}

static fn chown_walk(bipolar directory, string_address name, string_address shown,
                     positive depth)
{
        chown_one(directory, name, shown);

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
                chown_walk(walk.handle, entry->d_name, below, depth - 1);
        }

        file_walk_close(address_of walk);
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();

        chown_flags = file_take_options((string_address) "Rhfvc", address_of first);

        if (first + 1 >= count)
        {
                file_fail("chown: missing operand\n", 0);
                return 1;
        }

        string_address who = program_argument((b32)first++);
        p8 user[FILE_NAME_MAX];
        positive length = 0;

        while (string_get(who + length) && !string_is(who + length, ':') &&
               !string_is(who + length, '.') && length + 1 < FILE_NAME_MAX)
        {
                user[length] = string_get(who + length);
                length++;
        }

        user[length] = end;

        string_address group = null;

        if (string_is(who + length, ':') || string_is(who + length, '.'))
                group = who + length + 1;

        if (length > 0)
        {
                chown_user = file_all_digits(user) ? (bipolar)file_count(user)
                                                   : file_user_id(user);

                if (chown_user < 0)
                {
                        string_format(file_fail, "chown: invalid user: %s\n", who);
                        return 1;
                }
        }

        if (group && string_get(group))
        {
                chown_group = file_all_digits(group) ? (bipolar)file_count(group)
                                                     : file_group_id(group);

                if (chown_group < 0)
                {
                        string_format(file_fail, "chown: invalid group: %s\n", who);
                        return 1;
                }
        }

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (chown_flags & FILE_FLAG('R'))
                        chown_walk(AT_FDCWD, path, path, FILE_MAX_DEPTH);
                else
                        chown_one(AT_FDCWD, path, path);
        }

        log_flush();

        return chown_status;
}
