#include "../src/library.c"
#include "../src/spark.c"

#define label TERM_BOLD "[Init]" TERM_RESET " "
#define init_program "/shell"

// A shell that dies immediately would otherwise be restarted as fast as the
// machine can fork, forever. Backing off turns that into something a person
// can read and interrupt rather than a spin.
#define RESTART_BACKOFF_NS 250000000
#define RESTART_BACKOFF_MAX_NS 4000000000
#define RESTART_BACKOFF_AFTER 3

// A shell that stayed up this long was doing its job, so whatever came before
// it is not a crash loop and the backoff starts over.
#define SHELL_SETTLED_NS 2000000000

#define CLOCK_MONOTONIC 1

// Raw kernel return values: there is no errno here, a failed call comes back
// as the negated error itself.
#define ERROR_INTERRUPTED (-4)
#define ERROR_NO_CHILDREN (-10)
#define ERROR_EXISTS (-17)

// PID 1 has two jobs: start the first program, and reap every orphan the
// system ever produces. It must not exec into the shell -- doing that makes
// the shell PID 1, so the kernel panics the moment the shell exits, and
// nothing is left to reap anything.

string_address init_argv[] = {init_program, null};
string_address init_envp[] = {null};

positive now_ns()
{
        timespec now = {0, 0};

        // A clock that will not answer leaves every lifetime measured as zero,
        // which counts every restart as a quick one: the backoff comes on
        // early rather than never.
        if (system_call_2(syscall(clock_gettime), CLOCK_MONOTONIC,
                          (positive)address_of now))
                return 0;

        return (positive)now.tv_sec * 1000000000 + (positive)now.tv_nsec;
}

fn pause_for(positive nanoseconds)
{
        timespec span = {nanoseconds / 1000000000, nanoseconds % 1000000000};

        sleep(address_of span);
}

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

// A wait status carries the signal in the low seven bits and the exit code in
// the next eight, so reading it as an exit code alone reports a crash as a
// clean exit of zero -- which is the one thing this line exists to make
// visible.
fn report_exit(positive status)
{
        positive signal = status & 0x7f;

        if (signal)
                string_format(log, label "%s killed by signal %p, restarting\n",
                              init_program, signal);
        else
                string_format(log, label "%s exited (%p), restarting\n",
                              init_program, status >> 8 & 0xff);

        log_flush();
}

/*
        A pty needs somewhere for its other end to appear.

        Not the kernel's job here even though the kernel does the other
        mounts: devpts registers itself with module_init, which for built-in
        code runs at the same initcall level the compositor starts at, and
        kernel/ links before fs/. By the time there is an init there is a
        devpts, and this is where a system mounts it anyway.
*/
fn mount_devpts()
{
        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                     (positive)"/dev/pts", 0755);

        if (made < 0 && made != ERROR_EXISTS)
        {
                string_format(log, label "/dev/pts could not be created: %b\n", made);
                log_flush();
        }

        bipolar mounted = system_call_5(syscall(mount), (positive)"devpts",
                                        (positive)"/dev/pts",
                                        (positive)"devpts", 0, 0);

        // Nothing needs a pty this early, so this is not worth refusing to
        // boot over. It is worth saying: without it the terminal fails much
        // later, and for a reason that looks nothing like this one.
        if (mounted < 0)
        {
                string_format(log, label "devpts mount failed: %b, terminals will not work\n",
                              mounted);
                log_flush();
        }
}

b32 main()
{
        system_call(syscall(setsid));
        mount_devpts();

        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        positive quick_exits = 0;
        positive backoff = 0;
        positive started = now_ns();
        bipolar wait_error = 0;

        bipolar shell = start_shell(device);

        // Returning from PID 1 panics the kernel, which on a machine with no
        // serial console says nothing at all. Retrying at a bounded rate keeps
        // the reason on screen instead.
        while (shell < 0)
        {
                string_format(log, label "could not start %s: %b\n",
                              init_program, shell);
                log_flush();
                pause_for(RESTART_BACKOFF_MAX_NS);

                started = now_ns();
                shell = start_shell(device);
        }

        while (1)
        {
                positive status = 0;

                // -1 reaps any child, not just the shell: as PID 1 every
                // orphan on the system is eventually ours to collect.
                bipolar reaped = system_call_4(syscall(wait4), -1,
                                               (positive)address_of status, 0, 0);

                if (reaped == ERROR_INTERRUPTED)
                        continue;

                // Retrying a failing wait as fast as the CPU allows is the one
                // way PID 1 can spin with nothing to show for it. Say it once
                // per run of the same error, then slow down.
                if (reaped < 0 && reaped != ERROR_NO_CHILDREN)
                {
                        if (reaped != wait_error)
                        {
                                wait_error = reaped;
                                string_format(log, label "wait failed: %b\n", reaped);
                                log_flush();
                        }

                        pause_for(RESTART_BACKOFF_NS);
                        continue;
                }

                if (reaped > 0)
                {
                        wait_error = 0;

                        if (reaped != shell)
                                continue;
                }

                // ECHILD means there is nothing left to wait for at all, so
                // the shell is gone whether or not anyone reported it.
                if (reaped == ERROR_NO_CHILDREN)
                {
                        string_format(log, label "nothing left to wait for, restarting %s\n",
                                      init_program);
                        log_flush();
                }
                else
                        report_exit(status);

                if (now_ns() - started >= SHELL_SETTLED_NS)
                {
                        quick_exits = 0;
                        backoff = 0;
                }
                else if (++quick_exits > RESTART_BACKOFF_AFTER)
                {
                        backoff = backoff ? backoff * 2 : RESTART_BACKOFF_NS;

                        if (backoff > RESTART_BACKOFF_MAX_NS)
                                backoff = RESTART_BACKOFF_MAX_NS;

                        string_format(log, label "%p restarts in a row, waiting %p ms\n",
                                      quick_exits, backoff / 1000000);
                        log_flush();

                        pause_for(backoff);
                }

                started = now_ns();
                shell = start_shell(device);

                // Retried here rather than by falling back into wait4,
                // which would have no children to wait for and would report
                // the shell as having died when it never started.
                while (shell < 0)
                {
                        string_format(log, label "could not start %s: %b\n",
                                      init_program, shell);
                        log_flush();
                        pause_for(RESTART_BACKOFF_MAX_NS);

                        started = now_ns();
                        shell = start_shell(device);
                }
        }
}
