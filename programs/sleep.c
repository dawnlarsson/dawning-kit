#include "../src/sh/file.c"

/*
        sleep NUMBER[SUFFIX]..., where the number may have a fraction and the
        suffix is s, m, h or d. The fraction is read digit by digit into
        nanoseconds rather than through a decimal, so there is no rounding
        anywhere between the text and the timespec.
*/
static bool sleep_read(string_address text, p64 address_to seconds,
                       p64 address_to nanoseconds)
{
        p64 whole = 0;
        p64 fraction = 0;
        p64 scale = 100000000;
        bool any = false;

        while (string_get(text) >= '0' && string_get(text) <= '9')
        {
                whole = whole * 10 + (p64)(string_get(text) - '0');
                text++;
                any = true;
        }

        if (string_is(text, '.'))
        {
                text++;

                while (string_get(text) >= '0' && string_get(text) <= '9')
                {
                        if (scale > 0)
                        {
                                fraction += (p64)(string_get(text) - '0') * scale;
                                scale /= 10;
                        }

                        text++;
                        any = true;
                }
        }

        if (!any)
                return false;

        p64 multiplier = 1;

        if (string_is(text, 's'))
                text++;
        else if (string_is(text, 'm'))
        {
                multiplier = 60;
                text++;
        }
        else if (string_is(text, 'h'))
        {
                multiplier = 3600;
                text++;
        }
        else if (string_is(text, 'd'))
        {
                multiplier = 86400;
                text++;
        }

        if (string_get(text))
                return false;

        p64 total = whole * multiplier * 1000000000 + fraction * multiplier;

        address_to seconds = total / 1000000000;
        address_to nanoseconds = total % 1000000000;

        return true;
}

b32 main()
{
        positive count = (positive)program_argument_count();

        if (count < 2)
        {
                file_fail("sleep: missing operand\n", 0);
                return 1;
        }

        for (positive i = 1; i < count; i++)
        {
                p64 wanted[2] = {0, 0};

                if (!sleep_read(program_argument((b32)i), address_of wanted[0],
                                address_of wanted[1]))
                {
                        string_format(file_fail, "sleep: invalid time interval: %s\n",
                                      program_argument((b32)i));
                        return 1;
                }

                // A signal that arrives partway through leaves the remainder
                // in the second timespec, and the sleep goes on from there.
                p64 left[2] = {wanted[0], wanted[1]};

                while (system_call_2(syscall(nanosleep), (positive)left,
                                     (positive)left) < 0)
                {
                        if (left[0] == 0 && left[1] == 0)
                                break;
                }
        }

        return 0;
}
