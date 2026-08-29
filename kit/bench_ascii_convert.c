/* ASCII case conversion: inlined former loops against reusable assembly. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define TRIES 7
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 24)

enum case_operation { LOWER_CASE, UPPER_CASE };
enum case_method {
        FORMER_LOOP,
        FORMER_FIXED,
        SCALAR_CALLS,
        BULK_ASSEMBLY,
        TABLE_ASSEMBLY
};

static p8 former_block[MAXIMUM + 16];
static p8 fixed_block[MAXIMUM + 16];
static p8 scalar_block[MAXIMUM + 16];
static p8 bulk_block[MAXIMUM + 16];
static p8 table_block[MAXIMUM + 16];
static p8 lower_table[256];
static p8 upper_table[256];
static volatile positive sink;

NOT_INLINED static address_any former_convert(p8 address_to block,
                                              positive length,
                                              enum case_operation operation)
{
        for (positive at = 0; at < length; at++)
        {
                p8 value = block[at];

                if (operation == LOWER_CASE)
                        block[at] = value >= 'A' && value <= 'Z'
                                        ? (p8)(value + 32) : value;
                else
                        block[at] = value >= 'a' && value <= 'z'
                                        ? (p8)(value - 32) : value;
        }

        return block;
}

// dd has selected its conv mode outside the loop, unlike the shell expansion
// loop above whose direction is data. Keep both caller shapes in the lane.
NOT_INLINED static address_any former_fixed_convert(
    p8 address_to block, positive length, enum case_operation operation)
{
        if (operation == LOWER_CASE)
                for (positive at = 0; at < length; at++)
                {
                        p8 value = block[at];
                        block[at] = value >= 'A' && value <= 'Z'
                                        ? (p8)(value + 32) : value;
                }
        else
                for (positive at = 0; at < length; at++)
                {
                        p8 value = block[at];
                        block[at] = value >= 'a' && value <= 'z'
                                        ? (p8)(value - 32) : value;
                }

        return block;
}

NOT_INLINED static address_any scalar_convert(p8 address_to block,
                                              positive length,
                                              enum case_operation operation)
{
        for (positive at = 0; at < length; at++)
                block[at] = operation == LOWER_CASE
                                ? byte_to_lower(block[at])
                                : byte_to_upper(block[at]);

        return block;
}

static fn fill(p8 address_to block, positive length, positive salt)
{
        for (positive at = 0; at < length; at++)
                block[at] = (p8)(at * 37 + salt);
}

static bool correctness_one(enum case_operation operation)
{
        p8 guarded[4128];
        p8 expected[4128];
        p8 fixed[4128];
        p8 scalar[4128];
        p8 bulk[4128];
        p8 table[4128];
        static const positive offsets[] = {0, 1, 7, 15, 31, 4093, 4095};
        static const positive lengths[] = {0, 1, 2, 15, 16, 17, 31, 32, 33};
        p8 address_to translation = operation == LOWER_CASE
                                        ? lower_table : upper_table;

        for (positive value = 0; value < 256; value++)
        {
                p8 byte = (p8)value;
                p8 wanted = operation == LOWER_CASE
                                ? (byte >= 'A' && byte <= 'Z'
                                       ? (p8)(byte + 32) : byte)
                                : (byte >= 'a' && byte <= 'z'
                                       ? (p8)(byte - 32) : byte);

                if ((operation == LOWER_CASE ? byte_to_lower(byte)
                                             : byte_to_upper(byte)) != wanted ||
                    translation[value] != wanted)
                        return false;
        }

        for (positive oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); oi++)
                for (positive li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++)
                {
                        positive offset = offsets[oi];
                        positive length = lengths[li];

                        if (offset + length > sizeof(guarded))
                                continue;

                        fill(guarded, sizeof(guarded), offset + length);
                        memory_copy(expected, guarded, sizeof(guarded));
                        memory_copy(fixed, guarded, sizeof(guarded));
                        memory_copy(scalar, guarded, sizeof(guarded));
                        memory_copy(bulk, guarded, sizeof(guarded));
                        memory_copy(table, guarded, sizeof(guarded));

                        former_convert(expected + offset, length, operation);
                        former_fixed_convert(fixed + offset, length, operation);
                        scalar_convert(scalar + offset, length, operation);
                        address_any bulk_answer = operation == LOWER_CASE
                                                      ? memory_to_lower_ascii(
                                                            bulk + offset, length)
                                                      : memory_to_upper_ascii(
                                                            bulk + offset, length);

                        if (bulk_answer != bulk + offset ||
                            memory_translate(table + offset, length, translation) !=
                            table + offset ||
                            memory_compare(guarded, expected, offset) ||
                            memory_compare(expected, fixed, sizeof(guarded)) ||
                            memory_compare(expected, scalar, sizeof(guarded)) ||
                            memory_compare(expected, bulk, sizeof(guarded)) ||
                            memory_compare(expected, table, sizeof(guarded)))
                                return false;
                }

        return true;
}

static positive rounds_for(positive length)
{
        positive rounds = TARGET_BYTES / length;

        if (rounds < 8)
                rounds = 8;
        if (rounds > (1u << 20))
                rounds = 1u << 20;
        return rounds;
}

static p64 run(enum case_method method, enum case_operation operation,
               positive length, positive rounds)
{
        p8 address_to block = method == FORMER_LOOP ? former_block
                              : method == FORMER_FIXED ? fixed_block
                              : method == SCALAR_CALLS ? scalar_block
                              : method == BULK_ASSEMBLY ? bulk_block
                                                       : table_block;
        p8 address_to translation = operation == LOWER_CASE
                                        ? lower_table : upper_table;
        p64 start = get_cpu_time();

        for (positive round = 0; round < rounds; round++)
        {
                address_any answer = method == FORMER_LOOP
                                         ? former_convert(block, length, operation)
                                     : method == FORMER_FIXED
                                         ? former_fixed_convert(block, length, operation)
                                     : method == SCALAR_CALLS
                                         ? scalar_convert(block, length, operation)
                                     : method == BULK_ASSEMBLY
                                         ? (operation == LOWER_CASE
                                                ? memory_to_lower_ascii(block, length)
                                                : memory_to_upper_ascii(block, length))
                                         : memory_translate(block, length, translation);
                sink += (positive)answer + block[0] + block[length - 1];
        }

        return get_cpu_time() - start;
}

static fn order(positive address_to values)
{
        for (positive i = 1; i < TRIES; i++)
        {
                positive value = values[i];
                positive at = i;

                while (at && values[at - 1] > value)
                {
                        values[at] = values[at - 1];
                        at--;
                }

                values[at] = value;
        }
}

static fn row(enum case_operation operation, positive length)
{
        positive scalar_ratios[TRIES];
        positive bulk_ratios[TRIES];
        positive fixed_ratios[TRIES];
        positive table_ratios[TRIES];
        positive rounds = rounds_for(length);

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 former;
                p64 fixed;
                p64 scalar;
                p64 bulk;
                p64 table;

                fill(former_block, length, trial + 11);
                memory_copy(fixed_block, former_block, length);
                memory_copy(scalar_block, former_block, length);
                memory_copy(bulk_block, former_block, length);
                memory_copy(table_block, former_block, length);

                if (trial & 1)
                {
                        table = run(TABLE_ASSEMBLY, operation, length, rounds);
                        bulk = run(BULK_ASSEMBLY, operation, length, rounds);
                        scalar = run(SCALAR_CALLS, operation, length, rounds);
                        fixed = run(FORMER_FIXED, operation, length, rounds);
                        former = run(FORMER_LOOP, operation, length, rounds);
                }
                else
                {
                        former = run(FORMER_LOOP, operation, length, rounds);
                        fixed = run(FORMER_FIXED, operation, length, rounds);
                        scalar = run(SCALAR_CALLS, operation, length, rounds);
                        bulk = run(BULK_ASSEMBLY, operation, length, rounds);
                        table = run(TABLE_ASSEMBLY, operation, length, rounds);
                }

                if (memory_compare(former_block, fixed_block, length) ||
                    memory_compare(former_block, scalar_block, length) ||
                    memory_compare(former_block, bulk_block, length) ||
                    memory_compare(former_block, table_block, length))
                {
                        string_format(log, "  result mismatch at %p bytes\n", length);
                        return;
                }

                scalar_ratios[trial] = (positive)(scalar * 10000 /
                                                   (former ? former : 1));
                bulk_ratios[trial] = (positive)(bulk * 10000 /
                                                 (former ? former : 1));
                fixed_ratios[trial] = (positive)(bulk * 10000 /
                                                  (fixed ? fixed : 1));
                table_ratios[trial] = (positive)(table * 10000 /
                                                  (former ? former : 1));
        }

        order(scalar_ratios);
        order(bulk_ratios);
        order(fixed_ratios);
        order(table_ratios);
        string_format(log,
                      "  %p bytes  scalar/dispatch %p.%p%%  bulk/dispatch %p.%p%%  bulk/fixed %p.%p%%  table/dispatch %p.%p%%\n",
                      length, scalar_ratios[TRIES / 2] / 100,
                      scalar_ratios[TRIES / 2] % 100,
                      bulk_ratios[TRIES / 2] / 100,
                      bulk_ratios[TRIES / 2] % 100,
                      fixed_ratios[TRIES / 2] / 100,
                      fixed_ratios[TRIES / 2] % 100,
                      table_ratios[TRIES / 2] / 100,
                      table_ratios[TRIES / 2] % 100);
}

b32 main(void)
{
        static const positive sizes[] = {1, 8, 16, 24, 32, 48, 64,
                                         128, 256, 4096, MAXIMUM};

        for (positive value = 0; value < 256; value++)
        {
                lower_table[value] = value >= 'A' && value <= 'Z'
                                         ? (p8)(value + 32) : (p8)value;
                upper_table[value] = value >= 'a' && value <= 'z'
                                         ? (p8)(value - 32) : (p8)value;
        }

        if (!correctness_one(LOWER_CASE) || !correctness_one(UPPER_CASE))
        {
                string_format(log, "ASCII conversion correctness failed\n");
                log_flush();
                return 1;
        }

        string_format(log, "ASCII lower, paired median of %p\n", (positive)TRIES);
        for (positive at = 0; at < sizeof(sizes) / sizeof(sizes[0]); at++)
                row(LOWER_CASE, sizes[at]);

        string_format(log, "ASCII upper, paired median of %p\n", (positive)TRIES);
        for (positive at = 0; at < sizeof(sizes) / sizeof(sizes[0]); at++)
                row(UPPER_CASE, sizes[at]);

        log_flush();
        return 0;
}
