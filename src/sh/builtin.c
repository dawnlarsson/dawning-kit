#include "../library.c"

const positive page_size = 4096;


/*
        Where a complaint goes.

        Not through the writer a builtin was handed: that one may have been
        pointed at a file or down a pipe, and "ls /nowhere 2>/dev/null" is a
        script saying it wants the complaint gone and the output kept. The two
        have to be separable, so a diagnostic is written straight to the second
        descriptor and never buffered behind the first.
*/
fn shell_diagnostic(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        log_flush();
        system_call_3(syscall(write), stderr, (positive)data, length);
}

// The status the last thing to run answered with, which $? reads.
b32 shell_status;

// What it held when the builtin now running was reached. Only exit wants it:
// leaving with no number given means leaving with the last status.
b32 shell_status_entering;

fn shell_answer(b32 value)
{
        shell_status = value;
}

/*
        The words as the shell tokenised them.

        A builtin used to be handed the rest of the line joined back into one
        string, which loses exactly the quoting that printf and test live on.
        These are the arrays shell.c fills. The type is left incomplete so that
        this file makes no claim about how many words fit in them.
*/
string_address shell_argv[];
positive shell_argc;

// eval runs a line, and what runs lines sits above this file. Weak, because
// programs/edit.c includes this file with no shell around it.
fn run_line(string_address line) __attribute__((weak));
bool exec_function_here(string_address name) __attribute__((weak));
bool shell_builtin(string_address arguments);
string_address shell_arguments();
fn shell_execute_command();
fn parse_nest_enter() __attribute__((weak));
fn parse_nest_leave() __attribute__((weak));

b32 shell_find_in_path(string_address name, p8 address_to into, positive room);
bipolar shell_signed(string_address input, bool address_to good);

/*
        The set flags, remembered but not obeyed.

        Stopping on an error, tracing a command before it runs, refusing a name
        that was never set: all of that happens where commands are run. This is
        only where the letters are kept so that code there can ask.
*/
positive shell_options;

/*
        Names an assignment may no longer touch. Held apart from the values so
        that readonly can be spoken about a name that has none yet.
*/
#define READONLY_MAX 16
#define READONLY_STORAGE 512

static p8 readonly_storage[READONLY_STORAGE];
static positive readonly_used;
static string_address readonly_name[READONLY_MAX];
static positive readonly_count;

bool env_readonly(const_string name)
{
        positive index = 0;

        while (index < readonly_count)
        {
                if (!string_compare(readonly_name[index], (string_address)name))
                        return true;

                index++;
        }

        return false;
}

#define ENV_MAX_ENTRIES 64
#define ENV_STORAGE_SIZE 8192

static p8 env_storage[ENV_STORAGE_SIZE];
static positive env_used = 0;

string_address shell_envp[ENV_MAX_ENTRIES + 1];

fn shell_env_init()
{
        memory_fill(env_storage, 0, ENV_STORAGE_SIZE);
        env_used = 0;

        // Programs live at the root of the image, so it is on the path.
        string_address defaults[] = {"PATH=/bin:/usr/bin:/", "SHELL=/bin/sh", null};

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

        if (env_readonly(name))
                return false;

        positive name_len = string_length(name);
        positive value_len = string_length(value);
        positive needed = name_len + 1 + value_len + 1;

        positive idx = 0;
        bool replacing = false;

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

                        // Only where it fits. The entries sit end to end in one
                        // block, so a longer value written in place runs over
                        // the name of whatever comes next.
                        if (i == name_len && value_len <= string_length(eq + 1))
                        {
                                string_copy(eq + 1, value);
                                return true;
                        }

                        if (i == name_len)
                        {
                                replacing = true;
                                break;
                        }
                }
                idx++;
        }

        if ((!replacing && idx >= ENV_MAX_ENTRIES) ||
            env_used + needed > ENV_STORAGE_SIZE)
                return false;

        string_address dest = env_storage + env_used;
        string_copy(dest, name);
        string_copy(dest + name_len, "=");
        string_copy(dest + name_len + 1, value);

        shell_envp[idx] = dest;

        // Replacing an entry keeps the list the length it was; the terminator
        // is already past it.
        if (!replacing)
                shell_envp[idx + 1] = null;

        env_used += needed;

        return true;
}

#define ERROR_NOT_PERMITTED 1
#define ERROR_NO_ENTRY 2
#define ERROR_EXISTS 17
#define ERROR_NOT_DIRECTORY 20
#define ERROR_IS_DIRECTORY 21

#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200

#define ACCESS_EXECUTE 1

#define STATX_BASIC 0x7ff

#define MODE_FORMAT 0170000
#define MODE_SOCKET 0140000
#define MODE_LINK 0120000
#define MODE_FILE 0100000
#define MODE_BLOCK 0060000
#define MODE_DIRECTORY 0040000
#define MODE_CHARACTER 0020000
#define MODE_PIPE 0010000

// How deep cp -r and rm -r are willing to walk. Every level holds an open
// descriptor and a stack frame, so this is what keeps a loop of symlinked or
// pathologically nested directories from taking the shell down with it.
#define SHELL_MAX_DEPTH 24

/*
        file_facts is statx's 256 bytes, and file.c beside this spells all of
        them out. This file used to carry its own shorter copy of the same
        layout, which was the same struct written twice.

        Where there is no file.c -- programs/edit.c takes this file on its own
        -- the short form is still here, because what is wanted from it is the
        mode and nothing further in.
*/
#ifndef FILE_MAX_DEPTH
typedef struct
{
        p32 mask;
        p32 blocksize;
        p64 attributes;
        p32 hard_links;
        p32 owner;
        p32 group;
        p16 mode;
        p16 reserved;
        p64 inode;
        p64 size;
        p64 blocks;
        p8 remainder[200];
} file_facts;
#endif

bool shell_facts(bipolar directory, string_address path, file_facts address_to out)
{
        return system_call_5(syscall(statx), directory, (positive)path,
                             AT_SYMLINK_NOFOLLOW, STATX_BASIC, (positive)out) == 0;
}

bool shell_is_directory(bipolar directory, string_address path)
{
        file_facts facts;

        if (!shell_facts(directory, path, address_of facts))
                return false;

        return (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;
}

#define SHELL_FLAG(letter) ((positive)1 << ((letter) - 'a'))

/*
        Takes the leading -abc words off the front of a builtin's arguments and
        hands back one bit per letter. The words are only ever cut once a '-'
        has been seen, because a caller may pass a literal: programs/edit.c
        calls shell_ls with a string in read only memory.
*/
/*
        Leading options, and only the ones the command actually has.

        A word is taken as options only when every letter in it is one the
        caller named. Swallowing any word that began with a dash meant "echo
        -foo" printed an empty line, and an option a command does not have
        vanished instead of being complained about.
*/
positive shell_flags(string_address address_to input, string_address allowed)
{
        positive flags = 0;
        string_address step = address_to input;

        while (step && string_is(step, '-') && string_not(step + 1, end))
        {
                string_address letter = step + 1;
                positive taken = 0;

                while (string_get(letter) && string_not(letter, ' '))
                {
                        if (!string_first_of(allowed, string_get(letter)))
                                break;

                        taken |= SHELL_FLAG(string_get(letter));
                        letter++;
                }

                // Anything unrecognised in the word makes the whole word an
                // operand, and the options end there. Nothing is cut until it
                // has been accepted, so the word is still intact to be one.
                if (string_get(letter) && string_not(letter, ' '))
                        break;

                flags |= taken;
                step = string_cut(step, ' ');
        }

        address_to input = step;

        return flags;
}

string_address shell_word(string_address address_to input)
{
        string_address word = address_to input;

        if (word == null)
                return null;

        address_to input = string_cut(word, ' ');

        return word;
}

// string_to_positive scans backwards from the end of the string, so it reads
// "0.5" as 5 and anything with a trailing space as 0. Arguments arrive as
// whole words here and have to be read forwards.
positive shell_number(string_address input)
{
        positive value = 0;

        while (input && string_get(input) >= '0' && string_get(input) <= '9')
                value = value * 10 + (string_get(input++) - '0');

        return value;
}

fn shell_number_padded(writer write, positive value, positive width)
{
        p8 digits[24];
        positive length = 0;

        if (value == 0)
                digits[length++] = '0';

        while (value && length < sizeof(digits))
        {
                digits[length++] = '0' + (value % 10);
                value /= 10;
        }

        while (width > length)
        {
                write(" ", 1);
                width--;
        }

        while (length)
                write(digits + --length, 1);
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
                return shell_diagnostic(str("export: invalid format (use NAME=value)\n"));

        if (eq == input)
                return shell_diagnostic(str("export: missing variable name\n"));

        *eq = end;
        env_set(input, eq + 1);
        *eq = '=';
}




// TODOs:
// - "cd -" aka cd $OLDPWD, and handle ~
fn shell_cd(writer write, string_address input)
{
        if (input == null)
                input = env_get("HOME");

        if (input == null)
                input = "/";

        if (!system_call_1(syscall(chdir), (positive)input))
                return shell_answer(0);

        // Two, not one: a special builtin that fails answers with two, and
        // the reference shell does.
        shell_answer(2);
        string_format(shell_diagnostic, "cd: No such directory: %s\n", input);
}

fn shell_clear(writer write, string_address input)
{
        write(str(TERM_CLEAR_SCREEN));
}


// Only ever live at the leaf of a copy, so one block at file scope keeps it
// off every recursion frame.
static p8 shell_copy_buffer[4096];

bool shell_copy_file(bipolar source_directory, string_address source,
                     bipolar dest_directory, string_address destination, positive mode)
{
        bipolar in = system_call_3(syscall(openat), source_directory, (positive)source, FILE_READ);

        if (in < 0)
                return false;

        bipolar out = system_call_4(syscall(openat), dest_directory, (positive)destination,
                                    FILE_CREATE | FILE_WRITE | O_TRUNC, mode);

        if (out < 0)
        {
                system_call_1(syscall(close), in);
                return false;
        }

        bool complete = true;

        while (complete)
        {
                bipolar bytes_read = system_call_3(syscall(read), in, (positive)shell_copy_buffer,
                                                   sizeof(shell_copy_buffer));

                if (bytes_read == 0)
                        break;

                if (bytes_read < 0)
                {
                        complete = false;
                        break;
                }

                // A short write is not an error, but ignoring what it returned
                // dropped the tail of the file without a word about it.
                positive written = 0;

                while (written < (positive)bytes_read)
                {
                        bipolar step = system_call_3(syscall(write), out,
                                                     (positive)(shell_copy_buffer + written),
                                                     (positive)bytes_read - written);

                        if (step <= 0)
                        {
                                complete = false;
                                break;
                        }

                        written += step;
                }
        }

        system_call_1(syscall(close), in);
        system_call_1(syscall(close), out);

        return complete;
}

bool shell_copy_into(bipolar source, bipolar destination, positive depth);

bool shell_copy_entry(bipolar source_directory, string_address name,
                      bipolar dest_directory, string_address destination, positive depth)
{
        file_facts facts;

        // A kernel too old for statx still gets the plain copy it always had.
        if (!shell_facts(source_directory, name, address_of facts))
                return shell_copy_file(source_directory, name, dest_directory, destination, 0666);

        if ((facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
                return shell_copy_file(source_directory, name, dest_directory, destination,
                                       facts.mode & 0777);

        if (depth == 0)
                return false;

        bipolar in = system_call_3(syscall(openat), source_directory, (positive)name,
                                   FILE_READ | O_DIRECTORY);

        if (in < 0)
                return false;

        // Already there is not a failure: the copy merges into it.
        system_call_3(syscall(mkdirat), dest_directory, (positive)destination, 0777);

        bipolar out = system_call_3(syscall(openat), dest_directory, (positive)destination,
                                    FILE_READ | O_DIRECTORY);

        if (out < 0)
        {
                system_call_1(syscall(close), in);
                return false;
        }

        bool complete = shell_copy_into(in, out, depth - 1);

        system_call_1(syscall(close), in);
        system_call_1(syscall(close), out);

        return complete;
}

bool shell_copy_into(bipolar source, bipolar destination, positive depth)
{
        p8 entries[1024];
        bool complete = true;

        while (1)
        {
                bipolar bytes_read = system_call_3(syscall(getdents64), source,
                                                   (positive)entries, sizeof(entries));

                if (bytes_read <= 0)
                        break;

                p8 address_to step = entries;

                while (step < entries + bytes_read)
                {
                        struct linux_dirent64 address_to entry = (struct linux_dirent64 address_to)step;

                        if (!(entry->d_name[0] == '.' && (entry->d_name[1] == end ||
                                                          (entry->d_name[1] == '.' && entry->d_name[2] == end))))
                        {
                                if (!shell_copy_entry(source, entry->d_name, destination,
                                                      entry->d_name, depth))
                                        complete = false;
                        }

                        step += entry->d_reclen;
                }
        }

        return complete;
}


fn shell_echo(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "n");

        if (input != null)
                write(input, 0);

        if (!(flags & SHELL_FLAG('n')))
                write("\n", 1);
}

fn shell_exec(writer write, string_address input)
{
        p8 found[768];

        // With nothing to run, exec is only there for the redirections that
        // were already applied to get here.
        if (shell_argc < 2)
                return shell_answer(0);

        if (!shell_find_in_path(shell_argv[1], found, sizeof(found)))
        {
                shell_answer(127);
                return string_format(shell_diagnostic, "exec: %s: not found\n", shell_argv[1]);
        }

        log_flush();

        // From argv[1] on, so the new program is named by what it was asked
        // for and not by the word "exec".
        system_call_3(syscall(execve), (positive)found,
                      (positive)(shell_argv + 1), (positive)shell_envp);

        shell_answer(126);
        string_format(shell_diagnostic, "exec: %s: cannot run\n", shell_argv[1]);
}
fn shell_style(writer write, positive mode)
{
        if (!shell_styles)
                return;

        positive kind = mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return write(str(TERM_BOLD TERM_BLUE));

        if (kind == MODE_LINK)
                return write(str(TERM_CYAN));

        if (kind == MODE_FILE)
                return write(str(TERM_RESET));

        write(str(TERM_YELLOW));
}

positive shell_kind_of_type(p8 type)
{
        if (type == DT_DIR)
                return MODE_DIRECTORY;

        if (type == DT_LNK)
                return MODE_LINK;

        if (type == DT_REG)
                return MODE_FILE;

        if (type == DT_CHR)
                return MODE_CHARACTER;

        if (type == DT_BLK)
                return MODE_BLOCK;

        if (type == DT_FIFO)
                return MODE_PIPE;

        if (type == DT_SOCK)
                return MODE_SOCKET;

        return 0;
}

p8 shell_kind_letter(positive mode)
{
        positive kind = mode & MODE_FORMAT;

        if (kind == MODE_DIRECTORY)
                return 'd';

        if (kind == MODE_LINK)
                return 'l';

        if (kind == MODE_CHARACTER)
                return 'c';

        if (kind == MODE_BLOCK)
                return 'b';

        if (kind == MODE_PIPE)
                return 'p';

        if (kind == MODE_SOCKET)
                return 's';

        return '-';
}

fn shell_ls_long(writer write, bipolar directory, string_address name)
{
        file_facts facts;

        if (!shell_facts(directory, name, address_of facts))
        {
                string_format(write, "?????????        ? %s\n", name);
                return;
        }

        p8 permissions[] = "rwxrwxrwx";
        p8 line[11];
        positive i = 0;

        line[0] = shell_kind_letter(facts.mode);

        for (i = 0; i < 9; i++)
                line[1 + i] = (facts.mode & ((positive)1 << (8 - i))) ? permissions[i] : '-';

        line[10] = end;

        write(line, 10);
        write(" ", 1);
        shell_number_padded(write, facts.hard_links, 3);
        write(" ", 1);
        shell_number_padded(write, facts.size, 9);
        write(" ", 1);

        shell_style(write, facts.mode);
        write(name, 0);

        if (shell_styles)
                write(str(TERM_RESET));

        write("\n", 1);
}







fn shell_mount(writer write, string_address input)
{
        if (input == null)
                return shell_diagnostic(str("mount: missing operand\n"));

        string_address destination = string_cut(input, ' ');

        if (destination == null)
                return shell_diagnostic(str("mount: missing destination\n"));

        if (!system_call_4(syscall(mount), (positive)input, (positive)destination, (positive)input, MS_BIND))
                return;

        string_format(shell_diagnostic, "mount: Cannot mount filesystem: %s\n", input);
}

bool shell_remove_into(bipolar directory, positive depth);

bool shell_remove_entry(bipolar directory, string_address name, positive depth)
{
        file_facts facts;
        bool known = shell_facts(directory, name, address_of facts);

        if (!known || (facts.mode & MODE_FORMAT) != MODE_DIRECTORY)
        {
                if (system_call_3(syscall(unlinkat), directory, (positive)name, 0) == 0)
                        return true;

                // Without statx there is no telling a directory from a file
                // beforehand, so a refused unlink is worth one more try below.
                if (known)
                        return false;
        }

        if (depth == 0)
                return false;

        bipolar inner = system_call_3(syscall(openat), directory, (positive)name,
                                      FILE_READ | O_DIRECTORY);

        if (inner < 0)
                return false;

        bool emptied = shell_remove_into(inner, depth - 1);

        system_call_1(syscall(close), inner);

        if (!emptied)
                return false;

        return system_call_3(syscall(unlinkat), directory, (positive)name, AT_REMOVEDIR) == 0;
}

/*
        Removing an entry while reading through getdents64 moves the ones that
        come after it, and the next read then skips them, so a single pass
        leaves a directory that is not empty. Each pass starts again from the
        beginning and the walk ends on the first pass that removes nothing.
*/
bool shell_remove_into(bipolar directory, positive depth)
{
        p8 entries[1024];
        bool complete = true;

        while (1)
        {
                bool removed_any = false;

                if (system_call_3(syscall(lseek), directory, 0, FILE_SEEK_SET) < 0)
                        return false;

                complete = true;

                while (1)
                {
                        bipolar bytes_read = system_call_3(syscall(getdents64), directory,
                                                           (positive)entries, sizeof(entries));

                        if (bytes_read <= 0)
                                break;

                        p8 address_to step = entries;

                        while (step < entries + bytes_read)
                        {
                                struct linux_dirent64 address_to entry = (struct linux_dirent64 address_to)step;

                                if (!(entry->d_name[0] == '.' && (entry->d_name[1] == end ||
                                                                  (entry->d_name[1] == '.' && entry->d_name[2] == end))))
                                {
                                        if (shell_remove_entry(directory, entry->d_name, depth))
                                                removed_any = true;
                                        else
                                                complete = false;
                                }

                                step += entry->d_reclen;
                        }
                }

                if (!removed_any)
                        break;
        }

        return complete;
}


fn shell_pwd(writer write, string_address input)
{
        p8 out_buffer[4096];

        system_call_2(syscall(getcwd), (positive)out_buffer, 4096);

        string_format(write, "%s\n", out_buffer);
}

fn shell_trap_exit();

fn shell_exit(writer write, string_address input)
{
        bipolar exit_code = shell_status_entering;
        bool good;

        if (shell_argc > 1)
                exit_code = shell_signed(shell_argv[1], address_of good) & 0xff;

        shell_status = (b32)exit_code;
        shell_trap_exit();

        log_flush();

        exit(exit_code);
}


#define UTSNAME_FIELD 65
#define UTSNAME_FIELDS 6

fn shell_uname_field(writer write, positive address_to shown, string_address value)
{
        if (address_to shown)
                write(" ", 1);

        address_to shown += 1;

        write(value, 0);
}



#define REBOOT_MAGIC 0xfee1dead
#define REBOOT_MAGIC_SECOND 672274793
#define REBOOT_RESTART 0x01234567
#define REBOOT_POWER_OFF 0x4321fedc

// The rootfs lives in RAM. Whatever is still in flight is all there is, so it
// goes out to whatever backing store there is before the machine stops.
fn shell_stop(writer write, positive command)
{
        write(str("Syncing...\n"));
        log_flush();

        system_call(syscall(sync));

        bipolar result = system_call_4(syscall(reboot), REBOOT_MAGIC, REBOOT_MAGIC_SECOND, command, 0);

        string_format(write, "Cannot stop the machine: %b\n", result);
        log_flush();
}

fn shell_reboot(writer write, string_address input)
{
        shell_stop(write, REBOOT_RESTART);
}

fn shell_poweroff(writer write, string_address input)
{
        shell_stop(write, REBOOT_POWER_OFF);
}


/*
        The POSIX builtins.

        These read shell_argv rather than the joined line the older commands in
        this file are handed: printf, test and set all turn on knowing where
        one word ended and the next began, which joining throws away.
*/

#define ACCESS_READ 4
#define ACCESS_WRITE 2

// The shell's own ioctl, spelled here because shell.c names it after this file
// has already been read.
#define BUILTIN_TCGETS 0x5401u

bool word_is(string_address word, string_address text)
{
        return word && !string_compare(word, text);
}

fn env_unset(string_address name)
{
        positive length = string_length(name);
        positive index = 0;

        while (shell_envp[index])
        {
                string_address entry = shell_envp[index];
                string_address mark = string_first_of(entry, '=');

                if (mark && (positive)(mark - entry) == length)
                {
                        positive at = 0;

                        while (at < length && string_get(entry + at) == string_get(name + at))
                                at++;

                        if (at == length)
                        {
                                while (shell_envp[index + 1])
                                {
                                        shell_envp[index] = shell_envp[index + 1];
                                        index++;
                                }

                                shell_envp[index] = null;
                                return;
                        }
                }

                index++;
        }
}

/*
        Hangs an already built "name=value" on the environment list.

        env_set copies into its own block, which is the wrong shape for the
        positional parameters: those are rewritten wholesale every time and
        would eat the block one copy at a time. These keep their own storage
        and only the pointer is handed over.
*/
bool env_place(string_address entry)
{
        string_address mark = string_first_of(entry, '=');
        positive length;
        positive index = 0;

        if (!mark)
                return false;

        length = mark - entry;

        while (shell_envp[index])
        {
                string_address other = shell_envp[index];
                string_address at_other = string_first_of(other, '=');

                if (at_other && (positive)(at_other - other) == length)
                {
                        positive at = 0;

                        while (at < length && string_get(other + at) == string_get(entry + at))
                                at++;

                        if (at == length)
                        {
                                shell_envp[index] = entry;
                                return true;
                        }
                }

                index++;
        }

        if (index >= ENV_MAX_ENTRIES)
                return false;

        shell_envp[index] = entry;
        shell_envp[index + 1] = null;

        return true;
}

positive shell_digits(p8 address_to into, positive value)
{
        p8 digits[24];
        positive length = 0;
        positive at = 0;

        if (value == 0)
                digits[length++] = '0';

        while (value && length < sizeof(digits))
        {
                digits[length++] = '0' + (value % 10);
                value /= 10;
        }

        while (length)
                into[at++] = digits[--length];

        into[at] = end;

        return at;
}

fn env_set_number(string_address name, positive value)
{
        p8 text[24];

        shell_digits(text, value);
        env_set(name, text);
}

// Forwards, and signed. string_to_bipolar reads from the end of the string,
// which answers 5 for "0.5" and 0 for anything with a space after it.
bipolar shell_signed(string_address input, bool address_to good)
{
        bipolar value = 0;
        bool negative = false;
        bool any = false;

        address_to good = false;

        if (!input)
                return 0;

        while (string_is(input, ' ') || string_is(input, '\t'))
                input++;

        if (string_is(input, '-') || string_is(input, '+'))
        {
                negative = string_is(input, '-');
                input++;
        }

        while (string_get(input) >= '0' && string_get(input) <= '9')
        {
                value = value * 10 + (string_get(input++) - '0');
                any = true;
        }

        if (!any || string_get(input))
                return 0;

        address_to good = true;

        return negative ? -value : value;
}

/*
        The positional parameters live in expand.c, which is what reads them.

        They used to be mirrored into the environment, because that was the
        only place the expander looked a name up and $1 is a name to it. The
        mirror also went to every child through execve, which no shell does,
        and cost an environment entry per parameter per call.
*/
#define POSITIONAL_MAX 64

#ifdef EXPAND_PARAMETERS
extern string_address shell_parameter[];
extern positive shell_parameter_count;
bool shell_parameters_set(string_address address_to words, positive count);
fn shell_parameters_shift(positive count);
#else
// Without the expander beside this file there is nothing to be positional
// about -- programs/edit.c wants a few of these commands and no shell.
static string_address shell_parameter[1];
static positive shell_parameter_count;
static bool shell_parameters_set(string_address address_to words, positive count)
{
        return false;
}
static fn shell_parameters_shift(positive count) {}
#endif

fn shell_set(writer write, string_address input)
{
        positive index = 1;
        bool operands = false;

        if (shell_argc < 2)
        {
                positive at = 0;

                while (shell_envp[at])
                        string_format(write, "%s\n", shell_envp[at++]);

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];

                if (word_is(word, "--"))
                {
                        operands = true;
                        index++;
                        break;
                }

                if ((string_is(word, '-') || string_is(word, '+')) &&
                    string_not(word + 1, end))
                {
                        bool on = string_is(word, '-');
                        string_address letter = word + 1;

                        while (string_get(letter))
                        {
                                p8 value = string_get(letter);

                                if (value >= 'a' && value <= 'z')
                                {
                                        if (on)
                                                shell_options |= SHELL_FLAG(value);
                                        else
                                                shell_options &= ~SHELL_FLAG(value);
                                }

                                letter++;
                        }

                        index++;
                        continue;
                }

                operands = true;
                break;
        }

        if (operands)
        {
                string_address values[POSITIONAL_MAX];
                positive count = 0;

                while (index < shell_argc && count < POSITIONAL_MAX)
                        values[count++] = shell_argv[index++];

                shell_parameters_set(values, count);
        }

        shell_answer(0);
}

fn shell_shift(writer write, string_address input)
{
        positive amount = 1;

        if (shell_argc > 1)
                amount = shell_number(shell_argv[1]);

        if (amount > shell_parameter_count)
                return shell_answer(1);

        shell_parameters_shift(amount);

        shell_answer(0);
}

fn shell_unset(writer write, string_address input)
{
        positive index = 1;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];

                if (string_is(word, '-') && string_not(word + 1, end))
                {
                        index++;
                        continue;
                }

                if (env_readonly(word))
                        return shell_answer(1);

                env_unset(word);
                index++;
        }

        shell_answer(0);
}

fn shell_readonly(writer write, string_address input)
{
        positive index = 1;

        if (shell_argc < 2)
        {
                positive at = 0;

                while (at < readonly_count)
                {
                        string_address value = env_get(readonly_name[at]);

                        if (value)
                                string_format(write, "readonly %s=%s\n", readonly_name[at], value);
                        else
                                string_format(write, "readonly %s\n", readonly_name[at]);

                        at++;
                }

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                string_address mark = string_first_of(word, '=');
                positive length = mark ? (positive)(mark - word) : string_length(word);

                if (mark)
                {
                        p8 name[128];

                        if (length < sizeof(name))
                        {
                                memory_copy(name, word, length);
                                name[length] = end;
                                env_set(name, mark + 1);
                        }
                }

                if (readonly_count < READONLY_MAX &&
                    readonly_used + length + 1 <= READONLY_STORAGE)
                {
                        string_address kept = readonly_storage + readonly_used;

                        memory_copy(kept, word, length);
                        kept[length] = end;
                        readonly_used += length + 1;

                        if (!env_readonly(kept))
                                readonly_name[readonly_count++] = kept;
                }

                index++;
        }

        shell_answer(0);
}

/*
        test, and the same thing spelled with brackets.

        The words are read straight out of argv: an expression is words, and
        rebuilding it from a joined line would have to guess where the quoting
        used to be. POSIX resolves the short forms by counting words first, so
        a binary operator in the middle wins over a unary one at the front and
        "-f = x" compares two strings.
*/

#define TEST_SAME 1
#define TEST_DIFFERENT 2
#define TEST_EQUAL 3
#define TEST_UNEQUAL 4
#define TEST_LESS 5
#define TEST_LESS_EQUAL 6
#define TEST_GREATER 7
#define TEST_GREATER_EQUAL 8
#define TEST_NEWER 9
#define TEST_OLDER 10
#define TEST_SAME_FILE 11

static positive test_at;
static positive test_stop;
static bool test_bad;

bool test_facts(string_address path, file_facts address_to out, bool follow)
{
        return system_call_5(syscall(statx), AT_FDCWD, (positive)path,
                             follow ? 0 : AT_SYMLINK_NOFOLLOW,
                             STATX_BASIC, (positive)out) == 0;
}

bool test_unary(p8 op, string_address value)
{
        file_facts facts;

        if (op == 'n')
                return value && string_not(value, end);

        if (op == 'z')
                return !value || string_is(value, end);

        if (op == 't')
        {
                p8 settings[64];
                bipolar descriptor = value ? (bipolar)shell_number(value) : 1;

                return system_call_3(syscall(ioctl), descriptor, BUILTIN_TCGETS,
                                     (positive)settings) == 0;
        }

        if (op == 'r' || op == 'w' || op == 'x')
        {
                positive mode = op == 'r' ? ACCESS_READ
                                          : (op == 'w' ? ACCESS_WRITE : ACCESS_EXECUTE);

                return system_call_4(syscall(faccessat), AT_FDCWD, (positive)value,
                                     mode, 0) == 0;
        }

        // The only two that are asked about the link itself; everything else
        // follows it, which is what POSIX says and what statx does with no
        // flags at all.
        if (op == 'h' || op == 'L')
        {
                if (!test_facts(value, address_of facts, false))
                        return false;

                return (facts.mode & MODE_FORMAT) == MODE_LINK;
        }

        if (!test_facts(value, address_of facts, true))
                return false;

        if (op == 'e')
                return true;

        if (op == 'f')
                return (facts.mode & MODE_FORMAT) == MODE_FILE;

        if (op == 'd')
                return (facts.mode & MODE_FORMAT) == MODE_DIRECTORY;

        if (op == 'b')
                return (facts.mode & MODE_FORMAT) == MODE_BLOCK;

        if (op == 'c')
                return (facts.mode & MODE_FORMAT) == MODE_CHARACTER;

        if (op == 'p')
                return (facts.mode & MODE_FORMAT) == MODE_PIPE;

        if (op == 'S')
                return (facts.mode & MODE_FORMAT) == MODE_SOCKET;

        if (op == 's')
                return facts.size > 0;

        if (op == 'g')
                return (facts.mode & 02000) != 0;

        if (op == 'u')
                return (facts.mode & 04000) != 0;

        if (op == 'k')
                return (facts.mode & 01000) != 0;

        return false;
}

// stx_mtime sits at offset 112 of the kernel's statx, which is 56 bytes past
// where file_facts stops spelling the fields out.
b64 test_modified(file_facts address_to facts)
{
        b64 seconds;

        memory_copy(address_of seconds, facts->remainder + 56, sizeof(seconds));

        return seconds;
}

bool test_is_unary(string_address word)
{
        p8 letter;

        if (!word || string_not(word, '-') || !string_get(word + 1) || string_get(word + 2))
                return false;

        letter = string_get(word + 1);

        return letter == 'b' || letter == 'c' || letter == 'd' || letter == 'e' ||
               letter == 'f' || letter == 'g' || letter == 'h' || letter == 'k' ||
               letter == 'n' || letter == 'p' || letter == 'r' || letter == 's' ||
               letter == 't' || letter == 'u' || letter == 'w' || letter == 'x' ||
               letter == 'z' || letter == 'L' || letter == 'S';
}

positive test_is_binary(string_address word)
{
        if (!word)
                return 0;

        if (word_is(word, "="))
                return TEST_SAME;

        if (word_is(word, "!="))
                return TEST_DIFFERENT;

        if (word_is(word, "-eq"))
                return TEST_EQUAL;

        if (word_is(word, "-ne"))
                return TEST_UNEQUAL;

        if (word_is(word, "-lt"))
                return TEST_LESS;

        if (word_is(word, "-le"))
                return TEST_LESS_EQUAL;

        if (word_is(word, "-gt"))
                return TEST_GREATER;

        if (word_is(word, "-ge"))
                return TEST_GREATER_EQUAL;

        if (word_is(word, "-nt"))
                return TEST_NEWER;

        if (word_is(word, "-ot"))
                return TEST_OLDER;

        if (word_is(word, "-ef"))
                return TEST_SAME_FILE;

        return 0;
}

bool test_compare(positive kind, string_address left, string_address right)
{
        bipolar first;
        bipolar second;
        bool first_good;
        bool second_good;

        if (kind == TEST_SAME)
                return !string_compare(left, right);

        if (kind == TEST_DIFFERENT)
                return string_compare(left, right) != 0;

        if (kind == TEST_NEWER || kind == TEST_OLDER || kind == TEST_SAME_FILE)
        {
                file_facts one;
                file_facts two;

                if (!test_facts(left, address_of one, true) ||
                    !test_facts(right, address_of two, true))
                        return false;

                if (kind == TEST_SAME_FILE)
                        return one.inode == two.inode;

                if (kind == TEST_NEWER)
                        return test_modified(address_of one) > test_modified(address_of two);

                return test_modified(address_of one) < test_modified(address_of two);
        }

        first = shell_signed(left, address_of first_good);
        second = shell_signed(right, address_of second_good);

        if (!first_good || !second_good)
        {
                test_bad = true;
                return false;
        }

        if (kind == TEST_EQUAL)
                return first == second;

        if (kind == TEST_UNEQUAL)
                return first != second;

        if (kind == TEST_LESS)
                return first < second;

        if (kind == TEST_LESS_EQUAL)
                return first <= second;

        if (kind == TEST_GREATER)
                return first > second;

        return first >= second;
}

bool test_expression();

bool test_primary()
{
        string_address word;

        if (test_at >= test_stop)
        {
                test_bad = true;
                return false;
        }

        word = shell_argv[test_at];

        if (word_is(word, "(") && test_at + 2 <= test_stop)
        {
                bool value;

                test_at++;
                value = test_expression();

                if (test_at < test_stop && word_is(shell_argv[test_at], ")"))
                        test_at++;
                else
                        test_bad = true;

                return value;
        }

        // Three words are a binary test before they are anything else.
        if (test_at + 2 < test_stop)
        {
                positive kind = test_is_binary(shell_argv[test_at + 1]);

                if (kind)
                {
                        bool value = test_compare(kind, shell_argv[test_at],
                                                  shell_argv[test_at + 2]);

                        test_at += 3;
                        return value;
                }
        }

        if (test_at + 1 < test_stop && test_is_unary(word))
        {
                bool value = test_unary(string_get(word + 1), shell_argv[test_at + 1]);

                test_at += 2;
                return value;
        }

        test_at++;

        return word && string_not(word, end);
}

bool test_negation()
{
        if (test_at + 1 < test_stop && word_is(shell_argv[test_at], "!"))
        {
                test_at++;
                return !test_negation();
        }

        return test_primary();
}

bool test_conjunction()
{
        bool value = test_negation();

        // The right side is read whatever the left said: skipping it would
        // leave the parser standing in the middle of the expression.
        while (test_at < test_stop && word_is(shell_argv[test_at], "-a"))
        {
                bool other;

                test_at++;
                other = test_negation();
                value = value && other;
        }

        return value;
}

bool test_expression()
{
        bool value = test_conjunction();

        while (test_at < test_stop && word_is(shell_argv[test_at], "-o"))
        {
                bool other;

                test_at++;
                other = test_conjunction();
                value = value || other;
        }

        return value;
}

fn shell_test(writer write, string_address input)
{
        bool value;

        test_at = 1;
        test_stop = shell_argc;
        test_bad = false;

        if (word_is(shell_argv[0], "["))
        {
                if (shell_argc < 2 || !word_is(shell_argv[shell_argc - 1], "]"))
                        return shell_answer(2);

                test_stop = shell_argc - 1;
        }

        if (test_at >= test_stop)
                return shell_answer(1);

        value = test_expression();

        if (test_bad || test_at != test_stop)
                return shell_answer(2);

        shell_answer(value ? 0 : 1);
}

fn shell_true(writer write, string_address input)
{
        shell_answer(0);
}

fn shell_false(writer write, string_address input)
{
        shell_answer(1);
}

/*
        printf.

        Not string_format: this one has to reuse its format until the arguments
        run out, take width and precision from the format, and read backslash
        escapes that the shell's own quoting left alone.
*/

static positive printf_argument;
static bool printf_took;
static p8 printf_nothing[1];

string_address printf_next()
{
        if (printf_argument < shell_argc)
        {
                printf_took = true;
                return shell_argv[printf_argument++];
        }

        return printf_nothing;
}

positive printf_render(p8 address_to into, positive value, positive base, bool upper)
{
        p8 digits[64];
        positive length = 0;
        positive at = 0;

        if (value == 0)
                digits[length++] = '0';

        while (value && length < sizeof(digits))
        {
                p8 digit = value % base;

                digits[length++] = digit < 10 ? '0' + digit
                                              : (upper ? 'A' : 'a') + (digit - 10);
                value /= base;
        }

        while (length)
                into[at++] = digits[--length];

        return at;
}

fn printf_fill(writer write, positive count, p8 filler)
{
        while (count--)
                write(address_of filler, 1);
}

/*
        One backslash escape, already past the backslash. Answers where to
        carry on reading; an octal run is up to three digits, and \0 in front
        of it is what POSIX writes even though every shell also takes it bare.
*/
string_address printf_escape(writer write, string_address step)
{
        p8 value;

        if (string_is(step, '0') || (string_get(step) >= '1' && string_get(step) <= '7'))
        {
                positive number = 0;
                positive taken = 0;

                if (string_is(step, '0'))
                        step++;

                while (taken < 3 && string_get(step) >= '0' && string_get(step) <= '7')
                {
                        number = number * 8 + (string_get(step++) - '0');
                        taken++;
                }

                value = (p8)number;
                write(address_of value, 1);

                return step;
        }

        value = string_get(step);

        if (value == 'n')
                value = '\n';
        else if (value == 't')
                value = '\t';
        else if (value == 'r')
                value = '\r';
        else if (value == 'a')
                value = 7;
        else if (value == 'b')
                value = 8;
        else if (value == 'e')
                value = 27;
        else if (value == 'f')
                value = 12;
        else if (value == 'v')
                value = 11;
        else if (value == '\\')
                value = '\\';
        else
        {
                p8 slash = '\\';

                write(address_of slash, 1);
        }

        if (string_get(step))
        {
                write(address_of value, 1);
                step++;
        }

        return step;
}

fn printf_escaped(writer write, string_address text)
{
        while (string_get(text))
        {
                if (string_is(text, '\\'))
                {
                        text = printf_escape(write, text + 1);
                        continue;
                }

                write(text, 1);
                text++;
        }
}

fn printf_number(writer write, positive magnitude, p8 sign, positive base, bool upper,
                 positive width, bipolar precision, bool left, bool zero)
{
        p8 body[80];
        positive length = printf_render(body, magnitude, base, upper);
        positive zeros = 0;
        positive head = sign ? 1 : 0;

        if (precision >= 0 && (positive)precision > length)
                zeros = (positive)precision - length;

        if (zero && !left && precision < 0 && width > head + length + zeros)
                zeros = width - head - length;

        if (!left)
                printf_fill(write, width > head + zeros + length ? width - head - zeros - length : 0, ' ');

        if (sign)
                write(address_of sign, 1);

        printf_fill(write, zeros, '0');
        write(body, length);

        if (left)
                printf_fill(write, width > head + zeros + length ? width - head - zeros - length : 0, ' ');
}

fn printf_one(writer write, string_address format)
{
        string_address step = format;

        while (string_get(step))
        {
                bool left = false;
                bool zero = false;
                bool plus = false;
                bool space = false;
                positive width = 0;
                bipolar precision = -1;
                p8 conversion;

                if (string_is(step, '\\'))
                {
                        step = printf_escape(write, step + 1);
                        continue;
                }

                if (string_not(step, '%'))
                {
                        write(step, 1);
                        step++;
                        continue;
                }

                step++;

                while (string_is(step, '-') || string_is(step, '0') ||
                       string_is(step, '+') || string_is(step, ' ') ||
                       string_is(step, '#'))
                {
                        if (string_is(step, '-'))
                                left = true;
                        else if (string_is(step, '0'))
                                zero = true;
                        else if (string_is(step, '+'))
                                plus = true;
                        else if (string_is(step, ' '))
                                space = true;

                        step++;
                }

                if (string_is(step, '*'))
                {
                        bool good;

                        width = (positive)shell_signed(printf_next(), address_of good);
                        step++;
                }
                else
                {
                        while (string_get(step) >= '0' && string_get(step) <= '9')
                                width = width * 10 + (string_get(step++) - '0');
                }

                if (string_is(step, '.'))
                {
                        step++;
                        precision = 0;

                        if (string_is(step, '*'))
                        {
                                bool good;

                                precision = shell_signed(printf_next(), address_of good);
                                step++;
                        }
                        else
                        {
                                while (string_get(step) >= '0' && string_get(step) <= '9')
                                        precision = precision * 10 + (string_get(step++) - '0');
                        }
                }

                // The length modifiers say nothing here: every number this
                // reads is already as wide as the machine.
                while (string_is(step, 'l') || string_is(step, 'h') ||
                       string_is(step, 'z') || string_is(step, 'j'))
                        step++;

                conversion = string_get(step);

                if (!conversion)
                        break;

                step++;

                if (conversion == '%')
                {
                        write("%", 1);
                        continue;
                }

                if (conversion == 's' || conversion == 'b')
                {
                        string_address value = printf_next();
                        positive length = string_length(value);

                        if (precision >= 0 && (positive)precision < length)
                                length = (positive)precision;

                        if (conversion == 'b')
                        {
                                // The escapes are in the argument, so a width
                                // around them would have to be measured after
                                // they were read; nothing asks for that.
                                printf_escaped(write, value);
                                continue;
                        }

                        if (!left)
                                printf_fill(write, width > length ? width - length : 0, ' ');

                        write(value, length);

                        if (left)
                                printf_fill(write, width > length ? width - length : 0, ' ');

                        continue;
                }

                if (conversion == 'c')
                {
                        string_address value = printf_next();
                        positive length = string_get(value) ? 1 : 0;

                        if (!left)
                                printf_fill(write, width > length ? width - length : 0, ' ');

                        if (length)
                                write(value, 1);

                        if (left)
                                printf_fill(write, width > length ? width - length : 0, ' ');

                        continue;
                }

                if (conversion == 'd' || conversion == 'i')
                {
                        bool good;
                        bipolar value = shell_signed(printf_next(), address_of good);
                        p8 sign = 0;

                        if (value < 0)
                                sign = '-';
                        else if (plus)
                                sign = '+';
                        else if (space)
                                sign = ' ';

                        printf_number(write, value < 0 ? (positive)(-value) : (positive)value,
                                      sign, 10, false, width, precision, left, zero);
                        continue;
                }

                if (conversion == 'u' || conversion == 'o' ||
                    conversion == 'x' || conversion == 'X')
                {
                        bool good;
                        bipolar value = shell_signed(printf_next(), address_of good);
                        positive base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);

                        printf_number(write, (positive)value, 0, base, conversion == 'X',
                                      width, precision, left, zero);
                        continue;
                }

                // An unknown conversion is written out as it stood, which is
                // more use than swallowing it.
                write("%", 1);
                write(address_of conversion, 1);
        }
}

fn shell_printf(writer write, string_address input)
{
        string_address format;

        if (shell_argc < 2)
                return shell_answer(2);

        format = shell_argv[1];
        printf_argument = 2;

        while (1)
        {
                printf_took = false;
                printf_one(write, format);

                // A format with no conversion in it would otherwise run for as
                // long as there were arguments left.
                if (printf_argument >= shell_argc || !printf_took)
                        break;
        }

        shell_answer(0);
}

/*
        read.

        A byte at a time, because the line arrives on the same descriptor the
        shell is reading its script from and anything larger swallows what
        comes after.
*/
fn shell_read(writer write, string_address input)
{
        p8 line[2048];
        positive length = 0;
        bool raw = false;
        bool got_any = false;
        positive index = 1;
        positive at = 0;
        positive names;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_not(shell_argv[index] + 1, end))
        {
                string_address letter = shell_argv[index] + 1;

                while (string_get(letter))
                {
                        if (string_get(letter) == 'r')
                                raw = true;

                        letter++;
                }

                index++;
        }

        names = index;

        while (length < sizeof(line) - 1)
        {
                p8 value;

                if (system_call_3(syscall(read), 0, (positive)address_of value, 1) != 1)
                        break;

                got_any = true;

                if (value == '\n')
                        break;

                if (!raw && value == '\\')
                {
                        p8 next;

                        if (system_call_3(syscall(read), 0, (positive)address_of next, 1) != 1)
                                break;

                        // A backslash before the newline joins the two lines.
                        if (next == '\n')
                                continue;

                        line[length++] = next;
                        continue;
                }

                line[length++] = value;
        }

        line[length] = end;

        if (names >= shell_argc)
        {
                env_set("REPLY", line);
                return shell_answer(got_any ? 0 : 1);
        }

        while (names < shell_argc)
        {
                positive begin;

                while (at < length && (line[at] == ' ' || line[at] == '\t'))
                        at++;

                begin = at;

                // The last name takes everything that is left, splitting and
                // all, which is what makes "read line" read a line.
                if (names + 1 == shell_argc)
                {
                        positive stop = length;

                        while (stop > begin && (line[stop - 1] == ' ' || line[stop - 1] == '\t'))
                                stop--;

                        line[stop] = end;
                        env_set(shell_argv[names], line + begin);
                        at = length;
                        names++;
                        continue;
                }

                while (at < length && line[at] != ' ' && line[at] != '\t')
                        at++;

                if (at < length)
                        line[at++] = end;

                env_set(shell_argv[names], line + begin);
                names++;
        }

        shell_answer(got_any ? 0 : 1);
}

/*
        getopts.

        One option per call, its place kept in OPTIND and, within a bundled
        word, in an offset of its own that OPTIND has no room for.
*/
static positive getopts_offset;

fn shell_getopts(writer write, string_address input)
{
        string_address options;
        string_address name;
        string_address list[POSITIONAL_MAX];
        positive count = 0;
        positive optind;
        string_address word;
        string_address found;
        p8 letter;
        p8 value[2];

        if (shell_argc < 3)
                return shell_answer(2);

        options = shell_argv[1];
        name = shell_argv[2];

        if (shell_argc > 3)
        {
                positive index = 3;

                while (index < shell_argc && count < POSITIONAL_MAX)
                        list[count++] = shell_argv[index++];
        }
        else
        {
                while (count < shell_parameter_count)
                {
                        list[count] = shell_parameter[count];
                        count++;
                }
        }

        optind = shell_number(env_get("OPTIND"));

        if (optind < 1)
                optind = 1;

        if (optind - 1 >= count)
                return shell_answer(1);

        word = list[optind - 1];

        if (string_not(word, '-') || string_is(word + 1, end))
                return shell_answer(1);

        if (word_is(word, "--"))
        {
                env_set_number("OPTIND", optind + 1);
                return shell_answer(1);
        }

        if (getopts_offset < 1)
                getopts_offset = 1;

        letter = string_get(word + getopts_offset);
        value[0] = letter;
        value[1] = end;

        found = letter == ':' ? null : string_first_of(options, letter);

        if (!found)
        {
                env_set(name, "?");
                env_unset("OPTARG");

                getopts_offset++;

                if (!string_get(word + getopts_offset))
                {
                        getopts_offset = 0;
                        env_set_number("OPTIND", optind + 1);
                }

                return shell_answer(0);
        }

        getopts_offset++;

        if (string_is(found + 1, ':'))
        {
                if (string_get(word + getopts_offset))
                {
                        env_set("OPTARG", word + getopts_offset);
                }
                else if (optind < count)
                {
                        env_set("OPTARG", list[optind]);
                        optind++;
                }
                else
                {
                        env_set(name, ":");
                        env_set("OPTARG", value);
                        getopts_offset = 0;
                        env_set_number("OPTIND", optind + 1);
                        return shell_answer(0);
                }

                env_set(name, value);
                getopts_offset = 0;
                env_set_number("OPTIND", optind + 1);

                return shell_answer(0);
        }

        env_set(name, value);

        if (!string_get(word + getopts_offset))
        {
                getopts_offset = 0;
                env_set_number("OPTIND", optind + 1);
        }

        shell_answer(0);
}

fn shell_umask(writer write, string_address input)
{
        positive mask;
        positive shift = 9;

        if (shell_argc > 1)
        {
                string_address step = shell_argv[1];
                positive value = 0;

                while (string_get(step) >= '0' && string_get(step) <= '7')
                        value = value * 8 + (string_get(step++) - '0');

                system_call_1(syscall(umask), value);

                return shell_answer(0);
        }

        // The only way to read it is to set it, so it is put straight back.
        mask = system_call_1(syscall(umask), 0);
        system_call_1(syscall(umask), mask);

        while (shift)
        {
                p8 digit;

                shift -= 3;
                digit = '0' + ((mask >> shift) & 7);

                if (shift == 6)
                        write("0", 1);

                write(address_of digit, 1);
        }

        write("\n", 1);

        shell_answer(0);
}

/*
        times.

        The kernel counts in clock ticks and there are a hundred of them to the
        second on every Linux that matters; the field is what a program is told
        through AT_CLKTCK and nothing here can be told anything.
*/
#define CLOCK_TICKS 100

typedef struct
{
        bipolar user;
        bipolar system;
        bipolar children_user;
        bipolar children_system;
} shell_clocks;

fn shell_time_written(writer write, bipolar ticks)
{
        positive seconds;
        positive thousandths;
        positive digit;

        if (ticks < 0)
                ticks = 0;

        seconds = (positive)ticks / CLOCK_TICKS;
        thousandths = ((positive)ticks % CLOCK_TICKS) * (1000 / CLOCK_TICKS);

        shell_number_padded(write, seconds / 60, 0);
        write("m", 1);
        shell_number_padded(write, seconds % 60, 0);
        write(".", 1);

        digit = '0' + thousandths / 100;
        write(address_of digit, 1);
        digit = '0' + (thousandths / 10) % 10;
        write(address_of digit, 1);
        digit = '0' + thousandths % 10;
        write(address_of digit, 1);

        write("s", 1);
}

fn shell_times(writer write, string_address input)
{
        shell_clocks clocks;

        memory_fill(address_of clocks, 0, sizeof(clocks));
        system_call_1(syscall(times), (positive)address_of clocks);

        shell_time_written(write, clocks.user);
        write(" ", 1);
        shell_time_written(write, clocks.system);
        write("\n", 1);

        shell_time_written(write, clocks.children_user);
        write(" ", 1);
        shell_time_written(write, clocks.children_system);
        write("\n", 1);

        shell_answer(0);
}

/*
        trap.

        The handler is recorded and nothing runs it yet: what runs a command is
        above this file, and a script that sets a trap must still get through
        the line rather than falling over on an unknown command.
*/
#define TRAP_MAX 24
#define TRAP_STORAGE 1024

typedef struct
{
        positive number;
        string_address action;
} shell_trap_entry;

static shell_trap_entry trap_table[TRAP_MAX];
static positive trap_count;
static p8 trap_storage[TRAP_STORAGE];
static positive trap_used;

static string_address trap_names[] = {
    "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    null,
};

bipolar trap_number(string_address word)
{
        positive index = 0;
        bool good;
        bipolar value;

        if (!word)
                return -1;

        if (string_is(word, 'S') && string_is(word + 1, 'I') && string_is(word + 2, 'G'))
                word += 3;

        while (trap_names[index])
        {
                if (word_is(word, trap_names[index]))
                        return (bipolar)index;

                index++;
        }

        value = shell_signed(word, address_of good);

        return good ? value : -1;
}

fn trap_forget(positive number)
{
        positive index = 0;

        while (index < trap_count)
        {
                if (trap_table[index].number == number)
                {
                        while (index + 1 < trap_count)
                        {
                                trap_table[index] = trap_table[index + 1];
                                index++;
                        }

                        trap_count--;
                        return;
                }

                index++;
        }
}

string_address trap_action(positive number)
{
        positive index = 0;

        while (index < trap_count)
        {
                if (trap_table[index].number == number)
                        return trap_table[index].action;

                index++;
        }

        return null;
}

fn shell_trap(writer write, string_address input)
{
        positive index = 1;
        string_address action;

        if (shell_argc < 2)
        {
                positive at = 0;

                while (at < trap_count)
                {
                        positive number = trap_table[at].number;

                        string_format(write, "trap -- '%s' ", trap_table[at].action);

                        if (number < 16)
                                string_format(write, "%s", trap_names[number]);
                        else
                                shell_number_padded(write, number, 0);

                        write("\n", 1);
                        at++;
                }

                return shell_answer(0);
        }

        if (word_is(shell_argv[index], "--"))
                index++;

        if (index >= shell_argc)
                return shell_answer(0);

        action = shell_argv[index++];

        // "trap - INT" and "trap '' INT" both take the handler away; the
        // difference between them is what the signal is set to, which is not
        // this file's to set yet.
        if (word_is(action, "-"))
                action = null;

        while (index < shell_argc)
        {
                bipolar number = trap_number(shell_argv[index]);

                index++;

                if (number < 0)
                        continue;

                trap_forget((positive)number);

                if (!action || trap_count >= TRAP_MAX)
                        continue;

                {
                        positive length = string_length(action);

                        if (trap_used + length + 1 > TRAP_STORAGE)
                                continue;

                        trap_table[trap_count].number = (positive)number;
                        trap_table[trap_count].action = trap_storage + trap_used;

                        memory_copy(trap_storage + trap_used, action, length + 1);
                        trap_used += length + 1;
                        trap_count++;
                }
        }

        shell_answer(0);
}

/*
        alias.

        Recorded, listed and taken away here. Putting one in front of a command
        happens where a line is read, which is not this file, so what this holds
        is the table that side will ask.
*/
#define ALIAS_MAX 32
#define ALIAS_STORAGE 2048

typedef struct
{
        string_address name;
        string_address value;
} shell_alias_entry;

static shell_alias_entry alias_table[ALIAS_MAX];
static positive alias_count;
static p8 alias_storage[ALIAS_STORAGE];
static positive alias_used;

string_address alias_lookup(string_address name)
{
        positive index = 0;

        while (index < alias_count)
        {
                if (word_is(alias_table[index].name, name))
                        return alias_table[index].value;

                index++;
        }

        return null;
}

bool alias_record(string_address name, positive name_length, string_address value)
{
        positive value_length = string_length(value);
        positive index = 0;

        if (alias_used + name_length + 1 + value_length + 1 > ALIAS_STORAGE)
                return false;

        {
                string_address kept_name = alias_storage + alias_used;

                memory_copy(kept_name, name, name_length);
                kept_name[name_length] = end;
                alias_used += name_length + 1;

                {
                        string_address kept_value = alias_storage + alias_used;

                        memory_copy(kept_value, value, value_length + 1);
                        alias_used += value_length + 1;

                        while (index < alias_count)
                        {
                                if (word_is(alias_table[index].name, kept_name))
                                {
                                        alias_table[index].value = kept_value;
                                        return true;
                                }

                                index++;
                        }

                        if (alias_count >= ALIAS_MAX)
                                return false;

                        alias_table[alias_count].name = kept_name;
                        alias_table[alias_count].value = kept_value;
                        alias_count++;
                }
        }

        return true;
}

// dash quotes the whole of name=value, not just the value, and a script that
// reads its own aliases back is reading that.
fn alias_written(writer write, positive index)
{
        string_format(write, "'%s=%s'\n", alias_table[index].name, alias_table[index].value);
}

fn shell_alias(writer write, string_address input)
{
        positive index = 1;
        b32 answer = 0;

        if (shell_argc < 2)
        {
                positive at = 0;

                while (at < alias_count)
                        alias_written(write, at++);

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                string_address mark = string_first_of(word, '=');

                if (mark && mark != word)
                {
                        alias_record(word, mark - word, mark + 1);
                        index++;
                        continue;
                }

                {
                        positive at = 0;
                        bool shown = false;

                        while (at < alias_count)
                        {
                                if (word_is(alias_table[at].name, word))
                                {
                                        alias_written(write, at);
                                        shown = true;
                                        break;
                                }

                                at++;
                        }

                        if (!shown)
                                answer = 1;
                }

                index++;
        }

        shell_answer(answer);
}

fn shell_unalias(writer write, string_address input)
{
        positive index = 1;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive at = 0;

                if (word_is(word, "-a"))
                {
                        alias_count = 0;
                        alias_used = 0;
                        index++;
                        continue;
                }

                while (at < alias_count)
                {
                        if (word_is(alias_table[at].name, word))
                        {
                                while (at + 1 < alias_count)
                                {
                                        alias_table[at] = alias_table[at + 1];
                                        at++;
                                }

                                alias_count--;
                                break;
                        }

                        at++;
                }

                index++;
        }

        shell_answer(0);
}

/*
        eval.

        The words are joined back into a line and the line is run. This has to
        reach the code that runs lines, which sits above this file and is only
        there when a shell was built around it.
*/
#define EVAL_STORAGE 4096
#define EVAL_DEPTH 4

static p8 eval_storage[EVAL_STORAGE];
static positive eval_depth;

fn shell_eval(writer write, string_address input)
{
        positive used = 0;
        positive index = 1;

        if (shell_argc < 2 || !run_line || eval_depth >= EVAL_DEPTH)
                return shell_answer(0);

        while (index < shell_argc)
        {
                positive length = string_length(shell_argv[index]);

                if (used + length + 2 > EVAL_STORAGE)
                        break;

                if (used)
                        eval_storage[used++] = ' ';

                memory_copy(eval_storage + used, shell_argv[index], length);
                used += length;
                index++;
        }

        eval_storage[used] = end;

        /*
                The nested line is lexed and parsed over the same arrays as the
                line that is running, and the walk through those is standing in
                the middle of them. The lexer's tokens are put back afterwards,
                text and all -- a word's text goes back to the address it was
                at, which makes the pointers good again -- and the parser is
                told to claim from above what is in use rather than over it.

                Only where there is a lexer to put back: programs/edit.c takes
                this file with no shell around it.
        */
#ifdef LEX_TOKENS
        {
                lex_token kept_tokens[LEX_TOKENS];
                p8 kept_text[LEX_TEXT];
                positive kept_used = lex_used;
                b32 kept_count = lex_count;

                memory_copy(kept_tokens, lex_tokens, sizeof(kept_tokens));
                memory_copy(kept_text, lex_text, sizeof(kept_text));

                parse_nest_enter();

                eval_depth++;
                run_line(eval_storage);
                eval_depth--;

                parse_nest_leave();

                memory_copy(lex_tokens, kept_tokens, sizeof(kept_tokens));
                memory_copy(lex_text, kept_text, sizeof(kept_text));

                lex_used = kept_used;
                lex_count = kept_count;
        }
#endif

        // After the nested line, not before it: the commands inside set the
        // status themselves, and claiming it early would eat their answer.
        shell_answer(shell_status);
}

/*
        return.

        Without a function to leave, all this can honestly do is say what the
        status is; leaving one is the business of whatever called it.
*/
fn shell_return(writer write, string_address input)
{
        bool good;

        if (shell_argc > 1)
                return shell_answer((b32)shell_signed(shell_argv[1], address_of good) & 0xff);

        shell_answer(shell_status);
}


/*
        The utilities, which are programs that do not need to be.

        Each of these is the same body that ships as its own binary: one
        implementation, reached either way. They are run in a child rather than
        here, and not to isolate them -- because a program that is exec'd gets
        its file scope as the linker left it, every single time, and a builtin
        does not. This process never runs one, so its copy stays untouched and
        every fork starts from it. A hundred greps in a loop each begin the way
        the first one did.
*/
#if defined(TEXT_ARENA_BYTES) && defined(FILE_MAX_DEPTH)

typedef b32 (address_to shell_tool_function)();

typedef struct
{
        string_address name;
        shell_tool_function function;
} shell_tool;

static shell_tool shell_tools[] = {
    {"cat", text_cat},
    {"cmp", text_cmp},
    {"cut", text_cut},
    {"expr", text_expr},
    {"fold", text_fold},
    {"grep", text_grep},
    {"head", text_head},
    {"nl", text_nl},
    {"rev", text_rev},
    {"sed", text_sed},
    {"sort", text_sort},
    {"tail", text_tail},
    {"tee", text_tee},
    {"tr", text_tr},
    {"uniq", text_uniq},
    {"wc", text_wc},

    {"basename", file_basename},
    {"chmod", file_chmod},
    {"chown", file_chown},
    {"cp", file_cp},
    {"df", file_df},
    {"dirname", file_dirname},
    {"du", file_du},
    {"env", file_env},
    {"find", file_find},
    {"hostname", file_hostname},
    {"id", file_id},
    {"ln", file_ln},
    {"ls", file_ls},
    {"mkdir", file_mkdir},
    {"mktemp", file_mktemp},
    {"mv", file_mv},
    {"readlink", file_readlink},
    {"realpath", file_realpath},
    {"rm", file_rm},
    {"rmdir", file_rmdir},
    {"seq", file_seq},
    {"sleep", file_sleep},
    {"stat", file_stat},
    {"touch", file_touch},
    {"uname", file_uname},
    {"yes", file_yes},

    {"edit", system_edit},
    {"init", system_init},
    {"pointer", screen_pointer},
    {"term", screen_term},
    {"text", screen_text},
    {"window", screen_window},
    {"world", system_world},
    {null, null},
};

#define SHELL_TOOLS (sizeof(shell_tools) / sizeof(shell_tools[0]) - 1)

/*
        The last element of a path, so that /bin/grep is grep.

        What a program was called is the first thing on its stack, and for one
        binary answering to forty names it is the only thing that says which.
*/
static string_address shell_tool_name(string_address path)
{
        string_address last = path;

        if (!path)
                return null;

        for (string_address step = path; string_get(step); step++)
                if (string_get(step) == '/' && string_get(step + 1))
                        last = step + 1;

        return last;
}

/*
        Run as the tool the binary was called as, if it was called as one.

        Returns what it answered, or -1 when the name is not a tool's and this
        is an ordinary shell after all. Nothing is forked: this process is the
        invocation, and it is about to end.
*/
b32 shell_tool_as_called()
{
        string_address name = shell_tool_name(program_argument(0));
        positive which;

        if (!name)
                return -1;

        which = string_table_find(name, shell_tools, sizeof(shell_tool), SHELL_TOOLS);

        if (which == SHELL_TOOLS)
                return -1;

        return shell_tools[which].function() & 0xff;
}

// Whether a name is one of the utilities, without running it.
bool shell_tool_here(string_address name)
{
        return string_table_find(name, shell_tools, sizeof(shell_tool),
                                 SHELL_TOOLS) != SHELL_TOOLS;
}

fn shell_tool_list(writer write)
{
        for (positive i = 0; i < SHELL_TOOLS; i++)
                string_format(write, TERM_BOLD " -  %s" TERM_RESET "\n",
                              shell_tools[i].name);
}

static bool shell_tool_run(string_address name)
{
        positive which = string_table_find(name, shell_tools, sizeof(shell_tool),
                                           SHELL_TOOLS);
        bipolar child;
        positive status = 0;

        if (which == SHELL_TOOLS)
                return false;

        // Before the fork, or the child inherits a copy of what is waiting in
        // the buffer and writes it out a second time.
        log_flush();

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                /*
                        Its own signals back.

                        The shell ignores interrupt and quit so control-C
                        cancels the command rather than the shell, and a fork
                        inherits that -- which the spawn path undoes before it
                        execs and this one has to undo for itself, because a
                        builtin never execs. Without it a grep over a large
                        tree could not be stopped.
                */
                shell_default(SIGNAL_INTERRUPT);
                shell_default(SIGNAL_QUIT);

                program_arguments_use(shell_argv, (b32)shell_argc);
                exit(shell_tools[which].function() & 0xff);
        }

        if (child < 0)
        {
                shell_answer(1);
                return true;
        }

        system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        shell_answer((b32)((status >> 8) & 0xff));

        return true;
}

#else

// Without the two layers beside this file there are no utilities to reach --
// programs/edit.c takes this one on its own for a handful of its commands.
b32 shell_tool_as_called() { return -1; }
bool shell_tool_here(string_address name) { return false; }
fn shell_tool_list(writer write) {}
static bool shell_tool_run(string_address name) { return false; }

#endif


/*
        The EXIT trap, run.

        Traps were recorded and nothing ever ran one, so "trap cleanup EXIT"
        was a promise the shell did not keep -- a script that removed its
        temporary files on the way out left them all behind. This is the one
        that can be run without a signal handler, because leaving is something
        the shell does to itself and it knows where.

        The action runs with the status the shell is leaving with, and cannot
        change it: POSIX says the exit status is the one that was already
        decided unless the trap itself calls exit.
*/
fn shell_trap_exit()
{
        string_address action = trap_action(0);
        b32 leaving = shell_status;

        if (!action || !run_line || !string_get(action))
                return;

        // Taken away first, so a trap that leaves again does not run twice.
        trap_forget(0);

        parse_nest_enter();
        run_line(action);
        parse_nest_leave();

        shell_status = leaving;
}

/*
        . and source: a file's lines, run by this shell and not another.

        The difference from running the file is the whole point -- what it sets
        has to still be set afterwards, which is how a profile works and why a
        subshell will not do. So it is eval with a file for its argument: the
        lexer's tokens are put aside, the parser is told to claim above what is
        already in use, and the lines go through run_line one at a time.

        A line is only run once it is whole. A while loop spread over six lines
        is one command, and the parser says so by staying incomplete, which is
        the same thing the reader in programs/shell.c listens to.
*/
#define SOURCE_MAX 65536

static p8 source_text[SOURCE_MAX];
static positive source_depth;

fn shell_dot(writer write, string_address input)
{
        p8 found[768];
        string_address path;
        bipolar handle;
        positive filled = 0;
        positive at = 0;

        if (shell_argc < 2 || !run_line)
                return shell_answer(shell_argc < 2 ? 2 : 0);

        if (source_depth >= EVAL_DEPTH)
        {
                string_format(shell_diagnostic, "%s: too deep\n", shell_argv[0]);
                return shell_answer(1);
        }

        path = shell_argv[1];

        // A name with no slash is looked for on the path, which is what POSIX
        // says and what makes ". functions" find /etc/functions.
        if (!string_first_of(path, '/') && shell_find_in_path(path, found, sizeof(found)))
                path = found;

        handle = system_call_3(syscall(openat), AT_FDCWD, (positive)path, FILE_READ);

        if (handle < 0)
        {
                string_format(shell_diagnostic, "%s: %s: cannot open\n",
                              shell_argv[0], shell_argv[1]);
                shell_answer(1);

                /*
                        A special builtin that fails ends the script.

                        POSIX says so of the whole set -- ., eval, exec, exit,
                        export, readonly, set, shift, times, trap, unset -- and
                        it matters most here: a script that sources a file it
                        cannot find should stop, not carry on without whatever
                        was in it. Only when nobody is watching; at a terminal
                        the shell stays, or a typo would close the session.
                */
                if (!shell_is_interactive)
                {
                        log_flush();
                        exit(1);
                }

                return;
        }

        while (filled < SOURCE_MAX - 1)
        {
                bipolar got = system_call_3(syscall(read), (positive)handle,
                                            (positive)(source_text + filled),
                                            SOURCE_MAX - 1 - filled);

                if (got <= 0)
                        break;

                filled += (positive)got;
        }

        system_call_1(syscall(close), (positive)handle);
        source_text[filled] = end;

        {
                lex_token kept_tokens[LEX_TOKENS];
                p8 kept_text[LEX_TEXT];
                positive kept_used = lex_used;
                b32 kept_count = lex_count;

                memory_copy(kept_tokens, lex_tokens, sizeof(kept_tokens));
                memory_copy(kept_text, lex_text, sizeof(kept_text));

                parse_nest_enter();
                source_depth++;

                while (at < filled)
                {
                        positive stop = at;

                        while (stop < filled && source_text[stop] != '\n')
                                stop++;

                        source_text[stop] = end;
                        run_line(source_text + at);
                        at = stop + 1;
                }

                source_depth--;
                parse_nest_leave();

                memory_copy(lex_tokens, kept_tokens, sizeof(kept_tokens));
                memory_copy(lex_text, kept_text, sizeof(kept_text));

                lex_used = kept_used;
                lex_count = kept_count;
        }

        // The status of the last line it ran, which is already there.
        shell_answer(shell_status);
}





/*
        wait: until the children are gone, or until one of them is.

        No job control here, so there are no job numbers to name -- a bare wait
        collects everything and a wait with a number collects that process.
*/
fn shell_wait(writer write, string_address input)
{
        positive status = 0;

        if (shell_argc > 1)
        {
                bipolar want = (bipolar)shell_number(shell_argv[1]);
                bipolar got = system_call_4(syscall(wait4), want,
                                            (positive)address_of status, 0, 0);

                if (got < 0)
                        return shell_answer(127);

                return shell_answer((b32)((status >> 8) & 0xff));
        }

        while (system_call_4(syscall(wait4), -1, (positive)address_of status, 0, 0) >= 0)
                ;

        shell_answer(0);
}

fn shell_help(writer write, string_address input);
fn shell_which(writer write, string_address input);
fn shell_type(writer write, string_address input);
fn shell_command_builtin(writer write, string_address input);

typedef fn(address_to shell_command_function)(writer write, string_address input);

typedef struct
{
        string_address name;
        shell_command_function function;
} shell_command;

shell_command shell_commands[] = {
    {":", shell_true},
    {".", shell_dot},
    {"[", shell_test},
    {"alias", shell_alias},
    {"cd", shell_cd},
    {"clear", shell_clear},
    {"command", shell_command_builtin},
    {"echo", shell_echo},
    {"eval", shell_eval},
    {"exec", shell_exec},
    {"exit", shell_exit},
    {"false", shell_false},
    {"getopts", shell_getopts},
    {"mount", shell_mount},
    {"poweroff", shell_poweroff},
    {"printf", shell_printf},
    {"pwd", shell_pwd},
    {"read", shell_read},
    {"readonly", shell_readonly},
    {"reboot", shell_reboot},
    {"return", shell_return},
    {"set", shell_set},
    {"shift", shell_shift},
    {"source", shell_dot},
    {"test", shell_test},
    {"times", shell_times},
    {"trap", shell_trap},
    {"type", shell_type},
    {"true", shell_true},
    {"umask", shell_umask},
    {"unalias", shell_unalias},
    {"unset", shell_unset},
    {"wait", shell_wait},
    {"which", shell_which},
    {"help", shell_help},
    {"export", shell_export},
    {null, null},
};

/*
        Where a bare command name is actually found.

        Shared with the shell itself, which needs the same answer before it can
        run anything typed without a slash -- and had no way to ask, so every
        program had to be named by its full path.
*/
b32 shell_find_in_path(string_address name, p8 address_to into, positive room)
{
        // On a copy: PATH points into env_storage, and cutting it apart there
        // would leave the environment holding only its first directory.
        p8 search[512];
        string_address value = env_get("PATH");
        string_address segment;
        positive name_length;

        if (name == null)
                return false;

        if (string_first_of(name, '/'))
        {
                if (system_call_4(syscall(faccessat), AT_FDCWD, (positive)name,
                                  ACCESS_EXECUTE, 0))
                        return false;

                string_copy_max(into, name, room - 1);
                into[room - 1] = end;
                return true;
        }

        if (value == null)
                value = "/bin:/usr/bin:/";

        string_copy_max(search, value, sizeof(search) - 1);
        search[sizeof(search) - 1] = end;

        segment = search;
        name_length = string_length(name);

        while (segment)
        {
                string_address next = string_cut(segment, ':');
                positive length = string_length(segment);

                if (length && length + name_length + 2 <= room)
                {
                        string_copy(into, segment);

                        if (into[length - 1] != '/')
                                into[length++] = '/';

                        string_copy(into + length, name);

                        if (!system_call_4(syscall(faccessat), AT_FDCWD,
                                           (positive)into, ACCESS_EXECUTE, 0))
                                return true;
                }

                segment = next;
        }

        return false;
}

/*
        type: what a name would run.

        In the order the shell would actually try them, which is the only
        useful answer -- a grep on the path is not the grep that runs.
*/
fn shell_type(writer write, string_address input)
{
        b32 index = 1;
        b32 bad = 0;

        if (shell_argc < 2)
                return shell_answer(0);

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                shell_command address_to command = shell_commands;
                p8 found[768];
                bool said = false;

                while (command->name)
                {
                        if (!string_compare(command->name, name))
                        {
                                string_format(write, "%s is a shell builtin\n", name);
                                said = true;
                                break;
                        }

                        command++;
                }

                if (said)
                        continue;

                if (shell_tool_here(name))
                {
                        string_format(write, "%s is a shell builtin\n", name);
                        continue;
                }

                if (exec_function_here && exec_function_here(name))
                {
                        string_format(write, "%s is a shell function\n", name);
                        continue;
                }

                if (shell_find_in_path(name, found, sizeof(found)))
                {
                        string_format(write, "%s is %s\n", name, found);
                        continue;
                }

                // On standard output, as POSIX says of type and as the
                // reference shell does: it is an answer, not a complaint.
                string_format(write, "%s: not found\n", name);
                bad = 127;
        }

        shell_answer(bad);
}

/*
        command: run a name as the shell would, and never as a function.

        command -v prints what would run rather than running it, which is what
        a script uses to ask whether something is there at all.
*/
fn shell_command_builtin(writer write, string_address input)
{
        b32 index = 1;
        bool only_say = false;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                if (string_is(letter, '-') && !string_get(letter + 1))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        // -p is the standard path, which is the only path here.
                        if (string_get(letter) == 'v' || string_get(letter) == 'V')
                                only_say = true;
                        else if (string_get(letter) != 'p')
                                break;

                        letter++;
                }

                index++;
        }

        if (index >= shell_argc)
                return shell_answer(0);

        if (only_say)
        {
                string_address name = shell_argv[index];
                shell_command address_to command = shell_commands;
                p8 found[768];

                while (command->name)
                {
                        if (!string_compare(command->name, name))
                        {
                                string_format(write, "%s\n", name);
                                return shell_answer(0);
                        }

                        command++;
                }

                if (shell_tool_here(name))
                {
                        string_format(write, "%s\n", name);
                        return shell_answer(0);
                }

                if (shell_find_in_path(name, found, sizeof(found)))
                {
                        string_format(write, "%s\n", found);
                        return shell_answer(0);
                }

                return shell_answer(127);
        }

        // Running it is the executor's business, and it is told to skip the
        // function table by the words it is handed.
        {
                b32 at = index;

                for (b32 to = 0; at < shell_argc; at++, to++)
                        shell_argv[to] = shell_argv[at];

                shell_argc -= index;
                shell_argv[shell_argc] = null;
        }

        if (shell_builtin(shell_arguments()))
                return;

        shell_execute_command();
}

fn shell_which(writer write, string_address input)
{
        if (input == null)
                return shell_diagnostic(str("which: missing operand\n"));

        shell_command address_to command = shell_commands;

        while (command->name)
        {
                if (!string_compare(command->name, input))
                        return string_format(write, "%s: shell builtin\n", input);

                command++;
        }

        // Before the path, because that is the order the shell runs them in:
        // a grep on the path is not the grep that would run.
        if (shell_tool_here(input))
                return string_format(write, "%s: shell builtin\n", input);

        p8 found[768];

        if (shell_find_in_path(input, found, sizeof(found)))
                return string_format(write, "%s\n", found);

        // On standard output, the same as type: both answer the same
        // question and used to answer it down different descriptors.
        string_format(write, "%s: not found\n", input);
        shell_answer(127);
}

fn shell_help(writer write, string_address input)
{
        string_format(write, "Moonwater shell, WIP, " TERM_RED TERM_BOLD "expect crashes! \n\n" TERM_RESET "Available built-in commands:\n");

        shell_command address_to command = shell_commands;

        while (command->name)
        {
                string_format(write, TERM_BOLD " -  %s" TERM_RESET "\n", command->name);
                command++;
        }

        shell_tool_list(write);

        write("\n", 1);
}
