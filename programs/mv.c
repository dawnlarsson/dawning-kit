#include "../src/sh/file.c"

/*
        mv [-f] SOURCE... DESTINATION

        renameat2 rather than renameat, because riscv64 never had renameat and
        this tree builds for it; a flags word of zero is the same operation.
*/
static b32 mv_status;

static bool mv_across(string_address source, string_address destination, positive depth);

static bool mv_across_directory(string_address source, string_address destination,
                                positive depth)
{
        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD, source))
                return false;

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

                if (!mv_across(from, to, depth - 1))
                        complete = false;
        }

        file_walk_close(address_of walk);

        if (complete)
                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)source, AT_REMOVEDIR);

        return complete;
}

// A rename that crosses a mount point is not a rename at all, so the bytes
// have to be carried over and the original taken away afterwards.
static bool mv_across(string_address source, string_address destination, positive depth)
{
        file_facts facts;

        if (!file_look_link(source, address_of facts))
                return false;

        if (depth == 0)
                return false;

        positive kind = facts.mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
        {
                bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                             (positive)destination, facts.mode & 07777);

                if (made < 0 && made != -ERROR_EXISTS)
                        return false;

                return mv_across_directory(source, destination, depth);
        }

        if (kind == MODE_LINK)
        {
                p8 target[FILE_PATH_MAX];

                if (file_link_text(source, target, FILE_PATH_MAX) < 0)
                        return false;

                system_call_3(syscall(unlinkat), AT_FDCWD, (positive)destination, 0);

                if (system_call_3(syscall(symlinkat), (positive)target, AT_FDCWD,
                                  (positive)destination) < 0)
                        return false;
        }
        else if (!file_copy_contents(AT_FDCWD, source, AT_FDCWD, destination,
                                     facts.mode & 07777))
                return false;

        p64 times[4];

        times[0] = (p64)facts.accessed.seconds;
        times[1] = facts.accessed.nanoseconds;
        times[2] = (p64)facts.modified.seconds;
        times[3] = facts.modified.nanoseconds;

        system_call_4(syscall(utimensat), AT_FDCWD, (positive)destination,
                      (positive)times, AT_SYMLINK_NOFOLLOW);

        return system_call_3(syscall(unlinkat), AT_FDCWD, (positive)source, 0) == 0;
}

static fn mv_one(string_address source, string_address destination)
{
        bipolar done = system_call_5(syscall(renameat2), AT_FDCWD, (positive)source,
                                     AT_FDCWD, (positive)destination, 0);

        if (done == 0)
                return;

        if (done == -ERROR_CROSS_DEVICE && mv_across(source, destination, FILE_MAX_DEPTH))
                return;

        string_format(file_fail, "mv: cannot move '%s' to '%s': %s\n", source,
                      destination, file_reason(done));
        mv_status = 1;
}

b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();

        file_take_options((string_address) "fivnT", address_of first);

        if (first + 1 >= count)
        {
                file_fail("mv: missing operand\n", 0);
                return 1;
        }

        string_address last = program_argument((b32)(count - 1));

        if (count - first == 2 && !file_is_directory_through(last))
        {
                mv_one(program_argument((b32)first), last);
                log_flush();
                return mv_status;
        }

        if (!file_is_directory_through(last))
        {
                string_format(file_fail, "mv: target '%s' is not a directory\n", last);
                return 1;
        }

        while (first < count - 1)
        {
                string_address source = program_argument((b32)first++);
                p8 tail[FILE_PATH_MAX];
                p8 destination[FILE_PATH_MAX];

                file_tail(source, tail);
                file_join(destination, FILE_PATH_MAX, last, tail);

                mv_one(source, destination);
        }

        log_flush();

        return mv_status;
}
