#include "../src/sh/file.c"

/*
        cp [-r] [-p] SOURCE... DESTINATION

        A destination that is a directory takes each source under its own last
        component; two operands where the second is not a directory make the
        copy itself.
*/
static bool cp_recursive;
static bool cp_preserve;
static b32 cp_status;

static fn cp_keep(string_address destination, file_facts address_to facts)
{
        if (!cp_preserve)
                return;

        p64 times[4];

        times[0] = (p64)facts->accessed.seconds;
        times[1] = facts->accessed.nanoseconds;
        times[2] = (p64)facts->modified.seconds;
        times[3] = facts->modified.nanoseconds;

        system_call_5(syscall(fchownat), AT_FDCWD, (positive)destination,
                      facts->owner, facts->group, AT_SYMLINK_NOFOLLOW);

        system_call_4(syscall(utimensat), AT_FDCWD, (positive)destination,
                      (positive)times, AT_SYMLINK_NOFOLLOW);

        system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                      facts->mode & 07777, 0);
}

static bool cp_one(string_address source, string_address destination, positive depth)
{
        file_facts facts;

        if (!file_look_link(source, address_of facts))
        {
                string_format(file_fail, "cp: cannot stat '%s': No such file or directory\n",
                              source);
                cp_status = 1;
                return false;
        }

        positive kind = facts.mode & MODE_FORMAT;

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                {
                        cp_status = 1;
                        return false;
                }

                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

                if (system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                  (positive)destination) < 0)
                {
                        string_format(file_fail, "cp: cannot create link '%s'\n", destination);
                        cp_status = 1;
                        return false;
                }

                cp_keep(destination, address_of facts);
                return true;
        }

        if (kind != MODE_DIRECTORY)
        {
                if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                        facts.mode & 07777))
                {
                        string_format(file_fail, "cp: cannot copy '%s'\n", source);
                        cp_status = 1;
                        return false;
                }

                // The open above only sets the mode on a file it created, so a
                // destination that was already there keeps whatever it had
                // unless the mode is written afterwards.
                system_call_4(syscall(fchmodat), AT_FDCWD, (positive)destination,
                              facts.mode & 07777, 0);

                cp_keep(destination, address_of facts);
                return true;
        }

        if (!cp_recursive)
        {
                string_format(file_fail, "cp: -r not specified; omitting directory '%s'\n",
                              source);
                cp_status = 1;
                return false;
        }

        if (depth == 0)
        {
                string_format(file_fail, "cp: '%s' is nested too deep\n", source);
                cp_status = 1;
                return false;
        }

        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)destination,
                                     facts.mode & 07777);

        if (made < 0 && made != -ERROR_EXISTS)
        {
                string_format(file_fail, "cp: cannot create directory '%s': %s\n",
                              destination, file_reason(made));
                cp_status = 1;
                return false;
        }

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
        {
                string_format(file_fail, "cp: cannot read directory '%s'\n", source);
                cp_status = 1;
                return false;
        }

        struct linux_dirent64 address_to entry;
        bool complete = true;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                p8 from[FILE_PATH_MAX];
                p8 to[FILE_PATH_MAX];

                file_join(from, FILE_PATH_MAX, source, entry->d_name);
                file_join(to, FILE_PATH_MAX, destination, entry->d_name);

                if (!cp_one(from, to, depth - 1))
                        complete = false;
        }

        file_walk_close(address_of walk);

        cp_keep(destination, address_of facts);

        return complete;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive flags = file_take_options((string_address) "rRpafdvit", address_of first);

        cp_recursive = (flags & (FILE_FLAG('r') | FILE_FLAG('R') | FILE_FLAG('a'))) != 0;
        cp_preserve = (flags & (FILE_FLAG('p') | FILE_FLAG('a'))) != 0;

        if (first + 1 >= count)
        {
                file_fail("cp: missing operand\n", 0);
                return 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (count - first == 2 && !file_is_directory_through(last))
        {
                cp_one(program_argument((b32)first), last, FILE_MAX_DEPTH);
                log_flush();
                return cp_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "cp: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < count - 1)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                file_tail(source, tail);
                file_join(destination, FILE_PATH_MAX, last, tail);

                cp_one(source, destination, FILE_MAX_DEPTH);
        }

        log_flush();

        return cp_status;
}
