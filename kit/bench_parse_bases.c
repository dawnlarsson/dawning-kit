/*
        Bounded base-input parsing at the call shapes that were folded.

        The scalar columns are the exact fixed-base loops the shell used:
        octal has one contiguous digit range, hexadecimal folds two letter
        ranges, and base 36 is the generic classifier. Both columns are called
        through the same function-pointer shape so dispatch overhead is equal.

        Native runs are hardware ticks. Qemu runs are emulator work and only
        useful as regression ratios, never as hardware-floor measurements.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define SUBJECTS 64
#define ROUNDS (1u << 20)
#define TRIES 11

/*
        Hardware-counter mode builds one isolated side of one row:

          -DINPUT_BASE_PERF_ROW=1..5
          -DINPUT_BASE_PERF_ASSEMBLY=0|1

        Rows are octal short/word, hexadecimal short/word, then base 36.
        Both sides still use the same noinline indirect-call harness.
*/
#ifndef INPUT_BASE_PERF_ASSEMBLY
#define INPUT_BASE_PERF_ASSEMBLY 0
#endif

typedef positive (*fixed_parser)(string_address, positive, positive address_to);
typedef positive (*base_parser)(string_address, positive, positive,
                                positive address_to);

static p8 subjects[SUBJECTS][96];
static positive limits[SUBJECTS];
static volatile positive sink;

NOT_INLINED positive scalar_octal(string_address source, positive bound,
                                  positive address_to used)
{
        positive value = 0;
        positive at = 0;

        while (at < bound && source[at] >= '0' && source[at] <= '7')
        {
                value = (value << 3) + (positive)(source[at] - '0');
                at++;
        }

        if (used)
                address_to used = at;
        return value;
}

NOT_INLINED positive scalar_hex(string_address source, positive bound,
                                positive address_to used)
{
        positive value = 0;
        positive at = 0;

        while (at < bound)
        {
                p8 character = source[at];
                positive digit;

                if (character >= '0' && character <= '9')
                        digit = (positive)(character - '0');
                else if (character >= 'a' && character <= 'f')
                        digit = (positive)(character - 'a' + 10);
                else if (character >= 'A' && character <= 'F')
                        digit = (positive)(character - 'A' + 10);
                else
                        break;

                value = (value << 4) + digit;
                at++;
        }

        if (used)
                address_to used = at;
        return value;
}

NOT_INLINED positive scalar_generic(string_address source, positive bound,
                                    positive base, positive address_to used)
{
        positive value = 0;
        positive at = 0;

        while (at < bound)
        {
                p8 character = source[at];
                positive digit;

                if (character >= '0' && character <= '9')
                        digit = (positive)(character - '0');
                else if (character >= 'a' && character <= 'z')
                        digit = (positive)(character - 'a' + 10);
                else if (character >= 'A' && character <= 'Z')
                        digit = (positive)(character - 'A' + 10);
                else
                        break;

                if (digit >= base)
                        break;

                value = value * base + digit;
                at++;
        }

        if (used)
                address_to used = at;
        return value;
}

static fn make_subjects(positive base, positive maximum)
{
        for (positive which = 0; which < SUBJECTS; which++)
        {
                positive length = 1 + which % maximum;

                for (positive at = 0; at < length; at++)
                {
                        positive digit = (which * 13 + at * 17) % base;

                        subjects[which][at] =
                            (p8)(digit < 10 ? '0' + digit
                                            : ((at & 1) ? 'A' : 'a') + digit - 10);
                }

                // Included in the bound, so both parsers pay their stopping
                // classifier rather than timing only the all-valid fast path.
                subjects[which][length] = '@';
                limits[which] = length + 1;
        }
}

NOT_INLINED static p64 run_fixed(fixed_parser parser)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
        {
                positive which = round & (SUBJECTS - 1);
                positive used;
                positive value;

                value = parser(subjects[which], limits[which], address_of used);
                sink += value + used;
        }

        return get_cpu_time() - start;
}

NOT_INLINED static p64 run_base(base_parser parser, positive base)
{
        p64 start = get_cpu_time();

        for (positive round = 0; round < ROUNDS; round++)
        {
                positive which = round & (SUBJECTS - 1);
                positive used;
                positive value;

                value = parser(subjects[which], limits[which], base,
                               address_of used);
                sink += value + used;
        }

        return get_cpu_time() - start;
}

static p64 median(p64 values[TRIES])
{
        for (positive at = 1; at < TRIES; at++)
        {
                p64 value = values[at];
                positive before = at;

                while (before && values[before - 1] > value)
                {
                        values[before] = values[before - 1];
                        before--;
                }

                values[before] = value;
        }

        return values[TRIES / 2];
}

#ifdef INPUT_BASE_PERF_ROW
NOT_INLINED static fn perf_fixed(fixed_parser parser)
{
        for (positive trial = 0; trial < TRIES; trial++)
                for (positive round = 0; round < ROUNDS; round++)
                {
                        positive which = round & (SUBJECTS - 1);
                        positive used;
                        positive value = parser(subjects[which], limits[which],
                                                address_of used);

                        sink += value + used;
                }
}

NOT_INLINED static fn perf_base(base_parser parser, positive base)
{
        for (positive trial = 0; trial < TRIES; trial++)
                for (positive round = 0; round < ROUNDS; round++)
                {
                        positive which = round & (SUBJECTS - 1);
                        positive used;
                        positive value = parser(subjects[which], limits[which],
                                                base, address_of used);

                        sink += value + used;
                }
}
#endif

static fn fixed_row(string_address name, fixed_parser scalar,
                    fixed_parser assembly, positive base, positive maximum)
{
        make_subjects(base, maximum);

        p64 scalar_samples[TRIES];
        p64 assembly_samples[TRIES];

        // Alternate order so frequency scaling or host contention cannot
        // consistently favor the scalar or assembly half of a row.
        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 scalar_ticks;
                p64 assembly_ticks;

                if (trial & 1)
                {
                        assembly_ticks = run_fixed(assembly);
                        scalar_ticks = run_fixed(scalar);
                }
                else
                {
                        scalar_ticks = run_fixed(scalar);
                        assembly_ticks = run_fixed(assembly);
                }

                scalar_samples[trial] = scalar_ticks;
                assembly_samples[trial] = assembly_ticks;
        }

        p64 old = median(scalar_samples);
        p64 floor = median(assembly_samples);

        string_format(log,
                      "  %s: scalar %p  assembly %p  assembly/scalar %p%%\n",
                      name, (positive)old, (positive)floor,
                      (positive)(floor * 100 / (old ? old : 1)));
}

static fn base_row(string_address name, base_parser scalar, positive base,
                   positive maximum)
{
        make_subjects(base, maximum);

        p64 scalar_samples[TRIES];
        p64 assembly_samples[TRIES];

        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 scalar_ticks;
                p64 assembly_ticks;

                if (trial & 1)
                {
                        assembly_ticks = run_base(string_digits_base_max, base);
                        scalar_ticks = run_base(scalar, base);
                }
                else
                {
                        scalar_ticks = run_base(scalar, base);
                        assembly_ticks = run_base(string_digits_base_max, base);
                }

                scalar_samples[trial] = scalar_ticks;
                assembly_samples[trial] = assembly_ticks;
        }

        p64 old = median(scalar_samples);
        p64 floor = median(assembly_samples);

        string_format(log,
                      "  %s: scalar %p  assembly %p  assembly/scalar %p%%\n",
                      name, (positive)old, (positive)floor,
                      (positive)(floor * 100 / (old ? old : 1)));
}

b32 main()
{
#ifdef INPUT_BASE_PERF_ROW
#if INPUT_BASE_PERF_ROW == 1
        make_subjects(8, 3);
#if INPUT_BASE_PERF_ASSEMBLY
        perf_fixed(string_digits_octal_escape_max);
#else
        perf_fixed(scalar_octal);
#endif
#elif INPUT_BASE_PERF_ROW == 2
        make_subjects(8, 22);
#if INPUT_BASE_PERF_ASSEMBLY
        perf_fixed(string_digits_octal_max);
#else
        perf_fixed(scalar_octal);
#endif
#elif INPUT_BASE_PERF_ROW == 3
        make_subjects(16, 2);
#if INPUT_BASE_PERF_ASSEMBLY
        perf_fixed(string_digits_hexadecimal_escape_max);
#else
        perf_fixed(scalar_hex);
#endif
#elif INPUT_BASE_PERF_ROW == 4
        make_subjects(16, 16);
#if INPUT_BASE_PERF_ASSEMBLY
        perf_fixed(string_digits_hexadecimal_max);
#else
        perf_fixed(scalar_hex);
#endif
#elif INPUT_BASE_PERF_ROW == 5
        make_subjects(36, 13);
#if INPUT_BASE_PERF_ASSEMBLY
        perf_base(string_digits_base_max, 36);
#else
        perf_base(scalar_generic, 36);
#endif
#else
#error INPUT_BASE_PERF_ROW must be 1 through 5
#endif
        return sink == (positive)-1;
#else
        string_format(log, "bounded base parsing, paired median of %p, %p calls\n",
                      (positive)TRIES, (positive)ROUNDS);
        fixed_row((string_address)"octal escape, 1-3", scalar_octal,
                  string_digits_octal_escape_max, 8, 3);
        fixed_row((string_address)"octal word, 1-22", scalar_octal,
                  string_digits_octal_max, 8, 22);
        fixed_row((string_address)"hex escape, 1-2", scalar_hex,
                  string_digits_hexadecimal_escape_max, 16, 2);
        fixed_row((string_address)"hex word, 1-16", scalar_hex,
                  string_digits_hexadecimal_max, 16, 16);
        base_row((string_address)"base 36, 1-13", scalar_generic, 36, 13);
        log_flush();
        return 0;
#endif
}
