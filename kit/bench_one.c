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

#include "bench_reference.c"

static p8 subject[8192];
static p8 mirror[8192];
static volatile positive sink;

b32 main()
{
        for (positive i = 0; i < LENGTH; i++)
                subject[i] = (p8)('a' + i % 26);

        subject[LENGTH] = 0;
        memory_copy_apart(mirror, subject, LENGTH + 1);

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
