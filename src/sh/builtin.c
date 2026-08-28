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

// eval runs a line, and what runs lines sits below this file.
fn run_line(string_address line);
bool exec_function_here(string_address name);
bool shell_builtin(string_address arguments);
string_address shell_arguments();
fn shell_execute_command();
fn parse_nest_enter();
fn parse_nest_leave();

#define SHELL_DIRECTORY_MAX 4096

extern p8 shell_directory[SHELL_DIRECTORY_MAX];

b32 shell_find_in_path(string_address name, p8 address_to into, positive room);
bipolar shell_signed(string_address input, bool address_to good);
bool test_facts(string_address path, file_facts address_to out, bool follow);
bool word_is(string_address word, string_address text);
p64 test_device(file_facts address_to facts);
fn hash_forget();
fn shell_here(p8 address_to into, positive room);
positive printf_render(p8 address_to into, positive value, positive base, bool upper);

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
        string_address defaults[] = {"PATH=/bin:/usr/bin:/", "SHELL=/bin/sh",
                                     "OPTIND=1", null};

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

        // Where the shell is, before anything asks. cd keeps it from here on;
        // without a first answer, PWD is empty until the first cd and a script
        // that names a file relative to it names nothing.
        shell_here(shell_directory, sizeof(shell_directory));
        env_set("PWD", shell_directory);
}

/*
        The library's string routines take a mutable pointer and do not write
        through it. Rather than loosen every one of those declarations, the
        promise these two make to their callers is kept here and the cast is
        made where it is safe to see that nothing is written.
*/
#define env_reading(text) ((string_address)(text))

string_address env_get(const_string name)
{
        if (name == null)
                return null;

        positive name_len = string_length(env_reading(name));
        positive idx = 0;

        while (shell_envp[idx])
        {
                string_address entry = shell_envp[idx];
                string_address eq = string_first_of(entry, '=');

                if (eq && (positive)(eq - entry) == name_len &&
                    !memory_compare(entry, name, name_len))
                        return eq + 1;

                idx++;
        }

        return null;
}

/*
        The block put back end to end, dropping what nothing points at.

        Entries are laid down one after another and a value that outgrows its
        place is written after them all, so a name that keeps changing length
        eats the block a copy at a time. Nothing ever gave any of it back,
        which is the difference between a function using local four hundred
        times and one that can use it for as long as the script runs.

        Only what lives in the block is moved. The positional parameters are
        hung on the list by pointer from storage of their own and are left
        exactly where they are.
*/
static p8 env_spare[ENV_STORAGE_SIZE];
static p8 env_staging[ENV_STORAGE_SIZE];

static fn env_compact()
{
        positive used = 0;
        positive index = 0;

        while (shell_envp[index])
        {
                string_address entry = shell_envp[index];

                if (entry >= (string_address)env_storage &&
                    entry < (string_address)env_storage + ENV_STORAGE_SIZE)
                {
                        positive length = string_length(entry) + 1;

                        memory_copy(env_spare + used, entry, length);
                        shell_envp[index] = env_storage + used;
                        used += length;
                }

                index++;
        }

        memory_copy(env_storage, env_spare, used);
        env_used = used;
}

bool env_set(const_string name, const_string value)
{
        if (!name || !value)
                return false;

        if (env_readonly(name))
                return false;

        // Every path remembered was an answer about the old PATH.
        if (!string_compare(env_reading(name), "PATH"))
                hash_forget();

        positive name_len = string_length(env_reading(name));
        positive value_len = string_length(env_reading(value));
        positive needed = name_len + 1 + value_len + 1;

        positive idx = 0;
        bool replacing = false;

        // Either of them can be inside the block that is about to move, so
        // both are stood aside first, somewhere the move will not reach.
        if (env_used + needed > ENV_STORAGE_SIZE && needed <= ENV_STORAGE_SIZE)
        {
                memory_copy(env_staging, env_reading(name), name_len + 1);
                memory_copy(env_staging + name_len + 1, env_reading(value), value_len + 1);

                name = env_staging;
                value = env_staging + name_len + 1;

                env_compact();
        }

        while (shell_envp[idx])
        {
                string_address entry = shell_envp[idx];
                string_address eq = string_first_of(entry, '=');

                if (eq && (positive)(eq - entry) == name_len &&
                    !memory_compare(entry, name, name_len))
                {
                        // Only where it fits. The entries sit end to end in one
                        // block, so a longer value written in place runs over
                        // the name of whatever comes next.
                        if (value_len <= string_length(eq + 1))
                        {
                                string_copy(eq + 1, env_reading(value));
                                return true;
                        }

                        replacing = true;
                        break;
                }
                idx++;
        }

        if ((!replacing && idx >= ENV_MAX_ENTRIES) ||
            env_used + needed > ENV_STORAGE_SIZE)
                return false;

        string_address dest = env_storage + env_used;
        string_copy(dest, env_reading(name));
        string_copy(dest + name_len, "=");
        string_copy(dest + name_len + 1, env_reading(value));

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

#define SHELL_FLAG(letter) ((positive)1 << ((letter) - 'a'))

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
        p8 body[24];
        positive length = printf_render(body, value, 10, false);

        while (width > length)
        {
                write(" ", 1);
                width--;
        }

        write(body, length);
}

/*
        A value written so it can be read back.

        Single quoted, and a single quote inside it closed, escaped and opened
        again: 'it'\''s'. Anything printed by export -p or readonly -p is meant
        to be a line the shell could be fed.
*/
fn shell_quoted(writer write, string_address value)
{
        write("'", 1);

        while (value && string_get(value))
        {
                string_address stop = string_first_of_or_end(value, '\'');

                if (stop > value)
                        write(value, (positive)(stop - value));

                value = stop;

                if (string_get(value))
                {
                        write("'\\''", 4);
                        value++;
                }
        }

        write("'", 1);
}

fn shell_named_written(writer write, string_address word, string_address entry)
{
        string_address mark = string_first_of(entry, '=');

        string_format(write, "%s ", word);

        if (!mark)
                return string_format(write, "%s\n", entry);

        write(entry, (positive)(mark - entry));
        write("=", 1);
        shell_quoted(write, mark + 1);
        write("\n", 1);
}

fn shell_export(writer write, string_address input)
{
        positive index = 1;
        bool listed = shell_argc < 2;

        while (index < shell_argc && word_is(shell_argv[index], "-p"))
        {
                listed = true;
                index++;
        }

        if (listed && index >= shell_argc)
        {
                positive at = 0;

                while (shell_envp[at])
                        shell_named_written(write, "export", shell_envp[at++]);

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index++];
                string_address mark = string_first_of(word, '=');

                // A name on its own is already exported here: every variable
                // this shell has is in the block the environment is made of.
                if (!mark || mark == word)
                        continue;

                address_to mark = end;
                env_set(word, mark + 1);
                address_to mark = '=';
        }

        shell_answer(0);
}

/*
        The directory the shell says it is in, which is not always the one the
        kernel would name.

        A symlink walked into keeps its own name here: "cd link" then "cd .."
        goes back to where the link was, not to the parent of what it pointed
        at. That is the -L rule, it is the default, and it needs the path
        remembered rather than asked for -- getcwd has already forgotten it.
*/
p8 shell_directory[SHELL_DIRECTORY_MAX];
static p8 shell_directory_was[SHELL_DIRECTORY_MAX];

fn shell_here(p8 address_to into, positive room)
{
        into[0] = end;
        system_call_2(syscall(getcwd), (positive)into, room);
}

/*
        The dots taken out, without asking the kernel about any of it.

        Lexical on purpose: ".." after a symlink has to come off the name the
        shell is holding, and a walk through the filesystem would answer about
        the target instead.
*/
fn shell_path_tidy(p8 address_to path)
{
        positive read = 0;
        positive write_at = 0;
        bool rooted = path[0] == '/';

        if (rooted)
                path[write_at++] = '/';

        while (path[read])
        {
                positive begin;
                positive length;

                while (path[read] == '/')
                        read++;

                begin = read;

                while (path[read] && path[read] != '/')
                        read++;

                length = read - begin;

                if (!length)
                        continue;

                if (length == 1 && path[begin] == '.')
                        continue;

                if (length == 2 && path[begin] == '.' && path[begin + 1] == '.')
                {
                        positive back = write_at;

                        while (back > (rooted ? 1 : 0) && path[back - 1] != '/')
                                back--;

                        // A leading ".." in a relative name has nothing above
                        // it to take away, so it stays.
                        if (back > (rooted ? 1 : 0) || rooted)
                        {
                                write_at = back;

                                if (write_at > 1 && path[write_at - 1] == '/')
                                        write_at--;

                                continue;
                        }
                }

                if (write_at && path[write_at - 1] != '/')
                        path[write_at++] = '/';

                memory_copy(path + write_at, path + begin, length);
                write_at += length;
        }

        if (!write_at)
                path[write_at++] = rooted ? '/' : '.';

        path[write_at] = end;
}

// PWD is only worth believing while it still names the directory the shell is
// actually in; a chdir anywhere else leaves it a lie.
bool shell_directory_holds()
{
        file_facts named;
        file_facts here;

        if (shell_directory[0] != '/')
                return false;

        if (!test_facts(shell_directory, address_of named, true) ||
            !test_facts(".", address_of here, true))
                return false;

        return named.inode == here.inode &&
               test_device(address_of named) == test_device(address_of here);
}

fn shell_directory_moved(string_address logical)
{
        string_copy_max(shell_directory_was, shell_directory,
                        sizeof(shell_directory_was) - 1);
        shell_directory_was[sizeof(shell_directory_was) - 1] = end;

        string_copy_max(shell_directory, logical, sizeof(shell_directory) - 1);
        shell_directory[sizeof(shell_directory) - 1] = end;

        env_set("OLDPWD", shell_directory_was);
        env_set("PWD", shell_directory);
}

static p8 shell_cd_target[4096];

bool shell_cd_try(string_address candidate, bool physical)
{
        p8 wanted[4096];

        string_copy_max(wanted, candidate, sizeof(wanted) - 1);
        wanted[sizeof(wanted) - 1] = end;

        if (!physical)
                shell_path_tidy(wanted);

        if (system_call_1(syscall(chdir), (positive)wanted))
                return false;

        if (physical)
                shell_here(wanted, sizeof(wanted));

        shell_directory_moved(wanted);

        return true;
}

/*
        Where the name asked for actually is.

        An absolute name is itself; a name beginning with a dot is under the
        directory the shell is in and nothing else; anything else is looked for
        along CDPATH first, and a hit there is said out loud because the script
        did not name the place it landed.
*/
bool shell_cd_walk(bool physical, bool address_to say)
{
        p8 candidate[4096];

        if (shell_cd_target[0] == '/')
                return shell_cd_try(shell_cd_target, physical);

        if (!(shell_cd_target[0] == '.' &&
              (shell_cd_target[1] == end || shell_cd_target[1] == '/' ||
               (shell_cd_target[1] == '.' &&
                (shell_cd_target[2] == end || shell_cd_target[2] == '/')))))
        {
                p8 search[1024];
                string_address value = env_get("CDPATH");

                if (value && string_get(value))
                {
                        string_address segment;

                        string_copy_max(search, value, sizeof(search) - 1);
                        search[sizeof(search) - 1] = end;
                        segment = search;

                        while (segment)
                        {
                                string_address next = string_cut(segment, ':');

                                file_join(candidate, sizeof(candidate),
                                          string_get(segment) ? segment
                                                              : shell_directory,
                                          shell_cd_target);

                                if (shell_cd_try(candidate, physical))
                                {
                                        address_to say = true;
                                        return true;
                                }

                                segment = next;
                        }
                }
        }

        file_join(candidate, sizeof(candidate), shell_directory,
                  shell_cd_target);

        return shell_cd_try(candidate, physical);
}

fn shell_cd(writer write, string_address input)
{
        positive index = 1;
        bool physical = false;
        string_address name = null;
        bool say = false;

        if (!shell_directory_holds())
        {
                shell_here(shell_directory, sizeof(shell_directory));
                env_set("PWD", shell_directory);
        }

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1) &&
               !string_get(shell_argv[index] + 2))
        {
                p8 letter = string_get(shell_argv[index] + 1);

                if (letter != 'L' && letter != 'P')
                        break;

                physical = letter == 'P';
                index++;
        }

        if (index < shell_argc)
                name = shell_argv[index];

        if (name && word_is(name, "--") && index + 1 < shell_argc)
                name = shell_argv[++index];

        if (!name)
        {
                name = env_get("HOME");

                if (!name)
                        return shell_answer(0);
        }
        else if (word_is(name, "-"))
        {
                name = env_get("OLDPWD");
                say = true;

                if (!name)
                        name = shell_directory;
        }

        // On a copy: both HOME and OLDPWD point into env_storage, which the
        // first env_set below is free to move out from under them.
        string_copy_max(shell_cd_target, name, sizeof(shell_cd_target) - 1);
        shell_cd_target[sizeof(shell_cd_target) - 1] = end;

        if (!shell_cd_walk(physical, address_of say))
        {
                shell_answer(2);

                return string_format(shell_diagnostic, "cd: can't cd to %s\n",
                                     shell_cd_target);
        }

        if (say)
                string_format(write, "%s\n", shell_directory);

        shell_answer(0);
}

fn shell_clear(writer write, string_address input)
{
        write(str(TERM_CLEAR_SCREEN));
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

fn shell_pwd(writer write, string_address input)
{
        p8 out_buffer[4096];
        bool physical = shell_argc > 1 && word_is(shell_argv[1], "-P");

        if (!physical && shell_directory_holds())
                return string_format(write, "%s\n", shell_directory);

        shell_here(out_buffer, sizeof(out_buffer));

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
        positive length = string_length(env_reading(name));
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
        positive at = printf_render(into, value, 10, false);

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

extern string_address shell_parameter[];
extern positive shell_parameter_count;
bool shell_parameters_set(string_address address_to words, positive count);
fn shell_parameters_shift(positive count);

/*
        The long names for the same letters.

        A script writes "set -o nounset" where a terminal writes "set -u", and
        four of the names have no letter at all. The ones that do are kept in
        the same bits the letters use, so the two spellings cannot disagree.
*/
typedef struct
{
        string_address name;
        p8 letter;
} shell_option;

static shell_option shell_option_names[] = {
    {"errexit", 'e'},   {"noglob", 'f'},     {"ignoreeof", 0},
    {"interactive", 0}, {"monitor", 'm'},    {"noexec", 'n'},
    {"stdin", 0},       {"xtrace", 'x'},     {"verbose", 'v'},
    {"vi", 0},          {"emacs", 0},        {"noclobber", 0},
    {"allexport", 'a'}, {"notify", 'b'},     {"nounset", 'u'},
    {"nolog", 0},       {"pipefail", 0},     {"debug", 0},
    {null, 0},
};

#define SHELL_OPTION_NAMES \
        (sizeof(shell_option_names) / sizeof(shell_option_names[0]))

static positive shell_options_named;

bool shell_option_on(positive index)
{
        // Two of them are not settings at all but answers about how the shell
        // was started, and it is started one way.
        if (!string_compare(shell_option_names[index].name, "stdin"))
                return true;

        if (!string_compare(shell_option_names[index].name, "interactive"))
                return shell_is_interactive != 0;

        if (shell_option_names[index].letter)
                return (shell_options & SHELL_FLAG(shell_option_names[index].letter)) != 0;

        return (shell_options_named & ((positive)1 << index)) != 0;
}

fn shell_option_told(positive index, bool on)
{
        if (shell_option_names[index].letter)
        {
                if (on)
                        shell_options |= SHELL_FLAG(shell_option_names[index].letter);
                else
                        shell_options &= ~SHELL_FLAG(shell_option_names[index].letter);

                return;
        }

        if (on)
                shell_options_named |= (positive)1 << index;
        else
                shell_options_named &= ~((positive)1 << index);
}

fn shell_options_listed(writer write, bool as_commands)
{
        positive index = 0;

        if (!as_commands)
                string_format(write, "Current option settings\n");

        while (shell_option_names[index].name)
        {
                bool on = shell_option_on(index);

                if (as_commands)
                {
                        write(on ? "set -o " : "set +o ", 7);
                        string_format(write, "%s\n", shell_option_names[index].name);
                }
                else
                {
                        positive length = string_length(shell_option_names[index].name);

                        write(shell_option_names[index].name, length);

                        while (length < 15)
                        {
                                write(" ", 1);
                                length++;
                        }

                        write(" ", 1);
                        string_format(write, "%s\n", on ? "on" : "off");
                }

                index++;
        }
}

bool shell_option_named(string_address word, bool on)
{
        positive index = string_table_find(word, shell_option_names,
                                           sizeof(shell_option_names[0]),
                                           SHELL_OPTION_NAMES);

        if (index >= SHELL_OPTION_NAMES)
                return false;

        shell_option_told(index, on);

        return true;
}

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

                                if (value == 'o')
                                {
                                        // The name is the next word, and with
                                        // no next word what is asked for is
                                        // the list of them.
                                        if (index + 1 >= shell_argc)
                                        {
                                                shell_options_listed(write, !on);
                                                letter++;
                                                continue;
                                        }

                                        if (!shell_option_named(shell_argv[++index], on))
                                        {
                                                shell_answer(2);

                                                shell_diagnostic(
                                                    on ? "set: Illegal option -o "
                                                       : "set: Illegal option +o ",
                                                    23);

                                                return string_format(
                                                    shell_diagnostic, "%s\n",
                                                    shell_argv[index]);
                                        }

                                        letter++;
                                        continue;
                                }

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

/*
        local.

        Not POSIX, and in every script anybody has written. What it is here is
        a save: the value a name had on the way into a function is put back on
        the way out, so what the function assigns cannot be seen outside it.

        The scope is dynamic and not lexical -- a function called from inside
        this one sees the local value -- because that is what dash does and
        what the scripts written against it expect.

        A name given without a value keeps the value it had. dash does that
        too, and it is the difference between marking a name and clearing it.
*/
#define LOCAL_MAX 128
#define LOCAL_STORAGE 8192
#define LOCAL_NAME 64
#define LOCAL_DEPTH 128
#define LOCAL_ABSENT ((positive)-1)

typedef struct
{
        p8 name[LOCAL_NAME];
        positive value;
} shell_local_entry;

static shell_local_entry local_table[LOCAL_MAX];
static positive local_count;
static p8 local_storage[LOCAL_STORAGE];
static positive local_used;
static positive local_from[LOCAL_DEPTH];
static positive local_held[LOCAL_DEPTH];
static positive local_depth;

fn shell_local_enter()
{
        if (local_depth < LOCAL_DEPTH)
        {
                local_from[local_depth] = local_count;
                local_held[local_depth] = local_used;
        }

        local_depth++;
}

fn shell_local_leave()
{
        positive at;

        if (!local_depth)
                return;

        local_depth--;

        if (local_depth >= LOCAL_DEPTH)
                return;

        at = local_count;

        // Backwards, so that a name saved twice ends on the value it had
        // before the first of them.
        while (at > local_from[local_depth])
        {
                at--;

                if (local_table[at].value == LOCAL_ABSENT)
                        env_unset(local_table[at].name);
                else
                        env_set(local_table[at].name, local_storage + local_table[at].value);
        }

        local_count = local_from[local_depth];
        local_used = local_held[local_depth];
}

static bool local_remember(string_address name)
{
        positive begin = local_depth && local_depth <= LOCAL_DEPTH
                             ? local_from[local_depth - 1]
                             : 0;
        string_address value;

        // Twice in one function is once. Without this a local in a loop fills
        // the table an iteration at a time.
        for (positive at = begin; at < local_count; at++)
                if (!string_compare(local_table[at].name, name))
                        return true;

        if (local_count >= LOCAL_MAX)
                return false;

        value = env_get(name);

        if (!value)
                local_table[local_count].value = LOCAL_ABSENT;
        else
        {
                positive length = string_length(env_reading(value));

                if (local_used + length + 1 > LOCAL_STORAGE)
                        return false;

                memory_copy(local_storage + local_used, value, length + 1);
                local_table[local_count].value = local_used;
                local_used += length + 1;
        }

        string_copy(local_table[local_count].name, name);
        local_count++;

        return true;
}

fn shell_local(writer write, string_address input)
{
        positive index = 1;

        if (!local_depth)
        {
                shell_diagnostic("local: not in a function\n", 0);
                return shell_answer(2);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index++];
                string_address mark = string_first_of(word, '=');
                positive length = mark ? (positive)(mark - word) : string_length(word);
                p8 name[LOCAL_NAME];

                if (!length || length >= LOCAL_NAME)
                {
                        shell_diagnostic("local: bad name\n", 0);
                        return shell_answer(2);
                }

                memory_copy(name, word, length);
                name[length] = end;

                if (!local_remember(name))
                {
                        shell_diagnostic("local: too many\n", 0);
                        return shell_answer(2);
                }

                if (mark)
                        env_set(name, mark + 1);
        }

        shell_answer(0);
}

fn shell_readonly(writer write, string_address input)
{
        positive index = 1;
        bool listed = shell_argc < 2;

        while (index < shell_argc && word_is(shell_argv[index], "-p"))
        {
                listed = true;
                index++;
        }

        if (listed && index >= shell_argc)
        {
                positive at = 0;

                while (at < readonly_count)
                {
                        string_address value = env_get(readonly_name[at]);

                        string_format(write, "readonly %s", readonly_name[at]);

                        if (value)
                        {
                                write("=", 1);
                                shell_quoted(write, value);
                        }

                        write("\n", 1);
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
#define TEST_BEFORE 12
#define TEST_AFTER 13

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

        if (op == 'O')
                return facts.owner == (p32)system_call_1(syscall(geteuid), 0);

        if (op == 'G')
                return facts.group == (p32)system_call_1(syscall(getegid), 0);

        return false;
}

// The three fields test wants out of statx.
b64 test_modified(file_facts address_to facts)
{
        return facts->modified.seconds;
}

p32 test_modified_fraction(file_facts address_to facts)
{
        return facts->modified.nanoseconds;
}

p64 test_device(file_facts address_to facts)
{
        return ((p64)facts->device_major << 32) | facts->device_minor;
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
               letter == 'z' || letter == 'G' || letter == 'L' || letter == 'O' ||
               letter == 'S';
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

        if (word_is(word, "<"))
                return TEST_BEFORE;

        if (word_is(word, ">"))
                return TEST_AFTER;

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
                bool here = test_facts(left, address_of one, true);
                bool there = test_facts(right, address_of two, true);

                /*
                        A file that is not there is older than one that is.
                        POSIX says so of both -nt and -ot, and it is the answer
                        a script wants: "test built -nt source" has to be false
                        the first time round, when nothing has been built yet.
                */
                if (kind == TEST_NEWER && here && !there)
                        return true;

                if (kind == TEST_OLDER && !here && there)
                        return true;

                if (!here || !there)
                        return false;

                if (kind == TEST_SAME_FILE)
                        return one.inode == two.inode &&
                               test_device(address_of one) == test_device(address_of two);

                // Two files written in the same second are not the same age,
                // and a script that touches one after the other says so.
                if (test_modified(address_of one) != test_modified(address_of two))
                {
                        if (kind == TEST_NEWER)
                                return test_modified(address_of one) >
                                       test_modified(address_of two);

                        return test_modified(address_of one) <
                               test_modified(address_of two);
                }

                if (kind == TEST_NEWER)
                        return test_modified_fraction(address_of one) >
                               test_modified_fraction(address_of two);

                return test_modified_fraction(address_of one) <
                       test_modified_fraction(address_of two);
        }

        if (kind == TEST_BEFORE)
                return string_compare(left, right) < 0;

        if (kind == TEST_AFTER)
                return string_compare(left, right) > 0;

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

/*
        The short forms, counted before they are parsed.

        POSIX settles one, two, three and four words by how many there are and
        not by what they look like, and the two orders disagree: "! = x" is
        three words with a binary operator in the middle, so it compares "!"
        against "x" rather than negating anything. A parser that reads left to
        right takes the "!" first and is wrong here, which is the shape most
        implementations of test get wrong.

        Answers false and clears handled when the count says nothing, which is
        where the general parser takes over.
*/
bool test_short(positive from, positive to, bool address_to handled)
{
        positive count = to - from;
        bool inner;

        address_to handled = true;

        if (count == 1)
                return string_get(shell_argv[from]) != end;

        if (count == 2)
        {
                if (word_is(shell_argv[from], "!"))
                        return string_get(shell_argv[from + 1]) == end;

                if (test_is_unary(shell_argv[from]))
                        return test_unary(string_get(shell_argv[from] + 1),
                                          shell_argv[from + 1]);

                address_to handled = false;
                return false;
        }

        if (count == 3)
        {
                positive kind = test_is_binary(shell_argv[from + 1]);

                if (kind)
                        return test_compare(kind, shell_argv[from], shell_argv[from + 2]);

                if (word_is(shell_argv[from], "!"))
                {
                        bool value = !test_short(from + 1, to, address_of inner);

                        address_to handled = inner;
                        return inner ? value : false;
                }

                if (word_is(shell_argv[from], "(") && word_is(shell_argv[to - 1], ")"))
                        return test_short(from + 1, to - 1, handled);

                address_to handled = false;
                return false;
        }

        if (count == 4)
        {
                if (word_is(shell_argv[from], "!"))
                {
                        bool value = !test_short(from + 1, to, address_of inner);

                        address_to handled = inner;
                        return inner ? value : false;
                }

                if (word_is(shell_argv[from], "(") && word_is(shell_argv[to - 1], ")"))
                        return test_short(from + 1, to - 1, handled);
        }

        address_to handled = false;
        return false;
}

fn shell_test(writer write, string_address input)
{
        bool value;
        bool handled;

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

        if (test_stop - test_at <= 4)
        {
                value = test_short(test_at, test_stop, address_of handled);

                if (handled)
                        return shell_answer(test_bad ? 2 : (value ? 0 : 1));
        }

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

// \c says stop, and it means the whole of printf and not just the argument it
// was found in: everything still to be written, format and all, is dropped.
static bool printf_cut;
static bool printf_in_b;
static b32 printf_status;

static p8 printf_hold[2048];
static positive printf_held;

// What printf writes without looking at it: everything but the terminator and
// the one or two bytes that mean something where it is being read.
static b8 printf_plain[STRING_SET_BYTES];
static b8 printf_text[STRING_SET_BYTES];
static b32 printf_sets_ready;

static fn printf_sets_prepare()
{
        if (printf_sets_ready)
                return;

        for (positive c = 1; c < STRING_SET_BYTES; c++)
                printf_plain[c] = printf_text[c] = 1;

        printf_plain['\\'] = printf_plain['%'] = 0;
        printf_text['\\'] = 0;
        printf_sets_ready = true;
}

static fn printf_holder(address_any data, positive length)
{
        positive room = sizeof(printf_hold) - printf_held;

        if (length > room)
                length = room;

        memory_copy_fast(printf_hold + printf_held, data, length);
        printf_held += length;
}

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

        // Only in a %b argument. In the format itself the reference shell
        // leaves the two bytes where they stood.
        if (value == 'c' && printf_in_b)
        {
                printf_cut = true;
                return step;
        }

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
        printf_sets_prepare();

        while (string_get(text) && !printf_cut)
        {
                positive run = string_span(text, printf_text);

                if (run)
                {
                        write(text, run);
                        text += run;
                        continue;
                }

                text = printf_escape(write, text + 1);
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

// An argument that is not a number is still printed, as zero, and the status
// says so afterwards; that is what the reference shell does.
fn printf_not_a_number(string_address word)
{
        string_format(shell_diagnostic, "printf: %s: expected numeric value\n", word);
        printf_status = 1;
}

fn printf_one(writer write, string_address format)
{
        string_address step = format;

        printf_sets_prepare();

        while (string_get(step) && !printf_cut)
        {
                bool left = false;
                bool zero = false;
                bool plus = false;
                bool space = false;
                positive width = 0;
                bipolar precision = -1;
                positive run = string_span(step, printf_plain);
                p8 conversion;

                if (run)
                {
                        write(step, run);
                        step += run;
                        continue;
                }

                if (string_is(step, '\\'))
                {
                        step = printf_escape(write, step + 1);
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
                        bipolar asked = shell_signed(printf_next(), address_of good);

                        // A negative width is the minus flag written out long.
                        if (asked < 0)
                        {
                                left = true;
                                asked = -asked;
                        }

                        width = (positive)asked;
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

                                if (precision < 0)
                                        precision = -1;

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
                        positive length;

                        // The escapes are in the argument, so a width around
                        // them can only be measured once they have been read.
                        // Streamed unless something has to be measured: what
                        // comes out is as long as the argument, and only a
                        // width or a precision needs it in hand first.
                        if (conversion == 'b' && !width && precision < 0)
                        {
                                printf_in_b = true;
                                printf_escaped(write, value);
                                printf_in_b = false;
                                continue;
                        }

                        if (conversion == 'b')
                        {
                                printf_held = 0;
                                printf_in_b = true;
                                printf_escaped(printf_holder, value);
                                printf_in_b = false;
                                value = printf_hold;
                                length = printf_held;
                        }
                        else
                        {
                                length = string_length(env_reading(value));
                        }

                        if (precision >= 0 && (positive)precision < length)
                                length = (positive)precision;

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
                        string_address word = printf_next();
                        bipolar value = shell_signed(word, address_of good);
                        p8 sign = 0;

                        if (!good)
                                printf_not_a_number(word);

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
                        string_address word = printf_next();
                        bipolar value = shell_signed(word, address_of good);
                        positive base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);

                        if (!good)
                                printf_not_a_number(word);

                        printf_number(write, (positive)value, 0, base, conversion == 'X',
                                      width, precision, left, zero);
                        continue;
                }

                {
                        p8 said[3] = {'%', conversion, end};

                        string_format(shell_diagnostic,
                                      "printf: %s: invalid directive\n", said);
                }

                printf_status = 2;
                printf_cut = true;
        }
}

fn shell_printf(writer write, string_address input)
{
        string_address format;

        if (shell_argc < 2)
                return shell_answer(2);

        format = shell_argv[1];
        printf_argument = 2;
        printf_cut = false;
        printf_status = 0;

        while (1)
        {
                printf_took = false;
                printf_one(write, format);

                // A format with no conversion in it would otherwise run for as
                // long as there were arguments left.
                if (printf_cut || printf_argument >= shell_argc || !printf_took)
                        break;
        }

        shell_answer(printf_status);
}

/*
        read.

        A byte at a time, because the line arrives on the same descriptor the
        shell is reading its script from and anything larger swallows what
        comes after.
*/

#define READ_LINE 4096

static p8 read_line[READ_LINE];
static p8 read_literal[READ_LINE];
static positive read_length;

// Whether a byte splits a field. The two questions are different: every byte
// in IFS ends a field, but only the blanks among them are allowed to run
// together and to be thrown away at the ends.
bool read_separates(string_address ifs, positive at)
{
        if (read_literal[at])
                return false;

        return string_first_of(ifs, read_line[at]) != null;
}

bool read_blank(string_address ifs, positive at)
{
        if (read_literal[at])
                return false;

        if (read_line[at] != ' ' && read_line[at] != '\t' && read_line[at] != '\n')
                return false;

        return string_first_of(ifs, read_line[at]) != null;
}

// A tenth of a second at a time is close enough for a timeout measured in
// whole seconds, and it keeps the wait in one place.
bool read_waited(bipolar tenths)
{
        struct
        {
                b32 descriptor;
                b16 asked;
                b16 got;
        } watch;

        struct
        {
                b64 seconds;
                b64 nanoseconds;
        } span;

        watch.descriptor = 0;
        watch.asked = 1;
        watch.got = 0;

        span.seconds = tenths / 10;
        span.nanoseconds = (tenths % 10) * 100000000;

        return system_call_5(syscall(ppoll), (positive)address_of watch, 1,
                             (positive)address_of span, 0, 8) > 0;
}

fn shell_read(writer write, string_address input)
{
        bool raw = false;
        positive index = 1;
        positive at = 0;
        positive names;
        bool ended = false;
        bool limited = false;
        positive limit = 0;
        bipolar tenths = -1;
        p8 stop_at = '\n';
        string_address ifs;
        p8 ifs_held[128];
        p8 ifs_default[] = " \t\n";

        read_length = 0;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_not(shell_argv[index] + 1, end))
        {
                string_address letter = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter);
                        string_address value = null;

                        if (which == 'r')
                        {
                                raw = true;
                                letter++;
                                continue;
                        }

                        if (which != 'p' && which != 'n' && which != 'd' && which != 't')
                        {
                                letter++;
                                continue;
                        }

                        // The rest of the word if there is any, and the next
                        // word if there is not.
                        if (string_get(letter + 1))
                        {
                                value = letter + 1;
                                letter += string_length(letter + 1) + 1;
                        }
                        else if (index + 1 < shell_argc)
                        {
                                value = shell_argv[++index];
                                letter++;
                        }
                        else
                        {
                                letter++;
                        }

                        if (!value)
                                continue;

                        if (which == 'p')
                                shell_diagnostic(value, 0);
                        else if (which == 'n')
                        {
                                limited = true;
                                limit = shell_number(value);
                        }
                        else if (which == 'd')
                                stop_at = string_get(value);
                        else
                                tenths = (bipolar)shell_number(value) * 10;
                }

                index++;
        }

        names = index;

        while (read_length < READ_LINE - 1 && !(limited && read_length >= limit))
        {
                p8 value;

                if (tenths >= 0 && !read_waited(tenths))
                {
                        ended = true;
                        break;
                }

                if (system_call_3(syscall(read), 0, (positive)address_of value, 1) != 1)
                {
                        ended = true;
                        break;
                }

                if (value == stop_at)
                        break;

                if (!raw && value == '\\')
                {
                        p8 next;

                        if (system_call_3(syscall(read), 0, (positive)address_of next, 1) != 1)
                        {
                                ended = true;
                                break;
                        }

                        // A backslash before the delimiter joins the two lines.
                        if (next == stop_at)
                                continue;

                        read_literal[read_length] = 1;
                        read_line[read_length++] = next;
                        continue;
                }

                read_literal[read_length] = 0;
                read_line[read_length++] = value;
        }

        read_line[read_length] = end;
        read_literal[read_length] = 0;

        if (names >= shell_argc)
        {
                env_set("REPLY", read_line);
                return shell_answer(ended ? 1 : 0);
        }

        {
                string_address value = env_get("IFS");

                // On a copy: IFS points into env_storage, and the first name
                // assigned below is free to compact the block out from under
                // it.
                if (value)
                {
                        string_copy_max(ifs_held, value, sizeof(ifs_held) - 1);
                        ifs_held[sizeof(ifs_held) - 1] = end;
                        ifs = ifs_held;
                }
                else
                {
                        ifs = ifs_default;
                }
        }

        while (names < shell_argc)
        {
                positive begin;

                while (at < read_length && read_blank(ifs, at))
                        at++;

                begin = at;

                /*
                        The last name takes everything that is left, delimiters
                        and all, which is what makes "read line" read a line.
                        Only the blanks at the end come off: a delimiter that
                        is not one is part of what was said.
                */
                if (names + 1 == shell_argc)
                {
                        positive stop = read_length;

                        while (stop > begin && read_blank(ifs, stop - 1))
                                stop--;

                        read_line[stop] = end;
                        env_set(shell_argv[names], read_line + begin);
                        at = read_length;
                        names++;
                        continue;
                }

                while (at < read_length && !read_separates(ifs, at))
                        at++;

                if (at < read_length)
                {
                        positive after = at;

                        while (after < read_length && read_blank(ifs, after))
                                after++;

                        // One that is not a blank ends the field on its own,
                        // and the blanks either side of it belong to it.
                        if (after < read_length && read_separates(ifs, after))
                        {
                                after++;

                                while (after < read_length && read_blank(ifs, after))
                                        after++;
                        }

                        // Only now: the byte at the cut is what said where the
                        // field ended, and reading it back as a terminator
                        // made every separator look like a blank.
                        read_line[at] = end;
                        at = after;
                }

                env_set(shell_argv[names], read_line + begin);
                names++;
        }

        shell_answer(ended ? 1 : 0);
}

/*
        getopts.

        One option per call, its place kept in OPTIND and, within a bundled
        word, in an offset of its own that OPTIND has no room for.

        OPTIND names the word the next call will start at, and a bundle is
        counted as read the moment its first letter is taken -- so "-ab" leaves
        OPTIND at two after the a as well as after the b, and the offset is all
        that says the word is not finished. Setting OPTIND back to one starts
        the whole thing again, which POSIX asks for and this reference shell
        does not do.
*/
static bipolar getopts_offset = -1;

// Nothing left to read: the name is told so, and where the walk stopped is
// left where it is for a caller that puts OPTIND back.
fn shell_getopts_done(string_address name, positive next)
{
        getopts_offset = -1;
        env_set_number("OPTIND", next + 1);
        env_set(name, "?");

        shell_answer(1);
}

// Where the next call starts, and how far into the word it just read. A step
// that has nothing after it says the word is finished with.
fn shell_getopts_answer(string_address name, string_address said,
                        string_address word, string_address step, positive next)
{
        getopts_offset = step && string_get(step) ? (bipolar)(step - word) : -1;

        env_set_number("OPTIND", next + 1);
        env_set(name, said);

        shell_answer(0);
}

fn shell_getopts(writer write, string_address input)
{
        string_address options;
        string_address name;
        string_address list[POSITIONAL_MAX];
        positive count = 0;
        positive optind;
        positive next;
        string_address word = null;
        string_address step = null;
        string_address found;
        p8 letter;
        p8 value[2];
        bool silent;

        if (shell_argc < 3)
                return shell_answer(2);

        options = shell_argv[1];
        name = shell_argv[2];
        silent = string_is(options, ':');

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

        next = optind - 1;

        // Where the last call stopped inside a word it had not finished. Only
        // believable while OPTIND still names the word after that one.
        if (optind > 1 && optind - 2 < count && getopts_offset >= 0 &&
            (positive)getopts_offset <= string_length(list[optind - 2]))
        {
                word = list[optind - 2];
                step = word + getopts_offset;
        }

        if (!step || !string_get(step))
        {
                word = next < count ? list[next] : null;
                step = word;

                if (!step || string_not(step, '-') || !string_get(step + 1))
                        return shell_getopts_done(name, next);

                step++;
                next++;

                if (string_is(step, '-') && !string_get(step + 1))
                        return shell_getopts_done(name, next);
        }

        letter = string_get(step++);
        value[0] = letter;
        value[1] = end;

        // A colon in the word is never an option, whatever the option string
        // says: it is the character that marks one as taking an argument.
        found = letter == ':' ? null : string_first_of(options, letter);

        if (!found)
        {
                if (silent)
                        env_set("OPTARG", value);
                else
                {
                        env_unset("OPTARG");
                        string_format(shell_diagnostic,
                                      "getopts: illegal option -- %s\n", value);
                }

                return shell_getopts_answer(name, "?", word, step, next);
        }

        if (string_is(found + 1, ':'))
        {
                if (!string_get(step) && next >= count)
                {
                        if (silent)
                        {
                                env_set("OPTARG", value);

                                return shell_getopts_answer(name, ":", word, null, next);
                        }

                        env_unset("OPTARG");
                        string_format(shell_diagnostic,
                                      "getopts: option requires an argument -- %s\n",
                                      value);

                        return shell_getopts_answer(name, "?", word, null, next);
                }

                if (!string_get(step))
                        step = list[next++];

                env_set("OPTARG", step);

                return shell_getopts_answer(name, value, word, null, next);
        }

        env_set("OPTARG", "");

        return shell_getopts_answer(name, value, word, step, next);
}

/*
        umask, in both spellings.

        The symbolic form talks about what a file is allowed, and the mask is
        the other way round -- "u=rwx" says the owner keeps everything, which
        is nothing masked off. So the letters are read into permissions and the
        answer is inverted on the way out.
*/
positive umask_letters(string_address address_to step)
{
        positive bits = 0;

        while (string_get(address_to step))
        {
                p8 letter = string_get(address_to step);

                if (letter == 'r')
                        bits |= 4;
                else if (letter == 'w')
                        bits |= 2;
                else if (letter == 'x')
                        bits |= 1;
                else
                        break;

                address_to step = address_to step + 1;
        }

        return bits;
}

bool umask_symbolic(string_address step, positive address_to mask)
{
        positive allowed = 07777 & ~(address_to mask);

        while (string_get(step))
        {
                positive who = 0;
                positive bits;
                p8 action;

                while (string_is(step, 'u') || string_is(step, 'g') ||
                       string_is(step, 'o') || string_is(step, 'a'))
                {
                        p8 letter = string_get(step++);

                        if (letter == 'u' || letter == 'a')
                                who |= 0700;

                        if (letter == 'g' || letter == 'a')
                                who |= 0070;

                        if (letter == 'o' || letter == 'a')
                                who |= 0007;
                }

                if (!who)
                        who = 0777;

                action = string_get(step);

                if (action != '=' && action != '+' && action != '-')
                        return false;

                step++;
                bits = umask_letters(address_of step);
                bits = ((bits << 6) | (bits << 3) | bits) & who;

                if (action == '=')
                        allowed = (allowed & ~who) | bits;
                else if (action == '+')
                        allowed |= bits;
                else
                        allowed &= ~bits;

                if (string_is(step, ','))
                {
                        step++;
                        continue;
                }

                if (string_get(step))
                        return false;
        }

        address_to mask = 0777 & ~allowed;

        return true;
}

fn umask_written(writer write, positive mask)
{
        positive shift = 9;

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
}

fn umask_spoken(writer write, positive mask)
{
        positive allowed = 0777 & ~mask;
        positive shift = 9;
        string_address names = "ugo";

        while (shift)
        {
                positive three;

                shift -= 3;
                three = (allowed >> shift) & 7;

                write(names++, 1);
                write("=", 1);

                if (three & 4)
                        write("r", 1);

                if (three & 2)
                        write("w", 1);

                if (three & 1)
                        write("x", 1);

                write(shift ? "," : "\n", 1);
        }
}

fn shell_umask(writer write, string_address input)
{
        positive index = 1;
        bool spoken = false;
        positive mask;

        while (index < shell_argc && word_is(shell_argv[index], "-S"))
        {
                spoken = true;
                index++;
        }

        // The only way to read it is to set it, so it is put straight back.
        mask = system_call_1(syscall(umask), 0);
        system_call_1(syscall(umask), mask);

        if (index >= shell_argc)
        {
                if (spoken)
                        umask_spoken(write, mask);
                else
                        umask_written(write, mask);

                return shell_answer(0);
        }

        {
                string_address word = shell_argv[index];

                if (string_get(word) >= '0' && string_get(word) <= '7')
                {
                        positive value = 0;

                        while (string_get(word) >= '0' && string_get(word) <= '7')
                                value = value * 8 + (string_get(word++) - '0');

                        if (string_get(word))
                        {
                                shell_answer(2);

                                return string_format(shell_diagnostic,
                                                     "umask: Illegal mode: %s\n",
                                                     shell_argv[index]);
                        }

                        mask = value;
                }
                else if (!umask_symbolic(word, address_of mask))
                {
                        shell_answer(2);

                        return string_format(shell_diagnostic,
                                             "umask: Illegal mode: %s\n", word);
                }
        }

        system_call_1(syscall(umask), mask);

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

// Six places after the point, which is what a %f with nothing said about it
// writes and so what the reference shell prints. Only the first two of them
// can ever be anything but zero at a hundred ticks to the second.
#define CLOCK_PLACES 1000000

fn shell_time_written(writer write, bipolar ticks)
{
        positive seconds;
        positive fraction;
        positive scale = CLOCK_PLACES / 10;

        if (ticks < 0)
                ticks = 0;

        seconds = (positive)ticks / CLOCK_TICKS;
        fraction = ((positive)ticks % CLOCK_TICKS) * (CLOCK_PLACES / CLOCK_TICKS);

        shell_number_padded(write, seconds / 60, 0);
        write("m", 1);
        shell_number_padded(write, seconds % 60, 0);
        write(".", 1);

        while (scale)
        {
                p8 digit = '0' + (fraction / scale) % 10;

                write(address_of digit, 1);
                scale /= 10;
        }

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

#define TRAP_NAMES (sizeof(trap_names) / sizeof(trap_names[0]))

bipolar trap_number(string_address word)
{
        positive index = 0;
        bool good;
        bipolar value;

        if (!word)
                return -1;

        if (string_is(word, 'S') && string_is(word + 1, 'I') && string_is(word + 2, 'G'))
                word += 3;

        index = string_table_find(word, trap_names, sizeof(trap_names[0]),
                                  TRAP_NAMES);

        if (index < TRAP_NAMES)
                return (bipolar)index;

        value = shell_signed(word, address_of good);

        return good ? value : -1;
}

/*
        Where a signal is written down, and where it is acted on.

        A handler runs on top of whatever the shell was in the middle of, so
        it does one thing: mark the signal. The action is run at the end of a
        command, which is where POSIX says it runs and the only place the
        parser is not already in use.

        Volatile because the writer is the handler and the reader is the code
        it interrupted, which is the one arrangement where a compiler keeping
        the value in a register is wrong.
*/
#define TRAP_SIGNAL_MAX 64

static volatile p8 trap_pending[TRAP_SIGNAL_MAX + 1];
static volatile bool trap_caught;
static bool trap_inside;

fn trap_signal_caught(b32 number)
{
        if (number > 0 && number <= TRAP_SIGNAL_MAX)
                trap_pending[number] = 1;

        trap_caught = true;
}

bool trap_waiting()
{
        return trap_caught && !trap_inside;
}

/*
        What is left of a trap in a child.

        A fork inherits the handlers and has no shell behind it to run the
        action, so every trapped signal goes back to what it was. An ignored
        one stays ignored, which is what POSIX asks for and what keeps a
        subshell from dying of a signal its parent chose to sit out.
*/
fn trap_default_all()
{
        positive at = 0;

        while (at < trap_count)
        {
                positive number = trap_table[at].number;

                if (number && string_get(trap_table[at].action))
                        shell_default((b32)number);

                at++;
        }

        for (positive i = 0; i <= TRAP_SIGNAL_MAX; i++)
                trap_pending[i] = 0;

        trap_caught = false;
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

                /*
                        Ignored on the way in and not a terminal: the action is
                        written down and the signal is left alone, so trap
                        lists what the script asked for and the script is
                        still never woken by it. dash keeps the string the
                        same way, and a signal that never arrives never runs
                        what is written against it.
                */
                bool deaf = number > 0 && !shell_is_interactive &&
                            shell_was_ignored((positive)number);

                trap_forget((positive)number);

                /*
                        "trap - INT" gives the signal back to the kernel,
                        "trap '' INT" makes the shell deaf to it, and anything
                        else is a line to run when it arrives. Only the third
                        needs a handler, and only signals: EXIT is something
                        the shell does to itself.
                */
                if (number && !deaf)
                {
                        if (!action)
                                shell_default((b32)number);
                        else if (!string_get(action))
                                shell_ignore((b32)number);
                        else
                                shell_catch((b32)number);
                }

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
        positive at = string_table_find(name, alias_table, sizeof(alias_table[0]),
                                        alias_count);

        return at < alias_count ? alias_table[at].value : null;
}

bool alias_record(string_address name, positive name_length, string_address value)
{
        positive value_length = string_length(env_reading(value));
        positive index;

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

                        index = string_table_find(kept_name, alias_table,
                                                  sizeof(alias_table[0]),
                                                  alias_count);

                        if (index < alias_count)
                        {
                                alias_table[index].value = kept_value;
                                return true;
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
        b32 status = 0;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive at;

                if (word_is(word, "-a"))
                {
                        alias_count = 0;
                        alias_used = 0;
                        index++;
                        continue;
                }

                at = string_table_find(word, alias_table, sizeof(alias_table[0]),
                                       alias_count);

                // A name that was never an alias is something the script asked
                // for and did not get, which POSIX has this say so.
                if (at >= alias_count)
                        status = 1;

                while (at + 1 < alias_count)
                {
                        alias_table[at] = alias_table[at + 1];
                        at++;
                }

                if (at < alias_count)
                        alias_count--;

                index++;
        }

        shell_answer(status);
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

        */
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
typedef b32 (address_to shell_tool_function)();

typedef struct
{
        string_address name;
        shell_tool_function function;
} shell_tool;

static shell_tool shell_tools[] = {
    {"awk", text_awk},
    {"cat", text_cat},
    {"dd", tools_dd},
    {"diff", tools_diff},
    {"ps", tools_ps},
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
    {"date", file_date},
    {"df", file_df},
    {"dirname", file_dirname},
    {"du", file_du},
    {"env", file_env},
    {"find", file_find},
    {"hostname", file_hostname},
    {"id", file_id},
    {"kill", file_kill},
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
    {"xargs", file_xargs},
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
                trap_default_all();

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


// The next signal that arrived and has not been acted on, or nothing.
bipolar trap_taken()
{
        if (!trap_caught)
                return -1;

        for (positive number = 1; number <= TRAP_SIGNAL_MAX; number++)
        {
                if (!trap_pending[number])
                        continue;

                trap_pending[number] = 0;
                return (bipolar)number;
        }

        trap_caught = false;

        return -1;
}

fn trap_entered(bool inside)
{
        trap_inside = inside;
}

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
fn shell_hash(writer write, string_address input);
fn shell_ulimit(writer write, string_address input);

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
    {"hash", shell_hash},
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
    {"ulimit", shell_ulimit},
    {"umask", shell_umask},
    {"unalias", shell_unalias},
    {"unset", shell_unset},
    {"wait", shell_wait},
    {"which", shell_which},
    {"help", shell_help},
    {"local", shell_local},
    {"export", shell_export},
    {null, null},
};

/*
        Where a name was found last time.

        A path walk is one faccessat per directory on PATH, and a loop calling
        the same program a thousand times pays for all of them a thousand
        times. What is remembered here is the answer, not a hint: it is used as
        it stands, which is what makes hash -r something a script needs when it
        puts a new program somewhere earlier on the path.

        Assigning PATH throws the whole table away, because every answer in it
        was about the old one.
*/
#define HASH_MAX 64
#define HASH_STORAGE 4096

static string_address hash_name[HASH_MAX];
static string_address hash_path[HASH_MAX];
static p8 hash_storage[HASH_STORAGE];
static positive hash_used;
static positive hash_count;

fn hash_forget()
{
        hash_count = 0;
        hash_used = 0;
}

string_address hash_find(string_address name)
{
        positive at = string_table_find(name, hash_name, sizeof(hash_name[0]),
                                        hash_count);

        return at < hash_count ? hash_path[at] : null;
}

fn hash_remember(string_address name, string_address path)
{
        positive name_length = string_length(name);
        positive path_length = string_length(path);

        if (hash_find(name))
                return;

        if (hash_count >= HASH_MAX ||
            hash_used + name_length + path_length + 2 > HASH_STORAGE)
                return;

        hash_name[hash_count] = hash_storage + hash_used;
        memory_copy(hash_storage + hash_used, name, name_length + 1);
        hash_used += name_length + 1;

        hash_path[hash_count] = hash_storage + hash_used;
        memory_copy(hash_storage + hash_used, path, path_length + 1);
        hash_used += path_length + 1;

        hash_count++;
}

fn shell_hash(writer write, string_address input)
{
        positive index = 1;
        b32 bad = 0;

        while (index < shell_argc && word_is(shell_argv[index], "-r"))
        {
                hash_forget();
                index++;
        }

        if (index >= shell_argc)
        {
                positive at = 0;

                while (at < hash_count)
                        string_format(write, "%s\n", hash_path[at++]);

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                p8 found[768];

                if (!shell_find_in_path(shell_argv[index], found, sizeof(found)))
                {
                        bad = 1;
                        string_format(shell_diagnostic, "hash: %s: not found\n",
                                      shell_argv[index]);
                }

                index++;
        }

        shell_answer(bad);
}

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

        {
                string_address known = hash_find(name);

                if (known)
                {
                        string_copy_max(into, known, room - 1);
                        into[room - 1] = end;
                        return true;
                }
        }

        if (value == null)
                value = "/bin:/usr/bin:/";

        string_copy_max(search, value, sizeof(search) - 1);
        search[sizeof(search) - 1] = end;

        segment = search;
        name_length = string_length(env_reading(name));

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
                        {
                                hash_remember(name, into);
                                return true;
                        }
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
        bool at_length = false;

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
                        if (string_get(letter) == 'v')
                                only_say = true;
                        else if (string_get(letter) == 'V')
                        {
                                only_say = true;
                                at_length = true;
                        }
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
                                string_format(write, at_length
                                                             ? "%s is a shell builtin\n"
                                                             : "%s\n",
                                              name);
                                return shell_answer(0);
                        }

                        command++;
                }

                if (shell_tool_here(name))
                {
                        string_format(write, at_length ? "%s is a shell builtin\n"
                                                       : "%s\n",
                                      name);
                        return shell_answer(0);
                }

                if (exec_function_here && exec_function_here(name))
                {
                        string_format(write, at_length ? "%s is a shell function\n"
                                                       : "%s\n",
                                      name);
                        return shell_answer(0);
                }

                if (shell_find_in_path(name, found, sizeof(found)))
                {
                        if (at_length)
                                string_format(write, "%s is %s\n", name, found);
                        else
                                string_format(write, "%s\n", found);

                        return shell_answer(0);
                }

                if (at_length)
                        string_format(write, "%s: not found\n", name);

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

/*
        ulimit.

        prlimit64 rather than getrlimit, because getrlimit is not on the
        riscv64 table at all and prlimit64 is on all three. The kernel counts
        in bytes and in seconds; the shell has been quoting file sizes in five
        hundred and twelve byte blocks and memory in kilobytes since long
        before either of us, so each limit carries the number it is divided by.
*/
#define LIMIT_INFINITE ((p64)0 - 1)

typedef struct
{
        string_address name;
        p8 letter;
        p8 resource;
        positive step;
} shell_limit;

static shell_limit shell_limits[] = {
    {"time(seconds)", 't', 0, 1},
    {"file(blocks)", 'f', 1, 512},
    {"data(kbytes)", 'd', 2, 1024},
    {"stack(kbytes)", 's', 3, 1024},
    {"coredump(blocks)", 'c', 4, 512},
    {"memory(kbytes)", 'm', 5, 1024},
    {"locked memory(kbytes)", 'l', 8, 1024},
    {"process", 'p', 6, 1},
    {"nofiles", 'n', 7, 1},
    {"vmemory(kbytes)", 'v', 9, 1024},
    {"locks", 'w', 10, 1},
    {"rtprio", 'r', 14, 1},
    {null, 0, 0, 0},
};

typedef struct
{
        p64 soft;
        p64 hard;
} shell_limit_pair;

bool shell_limit_read(positive resource, shell_limit_pair address_to out)
{
        return system_call_4(syscall(prlimit64), 0, resource, 0,
                             (positive)out) == 0;
}

bool shell_limit_write(positive resource, shell_limit_pair address_to in)
{
        return system_call_4(syscall(prlimit64), 0, resource, (positive)in, 0) == 0;
}

fn shell_limit_said(writer write, shell_limit address_to limit, bool hard)
{
        shell_limit_pair pair;
        p64 value;

        if (!shell_limit_read(limit->resource, address_of pair))
                return string_format(write, "unlimited\n");

        value = hard ? pair.hard : pair.soft;

        if (value == LIMIT_INFINITE)
                return string_format(write, "unlimited\n");

        shell_number_padded(write, (positive)(value / limit->step), 0);
        write("\n", 1);
}

fn shell_limit_listed(writer write)
{
        shell_limit address_to limit = shell_limits;

        while (limit->name)
        {
                positive length = string_length(limit->name);

                write(limit->name, length);

                while (length < 20)
                {
                        write(" ", 1);
                        length++;
                }

                write(" ", 1);
                shell_limit_said(write, limit, false);
                limit++;
        }
}

fn shell_ulimit(writer write, string_address input)
{
        positive index = 1;
        bool hard = false;
        bool soft = false;
        bool listed = false;
        shell_limit address_to chosen = null;
        shell_limit address_to limit;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

                        if (which == 'H')
                        {
                                hard = true;
                                continue;
                        }

                        if (which == 'S')
                        {
                                soft = true;
                                continue;
                        }

                        if (which == 'a')
                        {
                                listed = true;
                                continue;
                        }

                        limit = shell_limits;

                        while (limit->name && limit->letter != which)
                                limit++;

                        if (!limit->name)
                        {
                                shell_answer(2);

                                {
                                        p8 said[2] = {which, end};

                                        return string_format(
                                            shell_diagnostic,
                                            "ulimit: Illegal option -%s\n", said);
                                }
                        }

                        chosen = limit;
                }

                index++;
        }

        if (listed)
        {
                shell_limit_listed(write);

                return shell_answer(0);
        }

        // No resource named is the file size, which is what every shell means
        // by a bare ulimit.
        if (!chosen)
                chosen = shell_limits + 1;

        if (index >= shell_argc)
        {
                shell_limit_said(write, chosen, hard);

                return shell_answer(0);
        }

        {
                shell_limit_pair pair;
                p64 value;

                if (!shell_limit_read(chosen->resource, address_of pair))
                        return shell_answer(1);

                if (word_is(shell_argv[index], "unlimited"))
                        value = LIMIT_INFINITE;
                else
                {
                        bool good;
                        bipolar asked = shell_signed(shell_argv[index], address_of good);

                        if (!good)
                        {
                                shell_answer(2);

                                return string_format(shell_diagnostic,
                                                     "ulimit: bad number %s\n",
                                                     shell_argv[index]);
                        }

                        value = (p64)asked * chosen->step;
                }

                // Neither said means both, which is the only way a script can
                // lower a ceiling it will never be allowed to raise again.
                if (hard || !soft)
                        pair.hard = value;

                if (soft || !hard)
                        pair.soft = value;

                if (!shell_limit_write(chosen->resource, address_of pair))
                {
                        shell_answer(2);

                        return string_format(shell_diagnostic,
                                             "ulimit: error setting limit\n");
                }
        }

        shell_answer(0);
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
