/*
        Scalar references for the assembly string benchmarks.

        These stay out of line on purpose.  The public assembly is reached by
        a call too; inlining only this side would measure call elimination at
        short lengths instead of the byte loop the routines replaced.
*/

#ifndef DAWNING_BENCH_REFERENCE_C
#define DAWNING_BENCH_REFERENCE_C

#define BENCH_NOT_INLINED __attribute__((noinline, noclone))

BENCH_NOT_INLINED positive reference_length(string_address source)
{
        string_address step = source;

        while (string_get(step))
                step++;

        return step - source;
}

BENCH_NOT_INLINED b32 reference_compare(string_address source,
                                        string_address input)
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

BENCH_NOT_INLINED string_address reference_first_of(string_address source,
                                                     p8 character)
{
        while (string_get(source))
        {
                if string_is (source, character)
                        return source;

                source++;
        }

        return character ? null : source;
}

#undef BENCH_NOT_INLINED
#endif
