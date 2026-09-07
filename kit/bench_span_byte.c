/* Bounded equal-byte prefix: scalar caller loop against library assembly. */
#include "../src/compiler_memory.c"
#include "bench_measure.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define TRIES 9
#define MAXIMUM (1u << 20)
#define TARGET_BYTES (1u << 25)

static p8 block[MAXIMUM + 16];
static volatile positive sink;

NOT_INLINED static positive former_span(address_any address, p8 value,
                                        positive length)
{
        p8 address_to bytes = address;
        positive at = 0;

        while (at < length && bytes[at] == value)
                at++;

        return at;
}

typedef positive (*span_call)(address_any, p8, positive);
static span_call volatile span_calls[2] = {former_span, memory_span_byte};

static bool correctness(void)
{
        static const positive long_lengths[] = {255, 256, 257, 4095};

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive length = 0; length <= 20; length++)
                                for (positive mismatch = 0; mismatch <= length; mismatch++)
                                {
                                        memory_fill(block + offset, (p8)value, length);
                                        if (mismatch < length)
                                                block[offset + mismatch] = (p8)(value + 1);
                                        if (memory_span_byte(block + offset, (p8)value,
                                                             length) != mismatch)
                                                return false;
                                }

        for (positive value = 0; value < 256; value++)
                for (positive offset = 0; offset < 16; offset++)
                        for (positive li = 0;
                             li < sizeof(long_lengths) / sizeof(*long_lengths); li++)
                        {
                                positive length = long_lengths[li];
                                positive positions[] = {0, 1, 15, 16, length / 2,
                                                        length - 1, length};

                                for (positive pi = 0;
                                     pi < sizeof(positions) / sizeof(*positions); pi++)
                                {
                                        positive mismatch = positions[pi];

                                        memory_fill(block + offset, (p8)value, length);
                                        if (mismatch < length)
                                                block[offset + mismatch] = (p8)(value + 1);
                                        if (memory_span_byte(block + offset, (p8)value,
                                                             length) != mismatch)
                                                return false;
                                }
                        }
        return true;
}

static positive rounds_for(positive n){positive r=TARGET_BYTES/(n?n:1);if(r<8)r=8;if(r>(1u<<22))r=1u<<22;return r;}
static p64 run(positive implementation, positive n, positive rounds)
{
        p64 start = get_cpu_time();
        while (rounds--) sink += span_calls[implementation](block, '0', n);
        return get_cpu_time() - start;
}

static fn row(positive n, positive shape)
{
        positive raw[TRIES], rounds = rounds_for(n);
        memory_fill(block, '0', n);
        if (shape && n) block[shape == 1 ? n - 1 : 0] = '1';
        for (positive trial = 0; trial < TRIES; trial++)
        {
                p64 elapsed[2];
                for (positive i = 0; i < 2; i++)
                {
                        positive which = (i + trial) % 2;
                        elapsed[which] = run(which, n, rounds);
                }
                raw[trial] = elapsed[1] * 10000 / max(elapsed[0], (p64)1);
        }
        order(raw, TRIES);
        string_format(log, "  %s %p bytes  asm/C %p.%p%%\n",
                      shape == 2 ? "first" : shape ? "late" : "equal", n,
                      raw[TRIES / 2] / 100, raw[TRIES / 2] % 100);
}

b32 main(void)
{
        static const positive sizes[]={0,1,2,4,8,16,24,32,64,128,256,4096,MAXIMUM};
        if(!correctness()){string_format(log,"memory_span_byte correctness failed\n");log_flush();return 1;}
        string_format(log,"memory_span_byte, paired median of %p\n",(positive)TRIES);
        for (positive i = 0; i < array_count(sizes); i++)
                for (positive shape = 0; shape < 3; shape++) row(sizes[i], shape);
        log_flush();return 0;
}
