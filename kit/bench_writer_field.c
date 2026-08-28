/*
        The counted and string field primitives against the literal C helper
        shape they replace: compute minimum-width padding, preserve one writer
        call per pad byte, and make one counted body call only when nonempty.

        The string rows include exact/narrow fields of length 0, 1, 8 and 64
        and their width-64 padded forms. That makes the fixed wrapper/frame
        cost visible instead of letting a long padding loop hide it. Native
        runs are hardware ticks; qemu ratios are emulator work only.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 7
#define TARGET_CALLBACKS (1u << 20)
#define MAX_ROUNDS (1u << 18)
#define MIN_ROUNDS (1u << 12)

static p8 texts[4][80];
static const positive lengths[4] = {0, 1, 8, 64};
static volatile positive sink;

NOT_INLINED fn discard_writer(address_any data, positive length)
{
        volatile p8 address_to bytes = (volatile p8 address_to)data;

        sink += length + bytes[0] + bytes[length - 1];
}

NOT_INLINED fn former_writer_field(writer write, address_any data,
                                   positive length, positive width,
                                   b8 pad, bool left)
{
        positive padding = width > length ? width - length : 0;

        if (!left)
                writer_fill(write, padding, (p8)pad);

        if (length)
                write(data, length);

        if (left)
                writer_fill(write, padding, (p8)pad);
}

NOT_INLINED fn former_string_to_field(writer write, string_address text,
                                      positive width, b8 pad, bool left)
{
        positive length = string_length(text);
        positive padding = width > length ? width - length : 0;

        if (!left)
                writer_fill(write, padding, (p8)pad);

        if (length)
                write(text, length);

        if (left)
                writer_fill(write, padding, (p8)pad);
}

static positive rounds_for(positive length, positive width)
{
        positive callbacks = (width > length ? width - length : 0) +
                             (length != 0);
        positive rounds = TARGET_CALLBACKS / (callbacks ? callbacks : 1);

        if (rounds > MAX_ROUNDS)
                rounds = MAX_ROUNDS;
        if (rounds < MIN_ROUNDS)
                rounds = MIN_ROUNDS;
        return rounds;
}

static p64 raw_once(bool assembly, positive shape, positive width,
                    bool left, positive rounds)
{
        positive length = lengths[shape];
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
                if (assembly)
                        writer_field(discard_writer, texts[shape], length,
                                     width, (b8)0xa5, left);
                else
                        former_writer_field(discard_writer, texts[shape], length,
                                            width, (b8)0xa5, left);

        return get_cpu_time() - start;
}

static p64 string_once(bool assembly, positive shape, positive width,
                       bool left, positive rounds)
{
        p64 start = get_cpu_time();

        for (positive i = 0; i < rounds; i++)
                if (assembly)
                        string_to_field(discard_writer, texts[shape], width,
                                        (b8)0xa5, left);
                else
                        former_string_to_field(discard_writer, texts[shape], width,
                                               (b8)0xa5, left);

        return get_cpu_time() - start;
}

static fn row(string_address name, bool string_form, positive shape,
              positive width, bool left)
{
        positive rounds = rounds_for(lengths[shape], width);
        p64 former = positive_max;
        p64 assembly = positive_max;

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 one;
                p64 two;

                if (trial & 1)
                {
                        two = string_form
                                  ? string_once(true, shape, width, left, rounds)
                                  : raw_once(true, shape, width, left, rounds);
                        one = string_form
                                  ? string_once(false, shape, width, left, rounds)
                                  : raw_once(false, shape, width, left, rounds);
                }
                else
                {
                        one = string_form
                                  ? string_once(false, shape, width, left, rounds)
                                  : raw_once(false, shape, width, left, rounds);
                        two = string_form
                                  ? string_once(true, shape, width, left, rounds)
                                  : raw_once(true, shape, width, left, rounds);
                }

                if (one < former)
                        former = one;
                if (two < assembly)
                        assembly = two;
        }

        string_format(log,
                      "  %s (%p calls): former-C %p  assembly %p  asm/C %p%%\n",
                      name, rounds, (positive)former, (positive)assembly,
                      (positive)(assembly * 100 / (former ? former : 1)));
}

b32 main()
{
        for (positive shape = 0; shape < 4; shape++)
        {
                positive length = lengths[shape];

                for (positive i = 0; i < length; i++)
                        texts[shape][i] = (p8)(1 + (i * 47 + length) % 255);
                texts[shape][length] = end;
        }

        moonwater_cpu_detect();
        string_format(log, "writer fields, best of %p\n", (positive)TRIES);

        row((string_address)"raw empty width 0", false, 0, 0, false);
        row((string_address)"raw 1 width 1", false, 1, 1, false);
        row((string_address)"raw 8 width 8", false, 2, 8, false);
        row((string_address)"raw 1 width 8 right", false, 1, 8, false);
        row((string_address)"raw 1 width 8 left", false, 1, 8, true);

        row((string_address)"string 0 width 0", true, 0, 0, false);
        row((string_address)"string 1 width 1", true, 1, 1, false);
        row((string_address)"string 8 width 8", true, 2, 8, false);
        row((string_address)"string 64 width 64", true, 3, 64, false);
        row((string_address)"string 0 width 64 right", true, 0, 64, false);
        row((string_address)"string 1 width 64 right", true, 1, 64, false);
        row((string_address)"string 8 width 64 right", true, 2, 64, false);
        row((string_address)"string 8 width 64 left", true, 2, 64, true);

        log_flush();
        return 0;
}
