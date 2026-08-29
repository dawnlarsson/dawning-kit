#include "../src/compiler_memory.c"

/*
        The assembly against the C byte loop it replaced.

        library.c's string_length, string_compare and string_first_of forward
        to assembly on x86_64, arm64 and riscv64 and to a byte loop anywhere
        else. This runs both halves side by side at the lengths the choice
        actually turns on, so "a word at a time is faster" is a number rather
        than a belief.

        Foreign targets run under qemu, and a qemu tick is the emulator's work
        and not the machine's. What it measures honestly is how much less work
        there is to do; what it cannot tell you is what a real board would say.

        Built and run by kit/bench, which is the same freestanding link src/test/run
        uses.
*/

/*
        The C that string_length, string_compare and string_first_of fall
        through to under their last #else, copied verbatim from library.c.

        Kept out of line on purpose. The assembly is a call and can never be
        anything else, so letting the compiler inline the byte loop here would
        measure inlining rather than the loop -- at four bytes the call is most
        of the cost, and the comparison would say more about which one the
        optimiser could see through than about which one does less work.
*/
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

static positive lengths[] = {4, 8, 16, 32, 64, 256, 4096};

#define LENGTH_COUNT (sizeof(lengths) / sizeof(lengths[0]))
#define ROUNDS 65536

static p8 subject[8192];
static p8 mirror[8192];

// Somewhere the answers have to go that the optimiser cannot argue away.
static volatile positive sink;

fn line(string_address name, positive size, p64 byte_ticks, p64 word_ticks)
{
        string_format(log, "  %s %p bytes: byte %p  word %p\n",
                      name, size, (positive)byte_ticks, (positive)word_ticks);
}

b32 main()
{
        string_format(log, "assembly against the byte loop, %p calls each\n",
                      (positive)ROUNDS);

        for (positive e = 0; e < LENGTH_COUNT; e++)
        {
                positive size = lengths[e];

                for (positive i = 0; i < size; i++)
                        subject[i] = (p8)('a' + i % 26);

                subject[size] = 0;
                memory_copy_apart(mirror, subject, size + 1);

                p64 start = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += reference_length(subject);

                p64 middle = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += string_length(subject);

                p64 finish = get_cpu_time();

                line((string_address)"string_length   ", size,
                     middle - start, finish - middle);

                start = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += (positive)reference_compare(subject, mirror);

                middle = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += (positive)string_compare(subject, mirror);

                finish = get_cpu_time();

                line((string_address)"string_compare  ", size,
                     middle - start, finish - middle);

                // A character that is not there, so the whole string is walked
                // rather than however far the first 'q' happens to be.
                start = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += (positive)reference_first_of(subject, '#');

                middle = get_cpu_time();

                for (positive r = 0; r < ROUNDS; r++)
                        sink += (positive)string_first_of(subject, '#');

                finish = get_cpu_time();

                line((string_address)"string_first_of ", size,
                     middle - start, finish - middle);
        }

        log_flush();

        return 0;
}
