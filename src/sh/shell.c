/*
        The shell.

        Parsing, expansion, redirection and dispatch. The commands themselves
        are in builtin.c beside this, and programs/shell.c is the four lines
        that start it -- so the same core can be driven by something that is
        not a program, which is what an in-kernel console would need.
*/

/*
        Control-C cancels the command, not the shell.

        The line discipline sends SIGINT to everything in the terminal's
        foreground group, which is this and whatever it is running. Ignoring it
        here leaves the shell standing; every command it runs is given its own
        default disposition back first, whether it is spawned or is a builtin
        in a fork of this process.

        SIG_IGN needs no restorer, which is the whole reason this is three
        words and not a per-architecture trampoline.
*/
#define SIGNAL_INTERRUPT 2
#define SIGNAL_QUIT 3
#define SIGNAL_IGNORE 1
#define SIGNAL_DEFAULT 0

#define SIGNAL_RESTART 0x10000000
#define SIGNAL_RESTORER 0x04000000

fn shell_signal(b32 number, positive disposition)
{
        positive action[4] = {disposition, 0, 0, 0};

        system_call_4(syscall(rt_sigaction), number, (positive)address_of action, 0, 8);
}

#define shell_ignore(n) shell_signal(n, SIGNAL_IGNORE)
#define shell_default(n) shell_signal(n, SIGNAL_DEFAULT)

/*
        What was already ignored when this shell started.

        A non-interactive shell that inherits SIG_IGN for a signal keeps
        ignoring it: a trap on it does nothing, and neither does giving it
        back. The shell that started this one decided, and a script must not
        be able to undo that decision from the inside -- which is what makes a
        command run under nohup, or in the background, stay uninterruptible
        however it sets its own traps.

        Asked before this shell installs a disposition of its own, because
        that is the only moment the answer is about what was inherited rather
        than about what we just did.
*/
positive shell_signals_ignored;
static positive shell_signals_known;

static bool shell_signal_was_ignored(b32 number)
{
        positive mask;

        if (number <= 0 || number >= (b32)positive_bits)
                return false;

        mask = (positive)1 << number;

        if (!(shell_signals_known & mask))
        {
                positive action[4] = {0, 0, 0, 0};

                if (system_call_4(syscall(rt_sigaction), number, 0,
                                  (positive)address_of action, 8) >= 0 &&
                    action[0] == SIGNAL_IGNORE)
                        shell_signals_ignored |= mask;

                shell_signals_known |= mask;
        }

        return (shell_signals_ignored & mask) != 0;
}

#define shell_was_ignored(n) shell_signal_was_ignored((b32)(n))

fn shell_signals_start()
{
        positive ignored[4] = {SIGNAL_IGNORE, 0, 0, 0};
        b32 numbers[2] = {SIGNAL_INTERRUPT, SIGNAL_QUIT};

        /* Installing SIG_IGN can return the inherited action in the same
           call. These are the only two dispositions changed at entry; every
           other signal remains queryable when trap first touches it. */
        for (positive at = 0; at < 2; at++)
        {
                positive old[4] = {0, 0, 0, 0};
                b32 number = numbers[at];
                positive mask = (positive)1 << number;

                if (system_call_4(syscall(rt_sigaction), number,
                                  (positive)address_of ignored,
                                  (positive)address_of old, 8) >= 0)
                {
                        shell_signals_known |= mask;

                        if (old[0] == SIGNAL_IGNORE)
                                shell_signals_ignored |= mask;
                }
        }
}

// Set where the signal landed, read where a command ends. A handler that ran
// the action itself would be running the parser on top of whatever the parser
// was already in the middle of.
fn trap_signal_caught(b32 number);

#if defined(__x86_64__) || defined(_M_X64)

/*
        Where the handler goes when it is done.

        x86_64 is the one machine with no return trampoline of its own: the
        kernel jumps to sa_restorer, and what is there has to call
        rt_sigreturn. arm64 and riscv64 have one and are given none, which is
        the whole of the difference and the reason the flag below is set on
        one architecture and not the other two.
*/
asm(".text\n"
    ".globl shell_signal_return\n"
    "shell_signal_return:\n"
    "        movl $" MOONWATER_NUMBER(syscall(rt_sigreturn)) ", %eax\n"
    "        syscall\n");

fn shell_signal_return();

#define SIGNAL_CATCH_FLAGS (SIGNAL_RESTART | SIGNAL_RESTORER)
#define SIGNAL_CATCH_RESTORER ((positive)shell_signal_return)

#else

#define SIGNAL_CATCH_FLAGS SIGNAL_RESTART
#define SIGNAL_CATCH_RESTORER 0

#endif

/*
        A signal the script asked to hear about.

        Restarting, so that a wait for a child is not cut short by a signal
        the shell is only noting down: the action runs when the command it
        interrupted has finished, which is where POSIX says it runs.
*/
fn shell_catch(b32 number)
{
        positive action[4] = {(positive)trap_signal_caught, SIGNAL_CATCH_FLAGS,
                              SIGNAL_CATCH_RESTORER, 0};

        system_call_4(syscall(rt_sigaction), number, (positive)address_of action, 0, 8);
}

// Whether anybody is watching. A script and a terminal want different
// things of a shell that has just been told to do something impossible.
b32 shell_is_interactive;

// Whether output that can carry colour does. An interface that draws its own
// screen turns it off while it holds the terminal.
bool shell_styles = true;


/*
        Nothing the shell builds has a ceiling.

        This is init. It runs on a machine that may have a hundred thousand
        files in a directory, and a glob that quietly stops at the sixty
        fourth of them is worse than one that fails outright, because the
        script carries on believing it saw everything. Every list a script can
        make longer therefore grows, and the only thing that ends it is the
        kernel refusing more memory.

        Two shapes are needed, and the difference between them is whether
        anything is holding a pointer into the thing while it grows.

        A table of pointers may move. Nothing keeps an address inside one --
        it is always reached by index -- so it grows by taking a larger
        mapping, copying, and giving the old one back.

        The bytes those pointers point AT may never move, because argv entries
        are addresses into them and are handed to execve. So a byte store is a
        chain of blocks instead: when a block is full the next one is spliced
        on and everything already given out stays exactly where it was. A line
        ends by rewinding to the first block rather than freeing, so the steady
        state of a shell in a loop is no allocation at all.
*/

typedef struct shell_block
{
        struct shell_block address_to next;
        positive size;
        positive used;
} shell_block;

typedef struct
{
        shell_block address_to head;
        shell_block address_to here;
} shell_store;

#define SHELL_BLOCK 65536

static bool shell_memory_failed;

static address_any shell_map(positive size)
{
        address_any got = memory(size);

        //      memory answers with the kernel's own negative errno, which as
        //      an address is the top page of the space and never a mapping.
        if (!got || (positive)got >= (positive)-4095)
        {
                shell_memory_failed = true;
                return null;
        }

        return got;
}

//      Room for want entries of unit bytes each, moving the table if it must.
static bool shell_room(address_any address_to held, positive address_to have,
                       positive want, positive unit)
{
        if (memory_reserve(held, have, *have, want, unit, 64))
                return true;

        shell_memory_failed = true;
        return false;
}

//      Bytes that will not move for as long as the line lasts.
static p8 address_to shell_store_take(shell_store address_to store, positive room)
{
        shell_block address_to block;
        positive size;
        p8 address_to bytes;

        if (!room)
                room = 1;

        if (store->here && store->here->used + room <= store->here->size)
        {
                bytes = (p8 address_to)(store->here + 1) + store->here->used;
                store->here->used += room;
                return bytes;
        }

        //      A block left over from an earlier line, still big enough.
        if (store->here && store->here->next && room <= store->here->next->size)
        {
                store->here = store->here->next;
                store->here->used = room;
                return (p8 address_to)(store->here + 1);
        }

        size = memory_growth(0, room, SHELL_BLOCK);

        if (!size)
        {
                shell_memory_failed = true;
                return null;
        }

        block = (shell_block address_to)shell_map(size + sizeof(shell_block));

        if (!block)
                return null;

        block->size = size;
        block->used = room;
        block->next = null;

        if (store->here)
        {
                block->next = store->here->next;
                store->here->next = block;
        }
        else
        {
                store->head = block;
        }

        store->here = block;

        return (p8 address_to)(block + 1);
}

//      The line is over. Rewind rather than free: the next line will want the
//      same blocks, and a shell in a loop should stop allocating entirely.
static fn shell_store_reset(shell_store address_to store)
{
        store->here = store->head;

        if (store->here)
                store->here->used = 0;
}

/*
        A point in the store, and the way back to it.

        Some of this is built speculatively -- a command's assignments are kept
        while its words are expanded and thrown away if the line turns out not
        to run -- so the store has to be able to give back everything taken
        since a moment, without giving back what was there before it.
*/
typedef struct
{
        shell_block address_to block;
        positive used;
} shell_mark;

static shell_mark shell_store_mark(shell_store address_to store)
{
        shell_mark mark;

        mark.block = store->here;
        mark.used = store->here ? store->here->used : 0;

        return mark;
}

static fn shell_store_rewind(shell_store address_to store, shell_mark mark)
{
        if (!mark.block)
        {
                shell_store_reset(store);
                return;
        }

        store->here = mark.block;
        store->here->used = mark.used;
}

/*
        A list of words that grows as words are put in it.

        This is what argv is made of and what a field split fills, so it is the
        one place a glob's answer stops being bounded by anything.
*/
typedef struct
{
        string_address address_to address_to word;
        positive address_to room;
        positive count;
} shell_words;

//      Bound to wherever the table actually lives, so the same appending code
//      serves argv, a for loop's list and set's arguments alike.
static bool shell_words_add(shell_words address_to list, string_address word)
{
        if (!shell_room((address_any address_to)list->word, list->room,
                        list->count + 2, sizeof(string_address)))
                return false;

        (address_to list->word)[list->count++] = word;
        (address_to list->word)[list->count] = null;

        return true;
}

static fn shell_words_bind(shell_words address_to list,
                           string_address address_to address_to table,
                           positive address_to room)
{
        list->word = table;
        list->room = room;
        list->count = 0;
}

#include "lex.c"
#include "file.c"
#include "text.c"
#include "awk.c"
#include "tools.c"
#include "net.c"
#include "expand.c"
#include "../canvas/window.c"
#include "term.c"
#include "screen.c"
#include "edit.c"
#include "system.c"
#include "builtin.c"

#define PROMPT TERM_RESET TERM_BOLD " $ " TERM_RESET

/*
        The line being read, which grows to hold whatever arrives.

        This was four kilobytes, and a line longer than that was run truncated
        -- not refused, run -- which is the same silent wrongness the word
        lists had. A generated script with one very long line is the ordinary
        way to meet it.
*/
#define MAX_INPUT_STEP 4096

p8 address_to shell_buffer;
positive shell_buffer_room;

bool shell_output_failed;

static fn shell_write(address_any data, positive length)
{
        log(data, length);
}

writer shell_output = shell_write;
positive shell_output_file;

fn redirect_writer(address_any data, positive length)
{
        positive wrote;

        if (!shell_output_file)
        {
                shell_output_failed = true;
                return string_format(shell_output, "Redirection error file not open\n");
        }

        if (length == 0)
                length = string_length(data);

        wrote = system_write_all(shell_output_file, data, length);

        if (wrote != length)
                shell_output_failed = true;
}


/*
        A line becomes argv here and nowhere else, so the builtin path and the
        spawn path see the same words. Expansion can make a line longer than it
        was read at, so the words get storage of their own instead of being cut
        out of shell_buffer in place.
*/
//      The bytes the words of a line are cut into. It grows with the line,
//      and because a word's address is taken only after the word is finished
//      it is safe for the block to move while one is being built.
p8 address_to token_storage;
positive token_storage_room;
positive token_used;
bool token_overflow;

static bool token_room(positive want)
{
        return shell_room((address_any address_to)address_of token_storage,
                          address_of token_storage_room, want, 1);
}

//      argv and its parallel operator marks grow with the line. Nothing holds
//      an address inside either table, so both may move as they grow.
string_address address_to shell_argv;
positive shell_argv_room;
positive shell_argc;

/* The executor supplies these only when argv[0] came unchanged from a parsed
   literal. They let repeated loop commands reuse dispatch work without ever
   trusting the address of lexer storage, which is reused between lines. */
static bool shell_command_name_stable;
static string_address shell_command_name_address;
static positive shell_command_name_length;

// Which words arrived as a bare > or >>. A quoted ">" is a file name and must
// not be mistaken for the operator, and by then the two look identical.
bool address_to shell_operator;
positive shell_operator_room;

p8 address_to argument_line;
positive argument_line_room;

fn token_push(p8 value)
{
        if (!token_room(token_used + 2))
        {
                token_overflow = true;
                return;
        }

        token_storage[token_used++] = value;
}

static fn token_push_bytes(address_any data, positive length)
{
        if (!token_room(token_used + length + 2))
        {
                token_overflow = true;
                return;
        }

        if (length)
                memory_copy_apart(token_storage + token_used, data, length);

        token_used += length;
}

fn token_push_string(string_address text)
{
        if (text)
                token_push_bytes(text, string_length(text));
}

bool shell_name_character(p8 value)
{
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') ||
               value == '_';
}

/* Assignment syntax is a property of the parsed word, not of an execution.
   Return one for NAME= and two for NAME+=, together with the stable name
   length so the executor can reuse its hash metadata. */
static p8 shell_assignment_kind(string_address word,
                                positive address_to name_length)
{
        positive length = 0;

        while (shell_name_character(string_get(word + length)))
                length++;

        if (name_length)
                address_to name_length = length;

        if (!length || (string_get(word) >= '0' && string_get(word) <= '9'))
                return 0;

        if (string_get(word + length) == '=')
                return 1;

        return string_get(word + length) == '+' &&
                       string_get(word + length + 1) == '='
                   ? 2
                   : 0;
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

        /*
                The special parameters, which are not names and so never
                matched the loop above. $? is the one everything reaches for.
        */
        if (!length)
        {
                p8 special = string_get(step);

                if (special == '?')
                {
                        positive_to_string(token_push_bytes, (positive)shell_status);

                        return step + 1;
                }

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

                //      Room for this word and the null that ends the list.
                if (!shell_room((address_any address_to)address_of shell_argv,
                                address_of shell_argv_room,
                                (positive)count + 2, sizeof(string_address)) ||
                    !shell_room((address_any address_to)address_of shell_operator,
                                address_of shell_operator_room,
                                (positive)count + 2, sizeof(bool)))
                        return -1;

                positive token_at = token_used;
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
                shell_argv[count] = token_storage + token_at;
                count++;
        }

        if (!shell_room((address_any address_to)address_of shell_argv,
                        address_of shell_argv_room, (positive)count + 2,
                        sizeof(string_address)) ||
            !shell_room((address_any address_to)address_of shell_operator,
                        address_of shell_operator_room, (positive)count + 2,
                        sizeof(bool)))
                return -1;

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

                if (!shell_room((address_any address_to)address_of argument_line,
                                address_of argument_line_room,
                                used + length + 2, 1))
                        break;

                if (used)
                        argument_line[used++] = ' ';

                memory_copy(argument_line + used, shell_argv[index], length);
                used += length;
                index++;
        }

        if (!shell_room((address_any address_to)address_of argument_line,
                        address_of argument_line_room, used + 1, 1))
                return null;

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

                // Include the argv null and its matching false operator slot.
                positive moving = shell_argc - index - 1;

                memory_copy(shell_argv + index, shell_argv + index + 2,
                            moving * sizeof(shell_argv[0]));
                memory_copy(shell_operator + index, shell_operator + index + 2,
                            moving * sizeof(shell_operator[0]));

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
        if (!shell_was_ignored(SIGNAL_INTERRUPT))
                shell_default(SIGNAL_INTERRUPT);

        if (!shell_was_ignored(SIGNAL_QUIT))
                shell_default(SIGNAL_QUIT);

        // The child owns its own descriptors, so a redirection lands here and
        // never touches the shell's own output.
        if (shell_output_file)
                system_call_3(syscall(dup3), shell_output_file, standard_output_descriptor, 0);

        bipolar exec_result = shell_exec_file(shell_argv[0], shell_argv,
                                              shell_argc,
                                              shell_environment());

        string_format(shell_output, "failed with error: %b\n", exec_result);
        log_flush();

        exit(126);
}

// Opened once at startup. Spawning through it skips the fork whose address
// space copy execve would only throw away: about 3us per command here.
// Negative means the kernel has no spark device and we fall back to forking.
b32 spawn_device = -1;
static bool spawn_device_opened;

/*
        argv and envp go across as flat blocks of NUL terminated strings.

        Both grow to hold what is being run. They used to be four kilobytes
        each and a command whose words came to more than that was refused --
        which the caller answers by forking instead, so nothing broke, but a
        long command line quietly stopped taking the fast path. What made it
        visible was the line reader growing first: a twenty thousand byte
        echo went from being cut in half to being refused here.
*/
p8 address_to spawn_argv_block;
positive spawn_argv_room;
p8 address_to spawn_envp_block;
positive spawn_envp_room;

//      The environment, flattened into a block that is made to fit it.
positive shell_flatten_env(positive address_to count_out)
{
        string_address address_to environment = shell_environment();
        positive used = 0;
        positive count = 0;
        positive index = 0;

        while (environment[index])
                used += string_length(environment[index++]) + 1;

        if (!shell_room((address_any address_to)address_of spawn_envp_block,
                        address_of spawn_envp_room, used + 1, 1))
        {
                address_to count_out = 0;
                return 0;
        }

        used = 0;

        while (environment[count])
        {
                positive length = string_length(environment[count]) + 1;

                memory_copy(spawn_envp_block + used, environment[count], length);
                used += length;
                count++;
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

        //      How much the words come to, before any of them is copied.
        while (index < shell_argc)
                used += string_length(shell_argv[index++]) + 1;

        if (!shell_room((address_any address_to)address_of spawn_argv_block,
                        address_of spawn_argv_room, used + 1, 1))
                return -1;

        used = 0;

        for (index = 0; index < shell_argc; index++)
        {
                positive length = string_length(shell_argv[index]) + 1;

                memory_copy(spawn_argv_block + used, shell_argv[index], length);
                used += length;
        }

        request.path = (unsigned long)shell_argv[0];
        request.argv = (unsigned long)spawn_argv_block;
        request.argv_bytes = used;
        request.argv_count = shell_argc;
        request.envp = (unsigned long)spawn_envp_block;
        request.envp_bytes = shell_flatten_env(address_of envc);
        request.envp = (unsigned long)spawn_envp_block;
        request.envp_count = envc;

        return system_call_3(syscall(ioctl), spawn_device,
                             SPARK_IOCTL_SPAWN_SHELL,
                             (positive)address_of request);
}

fn shell_execute_command()
{
        bipolar child = -1;
        bipolar saved_output = -1;

        if (!spawn_device_opened)
        {
                spawn_device = system_call_4(syscall(openat), AT_FDCWD,
                                             (positive)SPARK_DEVICE,
                                             FILE_READ_WRITE, 0);
                spawn_device_opened = true;
        }

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
                saved_output = system_call_1(syscall(dup), standard_output_descriptor);

                if (saved_output >= 0)
                        system_call_3(syscall(dup3), shell_output_file, standard_output_descriptor, 0);
        }

        if (spawn_device >= 0 && (!shell_output_file || saved_output >= 0))
                child = shell_spawn_via_device();

        if (saved_output >= 0)
        {
                system_call_3(syscall(dup3), saved_output, standard_output_descriptor, 0);
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
                system_wait4_retry(child, address_of status, 0, null);
                shell_status = wait_status_code(status);

                /*
                        An exit byte is not an execve error channel.

                        A program is allowed to answer 127 itself, and its wait
                        status is bit-for-bit identical to a loader choosing
                        that number. The portable child diagnoses its own
                        failed execve before exiting; guessing here accused a
                        successfully run `/bin/sh -c 'exit 127'` of not having
                        run at all.
                */
        }
        else
                string_format(shell_output, "failed with error: %b\n", child);

        log_flush();
}

bool shell_builtin(string_address arguments)
{
        static shell_command address_to remembered;
        static positive remembered_length;
        shell_command address_to command = null;

        if (!arguments && shell_command_name_stable &&
            shell_argv[0] == shell_command_name_address && remembered &&
            remembered_length == shell_command_name_length &&
            !memory_compare(shell_argv[0], remembered->name,
                            remembered_length))
                command = remembered;
        else
        {
                command = shell_command_named(shell_argv[0]);

                if (!arguments && shell_command_name_stable &&
                    shell_argv[0] == shell_command_name_address && command)
                {
                        remembered = command;
                        remembered_length = shell_command_name_length;
                }
        }

        if (command)
        {

                /* Only these two old builtins still consume a rejoined line.
                   Everyone else reads argv directly, so do not scan and copy
                   every argument before every ordinary builtin. */
                if (!arguments &&
                    (command->function == shell_mount ||
                     command->function == shell_which))
                        arguments = shell_arguments();

                /*
                        A builtin that finishes without an opinion succeeded.

                        The status used to be left exactly as the previous
                        command set it, so "false; echo hi" reported failure
                        and a script whose last act was a successful echo
                        exited non-zero. Answering here rather than in each
                        builtin means the ones that cannot fail do not have to
                        remember to say they did not.
                */
                shell_status_entering = shell_status;
                shell_status = 0;

                command->function(shell_output, arguments);
                return true;
        }

        // Commands and utilities are disjoint tables.  A script executes the
        // former far more often, so do not make every echo, test, read and
        // printf pay for a guaranteed miss through all of the utility index.
        if (shell_tool_run(shell_argv[0]))
                return true;

        return false;
}

#include "parse.c"
#include "exec.c"

/*
        Whether the shell is in the middle of something.

        The reader hands over one line at a time and a while loop is not one
        line. When the parser runs out of tokens inside a construct it says so
        rather than failing, the tokens are kept, and the next line is added to
        them -- which is also how a here-document body is collected, except
        that a body is not source and is taken verbatim until its delimiter.
*/
static bool shell_more;

b32 shell_incomplete()
{
        return shell_more;
}

static fn run_line_inner(string_address line)
{
        string_address waiting = parse_here_open();
        b32 root;

        // A nested eval or sourced file can hand over more physical lines
        // after one of them failed expansion. They belong to the same outer
        // input line and none may restart execution underneath the failure.
        if (exec_line_aborted())
                return;

        if (waiting)
        {
                if (!string_compare(line, waiting))
                        parse_here_close();
                else
                        parse_here_line(line);
        }
        else if (!parse_feed(line))
        {
                string_format(exec_error, "Command line too long\n");
                parse_reset();
                shell_more = false;
                return;
        }

        if (parse_here_open())
        {
                shell_more = true;
                return;
        }

        root = parse_program();

        if (parse_state == PARSE_INCOMPLETE)
        {
                shell_more = true;
                return;
        }

        shell_more = false;

        if (parse_state)
        {
                string_format(exec_error, "Syntax error\n");
                shell_status = 2;
                parse_reset();
                return;
        }

        exec_program(root);
        parse_reset();
        shell_expand_reset();

        // A signal that arrived while the shell was reading rather than
        // running has no command boundary of its own to wait for.
        if (!exec_line_aborted())
                exec_traps();

        /*
                A terminal wants each line the moment it happens. A script does
                not, and flushing per line is one write system call per line of
                it -- which is where the time in a forty thousand line script
                went. The buffer drains when it fills, before anything is
                spawned, and when the input ends.
        */
        if (shell_is_interactive)
                log_flush();
}

/*
        Top-level lines recover from an interactive expansion error; nested
        lines are part of the command that failed and keep carrying it.

        Keeping the depth around the entire executor call is what makes an
        eval or a multi-line dot script stop, while the next line read from the
        terminal gets a clean execution signal and still sees $? == 2.
*/
static positive shell_run_depth;

fn run_line(string_address line)
{
        bool top = !shell_run_depth;

        // Hold the depth while recovery dispatches a pending trap. Its action
        // is a nested line and must not recursively begin another top-level
        // recovery before this one has reached the user's next command.
        shell_run_depth++;

        if (top)
                exec_line_begin();

        run_line_inner(line);
        shell_run_depth--;
}

/*
        No next line exists.

        An incomplete parse means "ask the reader for more" only while the
        reader can still answer. At EOF it is a syntax error. Nested readers
        (eval and dot) are special builtins, so their syntax error aborts the
        containing non-interactive shell rather than quietly returning to the
        outer line.
*/
fn shell_input_end()
{
        if (!shell_more)
                return;

        if (parse_eof_can_complete())
        {
                run_line_inner((string_address) "");

                if (!shell_more)
                        return;
        }

        string_format(exec_error, "Syntax error: unexpected end of input\n");
        parse_reset();
        shell_more = false;
        shell_status = 2;

        if (shell_run_depth)
                expand_fatal();
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
