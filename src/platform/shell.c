#include "../library.c"

const positive page_size = 4096;

bool shell_styles = true;

#define ENV_MAX_ENTRIES 64
#define ENV_STORAGE_SIZE 8192

static p8 env_storage[ENV_STORAGE_SIZE];
static positive env_used = 0;

string_address shell_envp[ENV_MAX_ENTRIES + 1];

fn shell_env_init()
{
        memory_fill(env_storage, 0, ENV_STORAGE_SIZE);
        env_used = 0;

        string_address defaults[] = {"PATH=/bin:/usr/bin", "SHELL=/bin/sh", null};

        positive idx = 0;
        positive i = 0;

        while (defaults[i] && idx < ENV_MAX_ENTRIES)
        {
                string_address dest = env_storage + env_used;
                string_copy(dest, defaults[i]);
                shell_envp[idx++] = dest;
                env_used += string_length(dest) + 1;
                i++;
        }

        shell_envp[idx] = null;
}

string_address env_get(const_string name)
{
        if (name == null)
                return null;

        positive name_len = string_length(name);
        positive idx = 0;

        while (shell_envp[idx])
        {
                string_address entry = shell_envp[idx];
                string_address eq = string_first_of(entry, '=');

                if (eq)
                {
                        positive key_len = eq - entry;

                        if (key_len == name_len)
                        {
                                positive i = 0;
                                for (i = 0; i < name_len; i++)
                                {
                                        if (string_get(entry + i) != string_get(name + i))
                                                break;
                                }

                                if (i == name_len)
                                        return eq + 1;
                        }
                }

                idx++;
        }

        return null;
}

bool env_set(const_string name, const_string value)
{
        if (!name || !value)
                return false;

        positive name_len = string_length(name);
        positive value_len = string_length(value);
        positive needed = name_len + 1 + value_len + 1;

        positive idx = 0;
        while (shell_envp[idx])
        {
                string_address entry = shell_envp[idx];
                string_address eq = string_first_of(entry, '=');

                if (eq && (eq - entry) == name_len)
                {
                        positive i = 0;
                        for (i = 0; i < name_len; i++)
                        {
                                if (string_get(entry + i) != string_get(name + i))
                                        break;
                        }

                        if (i == name_len)
                        {
                                string_copy(eq + 1, value);
                                return true;
                        }
                }
                idx++;
        }

        if (idx >= ENV_MAX_ENTRIES || env_used + needed > ENV_STORAGE_SIZE)
                return false;

        string_address dest = env_storage + env_used;
        string_copy(dest, name);
        string_copy(dest + name_len, "=");
        string_copy(dest + name_len + 1, value);

        shell_envp[idx] = dest;
        shell_envp[idx + 1] = null;
        env_used += needed;

        return true;
}

fn shell_export(writer write, string_address input)
{
        if (input == null)
        {
                positive idx = 0;
                while (shell_envp[idx])
                {
                        string_format(write, "export %s\n", shell_envp[idx]);
                        idx++;
                }
                return;
        }

        string_address eq = string_first_of(input, '=');
        if (!eq)
                return write(str("export: invalid format (use NAME=value)\n"));

        if (eq == input)
                return write(str("export: missing variable name\n"));

        *eq = end;
        env_set(input, eq + 1);
        *eq = '=';
}

fn shell_env(writer write, string_address input)
{
        positive idx = 0;
        while (shell_envp[idx])
        {
                string_format(write, "%s\n", shell_envp[idx]);
                idx++;
        }
}

fn shell_basename(writer write, string_address input)
{
        if (input == null)
                return write(str("basename: missing operand\n"));

        path_basename(write, input);

        write(str("\n"));
}

fn shell_cat(writer write, string_address input)
{
        if (input == null)
                return write(str("cat: missing operand\n"));

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ);

        if (file_descriptor < 0)
                return string_format(write, "cat: Cannot open file: %s\n", input);

        p8 buffer[page_size];

        while (1)
        {
                bipolar bytes_read = system_call_3(syscall(read), file_descriptor, (positive)buffer, page_size);

                if (bytes_read <= 0)
                        break;

                write(buffer, bytes_read);
        }

        system_call_1(syscall(close), file_descriptor);
}

// TODOs:
// - empty buffer should go to home directory & handle ~
// - "cd -" aka cd $OLDPWD
fn shell_cd(writer write, string_address input)
{
        if (input == null)
                input = "/";

        if (!system_call_1(syscall(chdir), (positive)input))
                return;

        string_format(write, "cd: No such directory: %s\n", input);
}

fn shell_clear(writer write, string_address input)
{
        write(str(TERM_CLEAR_SCREEN));
}

fn shell_chmod(writer write, string_address input)
{
        if (input == null)
                return write(str("chmod: missing operand\n"));

        if (!system_call_3(syscall(fchmodat), AT_FDCWD, (positive)input, 0777))
                return;

        string_format(write, "chmod: Cannot change permissions: %s\n", input);
}

fn shell_cp(writer write, string_address input)
{
        if (input == null)
                return write(str("cp: missing operand\n"));

        string_address destination = string_cut(input, ' ');

        bipolar source_file = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ);

        if (source_file < 0)
                return string_format(write, "cp: Cannot open source file: %s\n", input);

        bipolar dest_file = system_call_4(syscall(openat), AT_FDCWD, (positive)destination,
                                          FILE_CREATE | FILE_WRITE | O_TRUNC, 0666);

        if (dest_file < 0)
        {
                system_call_1(syscall(close), source_file);
                return string_format(write, "cp: Cannot create destination file: %s\n", destination);
        }

        p8 buffer[page_size];

        while (1)
        {
                bipolar bytes_read = system_call_3(syscall(read), source_file, (positive)buffer, page_size);

                if (bytes_read <= 0)
                        break;

                system_call_3(syscall(write), dest_file, (positive)buffer, bytes_read);
        }

        system_call_1(syscall(close), source_file);
        system_call_1(syscall(close), dest_file);
}

fn shell_echo(writer write, string_address input)
{
        if (input != null)
                write(input, 0);

        write(str("\n"));
}

fn shell_exec(writer write, string_address input)
{
        p8 address_to argv[] = {input};

        system_call_2(syscall(execve), (positive)input, (positive)argv);
}

// - Blue: Directories
// - Cyan: Symbolic links
// - Default: Regular files
// - Yellow: Special files (FIFO, sockets, devices, etc.)
fn shell_ls(writer write, string_address input)
{
        const p32 max_line_entries = 8;

        if (input == null)
                input = ".";

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ | O_DIRECTORY);

        if (file_descriptor < 0)
                return string_format(write, "ls: Cannot access '%s': No such file or directory\n", input);

        p8 out_buffer[page_size];

        positive entries_count = 0;

        while (1)
        {
                bipolar bytes_read = system_call_3(syscall(getdents64), file_descriptor, (positive)out_buffer, page_size);

                if (bytes_read <= 0)
                        break;

                p8 address_to step = out_buffer;

                while (step < out_buffer + bytes_read)
                {
                        struct linux_dirent64 address_to entry = (struct linux_dirent64 address_to)step;

                        if (entry->d_name[0] == '.' && (entry->d_name[1] == end ||
                                                        (entry->d_name[1] == '.' && entry->d_name[2] == end)))
                        {
                                step += entry->d_reclen;
                                continue;
                        }

                        if (shell_styles)
                        {
                                if (entry->d_type == DT_DIR)
                                        write(str(TERM_BOLD TERM_BLUE));
                                else if (entry->d_type == DT_LNK)
                                        write(str(TERM_CYAN));
                                else if (entry->d_type == DT_REG)
                                        write(str(TERM_RESET));
                                else
                                        write(str(TERM_YELLOW));
                        }

                        string_format(write, "%s ", entry->d_name);

                        if (shell_styles)
                                write(str(TERM_RESET));

                        entries_count++;

                        if (entries_count % max_line_entries == 0)
                                write(str("\n"));

                        step += entry->d_reclen;
                }
        }

        if (entries_count % max_line_entries != 0)
                write(str("\n"));

        system_call_1(syscall(close), file_descriptor);
}

fn shell_mkdir(writer write, string_address input)
{
        if (input == null)
                return write(str("mkdir: missing operand\n"));

        if (!system_call_3(syscall(mkdirat), AT_FDCWD, (positive)input, 0777))
                return;

        string_format(write, "mkdir: Cannot create directory: %s\n", input);
}

fn shell_mv(writer write, string_address input)
{
        if (input == null)
                return write(str("mv: missing operand\n"));

        string_address destination = string_cut(input, ' ');

        if (destination == null)
                return write(str("mv: missing destination\n"));

        // renameat2 with no flags is renameat. riscv64 never got renameat --
        // the generic syscall ABI dropped it before riscv was added -- and
        // renameat2 is the one call all three architectures have.
        if (!system_call_5(syscall(renameat2), AT_FDCWD, (positive)input, AT_FDCWD,
                           (positive)destination, 0))
                return;

        string_format(write, "mv: Cannot move file: %s\n", input);
}

fn shell_mount(writer write, string_address input)
{
        if (input == null)
                return write(str("mount: missing operand\n"));

        string_address destination = string_cut(input, ' ');

        if (destination == null)
                return write(str("mount: missing destination\n"));

        if (!system_call_4(syscall(mount), (positive)input, (positive)destination, (positive)input, MS_BIND))
                return;

        string_format(write, "mount: Cannot mount filesystem: %s\n", input);
}

fn shell_pwd(writer write, string_address input)
{
        p8 out_buffer[4096];

        system_call_2(syscall(getcwd), (positive)out_buffer, 4096);

        string_format(write, "%s\n", out_buffer);
}

fn shell_exit(writer write, string_address input)
{
        bipolar exit_code = 0;

        if (input != null)
                exit_code = string_to_bipolar(input);

        exit(exit_code);
}

fn shell_touch(writer write, string_address input)
{
        if (input == null)
                return write(str("touch: missing operand\n"));

        bipolar file_descriptor = system_call_4(syscall(openat), AT_FDCWD, (positive)input, FILE_CREATE | FILE_WRITE | O_TRUNC, 0666);

        if (file_descriptor < 0)
                return string_format(write, "touch: Cannot create file: %s\n", input);

        system_call_1(syscall(close), file_descriptor);
}

fn shell_help(writer write, string_address input);

typedef fn(address_to shell_command_function)(writer write, string_address input);

typedef struct
{
        string_address name;
        shell_command_function function;
} shell_command;

shell_command shell_commands[] = {
    {"basename", shell_basename},
    {"cat", shell_cat},
    {"cd", shell_cd},
    {"clear", shell_clear},
    {"cp", shell_cp},
    {"chmod", shell_chmod},
    {"echo", shell_echo},
    {"exec", shell_exec},
    {"exit", shell_exit},
    {"ls", shell_ls},
    {"mkdir", shell_mkdir},
    {"mv", shell_mv},
    {"mount", shell_mount},
    {"pwd", shell_pwd},
    {"touch", shell_touch},
    {"help", shell_help},
    {"env", shell_env},
    {"export", shell_export},
    {null, null},
};

fn shell_help(writer write, string_address input)
{
        string_format(write, "Moonwater shell, WIP, " TERM_RED TERM_BOLD "expect crashes! \n\n" TERM_RESET "Available built-in commands:\n");

        shell_command address_to command = shell_commands;

        while (command->name)
        {
                string_format(write, TERM_BOLD " -  %s" TERM_RESET "\n", command->name);
                command++;
        }

        write(str("\n"));
}