// A primitive pre-historic shell,
// it lobs rocks at the kernel and says ouga boga at the user.
#include "../../standard/library.c"
#include "../../standard/platform/shell.c"

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

        bipolar exec_result = system_call_3(syscall(execve), (positive)command, (positive)arguments_list, 0);

        string_format(shell_output, "failed with error: %b\n", exec_result);
        log_flush();

        exit(1);
}

fn shell_execute_command(string_address command, string_address arguments)
{
        log_flush();

        bipolar fork_result = system_call_1(syscall(clone), 0);

        if (fork_result == 0)
                shell_thread_instance(command, arguments);

        if (fork_result > 0)
        {
                positive status = 0;
                bipolar wait_result = system_call_4(syscall(wait4), fork_result, (positive)address_of status, 0, 0);
        }
        else
                string_format(shell_output, "failed with error: %b\n", fork_result);

        log_flush();
}

bool shell_builtin(string_address arguments)
{
        dawn_shell_command address_to command = dawn_shell_commands;

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