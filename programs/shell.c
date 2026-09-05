#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"

/*
        A complete -c program which cannot create shell state or observe it.

        Comments are only recognised after blanks/newlines because reaching
        this routine already means no word has begun.  `:#text` is therefore
        a command name and is deliberately rejected, while `: # text` is the
        no-op followed by a comment.  Only one literal colon, true or false
        command is accepted; separators, operands and further commands belong
        to the real parser even when their eventual combined status would be
        the same.
*/
static bool shell_command_literal_status(string_address command,
                                         b32 address_to status)
{
        bool command_seen = false;
        b32 answer = 0;
        string_address step = command;

        if (!step)
                return false;

        while (true)
        {
                while (*step == ' ' || *step == '\t' || *step == '\n')
                        step++;

                if (*step == '#')
                {
                        while (*step && *step != '\n')
                                step++;
                        continue;
                }

                if (!*step)
                {
                        address_to status = answer;
                        return true;
                }

                if (!command_seen && *step == ':' &&
                    (!step[1] || step[1] == ' ' || step[1] == '\t' ||
                     step[1] == '\n'))
                {
                        command_seen = true;
                        step++;
                        continue;
                }

                /* Exact literal true/false have no expansion, assignment,
                   redirection or inherited state to observe.  They can take
                   the same entry floor as colon; operands deliberately fall
                   through because expanding one may have side effects. */
                if (!command_seen && step[0] == 't' && step[1] == 'r' &&
                    step[2] == 'u' && step[3] == 'e' &&
                    (!step[4] || step[4] == ' ' || step[4] == '\t' ||
                     step[4] == '\n'))
                {
                        command_seen = true;
                        step += 4;
                        continue;
                }

                if (!command_seen && step[0] == 'f' && step[1] == 'a' &&
                    step[2] == 'l' && step[3] == 's' && step[4] == 'e' &&
                    (!step[5] || step[5] == ' ' || step[5] == '\t' ||
                     step[5] == '\n'))
                {
                        command_seen = true;
                        answer = 1;
                        step += 5;
                        continue;
                }

                return false;
        }
}

static bool shell_start_parameters(string_address address_to arguments,
                                   positive first, positive count)
{
        if (!shell_room((address_any address_to)address_of shell_argv,
                        address_of shell_argv_room, count + 1,
                        sizeof(shell_argv[0])))
                return false;

        if (count)
                memory_copy_apart(shell_argv, arguments + first,
                                  count * sizeof(shell_argv[0]));
        shell_argv[count] = null;

        return shell_parameters_set(shell_argv, count);
}

static positive shell_run_complete_lines(p8 address_to text, positive length)
{
        positive at = 0;

        while (at < length)
        {
                positive left = length - at;
                p8 address_to newline = null;

                /* A call cannot beat one or two byte compares. Longer scans
                   belong to the architecture floor. */
                if (text[at] == '\n')
                        newline = text + at;
                else if (left > 1 && text[at + 1] == '\n')
                        newline = text + at + 1;
                else if (left > 2)
                        newline = (p8 address_to)memory_first_of(
                            text + at + 2, '\n', left - 2);

                if (!newline)
                        break;

                address_to newline = end;

                // What a person typed, and only that: an eval or a sourced
                // file is a line this shell wrote for itself.
                if (shell_is_interactive)
                        history_remember(text + at);

                run_line(text + at);
                at = (positive)(newline - text) + 1;
        }

        return at;
}

typedef struct
{
        positive next;
        bool command;
        bool from_stdin;
        b32 interactive;
        b32 monitor;
} shell_invocation;

/* The command-line grammar locates the source and operands, while set's
   existing option adapter owns option state. No command string is synthesized
   and argv bytes never pass through expansion to become startup options. */
static bool shell_start_options(string_address address_to arguments,
                                positive count, shell_invocation *invocation)
{
        positive at = 1;

        invocation->interactive = -1;
        invocation->monitor = -1;
        while (at < count)
        {
                string_address word = arguments[at];
                bool on;

                if (word_is(word, "--") || word_is(word, "-"))
                {
                        at++;
                        break;
                }
                if ((word[0] != '-' && word[0] != '+') || !word[1])
                        break;

                if (shell_bash_compat &&
                    (word_is(word, "--noprofile") || word_is(word, "--norc")))
                {
                        /* These opt out of startup files, which this entry
                           does not currently load. */
                        at++;
                        continue;
                }

                on = word[0] == '-';
                for (string_address letter = word + 1; *letter; letter++)
                {
                        p8 value = *letter;

                        if (value == 'c')
                                invocation->command = true;
                        else if (value == 'm')
                                invocation->monitor = on;
                        else if (value == 'l')
                        {
                                if (on)
                                        shell_shopt_state |=
                                            SHELL_SHOPT(LOGIN_SHELL);
                                else
                                        shell_shopt_state &=
                                            ~SHELL_SHOPT(LOGIN_SHELL);
                        }
                        else if (value == 'o')
                        {
                                if (at + 1 == count)
                                        shell_options_listed(log, !on);
                                else if (word_is(arguments[at + 1], "monitor"))
                                {
                                        invocation->monitor = on;
                                        at++;
                                }
                                else if (!shell_option_named(arguments[++at],
                                                             on))
                                {
                                        string_format(log_error,
                                                      "sh: invalid option: %s\n",
                                                      arguments[at]);
                                        return false;
                                }
                        }
                        else if (value == 'O' && shell_bash_compat)
                        {
                                if (at + 1 == count)
                                        for (positive item = 0;
                                             item < SHELL_SHOPT_NAMES; item++)
                                                shell_shopt_said(log, item,
                                                                 !on);
                                else
                                {
                                        positive item =
                                            shell_shopt_find(arguments[++at]);

                                        if (item >= SHELL_SHOPT_NAMES)
                                        {
                                                string_format(
                                                    log_error,
                                                    "sh: invalid shell option: %s\n",
                                                    arguments[at]);
                                                return false;
                                        }
                                        if (on)
                                                shell_shopt_state |=
                                                    (positive)1 << item;
                                        else
                                                shell_shopt_state &=
                                                    ~((positive)1 << item);
                                }
                        }
                        else if (!shell_option_letter_told(value, on))
                        {
                                p8 said[2] = {value, end};
                                string_format(log_error,
                                              "sh: invalid option: %s%s\n",
                                              on ? "-" : "+", said);
                                return false;
                        }

                        if (value == 's')
                                invocation->from_stdin = shell_bash_compat || on;
                        else if (value == 'i')
                                invocation->interactive = on;
                }
                at++;
        }

        invocation->next = at;
        return true;
}

b32 main()
{
        b32 interactive;
        bipolar input = 0;
        bool script_file = false;
        bool command_option = false;
        string_address command = null;
        string_address address_to arguments;
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

        process_arguments = (positive)program_argument_count();
        arguments = program_argument_list();

        /*
                A login shell is one whose zeroth argument begins with a dash.

                That is the whole of the mark, it has been since the seventh
                edition, and `shopt -q login_shell` is the only thing here
                that asks: nothing behaves differently, because the profile
                files a login shell would read are not this shell's yet.
        */
        if (process_arguments && arguments[0] && arguments[0][0] == '-')
                shell_shopt_state |= SHELL_SHOPT(LOGIN_SHELL);

        /* Find the -c command without publishing any shell state yet. */
        if (process_arguments > 1)
        {
                string_address option = arguments[1];

                if (option && option[0] == '-' && option[1] == 'c' &&
                    !option[2])
                {
                        command_option = true;
                        if (process_arguments >= 3)
                                command = arguments[2];
                }
        }

        /* Signal policy is observable even while an otherwise empty shell is
           alive, so it remains on the semantic floor rather than being
           treated as parser setup. */
        shell_signals_start();

        {
                b32 literal_status = 0;

                if (shell_command_literal_status(command,
                                                 address_of literal_status))
                        return literal_status;
        }

        /* A literal status command above cannot observe its personality.
           Keep name scanning and the general option parser off that entry
           path, just as environment and parameter allocation stay off it. */
        shell_invocation invocation = {0};
        if (process_arguments && arguments[0])
        {
                string_address called = string_last_of(arguments[0], '/');
                called = called ? called + 1 : arguments[0];
                if (*called == '-')
                        called++;
                shell_bash_compat = word_is(called, "bash");
                shell_script_name = arguments[0];
        }

        /* environ is the live process environment. Ordinarily it is the
           kernel vector published by the startup shim; clone-and-reentry can
           deliberately replace it without forging another initial stack. */
        shell_env_init(environ);

        if (!shell_start_options(arguments, process_arguments,
                                 address_of invocation))
        {
                log_flush();
                return 2;
        }
        command_option = invocation.command;

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
        if (command_option)
        {
                positive first = invocation.next;
                positive count = process_arguments > first + 1
                                     ? process_arguments - first - 2 : 0;

                if (first >= process_arguments)
                {
                        log_error("sh: -c wants a command\n", 0);
                        return shell_bash_compat &&
                                       (shell_options & SHELL_FLAG('e'))
                                   ? 1 : 2;
                }

                command = arguments[first];
                if (!shell_start_parameters(arguments, first + 2, count))
                {
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                //      With no name operand $0 is unspecified, and what
                //      dash does is use the path it was invoked as. That is
                //      more use than the word "sh" when a message has to say
                //      where it came from.
                shell_script_name = process_arguments > first + 1
                                                          ? arguments[first + 1]
                                                          : arguments[0];
                shell_option_flags = (string_address) "c";
                script_file = true;
        }
        else if (!invocation.from_stdin && invocation.next < process_arguments)
        {
                positive first = invocation.next;
                string_address script = arguments[first];
                positive count = process_arguments - first - 1;
                bipolar handle = system_open_at(AT_FDCWD, script, FILE_READ);

                if (handle < 0)
                {
                        string_format(log_error, "sh: %s: cannot open\n", script);
                        return 2;
                }

                if (!shell_start_parameters(arguments, first + 1, count))
                {
                        system_close(handle);
                        log_error("sh: no room for arguments\n", 0);
                        return 1;
                }

                shell_script_name = script;
                shell_option_flags = (string_address) "";
                input = handle;
                script_file = true;
        }
        else if (invocation.next < process_arguments &&
                 !shell_start_parameters(arguments, invocation.next,
                                          process_arguments - invocation.next))
        {
                log_error("sh: no room for arguments\n", 0);
                return 1;
        }

        interactive = shell_is_interactive = invocation.interactive >= 0
                          ? invocation.interactive
                          : (!script_file && shell_interactive());
        shell_options_started(interactive, invocation.monitor);
        if (interactive && shell_option_on(SHELL_OPTION_MONITOR) &&
            !job_terminal_owned)
                shell_option_told(SHELL_OPTION_MONITOR, false);
        history_start();

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
                string_address first_newline =
                    string_first_of_or_end(command, '\n');

                /* The lexer and parser copy tokens and never write the source.
                   A single command can therefore stay in the process argument
                   block; only multi-line input needs a writable copy whose
                   newlines are ended in place. */
                if (!*first_newline)
                        run_line(command);
                else
                {
                        positive length = (positive)(first_newline - command) +
                                          1 + string_length(first_newline + 1);
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
                                positive at = shell_run_complete_lines(
                                    held_command, length);

                                if (at < length)
                                        run_line(held_command + at);
                        }

                        memory_free(held_command, length + 1);
                }

                /* Join the ordinary input teardown without closing stdin:
                   -c has no source descriptor of its own. */
                script_file = false;
                goto input_finished;
        }

        positive held = 0;

        while (1)
        {
                bipolar got;
                positive total, at;

                if (interactive)
                {
                        log_direct(str(TERM_MAIN_BUFFER TERM_RESET
                                           TERM_SHOW_CURSOR));
                        shell_prompt_write(log_direct, shell_reading_more());
                }

                //      Room for another read on top of whatever is being
                //      held back, so a line has no length it cannot reach.
                if (!shell_room((address_any address_to)address_of shell_buffer,
                                address_of shell_buffer_room,
                                held + MAX_INPUT_STEP + 1, 1))
                {
                        log_error("sh: no room to read\n", 0);
                        return 1;
                }

                got = system_read_once(input, shell_buffer + held,
                                       shell_buffer_room - 1 - held);

                if (got <= 0)
                        break;

                total = held + (positive)got;
                at = shell_run_complete_lines(shell_buffer, total);

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

                if (shell_is_interactive)
                        history_remember(shell_buffer);

                run_line(shell_buffer);
        }

input_finished:
        shell_input_end();

        if (script_file)
                system_close(input);

        // Input ran out, which is a way of leaving like any other.
        shell_trap_exit();

        log_flush();
        return shell_status;
}
