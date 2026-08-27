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
        statx, not fstat: the kernel's struct stat has a different shape on
        x86_64, arm64 and riscv64, while statx has one layout everywhere. Only
        the head of it is spelled out; the rest is timestamps and device
        numbers nothing here asks for, and the kernel writes all 256 bytes.
*/
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

        write("\n", 1);
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

/*
        cp SOURCE DEST, and cp -r for a directory. The destination is the copy
        itself, not a directory to copy into: "cp -r a b" leaves the contents
        of a in b, and merges if b was already a directory.
*/
fn shell_cp(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "r");

        if (input == null)
                return write(str("cp: missing operand\n"));

        string_address destination = string_cut(input, ' ');

        // string_format writes a %s through the writer without looking at it,
        // so a missing destination used to be a null dereference here.
        if (destination == null)
                return write(str("cp: missing destination\n"));

        if (shell_is_directory(AT_FDCWD, input) && !(flags & SHELL_FLAG('r')))
                return string_format(write, "cp: Omitting directory: %s\n", input);

        // "cp -r a a/b" copies the copy, and on a rootfs made of RAM that ends
        // as an out of memory, not as a full disk.
        positive length = string_length(input);
        positive shared = 0;

        while (shared < length && string_get(input + shared) == string_get(destination + shared))
                shared++;

        if (shared == length && string_is(destination + length, '/'))
                return string_format(write, "cp: Cannot copy '%s' into itself\n", input);

        if (shell_copy_entry(AT_FDCWD, input, AT_FDCWD, destination, SHELL_MAX_DEPTH))
                return;

        string_format(write, "cp: Cannot copy: %s\n", input);
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
        if (input == null)
                return write(str("exec: missing operand\n"));

        // The old argv was a single word with no terminator, and execve was
        // called without an environment at all, so the new program read
        // whatever followed the array on the stack as its argv and envp.
        string_address argv[16];
        positive count = 0;
        string_address step = input;

        while (step && count < 15)
        {
                string_address next = string_cut(step, ' ');

                argv[count++] = step;
                step = next;
        }

        argv[count] = null;

        log_flush();

        bipolar result = system_call_3(syscall(execve), (positive)input,
                                       (positive)argv, (positive)shell_envp);

        string_format(write, "exec: Cannot run '%s': %b\n", input, result);
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

// - Blue: Directories
// - Cyan: Symbolic links
// - Default: Regular files
// - Yellow: Special files (FIFO, sockets, devices, etc.)
fn shell_ls(writer write, string_address input)
{
        const p32 max_line_entries = 8;

        positive flags = shell_flags(address_of input, "la");
        bool detailed = (flags & SHELL_FLAG('l')) != 0;
        bool hidden_too = (flags & SHELL_FLAG('a')) != 0;

        if (input == null)
                input = ".";

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ | O_DIRECTORY);

        if (file_descriptor < 0)
        {
                // A path that is there but is not a directory is not a missing
                // path. ls names it, the way it does for any other file.
                if (file_descriptor == -ERROR_NOT_DIRECTORY)
                {
                        if (detailed)
                                return shell_ls_long(write, AT_FDCWD, input);

                        return string_format(write, "%s\n", input);
                }

                if (file_descriptor == -ERROR_NO_ENTRY)
                        return string_format(write, "ls: Cannot access '%s': No such file or directory\n", input);

                return string_format(write, "ls: Cannot access '%s': %b\n", input, file_descriptor);
        }

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

                        if (entry->d_name[0] == '.' && !hidden_too)
                        {
                                step += entry->d_reclen;
                                continue;
                        }

                        if (detailed)
                        {
                                shell_ls_long(write, file_descriptor, entry->d_name);
                                step += entry->d_reclen;
                                continue;
                        }

                        shell_style(write, shell_kind_of_type(entry->d_type));

                        string_format(write, "%s ", entry->d_name);

                        if (shell_styles)
                                write(str(TERM_RESET));

                        entries_count++;

                        if (entries_count % max_line_entries == 0)
                                write("\n", 1);

                        step += entry->d_reclen;
                }
        }

        if (!detailed && entries_count % max_line_entries != 0)
                write("\n", 1);

        system_call_1(syscall(close), file_descriptor);
}

fn shell_head(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "n");
        positive lines = 10;

        if (flags & SHELL_FLAG('n'))
                lines = shell_number(shell_word(address_of input));

        if (input == null)
                return write(str("head: missing operand\n"));

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ);

        if (file_descriptor < 0)
                return string_format(write, "head: Cannot open file: %s\n", input);

        p8 buffer[1024];
        positive seen = 0;

        while (seen < lines)
        {
                bipolar bytes_read = system_call_3(syscall(read), file_descriptor,
                                                   (positive)buffer, sizeof(buffer));

                if (bytes_read <= 0)
                        break;

                positive index = 0;

                while (index < (positive)bytes_read && seen < lines)
                {
                        if (buffer[index] == '\n')
                                seen++;

                        index++;
                }

                write(buffer, index);
        }

        system_call_1(syscall(close), file_descriptor);
}

/*
        The last lines of a file, without holding the file. Seek to the end,
        take a window off the tail and walk backwards through it. A file longer
        than the window keeps its first partial line out of the output.
*/
fn shell_tail(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "n");
        positive lines = 10;

        if (flags & SHELL_FLAG('n'))
                lines = shell_number(shell_word(address_of input));

        if (input == null)
                return write(str("tail: missing operand\n"));

        if (lines == 0)
                return;

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ);

        if (file_descriptor < 0)
                return string_format(write, "tail: Cannot open file: %s\n", input);

        p8 buffer[8192];

        bipolar size = system_call_3(syscall(lseek), file_descriptor, 0, FILE_SEEK_END);

        if (size < 0)
        {
                system_call_1(syscall(close), file_descriptor);
                return string_format(write, "tail: Cannot seek: %s\n", input);
        }

        positive window = (positive)size;

        if (window > sizeof(buffer))
                window = sizeof(buffer);

        system_call_3(syscall(lseek), file_descriptor, (positive)size - window, FILE_SEEK_SET);

        positive filled = 0;

        while (filled < window)
        {
                bipolar bytes_read = system_call_3(syscall(read), file_descriptor,
                                                   (positive)(buffer + filled), window - filled);

                if (bytes_read <= 0)
                        break;

                filled += bytes_read;
        }

        system_call_1(syscall(close), file_descriptor);

        positive index = filled;
        positive start = 0;
        positive seen = 0;

        // The newline that ends the last line does not begin one.
        if (index && buffer[index - 1] == '\n')
                index--;

        while (index > 0)
        {
                index--;

                if (buffer[index] != '\n')
                        continue;

                seen++;

                if (seen == lines)
                {
                        start = index + 1;
                        break;
                }
        }

        if (start == 0 && window < (positive)size)
        {
                while (start < filled && buffer[start] != '\n')
                        start++;

                if (start < filled)
                        start++;
        }

        // A writer reads a length of zero as "measure it yourself", so an
        // empty file would have it run off down an uninitialised buffer.
        if (filled > start)
                write(buffer + start, filled - start);
}

fn shell_wc(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "lwc");

        if (input == null)
                return write(str("wc: missing operand\n"));

        bipolar file_descriptor = system_call_3(syscall(openat), AT_FDCWD, (positive)input, FILE_READ);

        if (file_descriptor < 0)
                return string_format(write, "wc: Cannot open file: %s\n", input);

        positive lines = 0;
        positive words = 0;
        positive bytes = 0;
        bool inside_word = false;

        p8 buffer[1024];

        while (1)
        {
                bipolar bytes_read = system_call_3(syscall(read), file_descriptor,
                                                   (positive)buffer, sizeof(buffer));

                if (bytes_read <= 0)
                        break;

                positive index = 0;

                while (index < (positive)bytes_read)
                {
                        p8 value = buffer[index++];

                        bytes++;

                        if (value == '\n')
                                lines++;

                        bool blank = value == ' ' || value == '\t' || value == '\n' ||
                                     value == '\r' || value == '\v' || value == '\f';

                        if (blank)
                        {
                                inside_word = false;
                                continue;
                        }

                        if (!inside_word)
                        {
                                inside_word = true;
                                words++;
                        }
                }
        }

        system_call_1(syscall(close), file_descriptor);

        bool all = !(flags & (SHELL_FLAG('l') | SHELL_FLAG('w') | SHELL_FLAG('c')));

        if (all || (flags & SHELL_FLAG('l')))
                shell_number_padded(write, lines, 7);

        if (all || (flags & SHELL_FLAG('w')))
                shell_number_padded(write, words, 7);

        if (all || (flags & SHELL_FLAG('c')))
                shell_number_padded(write, bytes, 7);

        string_format(write, " %s\n", input);
}

fn shell_mkdir(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "p");

        if (input == null)
                return write(str("mkdir: missing operand\n"));

        if (!(flags & SHELL_FLAG('p')))
        {
                if (!system_call_3(syscall(mkdirat), AT_FDCWD, (positive)input, 0777))
                        return;

                return string_format(write, "mkdir: Cannot create directory: %s\n", input);
        }

        string_address step = input;
        bipolar result = 0;

        while (1)
        {
                while (string_get(step) && string_not(step, '/'))
                        step++;

                bool more = string_is(step, '/');
                p8 saved = string_get(step);

                string_set(step, end);

                // A leading slash leaves the first component empty, and a
                // component that is already there is what -p is for.
                if (string_get(input))
                {
                        result = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)input, 0777);

                        if (result == -ERROR_EXISTS)
                                result = 0;
                }

                string_set(step, saved);

                if (result || !more)
                        break;

                step++;
        }

        if (result)
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

fn shell_rm(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "rf");

        if (input == null)
                return write(str("rm: missing operand\n"));

        if (string_is(input, '/') && string_is(input + 1, end))
                return write(str("rm: refusing to remove /\n"));

        if (!(flags & SHELL_FLAG('r')))
        {
                bipolar result = system_call_3(syscall(unlinkat), AT_FDCWD, (positive)input, 0);

                if (!result)
                        return;

                if (result == -ERROR_IS_DIRECTORY || result == -ERROR_NOT_PERMITTED)
                        return string_format(write, "rm: Is a directory: %s\n", input);

                if (result == -ERROR_NO_ENTRY)
                        return string_format(write, "rm: Cannot remove '%s': No such file or directory\n", input);

                return string_format(write, "rm: Cannot remove '%s': %b\n", input, result);
        }

        if (shell_remove_entry(AT_FDCWD, input, SHELL_MAX_DEPTH))
                return;

        string_format(write, "rm: Cannot remove: %s\n", input);
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

        log_flush();

        exit(exit_code);
}

fn shell_touch(writer write, string_address input)
{
        if (input == null)
                return write(str("touch: missing operand\n"));

        // No truncation. FILE_WRITE already carries O_TRUNC, so this emptied
        // any file that was already there, which is the one thing touch must
        // never do.
        bipolar file_descriptor = system_call_4(syscall(openat), AT_FDCWD, (positive)input, FILE_CREATE | FILE_READ, 0666);

        if (file_descriptor < 0)
                return string_format(write, "touch: Cannot create file: %s\n", input);

        system_call_1(syscall(close), file_descriptor);
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

fn shell_uname(writer write, string_address input)
{
        positive flags = shell_flags(address_of input, "snrvma");

        p8 facts[UTSNAME_FIELD * UTSNAME_FIELDS];

        memory_fill(facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)facts))
                return write(str("uname: not available\n"));

        bool all = (flags & SHELL_FLAG('a')) != 0;
        positive shown = 0;

        if (all || !flags || (flags & SHELL_FLAG('s')))
                shell_uname_field(write, address_of shown, facts);

        if (all || (flags & SHELL_FLAG('n')))
                shell_uname_field(write, address_of shown, facts + UTSNAME_FIELD);

        if (all || (flags & SHELL_FLAG('r')))
                shell_uname_field(write, address_of shown, facts + UTSNAME_FIELD * 2);

        if (all || (flags & SHELL_FLAG('v')))
                shell_uname_field(write, address_of shown, facts + UTSNAME_FIELD * 3);

        if (all || (flags & SHELL_FLAG('m')))
                shell_uname_field(write, address_of shown, facts + UTSNAME_FIELD * 4);

        write("\n", 1);
}

fn shell_sleep(writer write, string_address input)
{
        if (input == null)
                return write(str("sleep: missing operand\n"));

        timespec duration = {0, 0};

        duration.tv_sec = shell_number(input);

        string_address fraction = string_first_of(input, '.');

        if (fraction)
        {
                positive scale = 100000000;

                fraction++;

                while (string_get(fraction) >= '0' && string_get(fraction) <= '9' && scale)
                {
                        duration.tv_nsec += (string_get(fraction++) - '0') * scale;
                        scale /= 10;
                }
        }

        // Anything queued has to reach the terminal before the shell goes
        // quiet for the duration.
        log_flush();

        sleep(address_of duration);
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

fn shell_help(writer write, string_address input);
fn shell_which(writer write, string_address input);

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
    {"head", shell_head},
    {"ls", shell_ls},
    {"mkdir", shell_mkdir},
    {"mv", shell_mv},
    {"mount", shell_mount},
    {"poweroff", shell_poweroff},
    {"pwd", shell_pwd},
    {"reboot", shell_reboot},
    {"rm", shell_rm},
    {"sleep", shell_sleep},
    {"tail", shell_tail},
    {"touch", shell_touch},
    {"uname", shell_uname},
    {"wc", shell_wc},
    {"which", shell_which},
    {"help", shell_help},
    {"env", shell_env},
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

fn shell_which(writer write, string_address input)
{
        if (input == null)
                return write(str("which: missing operand\n"));

        shell_command address_to command = shell_commands;

        while (command->name)
        {
                if (!string_compare(command->name, input))
                        return string_format(write, "%s: shell builtin\n", input);

                command++;
        }

        p8 found[768];

        if (shell_find_in_path(input, found, sizeof(found)))
                return string_format(write, "%s\n", found);

        string_format(write, "which: %s: not found\n", input);
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

        write("\n", 1);
}
