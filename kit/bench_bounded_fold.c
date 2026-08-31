/*
        Literal-bound string operations at the production shapes which can
        actually reach compiler_memory.c's bounded specializers.

        HTTP compares seven and eight bytes, resolv.conf compares eleven, and
        interface names are copied through a fifteen-byte bound.  Each row
        keeps the literal at the call site on the expanded side and hides the
        same value behind a volatile load on the assembly-routine side.  The
        source rotates through short, full, equal, early-different and
        late-different cases so no row measures one friendly exit alone.
*/
#include "../src/compiler_memory.c"

#define NOT_INLINED __attribute__((noinline, noclone))
#define SUBJECTS 64
#define ROUNDS (1u << 19)
#define TRIES 9

static p8 left[SUBJECTS][32];
static p8 right[SUBJECTS][32];
static p8 target[SUBJECTS][32];
static volatile positive sink;

#define BENCH_PAIRED_INDEXED
#include "bench_measure.c"

static fn prepare(void)
{
        for (positive which = 0; which < SUBJECTS; which++)
        {
                positive stop = which & 15;

                for (positive at = 0; at < 32; at++)
                        left[which][at] = right[which][at] =
                            (p8)('a' + (which * 3 + at * 5) % 23);

                if ((which & 3) == 0)
                        left[which][stop] = right[which][stop] = 0;
                else if ((which & 3) == 1)
                        right[which][0] ^= 0x11;
                else if ((which & 3) == 2)
                        right[which][14] ^= 0x21;
        }
}

#define DEFINE_BOUND(B)                                                       \
        NOT_INLINED static positive length_routine_##B(positive which)        \
        {                                                                     \
                return (string_length_max)(left[which], (B));                 \
        }                                                                     \
        NOT_INLINED static positive length_folded_##B(positive which)         \
        {                                                                     \
                return string_length_max(left[which], (B));                   \
        }                                                                     \
        NOT_INLINED static b32 compare_routine_##B(positive which)            \
        {                                                                     \
                return (string_compare_max)(left[which], right[which],        \
                                            (B));                             \
        }                                                                     \
        NOT_INLINED static b32 compare_folded_##B(positive which)             \
        {                                                                     \
                return string_compare_max(left[which], right[which], (B));    \
        }                                                                     \
        NOT_INLINED static positive copy_routine_##B(positive which)          \
        {                                                                     \
                return (positive)(string_copy_max_end)(                       \
                    target[which], left[which], (B));                         \
        }                                                                     \
        NOT_INLINED static positive copy_folded_##B(positive which)           \
        {                                                                     \
                return (positive)string_copy_max_end(                         \
                    target[which], left[which], (B));                         \
        }

DEFINE_BOUND(7)
DEFINE_BOUND(8)
DEFINE_BOUND(11)
DEFINE_BOUND(15)

static fn show(string_address name, positive bound,
               bench_indexed_work routine, bench_indexed_work folded)
{
        positive got = bench_paired_median(routine, folded);

        string_format(log, "  %s %p: folded/routine %p.%p%%\n", name, bound,
                      got / 100, got % 100);
}

#define SHOW(B)                                                               \
        show((string_address)"length", (B), length_routine_##B,              \
             length_folded_##B);                                              \
        show((string_address)"compare", (B),                                \
             (bench_indexed_work)compare_routine_##B,                         \
             (bench_indexed_work)compare_folded_##B);                         \
        show((string_address)"copy-end", (B), copy_routine_##B,              \
             copy_folded_##B)

b32 main(void)
{
        prepare();
        moonwater_cpu_detect();
        string_format(log, "literal bounded strings, paired median of %p\n",
                      (positive)TRIES);
        SHOW(7);
        SHOW(8);
        SHOW(11);
        SHOW(15);
        log_flush();
        return 0;
}
