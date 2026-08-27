#include "../src/library.c"
#include "../src/spark.c"

// Times fork + exec + wait for a target, repeatedly. The fork and wait cost is
// identical for both formats, so the difference between two runs is the cost
// of the loader.

#define CLOCK_MONOTONIC 1

// vfork semantics: the child borrows the parent's mm and the parent blocks
// until it execs. That skips the page table copy fork does purely so exec can
// throw it away, which is the cost worth knowing about.
#define AT_EMPTY_PATH 0x1000

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
        }

        return now_ns() - start;
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
        }

        positive elapsed = now_ns() - start;
        system_call_1(syscall(close), image);
        return elapsed;
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

        for (positive i = 0; i < ROUNDS; i++)
        {
                positive status = 0;
                if (spawned_pids[i] > 0)
                        system_call_4(syscall(wait4), spawned_pids[i],
                                      (positive)address_of status, 0, 0);
        }

        return elapsed;
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

        device = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        positive dev = 0;
        if (device >= 0)
        {
                bench_spawn_device("/tiny.spark");
                dev = bench_spawn_device("/tiny.spark");
                positive again = bench_spawn_device("/tiny.spark");
                if (again && again < dev)
                        dev = again;
        }
        else
        {
                string_format(log, "could not open %s: %b\n", SPARK_DEVICE, device);
                log_flush();
        }

        positive at_fd = best_of(null, 1);
        at_fd = bench_execveat("/tiny.spark");
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

        report("fork+wait only ", floor);
        report("fork  + elf    ", elf);
        report("fork  + spark  ", spark);
        report("vfork + elf    ", velf);
        report("vfork + spark  ", vspark);
        if (at_fd)
                report("fork + execveat ", at_fd);
        if (dev)
                report("/dev/spark     ", dev);
        if (nowait)
                report("/dev/spark nowait", nowait);

        struct stats stats;
        if (device >= 0 &&
            system_call_3(syscall(ioctl), device, SPARK_IOCTL_STATS,
                          (positive)address_of stats) == 0 && stats.spawns)
        {
                string_format(log, "\nkernel side, averaged over %p spawns:\n", stats.spawns);
                string_format(log, "  creating the task  %p ns\n", stats.task_ns / stats.spawns);
                string_format(log, "  loading the image  %p ns\n", stats.exec_ns / stats.spawns);
                if (stats.loads)
                {
                        positive loader = stats.loader_ns / stats.loads;
                        positive exec = stats.exec_ns / stats.spawns;
                        string_format(log, "    spark binfmt     %p ns\n", loader);
                        string_format(log, "      exec commit    %p ns\n",
                                      loader - stats.map_ns / stats.loads);
                        string_format(log, "      mapping regions %p ns\n",
                                      stats.map_ns / stats.loads);
                        string_format(log, "    generic prologue %p ns\n",
                                      exec > loader ? exec - loader : 0);
                }
                log_flush();
        }

        string_format(log, "load cost:  elf %p ns   spark %p ns\n",
                      (elf - floor) / ROUNDS, (spark - floor) / ROUNDS);
        string_format(log, "fork tax:   elf %p ns   spark %p ns\n",
                      (elf - velf) / ROUNDS, (spark - vspark) / ROUNDS);
        log_flush();

        return 0;
}
