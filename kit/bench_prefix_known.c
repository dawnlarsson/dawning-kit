/* Literal-size equal-span walk against the out-of-line hardware routine.
   Mismatch positions are exhaustively guarded by src/test/exact_prefix.c;
   this harness intentionally measures the maximum-traffic shape. */
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline))
#define SUBJECTS 64
#define ROUNDS (1u << 19)
#define TRIES 9

static p8 left[SUBJECTS][144];
static p8 right[SUBJECTS][144];
static volatile positive sink;

#define BENCH_PAIRED_INDEXED
#include "bench_measure.c"

static fn prepare(void)
{
        for (positive which = 0; which < SUBJECTS; which++)
                for (positive at = 0; at < 144; at++)
                        left[which][at] = right[which][at] =
                            (p8)(at * 37 + which * 11);
}

#define DEFINE_SIZE(N)                                                        \
        NOT_INLINED static positive routine_##N(positive which)               \
        {                                                                     \
                return (memory_common_prefix)(left[which], right[which], (N));\
        }                                                                     \
        NOT_INLINED static positive folded_##N(positive which)                \
        {                                                                     \
                return common_prefix_known(left[which], right[which], (N));   \
        }

DEFINE_SIZE(1)
DEFINE_SIZE(2)
DEFINE_SIZE(3)
DEFINE_SIZE(4)
DEFINE_SIZE(5)
DEFINE_SIZE(6)
DEFINE_SIZE(7)
DEFINE_SIZE(8)
DEFINE_SIZE(9)
DEFINE_SIZE(10)
DEFINE_SIZE(11)
DEFINE_SIZE(12)
DEFINE_SIZE(13)
DEFINE_SIZE(14)
DEFINE_SIZE(15)
DEFINE_SIZE(16)
DEFINE_SIZE(17)
DEFINE_SIZE(18)
DEFINE_SIZE(19)
DEFINE_SIZE(20)
DEFINE_SIZE(21)
DEFINE_SIZE(22)
DEFINE_SIZE(23)
DEFINE_SIZE(24)
DEFINE_SIZE(31)
DEFINE_SIZE(32)
DEFINE_SIZE(40)
DEFINE_SIZE(64)
DEFINE_SIZE(80)
DEFINE_SIZE(96)
DEFINE_SIZE(127)
DEFINE_SIZE(128)

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
        string_format(log, "literal common prefix, paired median of %p\n",
                      (positive)TRIES);
#define ROW(N) row((N), routine_##N, folded_##N)
        ROW(1); ROW(2); ROW(3); ROW(4); ROW(5); ROW(6); ROW(7); ROW(8);
        ROW(9); ROW(10); ROW(11); ROW(12); ROW(13); ROW(14); ROW(15);
        ROW(16); ROW(17); ROW(18); ROW(19); ROW(20); ROW(21); ROW(22);
        ROW(23); ROW(24); ROW(31); ROW(32); ROW(40);
        ROW(64); ROW(80); ROW(96); ROW(127); ROW(128);
#undef ROW
        log_flush();
        return 0;
}
