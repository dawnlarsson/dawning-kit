/* Bulk hex output: the former checksum loop versus the shared ASM encoder. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 24)

static p8 input[MAXIMUM];
static p8 former_output[MAXIMUM * 2];
static p8 assembly_output[MAXIMUM * 2];
static volatile positive sink;

__attribute__((noinline, noclone)) static positive former_hex(
    p8 address_to destination, p8 address_to source, positive size)
{
        static const p8 digits[] = "0123456789abcdef";
        for (positive at = 0; at < size; at++)
        {
                destination[at * 2] = digits[source[at] >> 4];
                destination[at * 2 + 1] = digits[source[at] & 15];
        }
        return size * 2;
}

static p64 run(bool assembly, positive size, positive rounds)
{
        p64 start = get_cpu_time();
        for (positive at = 0; at < rounds; at++)
                sink += assembly ? memory_into_hex(assembly_output, input, size) :
                                   former_hex(former_output, input, size);
        return get_cpu_time() - start;
}

b32 main(void)
{
        static const positive sizes[] = {1, 8, 15, 16, 17, 32, 64, 4096, MAXIMUM};
        for (positive at = 0; at < MAXIMUM; at++)
                input[at] = (p8)(at * 197 + (at >> 8));
        for (positive row = 0; row < sizeof(sizes) / sizeof(sizes[0]); row++)
        {
                positive size = sizes[row];
                positive ratios[TRIES];
                positive rounds = TARGET_BYTES / size;
                if (rounds > (1u << 20))
                        rounds = 1u << 20;
                if (former_hex(former_output, input, size) !=
                    memory_into_hex(assembly_output, input, size) ||
                    memory_compare(former_output, assembly_output, size * 2))
                        return 1;
                for (positive trial = 0; trial < TRIES; trial++)
                {
                        p64 former, assembly;
                        if (trial & 1)
                        {
                                assembly = run(true, size, rounds);
                                former = run(false, size, rounds);
                        }
                        else
                        {
                                former = run(false, size, rounds);
                                assembly = run(true, size, rounds);
                        }
                        ratios[trial] = (positive)(assembly * 10000 / (former ? former : 1));
                }
                order(ratios, TRIES);
                string_format(log, "memory_into_hex %p bytes: paired median ASM/C %p.%p%%\n",
                              size, ratios[TRIES / 2] / 100, ratios[TRIES / 2] % 100);
        }
        log_flush();
        return 0;
}
