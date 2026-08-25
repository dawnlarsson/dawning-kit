#include "../../standard/library.c"
#include "../../standard/spark.c"

#define label TERM_BOLD "[Init]" TERM_RESET " "
#define init_program "/shell"

// A shell that dies immediately would otherwise be restarted as fast as the
// machine can fork, forever. Backing off turns that into something a person
// can read and interrupt rather than a spin.
#define RESTART_BACKOFF_NS 250000000
#define RESTART_BACKOFF_AFTER 3

timespec restart_pause = {0, RESTART_BACKOFF_NS};

// PID 1 has two jobs: start the first program, and reap every orphan the
// system ever produces. It must not exec into the shell -- doing that makes
// the shell PID 1, so the kernel panics the moment the shell exits, and
// nothing is left to reap anything.

string_address init_argv[] = {init_program, null};
string_address init_envp[] = {null};

bipolar start_shell(b32 device)
{
        if (device >= 0)
        {
                struct spawn request;
                p8 argv_block[64];
                positive length = string_length(init_program) + 1;

                memory_copy(argv_block, init_program, length);

                request.path = (unsigned long)init_program;
                request.argv = (unsigned long)argv_block;
                request.argv_bytes = length;
                request.argv_count = 1;
                request.envp = 0;
                request.envp_bytes = 0;
                request.envp_count = 0;

                bipolar spawned = system_call_3(syscall(ioctl), device,
                                                SPARK_IOCTL_SPAWN,
                                                (positive)address_of request);

                if (spawned > 0)
                        return spawned;
        }

        // No spark device, or it refused: fall back to the portable path.
        // clone takes (flags, child_stack); passing neither leaves both as
        // whatever happened to be in those registers.
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                system_call_3(syscall(execve), (positive)init_program,
                              (positive)init_argv, (positive)init_envp);

                // Only reached if exec failed.
                system_call_1(syscall(exit), 127);
        }

        return child;
}

b32 main()
{
        system_call(syscall(setsid));

        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        bipolar shell = start_shell(device);

        if (shell < 0)
        {
                string_format(log, label "could not start %s: %b\n", init_program, shell);
                log_flush();
                return 1;
        }

        positive quick_exits = 0;

        while (1)
        {
                positive status = 0;

                // -1 reaps any child, not just the shell: as PID 1 every
                // orphan on the system is eventually ours to collect.
                bipolar reaped = system_call_4(syscall(wait4), -1,
                                               (positive)address_of status, 0, 0);

                if (reaped < 0)
                        continue;

                if (reaped != shell)
                        continue;

                string_format(log, label "%s exited (%p), restarting\n",
                              init_program, status >> 8 & 0xff);
                log_flush();

                // Only pause once restarts start coming back to back; a shell
                // someone exited on purpose should come straight back.
                if (++quick_exits > RESTART_BACKOFF_AFTER)
                        sleep(address_of restart_pause);

                shell = start_shell(device);

                if (shell < 0)
                {
                        string_format(log, label "could not restart %s: %b\n",
                                      init_program, shell);
                        log_flush();
                        return 1;
                }
        }
}
