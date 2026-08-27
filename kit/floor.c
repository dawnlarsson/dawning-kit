/*
        Every routine in library.c, against the speed of the machine.

        The floor is a loop that reads the same memory and computes nothing.
        Nothing that reads its input once can beat it, so a routine's worth is
        how close it gets, and a routine already there is finished -- there is
        no faster version of it, only a faster machine.

        Traffic matters and is stated rather than hidden. A copy moves its
        payload twice across the bus and a compare reads two streams, so those
        are reported against a floor measured the same way. Without that a copy
        at the bus limit reads as fifty percent and looks like slack.

        The number to distrust is a small one: at four bytes a call costs more
        than the work, so the column says what the routine costs to reach, not
        how fast it runs. The large sizes are the ones that mean anything about
        the loop.

            sh kit/bench floor        every architecture
*/
#include "../src/library.c"

#define NOT_INLINED __attribute__((noinline))

static p8 one[1 << 22];
static p8 two[1 << 22];
static p8 out[1 << 22];

static positive sizes[] = {64, 4096, 65536, 1048576};

/*
        The floor: every byte read, at the widest load the machine has, and
        nothing computed with any of it.

        Stepping a cache line at a time was the first attempt and it is the
        wrong ceiling: it moves the same traffic but issues a sixty fourth of
        the loads, so in cache it beats anything that has to look at every
        byte, and every routine measured against it read as slack that was not
        there. A routine that examines each byte cannot be compared against a
        loop that skips sixty three of them.
*/
NOT_INLINED positive floor_one(p8 address_to p, positive n)
{
        positive s = 0;
        positive i = 0;

#if X64
        if (cpu_has_avx2)
        {
                for (; i + 128 <= n; i += 128)
                        ir("vpaddb (%0), %%ymm0, %%ymm0\n"
                           "vpaddb 32(%0), %%ymm0, %%ymm0\n"
                           "vpaddb 64(%0), %%ymm0, %%ymm0\n"
                           "vpaddb 96(%0), %%ymm0, %%ymm0\n"
                           :: "r"(p + i) : "ymm0");
        }
#elif ARM64
        for (; i + 64 <= n; i += 64)
                ir("ldp q0, q1, [%0]\n   ldp q2, q3, [%0, #32]\n"
                   :: "r"(p + i) : "v0", "v1", "v2", "v3");
#endif
        // a clobber of "memory" on the loads above would make the compiler
        // reload everything each turn and the floor would measure that
        // instead of the machine
        for (; i + 8 <= n; i += 8) s += address_to(p64 address_to)(p + i);
        for (; i < n; i++) s += p[i];
        return s;
}

NOT_INLINED positive floor_two(p8 address_to a, p8 address_to b, positive n)
{
        return floor_one(a, n) + floor_one(b, n);
}

static volatile positive sink;

#define TIMED(rounds, body)                                                   \
        ({                                                                    \
                p64 best = ~(p64)0;                                           \
                for (b32 r = 0; r < 5; r++)                                   \
                {                                                             \
                        p64 s = get_cpu_time();                               \
                        for (b32 k = 0; k < (rounds); k++) { body; }           \
                        p64 e = get_cpu_time() - s;                           \
                        if (e < best) best = e;                               \
                }                                                             \
                best;                                                         \
        })

static fn row(string_address name, positive size, p64 ours, p64 floor,
              positive traffic)
{
        // ticks are a free running counter, so a ratio is the only honest
        // thing to print; the percentage is against the same traffic
        positive pct = floor ? (positive)((floor * 100 * traffic) / (ours ? ours : 1)) : 0;

        string_format(log, "  %s", name);
        for (positive i = string_length(name); i < 24; i++)
                string_format(log, " ");
        string_format(log, "%p", size);
        for (positive i = 0; i < 9; i++) string_format(log, " ");
        string_format(log, "%p%%\n", pct);
}

b32 main(void)
{
        for (positive i = 0; i < sizeof(one); i++)
        {
                one[i] = (p8)(i % 251 + 1);
                two[i] = one[i];
        }

        moonwater_cpu_detect();

        string_format(log, "  routine                 size     %% of floor\n");
        string_format(log, "  ------------------------------------------\n");

        for (positive z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++)
        {
                positive n = sizes[z];
                b32 rounds = (b32)((1 << 24) / n) + 1;

                p64 f1 = TIMED(rounds, sink += floor_one(one, n));
                p64 f2 = TIMED(rounds, sink += floor_two(one, two, n));

                row("memory_fill", n, TIMED(rounds, memory_fill(out, 7, n)), f1, 2);
                row("memory_copy", n, TIMED(rounds, memory_copy(out, one, n)), f1, 2);
                row("memory_copy_fast", n, TIMED(rounds, memory_copy_fast(out, one, n)), f1, 2);
                row("memory_count", n, TIMED(rounds, sink += memory_count(one, n, 7)), f1, 1);
                row("memory_first_of", n, TIMED(rounds, sink += (positive)memory_first_of(one, 0, n)), f1, 1);
                row("memory_compare", n, TIMED(rounds, sink += (positive)memory_compare(one, two, n)), f2, 1);

                one[n - 1] = 0;
                row("string_length", n, TIMED(rounds, sink += string_length(one)), f1, 1);
                row("string_first_of", n, TIMED(rounds, sink += (positive)string_first_of(one, 0)), f1, 1);
                row("string_last_of_or_end", n, TIMED(rounds, sink += (positive)string_last_of_or_end(one, 3)), f1, 1);
                two[n - 1] = 0;
                row("string_compare", n, TIMED(rounds, sink += (positive)string_compare(one, two)), f2, 1);
                row("string_copy", n, TIMED(rounds, string_copy(out, one)), f1, 3);
                one[n - 1] = (p8)((n - 1) % 251 + 1);
                two[n - 1] = one[n - 1];

                string_format(log, "\n");
        }

        log_flush();
        return 0;
}
