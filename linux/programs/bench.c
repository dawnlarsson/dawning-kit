#include "../../standard/library.c"
#include "../../standard/spark.h"

// Times fork + exec + wait for a target, repeatedly. The fork and wait cost is
// identical for both formats, so the difference between two runs is the cost
// of the loader.

#define CLOCK_MONOTONIC 1

// vfork semantics: the child borrows the parent's mm and the parent blocks
// until it execs. That skips the page table copy fork does purely so exec can
// throw it away, which is the cost worth knowing about.
#define CLONE_VM 0x00000100
#define CLONE_VFORK 0x00004000
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
positive run_many_flags(string_address path, positive flags)
{
        string_address argv[] = {path, null};
        positive start = now_ns();

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_2(syscall(clone), flags, 0);

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

positive run_many(string_address path)
{
        return run_many_flags(path, SIGCHLD);
}

positive run_many_vfork(string_address path)
{
        return run_many_flags(path, CLONE_VM | CLONE_VFORK | SIGCHLD);
}

// Repeats a measurement and keeps the fastest, which is the one least
// disturbed by scheduling noise.
positive best_of_flags(string_address path, positive flags, positive tries)
{
        positive best = 0;

        for (positive i = 0; i < tries; i++)
        {
                positive t = run_many_flags(path, flags);
                if (!best || t < best)
                        best = t;
        }

        return best;
}

positive best_of(string_address path, positive tries)
{
        return best_of_flags(path, SIGCHLD, tries);
}

// Spawns through /dev/spark, which creates the task with no address space to
// copy instead of forking one and throwing it away.
b32 spark_device = -1;

positive bench_spawn_device(string_address path)
{
        struct spark_spawn request;
        p8 argv_block[128];
        positive path_length = string_length(path);

        memory_copy(argv_block, path, path_length + 1);

        request.path = (unsigned long)path;
        request.argv = (unsigned long)argv_block;
        request.argv_bytes = path_length + 1;
        request.argv_count = 1;

        positive start = now_ns();

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_3(syscall(ioctl), spark_device,
                                              SPARK_IOCTL_SPAWN,
                                              (positive)address_of request);

                if (child < 0)
                {
                        string_format(log, "spawn ioctl failed: %b\n", child);
                        log_flush();
                        return 0;
                }

                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        }

        return now_ns() - start;
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

        positive vfl = CLONE_VM | CLONE_VFORK | SIGCHLD;

        positive floor = best_of(null, 5);
        positive elf = best_of("/tiny.elf", 5);
        positive spark = best_of("/tiny.spark", 5);
        positive vspark = best_of_flags("/tiny.spark", vfl, 5);
        positive velf = best_of_flags("/tiny.elf", vfl, 5);

        spark_device = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        positive dev = 0;
        if (spark_device >= 0)
        {
                bench_spawn_device("/tiny.spark");
                dev = bench_spawn_device("/tiny.spark");
                positive again = bench_spawn_device("/tiny.spark");
                if (again && again < dev)
                        dev = again;
        }
        else
        {
                string_format(log, "could not open %s: %b\n", SPARK_DEVICE, spark_device);
                log_flush();
        }

        report("fork+wait only ", floor);
        report("fork  + elf    ", elf);
        report("fork  + spark  ", spark);
        report("vfork + elf    ", velf);
        report("vfork + spark  ", vspark);
        if (dev)
                report("/dev/spark     ", dev);

        string_format(log, "load cost:  elf %p ns   spark %p ns\n",
                      (elf - floor) / ROUNDS, (spark - floor) / ROUNDS);
        string_format(log, "fork tax:   elf %p ns   spark %p ns\n",
                      (elf - velf) / ROUNDS, (spark - vspark) / ROUNDS);
        log_flush();

        return 0;
}
