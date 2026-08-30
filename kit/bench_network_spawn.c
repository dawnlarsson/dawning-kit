/*
        Boot-service launch floor: fork+exec against /dev/spark for the exact
        two-word /ip service shape, with an empty environment.

        Build into a Moonwater image and run there:

                sh kit/spark kit/bench_network_spawn fs/bench-network-spawn

        `ip link` exits, unlike `ip watch`, but enters the same multicall image
        with the same argv/env/fd contract. Its netlink work is identical on
        both paths, so the paired gap is the launch cost. Kernel-side Spark
        counters are reported over the device-spawn rounds as a second clock.
*/
#include "../src/compiler_memory.c"
#include "../src/spark.c"

#define CLOCK_MONOTONIC 1
#define ROUNDS 100
#define TRIES 3

static string_address ip_path = (string_address) "/ip";
static string_address ip_arguments[] = {(string_address) "/ip",
                                        (string_address) "link", null};
static string_address empty_environment[] = {null};
static b32 device = -1;

static positive now_ns(void)
{
        timespec now = {0, 0};

        system_call_2(syscall(clock_gettime), CLOCK_MONOTONIC,
                      (positive)address_of now);
        return (positive)now.tv_sec * 1000000000 + (positive)now.tv_nsec;
}

static positive run_fork_exec(void)
{
        positive started = now_ns();

        for (positive at = 0; at < ROUNDS; at++)
        {
                bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);
                positive status = 0;

                if (child == 0)
                {
                        system_call_3(syscall(execve), (positive)ip_path,
                                      (positive)ip_arguments,
                                      (positive)empty_environment);
                        system_call_1(syscall(exit), 127);
                }

                if (child < 0)
                        return 0;

                if (system_call_4(syscall(wait4), child,
                                  (positive)address_of status, 0, 0) != child ||
                    status)
                        return 0;
        }

        return now_ns() - started;
}

static positive run_spark(void)
{
        static p8 argv_block[] = "/ip\0link\0";
        struct spawn request = {
            .path = (unsigned long)"/ip",
            .argv = (unsigned long)argv_block,
            .argv_bytes = sizeof(argv_block) - 1,
            .argv_count = 2,
            .envp = 0,
            .envp_bytes = 0,
            .envp_count = 0,
        };
        positive started = now_ns();

        for (positive at = 0; at < ROUNDS; at++)
        {
                bipolar child = system_call_3(syscall(ioctl), device,
                                              SPARK_IOCTL_SPAWN,
                                              (positive)address_of request);
                positive status = 0;

                if (child < 0)
                        return 0;

                if (system_call_4(syscall(wait4), child,
                                  (positive)address_of status, 0, 0) != child ||
                    status)
                        return 0;
        }

        return now_ns() - started;
}

static positive best(positive (*run)(void))
{
        positive answer = 0;

        for (positive at = 0; at < TRIES; at++)
        {
                positive elapsed = run();

                if (!elapsed)
                        return 0;

                if (!answer || elapsed < answer)
                        answer = elapsed;
        }

        return answer;
}

b32 main(void)
{
        struct stats before = {0};
        struct stats after = {0};
        b32 terminal;
        b32 quiet;
        positive fork_ns;
        positive spark_ns;

        device = system_call_4(syscall(openat), AT_FDCWD,
                               (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);
        if (device < 0)
        {
                string_format(log_error, "cannot open %s: %b\n",
                              SPARK_DEVICE, device);
                return 1;
        }

        terminal = system_call_1(syscall(dup), standard_output_descriptor);
        quiet = system_call_4(syscall(openat), AT_FDCWD,
                              (positive)"/dev/null", FILE_WRITE, 0);
        if (terminal < 0 || quiet < 0)
                return 1;

        log_flush();
        system_call_3(syscall(dup3), quiet, standard_output_descriptor, 0);

        /* Warm image, netlink and scheduler paths before either timed best. */
        run_fork_exec();
        run_spark();

        fork_ns = best(run_fork_exec);
        system_call_3(syscall(ioctl), device, SPARK_IOCTL_STATS,
                      (positive)address_of before);
        spark_ns = best(run_spark);
        system_call_3(syscall(ioctl), device, SPARK_IOCTL_STATS,
                      (positive)address_of after);

        system_call_3(syscall(dup3), terminal, standard_output_descriptor, 0);
        system_call_1(syscall(close), quiet);
        system_call_1(syscall(close), terminal);

        if (!fork_ns || !spark_ns)
        {
                string_format(log_error, "a launch path failed\n");
                return 1;
        }

        string_format(log, "boot service /ip link, best of %p x %p\n",
                      (positive)TRIES, (positive)ROUNDS);
        string_format(log, "  fork+exec  %p ns/launch\n", fork_ns / ROUNDS);
        string_format(log, "  spark      %p ns/launch\n", spark_ns / ROUNDS);
        if (fork_ns >= spark_ns)
                string_format(log, "  saved      %p ns/launch\n",
                              (fork_ns - spark_ns) / ROUNDS);
        else
                string_format(log, "  extra      %p ns/launch\n",
                              (spark_ns - fork_ns) / ROUNDS);

        if (after.spawns > before.spawns)
        {
                positive count = after.spawns - before.spawns;

                string_format(log, "  kernel task %p ns, exec %p ns (%p spawns)\n",
                              (after.task_ns - before.task_ns) / count,
                              (after.exec_ns - before.exec_ns) / count, count);
        }

        log_flush();
        return 0;
}
