#include "../src/sh/file.c"

/*
        seq LAST, seq FIRST LAST, seq FIRST INCREMENT LAST.

        Whole numbers only. The rest of this tree has no way to read a decimal
        out of a string, and a seq that printed a rounded 0.1 would be worse
        than one that says it cannot.
*/
static fn seq_write(writer write, bipolar value, positive width)
{
        p8 text[24];
        positive length;

        if (value < 0)
        {
                length = file_digits(text, (positive)(-value));

                for (positive i = length + 1; i < width; i++)
                        write("0", 1);

                write("-", 1);
                write(text, length);
                return;
        }

        length = file_digits(text, (positive)value);

        for (positive i = length; i < width; i++)
                write("0", 1);

        write(text, length);
}

static positive seq_width(bipolar value)
{
        p8 text[24];
        positive length = file_digits(text, value < 0 ? (positive)(-value) : (positive)value);

        return value < 0 ? length + 1 : length;
}

b32 main()
{
        positive count = (positive)program_argument_count();
        positive index = 1;
        bool pad = false;
        string_address separator = (string_address) "\n";

        while (index < count)
        {
                string_address argument = program_argument((b32)index);

                if (string_is(argument, '-') && string_is(argument + 1, 'w') &&
                    string_is(argument + 2, end))
                {
                        pad = true;
                        index++;
                        continue;
                }

                if (string_is(argument, '-') && string_is(argument + 1, 's') &&
                    string_is(argument + 2, end) && index + 1 < count)
                {
                        separator = program_argument((b32)(index + 1));
                        index += 2;
                        continue;
                }

                break;
        }

        positive given = count - index;

        if (given < 1 || given > 3)
        {
                file_fail("seq: needs one, two or three numbers\n", 0);
                return 1;
        }

        bipolar first = 1;
        bipolar step = 1;
        bipolar last;

        if (given == 1)
                last = file_signed(program_argument((b32)index));
        else if (given == 2)
        {
                first = file_signed(program_argument((b32)index));
                last = file_signed(program_argument((b32)(index + 1)));
        }
        else
        {
                first = file_signed(program_argument((b32)index));
                step = file_signed(program_argument((b32)(index + 1)));
                last = file_signed(program_argument((b32)(index + 2)));
        }

        if (step == 0)
        {
                file_fail("seq: increment must not be zero\n", 0);
                return 1;
        }

        positive width = 0;

        if (pad)
        {
                width = seq_width(first);

                if (seq_width(last) > width)
                        width = seq_width(last);
        }

        bipolar value = first;

        bool written = false;

        while (step > 0 ? value <= last : value >= last)
        {
                if (written)
                        log(separator, 0);

                seq_write(log, value, width);
                written = true;
                value += step;
        }

        // The separator goes between the numbers; the line still ends the way
        // every other line does.
        if (written)
                log("\n", 1);

        log_flush();

        return 0;
}
