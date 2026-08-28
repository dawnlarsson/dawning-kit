/*
        The assembly padded-decimal writer against the exact C call shape it
        replaced: convert into twenty-four bytes, emit every pad byte in its
        own writer call, then emit the optional prefix and the digit run.

        A native run is hardware time. A qemu run is emulator work only.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define VALUE_COUNT 64
#define ROUNDS (1u << 15)
#define TRIES 7

static volatile positive sink;
static positive values[VALUE_COUNT];

NOT_INLINED fn discard_writer(address_any data, positive length)
{
        volatile p8 address_to bytes = (volatile p8 address_to)data;

        sink += length + bytes[0] + bytes[length - 1];
}

NOT_INLINED fn former_padded(writer write, positive value, positive width,
                             p8 pad, p8 prefix)
{
        p8 digits[24];
        positive length = positive_into(digits, value);
        positive occupied = length + (prefix != 0);

        while (pad && width > occupied)
        {
                write(address_of pad, 1);
                width--;
        }

        if (prefix)
                write(address_of prefix, 1);

        write(digits, length);
}

static fn make_values()
{
        positive state = 0x9e3779b97f4a7c15ull;

        for (positive i = 0; i < VALUE_COUNT; i++)
        {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                values[i] = i < 32 ? i * 313u : state;
        }

        values[0] = 0;
        values[1] = 9;
        values[2] = 10;
        values[3] = 99;
        values[4] = 100;
        values[5] = 9999;
        values[6] = 10000;
        values[7] = positive_max;
}

static p64 former_once(positive width, p8 prefix)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
                former_padded(discard_writer,
                              values[r & (VALUE_COUNT - 1)], width,
                              ' ', prefix);

        return get_cpu_time() - start;
}

static p64 assembly_once(positive width, p8 prefix)
{
        p64 start = get_cpu_time();

        for (positive r = 0; r < ROUNDS; r++)
                positive_to_padded(discard_writer,
                                   values[r & (VALUE_COUNT - 1)], width,
                                   ' ', prefix);

        return get_cpu_time() - start;
}

static fn row(string_address name, positive width, p8 prefix)
{
        p64 former = positive_max;
        p64 assembly = positive_max;

        // Alternate who runs first. Frequency changes over a long callback
        // row otherwise consistently reward the first implementation and can
        // turn the same binary from an apparent win into an apparent loss.
        for (positive t = 0; t < TRIES; t++)
        {
                p64 got_former;
                p64 got_assembly;

                if (t & 1)
                {
                        got_assembly = assembly_once(width, prefix);
                        got_former = former_once(width, prefix);
                }
                else
                {
                        got_former = former_once(width, prefix);
                        got_assembly = assembly_once(width, prefix);
                }

                if (got_former < former)
                        former = got_former;

                if (got_assembly < assembly)
                        assembly = got_assembly;
        }

        string_format(log, "  %s: former-C %p  assembly %p  asm/C %p%%\n",
                      name, (positive)former, (positive)assembly,
                      (positive)(assembly * 100 / (former ? former : 1)));
}

b32 main()
{
        make_values();
        moonwater_cpu_detect();

        string_format(log, "padded decimal writer, best of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        row((string_address)"width 0", 0, 0);
        row((string_address)"small pad to 6", 6, 0);
        row((string_address)"wide pad to 32", 32, 0);
        row((string_address)"signed pad to 32", 32, '-');
        log_flush();
        return 0;
}
