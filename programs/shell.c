// A primitive pre-historic shell,
// it lobs rocks at the kernel and says ouga boga at the user.
#include "../src/library.c"
#include "../src/platform/shell.c"
#include "../src/spark.c"

#define PROMPT TERM_RESET TERM_BOLD " $ " TERM_RESET

#define MAX_INPUT 4096
p8 shell_buffer[MAX_INPUT];
static b32 shell_is_interactive;

writer shell_output = log;
positive shell_output_file;

fn redirect_writer(address_any data, positive length)
{
        if (!shell_output_file)
                return string_format(shell_output, "Redirection error file not open\n");

        if (length == 0)
                length = string_length(data);

        system_call_3(syscall(write), shell_output_file, (positive)data, length);
}

/*
        Control-C cancels the command, not the shell.

        The line discipline sends SIGINT to everything in the terminal's
        foreground group, which is this and whatever it is running. Ignoring it
        here leaves the shell standing; the kernel gives every program it
        spawns its own default disposition back, so the command still dies.

        SIG_IGN needs no restorer, which is the whole reason this is three
        words and not a per-architecture trampoline.
*/
#define SIGNAL_INTERRUPT 2
#define SIGNAL_QUIT 3
#define SIGNAL_IGNORE 1
#define SIGNAL_DEFAULT 0

fn shell_signal(b32 number, positive disposition)
{
        positive action[4] = {disposition, 0, 0, 0};

        system_call_4(syscall(rt_sigaction), number, (positive)address_of action, 0, 8);
}

#define shell_ignore(n) shell_signal(n, SIGNAL_IGNORE)
#define shell_default(n) shell_signal(n, SIGNAL_DEFAULT)

/*
        A line becomes argv here and nowhere else, so the builtin path and the
        spawn path see the same words. Expansion can make a line longer than it
        was read at, so the words get storage of their own instead of being cut
        out of shell_buffer in place.
*/
#define MAX_TOKENS 64
#define TOKEN_STORAGE 8192

p8 token_storage[TOKEN_STORAGE];
positive token_used;
bool token_overflow;

string_address shell_argv[MAX_TOKENS + 1];
positive shell_argc;

// Which words arrived as a bare > or >>. A quoted ">" is a file name and must
// not be mistaken for the operator, and by then the two look identical.
bool shell_operator[MAX_TOKENS + 1];

p8 argument_line[TOKEN_STORAGE];

fn token_push(p8 value)
{
        if (token_used + 1 >= TOKEN_STORAGE)
        {
                token_overflow = true;
                return;
        }

        token_storage[token_used++] = value;
}

fn token_push_string(string_address text)
{
        while (text && string_get(text))
                token_push(string_get(text++));
}

bool shell_name_character(p8 value)
{
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') ||
               value == '_';
}

bool shell_separator(p8 value)
{
        return value == ' ' || value == '\t';
}

// Steps over a $NAME or ${NAME} and appends what it stands for. A name that
// was never exported expands to nothing, the way every other shell does it.
string_address shell_expand(string_address step)
{
        p8 name[128];
        positive length = 0;

        step++;

        bool braced = string_is(step, '{');

        if (braced)
                step++;

        while (shell_name_character(string_get(step)) && length < sizeof(name) - 1)
                name[length++] = string_get(step++);

        name[length] = end;

        if (braced && string_is(step, '}'))
                step++;

        if (!length)
        {
                token_push('$');
                return step;
        }

        token_push_string(env_get(name));

        return step;
}

string_address shell_single_quoted(string_address step)
{
        step++;

        while (string_get(step) && string_not(step, '\''))
                token_push(string_get(step++));

        if (string_get(step))
                step++;

        return step;
}

string_address shell_double_quoted(string_address step)
{
        step++;

        while (string_get(step) && string_not(step, '"'))
        {
                p8 next = string_get(step + 1);

                if (string_is(step, '\\') && (next == '"' || next == '\\' || next == '$'))
                {
                        step++;
                        token_push(string_get(step++));
                        continue;
                }

                if (string_is(step, '$'))
                {
                        step = shell_expand(step);
                        continue;
                }

                token_push(string_get(step++));
        }

        if (string_get(step))
                step++;

        return step;
}

// Returns the number of words, or negative when the line did not fit.
b32 shell_tokenize(string_address line)
{
        string_address step = line;
        b32 count = 0;

        token_used = 0;
        token_overflow = false;

        while (string_get(step))
        {
                while (shell_separator(string_get(step)))
                        step++;

                if (string_is(step, end))
                        break;

                if (count >= MAX_TOKENS)
                        return -1;

                string_address token = token_storage + token_used;
                bool is_operator = false;

                if (string_is(step, '>'))
                {
                        is_operator = true;
                        token_push(string_get(step++));

                        if (string_is(step, '>'))
                                token_push(string_get(step++));
                }
                else
                {
                        while (string_get(step) && !shell_separator(string_get(step)) &&
                               string_not(step, '>'))
                        {
                                if (string_is(step, '\\'))
                                {
                                        step++;

                                        if (string_get(step))
                                                token_push(string_get(step++));

                                        continue;
                                }

                                if (string_is(step, '\''))
                                {
                                        step = shell_single_quoted(step);
                                        continue;
                                }

                                if (string_is(step, '"'))
                                {
                                        step = shell_double_quoted(step);
                                        continue;
                                }

                                if (string_is(step, '$'))
                                {
                                        step = shell_expand(step);
                                        continue;
                                }

                                token_push(string_get(step++));
                        }
                }

                token_push(end);

                if (token_overflow)
                        return -1;

                shell_operator[count] = is_operator;
                shell_argv[count] = token;
                count++;
        }

        shell_argv[count] = null;
        shell_operator[count] = false;

        return count;
}

// The builtins still take the rest of the line as a single string, so the
// words are handed back joined. Quoting survives as far as argv, no further.
string_address shell_arguments()
{
        positive used = 0;
        positive index = 1;

        if (shell_argc < 2)
                return null;

        while (index < shell_argc)
        {
                positive length = string_length(shell_argv[index]);

                if (used + length + 2 > TOKEN_STORAGE)
                        break;

                if (used)
                        argument_line[used++] = ' ';

                memory_copy(argument_line + used, shell_argv[index], length);
                used += length;
                index++;
        }

        argument_line[used] = end;

        return argument_line;
}

// Takes "> file" and ">> file" back out of argv and opens the target.
bool shell_redirect()
{
        positive index = 0;

        while (index < shell_argc)
        {
                if (!shell_operator[index])
                {
                        index++;
                        continue;
                }

                bool append = string_is(shell_argv[index] + 1, '>');

                if (index + 1 >= shell_argc || shell_operator[index + 1])
                {
                        string_format(shell_output, "Missing file name for redirection\n");
                        return false;
                }

                string_address name = shell_argv[index + 1];

                bipolar file_descriptor = system_call_4(syscall(openat), AT_FDCWD, (positive)name,
                                                        append ? (FILE_READ | FILE_APPEND | FILE_CREATE)
                                                               : (FILE_WRITE | FILE_CREATE),
                                                        0666);

                if (file_descriptor < 0)
                {
                        string_format(shell_output, "Cannot open file for redirection: %s\n", name);
                        return false;
                }

                // Last one on the line wins, so an earlier target is closed
                // rather than leaked.
                if (shell_output_file)
                        system_call_1(syscall(close), shell_output_file);

                shell_output = redirect_writer;
                shell_output_file = file_descriptor;

                positive move = index;

                while (move + 2 <= shell_argc)
                {
                        shell_argv[move] = shell_argv[move + 2];
                        shell_operator[move] = shell_operator[move + 2];
                        move++;
                }

                shell_argc -= 2;
                shell_argv[shell_argc] = null;
        }

        return true;
}

fn shell_thread_instance()
{
        // Ignored signals cross execve, and this shell ignores interrupt so
        // that control-C does not take it down with the command. Handing that
        // deafness on would leave the command uninterruptible.
        shell_default(SIGNAL_INTERRUPT);
        shell_default(SIGNAL_QUIT);

        // The child owns its own descriptors, so a redirection lands here and
        // never touches the shell's own output.
        if (shell_output_file)
                system_call_3(syscall(dup3), shell_output_file, stdout, 0);

        bipolar exec_result = system_call_3(syscall(execve), (positive)shell_argv[0],
                                            (positive)shell_argv, (positive)shell_envp);

        string_format(shell_output, "failed with error: %b\n", exec_result);
        log_flush();

        exit(1);
}

// Opened once at startup. Spawning through it skips the fork whose address
// space copy execve would only throw away: about 3us per command here.
// Negative means the kernel has no spark device and we fall back to forking.
b32 spawn_device = -1;

// argv and envp go across as flat blocks of NUL terminated strings.
p8 spawn_argv_block[MAX_INPUT];
p8 spawn_envp_block[MAX_INPUT];

positive shell_flatten_env(p8 address_to block, positive limit, positive address_to count_out)
{
        positive used = 0;
        positive count = 0;
        positive index = 0;

        while (shell_envp[index])
        {
                positive length = string_length(shell_envp[index]) + 1;

                if (used + length > limit)
                        break;

                memory_copy(block + used, shell_envp[index], length);
                used += length;
                count++;
                index++;
        }

        address_to count_out = count;
        return used;
}

// Returns the child pid, or a negative error if the device could not take it.
bipolar shell_spawn_via_device()
{
        struct spawn request;
        positive used = 0;
        positive index = 0;
        positive envc = 0;

        while (index < shell_argc)
        {
                positive length = string_length(shell_argv[index]) + 1;

                // A word that does not fit is not a word that can be dropped:
                // refuse, and the caller forks instead, where there is no
                // single block to run out of.
                if (used + length > sizeof(spawn_argv_block))
                        return -1;

                memory_copy(spawn_argv_block + used, shell_argv[index], length);
                used += length;
                index++;
        }

        request.path = (unsigned long)shell_argv[0];
        request.argv = (unsigned long)spawn_argv_block;
        request.argv_bytes = used;
        request.argv_count = shell_argc;
        request.envp = (unsigned long)spawn_envp_block;
        request.envp_bytes = shell_flatten_env(spawn_envp_block, sizeof(spawn_envp_block), address_of envc);
        request.envp_count = envc;

        return system_call_3(syscall(ioctl), spawn_device, SPARK_IOCTL_SPAWN,
                             (positive)address_of request);
}

fn shell_execute_command()
{
        bipolar child = -1;
        bipolar saved_output = -1;

        log_flush();

        /*
                The device spawn copies the caller's descriptor table, so a
                redirection has to be in place on this side before the ioctl
                and gone again the moment it returns. The copy happens inside
                the call, which is why putting stdout back straight after is
                enough.
        */
        if (shell_output_file && spawn_device >= 0)
        {
                saved_output = system_call_1(syscall(dup), stdout);

                if (saved_output >= 0)
                        system_call_3(syscall(dup3), shell_output_file, stdout, 0);
        }

        if (spawn_device >= 0 && (!shell_output_file || saved_output >= 0))
                child = shell_spawn_via_device();

        if (saved_output >= 0)
        {
                system_call_3(syscall(dup3), saved_output, stdout, 0);
                system_call_1(syscall(close), saved_output);
        }

        if (child < 0)
        {
                // No spark device, or it refused the request: fall back to the
                // portable path so the shell still works on a stock kernel.
                //
                // clone takes (flags, child_stack, ...). Passing only flags
                // left child_stack as whatever happened to be in the second
                // argument register, so the child started on a garbage stack
                // and execve was handed an empty path.
                child = system_call_2(syscall(clone), SIGCHLD, 0);

                if (child == 0)
                        shell_thread_instance();
        }

        if (child > 0)
        {
                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);

                // Spawning hands back a pid before the image is loaded, so a
                // path that cannot be run shows up as the child exiting 127
                // rather than as an error from the spawn itself.
                if ((status >> 8 & 0xff) == 127)
                        string_format(shell_output, "Could not run: '%s'\n", shell_argv[0]);
        }
        else
                string_format(shell_output, "failed with error: %b\n", child);

        log_flush();
}

bool shell_builtin(string_address arguments)
{
        shell_command address_to command = shell_commands;

        while (command->name)
        {
                if (string_compare(command->name, shell_argv[0]))
                {
                        command++;
                        continue;
                }

                command->function(shell_output, arguments);
                return true;
        }

        return false;
}

fn process(string_address line)
{
        b32 count = shell_tokenize(line);

        if (count < 0)
                return string_format(shell_output, "Command line too long\n");

        shell_argc = count;

        if (!shell_argc)
                return;

        if (!shell_redirect())
                return;

        if (!shell_argc)
                return;

        if (string_is(shell_argv[0], '.') || string_is(shell_argv[0], '/'))
                return shell_execute_command();

        if (shell_builtin(shell_arguments()))
                return;

        /*
                A bare name, looked for on the path. A builtin wins over one --
                that is the order every shell uses, and it is checked above --
                but anything else typed without a slash used to be refused
                however plainly it was sitting in a directory on the path.
        */
        {
                static p8 found[768];

                if (shell_find_in_path(shell_argv[0], found, sizeof(found)))
                {
                        shell_argv[0] = found;
                        return shell_execute_command();
                }
        }

        string_format(shell_output, "Command not found: '%s'\n", shell_argv[0]);
}

fn run_line(string_address line)
{
        process(line);

        /*
                A terminal wants each line the moment it happens. A script does
                not, and flushing per line is one write system call per line of
                it -- which is where the time in a forty thousand line script
                went. The buffer drains when it fills, before anything is
                spawned, and when the input ends.
        */
        if (shell_is_interactive || shell_output_file)
                log_flush();

        if (shell_output_file)
                system_call_1(syscall(close), shell_output_file);

        shell_output = log;
        shell_output_file = 0;
}


// A prompt is for somebody watching. Asking the terminal about itself is the
// only way to know whether anybody is: a script piped in gets none, which is
// also what keeps its output free of them.
#define TCGETS 0x5401u

static b32 shell_interactive()
{
        p8 settings[64];

        return system_call_3(syscall(ioctl), 0, TCGETS, (positive)settings) == 0;
}

b32 main()
{
        b32 interactive;

        shell_ignore(SIGNAL_INTERRUPT);
        shell_ignore(SIGNAL_QUIT);

        interactive = shell_is_interactive = shell_interactive();

        shell_env_init();

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

        log_flush();
        return 0;
}
