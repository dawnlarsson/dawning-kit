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
#define SIGNAL_PIPE 13
#define SIGNAL_IGNORE 1
#define SIGNAL_DEFAULT 0

#define SIGNAL_RESTART 0x10000000
#define SIGNAL_RESTORER 0x04000000

fn shell_signal(b32 number, positive disposition)
{
        system_signal_install(number, disposition, 0, 0, null);
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

                if (system_signal_action(number, 0, address_of action, 8) >= 0 &&
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

                if (system_signal_action(number,
                                  address_of ignored,
                                  address_of old, 8) >= 0)
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

#define SIGNAL_CATCH_RESTORER ((positive)shell_signal_return)
#define SIGNAL_CATCH_FLAGS (SIGNAL_RESTART | SIGNAL_RESTORER)

#else

#define SIGNAL_CATCH_RESTORER 0
#define SIGNAL_CATCH_FLAGS SIGNAL_RESTART

#endif

/*
        A signal the script asked to hear about.

        Restarting, so that a wait for a child is not cut short by a signal
        the shell is only noting down: the action runs when the command it
        interrupted has finished, which is where POSIX says it runs.
*/
fn shell_catch_mode(b32 number, bool restart)
{
        positive flags = SIGNAL_CATCH_FLAGS;

        if (!restart)
                flags &= ~SIGNAL_RESTART;

        system_signal_install(number, (positive)trap_signal_caught, flags,
                              SIGNAL_CATCH_RESTORER, null);
}

fn shell_catch(b32 number)
{
        shell_catch_mode(number, true);
}

// Whether anybody is watching. A script and a terminal want different
// things of a shell that has just been told to do something impossible.
b32 shell_is_interactive;

/* One implementation, two conflicting shell policies. The standalone entry
   selects Bash policy only when invoked as bash; sh/dash and embedded callers
   retain Moonwater's existing dash-compatible defaults. */
bool shell_bash_compat;
bool shell_dash_compat;

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
/* A command which needed more than this keeps its expansion buffers for the
   next command, so repeated large work remains allocation-free. If the next
   command uses no more than this much, the old high-water mapping has proved
   to be a one-off and is returned to the kernel at that command boundary. */
#define SHELL_SCRATCH_RETAIN (1u << 20)

static bool shell_memory_failed;
/* Set by a completed exceptional expansion and cleared only when the complete
   top-level command ends. A compound command may finish on a tiny `:` after
   doing large work; its final expansion is not its working set. */
static bool shell_large_request;

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

static inline INLINE fn shell_scratch_bytes(positive want)
{
        if (!shell_large_request && want > SHELL_SCRATCH_RETAIN)
                shell_large_request = true;
}

// Most growing stores carry their width in their pointed-to type. Keep the
// cast and sizeof at this floor instead of repeating both at every caller.
#define shell_array_room(array, room, want)                                  \
        shell_room((address_any address_to)address_of (array),               \
                   address_of (room), (want), sizeof((array)[0]))

// Parallel byte stores share one capacity decision at their call sites.
#define shell_byte_pair_room(first, first_room, second, second_room, want)    \
        (shell_room((address_any address_to)address_of (first),               \
                    address_of (first_room), (want), 1) &&                    \
         shell_room((address_any address_to)address_of (second),              \
                    address_of (second_room), (want), 1))

//      Bytes that will not move for as long as the line lasts.
static p8 address_to shell_store_take(shell_store address_to store, positive room)
{
        shell_block address_to block;
        shell_block address_to before;
        positive size;
        p8 address_to bytes;

        if (!room)
                room = 1;

        if (store->here && store->here->used <= store->here->size &&
            room <= store->here->size - store->here->used)
        {
                bytes = (p8 address_to)(store->here + 1) + store->here->used;
                store->here->used += room;
                return bytes;
        }

        /* A block left over from an earlier line, still big enough.  Look
           through the released tail rather than only at its first block: an
           earlier large/small allocation order must not make an equally large
           block later in the chain invisible and map a duplicate beside it. */
        if (store->here && store->here->next &&
            room <= store->here->next->size)
        {
                store->here = store->here->next;
                store->here->used = room;
                return (p8 address_to)(store->here + 1);
        }

        if (store->here)
        {
                before = store->here->next;
                block = before ? before->next : null;
        }
        else
        {
                before = null;
                block = store->head;
        }

        while (block && room > block->size)
        {
                before = block;
                block = block->next;
        }

        if (block)
        {
                if (store->here)
                {
                        before->next = block->next;
                        block->next = store->here->next;
                        store->here->next = block;
                }
                else if (block != store->head)
                {
                        before->next = block->next;
                        block->next = store->head;
                        store->head = block;
                }

                store->here = block;
                block->used = room;
                return (p8 address_to)(block + 1);
        }

        size = memory_growth(0, room, SHELL_BLOCK);

        if (!size || size > positive_max - sizeof(shell_block))
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

/* Moving expansion arrays use a one-command grace before returning a stale
   high-water mapping. Chained stores keep every block: their many-small-word
   aggregate cannot be inferred from any one completed expansion without
   adding bookkeeping to the allocator hot path. */
static fn shell_room_relax(address_any address_to held,
                           positive address_to have, positive active,
                           positive unit)
{
        positive limit;

        if (!unit)
                return;

        limit = SHELL_SCRATCH_RETAIN / unit;

        if (address_to have <= limit || active > limit)
                return;

        memory_free(address_to held, address_to have * unit);
        address_to held = null;
        address_to have = 0;
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

static PURE shell_mark shell_store_mark(shell_store address_to store)
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

/* Set only in a process created solely to run one simple command. Utilities
   can then use that process directly instead of cloning a disposable wrapper
   around another disposable child. */
static bool shell_tail_command;
// The substitution child grants that privilege after parsing proves the line
// is one simple command; functions decline it later in exec_dispatch.
static bool shell_tail_line_requested;
fn shell_thread_instance_mode(bool preserve_ignored);

// More than one line of source, run one at a time: what a trap action, eval
// and dot hand over. Defined beside run_line below, after the executor.
fn run_lines(string_address text);

/* The mount table is shared by file and storage utilities. Its parser lives
   in storage_discovery.c, while this declaration keeps consumers independent
   of source inclusion order. */
typedef struct
{
        positive id;
        positive parent_id;
        string_address device;
        string_address root;
        string_address target;
        string_address options;
        string_address type;
        string_address source;
        string_address filesystem_options;
} storage_mount;

typedef struct
{
        byte_store text;
        storage_mount address_to entry;
        positive entry_room;
        positive count;
} storage_mount_table;

bool storage_mount_table_load(storage_mount_table address_to table,
                              writer diagnostic);
fn storage_mount_table_release(storage_mount_table address_to table);

/*
        shopt: the option namespace set does not carry.

        A script that opens with `shopt -s nullglob` must not die on the name,
        so every option Bash 5.3 has is stored whether or not anything reads
        it -- a stored no-op keeps the script running, which is the whole
        point of the builtin for the fifty-odd names nothing here acts on.

        One bit each in a single word, because the readers are the ones that
        cannot afford a lookup: pathname expansion asks about four of these
        for every pattern it walks, and the matcher asks about a fifth for
        every case arm. A bit test against a global is what that costs.

        The table is here rather than beside the builtin because the readers
        are spread across the expander, the matcher and the executor, and all
        three are included before builtin.c is.
*/
static string_address shell_shopt_names[] = {
    "array_expand_once", "assoc_expand_once", "autocd",
    "bash_source_fullpath", "cdable_vars", "cdspell", "checkhash",
    "checkjobs", "checkwinsize", "cmdhist", "compat31", "compat32",
    "compat40", "compat41", "compat42", "compat43", "compat44",
    "complete_fullquote", "direxpand", "dirspell", "dotglob", "execfail",
    "expand_aliases", "extdebug", "extglob", "extquote", "failglob",
    "force_fignore", "globasciiranges", "globskipdots", "globstar",
    "gnu_errfmt", "histappend", "histreedit", "histverify", "hostcomplete",
    "huponexit", "inherit_errexit", "interactive_comments", "lastpipe",
    "lithist", "localvar_inherit", "localvar_unset", "login_shell",
    "mailwarn", "no_empty_cmd_completion", "nocaseglob", "nocasematch",
    "noexpand_translation", "nullglob", "patsub_replacement", "progcomp",
    "progcomp_alias", "promptvars", "restricted_shell", "shift_verbose",
    "sourcepath", "varredir_close", "xpg_echo", null,
};

#define SHELL_SHOPT_NAMES (array_count(shell_shopt_names) - 1)

//      The position of a name in the table above is its bit. Only the names
//      something reads are spelled out; the rest are reached by lookup.
#define SHELL_SHOPT_CHECKWINSIZE 8
#define SHELL_SHOPT_CMDHIST 9
#define SHELL_SHOPT_COMPLETE_FULLQUOTE 17
#define SHELL_SHOPT_DOTGLOB 20
#define SHELL_SHOPT_EXECFAIL 21
#define SHELL_SHOPT_EXPAND_ALIASES 22
#define SHELL_SHOPT_EXTGLOB 24
#define SHELL_SHOPT_EXTQUOTE 25
#define SHELL_SHOPT_FAILGLOB 26
#define SHELL_SHOPT_FORCE_FIGNORE 27
#define SHELL_SHOPT_GLOBASCIIRANGES 28
#define SHELL_SHOPT_GLOBSKIPDOTS 29
#define SHELL_SHOPT_GLOBSTAR 30
#define SHELL_SHOPT_HOSTCOMPLETE 35
#define SHELL_SHOPT_INHERIT_ERREXIT 37
#define SHELL_SHOPT_INTERACTIVE_COMMENTS 38
#define SHELL_SHOPT_LASTPIPE 39
#define SHELL_SHOPT_LOCALVAR_INHERIT 41
#define SHELL_SHOPT_LOGIN_SHELL 43
#define SHELL_SHOPT_NOCASEGLOB 46
#define SHELL_SHOPT_NOCASEMATCH 47
#define SHELL_SHOPT_NULLGLOB 49
#define SHELL_SHOPT_PATSUB_REPLACEMENT 50
#define SHELL_SHOPT_PROGCOMP 51
#define SHELL_SHOPT_PROMPTVARS 53
#define SHELL_SHOPT_SOURCEPATH 56
#define SHELL_SHOPT_SHIFT_VERBOSE 55
#define SHELL_SHOPT_XPG_ECHO 58

#define SHELL_SHOPT(which) ((positive)1 << SHELL_SHOPT_##which)

//      What Bash reports for a shell that was started to read a script. Every
//      other name begins off, which is why the word and not the table is the
//      one place the state lives.
#define SHELL_SHOPT_STARTED                                                  \
        (SHELL_SHOPT(CHECKWINSIZE) | SHELL_SHOPT(CMDHIST) |                  \
         SHELL_SHOPT(COMPLETE_FULLQUOTE) | SHELL_SHOPT(EXTQUOTE) |           \
         SHELL_SHOPT(FORCE_FIGNORE) | SHELL_SHOPT(GLOBASCIIRANGES) |         \
         SHELL_SHOPT(GLOBSKIPDOTS) | SHELL_SHOPT(HOSTCOMPLETE) |             \
         SHELL_SHOPT(INTERACTIVE_COMMENTS) |                                 \
         SHELL_SHOPT(PATSUB_REPLACEMENT) | SHELL_SHOPT(PROGCOMP) |           \
         SHELL_SHOPT(PROMPTVARS) | SHELL_SHOPT(SOURCEPATH))

positive shell_shopt_state = SHELL_SHOPT_STARTED;

#define shell_shopt_on(which)                                                \
        ((shell_shopt_state & SHELL_SHOPT(which)) != 0)

/*
        The names that answer without being stored.

        Declared here because the expander is included first and is where the
        miss that reaches them happens; what they answer with is beside the
        environment table in builtin.c, which is the only place that knows how
        a name is looked up in the first place.
*/
#if defined(__aarch64__) || defined(_M_ARM64)
#define MOONWATER_HOSTTYPE "aarch64"
#define MOONWATER_MACHTYPE "aarch64-unknown-linux-gnu"
#elif defined(__riscv)
#define MOONWATER_HOSTTYPE "riscv64"
#define MOONWATER_MACHTYPE "riscv64-unknown-linux-gnu"
#else
#define MOONWATER_HOSTTYPE "x86_64"
#define MOONWATER_MACHTYPE "x86_64-pc-linux-gnu"
#endif

COLD string_address shell_dynamic_value(const_string name, positive length,
                                        positive hash,
                                        positive address_to value_length);
COLD bool shell_dynamic_assign(const_string name, positive length,
                               const_string value);
COLD bool shell_dynamic_wanted(const_string name, positive length);

//      How many subshells deep this process is, which is what $BASH_SUBSHELL
//      is and the only thing a fork has to remember to say it.
extern positive shell_subshell_depth;

fn shell_last_argument_set(string_address word);
PURE bool shell_braceexpand_on();
PURE bool shell_posix_on();

//      The next live function out of the executor's table. The table is in
//      exec.c, which is included last; callers keep only the stable name.
string_address exec_function_next(positive address_to slot,
                                  bool address_to readonly);

#include "lex.c"
#include "file.c"
#include "snapshot.c"
#include "storage_blkid.c"
#include "storage_discovery.c"
#include "storage_mount.c"
#include "text.c"
#include "checksum.c"
#include "cksum.c"
#include "awk.c"
#include "tools.c"
#include "pty.c"
#include "process_tools.c"
#include "monitor.c"
#include "net.c"
#include "expand.c"
#include "../canvas/window.c"
#include "term.c"
#include "screen.c"
#include "edit.c"
#include "system.c"
#include "../bowl/runtime.c"
#define PROMPT TERM_RESET TERM_BOLD " $ " TERM_RESET

static positive shell_syntax_generation;

#include "builtin.c"

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

// Expansion and here-documents share this growable byte arena. A word's
// address is taken only after it is complete, so the block may move while the
// word is being built.
p8 address_to token_storage;
positive token_storage_room;
positive token_used;
bool token_overflow;

static bool token_room(positive want)
{
        return shell_room((address_any address_to)address_of token_storage,
                          address_of token_storage_room, want, 1);
}

// Nothing holds an address inside argv, so it may move as the line grows.
string_address address_to shell_argv;
positive shell_argv_room;
positive shell_argc;

/* The executor supplies these only when argv[0] came unchanged from a parsed
   literal. They let repeated loop commands reuse dispatch work without ever
   trusting the address of lexer storage, which is reused between lines. */
static bool shell_command_name_stable;
static string_address shell_command_name_address;
static positive shell_command_name_length;

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

/* Assignment syntax is a property of the parsed word, not of an execution.
   Return one for NAME= and two for NAME+=, together with the stable name
   length so the executor can reuse its hash metadata. */
static p8 shell_assignment_kind(string_address word,
                                positive address_to name_length)
{
        positive length = 0;

        while (expand_name_character(string_get(word + length)))
                length++;

        if (!length || (string_get(word) >= '0' && string_get(word) <= '9'))
        {
                if (name_length)
                        address_to name_length = length;

                return 0;
        }

        /* A subscript is part of the name being assigned to. a[i+1]=v and
           m[a key]=v each name one element, and what follows the closing
           bracket is what says whether this is an assignment at all. */
        if (string_get(word + length) == '[')
        {
                positive depth = 1;
                positive at = length + 1;

                while (string_get(word + at) && depth)
                {
                        p8 value = string_get(word + at);

                        // A bracket inside quoting closes nothing: m["a]b"]
                        // is one subscript and not a broken one.
                        if (value == '\\' && string_get(word + at + 1))
                                at++;
                        else if (value == '\'' || value == '"')
                        {
                                at++;

                                while (string_get(word + at) &&
                                       string_get(word + at) != value)
                                        at++;

                                if (!string_get(word + at))
                                        break;
                        }
                        else if (value == '[')
                                depth++;
                        else if (value == ']')
                                depth--;

                        at++;
                }

                if (!depth && at > length + 2)
                        length = at;
        }

        if (name_length)
                address_to name_length = length;

        if (string_get(word + length) == '=')
                return 1;

        return string_get(word + length) == '+' &&
                       string_get(word + length + 1) == '='
                   ? 2
                   : 0;
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

/*
        A command gets the default disposition back, unless somebody chose
        otherwise.

        Ignored signals cross execve, and this shell ignores interrupt so that
        control-C does not take it down with the command. Handing that
        deafness on would leave the command uninterruptible -- but the
        deafness is the shell's own, and two other decisions outrank it: the
        script's, made with trap '' on the signal, and that of whoever started
        this shell with the signal already ignored. Both mean "the commands
        too", and giving the default back over either made a subshell or a
        spawned program the one thing in the script control-C could reach.
*/
static fn shell_child_default(b32 number)
{
        if (!trap_ignored((positive)number) && !shell_was_ignored(number))
                shell_default(number);
}

DEAD_END fn shell_thread_instance_mode(bool preserve_ignored)
{
        string_address address_to environment;

        if (!preserve_ignored)
        {
                shell_child_default(SIGNAL_INTERRUPT);
                shell_child_default(SIGNAL_QUIT);
        }

        environment = shell_environment();
        if (!environment)
        {
                string_format(log, "failed: no room for environment\n");
                log_flush();
                exit(126);
        }

        bipolar exec_result = shell_exec_file(shell_argv[0], shell_argv,
                                              shell_argc, environment);

        string_format(log, "failed with error: %b\n", exec_result);
        log_flush();

        exit(126);
}

DEAD_END fn shell_thread_instance()
{
        shell_thread_instance_mode(false);
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
positive spawn_envp_used;
positive spawn_envp_count;
positive spawn_envp_generation = positive_max;

/* A spawn vector has one wire shape whether it is argv or envp: a count and
   one packed run of terminated strings. Keep the overflow proof, sizing and
   copy together so the two launch paths cannot disagree about that shape. */
static positive shell_flatten_strings(string_address address_to strings,
                                      p8 address_to address_to block,
                                      positive address_to room,
                                      positive address_to count_out)
{
        positive count = 0;
        positive used = 0;

        while (strings[count])
        {
                positive length = string_length(strings[count++]);

                if (length == positive_max ||
                    used > positive_max - length - 1)
                {
                        address_to count_out = positive_max;
                        return 0;
                }

                used += length + 1;
        }

        if (used == positive_max ||
            !shell_room((address_any address_to)block, room, used + 1, 1))
        {
                address_to count_out = positive_max;
                return 0;
        }

        used = 0;

        for (positive at = 0; at < count; at++)
                used = (positive)(string_copy_end(address_to block + used,
                                                  strings[at]) -
                                    address_to block) + 1;

        address_to count_out = count;
        return used;
}

//      The environment, flattened into a block that is made to fit it.
positive shell_flatten_env(positive address_to count_out)
{
        string_address address_to environment = shell_environment();
        positive used;

        /* shell_environment already has a precise generation: it changes
           only when an exported value changes.  The flat ioctl block is an
           equally immutable view of that generation, so copying every byte
           again for every command was pure launch overhead. */
        if (!shell_envp_dirty &&
            spawn_envp_generation == shell_envp_generation)
        {
                address_to count_out = spawn_envp_count;
                return spawn_envp_used;
        }

        if (shell_envp_dirty)
        {
                address_to count_out = positive_max;
                return 0;
        }

        used = shell_flatten_strings(environment, address_of spawn_envp_block,
                                     address_of spawn_envp_room, count_out);
        if (address_to count_out == positive_max)
                return 0;

        spawn_envp_used = used;
        spawn_envp_count = address_to count_out;
        spawn_envp_generation = shell_envp_generation;

        return used;
}

static bool shell_spawn_device_open();

// Returns the child pid, or a negative error if the device could not take it.
static bipolar shell_spawn_via_device(b32 operation, string_address path,
                                      string_address address_to arguments,
                                      b32 output, b32 error)
{
        struct spawn_to directed;
        struct spawn address_to request = address_of directed.spawn;
        positive argc = 0;
        positive envc = 0;

        request->path = (unsigned long)path;
        request->argv_bytes = shell_flatten_strings(
            arguments, address_of spawn_argv_block,
            address_of spawn_argv_room, address_of argc);
        if (argc == positive_max)
                return -1;

        request->argv = (unsigned long)spawn_argv_block;
        request->argv_count = (unsigned int)argc;
        request->envp_bytes = shell_flatten_env(address_of envc);

        /* An allocation failure must take the fork/exec fallback.  Sending a
           syntactically valid request with envc zero silently stripped every
           exported variable from the child instead. */
        if (envc == positive_max)
                return -1;

        request->envp = (unsigned long)spawn_envp_block;
        request->envp_count = envc;
        request->envp_generation = spawn_envp_generation;
        directed.output = output;
        directed.error = error;

        return system_control(spawn_device, operation, address_of directed);
}

/*
        One pipeline stage, with its three descriptors named in the request.

        The difference from the launch above is the whole point of it: a
        stage used to be a forked copy of the shell that arranged its own
        descriptors and then replaced itself, and the copy was a page table
        duplicated for an address space the child discards microseconds
        later. Naming them here means the stage is spawned instead, and the
        fork never happens.

        A descriptor of -1 is left alone, so a stage at either end of the
        pipeline keeps the shell's own.
*/
bipolar shell_spawn_stage(string_address address_to arguments,
                          b32 input, b32 output, b32 error)
{
        struct spawn_into directed;
        struct spawn address_to request = address_of directed.spawn;
        positive argc = 0;
        positive envc = 0;

        if (!shell_spawn_device_open())
                return -1;

        request->path = (unsigned long)arguments[0];
        request->argv_bytes = shell_flatten_strings(
            arguments, address_of spawn_argv_block,
            address_of spawn_argv_room, address_of argc);

        if (argc == positive_max)
                return -1;

        request->argv = (unsigned long)spawn_argv_block;
        request->argv_count = (unsigned int)argc;
        request->envp_bytes = shell_flatten_env(address_of envc);

        if (envc == positive_max)
                return -1;

        request->envp = (unsigned long)spawn_envp_block;
        request->envp_count = envc;
        request->envp_generation = spawn_envp_generation;
        directed.input = input;
        directed.output = output;
        directed.error = error;

        return system_control(spawn_device, SPARK_IOCTL_SPAWN_SHELL_INTO,
                              address_of directed);
}

static bool shell_spawn_device_open()
{
        if (!spawn_device_opened)
        {
                spawn_device = system_open_at(AT_FDCWD,
                                             SPARK_DEVICE,
                                             FILE_READ_WRITE);
                spawn_device_opened = true;
        }

        return spawn_device >= 0;
}

/* argv[0] selects a utility in the kernel-owned /shell image. */
bipolar shell_spawn_tool(string_address address_to arguments,
                         b32 output, bool quiet)
{
        static b32 null_output = -1;

        if (!shell_spawn_device_open())
                return -1;

        if (quiet && null_output < 0)
                null_output = system_open_at(AT_FDCWD,
                                             "/dev/null",
                                             FILE_READ_WRITE | O_CLOEXEC);

        if (quiet && null_output < 0)
                return -1;

        return shell_spawn_via_device(output < 0 ? SPARK_IOCTL_SPAWN_TOOL
                                                 : SPARK_IOCTL_SPAWN_TOOL_TO,
                                      null, arguments, output,
                                      quiet ? null_output : -1);
}

fn shell_execute_command()
{
        bipolar child = -1;

        log_flush();

        if (shell_spawn_device_open())
                child = shell_spawn_via_device(SPARK_IOCTL_SPAWN_SHELL,
                                               shell_argv[0], shell_argv,
                                               -1, -1);

        if (child < 0)
        {
                // No spark device, or it refused the request: fall back to the
                // portable path so the shell still works on a stock kernel.
                //
                // clone takes (flags, child_stack, ...). Passing only flags
                // left child_stack as whatever happened to be in the second
                // argument register, so the child started on a garbage stack
                // and execve was handed an empty path.
                child = system_fork();

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
                string_format(log, "failed with error: %b\n", child);

        log_flush();
}

bool shell_builtin(string_address arguments, positive2 named)
{
        static shell_command address_to remembered;
        static positive remembered_length;
        shell_command address_to command = null;

        /* A slash is already a complete answer: neither a builtin nor an
           in-process utility has one in its name.  Direct executable paths
           are the startup-sensitive case and used to build both static name
           indexes only to prove two guaranteed misses. */
        if (string_first_of(shell_argv[0], '/'))
                return false;

        if (!arguments && shell_command_name_stable &&
            shell_argv[0] == shell_command_name_address && remembered &&
            remembered_length == shell_command_name_length &&
            !memory_compare(shell_argv[0], remembered->name,
                            remembered_length))
                command = remembered;
        else
        {
                command = shell_command_named_hashed(shell_argv[0], named);

                if (!arguments && shell_command_name_stable &&
                    shell_argv[0] == shell_command_name_address && command)
                {
                        remembered = command;
                        remembered_length = shell_command_name_length;
                }
        }

        if (command)
        {
                /* A normal builtin can evaluate nested commands and consumes
                   the one-command tail privilege. `command` keeps it until
                   its second lookup identifies the command it wraps; a tool
                   miss keeps it too. Keeping this beside the lookup avoids a
                   complete duplicate command-table probe in exec_dispatch. */
                if (command->function != shell_command_builtin)
                        shell_tail_command = false;

                /* This old builtin still consumes a rejoined line.  Everyone
                   else reads argv directly, so do not scan and copy every
                   argument before every ordinary builtin. */
                if (!arguments && command->function == shell_which)
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

                command->function(log, arguments);
                return true;
        }

        // Commands and utilities are disjoint tables.  A script executes the
        // former far more often, so do not make every echo, test, read and
        // printf pay for a guaranteed miss through all of the utility index.
        if (shell_tool_run_hashed(shell_argv[0], named))
                return true;

        return false;
}

#include "parse.c"
#include "exec.c"

/* The allocations below hold the moving expansion scratch for one top-level
   command. Small capacities remain the allocation-free steady state. A
   completed large expansion sets a command-wide grace: a second large
   command reuses the mappings, while the next small command proves their
   high-water capacity cold and returns it. Chained word stores and long-lived
   builtin streams retain their allocations; this boundary must not turn a
   repeated large-word or large read/printf/mapfile workload into churn. */
static fn shell_command_scratch_relax()
{
        positive array_active = shell_large_request
                                    ? SHELL_SCRATCH_RETAIN + 1
                                    : 0;

        /* Completed argv strings are dead even when their chained backing is
           retained for allocation-free reuse. */
        shell_expand_reset();

        shell_room_relax((address_any address_to)address_of expand_text,
                         address_of expand_text_room, array_active, 1);
        shell_room_relax((address_any address_to)address_of expand_mark,
                         address_of expand_mark_room, array_active, 1);
        shell_large_request = false;
}

/*
        Whether the shell is in the middle of something.

        The reader hands over one line at a time and a while loop is not one
        line. When the parser runs out of tokens inside a construct it says so
        rather than failing, the tokens are kept, and the next line is added to
        them -- which is also how a here-document body is collected, except
        that a body is not source and is taken verbatim until its delimiter.
*/
static bool shell_more;

//      Whether the parser is in the middle of a construct, which is the one
//      thing the reader needs to know to choose between PS1 and PS2.
bool shell_reading_more()
{
        return shell_more;
}

/*
        A syntax failure is scoped to the reader that encountered it.

        The process reader leaves on one, while eval and dot return it to the
        executor so POSIX special-builtin policy can distinguish a direct
        invocation from one behind command. A generation, rather than a
        sticky bit, lets nested readers notice only failures that happened
        inside their own input.
*/
static positive shell_run_depth;

static fn run_line_inner(string_address line)
{
        string_address waiting = parse_here_open();
        b32 root;

        // What a job made of a loop or a group is listed under: the words are
        // in the parse tree, but only the reader still has the line. And the
        // count of physical lines, which is the only place there is to keep
        // it: one is what a job is named after, the other what $LINENO reads.
        exec_current_line = line;

        // The lexer's own count of physical lines, which is where the line a
        // command was written on comes from; $LINENO reads the executor's
        // copy of that rather than this running total.
        shell_line_number++;

        // A nested eval or sourced file can hand over more physical lines
        // after one of them failed expansion. They belong to the same outer
        // input line and none may restart execution underneath the failure.
        if (exec_line_aborted())
                return;

        if (waiting)
                parse_here_line(line);
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

                shell_syntax_generation += 2;

                /* A terminal recovers at its next prompt. A direct script,
                   file, stdin stream or -c string has no enclosing builtin
                   to receive this error, so the rest of that input must not
                   run. Use the ordinary fatal boundary so an installed EXIT
                   trap is still honored. */
                if (shell_run_depth == 1 && !shell_source_depth)
                {
                        if (string_is(shell_option_flags, 'c'))
                                exec_child_leave(2);
                        if (!shell_is_interactive)
                                expand_fatal_status(2);
                }

                return;
        }

        if (!(shell_options & SHELL_FLAG('n')) || shell_is_interactive)
        {
                bool held_tail = shell_tail_command;

                if (shell_tail_line_requested && root &&
                    parse_nodes[root].kind == NODE_SIMPLE)
                        shell_tail_command = true;

                exec_program(root);
                shell_tail_command = held_tail;
        }
        parse_reset();

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

        /*
                The words a line made die with it -- the outer line's.

                A nested line runs while the command that started it is still
                standing on its own words in the same store: argv, and the
                assignments in front of it that have to be taken back when it
                is over. Resetting from inside a sourced file or a trap action
                handed those words to the next nested line to write over, and
                an export meant for one command was released by name from
                whatever had landed there instead.
        */
        if (top)
        {
                /* A physical line is not necessarily a command boundary. An
                   open quote, compound command or here-document keeps parser
                   state for the next line, and its capacity may be much
                   larger than the live suffix. Only a complete command owns
                   none of the scratch reclaimed above. Expansion retains the
                   old per-physical-line reset because nested execution may
                   have used it even while the outer parse remains open. */
                if (shell_more)
                        shell_expand_reset();
                else
                        shell_command_scratch_relax();
        }
}

/*
        More than one line, run one at a time.

        run_line is one physical line: the lexer stops at a newline, so a trap
        action or an eval argument with a second line lost everything after
        the first. The parser already joins the lines that belong together --
        an open quote, a substitution, a here-document body -- because that
        is how the reader feeds it, so this hands over the same physical lines
        the reader would and leaves the joining to the parser.

        The text is copied before it is cut up, because it may not be there
        by the time the second line runs: a trap action is the trap table's
        own copy and the first line is allowed to be "trap - USR1".
*/
fn run_lines(string_address text)
{
        p8 address_to copy = null;
        positive room = 0;
        positive length;
        string_address at;

        positive syntax = shell_syntax_generation;

        if (!string_first_of(text, '\n'))
        {
                run_line(text);
                return;
        }

        length = string_length(text);

        if (length == positive_max ||
            !shell_room((address_any address_to)address_of copy,
                        address_of room, length + 1, 1))
        {
                string_format(exec_error, "No room to run lines\n");
                shell_status = 2;
                return;
        }

        memory_copy(copy, text, length + 1);
        at = copy;

        while (string_get(at))
        {
                string_address stop = string_first_of_or_end(at, '\n');

                if (string_get(stop))
                {
                        address_to stop = end;
                        stop++;
                }

                // An empty line is a line: it is a body line of a
                // here-document, and it ends a command a backslash held open.
                run_line(at);
                if (shell_syntax_generation != syntax)
                        break;
                at = stop;
        }

        memory_free(copy, room);
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

        /*
                A here-document the input ended inside of.

                dash takes the end of the input as the delimiter, says so on
                stderr, and runs the command. Refusing the line here threw
                away a script whose last line was the body -- which is what
                a generated one looks like when the generator forgot the
                delimiter, and what "cat <<EOF" typed into eval looks like
                every time.
        */
        if (parse_here_open())
        {
                string_format(exec_error,
                              "Warning: here-document ended by end of input"
                              " (wanted %s)\n",
                              parse_here_open());

                while (parse_here_open())
                        parse_here_close();

                run_line_inner((string_address) "");

                if (!shell_more)
                        return;
        }

        if (parse_eof_can_complete())
        {
                run_line_inner((string_address) "");

                if (!shell_more)
                        return;
        }

        string_format(exec_error, "Syntax error: unexpected end of input\n");
        bool word_eof = parse_pending_used &&
                        lex_unfinished(parse_pending) == LEX_OPEN_WORD;
        parse_reset();
        shell_more = false;
        shell_status = 2;
        shell_syntax_generation += word_eof ? 1 : 2;
}

// A prompt is for somebody watching. Asking the terminal about itself is the
// only way to know whether anybody is: a script piped in gets none, which is
// also what keeps its output free of them.
#define TCGETS 0x5401u

static b32 shell_interactive()
{
        p8 settings[64];

        return system_control(0, TCGETS, settings) == 0;
}
