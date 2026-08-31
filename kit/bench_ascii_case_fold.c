/* Literal-size ASCII-folded compare against the out-of-line floor routine. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define INLINE_ALWAYS __attribute__((always_inline))
#define SUBJECTS 64
#define ROUNDS (1u << 19)
#define TRIES 9

static p8 left[SUBJECTS][40];
static p8 right[SUBJECTS][40];
static volatile positive sink;

#define BENCH_PAIRED_INDEXED
#include "bench_measure.c"

static inline INLINE_ALWAYS p8 case_byte(p8 value)
{
        return value >= 'a' && value <= 'z' ? (p8)(value - 32) : value;
}

static inline INLINE_ALWAYS b32 case_known(const p8 address_to one,
                                           const p8 address_to two,
                                           positive size)
{
        for (positive at = 0; at < size; at++)
        {
                p8 a = case_byte(one[at]);
                p8 b = case_byte(two[at]);

                if (a != b)
                        return (b32)a - (b32)b;
        }

        return 0;
}

static fn prepare(void)
{
        for (positive which = 0; which < SUBJECTS; which++)
                for (positive at = 0; at < 40; at++)
                {
                        p8 value = (p8)(at * 37 + which * 11);

                        left[which][at] = value;
                        right[which][at] =
                            value >= 'A' && value <= 'Z' && ((at + which) & 1)
                                ? (p8)(value + 32)
                                : value;
                }
}

#define DEFINE_SIZE(N)                                                        \
        NOT_INLINED static positive routine_##N(positive which)               \
        {                                                                     \
                return (positive)(memory_compare_ascii_case)(                 \
                    left[which], right[which], (N));                          \
        }                                                                     \
        NOT_INLINED static positive folded_##N(positive which)                \
        {                                                                     \
                return (positive)case_known(left[which], right[which], (N));  \
        }

DEFINE_SIZE(4)
DEFINE_SIZE(8)
DEFINE_SIZE(12)
DEFINE_SIZE(16)
DEFINE_SIZE(24)
DEFINE_SIZE(32)

static fn row(positive size, bench_indexed_work routine,
              bench_indexed_work folded)
{
        positive got = bench_paired_median(routine, folded);

        string_format(log, "  %p bytes: folded/routine %p.%p%%\n", size,
                      got / 100, got % 100);
}

b32 main(void)
{
        prepare();
        moonwater_cpu_detect();
        string_format(log, "literal ASCII-case compare, paired median of %p\n",
                      (positive)TRIES);
        row(4, routine_4, folded_4);
        row(8, routine_8, folded_8);
        row(12, routine_12, folded_12);
        row(16, routine_16, folded_16);
        row(24, routine_24, folded_24);
        row(32, routine_32, folded_32);
        log_flush();
        return 0;
}
