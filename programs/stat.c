#include "../src/sh/file.c"

/*
        stat [-L] [-c FORMAT] FILE...

        The default is a readable block. -c is the one that is meant to be
        parsed, so every specifier there prints exactly one field and nothing
        else -- %s is the size and not "size: 12".

        Times are UTC, and say so in the +0000 they carry.
*/
static bool stat_follow;
static b32 stat_status;

static fn stat_one_specifier(p8 letter, string_address path, file_facts address_to facts)
{
        p8 text[FILE_PATH_MAX];

        switch (letter)
        {
        case 'n':
                log(path, 0);
                return;

        case 'N':
                log("'", 1);
                log(path, 0);
                log("'", 1);

                if ((facts->mode & MODE_FORMAT) == MODE_LINK &&
                    file_link_text(path, text, FILE_PATH_MAX) >= 0)
                {
                        log(" -> '", 0);
                        log(text, 0);
                        log("'", 1);
                }

                return;

        case 's':
                return file_number(log, facts->size);

        case 'b':
                return file_number(log, facts->blocks);

        case 'B':
                return file_number(log, 512);

        case 'a':
                return file_octal(log, facts->mode & 07777, 1);

        case 'A':
                file_mode_letters(text, facts->mode);
                return log(text, 10);

        case 'f':
                return file_hexadecimal(log, facts->mode, 1);

        case 'F':
                return log(file_kind_name(facts->mode), 0);

        case 'h':
                return file_number(log, facts->hard_links);

        case 'i':
                return file_number(log, facts->inode);

        case 'u':
                return file_number(log, facts->owner);

        case 'g':
                return file_number(log, facts->group);

        case 'U':
                if (file_user_name(facts->owner, text, FILE_NAME_MAX))
                        return log(text, 0);

                return file_number(log, facts->owner);

        case 'G':
                if (file_group_name(facts->group, text, FILE_NAME_MAX))
                        return log(text, 0);

                return file_number(log, facts->group);

        case 'o':
                return file_number(log, facts->blocksize);

        case 'd':
                return file_number(log, facts->device_major * 256 + facts->device_minor);

        case 't':
                return file_hexadecimal(log, facts->rdev_major, 1);

        case 'T':
                return file_hexadecimal(log, facts->rdev_minor, 1);

        case 'X':
                return file_number(log, (positive)facts->accessed.seconds);

        case 'Y':
                return file_number(log, (positive)facts->modified.seconds);

        case 'Z':
                return file_number(log, (positive)facts->changed.seconds);

        case 'W':
                return file_number(log, (facts->mask & STATX_BIRTH)
                                            ? (positive)facts->created.seconds
                                            : 0);

        case 'x':
                return file_stamp(log, facts->accessed.seconds, facts->accessed.nanoseconds);

        case 'y':
                return file_stamp(log, facts->modified.seconds, facts->modified.nanoseconds);

        case 'z':
                return file_stamp(log, facts->changed.seconds, facts->changed.nanoseconds);

        case 'w':
                if (!(facts->mask & STATX_BIRTH))
                        return log("-", 1);

                return file_stamp(log, facts->created.seconds, facts->created.nanoseconds);

        case '%':
                return log("%", 1);
        }

        log("?", 1);
}

/*
        -c is a format, not a printf: the system's own stat reads backslash
        escapes only under --printf, and a format that said \t would print
        those two characters. So does this one.
*/
static fn stat_formatted(string_address format, string_address path,
                         file_facts address_to facts)
{
        string_address step = format;

        while (string_get(step))
        {
                if (string_is(step, '%') && string_get(step + 1))
                {
                        stat_one_specifier(string_get(step + 1), path, facts);
                        step += 2;
                        continue;
                }

                log(step, 1);
                step++;
        }

        log("\n", 1);
}

static fn stat_readable(string_address path, file_facts address_to facts)
{
        p8 text[FILE_PATH_MAX];

        log("  File: ", 0);
        log(path, 0);

        if ((facts->mode & MODE_FORMAT) == MODE_LINK &&
            file_link_text(path, text, FILE_PATH_MAX) >= 0)
        {
                log(" -> ", 0);
                log(text, 0);
        }

        log("\n  Size: ", 0);
        file_digits(text, facts->size);
        file_text_padded(log, text, 10);
        log("\tBlocks: ", 0);
        file_digits(text, facts->blocks);
        file_text_padded(log, text, 10);
        log(" IO Block: ", 0);
        file_digits(text, facts->blocksize);
        file_text_padded(log, text, 6);
        log(" ", 1);
        log(file_kind_name(facts->mode), 0);

        log("\nDevice: ", 0);
        file_number(log, facts->device_major);
        log(",", 1);
        file_number(log, facts->device_minor);
        log("\tInode: ", 0);
        file_digits(text, facts->inode);
        file_text_padded(log, text, 10);
        log("  Links: ", 0);
        file_number(log, facts->hard_links);

        log("\nAccess: (", 0);
        file_octal(log, facts->mode & 07777, 4);
        log("/", 1);
        file_mode_letters(text, facts->mode);
        log(text, 10);
        log(")  Uid: (", 0);
        file_number_padded(log, facts->owner, 5);
        log("/", 1);

        if (!file_user_name(facts->owner, text, FILE_NAME_MAX))
                file_digits(text, facts->owner);

        file_text_aligned(log, text, 8);

        log(")   Gid: (", 0);
        file_number_padded(log, facts->group, 5);
        log("/", 1);

        if (!file_group_name(facts->group, text, FILE_NAME_MAX))
                file_digits(text, facts->group);

        file_text_aligned(log, text, 8);

        log(")\nAccess: ", 0);
        file_stamp(log, facts->accessed.seconds, facts->accessed.nanoseconds);
        log("\nModify: ", 0);
        file_stamp(log, facts->modified.seconds, facts->modified.nanoseconds);
        log("\nChange: ", 0);
        file_stamp(log, facts->changed.seconds, facts->changed.nanoseconds);
        log("\n Birth: ", 0);

        if (facts->mask & STATX_BIRTH)
                file_stamp(log, facts->created.seconds, facts->created.nanoseconds);
        else
                log("-", 1);

        log("\n", 1);
}

b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        string_address format = null;

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

                if (string_is(argument + 1, 'L') && string_is(argument + 2, end))
                {
                        stat_follow = true;
                        index++;
                        continue;
                }

                if ((string_is(argument + 1, 'c') || string_is(argument + 1, 'f')) &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        format = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 'c') && string_get(argument + 2))
                {
                        format = argument + 2;
                        index++;
                        continue;
                }

                string_format(file_fail, "stat: unknown option: %s\n", argument);
                return 1;
        }

        if (index >= count)
        {
                file_fail("stat: missing operand\n", 0);
                return 1;
        }

        while (index < count)
        {
                string_address path = program_argument((b32)index++);
                file_facts facts;

                if (!file_look(AT_FDCWD, path, stat_follow ? 0 : AT_SYMLINK_NOFOLLOW,
                               address_of facts))
                {
                        string_format(file_fail,
                                      "stat: cannot statx '%s': No such file or directory\n",
                                      path);
                        stat_status = 1;
                        continue;
                }

                if (format)
                        stat_formatted(format, path, address_of facts);
                else
                        stat_readable(path, address_of facts);
        }

        log_flush();

        return stat_status;
}
