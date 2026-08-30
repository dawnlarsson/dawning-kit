#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

b32 main()
{
        b32 interactive;
        bipolar input = 0;
        bool script_file = false;
        string_address command = null;
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

        shell_signals_start();

        /* environ is the live process environment. Ordinarily it is the
           kernel vector published by the startup shim; clone-and-reentry can
           deliberately replace it without forging another initial stack. */
        shell_env_init(environ);

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

        /*
                sh -c COMMAND [name [word ...]]

                The one spelling nearly everything else uses. system(), the
                shell a Makefile runs a recipe with, find -exec sh -c and
                xargs sh -c all arrive this way, and without it this is not a
                shell anything can call -- it read "-c" as a file name and
                said it could not open it.

                POSIX puts the command string in the first operand and, if
                there is another, makes it $0 and the rest the positional
                parameters. So `sh -c 'echo $0 $1' name one` prints "name one"
                rather than treating either as a word of the command.
        */
        if (process_arguments > 1 && string_equals(program_argument(1), "-c"))
        {
                positive count = process_arguments > 3 ? process_arguments - 4 : 0;
                positive at;

                if (process_arguments < 3)
                {
                        log_error("sh: -c wants a command\n", 0);
                        return 2;
                }

                command = program_argument(2);

                if (!shell_room((address_any address_to)address_of shell_argv,
                                address_of shell_argv_room, count + 1,
                                sizeof(shell_argv[0])))
                {
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                for (at = 0; at < count; at++)
                        shell_argv[at] = program_argument((b32)(at + 4));

                shell_argv[count] = null;

                if (!shell_parameters_set(shell_argv, count))
                {
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                //      With no name operand $0 is unspecified, and what
                //      dash does is use the path it was invoked as. That is
                //      more use than the word "sh" when a message has to say
                //      where it came from.
                shell_script_name = process_arguments > 3 ? program_argument(3)
                                                          : program_argument(0);
                shell_option_flags = (string_address) "c";
                script_file = true;
        }
        else if (process_arguments > 1)
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
        shell_options_started(interactive);

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
        /*
                A command string is not read from anywhere, so it is run and
                that is the whole of it.

                It is copied first because the lines are ended in place, and
                what came in is the process's own argument block. The copy is
                as long as the string, so a -c of any size works -- there is no
                buffer here to be longer than.
        */
        if (command)
        {
                string_address first_newline = string_first_of(command, '\n');

                /* The lexer and parser copy tokens and never write the source.
                   A single command can therefore stay in the process argument
                   block; only multi-line input needs a writable copy whose
                   newlines are ended in place. */
                if (!first_newline)
                        run_line(command);
                else
                {
                        positive length = string_length(command);
                        p8 address_to held_command =
                            (p8 address_to)memory(length + 1);

                        if (!held_command ||
                            (positive)held_command >= (positive)-4095)
                        {
                                log_error("sh: no room for the command\n", 0);
                                return 1;
                        }

                        memory_copy(held_command, command, length + 1);

                        {
                                p8 address_to at = held_command;

                                while (string_get(at))
                                {
                                        p8 address_to stop =
                                            string_first_of(at, '\n');

                                        if (stop)
                                                address_to stop = end;

                                        run_line(at);

                                        if (!stop)
                                                break;

                                        at = stop + 1;
                                }
                        }

                        memory_free(held_command, length + 1);
                }

                shell_input_end();

                shell_trap_exit();
                log_flush();

                return shell_status;
        }

        positive held = 0;

        while (1)
        {
                bipolar got;
                positive total, at;

                if (interactive)
                        log_direct(str(TERM_MAIN_BUFFER TERM_RESET TERM_SHOW_CURSOR PROMPT));

                //      Room for another read on top of whatever is being
                //      held back, so a line has no length it cannot reach.
                if (!shell_room((address_any address_to)address_of shell_buffer,
                                address_of shell_buffer_room,
                                held + MAX_INPUT_STEP + 1, 1))
                {
                        log_error("sh: no room to read\n", 0);
                        return 1;
                }

                got = system_call_3(syscall(read), (positive)input,
                                    (positive)(shell_buffer + held),
                                    shell_buffer_room - 1 - held);

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

                        run_line(shell_buffer + at);

                        at = stop + 1;
                }

                held = total - at;

                //      What is held back is a line that has not ended yet, so
                //      it moves to the front and the next read lands after
                //      it. The buffer grows above rather than the line being
                //      cut, so there is no length at which this stops working.
                if (held)
                        memory_copy(shell_buffer, shell_buffer + at, held);
        }

        // Whatever was still in hand when the input ended.
        if (held)
        {
                shell_buffer[held] = end;
                run_line(shell_buffer);
        }

        shell_input_end();

        if (script_file)
                system_call_1(syscall(close), (positive)input);

        // Input ran out, which is a way of leaving like any other.
        shell_trap_exit();

        log_flush();
        return shell_status;
}
