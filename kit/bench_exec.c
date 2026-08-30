#include "../src/compiler_memory.c"
#include "../src/spark.c"

// Times fork + exec + wait for a target, repeatedly. The fork and wait cost is
// identical for both formats, so the difference between two runs is the cost
// of the loader.
//
// A measurement and not a test, which is why it is here and not in src/test.
// It needs /dev/spark and the two images beside it, so it runs inside the
// booted image:
//
//     sh kit/spark kit/bench_tiny fs/tiny.spark
//     sh kit/spark kit/bench_exec fs/bench

#define CLOCK_MONOTONIC 1

#define AT_EMPTY_PATH 0x1000

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
        bool failed = false;

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_2(syscall(clone), flags, 0);

                if (child == 0)
                {
                        if (path)
                        {
                                system_call_3(syscall(execve), (positive)path, (positive)argv, 0);
                                // A successful exec never returns.  Keep a missing or
                                // rejected fixture from looking like an exceptionally
                                // fast child.
                                system_call_1(syscall(exit), 127);
                        }
                        system_call_1(syscall(exit), 0);
                }

                if (child < 0)
                {
                        failed = true;
                        continue;
                }

                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);

                if (status)
                        failed = true;
        }

        return failed ? 0 : now_ns() - start;
}

positive run_many(string_address path)
{
        return run_many_flags(path, SIGCHLD);
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
b32 device = -1;

positive bench_spawn_device(string_address path)
{
        struct spawn request;
        p8 argv_block[128];
        positive path_length = string_length(path);

        memory_copy(argv_block, path, path_length + 1);

        request.path = (unsigned long)path;
        request.argv = (unsigned long)argv_block;
        request.argv_bytes = path_length + 1;
        request.argv_count = 1;
        request.envp = 0;
        request.envp_bytes = 0;
        request.envp_count = 0;

        positive start = now_ns();
        bool failed = false;

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_3(syscall(ioctl), device,
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

                if (status)
                        failed = true;
        }

        return failed ? 0 : now_ns() - start;
}

// execveat on an already open descriptor skips pathname resolution entirely.
// The gap between this and execve by path is what the walk costs, and so what
// registering images up front could win back.
positive bench_execveat(string_address path)
{
        string_address argv[] = {path, null};
        b32 image = system_call_4(syscall(openat), AT_FDCWD, (positive)path, FILE_READ, 0);

        if (image < 0)
                return 0;

        positive start = now_ns();
        bool failed = false;

        for (positive i = 0; i < ROUNDS; i++)
        {
                bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

                if (child == 0)
                {
                        system_call_5(syscall(execveat), image, (positive) "",
                                      (positive)argv, 0, AT_EMPTY_PATH);
                        system_call_1(syscall(exit), 127);
                }

                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);

                if (status)
                        failed = true;
        }

        positive elapsed = now_ns() - start;
        system_call_1(syscall(close), image);
        return failed ? 0 : elapsed;
}

// Spawns without waiting, reaping afterwards outside the timed section, so the
// figure is the cost of starting a program rather than of the round trip.
b32 spawned_pids[ROUNDS];

positive bench_spawn_nowait(string_address path)
{
        struct spawn request;
        p8 argv_block[128];
        positive path_length = string_length(path);

        memory_copy(argv_block, path, path_length + 1);

        request.path = (unsigned long)path;
        request.argv = (unsigned long)argv_block;
        request.argv_bytes = path_length + 1;
        request.argv_count = 1;
        request.envp = 0;
        request.envp_bytes = 0;
        request.envp_count = 0;

        positive start = now_ns();

        for (positive i = 0; i < ROUNDS; i++)
                spawned_pids[i] = system_call_3(syscall(ioctl), device,
                                                SPARK_IOCTL_SPAWN,
                                                (positive)address_of request);

        positive elapsed = now_ns() - start;
        bool failed = false;

        for (positive i = 0; i < ROUNDS; i++)
        {
                positive status = 0;
                if (spawned_pids[i] > 0)
                {
                        system_call_4(syscall(wait4), spawned_pids[i],
                                      (positive)address_of status, 0, 0);
                        if (status)
                                failed = true;
                }
                else
                        failed = true;
        }

        return failed ? 0 : elapsed;
}

fn report(string_address label, positive total)
{
        string_format(log, "%s  total %p us   per exec %p ns\n",
                      label, total / 1000, total / ROUNDS);
        log_flush();
}

fn report_difference(string_address label, positive baseline, positive candidate)
{
        positive difference;
        positive tenths;

        if (!baseline || !candidate)
                return;

        if (candidate <= baseline)
        {
                difference = baseline - candidate;
                tenths = difference * 1000 / baseline;
                string_format(log, "%s  %p ns/exec faster  (%p.%p percent)\n",
                              label, difference / ROUNDS,
                              tenths / 10, tenths % 10);
        }
        else
        {
                difference = candidate - baseline;
                tenths = difference * 1000 / baseline;
                string_format(log, "%s  %p ns/exec slower  (%p.%p percent)\n",
                              label, difference / ROUNDS,
                              tenths / 10, tenths % 10);
        }

        log_flush();
}

b32 main()
{
        struct stats stats_before = {0};
        struct stats stats_after = {0};

        log_direct((string_address) "spark process floor\n", 20);

        // warm the page cache for both images first
        run_many("/tiny.elf");
        run_many("/tiny.spark");

        positive floor = best_of(null, 5);
        positive elf = best_of("/tiny.elf", 5);
        positive spark = best_of("/tiny.spark", 5);

        if (!floor || !elf || !spark)
        {
                log_error("benchmark child could not execute\n", 0);
                return 1;
        }

        device = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        positive dev = 0;
        if (device >= 0)
        {
                if (system_call_3(syscall(ioctl), device, SPARK_IOCTL_STATS,
                                  (positive)address_of stats_before))
                {
                        log_error("could not read initial Spark counters\n", 0);
                        return 1;
                }
                bench_spawn_device("/tiny.spark");
                dev = bench_spawn_device("/tiny.spark");
                positive again = bench_spawn_device("/tiny.spark");
                if (again && again < dev)
                        dev = again;
                if (system_call_3(syscall(ioctl), device, SPARK_IOCTL_STATS,
                                  (positive)address_of stats_after))
                {
                        log_error("could not read final Spark counters\n", 0);
                        return 1;
                }
        }
        else
        {
                string_format(log, "could not open %s: %b\n", SPARK_DEVICE, device);
                log_flush();
        }

        positive at_fd = bench_execveat("/tiny.spark");
        positive at2 = bench_execveat("/tiny.spark");
        if (at2 && at2 < at_fd)
                at_fd = at2;

        positive nowait = 0;
        if (device >= 0)
        {
                bench_spawn_nowait("/tiny.spark");
                nowait = bench_spawn_nowait("/tiny.spark");
                positive nw2 = bench_spawn_nowait("/tiny.spark");
                if (nw2 && nw2 < nowait)
                        nowait = nw2;
        }

        if (!at_fd || (device >= 0 && (!dev || !nowait)) ||
            (device >= 0 && stats_after.loads - stats_before.loads != 3 * ROUNDS))
        {
                log_error("benchmark launch path did not complete its work\n", 0);
                return 1;
        }

        log_direct((string_address) "results\n", 8);

        report("fork+wait only ", floor);
        report("fork  + elf    ", elf);
        report("fork  + spark  ", spark);
        if (at_fd)
                report("fork + execveat ", at_fd);
        if (dev)
                report("/dev/spark     ", dev);
        if (nowait)
                report("/dev/spark nowait", nowait);

        string_format(log, "\nclear comparisons (lower is faster):\n");
        report_difference("  Spark image versus ELF ", elf, spark);
        if (dev)
                report_difference("  fresh-mm versus fork  ", spark, dev);

        if (stats_after.spawns > stats_before.spawns)
        {
                positive spawns = stats_after.spawns - stats_before.spawns;
                positive loads = stats_after.loads - stats_before.loads;
                positive task = stats_after.task_ns - stats_before.task_ns;
                positive exec = stats_after.exec_ns - stats_before.exec_ns;
                positive loader = stats_after.loader_ns - stats_before.loader_ns;
                positive mapped = stats_after.map_ns - stats_before.map_ns;

                string_format(log, "\nkernel side, averaged over %p sequential spawns:\n",
                              spawns);
                string_format(log, "  completed image loads %p\n", loads);
                string_format(log, "  creating the task  %p ns\n", task / spawns);
                string_format(log, "  loading the image  %p ns\n", exec / spawns);
                if (loads)
                {
                        loader /= loads;
                        mapped /= loads;
                        exec /= spawns;
                        string_format(log, "    spark binfmt     %p ns\n", loader);
                        string_format(log, "      exec commit    %p ns\n",
                                      loader > mapped ? loader - mapped : 0);
                        string_format(log, "      mapping regions %p ns\n",
                                      mapped);
                        string_format(log, "    generic prologue %p ns\n",
                                      exec > loader ? exec - loader : 0);
                }
                log_flush();
        }

        string_format(log, "load cost:  elf %p ns   spark %p ns\n",
                      elf > floor ? (elf - floor) / ROUNDS : 0,
                      spark > floor ? (spark - floor) / ROUNDS : 0);
        log_flush();

        return 0;
}
