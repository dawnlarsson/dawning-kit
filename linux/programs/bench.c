#include "../../standard/library.c"

// Times fork + exec + wait for a target, repeatedly. The fork and wait cost is
// identical for both formats, so the difference between two runs is the cost
// of the loader.

#define CLOCK_MONOTONIC 1
#define ROUNDS 400

timespec started;
timespec finished;

positive now_ns()
{
        timespec t;
        system_call_2(syscall(clock_gettime), CLOCK_MONOTONIC, (positive)address_of t);
        return (positive)t.tv_sec * 1000000000 + (positive)t.tv_nsec;
}

// path == null measures fork + exit + wait with no exec at all, which is the
// floor: whatever that costs is not the loader's fault. Subtracting it from
// the other two leaves the cost of actually loading the image.
positive run_many(string_address path)
{
        string_address argv[] = {path, null};
        positive start = now_ns();

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

                if (child == 0)
                {
                        if (path)
                                system_call_3(syscall(execve), (positive)path, (positive)argv, 0);
                        system_call_1(syscall(exit), 0);
                }

                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        }

        return now_ns() - start;
}

// Repeats a measurement and keeps the fastest, which is the one least
// disturbed by scheduling noise.
positive best_of(string_address path, positive tries)
{
        positive best = 0;

        for (positive i = 0; i < tries; i++)
        {
                positive t = run_many(path);
                if (!best || t < best)
                        best = t;
        }

        return best;
}

fn report(string_address label, positive total)
{
        string_format(log, "%s  total %p us   per exec %p ns\n",
                      label, total / 1000, total / ROUNDS);
        log_flush();
}

b32 main()
{
        // warm the page cache for both images first
        run_many("/tiny.elf");
        run_many("/tiny.spark");

        positive floor = best_of(null, 5);
        positive elf = best_of("/tiny.elf", 5);
        positive spark = best_of("/tiny.spark", 5);

        report("fork+wait only ", floor);
        report("elf            ", elf);
        report("spark          ", spark);

        string_format(log, "load cost: elf %p ns   spark %p ns\n",
                      (elf - floor) / ROUNDS, (spark - floor) / ROUNDS);
        log_flush();

        return 0;
}
