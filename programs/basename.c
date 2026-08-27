#include "../src/sh/file.c"

// basename NAME [SUFFIX], and the -a / -s forms that take many names.
static fn basename_one(string_address name, string_address suffix)
{
        p8 answer[FILE_PATH_MAX];

        file_tail(name, answer);

        if (suffix)
        {
                positive length = string_length(answer);
                positive cut = string_length(suffix);

                // A name that is nothing but its suffix keeps it: stripping
                // would leave an empty line where a name was asked for.
                if (cut > 0 && cut < length)
                {
                        positive i = 0;

                        while (i < cut && answer[length - cut + i] == string_get(suffix + i))
                                i++;

                        if (i == cut)
                                answer[length - cut] = end;
                }
        }

        file_line(answer);
}

b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool many = false;
        string_address suffix = null;

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

                if (string_is(argument + 1, 'a') && string_is(argument + 2, end))
                {
                        many = true;
                        index++;
                        continue;
                }

                if (string_is(argument + 1, 's') && string_is(argument + 2, end))
                {
                        if (index + 1 >= count)
                        {
                                file_fail("basename: -s needs a suffix\n", 0);
                                return 1;
                        }

                        suffix = program_argument((b32)(index + 1));
                        many = true;
                        index += 2;
                        continue;
                }

                if (string_is(argument + 1, 's'))
                {
                        suffix = argument + 2;
                        many = true;
                        index++;
                        continue;
                }

                string_format(file_fail, "basename: unknown option: %s\n", argument);
                return 1;
        }

        if (index >= count)
        {
                file_fail("basename: missing operand\n", 0);
                return 1;
        }

        if (!many && index + 1 < count)
                suffix = program_argument((b32)(index + 1));

        if (many)
        {
                while (index < count)
                        basename_one(program_argument((b32)index++), suffix);
        }
        else
                basename_one(program_argument((b32)index), suffix);

        log_flush();

        return 0;
}
