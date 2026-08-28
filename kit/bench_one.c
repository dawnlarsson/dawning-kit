#include "../src/compiler_memory.c"

#ifndef LENGTH
#define LENGTH 4
#endif
#ifndef ROUNDS
#define ROUNDS 1
#endif
#ifndef WHICH
#define WHICH 0
#endif

#define NOT_INLINED __attribute__((noinline))

NOT_INLINED positive reference_length(string_address source)
{
        string_address step = source;

        while (string_get(step))
                step++;

        return step - source;
}

NOT_INLINED b32 reference_compare(string_address source, string_address input)
{
        while (string_get(source) && string_get(input))
        {
                if string_not (source, address_to input)
                        break;

                source++;
                input++;
        }

        return string_get(source) - string_get(input);
}

NOT_INLINED string_address reference_first_of(string_address source, p8 character)
{
        while (string_get(source))
        {
                if string_is (source, character)
                        return source;

                source++;
        }

        return character ? null : source;
}

static p8 subject[8192];
static p8 mirror[8192];
static volatile positive sink;

b32 main()
{
        for (positive i = 0; i < LENGTH; i++)
                subject[i] = (p8)('a' + i % 26);

        subject[LENGTH] = 0;
        memory_copy_fast(mirror, subject, LENGTH + 1);

        for (positive r = 0; r < ROUNDS; r++)
        {
#if WHICH == 0
                sink += reference_length(subject);
#elif WHICH == 1
                sink += string_length(subject);
#elif WHICH == 2
                sink += (positive)reference_compare(subject, mirror);
#elif WHICH == 3
                sink += (positive)string_compare(subject, mirror);
#elif WHICH == 4
                sink += (positive)reference_first_of(subject, '#');
#elif WHICH == 5
                sink += (positive)string_first_of(subject, '#');
#endif
        }

        return 0;
}
