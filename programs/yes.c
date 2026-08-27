#include "../src/sh/file.c"

// yes [STRING]..., until something downstream stops reading.
b32 main()
{
        p8 line[FILE_PATH_MAX];
        positive count = (positive)program_argument_count();
        positive length = 0;

        if (count < 2)
        {
                line[length++] = 'y';
        }
        else
        {
                for (positive i = 1; i < count; i++)
                {
                        string_address word = program_argument((b32)i);

                        if (i > 1 && length + 1 < FILE_PATH_MAX)
                                line[length++] = ' ';

                        for (positive j = 0; string_get(word + j) && length + 1 < FILE_PATH_MAX; j++)
                                line[length++] = string_get(word + j);
                }
        }

        line[length++] = '\n';

        // One write of many copies rather than one write per line: the same
        // bytes leave the program in a fraction of the system calls.
        p8 block[FILE_BLOCK * 4];
        positive filled = 0;

        while (filled + length <= sizeof(block))
        {
                memory_copy(block + filled, line, length);
                filled += length;
        }

        while (1)
        {
                if (system_call_3(syscall(write), stdout, (positive)block, filled) <= 0)
                        return 1;
        }

        return 0;
}
