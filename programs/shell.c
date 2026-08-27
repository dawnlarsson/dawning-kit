#include "../src/library.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

b32 main()
{
        b32 interactive;

        /*
                One binary, forty names.

                The utilities are in here, so a link called grep pointing at
                this is grep -- there is nothing else to install and nothing
                that can be a version behind. Asked by its own name, or by any
                name that is not a tool's, it is a shell.
        */
        {
                b32 answered = shell_tool_as_called();

                if (answered >= 0)
                {
                        log_flush();
                        return answered;
                }
        }

        shell_ignore(SIGNAL_INTERRUPT);
        shell_ignore(SIGNAL_QUIT);

        interactive = shell_is_interactive = shell_interactive();

        shell_env_init();
        expand_shell_pid = (positive)system_call_1(syscall(getpid), 0);

        spawn_device = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        /*
                Whatever arrived, split into lines, with the last one held back
                if it has no newline yet.

                A read is not a command. A terminal hands over one line because
                the line discipline waits for Enter; a pipe or a file arrives
                in four kilobyte lumps that end wherever they end. Treating a
                lump as a line ran two commands as one at the start and split a
                command in half at every boundary -- a forty thousand line
                script came out with a hundred and forty three lines too many.
        */
        positive held = 0;

        while (1)
        {
                bipolar got;
                positive total, at;

                if (interactive)
                        log_direct(str(TERM_MAIN_BUFFER TERM_RESET TERM_SHOW_CURSOR PROMPT));

                got = system_call_3(syscall(read), 0,
                                    (positive)(shell_buffer + held),
                                    MAX_INPUT - 1 - held);

                if (got <= 0)
                        break;

                total = held + (positive)got;
                at = 0;

                while (at < total)
                {
                        positive stop = at;

                        while (stop < total && shell_buffer[stop] != '\n')
                                stop++;

                        // No newline yet: keep it for the next read rather than
                        // running half a command.
                        if (stop == total)
                                break;

                        shell_buffer[stop] = end;

                        if (stop > at)
                                run_line(shell_buffer + at);

                        at = stop + 1;
                }

                held = total - at;

                // A line longer than the buffer has nowhere left to grow, so
                // it is run as it stands rather than silently dropped.
                if (held >= MAX_INPUT - 1)
                {
                        shell_buffer[MAX_INPUT - 1] = end;
                        run_line(shell_buffer + at);
                        held = 0;
                }
                else if (held)
                {
                        memory_copy(shell_buffer, shell_buffer + at, held);
                }
        }

        // Whatever was still in hand when the input ended.
        if (held)
        {
                shell_buffer[held] = end;
                run_line(shell_buffer);
        }

        // Input ran out, which is a way of leaving like any other.
        shell_trap_exit();

        log_flush();
        return shell_status;
}
