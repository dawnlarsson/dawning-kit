#include "../src/sh/file.c"

// id [-u|-g|-G] [-n] [-r], and the readable default when none of them is given.
#define ID_GROUPS_MAX 64

static fn id_named(positive value, bool group)
{
        p8 name[FILE_NAME_MAX];
        bool known = group ? file_group_name(value, name, FILE_NAME_MAX)
                           : file_user_name(value, name, FILE_NAME_MAX);

        file_number(log, value);

        if (known)
        {
                log("(", 1);
                log(name, 0);
                log(")", 1);
        }
}

b32 main()
{
        positive first = 0;
        positive flags = file_take_options((string_address) "ugGnr", address_of first);

        bool real = (flags & FILE_FLAG('r')) != 0;
        bool names = (flags & FILE_FLAG('n')) != 0;

        positive user = (positive)system_call(syscall(getuid));
        positive effective_user = (positive)system_call(syscall(geteuid));
        positive group = (positive)system_call(syscall(getgid));
        positive effective_group = (positive)system_call(syscall(getegid));

        if (!real)
        {
                user = effective_user;
                group = effective_group;
        }

        p8 name[FILE_NAME_MAX];

        if (flags & FILE_FLAG('u'))
        {
                if (names && file_user_name(user, name, FILE_NAME_MAX))
                        file_line(name);
                else
                {
                        file_number(log, user);
                        log("\n", 1);
                }

                log_flush();
                return 0;
        }

        if (flags & FILE_FLAG('g'))
        {
                if (names && file_group_name(group, name, FILE_NAME_MAX))
                        file_line(name);
                else
                {
                        file_number(log, group);
                        log("\n", 1);
                }

                log_flush();
                return 0;
        }

        p32 members[ID_GROUPS_MAX];
        bipolar have = system_call_2(syscall(getgroups), ID_GROUPS_MAX - 1,
                                     (positive)(members + 1));

        if (have < 0)
                have = 0;

        // The kernel hands the supplementary groups back in its own order and
        // does not promise the primary one is among them; the group actually
        // in effect belongs at the front.
        members[0] = (p32)group;

        positive keep = 1;

        for (positive i = 1; i <= (positive)have; i++)
                if (members[i] != (p32)group)
                        members[keep++] = members[i];

        have = (bipolar)keep;

        if (flags & FILE_FLAG('G'))
        {
                for (positive i = 0; i < (positive)have; i++)
                {
                        if (i)
                                log(" ", 1);

                        if (names && file_group_name(members[i], name, FILE_NAME_MAX))
                                log(name, 0);
                        else
                                file_number(log, members[i]);
                }

                log("\n", 1);
                log_flush();
                return 0;
        }

        log("uid=", 0);
        id_named(user, false);
        log(" gid=", 0);
        id_named(group, true);

        if (have > 0)
        {
                log(" groups=", 0);

                for (positive i = 0; i < (positive)have; i++)
                {
                        if (i)
                                log(",", 1);

                        id_named(members[i], true);
                }
        }

        log("\n", 1);
        log_flush();

        return 0;
}
