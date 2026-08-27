#include "../src/sh/file.c"

/*
        touch [-a] [-m] [-c] [-r REFERENCE] [-d SECONDS] FILE...

        -a and -m are what pick which of the two stamps moves; naming neither
        moves both, and naming one leaves the other exactly where it was
        rather than setting it to now, which is what UTIME_OMIT is for.

        -d here takes seconds since the epoch rather than a written date. A
        date parser would be most of a calendar library, and every test that
        wants a repeatable stamp can say it as a number.
*/
b32 main()
{
        positive first = 0;
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool access = false;
        bool modify = false;
        bool no_create = false;
        bool given = false;
        b64 seconds = 0;
        p32 nanoseconds = 0;

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

                if ((string_is(argument + 1, 'r') || string_is(argument + 1, 'd')) &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        string_address value = program_argument((b32)(index + 1));

                        if (string_is(argument + 1, 'r'))
                        {
                                file_facts facts;

                                if (!file_look_at(value, address_of facts))
                                {
                                        string_format(file_fail,
                                                      "touch: failed to get attributes of '%s'\n",
                                                      value);
                                        return 1;
                                }

                                seconds = facts.modified.seconds;
                                nanoseconds = facts.modified.nanoseconds;
                        }
                        else
                        {
                                // @N is how a date is spelled as a number of
                                // seconds, and it is the only spelling read
                                // here; a written date would be a calendar.
                                string_address digits = string_is(value, '@') ? value + 1
                                                                              : value;

                                if (!file_all_digits(string_is(digits, '-') ? digits + 1
                                                                            : digits))
                                {
                                        string_format(file_fail,
                                                      "touch: invalid date format: %s\n",
                                                      value);
                                        return 1;
                                }

                                seconds = file_signed(digits);
                                nanoseconds = 0;
                        }

                        given = true;
                        index += 2;
                        continue;
                }

                string_address letter = argument + 1;
                bool known = true;

                while (string_get(letter) && known)
                {
                        if (string_is(letter, 'a'))
                                access = true;
                        else if (string_is(letter, 'm'))
                                modify = true;
                        else if (string_is(letter, 'c'))
                                no_create = true;
                        else
                                known = false;

                        letter++;
                }

                if (!known)
                {
                        string_format(file_fail, "touch: unknown option: %s\n", argument);
                        return 1;
                }

                index++;
        }

        first = index;

        if (first >= count)
        {
                file_fail("touch: missing operand\n", 0);
                return 1;
        }

        if (!access && !modify)
        {
                access = true;
                modify = true;
        }

        p64 times[4];

        times[0] = given ? (p64)seconds : 0;
        times[1] = access ? (given ? (p64)nanoseconds : (p64)UTIME_NOW) : (p64)UTIME_OMIT;
        times[2] = given ? (p64)seconds : 0;
        times[3] = modify ? (given ? (p64)nanoseconds : (p64)UTIME_NOW) : (p64)UTIME_OMIT;

        b32 status = 0;

        while (first < count)
        {
                string_address path = program_argument((b32)first++);

                if (!file_exists(AT_FDCWD, path))
                {
                        if (no_create)
                                continue;

                        bipolar made = system_call_4(syscall(openat), AT_FDCWD,
                                                     (positive)path, FILE_WRITE & ~O_TRUNC,
                                                     0666);

                        if (made < 0)
                        {
                                string_format(file_fail, "touch: cannot touch '%s': %s\n",
                                              path, file_reason(made));
                                status = 1;
                                continue;
                        }

                        system_call_1(syscall(close), made);
                }

                bipolar done = system_call_4(syscall(utimensat), AT_FDCWD, (positive)path,
                                             (positive)times, 0);

                if (done < 0)
                {
                        string_format(file_fail, "touch: setting times of '%s': %s\n",
                                      path, file_reason(done));
                        status = 1;
                }
        }

        log_flush();

        return status;
}
