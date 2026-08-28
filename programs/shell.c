#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

b32 main()
{
        b32 interactive;
        bipolar input = 0;
        bool script_file = false;
        positive process_arguments;

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

        shell_signals_inherit();

        shell_ignore(SIGNAL_INTERRUPT);
        shell_ignore(SIGNAL_QUIT);

        shell_env_init();
        expand_shell_pid = (positive)system_call_1(syscall(getpid), 0);

        /*
                sh file [word ...]

                The process arguments are not commands: the first one names
                the input file, and everything after it is the script's
                positional-parameter list. Keep the bytes in the shell's own
                parameter store before command parsing starts, because the
                command argv table is reused for every line it runs.

                The interpreter keeps the file descriptor apart from standard
                input. A read command inside the script still reads what the
                script's caller sent on descriptor zero; only this outer
                reader consumes the source file.
        */
        process_arguments = (positive)program_argument_count();

        if (process_arguments > 1)
        {
                string_address script = program_argument(1);
                positive count = process_arguments - 2;
                bipolar handle = system_call_3(syscall(openat), AT_FDCWD,
                                                (positive)script, FILE_READ);
                positive at;

                if (handle < 0)
                {
                        string_format(log_error, "sh: %s: cannot open\n", script);
                        return 2;
                }

                if (!shell_room((address_any address_to)address_of shell_argv,
                                address_of shell_argv_room, count + 1,
                                sizeof(shell_argv[0])))
                {
                        system_call_1(syscall(close), (positive)handle);
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                for (at = 0; at < count; at++)
                        shell_argv[at] = program_argument((b32)(at + 2));

                shell_argv[count] = null;

                if (!shell_parameters_set(shell_argv, count))
                {
                        system_call_1(syscall(close), (positive)handle);
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                shell_script_name = script;
                shell_option_flags = (string_address) "";
                input = handle;
                script_file = true;
        }

        interactive = shell_is_interactive = !script_file && shell_interactive();

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

                got = system_call_3(syscall(read), (positive)input,
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

        if (script_file)
                system_call_1(syscall(close), (positive)input);

        // Input ran out, which is a way of leaving like any other.
        shell_trap_exit();

        log_flush();
        return shell_status;
}
