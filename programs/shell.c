// A primitive pre-historic shell,
// it lobs rocks at the kernel and says ouga boga at the user.
#include "../std/library.c"
#include "../std/platform/shell.c"
#include "../std/spark.c"

#define PROMPT TERM_RESET TERM_BOLD " $ " TERM_RESET

#define MAX_INPUT 4096
p8 shell_buffer[MAX_INPUT];
positive input_length;

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

fn shell_thread_instance(string_address command, string_address arguments)
{
        string_address arguments_list[] = {command, arguments, null};

        bipolar exec_result = system_call_3(syscall(execve), (positive)command, (positive)arguments_list, (positive)shell_envp);

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
bipolar shell_spawn_via_device(string_address command, string_address arguments)
{
        struct spawn request;
        positive used = 0;
        positive argc = 0;
        positive envc = 0;

        positive length = string_length(command) + 1;
        memory_copy(spawn_argv_block, command, length);
        used = length;
        argc = 1;

        if (arguments)
        {
                length = string_length(arguments) + 1;

                if (used + length <= sizeof(spawn_argv_block))
                {
                        memory_copy(spawn_argv_block + used, arguments, length);
                        used += length;
                        argc++;
                }
        }

        request.path = (unsigned long)command;
        request.argv = (unsigned long)spawn_argv_block;
        request.argv_bytes = used;
        request.argv_count = argc;
        request.envp = (unsigned long)spawn_envp_block;
        request.envp_bytes = shell_flatten_env(spawn_envp_block, sizeof(spawn_envp_block), address_of envc);
        request.envp_count = envc;

        return system_call_3(syscall(ioctl), spawn_device, SPARK_IOCTL_SPAWN,
                             (positive)address_of request);
}

fn shell_execute_command(string_address command, string_address arguments)
{
        bipolar child = -1;

        log_flush();

        if (spawn_device >= 0)
                child = shell_spawn_via_device(command, arguments);

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
                        shell_thread_instance(command, arguments);
        }

        if (child > 0)
        {
                positive status = 0;
                system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);

                // Spawning hands back a pid before the image is loaded, so a
                // path that cannot be run shows up as the child exiting 127
                // rather than as an error from the spawn itself.
                if ((status >> 8 & 0xff) == 127)
                        string_format(shell_output, "Could not run: '%s'\n", command);
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
                if (string_compare(command->name, shell_buffer))
                {
                        command++;
                        continue;
                }

                command->function(shell_output, arguments);
                return true;
        }

        return false;
}

fn process()
{
        string_set_if(shell_buffer[input_length], '\n', end);

        string_address redirect = string_find(shell_buffer, " >>");

        if (redirect)
        {
                memory_fill(redirect, end, 4);
                redirect += 4;

                if string_is (redirect, end)
                        return string_format(shell_output, "Missing file name for redirection\n");

                bipolar file_descriptor = system_call_4(syscall(openat), AT_FDCWD, (positive)redirect, FILE_READ | FILE_APPEND | FILE_CREATE, 0666);

                if (file_descriptor < 0)
                        return string_format(shell_output, "Cannot open file for redirection: %s\n", redirect);

                shell_output = redirect_writer;
                shell_output_file = file_descriptor;
        }

        string_address step = string_cut(shell_buffer, ' ');

        if (string_is(shell_buffer, '.') || string_is(shell_buffer, '/'))
                return shell_execute_command(shell_buffer, step);

        if (shell_builtin(step))
                return;

        string_format(shell_output, "Command not found: '%s'\n", shell_buffer);
}

b32 main()
{
        system_call(syscall(setsid));
        system_call_2(2, (positive) "/dev/console", FILE_READ_WRITE | O_NOCTTY);

        shell_env_init();

        spawn_device = system_call_4(syscall(openat), AT_FDCWD,
                                     (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        while (1)
        {
                memory_fill(shell_buffer, end, MAX_INPUT);

                log_direct(str(TERM_MAIN_BUFFER TERM_RESET TERM_SHOW_CURSOR PROMPT));

                input_length = system_call_3(syscall(read), 0, (positive)shell_buffer, MAX_INPUT) - 1;

                if (input_length)
                        process();

                log_flush();

                if (shell_output_file)
                        system_call_1(syscall(close), shell_output_file);

                shell_output = log;
                shell_output_file = 0;
        }
}