#include "../compiler_memory.c"

const positive page_size = 4096;


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
        A builtin that could not do what it was told stops a script.

        Seven places in this file end the same way: complain, set the status,
        and then leave the process entirely -- unless somebody is watching. At
        a terminal the shell stays, or a mistyped exec would close the session,
        and the builtin returns to the prompt instead. Whatever is buffered is
        written out first, because leaving from here does not go back through
        the writer that would have flushed it.
*/
static fn shell_stop_when_scripted(b32 status)
{
        if (shell_is_interactive)
                return;

        log_flush();
        exit(status);
}

/*
        exec of a file that will not run. Interactive shells stay; execfail
        is the same stay for a script. Everything else ends the process,
        which is why a mistyped exec in a script is fatal unless asked not
        to be.
*/
static fn shell_exec_failed(b32 status)
{
        if (shell_is_interactive ||
            (shell_bash_compat && shell_shopt_on(EXECFAIL)))
                return;

        log_flush();
        exit(status);
}

/*
        The words as the shell tokenised them.

        A builtin used to be handed the rest of the line joined back into one
        string, which loses exactly the quoting that printf and test live on.
        These are the arrays shell.c fills. The type is left incomplete so that
        this file makes no claim about how many words fit in them.
*/
string_address address_to shell_argv;
positive shell_argc;

// eval runs a line, and what runs lines sits below this file.
fn run_line(string_address line);
fn shell_input_end();
bool exec_function_here_hashed(string_address name, positive2 named);
bool exec_function_readonly_set(string_address name);
bool exec_function_readonly_hashed(string_address name, positive2 named);
b32 exec_function_unset(string_address name);
static bool exec_line_aborted();
static COLD fn shell_posix_changed(bool on);
static COLD fn shell_getopts_index_changed();
static fn shell_getopts_parameters_changed();

/*
        The letter an option diagnostic names.

        The shared formatter carries %s, %p, %b and %f and no %c, so a
        "%c" in a message wrote nothing at all and every one of these
        read "declare: : invalid option" with the letter missing. A
        letter is spelled into two bytes and handed over as a string
        instead. The room is the caller's, because a diagnostic may
        name two letters at once.
*/
static string_address shell_option_spelled(string_address room, p8 letter)
{
        room[0] = letter;
        room[1] = 0;
        return room;
}

/*
        A letter no builtin would take.

        The two houses answer a bad option differently: dash names it and
        stops, while bash names it and then writes the line that says how
        the builtin is called. Thirty builtins refuse a letter, and none of
        them should have to know that, so the wording lives here. The answer
        says whether a usage line is still owed, because a few builtins
        spell their own name into it and cannot hand over a fixed string.

        The option arrives already spelled, sign and all, since declare and
        its family refuse a +x as readily as a -x.
*/
static COLD fn shell_diagnostic_where();
bool word_is(string_address word, string_address text);

static COLD bool shell_option_bad(string_address name, string_address said)
{
        //      The reference's own option reader names one byte behind the
        //      sign, and a long word by its two dashes and no more: "-ø" is
        //      named by the first of its two bytes and "--bogus" by "--".
        p8 room[3];

        if ((said[0] == '-' || said[0] == '+') && said[1] && said[2])
        {
                room[0] = said[0];
                room[1] = said[1];
                room[2] = end;
                said = room;
        }

        shell_diagnostic_where();

        if (!shell_bash_compat)
        {
                string_report(log_error, 2, "%s: Illegal option %s\n", name,
                              said);
                return false;
        }

        string_format(log_error, "%s: %s: invalid option\n", name, said);
        return true;
}

static COLD bool shell_letter_bad(string_address name, p8 letter)
{
        p8 said[3] = {'-', letter, end};

        return shell_option_bad(name, said);
}

//      The whole complaint, for the builtins whose usage line is one string.
static COLD b32 shell_option_refused(string_address name, string_address said,
                                     string_address usage)
{
        if (shell_option_bad(name, said))
                string_format(log_error, "%s: usage: %s\n", name, usage);

        return 2;
}

static COLD b32 shell_letter_refused(string_address name, p8 letter,
                                     string_address usage)
{
        p8 said[3] = {'-', letter, end};

        return shell_option_refused(name, said, usage);
}

/*
        A name that will not be written to.

        bash calls it a readonly variable and dash says it is read only, and
        they put a different builtin's name in front of it: dash names the
        builtin that tried, bash names only declare's family. Whichever of
        the two is owed is the caller's to say.
*/
static COLD fn shell_unset_readonly_refused(string_address name,
                                            positive length);

static COLD fn shell_readonly_refused(string_address in_bash,
                                      string_address in_dash,
                                      string_address name, positive length)
{
        string_address said = shell_bash_compat ? in_bash : in_dash;

        shell_diagnostic_where();

        if (said)
                string_format(log_error, "%s: ", said);

        log_error(name, length);
        log_error(shell_bash_compat ? ": readonly variable\n"
                                    : ": is read only\n", 0);
}

//      Whether the line about a bad operand has already gone out: neither
//      reference follows it with a second complaint about the operator
//      that was standing beside it.
static bool test_said;

//      A word test was handed where a number belonged: bash calls it an
//      integer it expected, dash an illegal number.
static COLD fn shell_test_number_refused(string_address word)
{
        test_said = true;
        shell_diagnostic_where();
        string_format(log_error,
                      shell_bash_compat ? "%s: %s: integer expected\n"
                                        : "%s: Illegal number: %s\n",
                      shell_argv[0], word);
}

/*
        A program exec could not become.

        bash names the file and what the kernel said and nothing else; dash
        names itself first and keeps "not found" for a file that is not
        there, the way it does for a command it could not find.
*/
static COLD fn shell_exec_refused(string_address name, bipolar answer)
{
        bipolar code = answer < 0 ? -answer : answer;
        string_address why = system_error_message(code);

        if (!why)
                why = (string_address) "No such file or directory";

        shell_diagnostic_where();

        if (shell_bash_compat)
        {
                string_format(log_error, "%s: %s\n", name, why);

                return;
        }

        string_format(log_error, "exec: %s: %s\n", name,
                      code == ERROR_NO_ENTRY ? (string_address) "not found"
                                             : why);
}

//      A word printf was handed where a number belonged. bash calls it an
//      invalid number; dash says it expected a numeric value.
static COLD b32 shell_printf_number_refused(string_address word)
{
        shell_diagnostic_where();

        return string_report(log_error, 1,
                             shell_bash_compat
                                 ? "printf: %s: invalid number\n"
                                 : "printf: %s: expected numeric value\n",
                             word);
}

//      A name read could not write to. Readonly is one reason and no room
//      the other, and only the first of them is worded by the house.
static COLD bool shell_read_refused(string_address name)
{
        if (!env_readonly(name))
        {
                shell_diagnostic_where();
                string_format(log_error, "read: no room for %s\n", name);

                return false;
        }

        shell_readonly_refused(null, (string_address) "read", name,
                               string_length(name));

        return false;
}

//      unset says what it could not do as well as what the name is, and
//      names itself in both houses while it is at it.
static COLD fn shell_unset_readonly_refused(string_address name,
                                            positive length)
{
        shell_diagnostic_where();
        string_format(log_error, "unset: ");
        log_error(name, length);
        log_error(shell_bash_compat
                      ? ": cannot unset: readonly variable\n"
                      : ": is read only\n", 0);
}

/*
        A word that is no name.

        bash quotes the word as it was handed over, back-quote and all, and
        says it is no identifier; dash names it and calls it a bad variable
        name. A word may carry its value -- "1bad=2" -- which bash keeps and
        dash cuts at the equals, so the caller gives the length of the name
        and the whole word is read for the other house.
*/
static COLD fn shell_name_refused(string_address command, string_address word,
                                  positive length)
{
        shell_diagnostic_where();

        if (shell_bash_compat)
        {
                string_format(log_error, "%s: `", command);
                log_error(word, string_length(word));
                log_error("': not a valid identifier\n", 0);

                return;
        }

        string_format(log_error, "%s: ", command);
        log_error(word, length);
        log_error(": bad variable name\n", 0);
}

/*
        Which usage line declare's family writes.

        One body serves declare, typeset and local, and bash gives each of
        them its own line: local takes no attribute list worth printing,
        and typeset's differs from declare's by a pair of brackets.
*/
static COLD string_address shell_declare_usage(string_address name)
{
        if (word_is(name, "local"))
                return (string_address) "local [option] name[=value] ...";

        if (word_is(name, "typeset"))
                return (string_address) "typeset [-aAfFgiIlnrtux] "
                    "name[=value] ... or typeset -p [-aAfFilnrtux] "
                    "[name ...]";

        return (string_address) "declare [-aAfFgiIlnrtux] [name[=value] ...] "
            "or declare -p [-aAfFilnrtux] [name ...]";
}

typedef struct
{
        bipolar offset;
        positive word;
        positive next;
} shell_getopts_state;

static bipolar getopts_offset = -1;
static positive getopts_word;
static positive getopts_next;
static bool getopts_publishing;

static shell_getopts_state shell_getopts_save()
{
        shell_getopts_state saved = {getopts_offset, getopts_word, getopts_next};
        return saved;
}

static fn shell_getopts_restore(shell_getopts_state saved)
{
        getopts_offset = saved.offset;
        getopts_word = saved.word;
        getopts_next = saved.next;
}
static bool exec_assignment_promote(const_string name, positive length);
static b32 exec_unset_prefix(const_string name, positive length);
static PURE bool exec_special_builtin(string_address name);
static fn exec_special_error_note();
static bool exec_child_process();
static bool job_any_stopped();
/* Whether the builtin before this one was exit, which is how both references
   decide that a second exit past a refused one may leave. The dispatcher
   maintains it, because it is a fact about what ran before. */
static bool shell_exit_was_previous;
static bool shell_exit_is_current;
static fn exec_command_reader_finish();
static fn exec_input_finish();
static positive shell_command_reader_depth;
static bool env_attribute_target_span(const_string name, positive length,
                                      const_string address_to target,
                                      positive address_to target_length,
                                      positive address_to target_index);
COLD bool shell_reference_element(
    const_string name, positive length, const_string address_to base,
    positive address_to base_length, const_string address_to subscript,
    positive address_to subscript_length);
static bool exec_source_stop(b32 address_to startup_status);
bool shell_builtin(string_address arguments, positive2 named);
string_address shell_arguments();
fn shell_execute_command();
bipolar shell_spawn_tool(string_address address_to arguments,
                         b32 output, bool quiet);
fn parse_nest_enter();
fn parse_nest_leave();
static bool exec_arithmetic_value(string_address text,
                                  bipolar address_to value,
                                  string_address command);
// exec owns the lifetime of PIPESTATUS; the variable engine materializes its
// deferred one-element value only when a reader actually names it.
fn exec_pipe_status_wanted();

static bool shell_pipe_status_wanted(const_string name, positive length)
{
        if (!shell_bash_compat || length != 10 ||
            memory_compare((address_any)name, "PIPESTATUS", 10))
                return false;

        exec_pipe_status_wanted();
        return true;
}

/*
        An executable text file does not need to name an interpreter when it
        is launched by this shell.

        The kernel quite properly answers ENOEXEC for a file with no #! line.
        The shell interface is older and friendlier: retry that one failure by
        invoking this shell with the file as its script operand. /proc/self/exe
        keeps the interpreter the one that was actually running even when
        argv[0] was a PATH spelling or the working directory has since moved.
        /bin/sh is the portable fallback for a system without procfs; in the
        image it is a link to this same binary.

        argv[0] belongs to the attempted program and is replaced by the script
        pathname. The remaining operands keep their exact addresses and order.
*/
#define ERROR_EXEC_FORMAT 8

bipolar shell_exec_file(string_address path,
                         string_address address_to arguments,
                         positive count,
                         string_address address_to environment)
{
        bipolar answered = system_execute(path, arguments, environment);
        string_address address_to fallback;
        positive entries;
        positive bytes;

        if (answered != -ERROR_EXEC_FORMAT)
                return answered;

        if (count > positive_max - 2)
                return answered;

        entries = count + 2;

        if (entries > positive_max / sizeof(fallback[0]))
                return answered;

        bytes = entries * sizeof(fallback[0]);
        fallback = (string_address address_to)memory(bytes);

        if (!fallback || system_failed(fallback))
                return answered;

        fallback[0] = (string_address)"/proc/self/exe";
        fallback[1] = path;

        if (count > 1)
                memory_copy_apart(fallback + 2, arguments + 1,
                                  (count - 1) * sizeof(arguments[0]));

        fallback[count + 1] = null;

        answered = system_execute(fallback[0], fallback, environment);

        if (answered < 0)
        {
                fallback[0] = (string_address)"/bin/sh";
                answered = system_execute(fallback[0], fallback, environment);
        }

        memory_free(fallback, bytes);
        return answered;
}

#define SHELL_DIRECTORY_MAX 4096

extern p8 address_to shell_directory;

// The three questions faccessat answers. The path walk below is asked all of
// them: whether a name can run, whether it can be read, which is what . wants
// of a file, and whether it is there at all.
#define ACCESS_READ 4
#define ACCESS_WRITE 2
#define ACCESS_EXECUTE 1

/*
        query says only where the name is and never whether it could run:
        type and command -v answer with a path or nothing, where the executor
        wants "found but cannot run" told apart from "not found".
*/
static bipolar shell_find_in_path_alloc_mode(string_address name,
                                              p8 address_to address_to into,
                                              positive address_to room,
                                              positive access, bool query,
                                              string_address fixed_path);
#define shell_find_in_path_alloc(name, into, room)                            \
        shell_find_in_path_alloc_mode((name), (into), (room), ACCESS_EXECUTE, \
                                      false, null)
#define shell_find_in_path_query_alloc(name, into, room)                      \
        shell_find_in_path_alloc_mode((name), (into), (room), ACCESS_EXECUTE, \
                                      true, null)
#define shell_find_in_standard_path_alloc(name, into, room, query)            \
        shell_find_in_path_alloc_mode((name), (into), (room), ACCESS_EXECUTE, \
                                      (query), "/bin:/usr/bin")
bipolar shell_signed(string_address input, bool address_to good);
bool test_facts(string_address path, file_facts address_to out, bool follow);
bool word_is(string_address word, string_address text);
fn hash_forget();
bool shell_here(p8 address_to into, positive room);

/*
        The set flags, remembered but not obeyed.

        Stopping on an error, tracing a command before it runs, refusing a name
        that was never set: all of that happens where commands are run. This is
        only where the letters are kept so that code there can ask.
*/
positive shell_options;

#define SHELL_FLAG(letter) ((positive)1 << ((letter) - 'a'))

/*
        Shell variables and the environment handed to execve.

        The vector may move: every user reaches it by index.  An entry may
        not. Expansion and command setup can hold a value pointer while they
        allocate another variable, so compacting one flat byte array would
        invalidate a live pointer. Entries therefore live in stable cells
        carved from a block store. Replaced and unset cells are kept on a
        free list, so a local variable in a loop reaches a steady state
        instead of consuming memory forever.
*/
typedef struct env_cell
{
        struct env_cell address_to next;
        positive room;
} env_cell;

static shell_store env_store;
static env_cell address_to env_free;

/*
        One record is the variable, its export state and its lookup metadata.

        `text` is either NAME=VALUE or NAME for an exported name which has not
        acquired a value yet.  The initial process stack lives for the whole
        process, so inherited NAME=VALUE strings are borrowed from it.  Text
        created or replaced later lives in an env_cell and sets `owned`.

        Keeping the export bit here matters for more than compactness.  The
        old parallel registries hashed every inherited name twice, allocated
        two indexes, and copied both the complete assignment and the name at
        startup.  A single record and index express all four states directly:
        absent, local value, exported value, and exported name without value.
*/
typedef struct
{
        string_address text;
        positive hash;
        positive name_length;
        positive value_length;
        bool owned;
        bool permanent;
        bool declared;
        // The Bash attributes, and the element table a declared array owns.
        // Both sit in padding the three flags above already left behind, so
        // a scalar variable is neither larger to hold nor slower to probe
        // than it was before arrays existed.
        p8 attributes;
        b32 array;
} env_variable;

static env_variable address_to shell_vars;
static positive shell_vars_room;
static positive shell_var_count;
// Fast negative answer for the overwhelmingly common shell with no readonly
// declarations. The names themselves remain in the indexed variable table;
// this is only a count, not a second registry.
static positive readonly_count;

/*
        The pointer vector above is the form execve and the utilities need,
        but it is a terrible lookup table. A normal inherited environment is
        already forty or fifty entries; putting a loop counter after it made
        every `$i` and every `i=...` scan the whole environment again.

        Keep the vector as the source of truth and index it by the library's
        hardware-floor hash. Slots remember the hash and length so a probe
        reaches the assembly byte comparison only on a real hash candidate.
        Replacement does not change an index. Unset leaves a reusable hash
        tombstone and adjusts vector indexes; only accumulated name churn
        rebuilds the table.
*/
typedef struct
{
        positive hash;
        positive length;
        positive index_plus_one;
} name_index_slot;

static name_index_slot address_to env_index;
static positive env_index_room;
static positive env_index_slots;
static positive env_index_tombstones;

// Rebuilt lazily for execve and the in-process utilities that spawn children.
string_address address_to shell_envp;
static positive shell_envp_room;
static bool shell_envp_dirty = true;
static positive shell_envp_generation;
static positive shell_envp_function_generation;
static bool shell_env_initialized;

static bool env_table_room(positive want)
{
        return shell_array_room(shell_vars, shell_vars_room, want);
}

static positive env_name_hash(const_string name, positive length)
{
        return memory_hash_33((address_any)name, length);
}

/*
        Size and clear a name index without rebuilding entries into it.

        Environment import knows its upper bound before it reads the first
        name. Reserving both indexes once lets that one pass insert directly;
        growing from 64 halfway through startup used to hash every name again
        at each rebuild.
*/
static bool name_index_prepare(name_index_slot address_to address_to table,
                               positive address_to room,
                               positive address_to slot_count,
                               positive address_to tombstones,
                               positive count)
{
        positive slots = 64;

        if (count > positive_max / 2)
        {
                address_to slot_count = 0;
                return false;
        }

        while (slots < count * 2)
        {
                if (slots > positive_max / 2)
                {
                        address_to slot_count = 0;
                        return false;
                }

                slots *= 2;
        }

        if (!shell_room((address_any address_to)table, room, slots,
                        sizeof((address_to table)[0])))
        {
                address_to slot_count = 0;
                return false;
        }

        memory_fill(address_to table, 0,
                    slots * sizeof((address_to table)[0]));
        address_to tombstones = 0;
        address_to slot_count = slots;
        return true;
}

static fn name_index_put(name_index_slot address_to table, positive slots,
                         positive hash, positive length, positive index,
                         positive address_to tombstones)
{
        positive at = hash & (slots - 1);
        positive tombstone = slots;
        positive probes;

        /*
                Bounded, because an open table with no free slot has no
                stopping condition and this walked forever. The assertions
                beside the tables below are what keep it from happening at
                all; this is here because of how the failure looked when it
                did. A shell that had one name too many spun at full speed
                on its first lookup, printing nothing and making no system
                call, so a trace showed a process that had simply stopped.
        */
        for (probes = 0; probes < slots && table[at].index_plus_one; probes++)
        {
                if (table[at].index_plus_one == positive_max &&
                    tombstone == slots)
                        tombstone = at;

                at = (at + 1) & (slots - 1);
        }

        if (probes == slots && tombstone == slots)
                return;

        if (tombstone != slots)
        {
                at = tombstone;
                address_to tombstones = address_to tombstones - 1;
        }

        table[at].hash = hash;
        table[at].length = length;
        table[at].index_plus_one = index + 1;
}

static fn name_index_remove(name_index_slot address_to table, positive slots,
                            positive hash, positive index, positive count,
                            positive address_to tombstones)
{
        positive at = hash & (slots - 1);

        for (positive probes = 0; probes < slots; probes++)
        {
                positive held = table[at].index_plus_one;

                if (!held)
                        break;

                if (held == index + 1)
                {
                        table[at].index_plus_one = positive_max;
                        address_to tombstones = address_to tombstones + 1;
                        break;
                }

                at = (at + 1) & (slots - 1);
        }

        if (index + 1 == count)
                return;

        for (at = 0; at < slots; at++)
                if (table[at].index_plus_one != positive_max &&
                    table[at].index_plus_one > index + 1)
                        table[at].index_plus_one--;
}

static bool env_index_rebuild(positive count)
{
        if (!name_index_prepare(address_of env_index,
                                address_of env_index_room,
                                address_of env_index_slots,
                                address_of env_index_tombstones, count))
                return false;

        for (positive index = 0; index < count; index++)
                name_index_put(env_index, env_index_slots,
                               shell_vars[index].hash,
                               shell_vars[index].name_length, index,
                               address_of env_index_tombstones);

        return true;
}

static PURE positive env_find_hashed_span(const_string name, positive length,
                                     positive hash)
{
        if (env_index_slots)
        {
                positive at = hash & (env_index_slots - 1);

                for (positive probes = 0; probes < env_index_slots; probes++)
                {
                        name_index_slot address_to slot = env_index + at;

                        if (!slot->index_plus_one)
                                return shell_var_count;

                        if (slot->index_plus_one != positive_max &&
                            slot->hash == hash && slot->length == length)
                        {
                                positive index = slot->index_plus_one - 1;

                                if (index < shell_var_count &&
                                    !memory_compare(shell_vars[index].text,
                                                    (address_any)name, length))
                                        return index;
                        }

                        at = (at + 1) & (env_index_slots - 1);
                }

                return shell_var_count;
        }

        /* Allocation failure leaves correctness, but not the acceleration. */
        for (positive index = 0; index < shell_var_count; index++)
                if (shell_vars[index].hash == hash &&
                    shell_vars[index].name_length == length &&
                    !memory_compare(shell_vars[index].text,
                                    (address_any)name, length))
                        return index;

        return shell_var_count;
}

static PURE positive env_find_span(const_string name, positive length)
{
        return env_find_hashed_span(name, length,
                                    env_name_hash(name, length));
}

static env_cell address_to env_cell_take(positive needed)
{
        env_cell address_to cell = env_free;
        env_cell address_to before = null;

        while (cell && cell->room < needed)
        {
                before = cell;
                cell = cell->next;
        }

        if (cell)
        {
                if (before)
                        before->next = cell->next;
                else
                        env_free = cell->next;

                return cell;
        }

        {
                positive room = needed < 64 ? 64 : (needed + 63) & (positive)-64;
                positive total;
                p8 address_to raw;

                if (room < needed || room > (positive)-1 - sizeof(env_cell) - 7)
                        return null;

                total = sizeof(env_cell) + room + 7;
                raw = shell_store_take(address_of env_store, total);

                if (!raw)
                        return null;

                cell = (env_cell address_to)(((positive)raw + 7) & (positive)-8);
                cell->room = room;
        }

        return cell;
}

static fn env_cell_drop(string_address text)
{
        env_cell address_to cell;

        if (!text)
                return;

        cell = ((env_cell address_to)text) - 1;
        cell->next = env_free;
        env_free = cell;
}

static bool env_variable_has_value(env_variable address_to variable)
{
        return variable->text &&
               variable->text[variable->name_length] == '=';
}

/*
        The elements of an array variable, apart from the one it holds itself.

        Element zero of an indexed array is the variable's own value, so $a,
        ${a[0]} and ${#a} stay the ordinary scalar path to the byte and every
        rule about an absent value keeps working without being restated: a=()
        is unset, and `unset a[0]` leaves the rest of the array standing. What
        lives here is the sparse remainder -- subscripts above zero in
        ascending order, so ${a[@]} is a walk, ${!a[@]} needs no sort and
        ${#a[@]} is a field rather than a count of anything.

        An associative array has no scalar element to be, so all of its keys
        are here and its own value slot stays empty. That is also the right
        answer for ${m-word}, which Bash takes from ${m[0]} and not from
        whether any key is set. Keys are compared by their memory_hash_33
        first and by their bytes only on a hash candidate.
*/
typedef struct
{
        // The subscript of an indexed element; the key's hash for a keyed one.
        positive key;
        positive key_length;
        positive value_length;
        // The value, or KEY=VALUE when the element is named by bytes.
        string_address text;
} array_element;

typedef struct
{
        array_element address_to element;
        positive room;
        positive count;
        b32 next_free;
        p32 references;
} array_table;

static array_table address_to array_tables;
static positive array_table_room;
static positive array_table_count;
static b32 array_table_free;

// Slot numbers are one based so that a scalar's zero means no table at all.
static array_table address_to array_table_of(env_variable address_to variable)
{
        return variable->array ? array_tables + (variable->array - 1) : null;
}

static COLD b32 array_table_take()
{
        b32 slot;

        if (array_table_free)
        {
                slot = array_table_free;
                array_table_free = array_tables[slot - 1].next_free;
                array_tables[slot - 1].count = 0;
                array_tables[slot - 1].next_free = 0;
                array_tables[slot - 1].references = 1;
                return slot;
        }

        if (array_table_count >= (positive)0x7ffffffe ||
            !shell_array_room(array_tables, array_table_room,
                              array_table_count + 1))
                return 0;

        slot = (b32)(array_table_count + 1);
        array_tables[array_table_count] = (array_table){.references = 1};
        array_table_count++;

        return slot;
}

/*
        A released table keeps the block its elements were listed in.

        An array unset in a loop would otherwise ask for the same vector
        every iteration. The element cells go back to the same free list
        every replaced variable value uses, so the two reclaim each other's
        bytes rather than each holding its own high-water mark.
*/
static COLD fn array_table_release(b32 slot)
{
        array_table address_to table;

        if (!slot)
                return;

        table = array_tables + (slot - 1);
        if (--table->references)
                return;

        for (positive at = 0; at < table->count; at++)
                env_cell_drop(table->element[at].text);

        table->count = 0;
        table->next_free = array_table_free;
        array_table_free = slot;
}

/* Scope snapshots own the table, not a second serialization of its keys.
   Only a write to a shared table copies cells; hiding/replacing an array
   and restoring a scope are constant-time ownership changes. */
static b32 array_table_hold(b32 slot)
{
        if (slot && array_tables[slot - 1].references == p32_max)
                return -1;
        if (slot)
                array_tables[slot - 1].references++;
        return slot;
}

static COLD bool array_table_edit(env_variable address_to variable, bool clear)
{
        b32 old = variable->array;
        if (!old || array_tables[old - 1].references == 1)
                return true;
        b32 slot = array_table_take();
        if (!slot)
                return false;
        array_table address_to from = array_tables + old - 1;
        array_table address_to to = array_tables + slot - 1;
        if (!clear)
        {
                if (!shell_array_room(to->element, to->room, from->count))
                        goto failed;
                for (positive at = 0; at < from->count; at++)
                {
                        array_element item = from->element[at];
                        positive bytes = item.key_length + (item.key_length != 0) +
                                         item.value_length + 1;
                        env_cell address_to cell = env_cell_take(bytes);
                        if (!cell)
                                goto failed;
                        memory_copy_apart(cell + 1, item.text, bytes);
                        item.text = (string_address)(cell + 1);
                        to->element[to->count++] = item;
                }
        }
        variable->array = slot;
        array_table_release(old);
        return true;
failed:
        array_table_release(slot);
        return false;
}

/*
        Where a subscript is, or where it would go.

        Ascending order is what makes ${a[@]} a walk, so the search that
        finds an element is the same one that says where a new one belongs
        and there is no second ordering pass anywhere.
*/
static COLD PURE positive array_place(array_table address_to table, positive key)
{
        positive low = 0;
        positive high = table->count;

        while (low < high)
        {
                positive middle = low + (high - low) / 2;

                if (table->element[middle].key < key)
                        low = middle + 1;
                else
                        high = middle;
        }

        return low;
}

static COLD PURE positive array_keyed_place(array_table address_to table,
                                       positive hash, const_string key,
                                       positive key_length)
{
        for (positive at = 0; at < table->count; at++)
                if (table->element[at].key == hash &&
                    table->element[at].key_length == key_length &&
                    !memory_compare(table->element[at].text,
                                    (address_any)key, key_length))
                        return at;

        return table->count;
}

// The element bytes, past the key an associative element carries with it.
static PURE string_address array_element_value(array_element address_to element)
{
        return element->text + element->key_length +
               (element->key_length ? 1 : 0);
}

static COLD fn array_element_forget(array_table address_to table, positive at)
{
        positive left = table->count - at - 1;

        env_cell_drop(table->element[at].text);

        if (left)
                memory_copy(table->element + at, table->element + at + 1,
                            left * sizeof(table->element[0]));

        table->count--;
}

/*
        An element written, made or replaced.

        The cell holds KEY=VALUE for a keyed element and VALUE alone for a
        subscripted one, which lets both reuse the one free list. Assignment
        shaping, including append and integer evaluation, is already complete.
*/
static COLD bool array_element_write(array_table address_to table, positive at,
                                bool making, positive key,
                                const_string key_text, positive key_length,
                                const_string value, positive value_length)
{
        if (key_length > positive_max - 2)
                return false;
        positive prefix = key_length ? key_length + 1 : 0;
        env_cell address_to cell;
        p8 address_to into;

        if (value_length > positive_max - prefix - 1)
                return false;

        cell = env_cell_take(prefix + value_length + 1);

        if (!cell)
                return false;

        into = (p8 address_to)(cell + 1);

        if (key_length)
        {
                memory_copy(into, (address_any)key_text, key_length);
                into[key_length] = '=';
        }

        memory_copy_end(into + prefix, (address_any)value, value_length);

        if (making)
        {
                positive left = table->count - at;

                if (!shell_array_room(table->element, table->room,
                                      table->count + 1))
                {
                        env_cell_drop((string_address)into);
                        return false;
                }

                if (left)
                        memory_copy(table->element + at + 1, table->element + at,
                                    left * sizeof(table->element[0]));

                table->count++;
        }
        else
                env_cell_drop(table->element[at].text);

        table->element[at].key = key;
        table->element[at].key_length = key_length;
        table->element[at].value_length = value_length;
        table->element[at].text = (string_address)into;

        return true;
}

/*
        An array is never handed to execve.

        Its text is only element zero, and a child given NAME=<element zero>
        would read an array as a scalar that lost the rest of itself. Bash
        does not export arrays either, and for the same reason: the
        environment has no spelling for one.
*/
static bool env_variable_exports(env_variable address_to variable)
{
        return env_variable_has_value(variable) &&
               !(variable->attributes & SHELL_ARRAY_EITHER) &&
               variable->permanent;
}

static fn env_variable_drop(positive index)
{
        positive left = shell_var_count - index - 1;
        env_variable dropped = shell_vars[index];

        if (dropped.attributes & SHELL_ARRAY_READONLY)
                readonly_count--;

        // Every path remembered was an answer about a PATH that is now gone,
        // the same as when it is assigned over.
        if (dropped.name_length == 4 &&
            memory_is_4(dropped.text, 'P', 'A', 'T', 'H'))
                hash_forget();
        env_locale_touch(dropped.text, dropped.name_length);

        if (env_index_slots)
                name_index_remove(env_index, env_index_slots, dropped.hash,
                                  index, shell_var_count,
                                  address_of env_index_tombstones);

        if (dropped.owned)
                env_cell_drop(dropped.text);

        array_table_release(dropped.array);

        if (left >= 4)
                memory_copy(shell_vars + index, shell_vars + index + 1,
                            left * sizeof(shell_vars[0]));
        else
                for (positive at = 0; at < left; at++)
                        shell_vars[index + at] = shell_vars[index + at + 1];

        shell_var_count--;

        if (env_index_slots &&
            env_index_tombstones >= env_index_slots / 4)
                env_index_rebuild(shell_var_count);

        shell_envp_dirty = true;
}

/*
        A record on the end of the vector, and in the index.

        Three places made one -- an exported name with no value yet, an
        inherited assignment borrowed from the initial stack, and a value
        written here -- and each wrote the eight fields and the index step by
        hand. The caller has already made room in the table; what differs
        between them is only who owns the text and whether it is exported.
*/
static env_variable address_to env_record_append(string_address text,
                                                  positive hash,
                                                  positive name_length,
                                                  positive value_length,
                                                  bool owned, bool permanent)
{
        env_variable address_to record = shell_vars + shell_var_count;

        record->text = text;
        record->hash = hash;
        record->name_length = name_length;
        record->value_length = value_length;
        record->owned = owned;
        record->permanent = permanent;
        record->declared = true;
        // The vector reuses the slot an unset name left, so a new name that
        // lands on it must not inherit the last one's kind -- or, worse, the
        // element table that was handed back with it.
        record->attributes = 0;
        record->array = 0;
        shell_var_count++;

        if (!env_index_slots || shell_var_count > env_index_slots / 2)
                env_index_rebuild(shell_var_count);
        else
                name_index_put(env_index, env_index_slots, hash, name_length,
                               shell_var_count - 1,
                               address_of env_index_tombstones);

        return record;
}

static env_variable address_to env_export_take_hashed(const_string name,
                                                       positive length,
                                                       positive hash)
{
        positive found = env_find_hashed_span(name, length, hash);
        env_cell address_to cell;

        if (found < shell_var_count)
                return shell_vars + found;

        if (!env_table_room(shell_var_count + 1))
                return null;

        cell = env_cell_take(length + 1);

        if (!cell)
                return null;

        memory_copy_end((p8 address_to)(cell + 1), (address_any)name, length);

        return env_record_append((string_address)(cell + 1), hash, length, 0,
                                 true, false);
}

static env_variable address_to env_export_take(const_string name,
                                                positive length)
{
        return env_export_take_hashed(name, length,
                                      env_name_hash(name, length));
}

/* Hidden bindings carry the same payload as live variables. Their owner
   keeps the name and trailer alive when a direct write replaces the value. */
typedef struct
{
        string_address name;
        env_variable variable;
} shell_binding;

static PURE string_address env_variable_value(env_variable address_to variable)
{
        return variable && env_variable_has_value(variable)
                   ? variable->text + variable->name_length + 1 : null;
}

/* Written where it lands, not returned.

   A shell_binding is wider than a register pair, so returning one by value
   put it in the caller's frame and the caller read it straight back out. That
   reload was 91% of exec_keep_value's samples and exec_keep_value was 8.7% of
   an assignment-heavy run, so it looked like most of the cost of every
   assignment this shell performs. Every caller already had somewhere to put
   the answer, so the destination comes in instead.

   Measured, it is not faster: 1,005,575,692 cycles against 1,003,790,403,
   which is +0.18% and inside the noise of nine alternating runs. The round
   trip is gone -- exec_keep_value falls to 0.9% and the samples land in
   env_find_hashed_span, which doubles -- so the stall was real and the core
   was already hiding it behind the lookup it waits on. Kept because a
   forty-byte struct should not travel through the frame to reach a field it
   was going straight into, not because it bought time. Do not spend the
   afternoon here again expecting it to. */
static fn env_saved_state(shell_binding address_to into, const_string name,
                          positive length)
{
        positive found = env_find_span(name, length);

        into->name = (string_address)name;
        into->variable = found < shell_var_count ? shell_vars[found]
            : (env_variable){.hash = env_name_hash(name, length),
                             .name_length = length};
}

static bool shell_binding_hold(shell_binding address_to saved, positive extra)
{
        env_variable value = saved->variable;
        positive length = value.name_length;
        positive size = env_variable_has_value(&value) ? value.value_length + 1 : 0;
        if (length > (positive_max - 2) / 2 ||
            size > positive_max - 2 - length * 2 ||
            extra > positive_max - 2 - length * 2 - size)
                return false;
        env_cell address_to cell = env_cell_take(length * 2 + size + extra + 2);
        if (!cell)
                return false;
        if (array_table_hold(value.array) < 0)
        {
                env_cell_drop((string_address)(cell + 1));
                return false;
        }
        string_address name = (string_address)(cell + 1);
        memory_copy_end(name, saved->name, length);
        saved->variable.text = name + length + 1 + extra;
        saved->variable.owned = false;
        memory_copy_end(saved->variable.text, value.text ? value.text : name, length + size);
        saved->name = name;
        return true;
}

static fn shell_binding_drop(shell_binding address_to saved)
{
        array_table_release(saved->variable.array);
        if (saved->variable.owned)
                env_cell_drop(saved->variable.text);
        env_cell_drop(saved->name);
        *saved = (shell_binding){};
}

static bool env_declare(string_address name, positive length)
{
        env_variable address_to entry = env_export_take(name, length);

        if (!entry)
                return false;

        entry->declared = true;
        return true;
}

/* Declaration and export ownership are the same reversible bit transition.
   The third argument chooses the bit and whether the environment cache is
   affected; all cell creation and empty-cell reclamation stays in one path. */
static fn env_mark_restore(string_address name, bool enabled, bool export_mark)
{
        positive length = string_length(name);
        positive hash = env_name_hash(name, length);
        positive found = env_find_hashed_span(name, length, hash);

        if (enabled)
        {
                env_variable address_to entry =
                    found < shell_var_count
                        ? shell_vars + found
                        : env_export_take_hashed(name, length, hash);

                if (entry)
                {
                        if (export_mark)
                                entry->permanent = true;
                        else
                                entry->declared = true;
                }
        }
        else if (found < shell_var_count)
        {
                env_variable address_to entry = shell_vars + found;

                if (export_mark)
                        entry->permanent = false;
                else
                        entry->declared = false;

                if (!entry->permanent && !entry->declared &&
                    !env_variable_has_value(entry))
                        env_variable_drop(found);
        }

        if (export_mark)
                shell_envp_dirty = true;
}

#define env_declare_restore(name, enabled)                                  \
        env_mark_restore((name), (enabled), false)

static bool env_export_mark_span_mode(const_string name, positive length,
                                      bool direct)
{
        const_string target = name;
        positive target_length = length;
        positive target_index = shell_var_count;
        env_variable address_to entry;

        if (!direct &&
            !env_attribute_target_span(name, length, address_of target,
                                       address_of target_length,
                                       address_of target_index))
                return false;

        entry = target_index < shell_var_count
                    ? shell_vars + target_index
                    : env_export_take(target, target_length);

        if (!entry)
                return false;

        entry->permanent = true;
        exec_assignment_promote(name, length);
        shell_envp_dirty = true;
        return true;
}

static bool env_export_mark_span(const_string name, positive length)
{
        return env_export_mark_span_mode(name, length, false);
}

static bool env_export_mark(string_address name)
{
        return env_export_mark_span(name, string_length(name));
}

static bool env_export_unmark(string_address name)
{
        positive length = string_length(name);
        const_string target = name;
        positive target_length = length;
        positive target_index = shell_var_count;

        if (!env_attribute_target_span(name, length, address_of target,
                                       address_of target_length,
                                       address_of target_index))
                return false;

        if (target_index < shell_var_count)
        {
                env_variable address_to entry = shell_vars + target_index;

                entry->permanent = false;
                if (!entry->declared && !env_variable_has_value(entry))
                        env_variable_drop(target_index);
        }

        shell_envp_dirty = true;
        return true;
}

#define env_export_restore(name, enabled)                                   \
        env_mark_restore((name), (enabled), true)

string_address address_to shell_environment()
{
        static string_address empty[1];
        positive count = 0;
        positive function_generation =
            exec_function_environment_generation();
        positive function_count;

        if (!shell_envp_dirty &&
            shell_envp_function_generation == function_generation)
                return shell_envp ? shell_envp : empty;

        for (positive at = 0; at < shell_var_count; at++)
                if (env_variable_exports(shell_vars + at))
                        count++;

        function_count = exec_function_environment_count();
        if (function_count > positive_max - count - 1 ||
            !shell_array_room(shell_envp, shell_envp_room,
                              count + function_count + 1))
                return null;

        count = 0;

        for (positive at = 0; at < shell_var_count; at++)
                if (env_variable_exports(shell_vars + at))
                        shell_envp[count++] = shell_vars[at].text;

        if (!exec_function_environment_fill(shell_envp + count,
                                             function_count))
                return null;
        count += function_count;

        shell_envp[count] = null;
        shell_envp_dirty = false;
        shell_envp_function_generation = function_generation;
        shell_envp_generation++;

        return shell_envp;
}

PURE bool shell_environment_is_initialized()
{
        return shell_env_initialized;
}

static bool env_write_hashed_span(const_string name, positive name_len,
                                  positive hash, const_string value,
                                  bool assignment);
static bool env_write_destination(const_string name, positive name_len,
    positive hash, positive idx, const_string value, bool assignment,
    env_variable address_to destination);
#define env_write_found_span(name, length, hash, index, value, assignment) \
        env_write_destination(name, length, hash, index, value, assignment, null)
static bool shell_array_set_destination(const_string name, positive length,
    const_string key, positive key_length, const_string value, bool append,
    env_variable address_to destination);
static bool env_assign_hashed_span(const_string name, positive name_len,
                                   positive hash, const_string value);
static bool env_write(const_string name, const_string value, bool assignment);

PURE string_address env_get(const_string name);
bool env_set(const_string name, const_string value);
bool env_assign(const_string name, const_string value);
fn env_unset(string_address name);
static PURE bool env_optlist_name(const_string name, positive length);
static COLD bool env_optlist_take(string_address entry);
static COLD string_address shell_optlist_value(bool shopts,
                                               positive address_to value_length);

/* Adopt a process-lifetime assignment without copying its bytes. */
static bool env_borrow_assignment(string_address entry, bool replace)
{
        string_address mark = string_first_of(entry, '=');
        positive length;
        positive hash;
        positive found;

        if (!mark || mark == entry)
                return false;

        length = (positive)(mark - entry);
        hash = env_name_hash(entry, length);
        found = env_find_hashed_span(entry, length, hash);

        if (found < shell_var_count && !replace)
                return true;

        if (found >= shell_var_count)
        {
                if (!env_table_room(shell_var_count + 1))
                        return false;

                env_record_append(entry, hash, length, string_length(mark + 1),
                                  false, true);
                env_locale_touch(entry, length);
                return true;
        }

        if (shell_vars[found].owned)
                env_cell_drop(shell_vars[found].text);

        shell_vars[found].text = entry;
        shell_vars[found].hash = hash;
        shell_vars[found].name_length = length;
        shell_vars[found].value_length = string_length(mark + 1);
        shell_vars[found].owned = false;
        shell_vars[found].permanent = true;
        shell_vars[found].declared = true;
        env_locale_touch(entry, length);

        return true;
}

static PURE bool env_function_assignment(string_address entry)
{
        static const p8 prefix[] = "BASH_FUNC_";
        string_address at;

        if (!shell_bash_compat || !entry)
                return false;
        for (positive at = 0; at < sizeof(prefix) - 1; at++)
                if (entry[at] != prefix[at])
                        return false;

        at = entry + sizeof(prefix) - 1;
        if (!string_get(at) || string_is(at, '='))
                return false;
        while (string_get(at) && string_not(at, '='))
                at++;

        return string_is(at, '=') && at >= entry + sizeof(prefix) + 2 &&
               at[-1] == '%' && at[-2] == '%';
}

fn shell_env_init(string_address address_to process_environment)
{
        positive inherited = 0;

        shell_env_initialized = true;

        while (process_environment && process_environment[inherited])
                inherited++;

        if (!env_table_room(inherited + 4))
                return;

        // Production enters once. Keeping the routine restartable makes the
        // ownership boundary testable and prevents an embedding harness from
        // leaking cells when it supplies a second synthetic initial stack.
        for (positive at = 0; at < shell_var_count; at++)
                if (shell_vars[at].owned)
                        env_cell_drop(shell_vars[at].text);

        shell_var_count = 0;
        readonly_count = 0;
        shell_envp_dirty = true;
        env_index_slots = 0;
        env_index_tombstones = 0;

        // The inherited entries and four defaults are the upper bound.  One
        // allocation and clear now serves variable lookup and export state.
        // Allocation failure retains the existing linear fallback.
        name_index_prepare(address_of env_index, address_of env_index_room,
                           address_of env_index_slots,
                           address_of env_index_tombstones, inherited + 4);

        // A shell launched by make, system(), or another shell starts with the
        // environment it was given.  Initial-stack strings are immutable and
        // process-lifetime stable, so keep them in place instead of allocating
        // and copying an assignment cell plus a second export-name cell.
        for (positive at = 0;
             process_environment && process_environment[at]; at++)
        {
                /* Function transport is parsed and validated after shell
                   startup has initialized parser policy. Never also expose
                   its implementation-name as an ordinary variable. */
                if (env_function_assignment(process_environment[at]))
                        continue;
                /* SHELLOPTS and BASHOPTS are not variables. An inherited
                   value turns those options on; the live listing is built
                   when something reads the name. */
                if (env_optlist_take(process_environment[at]))
                        continue;
                // Duplicate names are legal; keep the old last-one-wins
                // behavior without creating a second index entry.
                env_borrow_assignment(process_environment[at], true);
        }

        // Programs live at the root of the image, so it is on the path.
        // IFS is a variable and not only a splitting policy: a script may
        // read it, save it and put it back, and under set -u one that is
        // absent rather than defaulted is an error where every other shell
        // hands over the three bytes.
        string_address defaults[] = {"PATH=" BOWL_DEFAULT_PATH,
                                     "SHELL=/bin/sh",
                                     "HOME=/root",
                                     "LANG=C.UTF-8",
                                     "IFS= \t\n",
                                     "OPTIND=1", null};

        positive i = 0;

        while (defaults[i])
        {
                string_address mark = string_first_of(defaults[i], '=');
                p8 name[16];

                string_copy_max_end(name, defaults[i], (positive)(mark - defaults[i]));

                if (!env_get(name))
                        // String literals, like initial-stack strings, remain
                        // valid for the process lifetime. Mutation takes the
                        // ordinary owned-cell path later.
                        env_borrow_assignment(defaults[i], false);
                else
                        env_export_mark(name);

                i++;
        }

        /*
                How many shells deep this one is.

                Counted rather than inherited: the next shell reads what this
                one exported and adds its own, which is how a script guarding
                on SHLVL can tell it is being run from inside itself. It is
                one variable and it is written once, at startup, because a
                name that has to be exported cannot be answered from a clock.
        */
        {
                //      Written into a buffer that outlives the shell and
                //      borrowed rather than copied, the same way the defaults
                //      above are: an owned cell here would be the first
                //      allocation a shell makes, and a shell that runs one
                //      command should make none.
                static p8 level[32] = "SHLVL=";
                string_address held = env_get("SHLVL");

                level[6 + positive_into_string(
                             level + 6,
                             (held ? string_digits(held, null) : 0) + 1)] = end;

                env_borrow_assignment(level, true);
        }

        // The shell that started this one left its own last argument in the
        // environment as _, and a record standing there is what a lookup
        // would find instead of the one this shell keeps as it runs.
        env_unset("_");

        /*
                Preserve the logical directory inherited through a symlink
                without paying getcwd only to throw its answer away. cd and
                pwd already validate this name against "." before trusting
                it; a stale, relative or truncated PWD is repaired there.
                An empty environment still needs the kernel's first answer.
        */
        {
                string_address inherited_directory = env_get("PWD");

                if (inherited_directory)
                        string_copy_max_end(shell_directory,
                                            inherited_directory,
                                            SHELL_DIRECTORY_MAX - 1);
                else
                {
                        memory_copy(shell_directory - 4, "PWD=", 4);
                        shell_here(shell_directory, SHELL_DIRECTORY_MAX);
                        env_borrow_assignment(shell_directory - 4, false);
                }
        }
}

/*
        The library's string routines take a mutable pointer and do not write
        through it. Rather than loosen every one of those declarations, the
        promise these two make to their callers is kept here and the cast is
        made where it is safe to see that nothing is written.
*/
#define env_reading(text) ((string_address)(text))

typedef struct
{
        const_string name;
        positive length;
        positive hash;
        positive index;
        const_string subscript;
        positive subscript_length;
        bool element;
        bool valid;
        env_variable address_to destination;
} env_reference;

static bool env_reference_element_span(
    const_string name, positive length, positive address_to base_length,
    const_string address_to subscript, positive address_to subscript_length)
{
        positive open = 0;

        if (length < 3 || string_get(name + length - 1) != ']' ||
            !expand_name_character(string_get(name)) ||
            byte_is_digit(string_get(name)))
                return false;

        open = string_span_max(env_reading(name), length, string_set_name);

        if (!open || open >= length - 1 || string_get(name + open) != '[')
                return false;

        address_to base_length = open;
        address_to subscript = name + open + 1;
        address_to subscript_length = length - open - 2;
        return true;
}

/* One bounded walk resolves both live names and an explicitly selected hidden
   binding. Arithmetic lookup never receives that destination. Element targets
   retain it until the subscript has been evaluated once by the writer. */
static COLD PURE env_reference env_reference_destination(const_string name,
    positive length, positive hash, env_variable address_to destination)
{
        env_reference answer = {name, length, hash, shell_var_count,
                                null, 0, false, true, null};
        for (positive step = 0; step <= 16; step++)
        {
                answer.index = env_find_hashed_span(answer.name, answer.length, answer.hash);
                answer.destination = destination && answer.length == destination->name_length &&
                    !memory_compare(answer.name, destination->text, answer.length) ? destination : null;
                env_variable address_to variable = answer.destination ? answer.destination
                    : answer.index < shell_var_count ? shell_vars + answer.index : null;
                if (answer.element || !variable ||
                    !(variable->attributes & SHELL_ARRAY_NAMEREF) || !env_variable_has_value(variable))
                {
                        if (step && variable && !answer.element)
                        {
                                answer.name = variable->text;
                                answer.length = variable->name_length;
                                answer.hash = variable->hash;
                        }
                        return answer;
                }
                if (step == 16)
                        break;
                answer.name = variable->text + variable->name_length + 1;
                answer.length = variable->value_length;
                positive base;
                if (env_reference_element_span(answer.name, answer.length, &base,
                    &answer.subscript, &answer.subscript_length))
                {
                        answer.length = base;
                        answer.element = true;
                }
                answer.hash = env_name_hash(answer.name, answer.length);
        }
        answer.valid = false;
        answer.index = shell_var_count;
        answer.destination = null;
        return answer;
}

#define env_reference_hashed(name, length, hash) \
        env_reference_destination(name, length, hash, null)

static COLD PURE env_reference env_reference_span(const_string name,
                                                   positive length)
{
        return env_reference_hashed(name, length,
                                    env_name_hash(name, length));
}

// A subscript may assign the nameref cell that supplied its name and bytes.
static COLD bool shell_reference_assign_destination(env_reference resolved,
    const_string value, bool append, env_variable address_to destination)
{
        string_address name = shell_store_copy(address_of expand_store,
            env_reading(resolved.name), resolved.length);
        string_address subscript = shell_store_copy(address_of expand_store,
            env_reading(resolved.subscript), resolved.subscript_length);
        value = shell_store_copy(address_of expand_store, env_reading(value),
                                  string_length(env_reading(value)));
        if (!name || !subscript || !value)
                return false;
        positive key_length;
        string_address key = shell_expand_subscript(name, resolved.length, subscript,
            resolved.subscript_length, address_of key_length);
        return key && shell_array_set_destination(name, resolved.length, key, key_length,
                                                  value, append, destination);
}

#define shell_reference_assign(resolved, value, append) \
        shell_reference_assign_destination(resolved, value, append, null)

/* Export and readonly name the variable visible through an ordinary nameref.
   Keep element-bound references on their own record here: Bash gives those a
   separate invalid-identifier policy, and treating their containing array as
   an ordinary scalar target would silently mark the wrong object. */
static bool env_attribute_target_span(
    const_string name, positive length, const_string address_to target,
    positive address_to target_length, positive address_to target_index)
{
        env_reference resolved = env_reference_span(name, length);

        if (!resolved.valid)
                return false;

        if (!resolved.element)
        {
                address_to target = resolved.name;
                address_to target_length = resolved.length;
                if (target_index)
                        address_to target_index = resolved.index;
        }
        else
        {
                address_to target = name;
                address_to target_length = length;
                if (target_index)
                        address_to target_index = env_find_span(name, length);
        }

        return true;
}

string_address env_get_hashed_span(const_string name, positive length,
                                   positive hash,
                                   positive address_to value_length)
{
        positive index;

        if (name == null)
                return null;

        index = env_find_hashed_span(name, length, hash);

        if (index >= shell_var_count ||
            !env_variable_has_value(shell_vars + index))
                return null;

        /* Keep the ordinary scalar path at one probe. Only the attribute bit
           pays for the shared multi-hop resolver. */
        if (shell_vars[index].attributes & SHELL_ARRAY_NAMEREF)
        {
                env_reference resolved = env_reference_hashed(name, length, hash);
                index = resolved.element ? shell_var_count : resolved.index;

                if (index >= shell_var_count ||
                    !env_variable_has_value(shell_vars + index))
                        return null;

                length = shell_vars[index].name_length;
        }

        if (value_length)
                address_to value_length = shell_vars[index].value_length;

        return shell_vars[index].text + length + 1;
}

PURE string_address env_get(const_string name)
{
        positive2 answer;

        if (!name)
                return null;

        answer = string_hash_33_length(env_reading(name));
        return env_get_hashed_span(name, answer.y, answer.x, null);
}

/*
        The set variable names beginning with prefix. Bash ${!prefix@} and
        ${!prefix*} need the same source table as lookup and export; a null
        destination is the sizing pass before the expander allocates its
        pointer vector. Exported or readonly names without values are unset
        and therefore do not take part.
*/
positive env_names_prefix(string_address prefix, positive length,
                          string_address address_to names, positive room)
{
        positive count = 0;

        // A deferred PIPESTATUS is still a variable name. Prefix discovery
        // must publish it before sizing the answer, including the empty
        // prefix which asks for every variable.
        if (shell_bash_compat && length <= 10 &&
            !memory_compare("PIPESTATUS", prefix, length))
                exec_pipe_status_wanted();

        for (positive at = 0; at < shell_var_count; at++)
        {
                env_variable address_to variable = shell_vars + at;

                if (!env_variable_has_value(variable) ||
                    variable->name_length < length ||
                    memory_compare(variable->text, prefix, length))
                        continue;

                if (count < room)
                        names[count] = variable->text;

                count++;
        }

        return count;
}

/*
        What an attribute makes of the bytes on their way in.

        -i, -l and -u act once, where the value is stored, which is why
        declare -p shows what they made of it and not what was written. The
        arithmetic answer is a small fixed spelling; a folded one is as long
        as the value and takes arena bytes the caller copies out of at once.
*/
#define ENV_ATTRIBUTE_VALUE                                                  \
        (SHELL_ARRAY_INTEGER | SHELL_ARRAY_LOWER | SHELL_ARRAY_UPPER)

static bool env_write_noted(const_string name, positive length, bool written)
{
        if (written)
                env_locale_touch(name, length);
        if (!shell_bash_compat || !written)
                return written;

        if (length == 15 &&
            !memory_compare((address_any)name, "POSIXLY_CORRECT", 15))
                shell_posix_changed(true);
        else if (length == 6 &&
                 !memory_compare((address_any)name, "OPTIND", 6))
                shell_getopts_index_changed();
        return written;
}

static COLD string_address env_attribute_value(p8 attributes,
                                               const_string value, bool fatal)
{
        positive length = string_length(env_reading(value));
        p8 address_to made;

        if (attributes & SHELL_ARRAY_INTEGER)
        {
                // Arithmetic can assign the cell that supplied either its
                // expression or the caller's name, and can itself be nested.
                string_address held = shell_store_copy(address_of expand_store,
                                                         env_reading(value), length);
                if (!held)
                        return null;
                string_address outer = arith_at;
                bool active = arith_active, bad = arith_bad;
                bipolar answer = arith_evaluate(*arith_skip_space(held)
                                                  ? held : (string_address)"0");
                bool failed = arith_bad;
                arith_at = outer;
                arith_active = active;
                arith_bad |= bad;
                if (failed)
                {
                        string_format(writer_stderr_once, "%s: invalid arithmetic expression\n", held);
                        if (fatal)
                        {
                                expand_fatal_status(1);
                                return null;
                        }
                        return held;
                }

                made = shell_store_take(address_of expand_store, 32);

                if (!made)
                        return null;

                made[bipolar_into_string(made, answer)] = end;

                return made;
        }

        made = shell_store_copy(address_of expand_store, env_reading(value),
                                length);

        if (!made)
                return null;

        if (attributes & SHELL_ARRAY_UPPER)
                memory_to_upper_ascii(made, length);
        else
                memory_to_lower_ascii(made, length);

        return made;
}

/*
        An assignment to a name that carries attributes.

        A nameref is not the variable being written at all: it says which one
        is. An associative array has no value of its own, so `m=x` writes the
        element Bash reads $m as. And -i, -l and -u change the bytes on the
        way in, which is why declare -p shows what they made of them.

        Out of line because none of this is what an assignment usually is:
        zero says the caller writes the value in `shaped` the ordinary way,
        and one or two say it has already been written, or refused.
*/
static COLD b32 env_write_attributed(positive idx, const_string name,
                                     positive name_len, const_string value,
                                     bool assignment,
                                     const_string address_to shaped,
                                     env_variable address_to destination)
{
        env_variable address_to variable = destination ? destination : shell_vars + idx;
        p8 attributes = variable->attributes;

        if ((attributes & SHELL_ARRAY_NAMEREF) &&
            env_variable_has_value(variable))
        {
                env_reference resolved = env_reference_destination(
                    name, name_len, variable->hash, destination);

                if (!resolved.valid)
                        return 2;

                if (resolved.element)
                        return shell_reference_assign_destination(resolved, value, false, destination) ? 1 : 2;

                return env_write_noted(
                           resolved.name, resolved.length,
                           env_write_found_span(
                               resolved.name, resolved.length, resolved.hash,
                               resolved.index, value, assignment))
                           ? 1 : 2;
        }

        if (attributes & SHELL_ARRAY_ASSOCIATIVE)
                return shell_array_set_destination(name, name_len, "0", 1, value, false, destination)
                           ? 1 : 2;

        if (attributes & ENV_ATTRIBUTE_VALUE)
        {
                value = env_attribute_value(attributes, value, true);

                if (!value)
                        return 2;

                address_to shaped = value;
        }

        return 0;
}

static bool env_write_destination(const_string name, positive name_len,
    positive hash, positive idx, const_string value, bool assignment,
    env_variable address_to destination)
{
        bool allexport = assignment && (shell_options & SHELL_FLAG('a'));
        if (!name || !value)
                return false;
        if (!destination && env_optlist_name(name, name_len))
                return false;
        if (!destination && name_len == 4 && memory_is_4(name, 'P', 'A', 'T', 'H'))
                hash_forget();
        if (!destination && idx == shell_var_count && shell_dynamic_assign(name, name_len, value))
                return true;

        env_variable address_to variable = destination ? destination
            : idx < shell_var_count ? shell_vars + idx : null;
        if (variable && variable->attributes)
        {
                if ((variable->attributes & SHELL_ARRAY_READONLY) &&
                    (!(variable->attributes & SHELL_ARRAY_NAMEREF) ||
                     !env_variable_has_value(variable)))
                        return false;
                if (variable->attributes & SHELL_ARRAY_INTEGER)
                {
                        name = shell_store_copy(&expand_store, env_reading(name), name_len);
                        if (!name)
                                return false;
                }
                b32 done = env_write_attributed(idx, name, name_len, value,
                                                assignment, &value, destination);
                if (done)
                {
                        if (done == 1)
                                env_locale_touch(name, name_len);
                        return done == 1;
                }
                idx = env_find_hashed_span(name, name_len, hash);
                variable = destination ? destination
                    : idx < shell_var_count ? shell_vars + idx : null;
                if (variable && (variable->attributes & SHELL_ARRAY_READONLY))
                        return false;
        }

        positive value_len = string_length(env_reading(value));
        positive needed = name_len + value_len + 2;
        if (!variable && !env_table_room(shell_var_count + 1))
                return false;
        env_cell address_to old = variable && variable->owned
            ? ((env_cell address_to)variable->text) - 1 : null;
        env_cell address_to cell = old && old->room >= needed ? old : env_cell_take(needed);
        if (!cell)
                return false;
        string_address text = (string_address)(cell + 1);
        memory_copy(text, env_reading(name), name_len);
        text[name_len] = '=';
        memory_copy_end(text + name_len + 1, env_reading(value), value_len);
        if (variable)
        {
                variable->text = text;
                variable->value_length = value_len;
                variable->owned = true;
                variable->declared = true;
                if (old && old != cell)
                        env_cell_drop((string_address)(old + 1));
        }
        else
                variable = env_record_append(text, hash, name_len, value_len, true, allexport);
        if (variable->attributes & SHELL_ARRAY_INDEXED)
                variable->attributes |= SHELL_ARRAY_ASSIGNED;
        if (allexport)
                variable->permanent = true;
        if (!destination && variable->permanent)
                shell_envp_dirty = true;
        env_locale_touch(name, name_len);
        return true;
}

static bool env_write_hashed_span(const_string name, positive name_len,
                                  positive hash, const_string value,
                                  bool assignment)
{
        positive idx;
        bool written;

        if (!name || !value)
                return false;

        idx = env_find_hashed_span(name, name_len, hash);
        written = env_write_found_span(name, name_len, hash, idx, value,
                                       assignment);
        return env_write_noted(name, name_len, written);
}

static bool env_write(const_string name, const_string value, bool assignment)
{
        if (!name || !value)
                return false;

        positive2 named = string_hash_33_length(env_reading(name));
        return env_write_hashed_span(name, named.y, named.x, value, assignment);
}

bool env_set(const_string name, const_string value)
{
        return env_write(name, value, false);
}

bool env_assign(const_string name, const_string value)
{
        return env_write(name, value, true);
}

static bool env_assign_hashed_span(const_string name, positive name_len,
                                   positive hash, const_string value)
{
        return env_write_hashed_span(name, name_len, hash, value, true);
}

/*
        The array surface the expander and the builtins reach arrays through.

        Every one of these takes the name as a span, because the caller has
        just cut it out of ${name[key]} and has no reason to make a
        terminated copy of it first. A subscript arrives as text for the same
        reason -- it was text in the word -- and subscript zero is answered
        from the variable's own value rather than from the element table.
*/
static PURE positive array_index_of(const_string key, positive key_length)
{
        positive value = 0;

        for (positive at = 0; at < key_length; at++)
                value = value * 10 +
                        (positive)(((string_address)key)[at] - '0');

        return value;
}

typedef struct
{
        positive key;
        positive at;
        bool keyed;
        bool found;
} array_location;

/* Reads, writes and removals use the same insertion position and exact-hit
   test. Indexed zero remains the variable's scalar cell, not a table slot. */
static COLD array_location array_locate(env_variable address_to variable,
                                         const_string key, positive length)
{
        array_table address_to table = array_table_of(variable);
        array_location located = {0};
        located.keyed = (variable->attributes & SHELL_ARRAY_ASSOCIATIVE) != 0;
        located.key = located.keyed ? memory_hash_33((address_any)key, length)
                                    : array_index_of(key, length);
        if (table && (located.keyed || located.key))
        {
                located.at = located.keyed
                    ? array_keyed_place(table, located.key, key, length)
                    : array_place(table, located.key);
                located.found = located.at < table->count &&
                    (located.keyed || table->element[located.at].key == located.key);
        }
        return located;
}

COLD PURE p8 shell_variable_attributes(const_string name, positive length)
{
        positive found = env_find_span(name, length);

        return found < shell_var_count ? shell_vars[found].attributes : 0;
}

/* Array syntax acts on a nameref's target, while declaration syntax still
   needs shell_variable_attributes() above to describe the reference itself. */
COLD PURE p8 shell_array_attributes(const_string name, positive length)
{
        env_reference resolved = env_reference_span(name, length);

        return resolved.valid && !resolved.element &&
                       resolved.index < shell_var_count
                   ? shell_vars[resolved.index].attributes
                   : 0;
}

COLD bool shell_reference_resolve(const_string name, positive length,
                                  const_string address_to resolved_name,
                                  positive address_to resolved_length)
{
        env_reference resolved = env_reference_span(name, length);

        if (!resolved.valid || resolved.element)
                return false;

        address_to resolved_name = resolved.name;
        address_to resolved_length = resolved.length;
        return true;
}

COLD bool shell_reference_element(
    const_string name, positive length, const_string address_to base,
    positive address_to base_length, const_string address_to subscript,
    positive address_to subscript_length)
{
        env_reference resolved = env_reference_span(name, length);

        if (!resolved.valid || !resolved.element)
                return false;

        if (base)
                address_to base = resolved.name;
        if (base_length)
                address_to base_length = resolved.length;
        if (subscript)
                address_to subscript = resolved.subscript;
        if (subscript_length)
                address_to subscript_length = resolved.subscript_length;
        return true;
}

COLD string_address shell_reference_element_value(
    const_string name, positive length, positive address_to value_length)
{
        const_string base;
        const_string subscript;
        positive base_length;
        positive subscript_length;
        positive key_length;
        string_address key;

        if (!shell_reference_element(
                name, length, address_of base, address_of base_length,
                address_of subscript, address_of subscript_length))
                return null;

        key = shell_expand_subscript(
            (string_address)base, base_length, (string_address)subscript,
            subscript_length, address_of key_length);

        return key ? shell_array_get(base, base_length, key, key_length,
                                     value_length)
                   : null;
}

/*
        rbash holds four names readonly.

        PATH decides what a bare name reaches, SHELL and ENV and BASH_ENV
        decide what runs on the way in; letting any of them be written is
        letting the restriction be written. Bash marks them readonly rather
        than refusing the assignment by name, so everything that already asks
        this question -- an assignment, export, unset, a declaration command
        -- refuses them without a check of its own.
*/
static PURE bool env_restricted_name(const_string name, positive length)
{
        static const string_address held[] = {
            (string_address) "PATH", (string_address) "SHELL",
            (string_address) "ENV", (string_address) "BASH_ENV"};
        positive at;

        if (!shell_restricted || !name)
                return false;

        for (at = 0; at < array_count(held); at++)
                if (string_length(held[at]) == length &&
                    !memory_compare(held[at], (address_any)name, length))
                        return true;

        return false;
}

static PURE bool env_assignment_readonly_destination(const_string name,
    positive length, positive hash, env_variable address_to destination)
{
        if (env_restricted_name(name, length))
                return true;
        if (env_optlist_name(name, length))
                return true;
        if (!name || (!readonly_count && (!destination ||
            !(destination->attributes & SHELL_ARRAY_READONLY))))
                return false;
        if (destination && (!(destination->attributes & SHELL_ARRAY_NAMEREF) ||
            !env_variable_has_value(destination)))
                return (destination->attributes & SHELL_ARRAY_READONLY) != 0;
        env_reference resolved = env_reference_destination(name, length, hash, destination);
        return resolved.valid && resolved.index < shell_var_count &&
               (shell_vars[resolved.index].attributes & SHELL_ARRAY_READONLY) != 0;
}

PURE bool env_assignment_readonly_hashed_span(const_string name,
                                              positive length,
                                              positive hash)
{
        return env_assignment_readonly_destination(name, length, hash, null);
}

PURE bool env_readonly_hashed_span(const_string name, positive length,
                                   positive hash)
{
        positive found;

        if (env_restricted_name(name, length))
                return true;
        if (env_optlist_name(name, length))
                return true;

        if (!name || !readonly_count)
                return false;

        found = env_find_hashed_span(name, length, hash);
        return found < shell_var_count &&
               (shell_vars[found].attributes &
                SHELL_ARRAY_READONLY) != 0;
}

PURE bool env_readonly(const_string name)
{
        positive2 named;

        if (!name)
                return false;

        named = string_hash_33_length(env_reading(name));
        if (env_restricted_name(name, named.y) || env_optlist_name(name, named.y))
                return true;
        if (!readonly_count)
                return false;
        return env_readonly_hashed_span(name, named.y, named.x);
}

static bool shell_variable_attribute_apply(env_variable address_to variable,
    const_string name, positive length, p8 set, p8 clear, bool visible)
{
        bool was_readonly;
        bool now_readonly;

        if (!variable)
                return false;

        was_readonly = (variable->attributes & SHELL_ARRAY_READONLY) != 0;

        if (set & SHELL_ARRAY_EITHER)
        {
                if (!variable->array)
                {
                        b32 slot = array_table_take();

                        if (!slot)
                                return false;

                        variable->array = slot;
                }
                if ((set & SHELL_ARRAY_ASSOCIATIVE) &&
                    !(variable->attributes & SHELL_ARRAY_EITHER) &&
                    env_variable_has_value(variable))
                {
                        string_address copied_name = null;
                        if (!variable->owned)
                        {
                                env_cell address_to cell = env_cell_take(length + 1);
                                if (!cell)
                                        return false;
                                copied_name = (string_address)(cell + 1);
                                memory_copy_end(copied_name, env_reading(name), length);
                        }
                        if (!array_element_write(array_table_of(variable), 0, true,
                                memory_hash_33("0", 1), "0", 1,
                                variable->text + length + 1, variable->value_length))
                        {
                                if (copied_name)
                                        env_cell_drop(copied_name);
                                return false;
                        }
                        if (copied_name)
                        {
                                variable->text = copied_name;
                                variable->owned = true;
                        }
                        else
                                variable->text[length] = end;
                        variable->value_length = 0;
                        set |= SHELL_ARRAY_ASSIGNED;
                        if (visible)
                                shell_envp_dirty = true;
                }
        }
        else if (clear & SHELL_ARRAY_EITHER)
        {
                // The table is the storage of both kinds, so taking either
                // bit away takes the elements with it rather than leaving
                // them held and unreachable.
                array_table_release(variable->array);
                variable->array = 0;
        }

        variable->attributes =
            (p8)((variable->attributes & (p8)~clear) | set);
        variable->declared = true;

        now_readonly = (variable->attributes & SHELL_ARRAY_READONLY) != 0;
        if (visible && was_readonly != now_readonly)
                readonly_count = now_readonly ? readonly_count + 1
                                              : readonly_count - 1;

        return true;
}

static bool shell_variable_attribute_destination(const_string name, positive length,
    p8 set, p8 clear, env_variable address_to destination)
{
        return shell_variable_attribute_apply(destination ? destination : env_export_take(name, length),
                                              name, length, set, clear, !destination);
}

COLD bool shell_variable_attribute_set(const_string name, positive length, p8 set,
                                  p8 clear)
{
        return shell_variable_attribute_destination(name, length, set, clear, null);
}

static bool readonly_add_mode(string_address name, positive length,
                              bool direct)
{
        const_string target = name;
        positive target_length = length;

        if (!direct &&
            !env_attribute_target_span(name, length, address_of target,
                                       address_of target_length,
                                       null))
                return false;

        if (!shell_variable_attribute_set(target, target_length,
                                          SHELL_ARRAY_READONLY, 0))
                return false;
        if (exec_assignment_promote(name, length))
                return env_export_mark_span(name, length);
        return true;
}

static COLD env_reference shell_array_reference(const_string name,
                                                 positive length,
                                                 bool frames)
{
        env_reference resolved = env_reference_span(name, length);

        if (resolved.valid && resolved.index >= shell_var_count && frames &&
            shell_frames_wanted(resolved.name, resolved.length))
                resolved.index = env_find_hashed_span(
                    resolved.name, resolved.length, resolved.hash);

        return resolved;
}

COLD positive shell_array_length(const_string name, positive length)
{
        env_reference resolved = shell_array_reference(name, length, true);
        env_variable address_to variable;

        if (!resolved.valid || resolved.element ||
            resolved.index >= shell_var_count)
                return 0;

        variable = shell_vars + resolved.index;

        return (variable->array ? array_tables[variable->array - 1].count : 0) +
               (env_variable_has_value(variable) ? 1 : 0);
}

// What ${a[-1]} counts back from. Bash names the last element by the largest
// subscript in use and not by how many there are, so a hole does not move it.
COLD PURE positive shell_array_highest(const_string name, positive length)
{
        env_reference resolved = env_reference_span(name, length);
        array_table address_to table;

        if (!resolved.valid || resolved.element ||
            resolved.index >= shell_var_count)
                return 0;

        table = array_table_of(shell_vars + resolved.index);

        return table && table->count ? table->element[table->count - 1].key : 0;
}

COLD positive shell_array_items(const_string name, positive length,
                           shell_array_item address_to items, positive room)
{
        env_reference resolved = shell_array_reference(name, length, true);
        env_variable address_to variable;
        array_table address_to table;
        positive count = 0;

        if (!resolved.valid || resolved.element ||
            resolved.index >= shell_var_count)
                return 0;

        variable = shell_vars + resolved.index;
        table = array_table_of(variable);

        if (env_variable_has_value(variable))
        {
                if (count < room)
                {
                        items[count].index = 0;
                        items[count].key = null;
                        items[count].key_length = 0;
                        items[count].value =
                            variable->text + resolved.length + 1;
                        items[count].value_length = variable->value_length;
                }

                count++;
        }

        for (positive at = 0; table && at < table->count; at++)
        {
                array_element address_to element = table->element + at;

                if (count < room)
                {
                        items[count].index = element->key;
                        items[count].key =
                            element->key_length ? element->text : null;
                        items[count].key_length = element->key_length;
                        items[count].value = array_element_value(element);
                        items[count].value_length = element->value_length;
                }

                count++;
        }

        return count;
}

COLD string_address shell_array_get(const_string name, positive length,
                               const_string key, positive key_length,
                               positive address_to value_length)
{
        env_reference resolved = shell_array_reference(name, length, true);
        if (!resolved.valid || resolved.element ||
            resolved.index >= shell_var_count)
                return null;

        env_variable address_to variable = shell_vars + resolved.index;
        array_location located = array_locate(variable, key, key_length);
        if (!located.keyed && !located.key)
        {
                if (!env_variable_has_value(variable))
                        return null;
                if (value_length)
                        *value_length = variable->value_length;
                return variable->text + resolved.length + 1;
        }
        if (!located.found)
                return null;

        array_element address_to element =
            array_table_of(variable)->element + located.at;
        if (value_length)
                *value_length = element->value_length;
        return array_element_value(element);
}

/* `declare -n n=value` changes what n names; an ordinary `n=value` follows
   n.  Keep that distinction at the declaration boundary and let the common
   environment writer do the actual cell growth/copy. */
static bool shell_declare_binding_rejected;
static bool shell_valid_name(string_address name, positive length);
static bool shell_declare_target_valid(const_string value)
{
        positive value_length = string_length(env_reading(value)), base, subscript_length;
        const_string subscript;

        if (!shell_valid_name(env_reading(value), value_length) &&
            !(env_reference_element_span(value, value_length, address_of base,
                 address_of subscript, address_of subscript_length) && subscript_length))
                return string_report(log_error, false, "%s: %s: invalid variable name for name reference\n",
                              shell_argv[0], value);
        return true;
}

static bool shell_declare_binding(const_string name, positive length,
                                  positive hash, const_string value,
                                  bool prepared_integer,
                                  env_variable address_to destination)
{
        positive found = env_find_hashed_span(name, length, hash);
        env_variable address_to variable = destination ? destination
            : found < shell_var_count ? shell_vars + found : null;
        p8 attributes = variable ? variable->attributes : 0;
        bool answer;

        shell_declare_binding_rejected = !prepared_integer &&
                                          !shell_declare_target_valid(value);
        if (shell_declare_binding_rejected)
                return false;
        if (attributes & SHELL_ARRAY_INTEGER)
        {
                if (!env_attribute_value(attributes, value, true))
                        return false;
                // An evaluated integer cannot name a variable. Evaluation's
                // side effects remain, but the old binding is not overwritten.
                shell_declare_binding_rejected = true;
                return false;
        }
        if (variable)
                variable->attributes &= (p8)~SHELL_ARRAY_NAMEREF;
        answer = env_write_destination(name, length, hash, found, value, true, destination);
        found = env_find_hashed_span(name, length, hash);
        variable = destination ? destination : found < shell_var_count ? shell_vars + found : null;
        if (variable)
                variable->attributes = attributes;
        return destination ? answer : env_write_noted(name, length, answer);
}

// Snapshot both operands before evaluation can replace either source cell.
// Integer += adds expressions; the other attributes act on concatenated bytes.
static COLD string_address env_append_value(string_address old, const_string value,
                                             p8 attributes)
{
        bool integer = (attributes & SHELL_ARRAY_INTEGER) != 0;
        old = old ? old : (string_address)"";
        if (integer)
        {
                if (!*arith_skip_space(old))
                        old = (string_address)"0";
                if (!*arith_skip_space(env_reading(value)))
                        value = "0";
        }
        positive left = string_length(old), right = string_length(env_reading(value));
        positive extra = integer ? 6 : 1;
        if (left > positive_max - extra || right > positive_max - left - extra)
                return null;
        p8 address_to made = shell_store_take(address_of expand_store, left + right + extra);
        if (!made)
                return null;
        p8 address_to into = made;
        if (integer)
                *into++ = '(';
        into = memory_copy_end(into, old, left);
        if (integer)
                into = memory_copy_end(into, ")+(", 3);
        into = memory_copy_end(into, env_reading(value), right);
        if (integer)
                *into++ = ')';
        *into = end;
        return made;
}

static COLD bool shell_scalar_assign_destination(const_string name, positive length,
                                      positive hash, const_string value,
                                      bool append, bool bind_reference,
                                      env_variable address_to destination)
{
        shell_mark held;
        p8 address_to joined;
        string_address old;
        p8 attributes = 0;
        bool answer;

        if (bind_reference)
                shell_declare_binding_rejected = false;
        if (!append)
                goto write_value;

        if (bind_reference || (destination &&
            (!(destination->attributes & SHELL_ARRAY_NAMEREF) ||
             !env_variable_has_value(destination))))
        {
                positive found = env_find_hashed_span(name, length, hash);

                env_variable address_to variable = destination ? destination
                    : found < shell_var_count ? shell_vars + found : null;
                attributes = variable ? variable->attributes : 0;
                old = env_variable_value(variable);
                if (!bind_reference && (attributes & SHELL_ARRAY_ASSOCIATIVE))
                {
                        array_location located = array_locate(variable, "0", 1);
                        old = located.found ? array_element_value(array_table_of(variable)->element + located.at) : null;
                }
        }
        else
        {
                env_reference resolved = env_reference_destination(name, length, hash, destination);
                if (!resolved.valid)
                        return false;
                if (resolved.element)
                        return shell_reference_assign_destination(resolved, value, true, destination);
                attributes = resolved.index < shell_var_count
                                 ? shell_vars[resolved.index].attributes : 0;
                old = attributes & SHELL_ARRAY_ASSOCIATIVE
                          ? shell_array_get(resolved.name, resolved.length, "0", 1, null)
                          : resolved.index < shell_var_count
                              ? env_variable_value(shell_vars + resolved.index) : null;
        }

        held = shell_store_mark(address_of expand_store);
        joined = env_append_value(old, value, attributes);

        if (!joined)
        {
                shell_store_rewind(address_of expand_store, held);
                return false;
        }

        value = joined;
write_value:
        answer = bind_reference
                     ? shell_declare_binding(name, length, hash, value,
                                              (attributes & SHELL_ARRAY_INTEGER) != 0, destination)
                     : env_write_destination(name, length, hash,
                         env_find_hashed_span(name, length, hash), value, true, destination);
        if (!destination && !bind_reference)
                answer = env_write_noted(name, length, answer);
        if (append)
                shell_store_rewind(address_of expand_store, held);
        return answer;
}

#define shell_scalar_assign(name, length, hash, value, append, binding) \
        shell_scalar_assign_destination(name, length, hash, value, append, binding, null)

static bool shell_array_set_destination(const_string name, positive length, const_string key,
                     positive key_length, const_string value, bool append,
                     env_variable address_to destination)
{
        env_reference resolved = env_reference_destination(name, length, env_name_hash(name, length), destination);
        positive hash;
        env_variable address_to variable =
            null;
        array_table address_to table;

        if (!resolved.valid)
                return true;
        if (resolved.element)
                return false;

        destination = resolved.destination;
        name = resolved.name;
        length = resolved.length;
        hash = resolved.hash;
        variable = destination ? destination : env_export_take_hashed(name, length, hash);

        if (!variable ||
            (variable->attributes & SHELL_ARRAY_READONLY))
                return false;

        array_location located = array_locate(variable, key, key_length);

        if (!located.keyed)
        {
                // A subscript on a name nobody declared declares it indexed,
                // which is what `a[5]=w` on an unknown name means in Bash.
                variable->attributes |=
                    SHELL_ARRAY_INDEXED | SHELL_ARRAY_ASSIGNED;
                variable->declared = true;
                if (!located.key)
                        return shell_scalar_assign_destination(name, length, hash, value,
                                                   append, false, destination);
        }

        if (append)
                value = env_append_value(located.found
                    ? array_element_value(array_table_of(variable)->element + located.at)
                    : null, value, variable->attributes);
        if (!value)
                return false;
        if (variable->attributes & ENV_ATTRIBUTE_VALUE)
        {
                name = shell_store_copy(address_of expand_store, env_reading(name), length);
                key = shell_store_copy(address_of expand_store, env_reading(key), key_length);
                if (!name || !key ||
                    !(value = env_attribute_value(variable->attributes, value, true)))
                        return false;
                // Arithmetic can grow or replace the variable and element tables.
                variable = destination ? destination : env_export_take_hashed(name, length, hash);
                if (!variable || (variable->attributes & SHELL_ARRAY_READONLY))
                        return false;
                located = array_locate(variable, key, key_length);
        }

        if (!variable->array)
        {
                b32 slot = array_table_take();

                if (!slot)
                        return false;

                variable->array = slot;
        }

        variable->declared = true;
        variable->attributes |= SHELL_ARRAY_ASSIGNED;
        if (!array_table_edit(variable, false))
                return false;
        table = array_table_of(variable);

        return array_element_write(
            table, located.at, !located.found, located.key,
            located.keyed ? key : null, located.keyed ? key_length : 0,
            value, string_length(env_reading(value)));
}

COLD bool shell_array_set(const_string name, positive length, const_string key,
                     positive key_length, const_string value, bool append)
{
        return shell_array_set_destination(name, length, key, key_length, value, append, null);
}

/*
        Every element gone, and the variable still an array.

        This is what `a=(...)` does before it writes the new elements: Bash
        replaces an array rather than merging into it, and the attributes and
        the table itself survive so that a declared kind is not lost with the
        contents.
*/
COLD bool shell_array_clear(const_string name, positive length)
{
        env_reference resolved = env_reference_span(name, length);
        env_variable address_to variable;
        array_table address_to table;

        if (!resolved.valid || resolved.element)
                return false;

        if (resolved.index >= shell_var_count)
                return true;

        name = resolved.name;
        length = resolved.length;
        variable = shell_vars + resolved.index;

        if (variable->attributes & SHELL_ARRAY_READONLY)
                return false;

        if (!array_table_edit(variable, true))
                return false;
        table = array_table_of(variable);

        for (positive at = 0; table && at < table->count; at++)
                env_cell_drop(table->element[at].text);

        if (table)
                table->count = 0;

        variable->attributes |= SHELL_ARRAY_ASSIGNED;

        if (env_variable_has_value(variable))
                return shell_array_forget(name, length, "0", 1);

        return true;
}

/*
        A whole array made at once, from words or from numbers.

        PIPESTATUS, BASH_REMATCH and read -a all make one from a list they
        already hold, and all three replace whatever was there rather than
        merging into it -- which is what an array the shell owns has to do,
        since a script may have left anything in it.
*/
static COLD bool shell_array_replace(const_string name, positive length,
                                     address_any items, positive count,
                                     bool numbers)
{
        p8 written[32];
        p8 number[32];

        if (!shell_variable_attribute_set(name, length,
                                          SHELL_ARRAY_INDEXED |
                                              SHELL_ARRAY_ASSIGNED,
                                          SHELL_ARRAY_ASSOCIATIVE) ||
            !shell_array_clear(name, length))
                return false;

        for (positive at = 0; at < count; at++)
        {
                string_address value;
                if (numbers)
                {
                        number[bipolar_into_string(number,
                            ((bipolar address_to)items)[at])] = end;
                        value = number;
                }
                else
                        value = ((string_address address_to)items)[at];

                if (!shell_array_set(name, length, written,
                                     bipolar_into_string(written, (bipolar)at),
                                     value, false))
                        return false;
        }

        return true;
}

COLD bool shell_array_words(const_string name, positive length,
                             string_address address_to words, positive count)
{
        return shell_array_replace(name, length, words, count, false);
}

COLD bool shell_array_numbers(const_string name, positive length,
                               bipolar address_to values, positive count)
{
        return shell_array_replace(name, length, values, count, true);
}

/* Bash's PIPESTATUS keeps a sole explicitly selected nonzero subscript as
   the slot for the next status. A vector continues at indexes one onward;
   index one naturally overwrites a selected index one. Everything else is
   the ordinary replacing array writer above. */
static COLD bool shell_status_array_numbers(const_string name, positive length,
                                             bipolar address_to values,
                                             positive count)
{
        shell_array_item item;
        p8 key[32];
        p8 number[32];

        if (!count || shell_array_length(name, length) != 1 ||
            shell_array_items(name, length, address_of item, 1) != 1 ||
            item.key || !item.index)
                return shell_array_numbers(name, length, values, count);

        for (positive at = 0; at < count; at++)
        {
                positive index = at ? at : item.index;

                number[bipolar_into_string(number, values[at])] = end;
                if (!shell_array_set(name, length, key,
                                     positive_into_string(key, index), number,
                                     false))
                        return false;
        }

        return true;
}

static COLD bool shell_array_forget_mode(const_string name, positive length,
                                         const_string key,
                                         positive key_length,
                                         bool allow_readonly)
{
        env_reference resolved = env_reference_span(name, length);
        if (!resolved.valid || resolved.element ||
            resolved.index >= shell_var_count)
                return true;

        name = resolved.name;
        length = resolved.length;
        env_variable address_to variable = shell_vars + resolved.index;
        if (!allow_readonly && (variable->attributes & SHELL_ARRAY_READONLY))
                return false;

        array_location located = array_locate(variable, key, key_length);
        if (!located.keyed && !located.key)
        {
                // Indexed zero is the scalar cell. Keep the name and its
                // attributes, taking owned storage before changing inherited text.
                if (!env_variable_has_value(variable))
                        return true;
                if (variable->owned)
                        variable->text[length] = end;
                else
                {
                        env_cell address_to cell = env_cell_take(length + 1);
                        if (!cell)
                                return false;
                        memory_copy_end((p8 address_to)(cell + 1),
                                        (address_any)name, length);
                        variable->text = (string_address)(cell + 1);
                        variable->owned = true;
                }
                variable->value_length = 0;
                shell_envp_dirty = true;
        }
        else if (located.found)
        {
                if (!array_table_edit(variable, false))
                        return false;
                array_element_forget(array_table_of(variable), located.at);
        }
        return true;
}

/*
        The variables that are not stored anywhere.

        RANDOM, SECONDS, EPOCHREALTIME and the rest have no record in the
        table: they are answered from a clock, a counter or a syscall at the
        moment they are named. That is not only smaller, it is the only way
        they can cost an ordinary lookup nothing -- a table with twenty more
        names in it is twenty more names every miss walks past, and a script
        that never writes RANDOM should not pay for the ones that do.

        So the whole family hangs off the miss: the expander looks a name up,
        does not find it, and only then asks here. A name that is not one of
        these leaves with one length comparison.
*/
#define SHELL_CLOCK_REALTIME 0
#define SHELL_CLOCK_MONOTONIC 1

static COLD p64 shell_clock_seconds(positive which, p64 address_to nanoseconds)
{
        timespec now = {0, 0};

        system_call_2(syscall(clock_gettime), which, (positive)address_of now);

        if (nanoseconds)
                address_to nanoseconds = now.tv_nsec;

        return now.tv_sec;
}

/*
        Bash's generator, which a script is allowed to depend on.

        Park and Miller's minimal standard, folded to fifteen bits by
        exclusive-or of the two halves rather than by truncation -- the fold
        is what makes RANDOM=4 answer 1693 where the low bits alone would say
        1692. A reseed forgets the last value, and a value equal to the last
        one is drawn again, both of which Bash does and both of which a
        sequence pinned against it can see.
*/
static positive shell_random_seed;
static positive shell_random_last;
static bool shell_random_started;

static COLD positive shell_random_step()
{
        p32 seed = (p32)(shell_random_seed ? shell_random_seed : 123459876);
        b32 high = (b32)(seed / 127773);
        b32 low = (b32)(seed - (p32)high * 127773);
        b32 next = 16807 * low - 2836 * high;

        if (next < 0)
                next += 0x7fffffff;

        shell_random_seed = (positive)(p32)next;

        return (((positive)(p32)next >> 16) ^ ((positive)(p32)next & 65535)) &
               0x7fff;
}

static COLD positive shell_random_next()
{
        positive value;

        // Started from the clock and the pid, because two shells begun in the
        // same second must not walk the same sequence.
        if (!shell_random_started)
        {
                shell_random_started = true;
                shell_pid_ensure();
                shell_random_seed =
                    (positive)(p32)(shell_clock_seconds(SHELL_CLOCK_REALTIME,
                                                        null) *
                                        1103515245 +
                                    (p64)expand_shell_pid);
        }

        do
                value = shell_random_step();
        while (value == shell_random_last);

        return shell_random_last = value;
}

/*
        Where SECONDS counts from, and when the shell began.

        Both are taken the first time either is asked for rather than at
        startup: reading two clocks costs two syscalls, and a shell that is
        never asked what time it is should not make them. What that gives up
        is the seconds before the first question, which no script can observe
        -- it has to ask to find out, and asking is what sets the origin.

        Assigning SECONDS moves the origin rather than storing a number, which
        is what makes it keep counting afterwards.
*/
static p64 shell_seconds_origin;
static p64 shell_started_seconds;
static bool shell_seconds_started;

static COLD fn shell_seconds_begin()
{
        if (shell_seconds_started)
                return;

        shell_seconds_started = true;
        shell_seconds_origin = shell_clock_seconds(SHELL_CLOCK_MONOTONIC, null);
        shell_started_seconds = shell_clock_seconds(SHELL_CLOCK_REALTIME, null);
}

static COLD p64 shell_seconds_now()
{
        shell_seconds_begin();

        return shell_clock_seconds(SHELL_CLOCK_MONOTONIC, null) -
               shell_seconds_origin;
}

//      One name's answer at a time, so the caller may keep a span of it until
//      it pushes the bytes into the word it is building.
static p8 shell_dynamic_text[80];

static COLD string_address shell_dynamic_number(positive value,
                                           positive address_to value_length)
{
        positive length = positive_into_string(shell_dynamic_text, value);

        shell_dynamic_text[length] = end;

        if (value_length)
                address_to value_length = length;

        return shell_dynamic_text;
}

static COLD string_address shell_dynamic_said(string_address text,
                                         positive address_to value_length)
{
        if (value_length)
                address_to value_length = string_length(text);

        return text;
}

//      uname's answer, read once. HOSTNAME and MACHTYPE both want a field of
//      it and neither is worth a second syscall.
static p8 shell_machine_node[65];
static bool shell_machine_read;

static COLD string_address shell_machine_name()
{
        file_machine facts;

        if (shell_machine_read)
                return shell_machine_node;

        shell_machine_read = true;
        memory_fill(address_of facts, 0, sizeof(facts));

        if (system_call_1(syscall(uname), (positive)address_of facts) >= 0)
                string_copy_max_end(shell_machine_node, facts.node,
                                    sizeof(shell_machine_node) - 1);

        return shell_machine_node;
}

/*
        The last argument of the command before this one.

        Bash writes it after the words of the command about to run have been
        expanded and before that command runs, which is why `echo a b; echo $_`
        says b. The bytes are copied because the words they came out of are
        the token store, and the next command writes over it.
*/
static p8 shell_last_argument[256] = "";

fn shell_last_argument_set(string_address word)
{
        positive at = 0;

        if (!word)
                return;

        /*
                A byte at a time on purpose.

                This runs once for every command the shell executes and the
                thing being copied is a command's last argument -- a handful
                of bytes almost always. The general copy is faster per byte
                and slower per call, and per call is what this is.
        */
        while (at + 1 < sizeof(shell_last_argument) && word[at])
        {
                shell_last_argument[at] = word[at];
                at++;
        }

        shell_last_argument[at] = end;
}

//      The three arrays. Made the first time one is named, the same way the
//      call-stack arrays are, because each costs a table of elements that a
//      script which never mentions them has no use for.
static bool shell_dynamic_arrays[3];

#define SHELL_DYNAMIC_VERSINFO 0
#define SHELL_DYNAMIC_GROUPS 1
#define SHELL_DYNAMIC_DIRSTACK 2

string_address address_to shell_dirstack_entries(positive address_to count);

static COLD fn shell_dynamic_versinfo()
{
        static string_address parts[] = {"5",       "3",
                                         "15",      "1",
                                         "release", MOONWATER_MACHTYPE};

        shell_array_words("BASH_VERSINFO", 13, parts, array_count(parts));
}

static COLD fn shell_dynamic_groups()
{
        b32 held[64];
        string_address said[64];
        p8 written[64 * 12];
        bipolar count = system_call_2(syscall(getgroups), array_count(held),
                                      (positive)held);
        positive used = 0;

        if (count < 0)
                count = 0;

        for (bipolar at = 0; at < count; at++)
        {
                said[at] = written + used;
                used += positive_into_string(written + used,
                                             (positive)(p32)held[at]);
                written[used++] = end;
        }

        shell_array_words("GROUPS", 6, said, (positive)count);
}

static COLD fn shell_dynamic_dirstack()
{
        positive count = 0;
        string_address address_to entries = shell_dirstack_entries(
            address_of count);

        shell_array_words("DIRSTACK", 8, entries, count);
}

/*
        Whether a name is one of the three arrays, and if it is, making it.

        The expander asks before it reads an array, exactly as it asks about
        the call-stack arrays, so a name that is not one of these costs three
        length comparisons on a path that has already missed.
*/
COLD bool shell_dynamic_wanted(const_string name, positive length)
{
        positive which;

        if (shell_pipe_status_wanted(name, length))
                return true;

        if (length == 13 &&
            shell_bash_compat &&
            !memory_compare((address_any)name, "BASH_VERSINFO", 13))
                which = SHELL_DYNAMIC_VERSINFO;
        else if (length == 6 && !memory_compare((address_any)name, "GROUPS", 6))
                which = SHELL_DYNAMIC_GROUPS;
        else if (length == 8 &&
                 !memory_compare((address_any)name, "DIRSTACK", 8))
                which = SHELL_DYNAMIC_DIRSTACK;
        else
                return false;

        // DIRSTACK is remade every time, because pushd and popd change it and
        // the array is the answer rather than a cache of one.
        if (shell_dynamic_arrays[which] && which != SHELL_DYNAMIC_DIRSTACK)
                return true;

        shell_dynamic_arrays[which] = true;

        if (which == SHELL_DYNAMIC_VERSINFO)
                shell_dynamic_versinfo();
        else if (which == SHELL_DYNAMIC_GROUPS)
                shell_dynamic_groups();
        else
                shell_dynamic_dirstack();

        return true;
}

COLD bool shell_array_forget(const_string name, positive length,
                             const_string key, positive key_length)
{
        return shell_array_forget_mode(name, length, key, key_length, false);
}

/* Bash permits `unset n` when n itself names one array element, even when
   the containing array is readonly.  Direct `unset a[key]` still reaches the
   ordinary wrapper above and refuses the mutation. */
static COLD bool shell_reference_element_forget(env_reference resolved)
{
        positive key_length;
        string_address key = shell_expand_subscript(
            (string_address)resolved.name, resolved.length,
            (string_address)resolved.subscript, resolved.subscript_length,
            address_of key_length);

        return key && shell_array_forget_mode(
                          resolved.name, resolved.length, key, key_length,
                          true);
}

positive shell_subshell_depth;

COLD string_address shell_dynamic_value(const_string name, positive length,
                                        positive address_to value_length)
{
        string_address text = env_reading(name);

        if (length == 1)
        {
                if (text[0] == '_')
                        return shell_dynamic_said(shell_last_argument,
                                                  value_length);

                return null;
        }

        // Grouped by length first: every one of these is a miss for almost
        // every name that reaches here, and a length that matches nothing
        // leaves without looking at a byte.
        switch (length)
        {
        case 3:
                if (!memory_compare((address_any)text, "UID", 3))
                        return shell_dynamic_number(
                            (positive)system_call_1(syscall(getuid), 0),
                            value_length);
                break;

        case 4:
                if (!memory_compare((address_any)text, "EUID", 4))
                        return shell_dynamic_number(
                            (positive)system_call_1(syscall(geteuid), 0),
                            value_length);

                if (!memory_compare((address_any)text, "PPID", 4))
                        return shell_dynamic_number(
                            (positive)system_call_1(syscall(getppid), 0),
                            value_length);
                break;

        case 6:
                if (!memory_compare((address_any)text, "RANDOM", 6))
                        return shell_dynamic_number(shell_random_next(),
                                                    value_length);

                /* The line the running command was written on, which the
                   executor keeps, and not the line the reader has reached:
                   inside a function body those are different, and bash
                   answers with the first. */
                if (!memory_compare((address_any)text, "LINENO", 6))
                        return shell_dynamic_number(shell_line_now(),
                                                    value_length);

                if (!memory_compare((address_any)text, "OSTYPE", 6))
                        return shell_dynamic_said("linux-gnu", value_length);
                break;

        case 7:
                if (!memory_compare((address_any)text, "SECONDS", 7))
                        return shell_dynamic_number(
                            (positive)shell_seconds_now(), value_length);

                if (!memory_compare((address_any)text, "SRANDOM", 7))
                {
                        p32 value = 0;

                        if (system_call_3(syscall(getrandom),
                                          (positive)address_of value,
                                          sizeof(value), 0) !=
                            (bipolar)sizeof(value))
                                value = (p32)(shell_random_next() << 16) ^
                                        (p32)shell_random_next();

                        return shell_dynamic_number((positive)value,
                                                    value_length);
                }

                if (!memory_compare((address_any)text, "BASHPID", 7))
                        return shell_dynamic_number(
                            (positive)system_call_1(syscall(getpid), 0),
                            value_length);
                break;

        case 8:
                if (!memory_compare((address_any)text, "HOSTNAME", 8))
                        return shell_dynamic_said(shell_machine_name(),
                                                  value_length);

                if (!memory_compare((address_any)text, "HOSTTYPE", 8))
                        return shell_dynamic_said(MOONWATER_HOSTTYPE,
                                                  value_length);

                if (!memory_compare((address_any)text, "MACHTYPE", 8))
                        return shell_dynamic_said(MOONWATER_MACHTYPE,
                                                  value_length);

                if (shell_bash_compat &&
                    !memory_compare((address_any)text, "BASHOPTS", 8))
                        return shell_optlist_value(true, value_length);
                break;

        case 9:
                if (shell_bash_compat &&
                    !memory_compare((address_any)text, "SHELLOPTS", 9))
                        return shell_optlist_value(false, value_length);
                break;

        case 12:
                if (shell_bash_compat &&
                    !memory_compare((address_any)text, "BASH_VERSION", 12))
                        return shell_dynamic_said("5.3.15(1)-release",
                                                  value_length);

                if (!memory_compare((address_any)text, "EPOCHSECONDS", 12))
                        return shell_dynamic_number(
                            (positive)shell_clock_seconds(SHELL_CLOCK_REALTIME,
                                                          null),
                            value_length);
                break;

        case 13:
                if (!memory_compare((address_any)text, "BASH_SUBSHELL", 13))
                        return shell_dynamic_number(shell_subshell_depth,
                                                    value_length);

                if (!memory_compare((address_any)text, "EPOCHREALTIME", 13))
                {
                        p64 nanoseconds = 0;
                        p64 seconds = shell_clock_seconds(
                            SHELL_CLOCK_REALTIME, address_of nanoseconds);
                        positive used = positive_into_string(
                            shell_dynamic_text, (positive)seconds);

                        // Six digits after the point, zero-filled, which is
                        // what a script cutting on the dot is measuring in.
                        shell_dynamic_text[used++] = '.';

                        for (positive place = 100000; place; place /= 10)
                        {
                                shell_dynamic_text[used++] =
                                    (p8)('0' + (p8)((nanoseconds / 1000 /
                                                     place) %
                                                    10));
                        }

                        shell_dynamic_text[used] = end;

                        if (value_length)
                                address_to value_length = used;

                        return shell_dynamic_text;
                }
                break;

        default:
                break;
        }

        return null;
}

/*
        An assignment to one of them.

        RANDOM and SECONDS are the two Bash lets a script write, and neither
        stores what it was given: one reseeds and the other moves the origin.
        Saying so here keeps the name out of the table, which is what keeps
        the next read dynamic instead of finding a stale number.
*/
COLD bool shell_dynamic_assign(const_string name, positive length,
                               const_string value)
{
        string_address text = env_reading(name);
        string_address said = env_reading(value);

        if (length == 6 && !memory_compare((address_any)text, "RANDOM", 6))
        {
                bool good;
                bipolar asked = shell_signed(said, address_of good);

                shell_random_started = true;
                shell_random_seed = good ? (positive)(p32)asked : 0;
                shell_random_last = 0;

                return true;
        }

        if (length == 7 && !memory_compare((address_any)text, "SECONDS", 7))
        {
                bool good;
                bipolar asked = shell_signed(said, address_of good);

                shell_seconds_started = true;
                shell_seconds_origin =
                    shell_clock_seconds(SHELL_CLOCK_MONOTONIC, null) -
                    (good ? (p64)asked : 0);

                return true;
        }

        return false;
}

// string_to_positive scans backwards from the end of the string, so it reads
// "0.5" as 5 and anything with a trailing space as 0. Arguments arrive as
// whole words here and have to be read forwards.
positive shell_number(string_address input)
{
        return input ? string_digits(input, 0) : 0;
}

/*
        A value written so it can be read back.

        Single quoted, and a run of single quotes inside it closed, put in
        double quotes and opened again: 'it'"'"'s'. That is the reference
        shell's spelling to the byte -- a quote at the very end leaves no
        empty '' behind it -- because anything printed by set, export -p,
        readonly -p, trap or alias is meant to be a line the shell could be
        fed, and a script that diffs two shells' listings should see none.
*/
fn shell_quoted(writer write, string_address value)
{
        if (!value)
                value = "";
        if (shell_bash_compat && string_is(value, '\'') && !string_get(value + 1))
                return write("\\'", 2);

        while (1)
        {
                string_address stop = string_first_of_or_end(value, '\'');
                positive quotes = 0;

                write("'", 1);

                if (stop > value)
                        write(value, (positive)(stop - value));

                write("'", 1);
                value = stop;

                while (string_is(value + quotes, '\''))
                        quotes++;

                if (!quotes)
                        return;

                if (shell_bash_compat)
                        for (positive at = 0; at < quotes; at++)
                        {
                                if (at)
                                        write("''", 2);
                                write("\\'", 2);
                        }
                else
                {
                        write("\"", 1);
                        write(value, quotes);
                        write("\"", 1);
                }
                value += quotes;

                if (!string_get(value) && !shell_bash_compat)
                        return;
        }
}

static bool shell_valid_name(string_address name, positive length)
{
        return length && !byte_is_digit(string_get(name)) &&
               string_span_max(name, length, string_set_name) == length;
}

/*
        The next option letter, and where the words stop being options.

        Builtins used to walk their -abc words with separate loops, and one of
        them fell off the end of it in silence: `command -x foo` skipped the
        word it did not understand and ran foo. This is that loop once.
        Answers false with index on the first operand: at a word that does
        not begin with a dash, at a lone dash, past "--" (which is stepped
        over), or at the end of argv. A builtin that takes + words as well
        says so with plus_too, and direction says which sign the letter came
        under.

        An option that takes the rest of its word as a value (read -n3) takes
        it from `rest` and moves rest to the end of it; one that takes the
        next word steps index onto that word and does the same. Either way
        the walk carries on with the word after.
*/
typedef struct
{
        positive index;
        string_address rest;
        p8 direction;
        bool plus_too;
} shell_option_walk;

static inline INLINE bool shell_option_letter(shell_option_walk address_to walk,
                                              p8 address_to letter)
{
        while (1)
        {
                string_address word;

                if (walk->rest && string_get(walk->rest))
                {
                        address_to letter = string_get(walk->rest++);
                        return true;
                }

                if (walk->rest)
                {
                        walk->rest = null;
                        walk->index++;
                }

                if (walk->index >= shell_argc)
                        return false;

                word = shell_argv[walk->index];
                walk->direction = string_get(word);

                if ((walk->direction != '-' &&
                     (walk->direction != '+' || !walk->plus_too)) ||
                    !string_get(word + 1))
                        return false;

                if (word_is(word, "--"))
                {
                        walk->index++;
                        return false;
                }

                walk->rest = word + 1;
        }
}

static string_address shell_option_argument(shell_option_walk address_to walk)
{
        string_address value = walk->rest;
        if (!value || !*value)
                value = ++walk->index < shell_argc ? shell_argv[walk->index] : null;
        walk->rest = null;
        walk->index++;
        return value;
}

// The one walk over every name the shell holds, in order. It lives with
// declare below, which is what it was written for; set, export and readonly
// list through it too so that the four agree on the order.
typedef fn(address_to shell_name_writer)(writer write, string_address name,
                                         positive length, b32 mark);
static inline INLINE bool shell_inventory_sorted(
    writer write, b32 mark, shell_name_writer written, bool functions, bool bodies);

/*
        The directory the shell says it is in, which is not always the one the
        kernel would name.

        A symlink walked into keeps its own name here: "cd link" then "cd .."
        goes back to where the link was, not to the parent of what it pointed
        at. That is the -L rule, it is the default, and it needs the path
        remembered rather than asked for -- getcwd has already forgotten it.
*/
/* Four bytes immediately before the logical directory spell its assignment
   name. An empty-environment shell can therefore publish the getcwd result as
   `PWD=value` without a second buffer or allocation. On the first cd or PWD
   assignment the ordinary borrowed-record COW path takes ownership. */
static p8 shell_directory_assignment[SHELL_DIRECTORY_MAX + 4];
p8 address_to shell_directory = shell_directory_assignment + 4;
static p8 shell_directory_was[SHELL_DIRECTORY_MAX];
static PURE bool shell_physical_on();

bool shell_here(p8 address_to into, positive room)
{
        into[0] = end;
        return system_call_2(syscall(getcwd), (positive)into, room) >= 0;
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
               file_device_key(named.device_major, named.device_minor) ==
               file_device_key(here.device_major, here.device_minor);
}

bool shell_directory_moved(string_address logical)
{
        string_copy_max_end(shell_directory_was, shell_directory,
                            sizeof(shell_directory_was) - 1);

        string_copy_max_end(shell_directory, logical, SHELL_DIRECTORY_MAX - 1);

        /* Bash updates PWD even when readonly OLDPWD rejects its assignment;
           dash retains its historical short circuit. The directory has
           already changed in either case. */
        if ((env_assign("OLDPWD", shell_directory_was)
            || string_report(log_error, false, env_readonly("OLDPWD")
                ? "cd: %s: is read only\n"
                : "cd: cannot assign %s\n", "OLDPWD")))
                return (env_assign("PWD", shell_directory)
                    || string_report(log_error, false, env_readonly("PWD")
                        ? "cd: %s: is read only\n"
                        : "cd: cannot assign %s\n", "PWD"));

        if (shell_bash_compat)
                (env_assign("PWD", shell_directory)
                    || string_report(log_error, false, env_readonly("PWD")
                        ? "cd: %s: is read only\n"
                        : "cd: cannot assign %s\n", "PWD"));

        return false;
}

static p8 shell_cd_target[4096];

static bipolar shell_cd_reason;

bool shell_cd_try(string_address candidate, bool physical,
                  bool address_to physical_named,
                  bool address_to variables_set,
                  string_address unnamed)
{
        p8 wanted[4096];

        string_copy_max_end(wanted, candidate, sizeof(wanted) - 1);

        if (!physical)
                shell_path_tidy(wanted);

        {
                bipolar answer = system_change_directory(wanted);

                //      Kept for the diagnostic: bash says why it could not
                //      go there, and the last name tried is the one it
                //      names, whether it came from the operand or CDPATH.
                if (answer)
                {
                        shell_cd_reason = answer < 0 ? -answer : answer;

                        return false;
                }
        }

        if (physical && !shell_here(wanted, sizeof(wanted)))
        {
                /* Bash permits `cd -P` without -e when getcwd cannot name an
                   otherwise successful chdir. Retain the requested spelling
                   as PWD; -e decides whether that unnamed success is an
                   error. shell_here clears its destination on failure. */
                if (shell_bash_compat)
                        string_copy_max_end(wanted,
                                            unnamed ? unnamed : candidate,
                                            sizeof(wanted) - 1);
                address_to physical_named = false;
        }
        else
                address_to physical_named = true;

        address_to variables_set = shell_directory_moved(wanted);

        return true;
}

/*
        Where the name asked for actually is.

        An absolute name is itself; a name beginning with a dot is under the
        directory the shell is in and nothing else; anything else is looked for
        along CDPATH first, and a hit there is said out loud because the script
        did not name the place it landed.
*/
static PURE bool shell_privileged_on();

bool shell_cd_walk(bool physical, bool address_to say,
                   bool address_to physical_named,
                   bool address_to variables_set)
{
        p8 candidate[4096];

        if (shell_cd_target[0] == '/')
                return shell_cd_try(shell_cd_target, physical, physical_named,
                                    variables_set, null);

        if (!(shell_cd_target[0] == '.' &&
              (shell_cd_target[1] == end || shell_cd_target[1] == '/' ||
               (shell_cd_target[1] == '.' &&
                (shell_cd_target[2] == end || shell_cd_target[2] == '/')))))
        {
                p8 search[1024];
                /* Bash leaves inherited CDPATH text visible in privileged
                   mode but does not let cd interpret it until set +p. Keep
                   the value in the shared environment engine and gate only
                   its special consumer. */
                string_address value = shell_privileged_on()
                                           ? null
                                           : env_get("CDPATH");

                if (value && string_get(value))
                {
                        path_walk walk = {search, null, 0, false};
                        p8 under[4096];

                        string_copy_max_end(search, value, sizeof(search) - 1);

                        while (path_walk_next(address_of walk))
                        {
                                string_address base = candidate;

                                /*
                                        An entry is a directory to look under.
                                        An empty entry is the directory
                                        itself, and the one case that is not
                                        said out loud afterwards, because the
                                        script named the place it landed. A
                                        relative one is relative to where
                                        the shell is: joined onto that, or
                                        PWD would be left holding "one/two"
                                        and every cd .. after it would be
                                        lost.
                                */
                                if (!path_walk_join(candidate, sizeof(candidate),
                                                    walk.segment, walk.length,
                                                    shell_cd_target,
                                                    shell_directory))
                                        continue;

                                if (walk.length && walk.segment[0] != '/')
                                {
                                        if (!path_walk_join(
                                                under, sizeof(under),
                                                shell_directory,
                                                string_length(shell_directory),
                                                candidate, ""))
                                                continue;

                                        base = under;
                                }

                                if (shell_cd_try(base, physical,
                                                 physical_named,
                                                 variables_set, null))
                                {
                                        if (walk.length)
                                        {
                                                address_to say = true;
                                                /* cd says the CDPATH spelling,
                                                   not getcwd's physical answer. */
                                                string_copy_end(shell_cd_target,
                                                                base);
                                        }

                                        return true;
                                }
                        }
                }
        }

        /* path_join is the tuned ordinary path. Only its maximum-length
           answer is ambiguous between an exact fit and truncation, so send
           that cold boundary through the shared checked walker. */
        if (path_join(candidate, sizeof(candidate), shell_directory,
                      shell_cd_target) == sizeof(candidate) - 1 &&
            !path_walk_join(candidate, sizeof(candidate), shell_directory,
                            string_length(shell_directory), shell_cd_target,
                            ""))
                return false;

        return shell_cd_try(physical ? shell_cd_target : candidate, physical,
                            physical_named, variables_set,
                            physical ? candidate : null);
}

COLD fn shell_cd(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];

        //      rbash: the working directory is the first thing a restricted
        //      shell keeps, because everything reached by a relative name
        //      follows from it.
        if (shell_restricted)
        {
                shell_diagnostic_where();
                return shell_answer(
                    string_report(log_error, 1, "cd: restricted\n"));
        }

        shell_option_walk walk = {1};
        p8 letter;
        bool physical = shell_physical_on();
        bool error_if_unnamed = false;
        bool physical_named = true;
        bool variables_set = true;
        string_address name = null;
        bool say = false;

        if (!shell_directory_holds())
        {
                /* A removed current directory makes getcwd fail. Keep the
                   last valid logical spelling in that case: relative cd and
                   Bash's non-exact `cd -P` still operate from it. */
                if (shell_here(shell_directory_was,
                               sizeof(shell_directory_was)))
                {
                        string_copy_end(shell_directory, shell_directory_was);
                        env_assign("PWD", shell_directory);
                }
        }

        while (shell_option_letter(address_of walk, address_of letter))
        {
                if (letter == 'L')
                        physical = false;
                else if (letter == 'P')
                        physical = true;
                //      -e and -@ are Bash's; dash has -L and -P and calls
                //      every other letter an illegal option.
                else if (letter == 'e' && shell_bash_compat)
                        error_if_unnamed = true;
                else
                        return shell_answer(shell_letter_refused(
                            "cd", letter,
                            "cd [-L|[-P [-e]]] [-@] [dir]"));
        }

        positive index = walk.index;
        if (index < shell_argc)
                name = shell_argv[index++];

        //      Bash counts the operands and refuses a second; dash reads
        //      the first and pays no attention to what follows it.
        if (index < shell_argc && shell_bash_compat)
        {
                shell_diagnostic_where();

                return shell_answer(string_report(log_error, 2,
                    "cd: too many arguments\n"));
        }

        if (!name)
        {
                name = env_get("HOME");

                if (!name || !string_get(name))
                {
                        if (shell_bash_compat)
                                return shell_answer(string_report(log_error, 1, "cd: HOME not set\n"));

                        return shell_answer(0);
                }
        }
        else if (!string_get(name))
        {
                //      An empty name is a null directory to Bash and nothing
                //      at all to dash, which stays where it is and says so
                //      with a zero.
                if (!shell_bash_compat)
                        return shell_answer(0);

                log_error("cd: empty directory\n", 0);
                return shell_answer(1);
        }
        else if (word_is(name, "-"))
        {
                name = env_get("OLDPWD");
                say = true;

                if (!name)
                {
                        if (shell_bash_compat)
                                return shell_answer(string_report(log_error, 1, "cd: OLDPWD not set\n"));

                        name = shell_directory;
                }
        }

        // On a copy: both HOME and OLDPWD point into env_storage, which the
        // first env_set below is free to move out from under them.
        {
                string_address copied = string_copy_max_end(
                    shell_cd_target, name, sizeof(shell_cd_target) - 1);

                if (string_get(name + (copied - shell_cd_target)))
                {
                        shell_answer(shell_bash_compat ? 1 : 2);
                        return log_error(str("cd: directory name too long\n"));
                }
        }

        if (!shell_cd_walk(physical, address_of say,
                           address_of physical_named,
                           address_of variables_set))
        {
                shell_answer(shell_bash_compat ? 1 : 2);

                //      dash says only that it could not; bash says what the
                //      kernel said, which is the difference between a name
                //      that is not there and one that is not readable.
                shell_diagnostic_where();

                if (!shell_bash_compat)
                        return string_format(log_error,
                                             "cd: can't cd to %s\n",
                                             shell_cd_target);

                {
                        string_address why =
                            system_error_message(shell_cd_reason);

                        return string_format(log_error, "cd: %s: %s\n",
                                             shell_cd_target,
                                             why ? why
                                                 : (string_address)
                                                   "No such file or "
                                                   "directory");
                }
        }

        if (!variables_set)
                return shell_answer(shell_bash_compat ? 1 : 2);

        if (say)
                string_format(write, "%s\n", shell_cd_target);

        shell_answer(error_if_unnamed && physical && !physical_named ? 1 : 0);
}

/*
        The directory stack: pushd, popd and dirs.

        Only what is under the top is kept. The top is the directory the shell
        is in, read from where cd already keeps it, which is what makes `cd -`
        move the top of the stack without pushd knowing anything about it --
        Bash behaves that way too, and a stack that stored its own copy of the
        top would disagree with pwd the moment anything else moved.

        A rotation and a removal both rewrite the kept part, so both go
        through one writer that copies aside first: the pointers handed to it
        are usually into the very bytes it is about to overwrite.
*/
#define SHELL_DIRSTACK_MAX 32
#define SHELL_DIRSTACK_BYTES 8192

static p8 shell_dirstack_pool[SHELL_DIRSTACK_BYTES];
static positive shell_dirstack_at[SHELL_DIRSTACK_MAX];
static positive shell_dirstack_count;
static positive shell_dirstack_used;
static string_address shell_dirstack_list[SHELL_DIRSTACK_MAX + 1];

COLD string_address address_to shell_dirstack_entries(positive address_to count)
{
        shell_dirstack_list[0] = shell_directory;

        for (positive at = 0; at < shell_dirstack_count; at++)
                shell_dirstack_list[at + 1] =
                    shell_dirstack_pool + shell_dirstack_at[at];

        address_to count = shell_dirstack_count + 1;

        return shell_dirstack_list;
}

static COLD bool shell_dirstack_write(string_address address_to kept,
                                 positive count)
{
        static p8 scratch[SHELL_DIRSTACK_BYTES];
        positive used = 0;

        if (count > SHELL_DIRSTACK_MAX)
                return false;

        for (positive at = 0; at < count; at++)
        {
                positive length = string_length(kept[at]);

                if (used + length + 1 > sizeof(scratch))
                        return false;

                memory_copy(scratch + used, kept[at], length + 1);
                shell_dirstack_at[at] = used;
                used += length + 1;
        }

        memory_copy(shell_dirstack_pool, scratch, used);
        shell_dirstack_used = used;
        shell_dirstack_count = count;

        return true;
}

//      Bash writes $HOME as a tilde in every listing but -l, which is what
//      makes the line short enough to read on a terminal.
static COLD fn shell_dirstack_said(writer write, string_address path, bool full)
{
        string_address home = full ? null : env_get("HOME");
        positive home_length = home ? string_length(home) : 0;
        positive length = string_length(path);

        if (home_length > 1 && length >= home_length &&
            !memory_compare(path, home, home_length) &&
            (length == home_length || path[home_length] == '/'))
        {
                write("~", 1);
                write(path + home_length, length - home_length);
                return;
        }

        write(path, length);
}

//      +N counts from the top and -N from the bottom, and neither is an
//      option letter however much it looks like one.
static COLD bool shell_dirstack_index(string_address word, positive count,
                                 positive address_to index)
{
        positive digits;
        positive value;

        if (!string_get(word) ||
            (string_not(word, '+') && string_not(word, '-')))
                return false;

        value = string_digits(word + 1, address_of digits);

        if (!digits || string_get(word + 1 + digits) || value >= count)
                return false;

        address_to index = string_is(word, '+') ? value : count - 1 - value;

        return true;
}

//      A sign and digits and nothing else is a stack index. Anything else
//      with one of those signs in front is a number the reference will not
//      read, and it says so and answers two rather than looking for an entry
//      it was never given the number of.
static PURE bool shell_dirstack_spec(string_address word)
{
        positive digits;

        if (string_not(word, '+') && string_not(word, '-'))
                return false;

        string_digits(word + 1, address_of digits);

        return digits && !string_get(word + 1 + digits);
}

static COLD b32 shell_dirstack_number_refused(string_address command,
                                              string_address word,
                                              string_address usage)
{
        shell_diagnostic_where();
        string_report(log_error, 2, "%s: %s: invalid number\n", command, word);

        return string_report(log_error, 2, "%s: usage: %s\n", command, usage);
}

static COLD fn shell_dirstack_listed(writer write, bool full, bool numbered,
                                bool lines)
{
        positive count;
        string_address address_to list = shell_dirstack_entries(
            address_of count);

        for (positive at = 0; at < count; at++)
        {
                if (numbered)
                {
                        p8 written[32];
                        positive digits = positive_into_string(written,
                                                               (positive)at);

                        // Right in a field of two, which is what lines up the
                        // paths under one another past nine entries.
                        while (digits < 2)
                        {
                                write(" ", 1);
                                digits++;
                        }

                        write(written, positive_into_string(written,
                                                            (positive)at));
                        write("  ", 2);
                }
                else if (at && !lines)
                        write(" ", 1);

                shell_dirstack_said(write, list[at], full);

                if (numbered || lines)
                        write("\n", 1);
        }

        if (!numbered && !lines)
                write("\n", 1);
}

COLD fn shell_dirs(writer write, string_address input)
{
        positive index = 1;
        bool full = false;
        bool numbered = false;
        bool lines = false;
        positive count;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive at;

                if (!string_is(word, '-') || !string_get(word + 1))
                        break;

                //      Nothing after it is read at all, not even an index:
                //      "dirs -- +0" writes the whole stack.
                if (word_is(word, "--"))
                {
                        index = shell_argc;
                        break;
                }

                // -1 is the entry one from the bottom, not an option word,
                // so a digit ends the option scan.
                if (word[1] >= '0' && word[1] <= '9')
                        break;

                //      One letter to a word: bash reads "-lp" as a stack
                //      index and refuses it as a number, where every other
                //      builtin would have taken the two letters.
                if (word[2])
                        return shell_answer(
                            shell_bash_compat
                                ? shell_dirstack_number_refused(
                                      "dirs", word, "dirs [-clpv] [+N] [-N]")
                                : shell_option_refused(
                                      "dirs", word,
                                      "dirs [-clpv] [+N] [-N]"));

                for (at = 1; string_get(word + at); at++)
                {
                        p8 letter = word[at];

                        if (letter == 'c')
                        {
                                shell_dirstack_count = 0;
                                shell_dirstack_used = 0;
                                return shell_answer(0);
                        }

                        if (letter == 'l')
                                full = true;
                        else if (letter == 'v')
                                numbered = true;
                        else if (letter == 'p')
                                lines = true;
                        else
                        {
                                //      A word dirs has no letter for is read
                                //      as a stack index, which is what it
                                //      would have been: bash refuses the
                                //      whole word as a number rather than
                                //      the letter as an option, so "-xy" is
                                //      named as it was written.
                                (void)letter;

                                if (!shell_bash_compat)
                                        return shell_answer(
                                            shell_option_refused(
                                                "dirs", word,
                                                "dirs [-clpv] [+N] [-N]"));

                                return shell_answer(
                                    shell_dirstack_number_refused(
                                        "dirs", word,
                                        "dirs [-clpv] [+N] [-N]"));
                        }
                }

                index++;
        }

        if (index >= shell_argc)
        {
                shell_dirstack_listed(write, full, numbered, lines);

                return shell_answer(0);
        }

        {
                positive wanted;
                string_address address_to list =
                    shell_dirstack_entries(address_of count);

                //      A word with no sign in front of it never was an
                //      index, and bash reads it as the option it is not.
                if (!shell_dirstack_spec(shell_argv[index]))
                {
                        if (string_is(shell_argv[index], '+') ||
                            string_is(shell_argv[index], '-'))
                                return shell_answer(
                                    shell_dirstack_number_refused(
                                        "dirs", shell_argv[index],
                                        "dirs [-clpv] [+N] [-N]"));

                        return shell_answer(shell_option_refused(
                            "dirs", shell_argv[index],
                            "dirs [-clpv] [+N] [-N]"));
                }

                if (!shell_dirstack_index(shell_argv[index], count,
                                          address_of wanted))
                {
                        shell_diagnostic_where();

                        //      With nothing pushed there is no stack to
                        //      index into, and that is what bash says; past
                        //      that it names the number, without the sign it
                        //      was given.
                        if (count < 2)
                                return shell_answer(string_report(
                                    log_error, 1,
                                    "dirs: directory stack empty\n"));

                        return shell_answer(string_report(log_error, 1,
                            "dirs: %s: directory stack index out of range\n",
                            shell_argv[index] + 1));
                }

                //      -v numbers one entry as it numbers a listing, in a
                //      field of two so the paths line up past nine.
                if (numbered)
                {
                        p8 written[32];
                        positive digits = positive_into_string(written, wanted);

                        while (digits < 2)
                        {
                                write(" ", 1);
                                digits++;
                        }

                        write(written, string_length(written));
                        write("  ", 2);
                }

                shell_dirstack_said(write, list[wanted], full);
                write("\n", 1);
        }

        shell_answer(0);
}

//      A move that pushd and popd both end with: change directory, then say
//      what the stack looks like afterwards.
static COLD bool shell_dirstack_move(string_address where)
{
        bool physical_named = true;
        bool variables_set = true;
        p8 joined[4096];

        //      A relative name is joined onto where the shell is, which is
        //      what cd does with it. Handed to shell_cd_try as written it
        //      became PWD as written, and every listing after "pushd db"
        //      then read "db" where both references write the whole path.
        if (string_not(where, '/'))
        {
                if (path_join(joined, sizeof(joined), shell_directory, where)
                        == sizeof(joined) - 1 &&
                    !path_walk_join(joined, sizeof(joined), shell_directory,
                                    string_length(shell_directory), where, ""))
                        return false;

                where = joined;
        }

        return shell_cd_try(where, false, address_of physical_named,
                            address_of variables_set, null);
}

COLD fn shell_pushd(writer write, string_address input)
{
        positive count;
        string_address address_to list = shell_dirstack_entries(
            address_of count);
        p8 wanted[SHELL_DIRECTORY_MAX];
        p8 previous[SHELL_DIRECTORY_MAX];
        string_address rotated[SHELL_DIRSTACK_MAX + 1];
        positive index;
        //      -n moves the stack and leaves the shell where it is; the
        //      operand is then written into the stack as it was given,
        //      because nothing ever went there to be named properly.
        bool stack_only = false;
        string_address named = null;

        for (positive at = 1; at < shell_argc; at++)
        {
                string_address word = shell_argv[at];

                if (!named && word_is(word, "-n"))
                {
                        stack_only = true;
                        continue;
                }

                if (!named && word_is(word, "--"))
                {
                        if (at + 1 < shell_argc)
                                named = shell_argv[++at];
                        continue;
                }

                //      A lone "-" is the previous directory, which is a
                //      name and not an index; a sign in front of anything
                //      but digits is a number pushd will not read.
                //      The count comes first: "pushd - -Z" is two words
                //      to bash before either of them is a number it could
                //      not read.
                if (named)
                {
                        shell_diagnostic_where();

                        return shell_answer(string_report(
                            log_error, 1, "pushd: too many arguments\n"));
                }

                if (!shell_dirstack_spec(word) && !word_is(word, "-") &&
                    (string_is(word, '+') || string_is(word, '-')))
                        return shell_answer(shell_dirstack_number_refused(
                            "pushd", word, "pushd [-n] [+N | -N | dir]"));

                named = word;
        }

        if (named && !shell_dirstack_spec(named))
        {
                //      A name rather than an index: with -n it goes into the
                //      stack under the top and the shell stays put.
                if (stack_only)
                {
                        string_address kept[SHELL_DIRSTACK_MAX + 1];
                        positive used = 0;

                        kept[used++] = named;

                        for (positive at = 1; at < count; at++)
                                kept[used++] = list[at];

                        if (!shell_dirstack_write(kept, used))
                                return shell_answer(string_report(log_error, 1, "pushd: directory stack full\n"));

                        shell_dirstack_listed(write, false, false, false);

                        return shell_answer(0);
                }

                string_copy_max_end(previous, shell_directory,
                                    sizeof(previous) - 1);
                string_copy_max_end(wanted, named, sizeof(wanted) - 1);

                rotated[0] = previous;

                if (count > 1)
                        memory_copy_apart(rotated + 1, list + 1,
                                          (count - 1) * sizeof(list[0]));

                if (!shell_dirstack_move(wanted))
                {
                        shell_diagnostic_where();

                        return shell_answer(string_report(log_error, 1,
                            "pushd: %s: No such file or directory\n", named));
                }

                if (!shell_dirstack_write(rotated, count))
                        return shell_answer(string_report(log_error, 1, "pushd: directory stack full\n"));

                shell_dirstack_listed(write, false, false, false);

                return shell_answer(0);
        }

        // With no operand the top two are exchanged, which needs something
        // under the top to exchange with.
        if (!named)
        {
                if (count < 2)
                        return shell_answer(string_report(log_error, 1, "pushd: no other directory\n"));

                if (stack_only)
                {
                        //      Nothing to move and nowhere to go: the stack
                        //      is left as it is and nothing is written.
                        return shell_answer(0);
                }

                memory_copy_apart(rotated, list, count * sizeof(list[0]));

                rotated[0] = list[1];
                rotated[1] = list[0];
        }
        else if (shell_dirstack_index(named, count, address_of index))
        {
                if (!index)
                {
                        //      Nothing moved, so with -n nothing is said.
                        if (!stack_only)
                                shell_dirstack_listed(write, false, false, false);

                        return shell_answer(0);
                }

                // A rotation is the suffix followed by the prefix.
                memory_copy_apart(rotated, list + index,
                                  (count - index) * sizeof(list[0]));
                memory_copy_apart(rotated + count - index, list,
                                  index * sizeof(list[0]));
        }
        else
        {
                shell_diagnostic_where();

                //      With nothing pushed there is no stack to index into,
                //      and bash says that rather than naming the number.
                if (count < 2)
                        return shell_answer(string_report(log_error, 1,
                            "pushd: directory stack empty\n"));

                return shell_answer(string_report(log_error, 1,
                    "pushd: %s: directory stack index out of range\n",
                    named));
        }

        string_copy_max_end(wanted, rotated[0], sizeof(wanted) - 1);

        // Written before the move, because the kept part is read out of the
        // pool the move does not touch and the top is already in hand.
        if (!shell_dirstack_write(rotated + 1, count - 1))
                return shell_answer(string_report(log_error, 1, "pushd: directory stack full\n"));

        //      -n rotates and stays: the shell keeps the directory it is in,
        //      which is still the top of what is listed, so the entry the
        //      rotation brought up is written under it rather than moved to.
        if (!stack_only && !shell_dirstack_move(wanted))
        {
                shell_diagnostic_where();

                return shell_answer(string_report(log_error, 1,
                    "pushd: %s: No such file or directory\n", wanted));
        }

        //      A rotation asked to leave the shell where it is says nothing:
        //      Bash writes the stack for a push and for a move, and a
        //      rotation that moved nothing is neither.
        if (!stack_only)
                shell_dirstack_listed(write, false, false, false);

        shell_answer(0);
}

COLD fn shell_popd(writer write, string_address input)
{
        positive count;
        string_address address_to list = shell_dirstack_entries(
            address_of count);
        string_address kept[SHELL_DIRSTACK_MAX + 1];
        p8 wanted[SHELL_DIRECTORY_MAX];
        positive index = 0;
        positive used = 0;
        //      -n asks for the stack to lose an entry and the shell to stay
        //      where it is, so the entry taken is the one under the top --
        //      the top is the directory the shell is in and only a move
        //      could remove it.
        bool stack_only = false;
        string_address named = null;

        for (positive at = 1; at < shell_argc; at++)
        {
                string_address word = shell_argv[at];

                if (!named && word_is(word, "-n"))
                {
                        stack_only = true;
                        continue;
                }

                if (!named && word_is(word, "--"))
                {
                        if (at + 1 < shell_argc)
                                named = shell_argv[++at];
                        continue;
                }

                if (!shell_dirstack_spec(word))
                        return shell_answer(shell_dirstack_number_refused(
                            "popd", word, "popd [-n] [+N | -N]"));

                if (named)
                        return shell_answer(string_report(
                            log_error, 1, "popd: too many arguments\n"));

                named = word;
        }

        if (count < 2)
        {
                shell_diagnostic_where();

                return shell_answer(string_report(log_error, 1,
                    "popd: directory stack empty\n"));
        }

        if (named && !shell_dirstack_index(named, count, address_of index))
                return shell_answer(string_report(log_error, 1, "popd: %s: directory stack index out of range\n",
                              named));

        //      With -n the top is never the one that goes.
        if (stack_only && !index)
                index = 1;

        for (positive at = 0; at < count; at++)
                if (at != index)
                        kept[used++] = list[at];

        // Removing the top is the only one that moves the shell; taking an
        // entry out from under it leaves it where it is.
        if (!index)
        {
                string_copy_max_end(wanted, kept[0], sizeof(wanted) - 1);

                if (!shell_dirstack_move(wanted))
                        return shell_answer(string_report(log_error, 1, "popd: %s: No such file or directory\n", wanted));
        }

        if (!shell_dirstack_write(kept + 1, used - 1))
                return shell_answer(string_report(log_error, 1, "popd: directory stack full\n"));

        shell_dirstack_listed(write, false, false, false);

        shell_answer(0);
}

COLD fn shell_clear(writer write, string_address input)
{
        write(str(TERM_CLEAR_SCREEN));
}

// echo and printf %b share the shell escape language. The implementation sits
// with printf below; these two flags also let \c stop echo's remaining words
// and final newline.
static bool printf_cut;
static bool printf_in_b;
fn printf_escaped(writer write, string_address text);

/*
        Join short builtin output into one write.

        echo hello world is four log copies and one flush; the copies are the
        builtin. A small stack buffer, or the log buffer itself when that is
        the writer and the line fits, makes one copy and leaves the flush
        where exec already puts it. Anything larger than the room is written
        in chunks of this size, still fewer traps than a byte or a word.
*/
#define SHELL_OUTPUT_JOIN 256

static inline INLINE fn shell_output_put(writer write, p8 address_to room,
                                         positive address_to used,
                                         address_any data, positive length)
{
        if (!length)
                return;

        if (*used && *used + length > SHELL_OUTPUT_JOIN)
        {
                write(room, *used);
                *used = 0;
        }

        if (length > SHELL_OUTPUT_JOIN)
        {
                write(data, length);
                return;
        }

        memory_copy_apart(room + *used, data, length);
        *used += length;
}

static inline INLINE fn shell_output_byte(writer write, p8 address_to room,
                                          positive address_to used, p8 value)
{
        if (*used == SHELL_OUTPUT_JOIN)
        {
                write(room, *used);
                *used = 0;
        }

        room[(*used)++] = value;
}

static inline INLINE bool shell_log_room(positive total)
{
        return log_writer_buffer_length <= MAX_INPUT &&
               total <= MAX_INPUT - log_writer_buffer_length;
}

static inline INLINE fn shell_echo_join(writer write, positive first,
                                        bool newline)
{
        positive words = shell_argc - first;
        p8 address_to dst;
        positive have;

        if (words <= 2)
        {
                string_address a = words ? shell_argv[first] : (string_address) "";
                positive la = words ? string_length(a) : 0;
                string_address b = words == 2 ? shell_argv[first + 1] : a;
                positive lb = words == 2 ? string_length(b) : 0;
                positive total = la + lb + (words == 2) + (newline ? 1 : 0);

                if (!total)
                        return;

                if (write == log && shell_log_room(total))
                {
                        have = log_writer_buffer_length;
                        dst = log_writer_buffer + have;
                        memory_copy_apart(dst, a, la);
                        dst += la;
                        if (words == 2)
                        {
                                *dst++ = ' ';
                                memory_copy_apart(dst, b, lb);
                                dst += lb;
                        }
                        if (newline)
                                *dst = '\n';
                        log_writer_buffer_length = have + total;
                        return;
                }

                if (total <= SHELL_OUTPUT_JOIN)
                {
                        p8 room[SHELL_OUTPUT_JOIN];

                        memory_copy_apart(room, a, la);
                        if (words == 2)
                        {
                                room[la] = ' ';
                                memory_copy_apart(room + la + 1, b, lb);
                        }
                        if (newline)
                                room[total - 1] = '\n';
                        write(room, total);
                        return;
                }

                if (la)
                        write(a, la);
                if (words == 2)
                {
                        write(" ", 1);
                        if (lb)
                                write(b, lb);
                }
                if (newline)
                        write("\n", 1);
                return;
        }

        {
                p8 room[SHELL_OUTPUT_JOIN];
                positive used = 0;
                positive at;
                bool more = false;

                for (at = first; at < shell_argc; at++)
                {
                        if (more)
                                shell_output_byte(write, room, address_of used,
                                                  ' ');
                        more = true;
                        shell_output_put(write, room, address_of used,
                                         shell_argv[at],
                                         string_length(shell_argv[at]));
                }

                if (newline)
                        shell_output_byte(write, room, address_of used, '\n');

                if (used)
                        write(room, used);
        }
}

static inline INLINE bool shell_echo_backslash(positive first)
{
        positive at;

        for (at = first; at < shell_argc; at++)
                if (string_first_of(shell_argv[at], '\\'))
                        return true;

        return false;
}

/*
        echo.

        One escape writer, with each personality's option grammar. Dash only
        takes a single exact -n; Bash takes runs of n/e/E options, unless
        POSIX mode together with xpg_echo makes every word an operand.
*/
fn shell_echo(writer write, string_address input)
{
        positive index = 1;
        bool newline = true;
        bool escapes = !shell_bash_compat || shell_shopt_on(XPG_ECHO);

        printf_cut = false;

        if (!shell_bash_compat && index < shell_argc &&
            word_is(shell_argv[index], "-n"))
        {
                newline = false;
                index++;
        }
        while (shell_bash_compat && !(shell_posix_on() &&
                                      shell_shopt_on(XPG_ECHO)) &&
               index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                letter += string_span_of_set(letter, "neE");

                // Anything else in the word makes the whole word an operand,
                // which is what every shell prints for `echo -q`.
                if (!string_get(letter))
                {
                        for (letter = shell_argv[index] + 1;
                             string_get(letter); letter++)
                        {
                                if (string_is(letter, 'n'))
                                        newline = false;
                                else
                                        escapes = string_is(letter, 'e');
                        }

                        index++;
                }
                else
                        break;
        }

        // -e, dash's default escapes, and a word that still has a backslash
        // in it keep the shared escape writer, including \c. A word with no
        // backslash is the same bytes either way, so it joins with the rest.
        if (!escapes || !shell_echo_backslash(index))
        {
                shell_echo_join(write, index, newline);
                return;
        }

        for (positive first = index; index < shell_argc; index++)
        {
                if (index != first)
                        write(" ", 1);

                printf_in_b = true;
                printf_escaped(write, shell_argv[index]);
                printf_in_b = false;

                if (printf_cut)
                {
                        newline = false;
                        break;
                }
        }

        if (newline)
                write("\n", 1);
}

/*
        exec.

        -a gives the new program a name of its own, which is the whole reason
        a process's zeroth argument and the file it came from are two separate
        things; -c starts it with nothing in the environment and -l puts a
        dash in front of the name, which is how a login shell is told it is
        one. All three are Bash's and none of them changes what is run.
*/
COLD fn shell_exec(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];

        //      rbash: replacing the shell would replace the restriction
        //      with whatever was named.
        if (shell_restricted)
        {
                shell_diagnostic_where();
                return shell_answer(
                    string_report(log_error, 1, "exec: restricted\n"));
        }

        p8 address_to found = null;
        positive found_room = 0;
        string_address address_to environment;
        static string_address empty_environment[1];
        p8 login_name[256];
        string_address named = null;
        bool clear = false;
        bool login = false;
        shell_option_walk walk = {1};
        p8 which;
        bipolar located;

        while (shell_option_letter(address_of walk, address_of which))
        {
                if (which == 'c')
                        clear = true;
                else if (which == 'l')
                        login = true;
                else if (which == 'a')
                {
                        named = shell_option_argument(address_of walk);
                        if (!named)
                        {
                                log_error("exec: -a: option requires an "
                                                 "argument\n", 0);
                                exec_special_error_note();
                                return shell_answer(2);
                        }
                }
                else
                {
                        shell_letter_refused("exec", which,
                            "exec [-cl] [-a name] [command [argument ...]] "
                            "[redirection ...]");
                        exec_special_error_note();
                        return shell_answer(2);
                }
        }

        positive index = walk.index;
        // Moved down over the options, so everything below is about the
        // command and its own arguments alone.
        if (index > 1)
        {
                memory_copy(shell_argv + 1, shell_argv + index,
                            (positive)(shell_argc - index + 1) *
                                sizeof(shell_argv[0]));
                shell_argc -= index - 1;
        }

        // With nothing to run, exec is only there for the redirections that
        // were already applied to get here.
        if (shell_argc < 2)
                return shell_answer(0);

        /*
                An empty operand is still a name. Dash treats it as a path
                that cannot be executed (EACCES, 126); bash never finds a
                command of that name (ENOENT, 127). The redirections-only
                case is the argc check above, not this one.
        */
        if (!string_get(shell_argv[1]))
        {
                bipolar refused = shell_bash_compat ? -ERROR_NO_ENTRY
                                                    : -ERROR_ACCESS;
                b32 status = shell_bash_compat ? 127 : 126;

                shell_answer(status);
                shell_exec_refused(shell_argv[1], refused);
                shell_exec_failed(status);

                return;
        }

        located = shell_find_in_path_alloc(shell_argv[1], address_of found,
                                           address_of found_room);

        if (located < 0)
        {
                if (found)
                        memory_free(found, found_room);

                shell_answer(string_report(log_error, 2, "%s: no room\n", "exec"));
                shell_stop_when_scripted(2);

                return;
        }

        if (!located)
        {
                if (found)
                        memory_free(found, found_room);

                shell_answer(127);
                shell_exec_refused(shell_argv[1], -ERROR_NO_ENTRY);
                shell_exec_failed(127);

                return;
        }

        environment = clear ? empty_environment : shell_environment();
        if (!environment)
        {
                memory_free(found, found_room);
                shell_answer(2);
                log_error(str("exec: no room for environment\n"));
                shell_stop_when_scripted(2);

                return;
        }

        if (named || login)
        {
                positive used = 0;

                if (login)
                        login_name[used++] = '-';

                string_copy_max_end(login_name + used,
                                    named ? named : shell_argv[1],
                                    sizeof(login_name) - used - 1);

                shell_argv[1] = login_name;
        }

        log_flush();

        // From argv[1] on, so the new program is named by what it was asked
        // for and not by the word "exec".
        {
                bipolar told = shell_exec_file(found, shell_argv + 1,
                                               shell_argc - 1, environment);

                memory_free(found, found_room);
                shell_answer(126);
                shell_exec_refused(shell_argv[1], told);
        }
        shell_exec_failed(126);
}







#define STORAGE_ADAPTER(name, command)                                      \
        fn shell_##name(writer output, string_address input)                \
        {                                                                   \
                (void)input;                                                \
                shell_answer(command(shell_argc, shell_argv, output,        \
                                     log_error));                    \
        }                                                                   \
                                                                            \
        static b32 storage_program_##name(void)                             \
        {                                                                   \
                return command((positive)program_argument_count(),          \
                               program_argument_list(), log, log_error);     \
        }

STORAGE_ADAPTER(mount, storage_mount_command)
STORAGE_ADAPTER(umount, storage_umount_command)
STORAGE_ADAPTER(mountpoint, storage_mountpoint)
STORAGE_ADAPTER(blkid, storage_blkid_run)
STORAGE_ADAPTER(findmnt, storage_findmnt)
STORAGE_ADAPTER(findfs, storage_findfs_run)

#undef STORAGE_ADAPTER

COLD fn shell_pwd(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        p8 out_buffer[4096];
        shell_option_walk walk = {1};
        p8 letter;
        bool physical = shell_physical_on();

        (void)input;

        while (shell_option_letter(address_of walk, address_of letter))
        {
                if (letter == 'L')
                        physical = false;
                else if (letter == 'P')
                        physical = true;
                else
                        return shell_answer(shell_letter_refused(
                            "pwd", letter, "pwd [-LP]"));
        }

        if (!physical && shell_directory_holds())
        {
                string_format(write, "%s\n", shell_directory);
                return shell_answer(0);
        }

        if (!shell_here(out_buffer, sizeof(out_buffer)))
        {
                log_error("pwd: cannot determine current directory\n", 0);

                if (shell_bash_compat)
                        return shell_answer(1);

                /* dash diagnoses a failed physical lookup but retains its
                   historical success status and empty output. */
                write("\n", 1);
                return shell_answer(0);
        }

        string_format(write, "%s\n", out_buffer);
        shell_answer(0);
}

fn shell_trap_exit();
static bool exec_control_integer(string_address word, bipolar address_to answer);

/*
        "exit", which bash says as an interactive shell leaves and dash does
        not. It is written before the operand is even looked at -- "exit bad"
        says it and then complains -- and a subshell leaving says nothing,
        because a subshell is not the shell leaving.

        The end of the input is the same leaving by another road, so the
        reader says it there too.
*/
fn shell_interactive_exit_said()
{
        if (shell_bash_compat && shell_is_interactive && !exec_child_process())
                string_format(log_error, "exit\n");
}

COLD fn shell_exit(writer write, string_address input)
{
        bipolar exit_code = shell_status_entering;
        positive first = 1;

        shell_interactive_exit_said();

        /*
                A stopped job is work the person asked for and has not seen
                the end of, so the first exit that would abandon one is
                refused and only says so; a second exit straight after it
                goes. Both references do this and each has its own sentence
                for it. The warning is forgotten as soon as anything else
                runs, which is why the flag is cleared by the reader rather
                than here.
        */
        if (shell_is_interactive && !exec_child_process() &&
            !shell_exit_was_previous && job_any_stopped())
        {
                string_format(log_error,
                              shell_bash_compat
                                  ? "There are stopped jobs.\n"
                                  : "You have stopped jobs.\n");
                return shell_answer(shell_bash_compat ? 1 : 0);
        }

        if (shell_bash_compat && first < shell_argc &&
            word_is(shell_argv[first], "--"))
                first++;

        if (first < shell_argc)
        {
                bool good = exec_control_integer(shell_argv[first],
                                                   address_of exit_code);

                // Reuse return's checked integer parser. Bash accepts signed
                // machine words; dash rejects negative statuses. Operand
                // errors go through the special-builtin policy so command
                // can suppress their fatality without suppressing valid exit.
                if (!good || (!shell_bash_compat && exit_code < 0))
                {
                        string_format(log_error,
                                      shell_bash_compat
                                          ? "%s: %s: numeric argument required\n"
                                          : "%s: Illegal number: %s\n",
                                      shell_argv[0], shell_argv[first]);
                        exec_special_error_note();
                        return shell_answer(2);
                }

                if (shell_bash_compat && shell_argc > first + 1)
                {
                        string_format(log_error,
                                      "%s: too many arguments\n", shell_argv[0]);
                        expand_fatal_status(1);
                        return;
                }
        }

        exit_code = (bipolar)((positive)exit_code & 0xff);
        shell_status = (b32)exit_code;
        shell_trap_exit();

        log_flush();

        exit(exit_code);
}

COLD fn shell_logout(writer write, string_address input)
{
        if (!shell_bash_compat || !shell_shopt_on(LOGIN_SHELL))
                return shell_answer(string_report(log_error, 1, "logout: not login shell: use `exit'\n"));

        shell_exit(write, input);
}


#define REBOOT_MAGIC 0xfee1dead
#define REBOOT_MAGIC_SECOND 672274793
#define REBOOT_RESTART 0x01234567
#define REBOOT_POWER_OFF 0x4321fedc

// The rootfs lives in RAM. Whatever is still in flight is all there is, so it
// goes out to whatever backing store there is before the machine stops.
/*
        An option nobody knows is not a reason to stop the machine.

        These read nothing at all, so `poweroff -Z` synced the disks and
        called reboot(2): a typed option was a shutdown. The reference reads
        its line first and refuses it before anything irreversible happens,
        which is the only safe order for a command whose whole effect is
        irreversible. Nothing here is understood yet, so anything written
        after the name is refused rather than guessed at -- for a machine
        stop, refusing the word nobody implemented is the safe direction.
*/
static COLD bool shell_stop_refused(string_address name)
{
        if (shell_argc < 2)
                return false;

        string_report(log_error, 1, "%s: unrecognized option '%s'\n", name,
                      shell_argv[1]);
        shell_answer(1);
        return true;
}

fn shell_stop(writer write, positive command)
{
        write(str("Syncing...\n"));
        log_flush();

        system_call(syscall(sync));

        bipolar result = system_call_4(syscall(reboot), REBOOT_MAGIC, REBOOT_MAGIC_SECOND, command, 0);

        /*      Reached only when the machine did not stop, so this is a
                failure and answers as one. It used to say so on the error
                stream and then answer 0, which is a script being told the
                machine went down when it is still running. */
        string_format(write, "Cannot stop the machine: %b\n", result);
        log_flush();
        shell_answer(1);
}

COLD fn shell_reboot(writer write, string_address input)
{
        if (shell_stop_refused((string_address) "reboot"))
                return;

        shell_stop(write, REBOOT_RESTART);
}

COLD fn shell_poweroff(writer write, string_address input)
{
        if (shell_stop_refused((string_address) "poweroff"))
                return;

        shell_stop(write, REBOOT_POWER_OFF);
}


/*
        The POSIX builtins.

        These read shell_argv rather than the joined line the older commands in
        this file are handed: printf, test and set all turn on knowing where
        one word ended and the next began, which joining throws away.
*/

// The shell's own ioctl, spelled here because shell.c names it after this file
// has already been read.
#define BUILTIN_TCGETS 0x5401u

PURE bool word_is(string_address word, string_address text)
{
        return word && !string_compare(word, text);
}

static COLD fn env_unset_noted(string_address name, positive length)
{
        env_locale_touch(name, length);
        if (length == 15 && !memory_compare(name, "POSIXLY_CORRECT", 15))
                shell_posix_changed(false);
        else if (length == 6 && !memory_compare(name, "OPTIND", 6))
        {
                shell_getopts_index_changed();
                shell_getopts_parameters_changed();
        }
}

static fn env_unset_span(string_address name, positive length)
{
        positive index = env_find_span(name, length);

        if (index < shell_var_count)
        {
                env_variable_drop(index);
                env_unset_noted(name, length);
        }
}

fn env_unset(string_address name)
{
        env_unset_span(name, string_length(env_reading(name)));
}

/* Restore stored bytes, not a new assignment: integer and nameref attributes
   must not evaluate them again. The retained array reference is consumed on
   every path, including allocation failure. */
static bool env_value_restore(string_address name, positive length,
                              string_address value, p8 attributes, b32 array)
{
        if (!value && !attributes)
        {
                env_unset_span(name, length);
                array_table_release(array);
                return true;
        }
        positive hash = env_name_hash(name, length);
        env_variable address_to variable = env_export_take_hashed(name, length, hash);
        if (!variable)
        {
                array_table_release(array);
                return false;
        }
        p8 current = variable->attributes;
        variable->attributes = 0;
        bool written = env_write_found_span(name, length, hash,
                                            (positive)(variable - shell_vars),
                                            value ? value : (string_address)"", false);
        variable->attributes = current;
        if (!written)
        {
                array_table_release(array);
                return false;
        }
        if ((current ^ attributes) & SHELL_ARRAY_READONLY)
                readonly_count = attributes & SHELL_ARRAY_READONLY
                                     ? readonly_count + 1 : readonly_count - 1;
        array_table_release(variable->array);
        variable->array = array;
        variable->attributes = attributes;
        if (!value)
                variable->text[length] = end;
        shell_envp_dirty = true;
        if (value)
                env_write_noted(name, length, true);
        else
                env_unset_noted(name, length);
        return true;
}

/* Consume the retained table while keeping the name/value alive for callers
   that restore private state before releasing the copied cell. */
static bool shell_binding_restore(shell_binding address_to saved)
{
        bool answer = env_value_restore(saved->name, saved->variable.name_length,
            env_variable_value(&saved->variable), saved->variable.attributes, saved->variable.array);
        saved->variable.array = 0;
        env_declare_restore(saved->name, saved->variable.declared);
        env_export_restore(saved->name, saved->variable.permanent);
        return answer;
}

bool env_set_number(string_address name, positive value)
{
        p8 text[24];

        positive_into_string(text, value);
        return env_assign(name, text);
}

// Forwards, and signed. string_to_bipolar reads from the end of the string,
// which answers 5 for "0.5" and 0 for anything with a space after it.
bipolar shell_signed(string_address input, bool address_to good)
{
        address_to good = false;

        if (!input)
                return 0;

        input += string_span(input, string_set_blanks);

        positive used;
        bipolar value = string_bipolar(input, address_of used);

        if (!used || string_get(input + used))
                return 0;

        address_to good = true;

        return value;
}

/*
        The positional parameters live in expand.c, which is what reads them.

        They used to be mirrored into the environment, because that was the
        only place the expander looked a name up and $1 is a name to it. The
        mirror also went to every child through execve, which no shell does,
        and cost an environment entry per parameter per call.
*/
//      What getopts is walking over, however many that is.
static string_address address_to shell_getopts_list;
static positive shell_getopts_room;

extern string_address address_to shell_parameter;
extern positive shell_parameter_count;
bool shell_parameters_set(string_address address_to words, positive count);
fn shell_parameters_shift(positive count);

/*
        The long names for the same letters.

        A script writes "set -o nounset" where a terminal writes "set -u", and
        four of the names have no letter at all. The ones that do are kept in
        the same bits the letters use, so the two spellings cannot disagree.
*/
typedef named_byte shell_option;

static shell_option shell_option_names[] = {
    {"errexit", 'e'},    {"noglob", 'f'},   {"ignoreeof", 'I'},
    {"interactive", 'i'}, {"monitor", 'm'}, {"noexec", 'n'},
    {"stdin", 's'},      {"xtrace", 'x'},   {"verbose", 'v'},
    {"vi", 0},           {"emacs", 0},      {"noclobber", 'C'},
    {"allexport", 'a'},  {"notify", 'b'},   {"nounset", 'u'},
    {"nolog", 0},        {"pipefail", 0},   {"debug", 0},
    {null, 0},
};

/*
        The option names Bash has and the reference shell does not.

        Kept out of the table above because that table is what `set -o` prints
        and what the reference shell's own listing is compared against, and
        because nothing here changes what the shell does with a script that
        never names them. `set -E` and `set -T` reach them by letter, which is
        how a script that opens with `set -eET` writes them.
*/
static shell_option shell_extra_options[] = {
    {"errtrace", 'E'},
    {"functrace", 'T'},
    {"history", 0},
    {"braceexpand", 'B'},
    {"hashall", 'h'},
    {"physical", 'P'},
    {"onecmd", 't'},
    {"keyword", 'k'},
    {"privileged", 'p'},
    {"histexpand", 'H'},
    {"interactive-comments", 0},
    {"posix", 0},
    {null, 0},
};

#define SHELL_EXTRA_OPTIONS (array_count(shell_extra_options) - 1)
#define SHELL_EXTRA_ERRTRACE 0
#define SHELL_EXTRA_FUNCTRACE 1
#define SHELL_EXTRA_BRACEEXPAND 3
#define SHELL_EXTRA_HASHALL 4
#define SHELL_EXTRA_PHYSICAL 5
#define SHELL_EXTRA_ONECMD 6
#define SHELL_EXTRA_KEYWORD 7
#define SHELL_EXTRA_PRIVILEGED 8
#define SHELL_EXTRA_HISTEXPAND 9
#define SHELL_EXTRA_INTERACTIVE_COMMENTS 10
#define SHELL_EXTRA_POSIX 11

/* Startup +/-H is parsed before interactivity is known.  Remember an explicit
   choice so the interactive Bash default does not overwrite it later. */
static bool shell_histexpand_told;
static bool shell_alias_startup_told;

static positive shell_extra_state = ((positive)1 << SHELL_EXTRA_BRACEEXPAND) |
                                    ((positive)1 << SHELL_EXTRA_HASHALL);

static PURE bool shell_extra_on(positive which)
{
        if (which == SHELL_EXTRA_INTERACTIVE_COMMENTS)
                return (shell_shopt_state &
                        SHELL_SHOPT(INTERACTIVE_COMMENTS)) != 0;
        return (shell_extra_state & ((positive)1 << which)) != 0;
}

PURE bool shell_posix_on()
{
        return shell_extra_on(SHELL_EXTRA_POSIX);
}

/* POSIX is a policy over the Bash personality, never a second interpreter.
   Variable writes call this after committing POSIXLY_CORRECT. Option changes
   also maintain that variable, but only after the environment is initialized. */
static COLD fn shell_posix_changed(bool on)
{
        if (!shell_bash_compat)
                return;
        if (on)
        {
                shell_extra_state |= (positive)1 << SHELL_EXTRA_POSIX;
                shell_shopt_state |= SHELL_SHOPT(EXPAND_ALIASES) |
                                     SHELL_SHOPT(INHERIT_ERREXIT) |
                                     SHELL_SHOPT(SHIFT_VERBOSE) |
                                     SHELL_SHOPT(INTERACTIVE_COMMENTS);
        }
        else
        {
                shell_extra_state &= ~((positive)1 << SHELL_EXTRA_POSIX);
                shell_shopt_state &= ~SHELL_SHOPT(SHIFT_VERBOSE);
                if (!shell_is_interactive)
                        shell_shopt_state &= ~SHELL_SHOPT(EXPAND_ALIASES);
        }
}

static COLD bool shell_posix_variable()
{
        const_string name = "POSIXLY_CORRECT";

        if (!shell_env_initialized)
                return true;
        if (shell_posix_on())
                return env_get(name) || env_assign(name, "y");
        /* `set +o posix` removes even a readonly POSIXLY_CORRECT, just as
           option restoration bypasses the ordinary user assignment guard. */
        env_unset((string_address)name);
        return true;
}

PURE bool shell_braceexpand_on()
{
        return shell_extra_on(SHELL_EXTRA_BRACEEXPAND);
}

static PURE bool shell_physical_on()
{
        /* Only Bash-gated setters can turn this extra bit on. Avoid a second
           identity test on every cd and pwd. */
        return shell_extra_on(SHELL_EXTRA_PHYSICAL);
}

PURE bool shell_onecmd_on()
{
        return shell_extra_on(SHELL_EXTRA_ONECMD);
}

#define shell_hashall_on() shell_extra_on(SHELL_EXTRA_HASHALL)
#define shell_keyword_on() shell_extra_on(SHELL_EXTRA_KEYWORD)

/*
        Bash privilege mode is process state, not an option-shaped promise.

        Keep the real IDs captured at entry: once set +p has reset effective
        and saved IDs, a later set -p may describe the mode but must not regain
        the entry identity. Group credentials go first so resetting the uid
        cannot take away the capability needed to reset the gid.
*/
static p32 shell_real_uid, shell_real_gid;
static bool shell_privilege_known;
static bool shell_privilege_resettable;
static bool shell_privilege_mismatched;
static bool shell_startup_privileged;

static fn shell_privilege_prepare()
{
        p32 uid[3], gid[3];

        if (shell_privilege_known)
                return;

        if (system_call_3(syscall(getresuid), (positive)uid,
                          (positive)(uid + 1), (positive)(uid + 2)) < 0 ||
            system_call_3(syscall(getresgid), (positive)gid,
                          (positive)(gid + 1), (positive)(gid + 2)) < 0)
        {
                string_format(log_error, "bash: cannot %s process privileges\n", "read");
                log_flush();
                system_call_1(syscall(exit_group), 1);
                __builtin_unreachable();
        }

        shell_real_uid = uid[0];
        shell_real_gid = gid[0];
        shell_privilege_mismatched = uid[0] != uid[1] || gid[0] != gid[1];
        shell_privilege_resettable = uid[0] != uid[1] || uid[0] != uid[2] ||
                                     gid[0] != gid[1] || gid[0] != gid[2];
        shell_privilege_known = true;
}

static fn shell_privilege_drop()
{
        shell_privilege_prepare();

        if (!shell_privilege_resettable)
                return;

        if (system_call_3(syscall(setresgid), shell_real_gid, shell_real_gid,
                          shell_real_gid) < 0 ||
            system_call_3(syscall(setresuid), shell_real_uid, shell_real_uid,
                          shell_real_uid) < 0)
        {
                string_format(log_error, "bash: cannot %s process privileges\n", "drop");
                log_flush();
                system_call_1(syscall(exit_group), 1);
                __builtin_unreachable();
        }

        shell_privilege_resettable = false;
}

static PURE bool shell_privileged_on()
{
        return shell_extra_on(SHELL_EXTRA_PRIVILEGED);
}

/* Called after invocation options have reached the shared option table. */
static fn shell_privilege_started()
{
        shell_privilege_prepare();
        shell_startup_privileged =
            shell_privilege_mismatched || shell_privileged_on();

        if (shell_privilege_mismatched && !shell_privileged_on())
                shell_privilege_drop();
}

static bool shell_extra_told(string_address word, bool on)
{
        positive index = string_table_find(word, shell_extra_options,
                                           sizeof(shell_extra_options[0]),
                                           SHELL_EXTRA_OPTIONS);

        if (index >= SHELL_EXTRA_OPTIONS)
                return false;

        if (index == SHELL_EXTRA_POSIX)
        {
                if (shell_posix_on() != on)
                        shell_posix_changed(on);
                return shell_posix_variable();
        }

        if (index == SHELL_EXTRA_INTERACTIVE_COMMENTS)
        {
                if (on)
                        shell_shopt_state |=
                            SHELL_SHOPT(INTERACTIVE_COMMENTS);
                else
                        shell_shopt_state &=
                            ~SHELL_SHOPT(INTERACTIVE_COMMENTS);
                return true;
        }

        if (index == SHELL_EXTRA_PRIVILEGED && !on)
                shell_privilege_drop();

        if (index == SHELL_EXTRA_HISTEXPAND)
                shell_histexpand_told = true;

        if (on)
                shell_extra_state |= (positive)1 << index;
        else
                shell_extra_state &= ~((positive)1 << index);

        return true;
}

PURE bool shell_histexpand_on()
{
        return shell_bash_compat && shell_is_interactive &&
               shell_extra_on(SHELL_EXTRA_HISTEXPAND);
}

static bool shell_extra_letter(p8 letter, bool on)
{
        for (positive at = 0; at < SHELL_EXTRA_OPTIONS; at++)
                if (shell_extra_options[at].value == letter)
                        return shell_extra_told(shell_extra_options[at].name,
                                                on);

        return false;
}

#define SHELL_OPTION_NAMES \
        (array_count(shell_option_names) - 1)
#define SHELL_OPTION_MONITOR 4
#define SHELL_OPTION_NOCLOBBER 11
#define SHELL_OPTION_PIPEFAIL 16

static positive shell_options_named;

PURE bool shell_option_on(positive index)
{
        if (shell_option_names[index].value >= 'a' &&
            shell_option_names[index].value <= 'z')
                return (shell_options & SHELL_FLAG(shell_option_names[index].value)) != 0;

        return (shell_options_named & ((positive)1 << index)) != 0;
}

fn job_monitor_told(bool on);

fn shell_option_told(positive index, bool on)
{
        if (shell_option_names[index].value >= 'a' &&
            shell_option_names[index].value <= 'z')
        {
                if (on)
                        shell_options |= SHELL_FLAG(shell_option_names[index].value);
                else
                        shell_options &= ~SHELL_FLAG(shell_option_names[index].value);

                // The bit is what the executor reads, so it is set before the
                // process groups and the terminal are arranged around it.
                if (index == SHELL_OPTION_MONITOR)
                        job_monitor_told(on);

                return;
        }

        if (on)
                shell_options_named |= (positive)1 << index;
        else
                shell_options_named &= ~((positive)1 << index);
}

/* Startup and set use the same option table and side effects. */
static bool shell_option_letter_told(p8 letter, bool on)
{
        for (positive option = 0; option < SHELL_OPTION_NAMES; option++)
                if (shell_option_names[option].value == letter)
                {
                        shell_option_told(option, on);
                        return true;
                }

        return shell_bash_compat && shell_extra_letter(letter, on);
}

/*
        The option letters as they are now, not as the process began.

        `$-` used to point straight at the startup spelling ("s", "c", or
        empty), so `set -euxC` changed the behaviour and continued to report
        the old flags. Dash emits these in its fixed option-table order rather
        than the order in which set saw them; doing the same makes the value
        stable enough for scripts to save and restore.
*/
RETURNS_NONNULL string_address shell_flags_current()
{
        static p8 flags[32];
        static positive last_options, last_named, last_extra;
        static bool last_bash, last_restricted, known;
        static p8 last_source;
        p8 source = string_get(shell_option_flags);

        /* Observe the state instead of maintaining invalidation hooks: local
           option unwind and command-substitution policy restore whole words
           directly. Repeated $- expansion needs no table walk when none of
           these words changed. */
        if (known && last_options == shell_options &&
            last_named == shell_options_named && last_extra == shell_extra_state &&
            last_bash == shell_bash_compat && last_source == source &&
            last_restricted == shell_restricted)
                return flags;

        last_options = shell_options;
        last_named = shell_options_named;
        last_extra = shell_extra_state;
        last_bash = shell_bash_compat;
        last_source = source;
        last_restricted = shell_restricted;
        known = true;

        string_address order = shell_bash_compat
                                   ? (string_address) "abefhiklmnprtuvxBCEHPT"
                                   : (string_address) "ubaCvxsiImfne";
        positive into = 0;

        for (positive at = 0; string_get(order + at); at++)
        {
                p8 letter = order[at];
                positive index;

                //      The one letter with no table entry, in the place
                //      Bash's own table puts it: between privileged and
                //      onecmd, so `bash -r` reads back as hrBc.
                if (letter == 'r')
                {
                        if (shell_restricted)
                                flags[into++] = letter;
                        continue;
                }

                for (index = 0; index < SHELL_OPTION_NAMES; index++)
                        if (shell_option_names[index].value == letter)
                                break;

                if (index < SHELL_OPTION_NAMES && shell_option_on(index))
                        flags[into++] = letter;
                else if (shell_bash_compat)
                        for (positive extra = 0; extra < SHELL_EXTRA_OPTIONS;
                             extra++)
                                if (shell_extra_options[extra].value == letter &&
                                    shell_extra_on(extra))
                                        flags[into++] = letter;
        }

        if (shell_bash_compat)
        {
                if (string_first_of(shell_option_flags, 'c'))
                        flags[into++] = 'c';
                if (shell_options & SHELL_FLAG('s'))
                        flags[into++] = 's';
        }

        flags[into] = end;
        return flags;
}

// Entry mode supplies the initial s/i state. From this point on they are
// ordinary set options: `set +s` and `set +i` must also disappear from `$-`.
fn shell_options_started(bool interactive, b32 monitor)
{
        if (string_first_of(shell_option_flags, 's'))
                shell_options |= SHELL_FLAG('s');

        if (interactive)
                shell_options |= SHELL_FLAG('i');

        if (shell_bash_compat && interactive && !shell_histexpand_told)
                shell_extra_state |= (positive)1 << SHELL_EXTRA_HISTEXPAND;
        if (shell_bash_compat && interactive && !shell_alias_startup_told)
                shell_shopt_state |= SHELL_SHOPT(EXPAND_ALIASES);

        // Defer the one monitor side effect until interactive identity is
        // known. Parsing -m earlier would mark job control initialized before
        // it could acquire the terminal; an explicit +m overrides the default.
        if (interactive || monitor >= 0)
                shell_option_told(SHELL_OPTION_MONITOR,
                                  monitor < 0 ? interactive : monitor != 0);
}

/*
        Whether a failure anywhere in a pipeline is the pipeline's answer.

        set -o pipefail was in the table of names from the start and was only
        ever a name: the shell said it was on when asked, and then reported
        the last stage's status the way it always had. A script opening with
        set -euo pipefail got the promise and none of the behaviour, which is
        worse than not having it -- errexit then misses exactly the earlier
        pipeline failures that pipefail was supposed to expose.

        This is the seventeenth table entry above. Reading its named-option
        bit directly keeps every pipeline out of the general name lookup.
*/
#define shell_named_option(which)                                            \
        ((shell_options_named & ((positive)1 << (which))) != 0)
#define shell_pipefail() shell_named_option(SHELL_OPTION_PIPEFAIL)
#define shell_noclobber() shell_named_option(SHELL_OPTION_NOCLOBBER)

static COLD fn shell_option_row(writer write, string_address name, bool on,
                                 string_address command, positive width, p8 separator)
{
        if (command)
        {
                write(command, string_length(command));
                string_format(write, "%s\n", name);
        }
        else
        {
                string_to_field(write, name, width, ' ', true);
                write(address_of separator, 1);
                write(on ? "on\n" : "off\n", on ? 3 : 4);
        }
}

/*
        The set options, as bash writes them.

        want is nought for all of them, one for the ones that are on and
        minus one for the ones that are off, which is what shopt -s -o and
        shopt -u -o ask for: the same list in the same order, with the rest
        left out, rather than a second walk over two tables in the order
        they happen to be stored in.
*/
/* The set -o names bash publishes, in the order SHELLOPTS lists them. */
static const string_address shell_setopt_names[] = {
    "allexport", "braceexpand", "emacs", "errexit",
    "errtrace", "functrace", "hashall", "histexpand", "history",
    "ignoreeof", "interactive-comments", "keyword",
    "monitor", "noclobber", "noexec", "noglob", "nolog",
    "notify", "nounset", "onecmd", "physical", "pipefail",
    "posix", "privileged", "verbose", "vi", "xtrace"
};

static PURE bool shell_setopt_named_on(string_address name)
{
        positive option = string_table_find(name, shell_option_names,
                                            sizeof(shell_option_names[0]),
                                            SHELL_OPTION_NAMES);

        if (option < SHELL_OPTION_NAMES)
                return shell_option_on(option);

        option = string_table_find(name, shell_extra_options,
                                   sizeof(shell_extra_options[0]),
                                   SHELL_EXTRA_OPTIONS);
        return option < SHELL_EXTRA_OPTIONS && shell_extra_on(option);
}

fn shell_options_listed_wanted(writer write, bool as_commands, bipolar want)
{
        positive index = 0;

        if (shell_bash_compat)
        {
                /* A policy view over the existing state, not a second option
                   registry. Unsupported options are not advertised as if a
                   stored bit implemented their behavior. */
                for (positive at = 0; at < array_count(shell_setopt_names); at++)
                {
                        bool on = shell_setopt_named_on(shell_setopt_names[at]);

                        if ((want > 0 && !on) || (want < 0 && on))
                                continue;

                        shell_option_row(write, shell_setopt_names[at], on,
                                         as_commands ? (on ? "set -o " : "set +o ") : null,
                                         15, '\t');
                }
                return;
        }

        if (!as_commands)
                string_format(write, "Current option settings\n");

        while (shell_option_names[index].name)
        {
                bool on = shell_option_on(index);

                shell_option_row(write, shell_option_names[index].name, on,
                                 as_commands ? (on ? "set -o " : "set +o ") : null,
                                 15, ' ');

                index++;
        }
}

fn shell_options_listed(writer write, bool as_commands)
{
        shell_options_listed_wanted(write, as_commands, 0);
}


bool shell_option_named(string_address word, bool on)
{
        positive index = string_table_find(word, shell_option_names,
                                           sizeof(shell_option_names[0]),
                                           SHELL_OPTION_NAMES);

        if (index >= SHELL_OPTION_NAMES)
                return shell_bash_compat && shell_extra_told(word, on);

        shell_option_told(index, on);

        return true;
}

/*
        shopt: the second option namespace, over the table in shell.c.

        Bash pads a name to twenty columns and then writes a tab, which is
        what a script that reads the listing with `read name state` is cutting
        on; -p writes the same states back as the commands that would restore
        them. -o is the same five switches over set's names instead, so that
        `shopt -so pipefail` and `set -o pipefail` are one option and not two.
*/
static COLD PURE positive shell_shopt_find(string_address name)
{
        return string_table_find(name, shell_shopt_names,
                                 sizeof(shell_shopt_names[0]),
                                 SHELL_SHOPT_NAMES);
}

static PURE bool shell_shopt_index_on(positive which)
{
        return (shell_shopt_state & ((positive)1 << which)) != 0;
}

/*
        SHELLOPTS and BASHOPTS.

        Bash publishes a colon-separated listing of the set and shopt names
        that are on, in alphabetical order. They are not stored: a read
        builds the list from the same bits set -o and shopt already keep,
        an assignment is readonly, and a value inherited at startup turns
        those names on rather than becoming a variable. restricted_shell
        and login_shell name how the shell was started, so they are neither
        imported nor listed.
*/
static p8 shell_optlist_text[1536];

static PURE bool env_optlist_name(const_string name, positive length)
{
        if (!shell_bash_compat || !name)
                return false;
        if (length == 9 && !memory_compare((address_any)name, "SHELLOPTS", 9))
                return true;
        if (length == 8 && !memory_compare((address_any)name, "BASHOPTS", 8))
                return true;
        return false;
}

static COLD fn shell_optlist_append(positive address_to used, string_address name)
{
        positive length = string_length(name);
        positive at = *used;

        if (at && at < array_count(shell_optlist_text) - 1)
                shell_optlist_text[at++] = ':';
        if (at + length >= array_count(shell_optlist_text))
                return;
        memory_copy(shell_optlist_text + at, name, length);
        *used = at + length;
}

static COLD string_address shell_optlist_value(bool shopts,
                                               positive address_to value_length)
{
        positive used = 0;

        if (shopts)
        {
                for (positive at = 0; at < SHELL_SHOPT_NAMES; at++)
                {
                        if (at == SHELL_SHOPT_RESTRICTED_SHELL ||
                            at == SHELL_SHOPT_LOGIN_SHELL)
                                continue;
                        if (shell_shopt_index_on(at))
                                shell_optlist_append(address_of used,
                                                     shell_shopt_names[at]);
                }
        }
        else
        {
                for (positive at = 0; at < array_count(shell_setopt_names); at++)
                        if (shell_setopt_named_on(shell_setopt_names[at]))
                                shell_optlist_append(address_of used,
                                                     shell_setopt_names[at]);
        }

        shell_optlist_text[used] = end;
        if (value_length)
                address_to value_length = used;
        return shell_optlist_text;
}

static COLD fn shell_optlist_apply(string_address list, bool shopts)
{
        p8 name[32];

        while (list && string_get(list))
        {
                string_address stop = string_first_of_or_end(list, ':');
                positive length = (positive)(stop - list);

                if (length && length < array_count(name))
                {
                        memory_copy_end(name, list, length);
                        if (shopts)
                        {
                                positive which = shell_shopt_find(name);

                                if (which < SHELL_SHOPT_NAMES &&
                                    which != SHELL_SHOPT_RESTRICTED_SHELL &&
                                    which != SHELL_SHOPT_LOGIN_SHELL)
                                        shell_shopt_state |= (positive)1 << which;
                        }
                        else
                        {
                                bool known = false;

                                for (positive at = 0; at < array_count(shell_setopt_names);
                                     at++)
                                        if (word_is(name, shell_setopt_names[at]))
                                                known = true;
                                if (known)
                                        shell_option_named(name, true);
                                else
                                {
                                        shell_diagnostic_where();
                                        string_format(log_error,
                                                      "%s: invalid option name\n",
                                                      name);
                                }
                        }
                }
                else if (length && !shopts)
                {
                        shell_diagnostic_where();
                        log_error(list, length);
                        log_error(": invalid option name\n", 0);
                }

                if (!string_is(stop, ':'))
                        break;
                list = stop + 1;
        }
}

static COLD bool env_optlist_take(string_address entry)
{
        if (!shell_bash_compat || !entry)
                return false;
        if (!string_compare_max(entry, "SHELLOPTS=", 10))
        {
                shell_optlist_apply(entry + 10, false);
                return true;
        }
        if (!string_compare_max(entry, "BASHOPTS=", 9))
        {
                shell_optlist_apply(entry + 9, true);
                return true;
        }
        return false;
}

static COLD fn shell_shopt_said(writer write, positive which, bool as_commands)
{
        bool on = shell_shopt_index_on(which);
        shell_option_row(write, shell_shopt_names[which], on,
                         as_commands ? (on ? "shopt -s " : "shopt -u ") : null,
                         20, '\t');
}

static COLD fn shell_shopt_option_said(writer write, string_address name,
                                       bool on, bool as_commands, positive column)
{
        /* Bash writes shopt -o in two columns, not one: the listing of every
           option takes the fifteen set -o writes, and a named option takes
           the twenty a shopt name gets. A script cutting the listing on the
           tab sees whichever of the two it asked for. */
        shell_option_row(write, name, on,
                         as_commands ? (on ? "set -o " : "set +o ") : null,
                         column, '\t');
}

COLD fn shell_shopt(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        shell_option_walk walk = {1};
        p8 which;
        bool set = false;
        bool unset = false;
        bool quiet = false;
        bool as_commands = false;
        bool set_options = false;
        bool all_on = true;
        bool bad = false;

        while (shell_option_letter(address_of walk, address_of which))
        {
                if (which == 's')
                        set = true;
                else if (which == 'u')
                        unset = true;
                else if (which == 'q')
                        quiet = true;
                else if (which == 'p')
                        as_commands = true;
                else if (which == 'o')
                        set_options = true;
                else
                        return shell_answer(shell_letter_refused(
                            "shopt", which, "shopt [-pqsu] [-o] [optname ...]"));
        }

        positive index = walk.index;
        // Both directions at once has no answer, so it is refused rather
        // than resolved into whichever was written last.
        if (set && unset)
                return shell_answer(string_report(log_error, 1,
                    "shopt: cannot set and unset shell options simultaneously\n"));

        if (index >= shell_argc)
        {
                positive count = set_options ? SHELL_OPTION_NAMES
                                             : SHELL_SHOPT_NAMES;

                // -q with nothing to ask about is a question with no
                // subject, and Bash answers yes to it.
                if (quiet)
                        return shell_answer(0);

                // shopt -o with nothing named is the view set -o writes, in
                // the same order and the same field; only -s or -u filtering
                // needs the table walk below.
                //      Bash writes the same list whichever of the three
                //      was asked for, in the same order, with the ones in
                //      the other state left out.
                if (set_options && shell_bash_compat)
                {
                        shell_options_listed_wanted(write, as_commands,
                                                    set ? 1 : unset ? -1 : 0);

                        return shell_answer(0);
                }

                if (set_options && !set && !unset)
                {
                        shell_options_listed(write, as_commands);

                        return shell_answer(0);
                }

                for (positive at = 0; at < count; at++)
                {
                        bool on = set_options ? shell_option_on(at)
                                              : shell_shopt_index_on(at);

                        // A bare -s or -u asks for the names in that state
                        // and not for a change to every one of them.
                        if ((set && !on) || (unset && on))
                                continue;

                        if (set_options)
                                shell_shopt_option_said(
                                    write, shell_option_names[at].name, on,
                                    as_commands, 15);
                        else
                                shell_shopt_said(write, at, as_commands);
                }

                /* Bash's letter-only options live in the same set namespace
                   even though dash must not see them. Keep shopt -o a view of
                   that existing state rather than a second registry. */
                if (set_options && shell_bash_compat)
                        for (positive at = 0; at < SHELL_EXTRA_OPTIONS; at++)
                        {
                                bool on = shell_extra_on(at);

                                if ((set && !on) || (unset && on))
                                        continue;

                                shell_shopt_option_said(
                                    write, shell_extra_options[at].name, on,
                                    as_commands, 15);
                        }

                return shell_answer(quiet && !all_on ? 1 : 0);
        }

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                positive which;
                positive extra = SHELL_EXTRA_OPTIONS;

                if (set_options)
                {
                        which = string_table_find(
                            name, shell_option_names,
                            sizeof(shell_option_names[0]), SHELL_OPTION_NAMES);

                        if (which >= SHELL_OPTION_NAMES && shell_bash_compat)
                                extra = string_table_find(
                                    name, shell_extra_options,
                                    sizeof(shell_extra_options[0]),
                                    SHELL_EXTRA_OPTIONS);
                }
                else
                        which = shell_shopt_find(name);

                if (which >= (set_options ? SHELL_OPTION_NAMES
                                          : SHELL_SHOPT_NAMES) &&
                    extra >= SHELL_EXTRA_OPTIONS)
                {
                        //      -o is a view over the set names, and Bash
                        //      names what it will not find there and then
                        //      answers zero for it when it was told to set
                        //      or clear one. Only the query form carries
                        //      the failure out.
                        shell_diagnostic_where();
                        string_format(log_error,
                                      set_options
                                          ? "shopt: %s: invalid option name\n"
                                          : "shopt: %s: invalid shell option name\n",
                                      name);

                        if (!(set_options && (set || unset)))
                                bad = true;

                        continue;
                }

                if (set || unset)
                {
                        if (set_options)
                        {
                                if (which < SHELL_OPTION_NAMES)
                                        shell_option_told(which, set);
                                else
                                        shell_extra_told(name, set);
                        }
                        else if (which != SHELL_SHOPT_RESTRICTED_SHELL)
                        {
                                /* restricted_shell reports how the shell
                                   started; shopt cannot enter or leave it. */
                                if (set)
                                        shell_shopt_state |= (positive)1 << which;
                                else
                                        shell_shopt_state &= ~((positive)1 << which);
                        }

                        continue;
                }

                all_on = all_on &&
                         (set_options ? (which < SHELL_OPTION_NAMES
                                            ? shell_option_on(which)
                                            : shell_extra_on(extra))
                                      : shell_shopt_index_on(which));

                if (quiet)
                        continue;

                if (set_options)
                        shell_shopt_option_said(
                            write, name,
                            which < SHELL_OPTION_NAMES ? shell_option_on(which)
                                                       : shell_extra_on(extra),
                            as_commands, 20);
                else
                        shell_shopt_said(write, which, as_commands);
        }

        if (bad)
                return shell_answer(1);

        shell_answer(set || unset ? 0 : (all_on ? 0 : 1));
}

static COLD fn shell_declare_elements(writer write, string_address name,
                                      positive length, bool keyed);
static COLD fn shell_listing_value(writer write, string_address value);

// A bare set is the variables as lines the shell could be fed: sorted, and
// scalar values quoted. Arrays use declare's existing reconstructible element
// serializer; treating their scalar slot as the whole value silently dropped
// every nonzero or keyed element.
static fn shell_set_written(writer write, string_address name,
                            positive length, b32 mark)
{
        shell_pipe_status_wanted(name, length);
        positive found = env_find_span(name, length);
        p8 attributes;

        (void)mark;

        if (found >= shell_var_count)
                return;

        attributes = shell_vars[found].attributes;

        if (attributes & SHELL_ARRAY_EITHER)
        {
                if (!(attributes & SHELL_ARRAY_ASSIGNED))
                        return;

                write(name, length);
                shell_declare_elements(
                    write, name, length,
                    (attributes & SHELL_ARRAY_ASSOCIATIVE) != 0);
                write("\n", 1);
                return;
        }

        if (!env_variable_has_value(shell_vars + found))
                return;

        write(name, length);
        write("=", 1);
        if (shell_bash_compat)
                shell_listing_value(write, shell_vars[found].text + length + 1);
        else
                shell_quoted(write, shell_vars[found].text + length + 1);
        write("\n", 1);
}

// An explicit set replaces a source caller's arguments; shift only borrows
// them. Function calls save this bit alongside their positional parameters.
static bool shell_parameters_replaced;

/*
        What the reference shells write in front of a diagnostic.

        Both name the shell and the line the failing command was written
        on, and both take the name from $0 rather than from the path the
        binary happens to have, so a shell run as ./bash says ./bash and one
        run as a script says the script. Bash spells the line "line 1" and
        leaves it out of an interactive session, where the line is the one
        the person can still see; dash spells it "1" and always writes it.

        Builtins and the executor write through this so a diagnostic carries
        the script and the line the way both references do. What a session
        still disagrees about is the sentence after the prefix, not the
        prefix itself.
*/
static PURE string_address shell_where_self()
{
        if (shell_bash_compat && shell_syntax_file &&
            string_get(shell_syntax_file))
                return shell_syntax_file;

        if (shell_script_name && string_get(shell_script_name))
                return shell_script_name;

        return (string_address) "sh";
}

static COLD fn shell_diagnostic_where_to(writer write)
{
        string_address self = shell_where_self();
        positive line = shell_line_now();

        if (shell_bash_compat && shell_is_interactive)
        {
                string_format(write, "%s: ", self);
                return;
        }

        /* Dash names eval and a sourced file on expansion errors too.
           Bash keeps the ordinary prefix: only syntax inserts `eval:`. */
        if (!shell_bash_compat && shell_syntax_command)
                string_format(write, "%s: %p: %s: ", self,
                              shell_line_number ? shell_line_number : line,
                              shell_syntax_command);
        else
                string_format(write,
                              shell_bash_compat ? "%s: line %p: " : "%s: %p: ",
                              self, line);
}

static COLD fn shell_diagnostic_where()
{
        shell_diagnostic_where_to(log_error);
}

/*
        Parser errors name the source the reader is in. Bash's unnamed
        -c string is that source spelled `-c`, between $0 and the line;
        a name operand is $0 itself and needs no extra word. Nested eval
        inserts `eval:` the same way, and a sourced file replaces $0.
        dash puts the extra word after the line. Expansion errors stay
        with the ordinary prefix: they never insert `-c`.
*/
static COLD fn shell_syntax_where()
{
        string_address self = shell_where_self();
        string_address extra = shell_syntax_command;
        positive line;

        if (shell_bash_compat && shell_is_interactive)
        {
                string_format(log_error, "%s: ", self);
                return;
        }

        /* A sourced file counts from one of its own lines. dash eval does
           too. Bash eval uses $LINENO's offset from the eval command. */
        if ((!shell_bash_compat && extra) || (shell_syntax_file && !extra))
                line = shell_line_number ? shell_line_number : 1;
        else
                line = shell_line_now();

        if (shell_bash_compat)
        {
                if (extra)
                        string_format(log_error, "%s: %s: line %p: ", self,
                                      extra, line);
                else if (string_is(shell_option_flags, 'c') &&
                         shell_run_depth == 1 && !shell_syntax_file)
                        string_format(log_error, "%s: -c: line %p: ", self,
                                      line);
                else
                        string_format(log_error, "%s: line %p: ", self, line);
                return;
        }

        if (extra)
                string_format(log_error, "%s: %p: %s: ", self, line, extra);
        else
                string_format(log_error, "%s: %p: ", self, line);
}

/*
        set's usage line, which Bash writes under every letter complaint and
        dash does not write at all.

        The letters are the ones the loop below accepts under the bash
        personality, so the line cannot advertise a letter set refuses.
*/
#define SHELL_SET_LETTERS "abefhkmnptuvxBCEHPT"

static COLD b32 shell_set_refused_letter(p8 sign, p8 letter)
{
        p8 said[3] = {sign, letter, end};

        shell_diagnostic_where();
        if (!shell_bash_compat)
        {
                said[0] = '-';
                return string_report(log_error, 2, "set: Illegal option %s\n",
                                     said);
        }
        string_format(log_error, "set: %s: invalid option\n", said);
        return string_report(log_error, 2,
            "set: usage: set [-" SHELL_SET_LETTERS
            "] [-o option-name] [--] [-] [arg ...]\n");
}

static COLD b32 shell_set_refused_name(bool on, string_address name)
{
        shell_diagnostic_where();
        if (!shell_bash_compat)
                return string_report(log_error, 2,
                    "set: Illegal option -o %s\n", name);
        return string_report(log_error, 2, "set: %s: invalid option name\n",
                             name);
}

/*
        Whether a letter names an option, without turning it on.

        Bash reads the whole option list before it changes anything, so a
        letter nobody has leaves every letter in front of it unchanged:
        `set -e -Z` is not errexit and an error, it is only an error. That
        needs the question asked apart from the answer, which is what this
        is. The two personalities do not have the same letters -- bash has
        no -i, -I or -s for set and does have -r -- so the question is asked
        of whichever shell this is being run as.
*/
static PURE bool shell_option_letter_known(p8 letter)
{
        if (shell_bash_compat)
        {
                for (positive at = 0; SHELL_SET_LETTERS[at]; at++)
                        if (SHELL_SET_LETTERS[at] == letter)
                                return true;
                return letter == 'r';
        }

        for (positive option = 0; option < SHELL_OPTION_NAMES; option++)
                if (shell_option_names[option].value == letter)
                        return true;
        return false;
}

COLD fn shell_set(writer write, string_address input)
{
        positive index = 1;
        bool operands = false;

        if (shell_argc < 2)
        {
                if (!shell_inventory_sorted(write, 0, shell_set_written, false, false))
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "set"));
                if (shell_bash_compat && !shell_posix_on() &&
                    !shell_inventory_sorted(write, 0, null, true, true))
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "set"));

                return shell_answer(0);
        }

        /*
                Every letter is read before any of them is acted on.

                A letter nobody has undoes the whole command rather than
                the rest of it: `set -e -Z` leaves errexit exactly as it
                was, which is what makes the shell that ran it carry on
                instead of leaving on the errexit it never turned on. An
                -o name is not asked about here, because the reference does
                not ask either -- `set -f -o bogus` keeps the -f.
        */
        for (positive look = 1; look < shell_argc; look++)
        {
                string_address word = shell_argv[look];

                if (word_is(word, "--") || word_is(word, "-") ||
                    !(string_is(word, '-') || string_is(word, '+')) ||
                    !string_not(word + 1, end))
                        break;

                for (string_address letter = word + 1; string_get(letter);
                     letter++)
                {
                        if (string_get(letter) == 'o')
                        {
                                if (look + 1 < shell_argc)
                                        look++;
                                continue;
                        }
                        if (!shell_option_letter_known(string_get(letter)))
                        {
                                shell_answer(shell_set_refused_letter(
                                    string_is(word, '-') ? '-' : '+',
                                    string_get(letter)));
                                exec_special_error_note();
                                return;
                        }
                }
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

                // A lone - also ends the options, and POSIX has it turn off
                // -x and -v on the way; neither reference keeps it as $1.
                if (word_is(word, "-"))
                {
                        shell_option_letter_told('x', false);
                        shell_option_letter_told('v', false);
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

                                if (value == 'r' && shell_bash_compat)
                                {
                                        //      Restricted goes on and never
                                        //      comes off again: +r is a
                                        //      no-op while it is off and an
                                        //      error once it is on, which is
                                        //      the only way a shell that has
                                        //      been restricted stays that
                                        //      way.
                                        if (!on && shell_restricted)
                                        {
                                                //      The words are the
                                                //      usage refusal's; the
                                                //      status is not. This
                                                //      one is the restriction
                                                //      speaking rather than a
                                                //      mistyped option, and
                                                //      bash answers 1 for it.
                                                shell_set_refused_letter('+',
                                                                         'r');
                                                shell_answer(
                                                    shell_bash_compat ? 1 : 2);
                                                exec_special_error_note();
                                                return;
                                        }
                                        if (on)
                                                shell_restricted = true;
                                        letter++;
                                        continue;
                                }

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

                                        // A name nobody has ends the script
                                        // the same as a letter nobody has:
                                        // set is a special builtin, and the
                                        // reference shell leaves 2 behind.
                                        if (!shell_option_named(shell_argv[++index], on))
                                        {
                                                shell_answer(
                                                    shell_set_refused_name(
                                                        on, shell_argv[index]));
                                                exec_special_error_note();
                                                return;
                                        }

                                        letter++;
                                        continue;
                                }

                                if (!shell_option_letter_told(value, on))
                                {
                                        //      The walk above already refused
                                        //      every letter nobody has, so
                                        //      reaching this is a letter the
                                        //      two disagree about rather than
                                        //      one the caller wrote.
                                        shell_answer(shell_set_refused_letter(
                                            on ? '-' : '+', value));
                                        exec_special_error_note();
                                        return;
                                }

                                letter++;
                        }

                        index++;
                        continue;
                }

                operands = true;
                break;
        }

        //      argv is already a contiguous table of the right shape and
        //      shell_parameters_set copies what it is given, so the operands
        //      go straight in rather than through a middleman with a size.
        if (operands &&
            !shell_parameters_set(shell_argv + index, shell_argc - index))
                return shell_answer(string_report(log_error, 2, "set: no room for arguments\n"));

        if (operands)
        {
                shell_parameters_replaced = true;
                shell_getopts_parameters_changed();
        }
        shell_answer(0);
}

fn shell_shift(writer write, string_address input)
{
        positive amount = 1;
        positive first = 1;

        if (shell_bash_compat && shell_argc > first &&
            word_is(shell_argv[first], "--"))
                first++;

        if (shell_bash_compat && shell_argc > first + 1)
        {
                shell_diagnostic_where();
                log_error("shift: too many arguments\n", 0);
                expand_fatal_status(1);
                return;
        }

        if (shell_argc > first)
        {
                bool good;
                bipolar asked = shell_signed(shell_argv[first], address_of good);

                if (!good || asked < 0)
                {
                        if (shell_bash_compat)
                        {
                                if (!good)
                                        exec_special_error_note();
                                shell_diagnostic_where();
                                return shell_answer(string_report(log_error, good ? 1 : 2, "shift: %s: %s\n",
                                              shell_argv[first],
                                              good ? "shift count out of range"
                                                   : "numeric argument required"));
                        }
                        shell_diagnostic_where();
                        string_format(log_error, "shift: Illegal number: %s\n",
                                      shell_argv[first]);
                        exec_special_error_note();
                        return shell_answer(2);
                }

                amount = (positive)asked;
        }

        if (amount > shell_parameter_count)
        {
                if (shell_bash_compat)
                {
                        if (shell_shopt_on(SHIFT_VERBOSE))
                        {
                                shell_diagnostic_where();
                                string_format(log_error,
                                              "shift: %s: shift count out of range\n",
                                              shell_argc > first
                                                  ? shell_argv[first]
                                                  : (string_address)"1");
                        }
                        return shell_answer(1);
                }
                shell_diagnostic_where();
                log_error("shift: can't shift that many\n", 0);
                exec_special_error_note();
                return shell_answer(2);
        }

        shell_parameters_shift(amount);
        shell_getopts_parameters_changed();

        shell_answer(0);
}

static bool shell_unset_variable(const_string name, positive length)
{
        if (env_restricted_name(name, length) || env_optlist_name(name, length))
        {
                shell_unset_readonly_refused((string_address)name, length);
                exec_special_error_note();
                shell_answer(shell_bash_compat ? 1 : 2);
                return false;
        }

        b32 detached = exec_unset_prefix(name, length);
        if (detached < 0)
                shell_answer(string_report(log_error, 2, "%s: no room\n", "unset"));
        else if (!detached)
                env_unset_span((string_address)name, length);
        return detached >= 0;
}

COLD fn shell_unset(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        shell_option_walk walk = {1};
        positive index;
        bool functions = false;
        bool variables = false;
        bool reference = false;
        p8 letter;

        while (shell_option_letter(address_of walk, address_of letter))
        {
                if (letter == 'f')
                        functions = true;
                else if (letter == 'v')
                        variables = true;
                else if (letter == 'n')
                        reference = true;
                else
                {
                        // A special builtin, so a letter it does not have
                        // ends the script, as the reference shell's does.
                        shell_letter_refused("unset", letter,
                            "unset [-f] [-v] [-n] [name ...]");
                        exec_special_error_note();
                        shell_answer(2);
                        return;
                }
        }

        //      Bash refuses to be told both at once; the reference shell lets
        //      the later letter win, so only the Bash personality complains.
        if (functions && variables && shell_bash_compat)
                return shell_answer(string_report(log_error, 1,
                    "unset: cannot simultaneously unset a function and a variable\n"));

        index = walk.index;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive word_length = string_length(word);
                string_address bracket =
                    functions ? null : string_first_of(word, '[');

                /* -n removes a nameref record rather than what it names.
                   Applied to an element or an ordinary variable it is a
                   successful no-op, as in Bash. */
                if (reference && !functions)
                {
                        p8 attributes;

                        if (bracket)
                        {
                                index++;
                                continue;
                        }

                        if (!shell_valid_name(word, word_length))
                        {
                                //      Bash passes over what it cannot use,
                                //      unless the script said -v and meant
                                //      a variable by it, which is a name it
                                //      will not have.
                                if (shell_bash_compat && !variables)
                                {
                                        index++;
                                        continue;
                                }

                                shell_name_refused("unset", word,
                                                   word_length);

                                if (shell_bash_compat)
                                {
                                        //      A special builtin refused a name takes
                                        //      the script with it under posix, and
                                        //      nowhere else.
                                        if (shell_posix_on())
                                                exec_special_error_note();

                                        shell_answer(1);
                                        return;
                                }

                                exec_special_error_note();
                                shell_answer(2);
                                return;
                        }

                        attributes = shell_variable_attributes(word,
                                                               word_length);

                        if (!(attributes & SHELL_ARRAY_NAMEREF))
                        {
                                index++;
                                continue;
                        }

                        if (attributes & SHELL_ARRAY_READONLY)
                        {
                                shell_unset_readonly_refused(word, word_length);
                                exec_special_error_note();
                                shell_answer(shell_bash_compat ? 1 : 2);
                                return;
                        }

                        if (!shell_unset_variable(word, word_length))
                                return;
                        index++;
                        continue;
                }

                /*
                        `unset a[1]` forgets one element and leaves the array
                        standing, hole and all. The subscript is resolved the
                        way every other one is, so a[-1] and a[i+1] name the
                        same element here as they do when read.
                */
                if (bracket && word[word_length - 1] == ']' &&
                    bracket > word &&
                    shell_valid_name(word, (positive)(bracket - word)))
                {
                        positive base = (positive)(bracket - word);
                        positive key_length;
                        string_address key;

                        if (env_assignment_readonly_hashed_span(
                                word, base, env_name_hash(word, base)))
                        {
                                env_reference resolved =
                                    env_reference_span(word, base);

                                shell_unset_readonly_refused(
                                    (string_address)resolved.name,
                                    resolved.length);
                                exec_special_error_note();
                                shell_answer(shell_bash_compat ? 1 : 2);
                                return;
                        }

                        key = shell_expand_subscript(word, base, bracket + 1,
                                                     word_length - base - 2,
                                                     address_of key_length);

                        b32 detached = 0;
                        env_reference resolved = env_reference_span(word, base);
                        if (key && word_is(key, "0") && !resolved.element &&
                            !(shell_array_attributes(word, base) & SHELL_ARRAY_EITHER))
                                detached = exec_unset_prefix(resolved.name, resolved.length);
                        if (!key || detached < 0 ||
                            (!detached && !shell_array_forget(word, base, key, key_length)))
                        {
                                shell_answer(string_report(log_error, 2, "%s: no room\n", "unset"));
                                return;
                        }

                        index++;
                        continue;
                }

                if (!shell_valid_name(word, word_length))
                {
                        //      Bash steps over a name it cannot use and
                        //      still answers zero -- "unset - v" forgets v
                        //      -- unless the script said -v and meant a
                        //      variable by it, which is a name bash will
                        //      not have. dash refuses either way, and being
                        //      a special builtin takes the script with it.
                        if (shell_bash_compat && !variables)
                        {
                                index++;
                                continue;
                        }

                        shell_name_refused("unset", word, word_length);

                        if (shell_bash_compat)
                        {
                                //      A special builtin refused a name takes
                                //      the script with it under posix, and
                                //      nowhere else.
                                if (shell_posix_on())
                                        exec_special_error_note();

                                shell_answer(1);
                                return;
                        }

                        exec_special_error_note();
                        shell_answer(2);
                        return;
                }

                if (functions)
                {
                        b32 removed = exec_function_unset(word);

                        if (removed < 0)
                        {
                                string_format(log_error,
                                              "unset: %s: cannot unset: readonly function\n",
                                              word);
                                exec_special_error_note();
                                shell_answer(1);
                                return;
                        }
                }
                else
                {
                        env_reference resolved =
                            env_reference_span(word, word_length);

                        if (!resolved.valid)
                        {
                                shell_answer(1);
                                return;
                        }

                        if (!resolved.element &&
                            resolved.index < shell_var_count &&
                            (shell_vars[resolved.index].attributes &
                             SHELL_ARRAY_READONLY))
                        {
                                shell_unset_readonly_refused(
                                    (string_address)resolved.name,
                                    resolved.length);
                                exec_special_error_note();
                                shell_answer(shell_bash_compat ? 1 : 2);
                                return;
                        }

                        if (resolved.element)
                        {
                                if (!shell_reference_element_forget(resolved))
                                {
                                        shell_answer(1);
                                        return;
                                }
                        }
                        else if (!shell_unset_variable(resolved.name, resolved.length))
                                return;
                }

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
typedef struct
{
        shell_binding binding;
        bool detached;
} shell_local_entry;

static shell_local_entry address_to local_table;
static positive local_room;
static positive local_count;
static positive address_to local_from;
static positive local_from_room;
static positive local_depth;

/*
        `local -` is Bash's spelling of "put the option letters back the way
        they were when this function was entered". The three words below are
        the whole of what $- reads, so one copy of each per frame is the
        whole of what has to be kept; a frame that never asked for it is
        marked absent and unwinds nothing.
*/
#define SHELL_LOCAL_OPTIONS_MAX 64
static positive local_options_saved[SHELL_LOCAL_OPTIONS_MAX];
static positive local_options_named_saved[SHELL_LOCAL_OPTIONS_MAX];
static positive local_options_extra_saved[SHELL_LOCAL_OPTIONS_MAX];
static bool local_options_kept[SHELL_LOCAL_OPTIONS_MAX];

static PURE bool local_getopts_scope(string_address name, positive length)
{
        return length == 6 && !memory_compare(name, "OPTIND", 6);
}

static PURE p8 address_to local_getopts_saved(shell_local_entry address_to entry)
{
        return entry->binding.name + entry->binding.variable.name_length + 1;
}

/* A fresh Bash local hides the value and attributes, but inherits export
   visibility. Reuse the existing cell and name index when owned: unsetting
   and recreating the entire record would move the variable vector twice. */
static COLD bool local_hide_saved(string_address name, positive length,
                                  bool assigning)
{
        env_variable address_to entry = env_export_take(name, length);

        if (!entry)
                return false;
        if (!entry->owned)
        {
                env_cell address_to cell = env_cell_take(length + 1);
                if (!cell)
                        return false;
                memory_copy_end((p8 address_to)(cell + 1), name, length);
                entry->text = (string_address)(cell + 1);
                entry->owned = true;
        }
        else
                entry->text[length] = end;

        array_table_release(entry->array);
        entry->array = 0;
        entry->attributes = 0;
        entry->value_length = 0;
        entry->declared = true;
        shell_envp_dirty = true;
        if (length == 4 && memory_is_4(name, 'P', 'A', 'T', 'H'))
                hash_forget();
        env_locale_touch(name, length);
        // An explicit OPTIND initializer controls cursor reset itself. In
        // particular, local OPTIND=2 must retain a pending bundled byte.
        if (!assigning || !local_getopts_scope(name, length))
                env_unset_noted(name, length);
        return true;
}


bool shell_local_enter()
{
        if (local_depth == positive_max ||
            !shell_array_room(local_from, local_from_room, local_depth + 1))
                return string_report(log_error, false, "No room for function locals\n");

        local_from[local_depth] = local_count;
        if (local_depth < SHELL_LOCAL_OPTIONS_MAX)
                local_options_kept[local_depth] = false;
        local_depth++;
        return true;
}

fn shell_local_leave()
{
        if (!local_depth)
                return;
        local_depth--;
        if (local_depth < SHELL_LOCAL_OPTIONS_MAX &&
            local_options_kept[local_depth])
        {
                shell_options = local_options_saved[local_depth];
                shell_options_named = local_options_named_saved[local_depth];
                shell_extra_state = local_options_extra_saved[local_depth];
                local_options_kept[local_depth] = false;
        }
        while (local_count > local_from[local_depth])
        {
                shell_local_entry address_to entry = local_table + --local_count;
                if (!entry->detached)
                {
                        shell_binding_restore(&entry->binding);
                        if (local_getopts_scope(entry->binding.name, entry->binding.variable.name_length))
                        {
                                shell_getopts_state saved;
                                memory_copy(&saved, local_getopts_saved(entry), sizeof(saved));
                                shell_getopts_restore(saved);
                        }
                }
                shell_binding_drop(&entry->binding);
        }
}

// -1 is allocation failure, zero was already local in this frame, and one is
// the first declaration here. Callers need that distinction because Bash
// `declare x` hides an outer value but a second declaration keeps the local.
static b32 local_remember(string_address name)
{
        positive begin = local_depth ? local_from[local_depth - 1] : 0;
        for (positive at = begin; at < local_count; at++)
                if (!local_table[at].detached && !string_compare(local_table[at].binding.name, name))
                        return 0;
        if (local_count == positive_max ||
            !shell_array_room(local_table, local_room, local_count + 1))
                return -1;

        positive length = string_length(name);
        shell_pipe_status_wanted(name, length);
        shell_binding address_to saved = &local_table[local_count].binding;
        env_saved_state(saved, name, length);
        bool option_scope = local_getopts_scope(name, length);
        if (!shell_binding_hold(saved, option_scope ? sizeof(shell_getopts_state) : 0))
                return -1;
        local_table[local_count].detached = false;
        if (option_scope)
        {
                shell_getopts_state option = shell_getopts_save();
                memory_copy(local_getopts_saved(local_table + local_count), &option, sizeof(option));
                shell_getopts_parameters_changed();
        }
        local_count++;
        return 1;
}

static PURE shell_local_entry address_to local_saved_global(string_address name)
{
        for (positive at = 0; at < local_count; at++)
                if (!local_table[at].detached && !string_compare(local_table[at].binding.name, name))
                        return local_table + at;

        return null;
}

#define DECLARE_EXPORT 1
#define DECLARE_READONLY 2
#define DECLARE_PRINT 4
#define DECLARE_GLOBAL 8
// The attributes that live on the variable rather than beside it. They are
// held in the same order the option letters take, so that turning a letter
// into a bit is a table and not a ladder.
#define DECLARE_ATTRIBUTE 16
#define DECLARE_FUNCTION_NAMES 32
#define DECLARE_FUNCTION_BODY 64

typedef struct
{
        positive index;
        b32 set;
        b32 clear;
        p8 attributes_set;
        p8 attributes_clear;
} shell_declare_state;

static bool shell_declare_options(shell_declare_state address_to state)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        p8 sign[2];
        shell_option_walk walk = {state->index, null, 0, true};
        bool indexed_told = false;
        bool associative_told = false;
        p8 value;

        while (shell_option_letter(address_of walk, address_of value))
        {
                p8 direction = walk.direction;
                p8 attribute = shell_attribute_bits[value] & (SHELL_ARRAY_READONLY - 1);
                b32 flag;

                flag = value == 'x' ? DECLARE_EXPORT
                       : value == 'r' ? DECLARE_READONLY
                       : value == 'p' && direction == '-' ? DECLARE_PRINT
                       : value == 'g' && direction == '-' ? DECLARE_GLOBAL
                       : value == 'F' && direction == '-' ? DECLARE_FUNCTION_NAMES
                       : value == 'f' ? DECLARE_FUNCTION_BODY
                       : attribute ? DECLARE_ATTRIBUTE
                                   : 0;

                if (!flag)
                {
                        {
                                //      The sign is part of the word bash
                                //      names: `declare +Z` is refused as
                                //      +Z, not as -Z.
                                p8 said[3] = {direction, value, end};

                                if (shell_option_bad(shell_argv[0], said))
                                        string_format(log_error,
                                            "%s: usage: %s\n", shell_argv[0],
                                            shell_declare_usage(shell_argv[0]));
                        }
                        shell_answer(2);
                        return false;
                }

                //      Asking for both array kinds at once is refused, and
                //      Bash names -a whichever order the two arrived in.
                if (attribute == SHELL_ARRAY_INDEXED && direction == '-')
                        indexed_told = true;

                if (attribute == SHELL_ARRAY_ASSOCIATIVE && direction == '-')
                        associative_told = true;

                // Upper and lower fold in opposite directions and an array
                // has one kind, so asking for one of a pair withdraws the
                // other rather than leaving a name that is both.
                if (attribute && direction == '-')
                {
                        p8 opposite =
                            attribute == SHELL_ARRAY_LOWER ? SHELL_ARRAY_UPPER
                            : attribute == SHELL_ARRAY_UPPER
                                ? SHELL_ARRAY_LOWER
                            : attribute == SHELL_ARRAY_INDEXED
                                ? SHELL_ARRAY_ASSOCIATIVE
                            : attribute == SHELL_ARRAY_ASSOCIATIVE
                                ? SHELL_ARRAY_INDEXED
                                : 0;

                        state->attributes_set =
                            (p8)((state->attributes_set & (p8)~opposite) |
                                 attribute);
                        state->attributes_clear |= opposite;
                }
                else if (attribute)
                        state->attributes_clear |= attribute;

                if (direction == '-')
                        state->set |= flag;
                else
                        state->clear |= flag;
        }

        //      Asking for both array kinds at once is refused, and Bash
        //      names -a whichever order the two arrived in. Weighed after
        //      the walk and not inside it, because -p may still arrive:
        //      "declare -A -a -p v" is a listing to Bash, which reads the
        //      attributes it holds rather than the pair it was handed.
        if (indexed_told && associative_told && !(state->set & DECLARE_PRINT))
        {
                string_format(log_error, "%s: -a: invalid option\n",
                              shell_argv[0]);
                shell_answer(2);
                return false;
        }

        state->index = walk.index;
        state->set &= ~state->clear;
        state->attributes_set &= (p8)~state->attributes_clear;
        return true;
}

static fn shell_declare_quoted(writer write, string_address value)
{
        bool control = string_get(value +
                                  string_span(value, shell_quote_printable));

        write(control ? "$'" : "\"", 2 - !control);

        while (string_get(value))
        {
                positive run = string_span(value, control ? shell_quote_ansi
                                                          : shell_quote_double);
                if (run)
                {
                        write(value, run);
                        value += run;
                }
                if (!string_get(value))
                        break;
                p8 byte = string_get(value++);

                if (control)
                {
                        p8 escaped[4];
                        write(escaped, shell_ansi_byte(escaped, byte, true));
                }
                else
                {
                        if (byte == '\\' || byte == '"' || byte == '$' ||
                            byte == '`')
                                write("\\", 1);
                        write(address_of byte, 1);
                }
        }

        write(control ? "'" : "\"", 1);
}

// A subscript is written bare when it could be typed back bare, and quoted
// the way a value is when it could not. Bash draws the line at a name.
static COLD fn shell_declare_key(writer write, string_address key, positive length)
{
        if (string_span_max(key, length, string_set_name) != length)
        {
                shell_mark held =
                    shell_store_mark(address_of expand_store);
                p8 address_to kept = shell_store_take(
                    address_of expand_store, length + 1);

                if (kept)
                {
                        memory_copy_end(kept, key, length);
                        shell_declare_quoted(write, kept);
                }

                shell_store_rewind(address_of expand_store, held);
                return;
        }

        write(key, length);
}

static COLD fn shell_declare_elements(writer write, string_address name,
                                 positive length, bool keyed)
{
        positive count = shell_array_length(name, length);
        shell_mark held = shell_store_mark(address_of expand_store);
        shell_array_item address_to items =
            (shell_array_item address_to)shell_store_take(
                address_of expand_store,
                (count ? count : 1) * sizeof(items[0]));
        p8 written[32];

        if (!items)
                return;

        shell_array_items(name, length, items, count);
        write("=(", 2);

        for (positive at = 0; at < count; at++)
        {
                if (at)
                        write(" ", 1);

                write("[", 1);

                if (items[at].key)
                        shell_declare_key(write, items[at].key,
                                          items[at].key_length);
                else
                        write(written,
                              bipolar_into_string(written,
                                                  (bipolar)items[at].index));

                write("]=", 2);
                shell_declare_quoted(write, items[at].value);
        }

        // Bash leaves one space before the bracket of a keyed listing and
        // none before an indexed one. A listing is meant to be a line the
        // shell could be fed back, and a diff of two shells' listings should
        // show nothing, so the difference is kept rather than tidied. An
        // empty keyed array has no bracket to stand before, and Bash writes
        // that one as =() with nothing between.
        if (keyed && count)
                write(" ", 1);

        write(")", 1);
        shell_store_rewind(address_of expand_store, held);
}

static bool shell_declare_print_one(writer write, string_address name,
                                    positive length, b32 filter)
{
        shell_pipe_status_wanted(name, length);
        positive found = env_find_span(name, length);
        env_variable address_to variable =
            found < shell_var_count ? shell_vars + found : null;
        bool readonly = env_readonly((const_string)name);
        bool exported = variable && variable->permanent;
        p8 attributes = variable ? variable->attributes : 0;

        if ((!variable || !variable->declared) && !readonly)
                return false;

        if ((filter & DECLARE_EXPORT) && !exported)
                return false;
        if ((filter & DECLARE_READONLY) && !readonly)
                return false;

        write("declare -", 9);

        if (!readonly && !exported && !(attributes & ~SHELL_ARRAY_ASSIGNED))
                write("-", 1);
        else
        {
                p8 letters[8];
                positive count = shell_attribute_letters(letters, attributes,
                                                          readonly, exported);
                if (count)
                        write(letters, count);
        }

        write(" ", 1);
        write(name, length);

        if (attributes & SHELL_ARRAY_EITHER)
        {
                if (attributes & SHELL_ARRAY_ASSIGNED)
                        shell_declare_elements(
                            write, name, length,
                            (attributes & SHELL_ARRAY_ASSOCIATIVE) != 0);
        }
        else if (variable && env_variable_has_value(variable))
        {
                write("=", 1);
                shell_declare_quoted(write,
                                     variable->text + length + 1);
        }

        write("\n", 1);
        return true;
}

/*
        Every name the shell holds, sorted, handed to a writer one at a time.

        The names are copied out first because the vector holds NAME=VALUE
        and the writer will want to look the name up, which wants it on its
        own. An unset readonly declaration is a variable record with no value,
        so it naturally takes this same path instead of needing a second name
        registry.
*/
static inline INLINE bool shell_inventory_sorted(
    writer write, b32 mark, shell_name_writer written, bool functions, bool bodies)
{
        shell_mark held = shell_store_mark(address_of expand_store);
        string_address address_to names;
        positive count = 0;
        positive at = 0;
        string_address name;

        // Variable callbacks may publish PIPESTATUS, so capture it before
        // the name vector. Function names already have stable storage.
        if (functions)
                while (exec_function_next(address_of at, null))
                        count++;
        else
        {
                if (shell_bash_compat)
                        exec_pipe_status_wanted();
                count = shell_var_count;
        }

        if (!count)
                goto done;
        if (count > positive_max / sizeof(names[0]) ||
            !(names = (string_address address_to)shell_store_take(
                  address_of expand_store, count * sizeof(names[0]))))
                goto failed;

        count = 0;
        if (functions)
        {
                at = 0;
                while ((name = exec_function_next(address_of at, null)))
                        names[count++] = name;
        }
        else
                for (at = 0; at < shell_var_count; at++)
                {
                        positive length = shell_vars[at].name_length;
                        name = shell_store_copy(address_of expand_store,
                                                shell_vars[at].text, length);
                        if (!name)
                                goto failed;
                        names[count++] = name;
                }

        if (!expand_sort_names(names, count))
                goto failed;
        for (at = 0; at < count; at++)
        {
                if (bodies)
                {
                        if (!exec_function_write(write, names[at], mark))
                                goto failed;
                }
                else
                        written(write, names[at], string_length(names[at]), mark);
        }
done:
        shell_store_rewind(address_of expand_store, held);
        return true;

failed:
        shell_store_rewind(address_of expand_store, held);
        return false;
}

static fn shell_declare_written(writer write, string_address name,
                                positive length, b32 filter)
{
        shell_declare_print_one(write, name, length, filter);
}

/*
        A value written the way `declare` with no operands writes one.

        Bash quotes here only when the value has something in it that would
        not survive being read back as a word, so `a=b` comes out bare and
        `a b` comes out quoted. Bash's bare set shares this spelling; dash
        continues to quote every scalar. Control bytes use the existing
        ANSI-C serializer instead of embedding literal newlines in a listing.
*/
static COLD PURE bool shell_listing_quoted(string_address value)
{
        static const b8 plain[STRING_SET_BYTES] = {
            [1 ... 8] = 1, [11 ... 31] = 1,
            [35] = 1, [37] = 1, [43 ... 58] = 1, [61] = 1,
            [64 ... 90] = 1, [95] = 1, [97 ... 122] = 1,
            [126 ... 255] = 1
        };

        return string_is(value, '#') || string_is(value, '~') ||
               string_get(value + string_span(value, plain));
}

static COLD fn shell_listing_value(writer write, string_address value)
{
        if (!shell_posix_on() &&
            string_get(value + string_span(value, shell_quote_printable)))
                shell_declare_quoted(write, value);
        else if (shell_listing_quoted(value))
                shell_quoted(write, value);
        else
                write(value, string_length(value));
}

static COLD fn shell_declare_listed(writer write, string_address name,
                                    positive length, b32 mark)
{
        positive found = env_find_span(name, length);
        string_address value;

        (void)mark;

        if (found >= shell_var_count ||
            !env_variable_has_value(shell_vars + found))
                return;

        value = shell_vars[found].text + length + 1;

        write(name, length);
        write("=", 1);

        shell_listing_value(write, value);

        write("\n", 1);
}

// local and declare have different scope and failure policy, but write a
// value with the same scalar/compound/append machinery once that policy has
// accepted the name.
static bool exec_declaration_compound(string_address word);
static b32 shell_declare_value(string_address name, positive length,
                               string_address mark, bool append,
                               bool bind_reference, bool declare_empty,
                               env_variable address_to destination)
{
        if (!mark)
        {
                if (destination)
                        destination->declared = true;
                return destination || !declare_empty || env_declare(name, length);
        }

        if (exec_declaration_compound(name) ||
            ((shell_array_attributes(name, length) & SHELL_ARRAY_EITHER) &&
             string_is(mark + 1, '(')))
        {
                positive body = string_length(mark + 1);

                return shell_compound_assign(name, length, mark + 2,
                                             body > 2 ? body - 2 : 0,
                                             append)
                           ? 1 : -1;
        }

        if (destination)
        {
                name = destination->text;
                length = destination->name_length;
                // An absent dynamic name updates its process state, even
                // when a temporary scalar currently hides it from lookup.
                if (!bind_reference && !destination->declared && !destination->permanent &&
                    !destination->attributes && !env_variable_has_value(destination) &&
                    shell_dynamic_assign(name, length, mark + 1))
                        return true;
        }
        return shell_scalar_assign_destination(name, length, env_name_hash(name, length),
                                    mark + 1, append, bind_reference, destination);
}

/* `declare -F` is metadata, not body serialization. Named queries retain the
   operand order Bash uses; the no-operand inventory is sorted through the
   same pointer sorter as variable/function completion. */
static COLD fn shell_declare_function_written(writer write,
                                              string_address name,
                                              positive length, b32 mark)
{
        positive2 named = {
            memory_hash_33((address_any)name, length), length};
        b32 attributes = exec_function_attributes_hashed(name, named);

        if (mark && !(attributes & mark))
                return;
        write("declare -f", 10);
        if (attributes & DECLARE_READONLY)
                write("r", 1);
        if (attributes & DECLARE_EXPORT)
                write("x", 1);
        write(" ", 1);
        write(name, length);
        write("\n", 1);
}

static COLD b32 shell_declare_functions(writer write, positive index,
                                        bool bodies)
{
        if (index < shell_argc)
        {
                bool failed = false;

                while (index < shell_argc)
                {
                        string_address name = shell_argv[index++];
                        positive2 named = string_hash_33_length(name);

                        if (!exec_function_here_hashed(name, named))
                        {
                                failed = true;
                                continue;
                        }

                        if (bodies)
                        {
                                if (!exec_function_write(write, name, 0))
                                        return -1;
                        }
                        else
                        {
                                write(name, named.y);
                                write("\n", 1);
                        }
                }

                return failed ? 0 : 1;
        }

        return shell_inventory_sorted(write, 0, shell_declare_function_written,
                                       true, bodies) ? 1 : -1;
}

/* Declarations share one mutation engine with explicit value destinations.
   Local and global ownership retain their own failure and metadata policy. */
static p8 shell_assignment_kind(string_address word,
                                positive address_to name_length);
static env_variable address_to exec_declare_global_destination(string_address name,
    positive length, shell_declare_state address_to state, string_address value,
    bool address_to address_to promoted);
static inline INLINE fn shell_declare_apply(shell_declare_state address_to state, bool local_mode)
{
        bool failed = false;

        while (state->index < shell_argc)
        {
                string_address word = shell_argv[state->index++];
                positive length;
                p8 assignment = shell_assignment_kind(word, address_of length);
                bool append = assignment == 2;
                string_address mark = assignment ? word + length + append
                                                 : null;
                if (!assignment)
                        length = string_length(word);
                positive base, subscript_length = 0;
                const_string subscript = null;
                if (env_reference_element_span(word, length, address_of base,
                        address_of subscript, address_of subscript_length))
                        length = base;
                string_address name_end = mark || subscript ? word + length : null;
                bool scoped = local_mode || (local_depth && !(state->set & DECLARE_GLOBAL));
                shell_local_entry address_to saved_global = null;
                env_variable address_to global_scope = null;
                env_variable address_to global_meta = null;
                bool address_to global_promoted = null;
                bool global_element = false;
                bool saved_scalar = false;
                bool global_adopt = subscript && mark &&
                    (state->set & (DECLARE_EXPORT | DECLARE_READONLY));
                p8 delimiter = name_end ? string_get(name_end) : 0;
                p8 held_attributes;
                b32 stored;
                bool readonly;

                if (!shell_valid_name(word, length) ||
                    (subscript && (!subscript_length ||
                                   (state->attributes_set & SHELL_ARRAY_NAMEREF))))
                {
                        shell_name_refused(shell_argv[0], word, length);
                        exec_special_error_note();
                        shell_answer(shell_bash_compat ? 1 : 2);

                        //      Bash names the word it cannot use and carries
                        //      on down the list, so "declare - v=1" still
                        //      leaves v set and answers one.
                        if (!shell_bash_compat)
                                return;

                        failed = true;
                        goto next;
                }

                if (name_end)
                        address_to name_end = end;
                if (!local_mode && (state->set & DECLARE_GLOBAL) &&
                    !exec_declaration_compound(word))
                {
                        global_scope = exec_declare_global_destination(word, length, state,
                            mark && !subscript ? mark + 1 : null, &global_promoted);
                        if (global_scope && global_scope->name_length == length &&
                            !memory_compare(global_scope->text, word, length))
                                global_meta = global_scope;
                }
                saved_global = !local_mode && !global_scope && (state->set & DECLARE_GLOBAL)
                                   ? local_saved_global(word)
                                   : null;
                saved_scalar = saved_global && !subscript;

                readonly = global_meta ? (global_meta->attributes & SHELL_ARRAY_READONLY) != 0
                                       : env_readonly(word);

                if (readonly && (scoped || (state->clear & DECLARE_READONLY)))
                {
                        string_format(log_error,
                                      "%s: %s: readonly variable\n",
                                      shell_argv[0], word);
                        if (local_mode)
                        {
                                if (name_end)
                                        *name_end = delimiter;
                                return shell_answer(shell_bash_compat ? 1 : 2);
                        }
                        failed = true;
                        goto next;
                }

                if (scoped)
                {
                        b32 fresh = local_remember(word);

                        if (fresh < 0 ||
                            (fresh && (!local_mode || shell_bash_compat) &&
                             !shell_shopt_on(LOCALVAR_INHERIT) &&
                             !local_hide_saved(word, length, mark != null)))
                        {
                                if (local_mode)
                                {
                                        if (name_end)
                                                *name_end = delimiter;
                                        return shell_answer(string_report(log_error, 2, "local: too many\n"));
                                }
                                goto no_room;
                        }
                }

                /*
                        An array has one kind for its whole life. Bash
                        refuses to reinterpret the subscripts it already
                        holds rather than answering to both spellings.
                */
                held_attributes = saved_global ? saved_global->binding.variable.attributes
                                  : global_meta ? global_meta->attributes : shell_variable_attributes(word, length);

                /*
                        A name that already holds an array, asked to become a
                        reference: refused outright, and it keeps what it
                        holds. Asking for both kinds at once is a different
                        question and is weighed below, after the reference's
                        own target has been read -- bash looks at the target
                        first, so `declare -an r="bad name"` is a bad name and
                        not a bad combination.
                */
                if ((state->attributes_set & SHELL_ARRAY_NAMEREF) &&
                    (held_attributes & SHELL_ARRAY_EITHER))
                {
                        string_format(log_error, "%s: %s: reference variable cannot be an array\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }

                if ((state->attributes_set & SHELL_ARRAY_EITHER) &&
                    (held_attributes & SHELL_ARRAY_EITHER) &&
                    (held_attributes & SHELL_ARRAY_EITHER) !=
                        (state->attributes_set & SHELL_ARRAY_EITHER))
                {
                        string_format(
                            log_error,
                            "%s: %s: cannot convert %s to %s array\n",
                            shell_argv[0], word,
                            (held_attributes & SHELL_ARRAY_ASSOCIATIVE)
                                ? "associative"
                                : "indexed",
                            (state->attributes_set & SHELL_ARRAY_ASSOCIATIVE)
                                ? "associative"
                                : "indexed");
                        failed = true;
                        goto next;
                }

                if (saved_scalar && ((state->set & DECLARE_READONLY) ||
                    ((state->clear & DECLARE_READONLY) && readonly) || (mark && readonly)))
                {
                        string_format(log_error, "%s: %s: readonly variable\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }
                if (saved_scalar)
                        goto store_value;

                // The kind is decided before the value is written, because
                // an associative array reads its subscripts as bytes and an
                // indexed one as arithmetic, and the value about to be
                // assigned is full of subscripts.
                p8 set = state->attributes_set, clear = state->attributes_clear;
                positive previous = env_find_span(word, length);
                env_variable address_to previous_variable = global_meta ? global_meta
                    : previous < shell_var_count ? shell_vars + previous : null;
                bool existed = previous_variable && (previous_variable->declared ||
                    previous_variable->permanent || previous_variable->attributes ||
                    env_variable_has_value(previous_variable));
                p8 previous_attributes = existed ? previous_variable->attributes : 0;
                /*
                        A reference to itself. Following it would be a loop
                        with one link in it, so bash refuses the declaration
                        by name rather than leaving a name that cannot be
                        read. Said before the target is weighed for being a
                        name at all, because "it is this one" is the more
                        particular complaint of the two.
                */
                if ((set & SHELL_ARRAY_NAMEREF) && mark && !append &&
                    string_get(mark + 1) && !string_compare(mark + 1, word))
                {
                        string_format(log_error,
                                      "%s: %s: nameref variable self "
                                      "references not allowed\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }

                if ((set & SHELL_ARRAY_NAMEREF) &&
                    ((mark && !append && string_get(mark + 1) &&
                      !shell_declare_target_valid(mark + 1)) ||
                     (!mark && existed && env_variable_has_value(previous_variable) &&
                      !shell_declare_target_valid(previous_variable->text + length + 1))))
                {
                        failed = true;
                        goto next;
                }
                /*
                        Both kinds asked for, and the target read and found
                        good. bash weighs the array and drops the reference:
                        what it makes is an array. Under a scope it says the
                        two cannot be combined and makes the array anyway, so
                        the complaint and the drop go together.
                */
                if ((set & SHELL_ARRAY_NAMEREF) && (set & SHELL_ARRAY_EITHER))
                {
                        if (scoped)
                        {
                                string_format(log_error, "%s: %s: reference variable cannot be an array\n",
                                              shell_argv[0], word);
                                failed = true;
                        }

                        set &= (p8)~SHELL_ARRAY_NAMEREF;
                        state->attributes_set &= (p8)~SHELL_ARRAY_NAMEREF;
                }

                if ((set & SHELL_ARRAY_NAMEREF) && !(set & SHELL_ARRAY_INTEGER))
                        clear |= SHELL_ARRAY_INTEGER;
                // Integer evaluation always rejects a new binding; its RHS
                // still assigns the original scalar or existing reference.
                if (mark && (set & SHELL_ARRAY_INTEGER) &&
                    !(previous_attributes & SHELL_ARRAY_NAMEREF))
                        set &= (p8)~SHELL_ARRAY_NAMEREF;
                if (subscript)
                {
                        if (saved_global)
                        {
                                // Bash marks the global declaration, but an
                                // element operand writes the visible local.
                                if (held_attributes & SHELL_ARRAY_NAMEREF)
                                        saved_global->binding.variable.text[length] = end;
                                if (!(set & SHELL_ARRAY_EITHER) &&
                                    !(held_attributes & SHELL_ARRAY_EITHER))
                                        set |= SHELL_ARRAY_INDEXED;
                                if ((set & SHELL_ARRAY_EITHER) && !saved_global->binding.variable.array)
                                {
                                        saved_global->binding.variable.array = array_table_take();
                                        if (!saved_global->binding.variable.array)
                                                goto no_room;
                                }
                                saved_global->binding.variable.attributes =
                                    (held_attributes & (p8)~(clear | SHELL_ARRAY_NAMEREF)) | set;
                                if (mark || env_variable_value(&saved_global->binding.variable))
                                        saved_global->binding.variable.attributes |= SHELL_ARRAY_ASSIGNED;
                                if (state->set & DECLARE_READONLY)
                                        saved_global->binding.variable.attributes |= SHELL_ARRAY_READONLY;
                                if (state->clear & DECLARE_EXPORT)
                                        saved_global->binding.variable.permanent = false;
                                if (state->set & DECLARE_EXPORT)
                                        saved_global->binding.variable.permanent = true;
                                saved_global->binding.variable.declared = true;
                                set = clear = 0;
                        }
                        else
                        {
                                // Declare converts the containing record itself,
                                // after a fresh local has saved the outer binding.
                                p8 attributes = global_meta ? global_meta->attributes : shell_variable_attributes(word, length);
                                if (attributes & SHELL_ARRAY_NAMEREF)
                                {
                                        if (global_meta)
                                        {
                                                global_meta->text[length] = end;
                                                global_meta->attributes = (attributes & (p8)~SHELL_ARRAY_NAMEREF) | SHELL_ARRAY_ASSIGNED;
                                        }
                                        else if (!env_value_restore(word, length, null,
                                            (attributes & (p8)~SHELL_ARRAY_NAMEREF) | SHELL_ARRAY_ASSIGNED, 0))
                                                goto no_room;
                                }
                                if (mark && readonly)
                                {
                                        shell_readonly_refused(shell_argv[0], shell_argv[0], word,
                                                               length);
                                        exec_special_error_note();
                                        shell_answer(shell_bash_compat ? 1 : 2);
                                        failed = true;
                                        goto next;
                                }
                                if (!(set & SHELL_ARRAY_EITHER) &&
                                    !((global_meta ? global_meta->attributes : shell_variable_attributes(word, length)) & SHELL_ARRAY_EITHER))
                                        set |= SHELL_ARRAY_INDEXED;
                                clear |= SHELL_ARRAY_NAMEREF;
                                if (state->set & DECLARE_READONLY)
                                        set |= SHELL_ARRAY_READONLY;
                        }
                }
                if ((set & SHELL_ARRAY_INDEXED) && existed &&
                    env_variable_has_value(previous_variable))
                        set |= SHELL_ARRAY_ASSIGNED;
                if ((set || clear) &&
                    !shell_variable_attribute_destination(word, length,
                                                  set, clear, global_meta))
                        goto no_room;

                if (global_scope && mark && !subscript &&
                    ((global_meta ? global_meta->attributes : shell_variable_attributes(word, length)) & SHELL_ARRAY_INDEXED) &&
                    !string_is(mark + 1, '('))
                {
                        subscript = "0";
                        subscript_length = 1;
                        if (!shell_variable_attribute_destination(word, length, SHELL_ARRAY_ASSIGNED, 0, global_meta))
                                goto no_room;
                }
                if (global_scope && subscript)
                {
                        if (mark && !shell_variable_attribute_destination(word, length,
                            SHELL_ARRAY_ASSIGNED, 0, global_meta))
                                goto no_room;
                        if (state->clear & DECLARE_EXPORT)
                                global_scope->permanent = false;
                        if (state->set & DECLARE_EXPORT)
                                global_scope->permanent = true;
                        if (state->set & DECLARE_READONLY)
                                global_scope->attributes |= SHELL_ARRAY_READONLY;
                        *global_promoted |= global_adopt;
                        global_scope = global_meta = null;
                        global_element = true;
                        readonly = env_readonly(word);
                }

                if (mark && readonly)
                {
                        shell_readonly_refused(shell_argv[0], shell_argv[0], word,
                                               length);
                        exec_special_error_note();
                        shell_answer(shell_bash_compat ? 1 : 2);
                        failed = true;
                        goto next;
                }
                if (subscript && mark && exec_declaration_compound(word))
                {
                        string_format(log_error, "%s: cannot assign list to array member\n", word);
                        address_to name_end = delimiter;
                        return expand_fatal_status(1);
                }
        store_value:
                if (saved_scalar)
                {
                        env_variable address_to destination = &saved_global->binding.variable;
                        p8 attributes = destination->attributes;
                        bool exported = destination->permanent;
                        destination->attributes = 0;
                        stored = !mark || shell_scalar_assign_destination(word, length,
                            destination->hash, mark + 1, append, false, destination);
                        destination->attributes = attributes;
                        destination->permanent = exported;
                        if (stored)
                                destination->declared = true;
                }
                else if (subscript && mark)
                {
                        if (!saved_global && !global_element && (state->set & DECLARE_READONLY))
                        {
                                string_format(log_error, "%s: readonly variable\n", word);
                                stored = true;
                        }
                        else
                                stored = shell_reference_assign(((env_reference){
                                    .name = word, .length = length,
                                    .subscript = subscript, .subscript_length = subscript_length}),
                                    mark + 1, append);
                }
                else
                        stored = shell_declare_value(word, length, mark, append,
                            (state->attributes_set & SHELL_ARRAY_NAMEREF) != 0, !local_mode, global_scope);
                if (stored <= 0)
                {
                        if (saved_scalar)
                                goto no_room;
                        if (mark && (state->attributes_set & SHELL_ARRAY_NAMEREF) &&
                            shell_declare_binding_rejected)
                        {
                                if (append && (state->attributes_set & SHELL_ARRAY_INTEGER))
                                        shell_declare_target_valid(mark + 1);
                                if (!existed && !global_meta)
                                        env_unset_span(word, length);
                                else if (!shell_variable_attribute_destination(word, length,
                                    previous_attributes & SHELL_ARRAY_NAMEREF,
                                    previous_attributes & SHELL_ARRAY_NAMEREF ? 0 : SHELL_ARRAY_NAMEREF,
                                    global_meta))
                                        goto no_room;
                                failed = true;
                                goto next;
                        }
                        if (!stored && env_assignment_readonly_destination(
                                word, length, env_name_hash(word, length), global_scope))
                        {
                                shell_readonly_refused(shell_argv[0], shell_argv[0], word,
                                                       length);
                                exec_special_error_note();
                                shell_answer(shell_bash_compat ? 1 : 2);
                                failed = true;
                                goto next;
                        }
                        if (local_mode && !stored && env_readonly(word))
                        {
                                shell_readonly_refused(shell_argv[0], shell_argv[0], word,
                                                       length);
                                if (name_end)
                                        *name_end = delimiter;
                                return shell_answer(2);
                        }
                        goto no_room;
                }

                if (global_scope || saved_scalar)
                {
                        env_variable address_to destination = global_scope ? global_scope
                            : &saved_global->binding.variable;
                        if (state->set & (DECLARE_EXPORT | DECLARE_READONLY))
                                destination->declared = true;
                        if (state->clear & DECLARE_EXPORT)
                                destination->permanent = false;
                        if (state->set & DECLARE_EXPORT)
                                destination->permanent = true;
                        if (state->set & DECLARE_READONLY)
                                destination->attributes |= SHELL_ARRAY_READONLY;
                }
                if (saved_global || global_element || global_scope)
                        goto next;

                if (state->clear & DECLARE_EXPORT)
                        env_export_restore(word, false);
                if ((state->set & DECLARE_EXPORT) &&
                    !env_export_mark_span_mode(word, length,
                        scoped || (state->attributes_set & SHELL_ARRAY_NAMEREF)))
                {
                        if (local_mode)
                                goto no_room;
                        failed = true;
                }
                if ((state->set & DECLARE_READONLY) &&
                    !readonly_add_mode(word, length,
                        scoped || (state->attributes_set & SHELL_ARRAY_NAMEREF)))
                {
                        if (local_mode)
                                goto no_room;
                        failed = true;
                }

        next:
                if (name_end)
                        address_to name_end = delimiter;
                continue;

        no_room:
                if (name_end)
                        address_to name_end = delimiter;
                return shell_answer(string_report(log_error, 2, "%s: no room\n", local_mode ? (string_address)"local" : (string_address)"declare"));
        }

        shell_answer(failed ? 1 : 0);
}

COLD fn shell_local(writer write, string_address input)
{
        shell_declare_state state = {1};

        if (!local_depth)
        {
                shell_diagnostic_where();
                log_error(shell_bash_compat
                              ? "local: can only be used in a function\n"
                              : "local: not in a function\n", 0);

                //      Bash answers one and carries on with the line it
                //      was given; dash answers two and the script ends
                //      there, because local is a special builtin to it.
                if (shell_bash_compat)
                {
                        shell_answer(1);

                        return;
                }

                expand_fatal_status(2);
                return;
        }

        //      Dash has no option letters on local: `--` and `--bogus`
        //      are names, and a bad name is the special-builtin abort.
        //      Bash walks the same option list declare does.
        if (!shell_bash_compat)
        {
                (void)write;
                (void)input;
                state.index = 1;
                if (state.index >= shell_argc)
                        return shell_answer(0);
                shell_declare_apply(address_of state, true);
                return;
        }

        if (!shell_declare_options(address_of state))
                return;

        //      A lone "-" is a name to the walk and an instruction to Bash:
        //      keep the option letters as they are now and put them back
        //      when this function returns.
        while (state.index < shell_argc && word_is(shell_argv[state.index], "-"))
        {
                if (local_depth <= SHELL_LOCAL_OPTIONS_MAX &&
                    !local_options_kept[local_depth - 1])
                {
                        local_options_saved[local_depth - 1] = shell_options;
                        local_options_named_saved[local_depth - 1] =
                            shell_options_named;
                        local_options_extra_saved[local_depth - 1] =
                            shell_extra_state;
                        local_options_kept[local_depth - 1] = true;
                }

                state.index++;
        }

        if (state.index >= shell_argc)
                return shell_answer(0);

        shell_declare_apply(address_of state, true);
}

static fn shell_declare(writer write, string_address input)
{
        shell_declare_state state = {1};
        bool failed = false;

        (void)input;

        if (!shell_declare_options(address_of state))
                return;

        if (state.set & (DECLARE_FUNCTION_NAMES | DECLARE_FUNCTION_BODY))
        {
                b32 listed;
                bool bodies = (state.set & DECLARE_FUNCTION_BODY) != 0;
                b32 attributes = state.set &
                                 (DECLARE_EXPORT | DECLARE_READONLY);

                if (((state.set & DECLARE_FUNCTION_NAMES) && bodies) ||
                    (state.set & ~(DECLARE_FUNCTION_NAMES |
                                   DECLARE_FUNCTION_BODY | DECLARE_PRINT |
                                   DECLARE_EXPORT | DECLARE_READONLY)) ||
                    state.attributes_set || state.attributes_clear ||
                    (state.clear & ~(DECLARE_FUNCTION_BODY | DECLARE_EXPORT)))
                        return shell_answer(string_report(log_error, 2,
                            "declare: function attribute combination is not supported\n"));

                /* With an attribute, -f selects function targets instead of
                   asking for their bodies. The same export/readonly bits are
                   used by the dedicated builtins below. */
                if (attributes || (state.clear & DECLARE_EXPORT))
                {
                        bool failed = false;

                        if (state.index >= shell_argc)
                        {
                                if (attributes)
                                        return shell_answer(
                                            shell_inventory_sorted(
                                                write, attributes,
                                                shell_declare_function_written, true,
                                                bodies)
                                                ? 0 : 1);
                                return shell_answer(string_report(log_error, 2,
                                    "declare: function attribute removal wants a name\n"));
                        }

                        while (state.index < shell_argc)
                        {
                                string_address name = shell_argv[state.index++];
                                positive2 named = string_hash_33_length(name);
                                bool okay = true;

                                if (!exec_function_here_hashed(name, named))
                                        okay = false;
                                else if (state.clear & DECLARE_EXPORT)
                                        okay = exec_function_export_set(name,
                                                                          false);
                                if (okay && (attributes & DECLARE_EXPORT))
                                        okay = exec_function_export_set(name,
                                                                          true);
                                if (okay && (attributes & DECLARE_READONLY))
                                        okay = exec_function_readonly_set(name);
                                if (!okay)
                                {
                                        string_format(log_error,
                                                      "declare: %s: function cannot be marked\n",
                                                      name);
                                        failed = true;
                                }
                        }
                        return shell_answer(failed ? 1 : 0);
                }

                listed = shell_declare_functions(write, state.index, bodies);
                if (listed < 0)
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "declare"));
                return shell_answer(listed ? 0 : 1);
        }

        if ((state.set & DECLARE_PRINT) || state.index >= shell_argc)
        {
                if (state.index < shell_argc)
                {
                        while (state.index < shell_argc)
                        {
                                string_address name = shell_argv[state.index++];
                                positive length = string_length(name);

                                if (!shell_valid_name(name, length) ||
                                    !shell_declare_print_one(write, name, length,
                                                             state.set))
                                {
                                        string_format(log_error,
                                                      "%s: %s: not found\n",
                                                      shell_argv[0], name);
                                        failed = true;
                                }
                        }
                }
                else if (state.set & DECLARE_PRINT)
                        failed = !shell_inventory_sorted(
                            write, state.set, shell_declare_written, false, false);
                else
                {
                        /*
                                Named nothing and not asked for -p, so this is
                                the listing and not the print: Bash writes the
                                variables the way `set` does, as assignments a
                                shell could be fed, rather than as the declare
                                commands that would rebuild their attributes.
                        */
                        failed = !shell_inventory_sorted(
                            write, 0, shell_declare_listed, false, false);
                }

                shell_answer(failed ? 1 : 0);
                return;
        }

        shell_declare_apply(address_of state, false);
}

/*
        export and readonly, which are one command with two marks.

        Both take -p, list what carries their mark when given nothing else,
        and otherwise walk their words: a name alone gets the mark, and
        name=value is assigned first and marked after. The two bodies had
        drifted a word apart in four places, which is how a script comes to
        see readonly refuse what export allowed. The listing is sorted and
        quoted the way the reference shell's is, through the walk declare -p
        already had.
*/
static fn shell_marked_written(writer write, string_address name,
                               positive length, b32 mark)
{
        if (shell_bash_compat && !shell_posix_on())
        {
                shell_declare_print_one(write, name, length, mark);
                return;
        }
        shell_pipe_status_wanted(name, length);
        positive found = env_find_span(name, length);
        env_variable address_to variable =
            found < shell_var_count ? shell_vars + found : null;

        if (mark == DECLARE_EXPORT ? !(variable && variable->permanent)
                                   : !env_readonly(name))
                return;

        if (mark == DECLARE_EXPORT)
                write("export ", 7);
        else
                write("readonly ", 9);

        write(name, length);

        if (variable && env_variable_has_value(variable))
        {
                write("=", 1);
                if (shell_bash_compat)
                        shell_declare_quoted(write,
                                             variable->text + length + 1);
                else
                        shell_quoted(write, variable->text + length + 1);
        }

        write("\n", 1);
}

static COLD fn shell_marked(writer write, p8 mark)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        string_address command = mark == DECLARE_EXPORT ? "export"
                                                        : "readonly";
        bool listed = shell_argc < 2;
        bool functions = false;
        bool unmark = false;
        bool refused = false;
        shell_option_walk walk = {1};
        p8 option;
        positive index;

        while (shell_option_letter(address_of walk, address_of option))
        {
                if (option == 'p')
                        listed = true;
                else if (option == 'f' && shell_bash_compat)
                        functions = true;
                else if (option == 'n' && shell_bash_compat &&
                         mark == DECLARE_EXPORT && walk.direction == '-')
                        unmark = true;
                else
                {
                        //      export and readonly are special builtins, and
                        //      a special builtin refused its options takes a
                        //      script with it wherever POSIX says so.
                        exec_special_error_note();
                        return shell_answer(shell_letter_refused(
                            command, option,
                            word_is(command, "export")
                                ? "export [-fn] [name[=value] ...] or "
                                  "export -p [-f]"
                                : "readonly [-aAf] [name[=value] ...] or "
                                  "readonly -p"));
                }
        }

        index = walk.index;

        if (functions)
        {
                bool failed = false;

                if (listed || index >= shell_argc)
                {
                        if (index >= shell_argc)
                                return shell_answer(
                                    shell_inventory_sorted(write, mark, null, true, true)
                                        ? 0 : 1);

                        while (index < shell_argc)
                        {
                                string_address name = shell_argv[index++];
                                positive2 named = string_hash_33_length(name);

                                if (!(exec_function_attributes_hashed(
                                          name, named) & mark) ||
                                    !exec_function_write(write, name, mark))
                                        failed = true;
                        }
                        return shell_answer(failed ? 1 : 0);
                }

                while (index < shell_argc)
                {
                        string_address name = shell_argv[index++];
                        positive2 named = string_hash_33_length(name);
                        bool exists = exec_function_here_hashed(name, named);

                        if (string_first_of(name, '=') ||
                            !exists ||
                            !(mark == DECLARE_EXPORT
                                  ? exec_function_export_set(name, !unmark)
                                  : exec_function_readonly_set(name)))
                        {
                                if (exists && mark == DECLARE_EXPORT &&
                                    !unmark)
                                        string_format(
                                            log_error,
                                            "export: %s: function body cannot be exported\n",
                                            name);
                                else
                                        string_format(
                                            log_error,
                                            "%s: %s: not a function\n",
                                            command, name);
                                failed = true;
                        }
                }

                return shell_answer(failed ? 1 : 0);
        }

        if (listed && index >= shell_argc)
        {
                if (!shell_inventory_sorted(write, mark, shell_marked_written, false, false))
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", command));

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index++];
                string_address value = string_first_of(word, '=');
                positive length = value ? (positive)(value - word)
                                        : string_length(word);
                bool kept;

                if (!shell_valid_name(word, length))
                {
                        shell_name_refused(command, word, length);
                        exec_special_error_note();

                        //      Under posix the refusal takes the script
                        //      with it, so bash never reaches the next word
                        //      to complain about that one too.
                        if (shell_posix_on())
                                return shell_answer(1);

                        //      Bash names the word it will not have and goes
                        //      on to the next one -- "export - v=1" leaves v
                        //      set to 1 and answers 1. dash stops there, and
                        //      being a special builtin the script stops too.
                        if (!shell_bash_compat)
                                return shell_answer(2);

                        refused = true;
                        continue;
                }

                // The name on its own while it is looked up and marked; the
                // word is argv's and goes back the way it was.
                if (value)
                        address_to value = end;

                shell_pipe_status_wanted(word, length);

                if (value && env_readonly(word))
                {
                        address_to value = '=';
                        shell_readonly_refused(null, command, word, length);
                        exec_special_error_note();
                        shell_answer(shell_bash_compat ? 1 : 2);
                        return;
                }

                // A name on its own is already marked here: every value
                // assigned to it later inherits the attribute. `export -n`
                // deliberately does not create a missing variable.
                if (mark == DECLARE_EXPORT && unmark)
                {
                        kept = !value || env_assign(word, value + 1);
                        if (kept)
                                kept = env_export_unmark(word);
                }
                else
                        kept = (!value || env_assign(word, value + 1)) &&
                               (mark == DECLARE_EXPORT
                                    ? env_export_mark(word)
                                    : readonly_add_mode(word, length, false));

                if (value)
                        address_to value = '=';

                if (!kept)
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", command));
        }

        shell_answer(refused ? 1 : 0);
}

COLD fn shell_export(writer write, string_address input)
{
        shell_marked(write, DECLARE_EXPORT);
}

COLD fn shell_readonly(writer write, string_address input)
{
        shell_marked(write, DECLARE_READONLY);
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

/* Lowercase unary primaries as a bit per letter, so -n/-z/-f do not walk
   twenty-one comparisons on every two-word form. G/L/O/S stay beside it. */
#define TEST_UNARY_LOWER                                                       \
        ((1u << ('b' - 'a')) | (1u << ('c' - 'a')) | (1u << ('d' - 'a')) |     \
         (1u << ('e' - 'a')) | (1u << ('f' - 'a')) | (1u << ('g' - 'a')) |     \
         (1u << ('h' - 'a')) | (1u << ('k' - 'a')) | (1u << ('n' - 'a')) |     \
         (1u << ('p' - 'a')) | (1u << ('r' - 'a')) | (1u << ('s' - 'a')) |     \
         (1u << ('t' - 'a')) | (1u << ('u' - 'a')) | (1u << ('w' - 'a')) |     \
         (1u << ('x' - 'a')) | (1u << ('z' - 'a')))

static positive test_at;
static positive test_stop;
static bool test_bad;

bool test_facts(string_address path, file_facts address_to out, bool follow)
{
        return system_stat_at(AT_FDCWD, path,
                              follow ? 0 : AT_SYMLINK_NOFOLLOW,
                              STATX_BASIC, out) == 0;
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
                bipolar descriptor = 1;

                // strtol's grammar, which is what the reference shell reads
                // the descriptor with: blanks either side are fine, and
                // anything else after the digits is not a number, which is
                // an error and not a false.
                if (value)
                {
                        string_address step =
                            value + string_span(value, string_set_blanks);
                        positive used;

                        descriptor = string_bipolar(step, address_of used);
                        step += used;
                        step += string_span(step, string_set_blanks);

                        if (!used || string_get(step))
                        {
                                shell_test_number_refused(value);
                                test_bad = true;
                                return false;
                        }
                }

                return system_control(descriptor, BUILTIN_TCGETS, settings) == 0;
        }

        if (op == 'r' || op == 'w' || op == 'x')
        {
                positive mode = op == 'r' ? ACCESS_READ
                                          : (op == 'w' ? ACCESS_WRITE : ACCESS_EXECUTE);

                return system_access_at(AT_FDCWD, value, mode) == 0;
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
#define test_modified(facts) ((facts)->modified.seconds)
#define test_modified_fraction(facts) ((facts)->modified.nanoseconds)

PURE bool test_is_unary(string_address word)
{
        p8 letter;

        if (!word || string_not(word, '-') || !string_get(word + 1) ||
            string_get(word + 2))
                return false;

        letter = string_get(word + 1);
        if ((p8)(letter - 'a') <= 25)
                return (TEST_UNARY_LOWER & (1u << (letter - 'a'))) != 0;

        return letter == 'G' || letter == 'L' || letter == 'O' || letter == 'S';
}

PURE positive test_is_binary(string_address word)
{
        p8 first;
        p8 second;
        p8 third;

        if (!word)
                return 0;

        first = string_get(word);
        second = string_get(word + 1);
        third = string_get(word + 2);

        /* Every binary operator is one or three bytes. A loop condition is
           -lt/-le/-gt/-ge/-eq/-ne, so the three-byte dash operators are
           first: walking =, != and == ahead of them made every `while [ "$i"
           -lt N ]` pay three misses before the pair that actually matched. */
        if (first == '-' && second && third && !string_get(word + 3))
        {
                p16 pair = ((p16)second << 8) | third;

                if (pair == ((p16)'e' << 8 | 'q'))
                        return TEST_EQUAL;
                if (pair == ((p16)'n' << 8 | 'e'))
                        return TEST_UNEQUAL;
                if (pair == ((p16)'l' << 8 | 't'))
                        return TEST_LESS;
                if (pair == ((p16)'l' << 8 | 'e'))
                        return TEST_LESS_EQUAL;
                if (pair == ((p16)'g' << 8 | 't'))
                        return TEST_GREATER;
                if (pair == ((p16)'g' << 8 | 'e'))
                        return TEST_GREATER_EQUAL;
                if (pair == ((p16)'n' << 8 | 't'))
                        return TEST_NEWER;
                if (pair == ((p16)'o' << 8 | 't'))
                        return TEST_OLDER;
                if (pair == ((p16)'e' << 8 | 'f'))
                        return TEST_SAME_FILE;
        }

        if (!second)
        {
                if (first == '=')
                        return TEST_SAME;
                if (first == '<')
                        return TEST_BEFORE;
                if (first == '>')
                        return TEST_AFTER;

                return 0;
        }

        if (!third)
        {
                if (first == '!' && second == '=')
                        return TEST_DIFFERENT;

                // Bash's second spelling of =, and the one scripts reach for
                // because [[ ]] wants it. POSIX has only the single one.
                if (first == '=' && second == '=')
                        return TEST_SAME;
        }

        return 0;
}

/*
        The six numeric comparisons, asked in one place.

        Two callers arrive here with a pair of numbers they found different
        ways: test reads two words as signed decimals, and [[ ]] evaluates two
        arithmetic expressions. What they then want of the pair is the same six
        questions, and those six were written out in both -- six chances for
        one list to answer a shade differently from the other, in the one kind
        of code where nobody would think to look.
*/
CONST bool test_ordered(positive kind, bipolar first, bipolar second)
{
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

/*
        POSIX test integers, not $(( )).

        A loop writes `[ "$i" -lt 500000 ]` and after expansion that is two
        digit strings. arith_evaluate would treat a leading zero as octal and
        refuse 08; test(1) is a signed decimal word, so 08 is eight. Leading
        and trailing blanks are the strtol grammar bash and dash use; anything
        else after the digits is not a number (status 2, not a false). A value
        that will not fit in a signed machine word is the same error: bash and
        dash refuse it, they do not wrap.
*/
static inline INLINE HOT bool test_digits_compare(string_address left,
                                                  string_address right,
                                                  bipolar address_to cmp)
{
        string_address a;
        string_address b;
        string_address as;
        string_address bs;
        p8 ca;
        p8 cb;
        positive la;
        positive lb;

        if (!left || !right)
                return false;

        ca = string_get(left);
        cb = string_get(right);
        if ((p8)(ca - '0') > 9 || (p8)(cb - '0') > 9)
                return false;

        /* A loop counter and `[ 1 -eq 1 ]` are one or two bytes. Comparing
           those as numbers through multiply is more work than the bytes. */
        if (!string_get(left + 1) && !string_get(right + 1))
        {
                address_to cmp = ca < cb ? -1 : (ca > cb ? 1 : 0);
                return true;
        }

        a = left;
        b = right;
        while (ca == '0' && (p8)(string_get(a + 1) - '0') <= 9)
        {
                a++;
                ca = string_get(a);
        }
        while (cb == '0' && (p8)(string_get(b + 1) - '0') <= 9)
        {
                b++;
                cb = string_get(b);
        }

        as = a;
        do
                a++;
        while ((p8)(string_get(a) - '0') <= 9);
        if (string_get(a))
                return false;

        bs = b;
        do
                b++;
        while ((p8)(string_get(b) - '0') <= 9);
        if (string_get(b))
                return false;

        la = (positive)(a - as);
        lb = (positive)(b - bs);
        if (la > 18 || lb > 18)
                return false;

        if (la != lb)
        {
                address_to cmp = la < lb ? -1 : 1;
                return true;
        }

        while (as != a)
        {
                ca = string_get(as);
                cb = string_get(bs);
                if (ca != cb)
                {
                        address_to cmp = ca < cb ? -1 : 1;
                        return true;
                }
                as++;
                bs++;
        }

        address_to cmp = 0;
        return true;
}

static HOT bool test_integer(string_address word, bipolar address_to out)
{
        p8 byte;
        bool negative;
        positive magnitude;
        positive bound;
        p8 digit;

        if (!word)
                return false;

        byte = string_get(word);
        while (byte == ' ' || byte == '\t')
        {
                word++;
                byte = string_get(word);
        }

        negative = false;
        if (byte == '-' || byte == '+')
        {
                negative = byte == '-';
                word++;
                byte = string_get(word);
        }

        if ((p8)(byte - '0') > 9)
                return false;

        while (byte == '0' && (p8)(string_get(word + 1) - '0') <= 9)
        {
                word++;
                byte = string_get(word);
        }

        magnitude = 0;
        bound = negative ? (positive)bipolar_max + 1 : (positive)bipolar_max;
        do
        {
                digit = (p8)(byte - '0');
                if (magnitude > bound / 10 ||
                    (magnitude == bound / 10 && (positive)digit > bound % 10))
                        return false;

                magnitude = magnitude * 10 + (positive)digit;
                word++;
                byte = string_get(word);
        } while ((p8)(byte - '0') <= 9);

        while (byte == ' ' || byte == '\t')
        {
                word++;
                byte = string_get(word);
        }

        if (byte)
                return false;

        address_to out = negative ? (bipolar)(0 - magnitude) : (bipolar)magnitude;
        return true;
}

static inline INLINE HOT bool test_integer_pair(string_address left, string_address op,
                                  string_address right, b32 address_to status)
{
        positive kind;
        bipolar cmp;
        bipolar first;
        bipolar second;

        kind = test_is_binary(op);
        if (kind < TEST_EQUAL || kind > TEST_GREATER_EQUAL)
                return false;

        if (test_digits_compare(left, right, address_of cmp))
        {
                address_to status = test_ordered(kind, cmp, 0) ? 0 : 1;
                return true;
        }

        if (!test_integer(left, address_of first))
        {
                shell_test_number_refused(left);
                address_to status = 2;
                return true;
        }

        if (!test_integer(right, address_of second))
        {
                shell_test_number_refused(right);
                address_to status = 2;
                return true;
        }

        address_to status = test_ordered(kind, first, second) ? 0 : 1;
        return true;
}

bool test_compare(positive kind, string_address left, string_address right)
{
        bipolar first;
        bipolar second;

        if (kind <= TEST_DIFFERENT)
        {
                if (kind == TEST_SAME)
                        return !string_compare(left, right);

                return string_compare(left, right) != 0;
        }

        if (kind <= TEST_GREATER_EQUAL)
        {
                bipolar cmp;

                if (test_digits_compare(left, right, address_of cmp))
                        return test_ordered(kind, cmp, 0);

                if (!test_integer(left, address_of first))
                {
                        shell_test_number_refused(left);
                        test_bad = true;
                        return false;
                }

                if (!test_integer(right, address_of second))
                {
                        shell_test_number_refused(right);
                        test_bad = true;
                        return false;
                }

                return test_ordered(kind, first, second);
        }

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
                               file_device_key(one.device_major, one.device_minor) ==
                               file_device_key(two.device_major, two.device_minor);

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

        return string_compare(left, right) > 0;
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

#define TEST_LOGICAL_LEVEL(name, lower, spelling, operation)                 \
        bool name()                                                         \
        {                                                                    \
                bool value = lower();                                       \
                                                                             \
                /* Parse the right side even when truth is already known:   \
                   the cursor must leave the complete expression. */        \
                while (test_at < test_stop &&                               \
                       word_is(shell_argv[test_at], (spelling)))             \
                {                                                            \
                        test_at++;                                           \
                        bool other = lower();                                \
                        value = value operation other;                       \
                }                                                            \
                                                                             \
                return value;                                                \
        }

TEST_LOGICAL_LEVEL(test_conjunction, test_negation, "-a", &&)
TEST_LOGICAL_LEVEL(test_expression, test_conjunction, "-o", ||)
#undef TEST_LOGICAL_LEVEL

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
HOT bool test_short(positive from, positive to, bool address_to handled)
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
        }

        if (count == 3 || count == 4)
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

FLAT HOT fn shell_test(writer write, string_address input)
{
        bool value;
        bool handled;
        string_address name;
        string_address last;
        positive count;
        positive argc;
        b32 status;

        argc = shell_argc;
        name = shell_argv[0];

        /* `[ a -lt b ]` and `[ -n x ]` after expansion. Decide from argc
           before touching the expression cursor: a loop condition is five
           argv words, a unary two-word form is four, and neither needs the
           general walker. */
        if (name && string_is(name, '[') && !string_get(name + 1))
        {
                if (argc == 5)
                {
                        last = shell_argv[4];
                        if (last && string_is(last, ']') && !string_get(last + 1) &&
                            test_integer_pair(shell_argv[1], shell_argv[2],
                                              shell_argv[3], address_of status))
                                return shell_answer(status);
                }
                else if (argc == 4)
                {
                        string_address op = shell_argv[1];
                        string_address operand = shell_argv[2];

                        last = shell_argv[3];
                        if (op && string_is(op, '-') && string_get(op + 1) &&
                            !string_get(op + 2) && last && string_is(last, ']') &&
                            !string_get(last + 1))
                        {
                                p8 letter = string_get(op + 1);

                                if (letter == 'n')
                                {
                                        shell_status =
                                            operand && string_not(operand, end)
                                                ? 0
                                                : 1;
                                        return;
                                }

                                if (letter == 'z')
                                {
                                        shell_status =
                                            !operand || string_is(operand, end)
                                                ? 0
                                                : 1;
                                        return;
                                }
                        }

                        if (last && string_is(last, ']') && !string_get(last + 1) &&
                            op && string_is(op, '!') && !string_get(op + 1))
                        {
                                shell_status =
                                    string_get(operand) == end ? 0 : 1;
                                return;
                        }
                }
        }
        else if (argc == 4)
        {
                if (test_integer_pair(shell_argv[1], shell_argv[2],
                                      shell_argv[3], address_of status))
                        return shell_answer(status);
        }
        else if (argc == 3)
        {
                string_address op = shell_argv[1];
                string_address operand = shell_argv[2];

                if (op && string_is(op, '-') && string_get(op + 1) &&
                    !string_get(op + 2))
                {
                        p8 letter = string_get(op + 1);

                        if (letter == 'n')
                                return shell_answer(
                                    operand && string_not(operand, end) ? 0
                                                                        : 1);

                        if (letter == 'z')
                                return shell_answer(
                                    !operand || string_is(operand, end) ? 0
                                                                        : 1);
                }
        }

        test_at = 1;
        test_stop = argc;
        test_bad = false;
        test_said = false;

        //      Both references say why they would not read the words, and
        //      a script that only looks at the status still cares that the
        //      channel spoke: silence here was the one place test differed
        //      from every shell it is compared against.
        if (name && string_is(name, '[') && !string_get(name + 1))
        {
                if (argc < 2)
                        return shell_answer(string_report(
                            log_error, 2, "%s: missing `]'\n", name));

                last = shell_argv[argc - 1];
                if (!last || string_not(last, ']') || string_get(last + 1))
                        return shell_answer(string_report(
                            log_error, 2, "%s: missing `]'\n", name));

                test_stop = argc - 1;
        }

        if (test_at >= test_stop)
                return shell_answer(1);

        count = test_stop - test_at;

        if (count == 1)
                return shell_answer(string_get(shell_argv[test_at]) != end ? 0
                                                                          : 1);

        if (count <= 4)
        {
                value = test_short(test_at, test_stop, address_of handled);

                if (handled)
                {
                        if (!test_bad)
                                return shell_answer(value ? 0 : 1);

                        if (test_said)
                                return shell_answer(2);

                        shell_diagnostic_where();

                        return shell_answer(string_report(
                            log_error, 2, "%s: %s: unexpected operator\n",
                            shell_argv[0], shell_argv[test_at]));
                }
        }

        value = test_expression();

        if (test_bad || test_at != test_stop)
        {
                if (test_said)
                        return shell_answer(2);

                shell_diagnostic_where();

                return shell_answer(string_report(
                    log_error, 2,
                    test_bad ? "%s: %s: unexpected operator\n"
                             : "%s: too many arguments\n",
                    shell_argv[0],
                    test_at < test_stop ? shell_argv[test_at]
                                        : shell_argv[shell_argc - 1]));
        }

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
static b32 printf_status;

static fn printf_format_failed()
{
        printf_status = shell_bash_compat ? 1 : 2;
        printf_cut = true;
}

static byte_store printf_hold;

// What printf writes without looking at it: everything but the terminator and
// the one or two bytes that mean something where it is being read.
static b8 printf_plain[STRING_SET_BYTES];
static b32 printf_sets_ready;

static fn printf_sets_prepare()
{
        if (printf_sets_ready)
                return;

        memory_fill(printf_plain + 1, 1, STRING_SET_BYTES - 1);

        printf_plain['\\'] = printf_plain['%'] = 0;
        printf_sets_ready = true;
}

/*
        Where printf -v gathers what it would otherwise have written.

        Its own store, because %b gathers into printf_hold below and the two
        would otherwise be the same bytes: a width on a %b rewinds that store
        to nothing, which would throw away everything the format had already
        produced.
*/
static byte_store printf_kept;

static inline INLINE fn printf_collect(byte_store address_to store,
                                       address_any data, positive length)
{
        if (length > positive_max - store->used ||
            !shell_array_room(store->bytes, store->room, store->used + length))
        {
                if (!printf_cut)
                        log_error("printf: no room\n", 0);

                printf_status = 2;
                printf_cut = true;
                return;
        }

        memory_copy_apart(store->bytes + store->used, data, length);
        store->used += length;
}

static fn printf_keeper(address_any data, positive length)
{
        printf_collect(address_of printf_kept, data, length);
}

static fn printf_holder(address_any data, positive length)
{
        printf_collect(address_of printf_hold, data, length);
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

/*
        One backslash escape, already past the backslash. Answers where to
        carry on reading; an octal run is up to three digits, and \0 in front
        of it is what POSIX writes even though every shell also takes it bare.
*/
RETURNS_NONNULL string_address printf_escape(writer write, string_address step)
{
        p8 value;

        if (string_is(step, '0') || (string_get(step) >= '1' && string_get(step) <= '7'))
        {
                // \0ddd is the %b argument's spelling; in the format itself
                // the zero is the first of up to three digits, so \0101 is
                // a backspace and a one there and an A in an argument.
                if (printf_in_b && string_is(step, '0'))
                        step++;

                positive used;
                positive number = string_digits_octal_escape_max(
                    step, 3, address_of used);

                step += used;

                value = (p8)number;
                write(address_of value, 1);

                return step;
        }

        // \xHH is one or two hexadecimal digits, in the format, a %b argument
        // and echo -e alike -- both references read it in all three. A bare
        // \x with no digit falls through and stays the two bytes it was.
        if (string_is(step, 'x'))
        {
                positive used;
                positive number = string_digits_hexadecimal_escape_max(
                    step + 1, 2, address_of used);

                if (used)
                {
                        step += used + 1;
                        value = (p8)number;
                        write(address_of value, 1);

                        return step;
                }
        }

        //      \uHHHH and \UHHHHHHHH, in the format, a %b argument and
        //      echo -e alike, the way $'...' reads them. Without digits the
        //      two bytes stand for themselves, as a bare \x does above.
        if (string_is(step, 'u') || string_is(step, 'U'))
        {
                p8 letter = string_get(step);
                positive wide = letter == 'u' ? 4 : 8;
                positive used;
                positive code = string_digits_hexadecimal_escape_max(
                    step + 1, wide, address_of used);

                if (used)
                {
                        p8 bytes[CODE_POINT_MAX_BYTES];
                        positive count =
                            shell_code_point_bytes(letter, wide, code, bytes);

                        step += used + 1;

                        if (code)
                                write(bytes, count);

                        return step;
                }
        }

        value = string_get(step);

        // Only in a %b argument. In the format itself the reference shell
        // leaves the two bytes where they stood.
        if (value == 'c' && printf_in_b)
        {
                printf_cut = true;
                return step;
        }

        p8 escaped = byte_simple_escape(value);

        if (value == 'e')
                value = 27;
        else if (escaped)
                value = escaped;
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

/*
        A value written so the shell could read it back: printf %q.

        Three shapes, and which one is used is decided by the bytes: nothing
        at all is a pair of quotes, a value with a byte no terminal would show
        is $'...' with the byte spelled out, and everything else has a
        backslash put in front of each byte that would otherwise mean
        something. That is Bash's rule and not a simplification of it, because
        %q exists to be pasted back into a command line.
*/
static COLD PURE bool printf_quote_wanted(p8 value)
{
        static const p8 wanted[] = " \t\n'\"\\|&;()<>!{}*[?]^$`,";

        return string_first_of((string_address)wanted, value) != null;
}

COLD fn printf_reusable(writer write, string_address text)
{
        string_address step = text;
        bool control = string_get(text + string_span(text, shell_quote_value));

        if (!string_get(text))
                return write("''", 2);

        if (control)
        {
                write("$'", 2);

                while (string_get(step))
                {
                        if (shell_quote_ansi[string_get(step)])
                        {
                                positive run = string_span(step, shell_quote_ansi);
                                write(step, run);
                                step += run;
                                continue;
                        }
                        p8 escaped[4];
                        write(escaped,
                              shell_ansi_byte(escaped, string_get(step++), false));
                }

                return write("'", 1);
        }

        for (step = text; string_get(step); step++)
        {
                p8 value = string_get(step);

                // A comment only begins a comment where the word does.
                if (printf_quote_wanted(value) ||
                    (value == '#' && step == text) ||
                    (value == '~' && step == text))
                        write("\\", 1);

                write(address_of value, 1);
        }
}

fn printf_escaped(writer write, string_address text)
{
        while (string_get(text) && !printf_cut)
        {
                positive run = (positive)(string_first_of_or_end(text, '\\') - text);

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
                 positive width, bipolar precision, bool left, bool zero,
                 bool alternate)
{
        positive style = sign | ((positive)upper << 26) |
                         ((positive)left << 27) | ((positive)zero << 28);

        /*
                An explicitly empty precision suppresses the zero digit.  A
                sign still belongs to the field, while zero padding is
                ignored once a precision was supplied.  Octal's alternate
                form is the sole exception: it must retain one zero digit.

                Keep this printf-only policy outside positive_to_base_field:
                its other callers rely on zero always having a digit.
        */
        if (!magnitude && !precision)
        {
                if (alternate && base == 8)
                        precision = 1;
                else
                {
                        writer_field(write, address_of sign, sign ? 1 : 0,
                                     width, ' ', left);
                        return;
                }
        }

        // The alternate form is a prefix, and only on a value that has
        // digits to put it in front of: 0x before sixteen, 0 before eight.
        if (alternate && magnitude)
        {
                if (base == 16)
                        style |= ((positive)'0' << 8) |
                                 ((positive)(upper ? 'X' : 'x') << 16) |
                                 ((positive)2 << 24);
                else if (base == 8)
                        style |= ((positive)'0' << 8) | ((positive)1 << 24);
        }

        positive_to_base_field(write, magnitude, base, width, precision, style);
}

// An argument that is not a number is still printed, as zero, and the status
// says so afterwards; that is what the reference shell does.
/*
        The number an integer conversion reads out of its argument.

        strtoimax's grammar, which is what the reference shell reads with:
        blanks in front, a sign, 0x for sixteen and a leading 0 for eight --
        and before any of that, a quote, which means the byte after it. An
        missing argument is zero and no complaint. Bash diagnoses an explicitly
        empty operand; dash accepts that operand but diagnoses blanks alone.
        What the digits
        leave behind is a complaint and not a refusal: the number read is
        printed, and "not completely converted" is said afterwards with the
        status, the same as no digits at all is zero and "expected numeric
        value". A conversion that refused every one of those printed nothing
        for "$maybe_empty" and set the status where every other shell did not.
*/
static positive printf_integer(string_address word, bool signed_value)
{
        string_address at = word;
        string_address stopped;
        positive value;
        b32 out_of_range;

        if (!word)
                return 0;

        at += string_span(at, string_set_blanks);

        if (!string_get(at))
        {
                if (word != printf_nothing &&
                    (shell_bash_compat || string_get(word)))
                        printf_status = shell_printf_number_refused(word);
                return 0;
        }

        if (string_is(at, '\'') || string_is(at, '"'))
                return (positive)string_get(at + 1);

        /* The standard layer's checked strtoimax/strtoumax cores already
           carry this exact prefix/sign grammar in one assembly scan. Keep
           printf on that path too: the old digit-count readers wrapped a
           long operand and lost the one bit that says to clamp and report. */
        value = signed_value
                    ? (positive)string_to_number_checked(
                          at, address_of stopped, 0, address_of out_of_range)
                    : string_to_number_unsigned_checked(
                          at, address_of stopped, 0, address_of out_of_range);

        if (stopped == at)
        {
                printf_status = shell_printf_number_refused(word);
                return 0;
        }

        if (out_of_range)
        {
                string_format(log_error,
                              "printf: %s: numerical result out of range\n",
                              word);
                printf_status = 1;
        }
        else if (string_get(stopped))
        {
                //      A number with a tail on it is no number at all to
                //      bash, which says so in the same words it uses for a
                //      word that was never one.
                if (shell_bash_compat)
                        shell_printf_number_refused(word);
                else
                {
                        shell_diagnostic_where();
                        string_format(log_error,
                            "printf: %s: not completely converted\n", word);
                }

                printf_status = 1;
        }

        return value;
}

/*
        The number a floating conversion reads out of its argument.

        strtod's grammar rather than strtoimax's, which is the whole of the
        difference from the reader above: the reference shell reads %f, %e,
        %g and %a with it, so "0x10" is sixteen, "1e3" is a thousand, and
        "inf" and "nan" are themselves. The quote that means the byte after
        it is read here too, because a format is free to spell the same
        argument either way. Empty or missing is zero and no complaint;
        digits that were never there, and digits with something left after
        them, complain in the two spellings the integer reader uses.
*/
static decimal printf_decimal(string_address word)
{
        string_address at = word;
        string_address stopped = null;
        decimal value;

        if (!word)
                return 0.0;

        at += string_span(at, string_set_blanks);

        if (!string_get(at))
        {
                if (word != printf_nothing &&
                    (shell_bash_compat || string_get(word)))
                        printf_status = shell_printf_number_refused(word);
                return 0.0;
        }

        if (string_is(at, '\'') || string_is(at, '"'))
                return (decimal)(positive)string_get(at + 1);

        value = string_to_decimal(at, address_of stopped);

        if (stopped == at)
        {
                printf_status = shell_printf_number_refused(word);
                return 0.0;
        }

        if (string_get(stopped))
        {
                //      A number with a tail on it is no number at all to
                //      bash, which says so in the same words it uses for a
                //      word that was never one.
                if (shell_bash_compat)
                        shell_printf_number_refused(word);
                else
                {
                        shell_diagnostic_where();
                        string_format(log_error,
                            "printf: %s: not completely converted\n", word);
                }

                printf_status = 1;
        }

        return value;
}

fn printf_one(writer write, string_address format)
{
        string_address step = format;

        printf_sets_prepare();

        while (string_get(step) && !printf_cut)
        {
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

                conversion_spec parsed = conversion_spec_take_max(&step, positive_max);
                //      An overflowed field is not a width; see awk_sprintf.
                positive width = (parsed.overflow & 1) ? 0 : parsed.field[0];
                bipolar precision = parsed.fields == 2 && !(parsed.overflow & 2)
                    ? (bipolar)parsed.field[1] : -1;
                for (p8 field = 0; field < parsed.fields; field++)
                        if (parsed.stars & (1u << field))
                        {
                                bipolar value = (bipolar)printf_integer(printf_next(), true);
                                if (field)
                                        precision = value < 0 ? -1 : value;
                                else
                                {
                                        parsed.flags |= value < 0 ? CONVERSION_FLAG_LEFT : 0;
                                        width = value < 0 ? (positive)0 - (positive)value : (positive)value;
                                }
                        }
                bool left = (parsed.flags & CONVERSION_FLAG_LEFT) != 0;
                bool zero = (parsed.flags & CONVERSION_FLAG_ZERO) != 0;
                bool plus = (parsed.flags & CONVERSION_FLAG_PLUS) != 0;
                bool space = (parsed.flags & CONVERSION_FLAG_SPACE) != 0;
                bool alternate = (parsed.flags & CONVERSION_FLAG_ALTERNATE) != 0;

                // The length modifiers say nothing here: every number this
                // reads is already as wide as the machine.
                step += string_span_of_set(step, "lhzj");

                conversion = string_get(step);

                if (!conversion)
                {
                        log_error("printf: missing format character\n", 0);
                        printf_format_failed();
                        break;
                }

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
                                printf_hold.used = 0;
                                printf_in_b = true;
                                printf_escaped(printf_holder, value);
                                printf_in_b = false;

                                // \c ends the output, but what stood in front
                                // of it is still written, in its field; the
                                // loop stops on the flag afterwards.
                                value = printf_hold.bytes;
                                length = printf_hold.used;
                        }
                        else
                        {
                                length = string_length(env_reading(value));
                        }

                        if (precision >= 0 && (positive)precision < length)
                                length = (positive)precision;

                        writer_field(write, value, length, width, ' ', left);

                        continue;
                }

                if (conversion == 'c')
                {
                        string_address value = printf_next();

                        // Empty and missing operands still write a NUL byte.
                        // printf_next supplies a real one-byte zero object for
                        // the latter, so the same field writer handles both.
                        writer_field(write, value, 1, width, ' ', left);

                        continue;
                }

                if (conversion == 'q')
                {
                        printf_reusable(write, printf_next());

                        continue;
                }

                /*
                        %(format)T: a moment written the way date writes it.

                        The parentheses are read here rather than by the flag
                        walk above because what is inside them is a format of
                        its own and not a width. -1 is now and -2 is when this
                        shell started, which is what Bash answers and what a
                        prompt timing itself is asking for.
                */
                if (conversion == '(')
                {
                        p8 shape[256];
                        positive kept = 0;
                        bipolar when;

                        while (string_get(step) && string_not(step, ')') &&
                               kept + 1 < sizeof(shape))
                                shape[kept++] = string_get(step++);

                        shape[kept] = end;

                        if (string_is(step, ')'))
                                step++;

                        // The T is what makes it a time; anything else there
                        // is a directive nobody has.
                        if (string_get(step) != 'T')
                        {
                                string_format(log_error,
                                              "printf: %%(%s: invalid directive\n",
                                              shape);
                                printf_format_failed();
                                continue;
                        }

                        step++;
                        when = printf_argument < shell_argc
                                 ? (bipolar)printf_integer(printf_next(), true)
                                 : -1;

                        if (when == -1)
                                when = (bipolar)shell_clock_seconds(
                                    SHELL_CLOCK_REALTIME, null);
                        else if (when == -2)
                        {
                                shell_seconds_begin();
                                when = (bipolar)shell_started_seconds;
                        }

                        date_shape(write, (b64)when, shape);

                        continue;
                }

                if (conversion == 'd' || conversion == 'i')
                {
                        bipolar value =
                            (bipolar)printf_integer(printf_next(), true);
                        p8 sign = 0;

                        if (value < 0)
                                sign = '-';
                        else if (plus)
                                sign = '+';
                        else if (space)
                                sign = ' ';

                        positive magnitude = (positive)value;

                        if (value < 0)
                                magnitude = (positive)0 - magnitude;

                        printf_number(write, magnitude, sign, 10, false, width,
                                      precision, left, zero, false);
                        continue;
                }

                if (conversion == 'u' || conversion == 'o' ||
                    conversion == 'x' || conversion == 'X')
                {
                        positive value = printf_integer(printf_next(), false);
                        positive base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);

                        printf_number(write, value, 0, base, conversion == 'X',
                                      width, precision, left, zero, alternate);
                        continue;
                }

                if (conversion == 'f' || conversion == 'F' ||
                    conversion == 'e' || conversion == 'E' ||
                    conversion == 'g' || conversion == 'G' ||
                    conversion == 'a' || conversion == 'A')
                {
                        /*
                                Through the standard layer's own field
                                writer, which awk already prints its numbers
                                with: the exact digits, the rounding and the
                                padding are one implementation and not two.

                                A sink pointed at the writer rather than at a
                                buffer, so a width or a precision the caller
                                asked for has no size to overrun -- "%.2000f"
                                is a legal thing to write and a buffer here
                                would have to guess how much of it to keep.
                        */
                        format_sink sink = {0};
                        format_spec spec = {0};

                        sink.downstream = write;

                        spec.flags = parsed.flags;
                        spec.width = width;
                        spec.precision = precision;
                        spec.conversion = conversion;

                        decimal value = printf_decimal(printf_next());

                        if (conversion == 'a' || conversion == 'A')
                                format_hex_field(address_of sink, value,
                                                 address_of spec);
                        else
                                format_decimal_field(address_of sink, value,
                                                     address_of spec);
                        continue;
                }

                {
                        p8 said[3] = {'%', conversion, end};

                        string_format(log_error,
                                      "printf: %s: invalid directive\n", said);
                }

                printf_format_failed();
        }
}

/*
        The common printf: literal text, %% , %s, %d/%i, and the one-byte
        backslash escapes. Widths, %b, %q, %n and the rest keep the full
        walker, including dash's missing %q (we still have it). `%s %s\n`
        and `%s\n` are the script-shaped cases and do not walk the format.
*/
static inline INLINE fn printf_join_two_strings(writer write,
                                                string_address a,
                                                string_address b, p8 between,
                                                bool newline)
{
        positive la = string_length(env_reading(a));
        positive lb = string_length(env_reading(b));
        positive total = la + lb + (between ? 1 : 0) + (newline ? 1 : 0);
        p8 address_to dst;
        positive have;

        if (!total)
                return;

        if (write == log && shell_log_room(total))
        {
                have = log_writer_buffer_length;
                dst = log_writer_buffer + have;
                memory_copy_apart(dst, a, la);
                dst += la;
                if (between)
                        *dst++ = between;
                memory_copy_apart(dst, b, lb);
                dst += lb;
                if (newline)
                        *dst = '\n';
                log_writer_buffer_length = have + total;
                return;
        }

        if (total <= SHELL_OUTPUT_JOIN)
        {
                p8 room[SHELL_OUTPUT_JOIN];

                memory_copy_apart(room, a, la);
                dst = room + la;
                if (between)
                        *dst++ = between;
                memory_copy_apart(dst, b, lb);
                if (newline)
                        room[total - 1] = '\n';
                write(room, total);
                return;
        }

        if (la)
                write(a, la);
        if (between)
                write(address_of between, 1);
        if (lb)
                write(b, lb);
        if (newline)
                write("\n", 1);
}

static inline INLINE fn printf_join_string_nl(writer write, string_address a)
{
        positive la = string_length(env_reading(a));
        positive total = la + 1;
        p8 address_to dst;
        positive have;

        if (write == log && shell_log_room(total))
        {
                have = log_writer_buffer_length;
                dst = log_writer_buffer + have;
                memory_copy_apart(dst, a, la);
                dst[la] = '\n';
                log_writer_buffer_length = have + total;
                return;
        }

        if (total <= SHELL_OUTPUT_JOIN)
        {
                p8 room[SHELL_OUTPUT_JOIN];

                memory_copy_apart(room, a, la);
                room[la] = '\n';
                write(room, total);
                return;
        }

        if (la)
                write(a, la);
        write("\n", 1);
}

static bool printf_format_simple(string_address format)
{
        string_address step = format;

        while (string_get(step))
        {
                p8 byte = string_get(step);

                if (byte != '%' && byte != '\\')
                {
                        step++;
                        continue;
                }

                if (byte == '\\')
                {
                        p8 next = string_get(step + 1);

                        if (!next)
                                return false;

                        if ((next >= '0' && next <= '7') || next == 'x' ||
                            next == 'u' || next == 'U')
                                return false;

                        step += 2;
                        continue;
                }

                step++;
                byte = string_get(step);

                if (byte == '%')
                {
                        step++;
                        continue;
                }

                if (byte == 's' || byte == 'd' || byte == 'i')
                {
                        step++;
                        continue;
                }

                return false;
        }

        return true;
}

static fn printf_simple_one(writer write, string_address format)
{
        p8 room[SHELL_OUTPUT_JOIN];
        positive used = 0;
        string_address step = format;

        while (string_get(step))
        {
                p8 byte = string_get(step);

                if (byte != '%' && byte != '\\')
                {
                        string_address start = step;

                        do
                                step++;
                        while (string_get(step) && string_not(step, '%') &&
                               string_not(step, '\\'));

                        shell_output_put(write, room, address_of used, start,
                                         (positive)(step - start));
                        continue;
                }

                if (byte == '\\')
                {
                        p8 next = string_get(step + 1);
                        p8 escaped = byte_simple_escape(next);

                        step += 2;

                        if (next == 'e')
                                escaped = 27;
                        else if (next == '\\')
                                escaped = '\\';
                        else if (!escaped)
                        {
                                shell_output_byte(write, room, address_of used,
                                                  '\\');
                                escaped = next;
                        }

                        shell_output_byte(write, room, address_of used,
                                          escaped);
                        continue;
                }

                step++;
                byte = string_get(step);
                step++;

                if (byte == '%')
                {
                        shell_output_byte(write, room, address_of used, '%');
                        continue;
                }

                if (byte == 's')
                {
                        string_address value = printf_next();

                        shell_output_put(write, room, address_of used, value,
                                         string_length(env_reading(value)));
                        continue;
                }

                {
                        bipolar number =
                            (bipolar)printf_integer(printf_next(), true);
                        p8 digits[32];
                        positive length = bipolar_into(digits, number);

                        shell_output_put(write, room, address_of used, digits,
                                         length);
                }
        }

        if (used)
                write(room, used);
}

fn shell_printf(writer write, string_address input)
{
        string_address format;
        string_address into = null;
        positive first = 1;

        /* `--` is needed for formats beginning with a dash. Bash also takes
           -vname as well as -v name; both reach the same existing keeper and
           assignment path, rather than growing another formatter. */
        while (first < shell_argc && string_is(shell_argv[first], '-') &&
               string_not(shell_argv[first] + 1, end))
        {
                string_address option = shell_argv[first];

                if (word_is(option, "--"))
                {
                        first++;
                        break;
                }

                if (string_is(option + 1, 'v'))
                {
                        if (string_get(option + 2))
                                into = option + 2;
                        else if (++first < shell_argc)
                                into = shell_argv[first];
                        else
                                return shell_answer(string_report(log_error, 2, "printf: -v: option requires an "
                                                 "argument\n"));

                        first++;
                        continue;
                }

                return shell_answer(shell_option_refused(
                    "printf", option, "printf [-v var] format [arguments]"));
        }

        if (shell_argc <= first)
                return shell_answer(2);

        format = shell_argv[first];
        printf_argument = first + 1;
        printf_cut = false;
        printf_status = 0;

        if (into)
        {
                printf_kept.used = 0;
                write = printf_keeper;
        }

        {
                p8 kind;

                if (word_is(format, "%s %s\n"))
                        kind = 1;
                else if (word_is(format, "%s\n"))
                        kind = 2;
                else if (printf_format_simple(format))
                        kind = 3;
                else
                        kind = 0;

                while (1)
                {
                        printf_took = false;
                        if (kind == 1)
                                printf_join_two_strings(write, printf_next(),
                                                        printf_next(), ' ',
                                                        true);
                        else if (kind == 2)
                                printf_join_string_nl(write, printf_next());
                        else if (kind)
                                printf_simple_one(write, format);
                        else
                                printf_one(write, format);

                        // A format with no conversion in it would otherwise run for as
                        // long as there were arguments left.
                        if (printf_cut || printf_argument >= shell_argc || !printf_took)
                                break;
                }
        }

        if (into)
        {
                if (!shell_array_room(printf_kept.bytes, printf_kept.room, printf_kept.used + 1))
                {
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "printf"));
                }

                printf_kept.bytes[printf_kept.used] = end;

                if (!env_assign(into, printf_kept.bytes))
                        return shell_answer(string_report(log_error, 1, "printf: %s: cannot assign\n", into));
        }

        shell_answer(printf_status);
}

/*
        read.

        A byte at a time, because the line arrives on the same descriptor the
        shell is reading its script from and anything larger swallows what
        comes after.
*/

static p8 address_to read_line;
static positive read_line_room;
static p8 address_to read_literal;
static positive read_literal_room;
static p8 address_to read_ifs;
static positive read_ifs_room;
static positive read_length;

// The bytes and their quotedness move together, but no pointer into either is
// retained while they grow. Keep both mappings for the next call: a shell that
// reads a long-lived stream reaches a steady state instead of allocating once
// per line.
#define read_reserve(want)                                                   \
        shell_byte_pair_room(read_line, read_line_room, read_literal,        \
                             read_literal_room, (want))

// Whether a byte splits a field. The two questions are different: every byte
// in IFS ends a field, but only the blanks among them are allowed to run
// together and to be thrown away at the ends.
PURE bool read_separates(string_address ifs, positive at)
{
        if (read_literal[at])
                return false;

        return string_first_of(ifs, read_line[at]) != null;
}

PURE bool read_blank(string_address ifs, positive at)
{
        if (read_literal[at])
                return false;

        if (read_line[at] != ' ' && read_line[at] != '\t' && read_line[at] != '\n')
                return false;

        return string_first_of(ifs, read_line[at]) != null;
}

/*
        The moment read -t gives up, and whether a byte arrived before it.

        The timeout is on the whole line, not on each byte of it: measured
        again from every byte, a writer sending one byte every N seconds
        kept a read -t N waiting forever. So the deadline is fixed once, on
        the monotonic clock, and every wait is for what is left of it.
*/
#define READ_CLOCK_MONOTONIC 1
#define READ_TIMEOUT_STATUS 142

/* Bash's timeout operand is a fixed decimal, kept to the microseconds its
   interface observes. Empty, a bare sign and a bare point are its spellings
   of zero; an exponent or any other suffix is not silently accepted. */
static bool read_timeout(string_address text, timespec address_to span)
{
        string_address at = text;
        positive seconds = 0;
        positive fraction = 0;
        positive scale = 100000;
        bool negative = false;

        if (string_is(at, '-') || string_is(at, '+'))
        {
                negative = string_is(at, '-');
                at++;
        }

        if (byte_is_digit(string_get(at)) &&
            (!string_digits_checked(address_of at, 10, address_of seconds) ||
             seconds > (positive)b64_max))
                return false;

        if (string_is(at, '.'))
        {
                at++;

                while (byte_is_digit(string_get(at)))
                {
                        positive digit = string_get(at++) - '0';

                        if (scale)
                        {
                                fraction += digit * scale;
                                scale /= 10;
                        }
                }
        }

        if (string_get(at) ||
            (negative && (seconds || fraction)))
                return false;

        span->tv_sec = seconds;
        span->tv_nsec = fraction * 1000;
        return true;
}

static fn read_deadline(timespec span, timespec address_to deadline)
{
        system_call_2(syscall(clock_gettime), READ_CLOCK_MONOTONIC,
                      (positive)deadline);

        if (span.tv_sec > (positive)b64_max - deadline->tv_sec ||
            (span.tv_sec == (positive)b64_max - deadline->tv_sec &&
             span.tv_nsec > 999999999 - deadline->tv_nsec))
        {
                deadline->tv_sec = b64_max;
                deadline->tv_nsec = 999999999;
                return;
        }

        deadline->tv_sec += span.tv_sec;
        deadline->tv_nsec += span.tv_nsec;

        if (deadline->tv_nsec >= 1000000000)
        {
                deadline->tv_sec++;
                deadline->tv_nsec -= 1000000000;
        }
}

static bool read_waited(b32 descriptor, timespec address_to deadline)
{
        timespec now;
        timespec left;

        system_call_2(syscall(clock_gettime), READ_CLOCK_MONOTONIC,
                      (positive)address_of now);

        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
                return false;

        left.tv_sec = deadline->tv_sec - now.tv_sec;

        if (deadline->tv_nsec >= now.tv_nsec)
                left.tv_nsec = deadline->tv_nsec - now.tv_nsec;
        else
        {
                left.tv_sec--;
                left.tv_nsec = deadline->tv_nsec + 1000000000 - now.tv_nsec;
        }

        return descriptor_wait_readable(descriptor, address_of left, null) > 0;
}

static PURE b32 read_result(bool failed, bool ended, bool timed_out)
{
        if (failed)
                return shell_bash_compat ? 1 : 2;
        if (timed_out)
                return shell_bash_compat ? READ_TIMEOUT_STATUS : 1;
        return ended ? 1 : 0;
}

static bool read_nonnegative(string_address text, positive maximum,
                             positive address_to value)
{
        string_address stopped;
        b32 out_of_range;
        bipolar got = string_to_number_checked(
            text, address_of stopped, 10, address_of out_of_range);

        if (out_of_range || stopped == text || string_get(stopped) || got < 0 ||
            (positive)got > maximum)
                return false;

        address_to value = (positive)got;
        return true;
}

/*
        read -s: the bytes arrive and the terminal does not show them.

        Only a terminal has an echo to turn off. On a pipe the first call
        fails and there is nothing to put back, which is why this is a pair of
        calls and a flag rather than a mode the shell has to remember -- and
        why a test that reads down a pipe cannot see whether it works.
*/
static bool read_echo_off(b32 descriptor, terminal_modes address_to held)
{
        terminal_modes quiet;

        if (system_control(descriptor, PTY_TCGETS, held) != 0)
                return false;

        quiet = address_to held;
        quiet.behaviour &= ~EDIT_LOCAL_ECHO;

        return system_control(descriptor, PTY_TCSETS, address_of quiet) == 0;
}

// The fields of one read line, when they are going into an array rather than
// into a name each. The vector is the builtin's own and is reused.
static string_address address_to read_words;
static positive read_words_room;

/* One IFS boundary for read's array and scalar destinations. The final
   scalar keeps the unsplit remainder, except a lone terminal delimiter. */
static string_address read_field(string_address ifs, positive address_to cursor,
                                  bool remainder)
{
        positive at = *cursor;
        while (at < read_length && read_blank(ifs, at))
                at++;
        *cursor = read_length;
        if (at == read_length)
                return null;
        positive begin = at;
        if (remainder)
        {
                positive stop = read_length;
                while (stop > begin && read_blank(ifs, stop - 1))
                        stop--;
                if (stop > begin && read_separates(ifs, stop - 1) &&
                    !read_blank(ifs, stop - 1))
                {
                        positive separator = begin;
                        while (separator + 1 < stop &&
                               (!read_separates(ifs, separator) ||
                                read_blank(ifs, separator)))
                                separator++;
                        if (separator + 1 == stop)
                                stop--;
                }
                read_line[stop] = end;
        }
        else
        {
                while (at < read_length && !read_separates(ifs, at))
                        at++;
                positive after = at;
                while (after < read_length && read_blank(ifs, after))
                        after++;
                if (after < read_length && read_separates(ifs, after))
                {
                        after++;
                        while (after < read_length && read_blank(ifs, after))
                                after++;
                }
                // Classify the separator before replacing it with NUL.
                read_line[at] = end;
                *cursor = after;
        }
        return read_line + begin;
}

COLD fn shell_read(writer write, string_address input)
{
        bool raw = false;
        string_address array_name = null;
        positive index = 1;
        positive at = 0;
        positive names;
        bool ended = false;
        bool failed = false;
        bool limited = false;
        positive limit = 0;
        bool timed = false;
        bool timed_out = false;
        timespec timeout;
        timespec deadline;
        p8 stop_at = '\n';
        bool exact = false;
        bool hidden = false;
        bool quieted = false;
        terminal_modes quiet_held;
        b32 descriptor = 0;
        string_address prompt = null;
        string_address ifs;
        p8 ifs_default[] = " \t\n";

        read_length = 0;

        if (!read_reserve(1))
        {
                return shell_answer(string_report(log_error, 2, "%s: no room\n", "read"));
        }

        shell_option_walk options = {.index = 1};
        p8 which;
        while (shell_option_letter(address_of options, address_of which))
        {
                if (which == 'r' || which == 's' || which == 'e')
                {
                        raw |= which == 'r';
                        hidden |= which == 's';
                        continue;
                }
                p8 said[2] = {which, end};
                if (!string_first_of("pnNdtaui", which))
                        return shell_answer(shell_letter_refused(
                            "read", which,
                            "read [-Eers] [-a array] [-d delim] [-i text] "
                            "[-n nchars] [-N nchars] [-p prompt] "
                            "[-t timeout] [-u fd] [name ...]"));
                string_address value = shell_option_argument(address_of options);
                if (!value)
                        return shell_answer(string_report(log_error, 2, "read: option -%s wants a value\n", said));
                if (which == 'a')
                {
                        if (!shell_valid_name(value,
                                              string_length(value)))
                        {
                                shell_name_refused("read", value,
                                                   string_length(value));

                                return shell_answer(2);
                        }

                        array_name = value;
                }
                else if (which == 'p')
                {
                        //      A prompt is for someone to read, so it is
                        //      held until the descriptor is known and
                        //      written only when that descriptor is a
                        //      terminal. Bash is silent down a pipe, and
                        //      writing it anyway put the prompt in the
                        //      error stream of every script that read a
                        //      file.
                        prompt = value;
                }
                else if (which == 'i')
                {
                        // The editor would put it in front of what is
                        // typed. Nothing is typed down a pipe, so the
                        // option is taken and its value is not.
                }
                else if (which == 'u')
                {
                        positive asked;

                        if (!read_nonnegative(value, b32_max,
                                              address_of asked))
                                return shell_answer(string_report(log_error, 1, "read: bad descriptor: %s\n",
                                              value));

                        descriptor = (b32)asked;
                }
                else if (which == 'n' || which == 'N')
                {
                        positive asked;

                        if (!read_nonnegative(value, positive_max,
                                              address_of asked))
                                return shell_answer(string_report(log_error, 1, "read: bad count: %s\n", value));

                        limited = true;
                        exact = which == 'N';
                        limit = asked;
                }
                else if (which == 'd')
                        stop_at = string_get(value);
                else
                {
                        if (!read_timeout(value, address_of timeout))
                                return shell_answer(string_report(log_error, 1, "read: bad timeout: %s\n", value));

                        timed = true;
                }
        }
        index = options.index;

        if (prompt)
        {
                p8 settings[64];

                if (system_control(descriptor, BUILTIN_TCGETS, settings) == 0)
                        log_error(prompt, 0);
        }

        names = index;

        // A malformed operand is not a variable assignment and can be
        // rejected before input is touched. A readonly name is different:
        // Issue 8 requires names before it to have been assigned, so that
        // error is found only when assignments are performed below.
        if (names >= shell_argc)
        {
                if (env_readonly("REPLY"))
                {
                        shell_read_refused((string_address) "REPLY");

                        return shell_answer(shell_bash_compat ? 1 : 2);
                }
        }
        else
        {
                for (positive name = names; name < shell_argc; name++)
                {
                        positive length = string_length(shell_argv[name]);

                        //      An operand that is not a name is a usage
                        //      error to the Debian shell and a plain
                        //      failure to Bash, which answers 1.
                        if (!shell_valid_name(shell_argv[name], length))
                        {
                                shell_name_refused("read", shell_argv[name],
                                                   length);

                                return shell_answer(shell_bash_compat ? 1 : 2);
                        }

                }
        }

        /*
                -t 0 asks whether a line could be read, not for one.

                Zero seconds is not a timeout a read can meet: Bash answers
                for whether the descriptor is ready and reads nothing, which
                is the only way a script can poll without consuming.
        */
        if (timed && !timeout.tv_sec && !timeout.tv_nsec)
        {
                timespec none = {0, 0};

                return shell_answer(
                    descriptor_wait_readable(descriptor, address_of none, null) > 0
                        ? 0
                        : 1);
        }

        if (timed)
                read_deadline(timeout, address_of deadline);

        if (hidden)
                quieted = read_echo_off(descriptor, address_of quiet_held);

        bool escaped = false;
        while (!(limited && read_length >= limit))
        {
                p8 value;

                if (read_length == positive_max || !read_reserve(read_length + 2))
                {
                        if (quieted)
                                system_control(descriptor, PTY_TCSETS,
                                               address_of quiet_held);

                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "read"));
                }

                if (timed && !read_waited(descriptor, address_of deadline))
                {
                        timed_out = true;
                        ended = true;
                        break;
                }

                bipolar got = system_read_once(descriptor, address_of value, 1);

                if (got != 1)
                {
                        if (got < 0)
                                failed = true;
                        else
                                ended = true;
                        break;
                }

                if (!escaped && !exact && value == stop_at)
                        break;
                if (!raw && !escaped && value == '\\')
                {
                        escaped = true;
                        continue;
                }
                if (escaped && value == '\n')
                {
                        escaped = false;
                        continue;
                }
                read_literal[read_length] = escaped;
                read_line[read_length++] = value;
                escaped = false;
        }

        read_line[read_length] = end;
        read_literal[read_length] = 0;

        // Put the terminal back before any name is assigned: every path out
        // of here from now on is a return.
        if (quieted)
        {
                system_control(descriptor, PTY_TCSETS, address_of quiet_held);

                // The newline the terminal did not show, so the next prompt
                // does not land on the same line as what was typed.
                log_error("\n", 1);
        }

        /* Bash leaves destinations alone on a descriptor/syscall failure.
           EOF and timeout are different: both still publish what was read. */
        if (failed && shell_bash_compat)
                return shell_answer(1);

        if (!array_name && names >= shell_argc)
        {
                if (!(env_assign("REPLY", read_line)
                    || shell_read_refused("REPLY")))
                        return shell_answer((shell_bash_compat && env_readonly("REPLY") ? 1 : 2));

                return shell_answer(read_result(failed, ended, timed_out));
        }

        /*
                -N hands the bytes over whole.

                What it read is a count of bytes and not a line, so there are
                no fields in it to cut: the first name takes all of it and the
                rest are cleared, which is what Bash does.
        */
        if (exact)
        {
                if (!array_name && !(env_assign(shell_argv[names], read_line)
                    || shell_read_refused(shell_argv[names])))
                        return shell_answer(
                            (shell_bash_compat && env_readonly(shell_argv[names]) ? 1 : 2));

                if (array_name)
                {
                        string_address one = read_line;

                        if (!shell_array_words(array_name,
                                               string_length(array_name),
                                               address_of one, 1))
                                return shell_answer(2);
                }
                else
                        for (positive name = names + 1; name < shell_argc;
                             name++)
                                if (!(env_assign(shell_argv[name], "")
                                    || shell_read_refused(shell_argv[name])))
                                        return shell_answer(
                                            (shell_bash_compat && env_readonly(shell_argv[name]) ? 1 : 2));

                return shell_answer(
                    read_result(failed, ended && read_length < limit,
                                timed_out));
        }

        {
                string_address value = env_get("IFS");

                // On a copy: IFS points into env_storage, and the first name
                // assigned below is free to compact the block out from under
                // it.
                if (value)
                {
                        positive length = string_length(value);

                        if (length == positive_max ||
                            !shell_array_room(read_ifs, read_ifs_room, length + 1))
                        {
                                return shell_answer(string_report(log_error, 2, "%s: no room\n", "read"));
                        }

                        memory_copy(read_ifs, value, length + 1);
                        ifs = read_ifs;
                }
                else
                {
                        ifs = ifs_default;
                }
        }

        if (array_name)
        {
                positive count = 0;
                string_address field;
                while ((field = read_field(ifs, address_of at, false)))
                {
                        if (!shell_array_room(read_words, read_words_room, count + 1))
                                return shell_answer(string_report(log_error, 2, "%s: no room\n", "read"));
                        read_words[count++] = field;
                }
                if (!shell_array_words(array_name, string_length(array_name),
                                       read_words, count))
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "read"));
        }
        else
                while (names < shell_argc)
                {
                        string_address field = read_field(ifs, address_of at,
                                                           names + 1 == shell_argc);
                        if (!(env_assign(shell_argv[names], field ? field : (string_address)"")
                            || shell_read_refused(shell_argv[names])))
                                return shell_answer((shell_bash_compat && env_readonly(shell_argv[names]) ? 1 : 2));
                        names++;
                }

        shell_answer(read_result(failed, ended, timed_out));
}

/*
        mapfile, and readarray which is the same command under its other name.

        One array element per delimited record.
        Every option is about which records and where they land: -s skips
        some, -n stops after some, -O says which subscript the first one
        takes, -u says which descriptor to read, -d changes what ends a
        record and -t leaves that byte off the element.

        Keep only the current refill and any record spanning it. A limited
        read must leave the next record on the descriptor: seek back unread
        bytes on files, and avoid read-ahead on pipes.
*/
static p8 address_to mapfile_text;
static positive mapfile_room;

COLD fn shell_mapfile(writer write, string_address input)
{
        string_address name = (string_address) "MAPFILE";
        positive index = 1;
        positive used = 0;
        positive at = 0;
        positive stop = 0;
        positive count = 0;
        positive wanted = 0;
        positive skip = 0;
        positive origin = 0;
        positive name_length;
        p8 delimiter = '\n';
        p8 written[32];
        bool trim = false;
        bool append = false;
        bool finished = false;
        b32 from = 0;

        (void)input;

        shell_option_walk options = {.index = 1};
        p8 which;
        while (shell_option_letter(address_of options, address_of which))
        {
                if (which == 't')
                {
                        trim = true;
                        continue;
                }
                p8 said[2] = {which, end};
                if (!string_first_of("nsOduc", which))
                {
                        if (shell_letter_bad(shell_argv[0], which))
                                string_format(log_error,
                                    "%s: usage: %s [-d delim] [-n count] "
                                    "[-O origin] [-s count] [-t] [-u fd] "
                                    "[-C callback] [-c quantum] [array]\n",
                                    shell_argv[0], shell_argv[0]);

                        return shell_answer(2);
                }
                string_address value = shell_option_argument(address_of options);
                if (!value)
                        return shell_answer(string_report(log_error, 2, "%s: option -%s wants a value\n",
                                      shell_argv[0], said));
                positive asked;
                if (which == 'd')
                {
                        delimiter = string_get(value);
                        continue;
                }

                //      -c is how many records go by between calls to the -C
                //      callback. There is no callback here, and Bash reads
                //      the number and does nothing with it when none was
                //      given, so a quantum on its own changes nothing but
                //      must still be taken.
                if (which == 'c')
                {
                        if (!read_nonnegative(value, (positive)bipolar_max,
                                              address_of asked))
                                return shell_answer(string_report(
                                    log_error, 2, "%s: %s: bad number\n",
                                    shell_argv[0], value));
                        continue;
                }

                if (!read_nonnegative(value, which == 'u' ? b32_max : (positive)bipolar_max,
                                      address_of asked))
                        return shell_answer(string_report(log_error, which == 'u' ? 1 : 2, "%s: %s: bad number\n",
                                      shell_argv[0], value));

                if (which == 'n')
                        wanted = asked;
                else if (which == 's')
                        skip = asked;
                else if (which == 'O')
                {
                        origin = asked;
                        append = true;
                }
                else
                {
                        from = (b32)asked;
                        if (system_call_3(syscall(fcntl), from, FILE_F_GETFL, 0) < 0)
                                return shell_answer(string_report(log_error, 1, "mapfile: bad descriptor\n"));
                }
        }
        index = options.index;

        if (index < shell_argc)
                name = shell_argv[index++];

        name_length = string_length(name);

        //      Bash calls this "not a valid identifier" and answers one;
        //      two is what it keeps for an option it does not have.
        //      A word after the name is ignored, the way bash ignores it.
        if (!shell_valid_name(name, name_length))
        {
                shell_name_refused(shell_argv[0], name, name_length);

                return shell_answer(1);
        }

        p8 attributes = shell_array_attributes(name, name_length);
        if (attributes & (SHELL_ARRAY_READONLY | SHELL_ARRAY_ASSOCIATIVE))
                return shell_answer(string_report(log_error, 1, "mapfile: %s: %s\n", name,
                              attributes & SHELL_ARRAY_READONLY
                                  ? "readonly variable" : "not an indexed array"));
        if (!shell_variable_attribute_set(name, name_length,
                                          SHELL_ARRAY_INDEXED |
                                              SHELL_ARRAY_ASSIGNED,
                                          SHELL_ARRAY_ASSOCIATIVE) ||
            (!append && !shell_array_clear(name, name_length)))
                return shell_answer(string_report(log_error, 2, "%s: no room\n", "mapfile"));

        bool seekable = wanted && system_seek(from, 0, FILE_SEEK_CUR) >= 0;
        positive amount = wanted && !seekable ? 1 : 4096;
        bool failed = false;
        while (!wanted || count < wanted)
        {
                if (stop < used)
                        stop += memory_span_without_byte(mapfile_text + stop, delimiter, used - stop);
                if (stop == used && !finished)
                {
                        if (skip)
                                at = used;
                        used -= at;
                        if (used && at)
                                memory_copy(mapfile_text, mapfile_text + at, used);
                        at = 0;
                        stop = used;
                        if (used > positive_max - amount - 1 ||
                            !shell_array_room(mapfile_text, mapfile_room, used + amount + 1))
                        {
                                failed = true;
                                break;
                        }
                        bipolar got = system_read_retry(from, mapfile_text + used, amount);
                        // Bash treats a read failure on an open descriptor as EOF.
                        finished = got <= 0;
                        if (got > 0)
                                used += (positive)got;
                        mapfile_text[used] = end;
                        continue;
                }
                if (at == used)
                        break;
                positive next = stop + (stop < used);
                if (skip)
                        skip--;
                else
                {
                        positive end_at = trim ? stop : next;
                        p8 held = mapfile_text[end_at];
                        mapfile_text[end_at] = end;
                        failed = origin > (positive)bipolar_max - count ||
                            !shell_array_set(name, name_length, written,
                                     bipolar_into_string(
                                         written, (bipolar)(origin + count)),
                                     mapfile_text + at, false);
                        mapfile_text[end_at] = held;
                        count++;
                }
                at = stop = next;
                if (failed)
                        break;
        }
        if (seekable && used > at)
                system_seek(from, (positive)(-(bipolar)(used - at)), FILE_SEEK_CUR);
        if (failed)
                return shell_answer(string_report(log_error, 2, "%s: no room\n", "mapfile"));
        shell_answer(0);
}

/*
        getopts.

        One option per call, its place kept in OPTIND and, within a bundled
        word, in an offset of its own that OPTIND has no room for.

        Bash exposes the current bundled word until its final letter; dash
        exposes the next word from the first letter. Both share the offset
        walk. Native POSIX behavior resets on OPTIND=1 and unsets an unused
        OPTARG; dash's historical exceptions apply only to that exact identity.
*/
static fn shell_getopts_parameters_changed()
{
        if (shell_dash_compat)
        {
                getopts_next = 0;
                getopts_offset = -1;
        }
}

static COLD fn shell_getopts_index_changed()
{
        if (!getopts_publishing && shell_number(env_get("OPTIND")) <= 1)
                getopts_offset = -1;
}

static bool getopts_publish(positive index)
{
        bool assigned;

        getopts_publishing = true;
        assigned = env_set_number("OPTIND", index);
        getopts_publishing = false;
        return assigned;
}

static bool getopts_optarg(string_address value)
{
        if (value)
                return env_assign("OPTARG", value);

        if (env_readonly("OPTARG"))
                return false;

        env_unset("OPTARG");
        return true;
}

// Nothing left to read: the name is told so, and where the walk stopped is
// left where it is for a caller that puts OPTIND back.
/*
        Whether getopts says anything about a bad option.

        OPTERR is a number and only zero silences it, which is what the
        reference shells read: an unset OPTERR complains, and so does one set
        to anything else.
*/
static PURE bool getopts_complains()
{
        string_address value = env_get("OPTERR");

        return !value || !(value[0] == '0' && !value[1]);
}

fn shell_getopts_done(string_address name, positive next, bool assigned)
{
        getopts_offset = -1;
        getopts_next = next;

        if (!getopts_publish(next + 1))
                assigned = false;
        if (!env_assign(name, "?"))
                assigned = false;

        shell_answer(assigned ? 1 : 2);
}

// Where the next call starts, and how far into the word it just read. A step
// that has nothing after it says the word is finished with.
fn shell_getopts_answer(string_address name, string_address said,
                        string_address word, string_address step, positive next,
                        bool assigned)
{
        getopts_offset = step && string_get(step) ? (bipolar)(step - word) : -1;
        getopts_next = next;

        if (!getopts_publish(next + 1 -
                             (shell_bash_compat && getopts_offset >= 0)))
                assigned = false;
        if (!env_assign(name, said))
                assigned = false;

        shell_answer(assigned ? 0 : 2);
}

/*
        getopts, called wrongly.

        Bash writes its usage line with nothing before it, the way every
        builtin's second line goes out; dash names the script and the line
        first, and spells the line its own way.
*/
static COLD b32 shell_getopts_usage()
{
        if (shell_bash_compat)
                return string_report(log_error, 2,
                    "getopts: usage: getopts optstring name [arg ...]\n");

        shell_diagnostic_where();

        return string_report(log_error, 2,
            "getopts: Usage: getopts optstring var [arg...]\n");
}

COLD fn shell_getopts(writer write, string_address input)
{
        string_address options;
        string_address name;
        positive count = 0;
        positive optind;
        positive next;
        string_address word = null;
        string_address step = null;
        string_address found;
        p8 letter;
        p8 value[2];
        bool silent;

        positive first = 1;

        //      Both shells read getopts' own options before the optstring,
        //      so a leading word that looks like one is refused rather than
        //      taken for the letters to look for.
        if (first < shell_argc && word_is(shell_argv[first], "--"))
                first++;
        else if (first < shell_argc && string_is(shell_argv[first], '-') &&
                 string_get(shell_argv[first] + 1))
        {
                shell_option_walk walk = {first};
                p8 which;

                if (shell_option_letter(address_of walk, address_of which))
                        return shell_answer(shell_letter_refused(
                            "getopts", which,
                            "getopts optstring name [arg ...]"));
        }

        if (shell_argc - first < 2)
                return shell_answer(shell_getopts_usage());

        options = shell_argv[first];
        name = shell_argv[first + 1];
        silent = string_is(options, ':');

        //      A name that is no name at all is named back: this used to
        //      answer two and say nothing, and a script could not tell that
        //      from the end of the options.
        if (!shell_valid_name(name, string_length(name)))
        {
                shell_name_refused("getopts", name, string_length(name));

                return shell_answer(shell_bash_compat ? 1 : 2);
        }

        if (shell_argc > first + 2)
        {
                positive index = first + 2;

                if (!shell_array_room(shell_getopts_list, shell_getopts_room,
                                      shell_argc - index + 1))
                        return shell_answer(2);

                positive listed = shell_argc - index;

                if (listed >= 4)
                {
                        memory_copy_apart(shell_getopts_list, shell_argv + index,
                                          listed * sizeof(string_address));
                        count = listed;
                }
                else
                        while (index < shell_argc)
                                shell_getopts_list[count++] = shell_argv[index++];
        }
        else
        {
                if (!shell_array_room(shell_getopts_list, shell_getopts_room,
                                      shell_parameter_count + 1))
                        return shell_answer(2);

                if (shell_parameter_count >= 4)
                {
                        memory_copy_apart(shell_getopts_list, shell_parameter,
                                          shell_parameter_count *
                                              sizeof(string_address));
                        count = shell_parameter_count;
                }
                else
                        while (count < shell_parameter_count)
                        {
                                shell_getopts_list[count] = shell_parameter[count];
                                count++;
                        }
        }

        optind = shell_dash_compat ? getopts_next + 1
                                  : shell_number(env_get("OPTIND"));

        if (optind < 1)
                optind = 1;

        next = optind - 1;

        if (shell_bash_compat && next >= count)
                return shell_getopts_done(name, count, getopts_optarg(null));

        // Bash publishes the current bundled word until its last option,
        // while dash publishes the following word immediately. Keep only the
        // word's index and byte offset, never a pointer into a replaced argv.
        if (getopts_offset >= 0 &&
            ((shell_bash_compat || shell_dash_compat)
                 ? getopts_word < count
                 : optind > 1 && optind - 2 < count))
        {
                positive resumed = (shell_bash_compat || shell_dash_compat)
                                       ? getopts_word : optind - 2;
                word = shell_getopts_list[resumed];
                if ((positive)getopts_offset < string_length(word))
                {
                        step = word + getopts_offset;
                        if (shell_bash_compat)
                                next++;
                        else if (shell_dash_compat)
                                next = resumed + 1;
                }
        }

        if (!step || !string_get(step))
        {
                getopts_word = next;
                word = next < count ? shell_getopts_list[next] : null;
                step = word;

                if (!step || string_not(step, '-') || !string_get(step + 1))
                        return shell_getopts_done(name, next,
                                                  shell_dash_compat ||
                                                      getopts_optarg(null));

                step++;
                next++;

                if (string_is(step, '-') && !string_get(step + 1))
                        return shell_getopts_done(name, next,
                                                  shell_dash_compat ||
                                                      getopts_optarg(null));
        }

        letter = string_get(step++);
        value[0] = letter;
        value[1] = end;

        // A colon in the word is never an option, whatever the option string
        // says: it is the character that marks one as taking an argument.
        found = letter == ':' ? null : string_first_of(options, letter);

        if (!found)
        {
                bool assigned;

                if (silent)
                        assigned = getopts_optarg(value);
                else
                {
                        assigned = getopts_optarg(null);

                        // OPTERR=0 asks for the answer without the
                        // complaint, which is what a script that prints its
                        // own usage sets before the loop.
                        if (getopts_complains())
                                string_format(log_error,
                                              "getopts: illegal option -- %s\n",
                                              value);
                }

                return shell_getopts_answer(name, "?", word, step, next,
                                            assigned);
        }

        if (string_is(found + 1, ':'))
        {
                if (!string_get(step) && next >= count)
                {
                        if (silent)
                        {
                                bool assigned = getopts_optarg(value);

                                return shell_getopts_answer(name, ":", word, null,
                                                            next, assigned);
                        }

                        bool assigned = getopts_optarg(null);

                        if (getopts_complains())
                                string_format(
                                    log_error,
                                    "getopts: option requires an argument -- %s\n",
                                    value);

                        return shell_getopts_answer(name, "?", word, null, next,
                                                    assigned);
                }

                if (!string_get(step))
                        step = shell_getopts_list[next++];

                bool assigned = getopts_optarg(step);

                return shell_getopts_answer(name, value, word, null, next,
                                            assigned);
        }

        bool assigned = getopts_optarg(shell_dash_compat
                                          ? (string_address)"" : null);

        return shell_getopts_answer(name, value, word, step, next, assigned);
}

/*
        umask, in both spellings.

        The symbolic form is chmod's grammar read against what a file is
        allowed, and the mask is the other way round -- "u=rwx" says the
        owner keeps everything, which is nothing masked off. So the allowance
        goes through the reader chmod uses, X and the copied classes
        included, and is inverted on the way out. The reference shell departs
        from chmod twice: it does not know t, which no mask could hold
        anyway, and a copied class is what the mask allowed before the
        command rather than what the clauses so far have made.
*/
//      "u+r," and "," name a clause with nothing in it. Bash refuses both;
//      dash reads what came before the comma and is content.
static PURE bool umask_clause_empty(string_address step)
{
        bool empty = true;

        while (string_get(step))
        {
                if (string_is(step, ','))
                {
                        if (empty)
                                return true;

                        empty = true;
                }
                else
                        empty = false;

                step++;
        }

        return empty;
}

/*
        The byte bash would name in a symbolic mode it will not read.

        A clause is [ugoa]* then one of +-= then [rwxXst] and the three
        letters that copy another field, and a comma starts the next one.
        Whatever stops that walk is what bash quotes, and where it stopped
        says whether it calls it an operator or a character.
*/
static COLD string_address umask_symbolic_refused(string_address word,
                                                  bool address_to operator_here)
{
        string_address at = word;

        for (;;)
        {
                while (string_first_of("ugoa", string_get(at)) &&
                       string_get(at))
                        at++;

                if (!string_first_of("+-=", string_get(at)) ||
                    !string_get(at))
                {
                        address_to operator_here = true;

                        return at;
                }

                at++;

                while (string_first_of("rwxXstugo", string_get(at)) &&
                       string_get(at))
                        at++;

                if (string_get(at) == ',')
                {
                        at++;
                        continue;
                }

                if (string_get(at))
                {
                        address_to operator_here = false;

                        return at;
                }

                return null;
        }
}

bool umask_symbolic(string_address step, positive address_to mask)
{
        positive allowed;

        if (string_first_of(step, 't'))
                return false;

        if (!file_mode_adjust(step, 0777 & ~(address_to mask), false, 07777,
                              true, address_of allowed))
                return false;

        address_to mask = 0777 & ~allowed;

        return true;
}

fn umask_written(writer write, positive mask)
{
        p8 digits[3];
        positive length = positive_into_base(digits, mask & 0777, 8, false);
        positive padding = 3 - length;

        // Keep the historical five one-byte writer calls: prefix, three
        // field digits (including padding), then newline.
        write("0", 1);

        writer_fill(write, padding, '0');

        for (positive i = 0; i < length; i++)
                write(digits + i, 1);

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

COLD fn shell_umask(writer write, string_address input)
{
        // Two bytes: the shared formatter has no %c.
        p8 room[2];
        shell_option_walk walk = {1};
        positive index;
        bool spoken = false;
        //      -p asks for a line that can be typed back in. It is Bash's
        //      alone; dash calls the letter an illegal option.
        bool reproducible = false;
        p8 option;
        positive mask;

        while (shell_option_letter(address_of walk, address_of option))
        {
                if (option == 'S')
                        spoken = true;
                else if (option == 'p' && shell_bash_compat)
                        reproducible = true;
                else
                        return shell_answer(shell_letter_refused(
                            "umask", option, "umask [-p] [-S] [mode]"));
        }

        index = walk.index;

        // The only way to read it is to set it, so it is put straight back.
        mask = system_call_1(syscall(umask), 0);
        system_call_1(syscall(umask), mask);

        if (index >= shell_argc)
        {
                if (reproducible)
                        string_format(write, spoken ? "umask -S " : "umask ");

                if (spoken)
                        umask_spoken(write, mask);
                else
                        umask_written(write, mask);

                return shell_answer(0);
        }

        {
                string_address word = shell_argv[index];
                //      Bash answers one for a mode it cannot read; dash and
                //      POSIX answer two.
                b32 refused = shell_bash_compat ? 1 : 2;

                //      A leading digit means a number, whichever digit it
                //      is: "umask 8" is a number out of range to both
                //      shells and a mode to neither.
                if (string_get(word) >= '0' && string_get(word) <= '9')
                {
                        positive used;
                        positive value = string_digits_octal_max(
                            word, (positive)-1, address_of used);

                        word += used;

                        if (string_get(word))
                        {
                                shell_diagnostic_where();

                                return shell_answer(string_report(
                                    log_error, refused,
                                    shell_bash_compat
                                        ? "umask: %s: octal number out of "
                                          "range\n"
                                        : "umask: Illegal number: %s\n",
                                    shell_argv[index]));
                        }

                        mask = value;
                }
                //      An empty clause -- a lone comma, or one on the end --
                //      is a mode operator Bash will not have. dash reads the
                //      trailing one and stops, which is where these two part.
                else if ((shell_bash_compat && umask_clause_empty(word)) ||
                         !umask_symbolic(word, address_of mask))
                {
                        shell_diagnostic_where();

                        if (!shell_bash_compat)
                                return shell_answer(string_report(
                                    log_error, refused,
                                    "umask: Illegal mode: %s\n", word));

                        {
                                //      Two bytes: the formatter has no %c.
                                p8 said[2];
                                bool operator_here = false;
                                string_address at = umask_symbolic_refused(
                                    word, address_of operator_here);

                                return shell_answer(string_report(
                                    log_error, refused,
                                    operator_here
                                        ? "umask: `%s': invalid symbolic "
                                          "mode operator\n"
                                        : "umask: `%s': invalid symbolic "
                                          "mode character\n",
                                    shell_option_spelled(
                                        said, at ? string_get(at) : end)));
                        }
                }
        }

        system_call_1(syscall(umask), mask);

        //      Bash writes the new mask out when -S was asked for as well as
        //      setting it; dash sets in silence. -p on its own is silent in
        //      both, which is where the two options differ.
        if (spoken && shell_bash_compat)
        {
                mask = system_call_1(syscall(umask), mask);
                umask_spoken(write, mask);
        }

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

// Dash writes six places after the point, which is what a %f with nothing
// said about it writes; bash writes three. Only the first two of either can
// ever be anything but zero at a hundred ticks to the second, so the places
// past them are the personality's spelling and nothing else.
#define CLOCK_PLACES 1000000
#define CLOCK_PLACES_BASH 1000

fn shell_time_written(writer write, bipolar ticks)
{
        positive seconds;
        positive fraction;
        positive places = shell_bash_compat ? 3 : 6;
        positive scale = shell_bash_compat ? CLOCK_PLACES_BASH : CLOCK_PLACES;

        if (ticks < 0)
                ticks = 0;

        seconds = (positive)ticks / CLOCK_TICKS;
        fraction = ((positive)ticks % CLOCK_TICKS) * (scale / CLOCK_TICKS);

        positive_to_string(write, seconds / 60);
        write("m", 1);
        positive_to_string(write, seconds % 60);
        write(".", 1);

        p8 fraction_text[6];
        positive fraction_length =
            positive_into_padded(fraction_text, fraction, places, '0');

        write(fraction_text, fraction_length);

        write("s", 1);
}

COLD fn shell_times(writer write, string_address input)
{
        shell_clocks clocks;
        shell_option_walk walk = {1};
        p8 which;

        //      times has no options; bash still reads for one, so that a
        //      letter is refused rather than counted as an operand. dash
        //      reads none and prints the four times whatever it was given.
        if (shell_bash_compat &&
            shell_option_letter(address_of walk, address_of which))
        {
                //      times is a special builtin, so the refusal takes the
                //      script with it wherever POSIX says it should.
                shell_letter_refused("times", which, "times");
                exec_special_error_note();

                return shell_answer(2);
        }

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
typedef struct
{
        positive number;
        string_address action;
        positive action_room;
} shell_trap_entry;

static shell_trap_entry address_to trap_table;
static positive trap_room;
static positive trap_count;

static string_address trap_names[] = {
    "EXIT", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
    "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
    null,
};

#define TRAP_NAMES (array_count(trap_names))
// The real-time signals have numbers and no names, up to the kernel's last.
#define TRAP_NUMBER_MAX 64

/*
        ERR, RETURN and DEBUG are conditions and not signals.

        Nothing sends them and no handler is installed for them, but they are
        written down, listed and taken away exactly as a signal is, so they
        are given numbers above the last one the kernel has. Everything that
        installs a disposition tests for that.
*/
#define TRAP_ERR (TRAP_NUMBER_MAX + 1)
#define TRAP_RETURN (TRAP_NUMBER_MAX + 2)
#define TRAP_DEBUG (TRAP_NUMBER_MAX + 3)
#define TRAP_CONDITION_MAX TRAP_DEBUG

static string_address trap_condition_names[] = {"ERR", "RETURN", "DEBUG",
                                                null};

// Whether each is standing, so the executor asks a byte rather than walking
// the table before every command it runs.
bool trap_err_here;
bool trap_return_here;
bool trap_debug_here;

/*
        The signal a word names: a number up to the last real-time signal,
        or a name in either case with or without SIG in front, as the
        reference shell reads it.
*/
//      A condition nobody can use, in the words of the shell being run:
//      Bash calls it an invalid signal specification, dash a bad trap.
static fn trap_refused(string_address word)
{
        //      dash writes this one without naming the script first, alone
        //      among its diagnostics.
        if (shell_bash_compat)
                shell_diagnostic_where();

        string_format(log_error, shell_bash_compat
                                     ? "trap: %s: invalid signal specification\n"
                                     : "trap: %s: bad trap\n",
                      word);
}

bipolar trap_number(string_address word)
{
        if (!word)
                return -1;

        string_address name = word;

        //      Bash reads SIG in front of a name, in its POSIX mode too;
        //      dash reads only the bare name and calls SIGINT a bad trap.
        if (shell_bash_compat && !string_compare_folded_max(name, "SIG", 3))
                name += 3;

        positive index = string_table_find_ascii_case(
            name, trap_names, sizeof(trap_names[0]), TRAP_NAMES);
        if (index < TRAP_NAMES)
                return (bipolar)index;

        index = string_table_find_ascii_case(name, trap_condition_names,
                                             sizeof(trap_condition_names[0]), 3);
        if (index < 3)
                return (bipolar)(TRAP_ERR + index);

        // Linux's second spelling of IO, accepted by both trap and kill.
        if (!string_compare_folded(name, "POLL"))
                return 29;

        //      Both shells trap a real-time signal by the name they write it
        //      under, and neither knows the utility's RT0.
        bipolar real_time = kill_real_time_of(name);
        if (real_time >= 0)
                return real_time;

        bool good;
        bipolar value = shell_signed(word, address_of good);
        return good && value >= 0 && value <= TRAP_NUMBER_MAX ? value : -1;
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
/* A fork sees the parent's trap table, and `trap -p EXIT` in that child must
   still print it, but the inherited action must not run when the child ends.
   Keep that distinction beside the shared table instead of copying or
   deleting the action. A child which replaces EXIT clears the marker below. */
static bool trap_child_context;
static bool trap_exit_inherited;

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

// Where a signal is in the table, or trap_count when it is not: the three
// readers below asked the same question with three loops.
static PURE positive trap_index(positive number)
{
        positive at = 0;

        while (at < trap_count && trap_table[at].number != number)
                at++;

        return at;
}

PURE bool trap_ignored(positive number)
{
        positive at = trap_index(number);

        return at < trap_count && !string_get(trap_table[at].action);
}

/* wait is the one command POSIX has a caught signal interrupt. Every other
   blocking command keeps SA_RESTART and runs its trap at the next command
   boundary. Change only the handlers with executable actions, then put their
   usual disposition back before wait returns. */
static fn trap_wait_restarting(bool restart)
{
        for (positive at = 0; at < trap_count; at++)
        {
                positive number = trap_table[at].number;
                string_address action = trap_table[at].action;

                if (number && string_get(action) &&
                    (shell_is_interactive || !shell_was_ignored((b32)number)))
                        shell_catch_mode((b32)number, restart);
        }
}

static bipolar trap_pending_number()
{
        if (trap_caught)
                for (positive number = 1; number <= TRAP_SIGNAL_MAX; number++)
                        if (trap_pending[number])
                                return (bipolar)number;

        return -1;
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

        // Signal handlers write these bytes asynchronously; keep the volatile
        // byte stores rather than casting that contract away for a bulk fill.
        for (positive i = 0; i <= TRAP_SIGNAL_MAX; i++)
                trap_pending[i] = 0;

        trap_caught = false;
}

static string_address trap_detach(positive number, positive address_to room)
{
        positive index = trap_index(number);
        string_address action;

        if (index == trap_count)
                return null;

        action = trap_table[index].action;

        if (room)
                *room = trap_table[index].action_room;

        memory_copy(trap_table + index, trap_table + index + 1,
                    (trap_count - index - 1) * sizeof(trap_table[0]));

        trap_count--;
        return action;
}

fn trap_forget(positive number)
{
        positive room = 0;
        string_address action = trap_detach(number, address_of room);

        if (action)
                memory_free(action, room);
}

static bool trap_record(positive number, string_address action)
{
        positive length = string_length(action);
        string_address kept;

        if (length == positive_max ||
            !shell_array_room(trap_table, trap_room, trap_count + 1))
                return false;

        kept = shell_map(length + 1);

        if (!kept)
                return false;

        memory_copy(kept, action, length + 1);
        trap_table[trap_count].number = number;
        trap_table[trap_count].action = kept;
        trap_table[trap_count].action_room = length + 1;
        trap_count++;
        return true;
}

PURE string_address trap_action(positive number)
{
        positive index = trap_index(number);

        return index < trap_count ? trap_table[index].action : null;
}

fn trap_child_began()
{
        trap_child_context = true;
        trap_exit_inherited = trap_action(0) != null;
}

static fn trap_write_condition(writer write, positive number,
                               string_address action)
{
        write("trap -- ", 8);

        if (action)
                shell_quoted(write, action);
        else
                write("-", 1);

        write(" ", 1);

        //      Bash writes a signal as SIGINT and a condition as EXIT; dash
        //      and Bash's POSIX mode write the bare name for both, and this
        //      listing is compared against whichever shell was invoked.
        if (shell_bash_compat && !shell_posix_on() && number &&
            number <= TRAP_NUMBER_MAX)
                write("SIG", 3);

        if (number < TRAP_NAMES - 1)
                string_format(write, "%s", trap_names[number]);
        else if (number >= TRAP_ERR && number <= TRAP_DEBUG)
                string_format(write, "%s", trap_condition_names[number -
                                                                TRAP_ERR]);
        else if (number <= TRAP_NUMBER_MAX)
        {
                //      The real-time signals have no entry in the name table
                //      and are spelled RTMIN+n and RTMAX, as kill -l has them.
                p8 name[16];

                kill_name(number, name);
                write(name, string_length(name));
        }
        else
                positive_to_string(write, number);

        write("\n", 1);
}

//      What is standing, asked once after every change rather than by the
//      executor before every command.
static COLD fn trap_conditions_noted()
{
        trap_err_here = trap_action(TRAP_ERR) != null;
        trap_return_here = trap_action(TRAP_RETURN) != null;
        trap_debug_here = trap_action(TRAP_DEBUG) != null;
}

/*
        trap -l: the signals by number, five to a line.

        The same listing Bash writes, because a script that reads it is
        cutting on the number and the tab between the pairs.
*/
static COLD fn trap_listed(writer write)
{
        positive shown = 0;

        for (positive number = 1; number <= TRAP_NUMBER_MAX; number++)
        {
                p8 written[8];
                positive digits;

                // Thirty-two and thirty-three belong to the thread library
                // and no shell offers them, so no shell lists them.
                if (number == 32 || number == 33)
                        continue;

                digits = positive_into_string(written, number);

                if (digits < 2)
                        write(" ", 1);

                write(written, digits);
                write(") SIG", 5);

                if (number < TRAP_NAMES - 1)
                        write(trap_names[number],
                              string_length(trap_names[number]));
                else
                {
                        p8 name[16];
                        kill_name(number, name);
                        write(name, string_length(name));
                }

                shown++;
                write(shown % 5 ? "\t" : "\n", 1);
        }

        if (shown % 5)
                write("\n", 1);
}

COLD fn shell_trap(writer write, string_address input)
{
        positive index = 1;
        string_address action;
        b32 answer = 0;
        bool print = false;
        bool bare = false;

        bool listing = false;

        /*
                The letters, which cluster: Bash reads -lp as -l and -p. dash
                has neither letter and refuses the first one it is shown,
                fatally, because trap is a special builtin.
        */
        while (index < shell_argc && shell_argv[index][0] == '-' &&
               shell_argv[index][1] && !word_is(shell_argv[index], "--"))
        {
                for (string_address at = shell_argv[index] + 1; at[0]; at++)
                {
                        //      dash names the first letter and stops, and
                        //      that ends the script: trap is special.
                        if (!shell_bash_compat)
                        {
                                shell_letter_refused("trap", at[0],
                                    "trap [-Plp] [[action] signal_spec ...]");
                                exec_special_error_note();

                                return shell_answer(2);
                        }

                        if (at[0] == 'l')
                        {
                                listing = true;
                                continue;
                        }

                        if (at[0] == 'p')
                        {
                                print = true;
                                continue;
                        }

                        //      -P writes the action by itself, for scripts
                        //      that want the line and not the command that
                        //      would set it again.
                        if (at[0] == 'P')
                        {
                                bare = true;
                                continue;
                        }

                        //      Bash names the letter and prints how it is
                        //      called; the word is never read as an operand,
                        //      so `trap -x INT` sets nothing.
                        {
                                shell_letter_refused("trap", at[0],
                                    "trap [-Plp] [[action] signal_spec ...]");

                                if (shell_posix_on())
                                        exec_special_error_note();

                                return shell_answer(2);
                        }
                }

                index++;
        }

        if (listing)
        {
                trap_listed(write);

                return shell_answer(0);
        }

        //      A usage error from a special builtin ends the script in
        //      POSIX mode, so the listing after it never runs.
        if (print && bare)
        {
                string_format(log_error, "trap: cannot specify both -p and -P\n");

                if (shell_posix_on())
                        exec_special_error_note();

                return shell_answer(2);
        }

        if (index < shell_argc && word_is(shell_argv[index], "--"))
                index++;

        if (bare)
        {
                //      Nothing to write the action of, which Bash refuses
                //      rather than treating as every condition.
                if (index >= shell_argc)
                {
                        string_format(log_error,
                            "trap: -P requires at least one signal name\n");

                        if (shell_posix_on())
                                exec_special_error_note();

                        return shell_answer(2);
                }

                while (index < shell_argc)
                {
                        bipolar number = trap_number(shell_argv[index++]);
                        string_address recorded;

                        if (number < 0 || number > TRAP_CONDITION_MAX)
                        {
                                trap_refused(shell_argv[index - 1]);
                                answer = 1;
                                continue;
                        }

                        recorded = trap_action((positive)number);

                        if (recorded)
                        {
                                write(recorded, string_length(recorded));
                                write("\n", 1);
                        }
                }

                return shell_answer(answer);
        }

        if (print)
        {
                if (index >= shell_argc)
                {
                        //      Bash writes only the conditions somebody has
                        //      set; in its POSIX mode it writes every one it
                        //      accepts, KILL and STOP among them, out to the
                        //      last real-time signal and the three conditions
                        //      that are not signals at all.
                        bool every = shell_posix_on();
                        positive last = every ? TRAP_NUMBER_MAX : TRAP_NAMES - 2;

                        for (positive number = 0; number <= last; number++)
                        {
                                string_address recorded = trap_action(number);

                                if (!recorded && number &&
                                    shell_was_ignored(number))
                                        recorded = (string_address) "";

                                if (!recorded && !every)
                                        continue;

                                trap_write_condition(write, number, recorded);
                        }

                        //      Bash writes the three that are not signals
                        //      in the order DEBUG, ERR, RETURN, which is not
                        //      the order they are numbered in.
                        static const positive conditions[] = {
                            TRAP_DEBUG, TRAP_ERR, TRAP_RETURN};

                        for (positive at = 0; at < array_count(conditions); at++)
                        {
                                positive number = conditions[at];
                                string_address recorded = trap_action(number);

                                if (recorded || every)
                                        trap_write_condition(write, number,
                                                             recorded);
                        }
                }
                else
                {
                        while (index < shell_argc)
                        {
                                bipolar number = trap_number(shell_argv[index++]);

                                if (number < 0 ||
                                    number > TRAP_CONDITION_MAX)
                                {
                                        trap_refused(shell_argv[index - 1]);
                                        answer = 1;
                                        continue;
                                }

                                string_address recorded =
                                    trap_action((positive)number);

                                if (!recorded && number &&
                                    number <= TRAP_NUMBER_MAX &&
                                    shell_was_ignored((b32)number))
                                        recorded = (string_address) "";

                                // POSIX wants the default disposition of a
                                // signal written out. A condition has no
                                // disposition to write, so nothing is said
                                // about one nobody has set.
                                if (!recorded && (number > TRAP_NUMBER_MAX ||
                                                  !shell_posix_on()))
                                        continue;

                                trap_write_condition(write, (positive)number,
                                                     recorded);
                        }
                }

                return shell_answer(answer);
        }

        if (index >= shell_argc)
        {
                // Without -p only non-default conditions are listed. Every
                // number a trap can be set on is walked, so a trap on a
                // real-time signal past the named ones is listed too.
                //
                //      Bash also writes the signals that arrived already
                //      ignored, which have no table entry of their own;
                //      dash writes only what this shell has set. Asking a
                //      shell that inherited an ignored HUP for its traps
                //      therefore says nothing in dash and three lines in
                //      Bash.
                for (positive number = 0; number <= TRAP_NUMBER_MAX; number++)
                {
                        string_address recorded = trap_action(number);

                        if (!recorded && number && shell_bash_compat &&
                            shell_was_ignored((b32)number))
                                recorded = (string_address) "";

                        if (recorded)
                                trap_write_condition(write, number, recorded);
                }

                for (positive number = TRAP_ERR; number <= TRAP_DEBUG;
                     number++)
                {
                        string_address recorded = trap_action(number);

                        if (recorded)
                                trap_write_condition(write, number, recorded);
                }

                return shell_answer(0);
        }

        /*
                One operand is the historical reset form: the word is the
                condition, not an action, whether it is spelled INT or 2. Two
                or more and the first is the action, even where it happens to
                spell a signal -- `trap INT TERM` sets TERM to run INT.

                And the one word has to name a condition. Both references
                refuse one that does not, where this used to take it for an
                action, find no condition to attach it to, and say nothing.
        */
        if (index + 1 == shell_argc)
        {
                positive digits;
                bipolar only =
                    shell_posix_on()
                        ? (string_digits_checked_exact(shell_argv[index], 10,
                                               address_of digits)
                               ? (bipolar)digits : -1)
                        : trap_number(shell_argv[index]);

                //      Bash's POSIX mode keeps only the numeric reset form:
                //      `trap 2` takes the handler away, `trap INT` is a usage
                //      error, and a special builtin's usage error ends the
                //      script.
                if (only < 0 || only > TRAP_CONDITION_MAX)
                {
                        //      Bash answers a word it cannot use with how
                        //      trap is called, and in its POSIX mode that
                        //      ends the script; dash names the word instead.
                        if (shell_bash_compat)
                        {
                                string_format(log_error,
                                    "trap: usage: trap [-Plp] "
                                    "[[action] signal_spec ...]\n");

                                if (shell_posix_on())
                                        exec_special_error_note();

                                return shell_answer(2);
                        }

                        string_format(log_error, "trap: %s: bad trap\n",
                                      shell_argv[index]);

                        return shell_answer(1);
                }

                action = null;
        }
        else
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

                if (number < 0 || number > TRAP_CONDITION_MAX)
                {
                        trap_refused(shell_argv[index - 1]);
                        answer = 1;
                        continue;
                }

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

                /*
                        Bash goes further: a signal that arrived ignored
                        cannot be trapped at all, so the action is not even
                        written down and the listing keeps saying ''. POSIX
                        allows either reading and dash takes the other one,
                        keeping the string it was given.
                */
                if (deaf && shell_bash_compat)
                        continue;

                if (!number)
                        trap_exit_inherited = false;

                trap_forget((positive)number);

                if (action && !trap_record((positive)number, action))
                {
                        answer = 1;

                        if (number && !deaf)
                                shell_default((b32)number);

                        continue;
                }

                /*
                        "trap - INT" gives the signal back to the kernel,
                        "trap '' INT" makes the shell deaf to it, and anything
                        else is a line to run when it arrives. Only the third
                        needs a handler, and only signals: EXIT is something
                        the shell does to itself.
                */
                // Nothing installs a disposition for a condition: the
                // executor is what raises those.
                if (number && !deaf && number <= TRAP_NUMBER_MAX)
                {
                        if (!action)
                                shell_default((b32)number);
                        else if (!string_get(action))
                                shell_ignore((b32)number);
                        else
                                shell_catch((b32)number);
                }

                trap_conditions_noted();

        }

        shell_answer(answer);
}

/*
        alias.

        Recorded, listed and taken away here. Putting one in front of a command
        happens where a line is read, which is not this file, so what this holds
        is the table that side will ask.
*/
typedef struct
{
        string_address name;
        string_address value;
        positive name_room;
        positive value_room;
} shell_alias_entry;

static shell_alias_entry address_to alias_table;
static positive alias_room;
static positive alias_count;

PURE string_address alias_lookup(string_address name)
{
        positive at = string_table_find(name, alias_table, sizeof(alias_table[0]),
                                        alias_count);

        return at < alias_count ? alias_table[at].value : null;
}

// The counted name is also terminated; shell_alias restores its argv '='
// immediately after the call, including every allocation-failure return.
bool alias_record(string_address name, positive name_length, string_address value)
{
        positive value_length = string_length(env_reading(value));
        positive index;
        string_address kept_name;
        string_address kept_value;

        if (name_length == positive_max || value_length == positive_max)
                return false;

        index = string_table_find(name, alias_table, sizeof(alias_table[0]), alias_count);
        if (index < alias_count && value_length < alias_table[index].value_room &&
            (alias_table[index].value_room <= SHELL_SCRATCH_RETAIN ||
             value_length >= SHELL_SCRATCH_RETAIN))
        {
                memory_copy(alias_table[index].value, value, value_length + 1);
                return true;
        }

        kept_value = shell_map(value_length + 1);
        if (!kept_value)
                return false;
        // A growth source may be in the old value; finish copying before free.
        memory_copy(kept_value, value, value_length + 1);

        if (index < alias_count)
        {
                memory_free(alias_table[index].value, alias_table[index].value_room);
                alias_table[index].value = kept_value;
                alias_table[index].value_room = value_length + 1;
                return true;
        }

        kept_name = shell_map(name_length + 1);
        if (!kept_name || !shell_array_room(alias_table, alias_room, alias_count + 1))
        {
                if (kept_name)
                        memory_free(kept_name, name_length + 1);
                memory_free(kept_value, value_length + 1);
                return false;
        }
        string_copy_max_end(kept_name, name, name_length);
        alias_table[alias_count].name = kept_name;
        alias_table[alias_count].value = kept_value;
        alias_table[alias_count].name_room = name_length + 1;
        alias_table[alias_count].value_room = value_length + 1;
        alias_count++;

        return true;
}

// dash quotes the whole of name=value, not just the value, and a script that
// reads its own aliases back is reading that.
fn alias_written(writer write, positive index)
{
        if (shell_bash_compat)
        {
                string_format(write, "%s=", alias_table[index].name);
                shell_quoted(write, alias_table[index].value);
                write("\n", 1);
                return;
        }
        string_format(write, "'%s=%s'\n", alias_table[index].name, alias_table[index].value);
}

COLD fn shell_alias(writer write, string_address input)
{
        positive index = 1;
        b32 answer = 0;
        bool prefixed = shell_bash_compat && !shell_posix_on();
        bool listed = false;

        while (shell_bash_compat && index < shell_argc &&
               string_is(shell_argv[index], '-') && shell_argv[index][1])
        {
                string_address word = shell_argv[index++];

                if (word_is(word, "--"))
                        break;
                if (!word_is(word, "-p"))
                        return shell_answer(shell_option_refused(
                            "alias", word, "alias [-p] [name[=value] ... ]"));
                prefixed = listed = true;
        }

        if (index == shell_argc || listed)
        {
                positive at = 0;

                while (at < alias_count)
                {
                        if (prefixed)
                                write("alias ", 6);
                        alias_written(write, at++);
                }

                if (index == shell_argc)
                        return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                string_address mark = string_first_of(word, '=');

                if (mark && mark != word)
                {
                        *mark = end;
                        bool recorded = alias_record(word, mark - word, mark + 1);
                        *mark = '=';
                        if (!recorded)
                                answer = 1;

                        index++;
                        continue;
                }

                {
                        positive at = string_table_find(word, alias_table,
                                                        sizeof(alias_table[0]),
                                                        alias_count);

                        if (at < alias_count)
                        {
                                if (prefixed)
                                        write("alias ", 6);
                                alias_written(write, at);
                        }
                        else
                        {
                                shell_diagnostic_where();
                                answer = string_report(log_error, 1,
                                                       "alias: %s: not found\n",
                                                       word);
                        }
                }

                index++;
        }

        shell_answer(answer);
}

COLD fn shell_unalias(writer write, string_address input)
{
        // Two bytes: the shared formatter has no %c, so a letter is spelled.
        p8 room[2];
        shell_option_walk walk = {1};
        positive index;
        b32 status = 0;
        bool all = false;
        p8 option;

        //      The options are walked rather than compared word by word, so
        //      that "--" ends them and a letter that is not -a is refused
        //      with the status a usage error carries. Reading them by hand
        //      made "unalias --" ask for an alias called "--".
        while (shell_option_letter(address_of walk, address_of option))
        {
                if (option != 'a')
                {
                        return shell_answer(shell_letter_refused(
                            "unalias", option,
                            "unalias [-a] name [name ...]"));
                }

                all = true;
        }

        index = walk.index;

        //      -a takes them all and stops. The names behind it are not
        //      looked for -- there is nothing left to find -- and both
        //      references answer zero for them.
        if (all)
        {
                positive at;

                for (at = 0; at < alias_count; at++)
                {
                        memory_free(alias_table[at].name,
                                    alias_table[at].name_room);
                        memory_free(alias_table[at].value,
                                    alias_table[at].value_room);
                }

                alias_count = 0;
                return shell_answer(0);
        }

        //      Nothing named is a usage error in Bash and a quiet success in
        //      dash, which was asked for no alias and removed none.
        if (index >= shell_argc)
                return shell_answer(
                    shell_bash_compat
                        ? string_report(log_error, 2,
                                        "unalias: usage: unalias [-a] name [name ...]\n")
                        : 0);

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive at;

                at = string_table_find(word, alias_table, sizeof(alias_table[0]),
                                       alias_count);

                // A name that was never an alias is something the script asked
                // for and did not get, which POSIX has this say so.
                if (at >= alias_count)
                {
                        shell_diagnostic_where();
                        status = string_report(log_error, 1,
                                               "unalias: %s: not found\n", word);
                }

                if (at < alias_count)
                {
                        memory_free(alias_table[at].name,
                                    alias_table[at].name_room);
                        memory_free(alias_table[at].value,
                                    alias_table[at].value_room);
                        memory_copy(alias_table + at, alias_table + at + 1,
                                    (alias_count - at - 1) *
                                        sizeof(alias_table[0]));
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
COLD fn shell_eval(writer write, string_address input)
{
        p8 address_to eval_storage = null;
        positive eval_room = 0;
        positive used = 0;
        positive index = 1;
        bool room = true;
        positive syntax = shell_syntax_generation;

        if (shell_argc < 2)
                return shell_answer(0);

        //      Bash reads eval's words for options first, so "eval --"
        //      runs nothing and "eval -x" is a usage error fatal to a
        //      POSIX script. dash hands every word to the line, which is
        //      why "eval -x" is a command not found there.
        if (shell_bash_compat)
        {
                // Two bytes: the shared formatter has no %c.
                p8 room_spelled[2];
                shell_option_walk walk = {1};
                p8 option;

                while (shell_option_letter(address_of walk, address_of option))
                {
                        exec_special_error_note();
                        return shell_answer(shell_letter_refused(
                            "eval", option, "eval [arg ...]"));
                }

                index = walk.index;

                if (index >= shell_argc)
                        return shell_answer(0);
        }

        while (index < shell_argc)
        {
                positive length = string_length(shell_argv[index]);
                positive wanted;

                if (used > positive_max - 2 ||
                    length > positive_max - used - 2)
                {
                        room = false;
                        break;
                }

                wanted = used + length + 2;

                if (!shell_array_room(eval_storage, eval_room, wanted))
                {
                        room = false;
                        break;
                }

                if (used)
                        eval_storage[used++] = ' ';

                memory_copy(eval_storage + used, shell_argv[index], length);
                used += length;
                index++;
        }

        if (!room)
        {
                if (eval_storage)
                        memory_free(eval_storage, eval_room);

                shell_answer(string_report(log_error, 2, "%s: no room\n", "eval"));
                shell_stop_when_scripted(2);

                return;
        }

        eval_storage[used] = end;

        /* The nested line gets independent lexer storage and parser marks. */
        {
                lex_frame frame;
                string_address saved_command = shell_syntax_command;
                positive saved_base = shell_eval_lineno_base;

                shell_syntax_command = (string_address) "eval";
                if (shell_bash_compat)
                        shell_eval_lineno_base = shell_eval_lineno_base_now();

                lex_nest_enter(address_of frame);

                // Every line of it, not the first: eval "$(cmd)" is the
                // idiom, and what cmd printed has as many lines as it likes.
                run_lines(eval_storage);
                shell_input_end();
                exec_input_finish();

                lex_nest_leave(address_of frame);

                shell_syntax_command = saved_command;
                shell_eval_lineno_base = saved_base;
        }

        memory_free(eval_storage, eval_room);

        if (shell_syntax_generation != syntax)
        {
                positive kind = shell_syntax_generation - syntax;
                shell_syntax_generation = syntax;
                if (!shell_bash_compat ||
                    (!shell_command_reader_depth && kind != 1))
                        exec_special_error_note();
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

bool exec_control_builtin(string_address name, bool run);

/*
        break and continue.

        The work is the executor's, because leaving a loop means unwinding C
        frames it owns. They are named here so that they carry a disabled
        flag like every other builtin: `enable` lists them, and `enable -n
        break` sends the name back out to PATH the way the reference does.
*/
fn shell_break(writer write, string_address input)
{
        exec_control_builtin(shell_argv[0], true);
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

/*
        A static name table should not become a linear interpreter cost.
        Every simple command asks both the utility table and the builtin table;
        walking sixty utility names before discovering `[` made dispatch one
        of the hottest loops in the shell.

        The policy stays here, while all byte work stays at the hardware floor:
        memory_hash_33 hashes a name and memory_compare verifies the one hash
        candidate. The same index shape serves utilities and shell commands.
*/
typedef name_index_slot shell_name_slot;

static fn shell_name_index_build(address_any table, positive stride,
                                 positive count, shell_name_slot address_to slots,
                                 positive room)
{
        positive tombstones = 0;

        memory_fill(slots, 0, room * sizeof(slots[0]));

        for (positive index = 0; index < count; index++)
        {
                string_address name =
                    *(string_address address_to)((p8 address_to)table +
                                                  index * stride);
                positive2 answer = string_hash_33_length(name);

                name_index_put(slots, room, answer.x, answer.y, index,
                               address_of tombstones);
        }
}

static positive shell_name_index_find(string_address name, address_any table,
                                      positive stride, positive count,
                                      shell_name_slot address_to slots,
                                      positive room, bool address_to ready,
                                      positive2 named)
{
        positive at;

        if (!address_to ready)
        {
                shell_name_index_build(table, stride, count, slots, room);
                address_to ready = true;
        }

        at = named.x & (room - 1);

        for (positive probes = 0; probes < room; probes++)
        {
                shell_name_slot address_to slot = slots + at;

                if (!slot->index_plus_one)
                        return count;

                if (slot->hash == named.x && slot->length == named.y)
                {
                        positive index = slot->index_plus_one - 1;
                        string_address candidate =
                            *(string_address address_to)((p8 address_to)table +
                                                          index * stride);

                        if (!memory_compare(name, candidate, named.y))
                                return index;
                }

                at = (at + 1) & (room - 1);
        }

        return count;
}

/*
        Which categories this build keeps, decided once.

        The table below and the key array beside it both expand through
        SHELL_TOOL_KEEP, so a configuration cannot hand them different sets of
        tools: there is one cascade and they read it in turn. Spelling the
        cascade twice is what would let the key array describe a table that is
        not there.
*/
#ifdef SHELL_NO_UTILITIES
#define SHELL_TOOL_GENERAL(name, function)
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#define SHELL_TOOL_MONITOR(name, function)
#else
#define SHELL_TOOL_GENERAL(name, function) SHELL_TOOL_KEEP(name, function)
#ifdef SHELL_NO_UTIL_LINUX
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#else
#define SHELL_TOOL_UTIL_BIN(name, function) SHELL_TOOL_KEEP(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function) SHELL_TOOL_KEEP(name, function)
#endif
#ifdef SHELL_NO_MONITOR
#define SHELL_TOOL_MONITOR(name, function)
#else
#define SHELL_TOOL_MONITOR(name, function) SHELL_TOOL_KEEP(name, function)
#endif
#endif
#ifdef SHELL_UTILITY_PROGRAM
#define SHELL_TOOL_SYSTEM(name, function)
#else
#define SHELL_TOOL_SYSTEM(name, function) SHELL_TOOL_KEEP(name, function)
#endif
#define SHELL_TOOL(category, name, function) \
        SHELL_TOOL_##category(name, function)

static shell_tool shell_tools[] = {
#define SHELL_TOOL_KEEP(name, function) {#name, function},
#include "tools.inc"
#undef SHELL_TOOL_KEEP
    {null, null},
};

#define SHELL_TOOLS (array_count(shell_tools) - 1)

/*
        The first byte and the length of every name, in table order.

        A name is a pointer into the string pool, and the pool is laid out by
        the compiler in whatever order the literals were emitted, so walking
        the table to compare names reads a byte here and a byte there across
        the whole of it. Answering "no" from two bytes held together keeps
        that walk inside this array: 197 names are 394 bytes, one page that
        was going to be read anyway, instead of the eleven pages of pool that
        the string compares used to touch.

        It matters most to the one caller that cannot avoid the walk. Every
        installed utility name is found by the index below, but argv[0] is
        asked once per process and init -- what the kernel execs, PID 1 for
        the life of the machine -- sits near the end of the table, so it used
        to read almost every name in the image to discover its own.

        Two bytes is the whole key on purpose. It fits 197 entries in one
        page, and the pair already separates the table into 102 groups of
        which the largest is six, so what survives the filter is a handful of
        candidates rather than a shorter list of the same kind.
*/
static const p8 shell_tool_key[][2] = {
#define SHELL_TOOL_KEEP(name, function) \
        {(p8)(#name)[0], (p8)(sizeof(#name) - 1)},
#include "tools.inc"
#undef SHELL_TOOL_KEEP
};

#undef SHELL_TOOL
#undef SHELL_TOOL_SYSTEM
#undef SHELL_TOOL_UTIL_SBIN
#undef SHELL_TOOL_UTIL_BIN
#undef SHELL_TOOL_MONITOR
#undef SHELL_TOOL_GENERAL

_Static_assert(array_count(shell_tool_key) == SHELL_TOOLS,
               "the key array and the tool table describe the same tools");
/*
        Room for every name with slots to spare, because the index is open:
        a full one has nowhere to put the next name and nowhere to stop
        looking. The assertion under the table is what makes outgrowing it a
        build that stops rather than a shell that hangs, which is how this
        was found -- one tool too many and every lookup spun.
*/
#define SHELL_TOOL_INDEX_ROOM 256

static shell_name_slot shell_tool_index[SHELL_TOOL_INDEX_ROOM];
_Static_assert(SHELL_TOOLS < SHELL_TOOL_INDEX_ROOM,
               "the tool index needs a free slot for every tool");
static bool shell_tool_index_ready;

static positive shell_tool_find_hashed(string_address name, positive2 named)
{
        return shell_name_index_find(name, shell_tools, sizeof(shell_tools[0]),
                                     SHELL_TOOLS, shell_tool_index,
                                     SHELL_TOOL_INDEX_ROOM,
                                     address_of shell_tool_index_ready, named);
}

/*
        The last element of a path, so that /bin/grep is grep.

        What a program was called is the first thing on its stack, and for one
        binary answering to forty names it is the only thing that says which.
*/
static string_address shell_tool_name(string_address path)
{
        string_address slash;

        if (!path)
                return null;

        slash = string_last_of(path, '/');

        if (!slash)
                return path;

        if (slash[1])
                return slash + 1;

        /* Preserve the historical spelling for a trailing slash: /bin/sh/
           is called sh/, not the empty name.  The common path above remains
           one hardware-floor reverse scan. */
        slash = (string_address)memory_last_of(path, '/',
                                               (positive)(slash - path));

        return slash ? slash + 1 : path;
}

/*
        Floodlight -- what this program is allowed to do.

        The register itself is floodlight.c, a kernel module of its own that
        holds the answers and every deviation from them. This is the half that
        acts on them, and it acts here because this is the one place an applet
        is named before it has read anything: whatever the awk program says, or
        the filename find walked to, or the line that arrived on xargs' input,
        the confinement is already on by the time that data exists.

        Which is the whole point. The applets that matter are the ones whose
        behaviour is driven by what they read; fixing their identity before the
        reading starts is what closes them.
*/

#define FLOODLIGHT_PATH "/dev/floodlight"

/*
        What is refused when the register cannot be read.

        Not "everything", which would refuse the machine, and not "nothing",
        which would mean removing the device is a way of removing the policy.
        These are the three floodlight.c is built refusing -- awk, which
        builds a command from what it reads, and script and setarch, which
        start a shell -- so the absence of the register leaves the built-in
        answers standing and only the deviations unavailable. The floodlight
        harness fails the build if this list and floodlight.c's disagree.
*/
static string_address const floodlight_denied[] = {
    "awk", "script", "setarch", null};

/*
        The register, read once and reduced to what it changes.

        Reading it per applet would put four calls on a path that already costs
        forty microseconds, so it is read on the first applet a process runs
        and kept. A deviation made after that reaches the next program started,
        which is the next thing anybody runs.

        What is kept is not the report. The report is mostly the built-in
        answers, and this shell already carries those; only the rows that
        deviate from them say anything it does not already know. So the text is
        walked once, at load, and what comes out is a handful of rows -- none
        at all on a machine nobody has changed, which is every machine most of
        the time. An applet then costs three comparisons against a count of
        zero rather than three walks over eight kilobytes of text.
*/
#define FLOODLIGHT_REPORT 8192
#define FLOODLIGHT_NAME 64
#define FLOODLIGHT_DETAIL 32
#define FLOODLIGHT_ROWS 48

/* The settings, in the order floodlight.c names them. */
#define FLOODLIGHT_RUN 0
#define FLOODLIGHT_FLAG 1
#define FLOODLIGHT_SPAWN 2
#define FLOODLIGHT_NETWORK 3
#define FLOODLIGHT_SETTINGS 4

static string_address const floodlight_settings[FLOODLIGHT_SETTINGS] = {
    "run", "flag", "spawn", "network"};

typedef struct
{
        p8 subject[FLOODLIGHT_NAME];
        p8 detail[FLOODLIGHT_DETAIL];
        p8 setting;
        p8 allowed;
} floodlight_row;

static floodlight_row floodlight_rows[FLOODLIGHT_ROWS];
static positive floodlight_row_count;
static bool floodlight_report_read;

/*
        One word of a report line.

        With its length, because the report is one string and a word in it is
        not terminated: string_length on a word runs to the end of the whole
        report, so every comparison against a setting name failed and this
        shell read no deviation at all. Nothing noticed, because the reader had
        only ever been checked by reading it.
*/
typedef struct
{
        string_address at;
        positive length;
} floodlight_token;

static floodlight_token floodlight_word(string_address address_to at)
{
        floodlight_token word;
        string_address start = address_to at;

        while (address_to start == ' ')
                start++;

        address_to at = start;
        while (address_to(address_to at) && address_to(address_to at) != ' ' &&
               address_to(address_to at) != '\n')
                (address_to at)++;

        word.at = start;
        word.length = (positive)((address_to at) - start);
        return word;
}

static bool floodlight_is(floodlight_token word, string_address name)
{
        positive named = string_length(name);

        return word.length == named && !memory_compare(word.at, name, named);
}

/* Everything the register says that this shell does not already know. */
static fn floodlight_take(string_address text)
{
        string_address at = text;

        while (address_to at && floodlight_row_count < FLOODLIGHT_ROWS)
        {
                floodlight_token subject, said, state, detail = {null, 0};
                floodlight_row address_to row;
                positive i;

                if (address_to at == '#')
                        goto line;

                subject = floodlight_word(&at);
                said = floodlight_word(&at);
                state = floodlight_word(&at);

                if (!subject.length || !said.length || !state.length)
                        goto line;

                for (i = 0; i < FLOODLIGHT_SETTINGS; i++)
                        if (floodlight_is(said, floodlight_settings[i]))
                                break;

                if (i == FLOODLIGHT_SETTINGS)
                        goto line;

                /* A flag row carries the flag between the setting and the
                   state, so the state is one word further along. */
                if (i == FLOODLIGHT_FLAG)
                {
                        detail = state;
                        state = floodlight_word(&at);
                        if (!state.length)
                                goto line;
                }

                /*
                        Only what deviates. A row this kernel was built with
                        says what this shell already believes, and keeping it
                        would be carrying the same answer twice.
                */
                if (!floodlight_is(floodlight_word(&at), "changed"))
                        goto line;

                if (subject.length >= FLOODLIGHT_NAME ||
                    detail.length >= FLOODLIGHT_DETAIL)
                        goto line;

                row = address_of floodlight_rows[floodlight_row_count++];
                memory_copy_apart(row->subject, subject.at, subject.length);
                row->subject[subject.length] = 0;
                if (detail.length)
                        memory_copy_apart(row->detail, detail.at, detail.length);
                row->detail[detail.length] = 0;
                row->setting = (p8)i;
                row->allowed = (p8)floodlight_is(state, "allow");

        line:
                while (address_to at && address_to at != '\n')
                        at++;
                at += address_to at == '\n';
        }
}

static fn floodlight_load()
{
        p8 report[FLOODLIGHT_REPORT];
        file_facts facts;
        bipolar handle;
        bipolar got;

        if (floodlight_report_read)
                return;

        floodlight_report_read = true;

        handle = system_open_at(AT_FDCWD, FLOODLIGHT_PATH, FILE_READ);

        if (handle < 0)
                return;

        /*
                The register, and not something wearing its name.

                A program that can put a filesystem over /dev -- an unprivileged
                user namespace is enough on a kernel that allows them -- could
                leave an ordinary file at this path saying every applet is
                allowed everything, and be believed. The device is a character
                device on the misc major; a regular file is not, and neither is
                a pipe somebody left there. Asked of the open handle rather
                than the path, so nothing can be swapped between the two.
        */
        if (!file_look(handle, (string_address)"", AT_EMPTY_PATH, &facts) ||
            (facts.mode & MODE_FORMAT) != MODE_CHARACTER ||
            facts.rdev_major != 10)
        {
                system_close(handle);
                return;
        }

        got = system_read_once(handle, report, sizeof(report) - 1);
        system_close(handle);

        if (got <= 0)
                return;

        /*
                A report that filled the buffer is one that may have been cut,
                and half a report is worse than none: the half that is missing
                is the half that refuses something. Thrown away, so the
                built-in answers stand.
        */
        if ((positive)got >= sizeof(report) - 1)
                return;

        report[got] = 0;
        floodlight_take((string_address)report);
}

/*
        What the register says about one program and one setting, if anything.

        A count of zero is every machine nobody has changed, and the whole of
        the work there is the compare that finds it.
*/
static bool floodlight_says(string_address name, positive setting,
                            string_address detail, bool address_to answer)
{
        positive i;

        floodlight_load();

        if (!floodlight_row_count)
                return false;

        for (i = 0; i < floodlight_row_count; i++)
        {
                floodlight_row address_to row = address_of floodlight_rows[i];
                floodlight_token held;

                if (row->setting != setting)
                        continue;

                held.at = (string_address)row->subject;
                held.length = string_length((string_address)row->subject);

                if (!floodlight_is(held, name))
                        continue;

                if (setting == FLOODLIGHT_FLAG)
                {
                        held.at = (string_address)row->detail;
                        held.length = string_length((string_address)row->detail);

                        if (!floodlight_is(held, detail))
                                continue;
                }

                *answer = row->allowed != 0;
                return true;
        }

        return false;
}

/* The built-in answer this shell carries, for when the register is silent. */
static bool floodlight_built_in(string_address name)
{
        positive i;

        for (i = 0; floodlight_denied[i]; i++)
                if (word_is(name, floodlight_denied[i]))
                        return false;

        return true;
}

static bool floodlight_may(string_address name, positive setting,
                           bool otherwise)
{
        bool answer;

        return floodlight_says(name, setting, (string_address)"", &answer)
                   ? answer
                   : otherwise;
}

/*
        A filter that refuses one thing, installed on this process for good.

        Classic BPF, which is what seccomp takes: check the architecture the
        call arrived on, then the call number, and answer. The architecture
        check is not decoration -- without it a process could make the same
        call through a different ABI and arrive at a number that means
        something else entirely.

        SECCOMP_RET_ERRNO rather than killing: a refused exec should look to
        the program like a refused exec, so find says it could not run the
        command and carries on walking rather than dying halfway.
*/
#define BPF_LOAD_WORD 0x20
#define BPF_JUMP_EQUAL 0x15
#define BPF_RETURN 0x06
#define SECCOMP_DATA_NR 0
#define SECCOMP_DATA_ARCH 4
#define SECCOMP_RET_ERRNO_EPERM 0x00050001u
#define SECCOMP_RET_ALLOW 0x7fff0000u
#define SECCOMP_SET_MODE_FILTER 1
#define PR_SET_NO_NEW_PRIVS 38

#if defined(__x86_64__)
#define FLOODLIGHT_AUDIT_ARCH 0xc000003eu
#elif defined(__aarch64__)
#define FLOODLIGHT_AUDIT_ARCH 0xc00000b7u
#else
#define FLOODLIGHT_AUDIT_ARCH 0xc00000f3u
#endif

/* The kernel's own shapes: struct sock_filter and struct sock_fprog. Both
   sixteen-bit fields are unsigned there, and a signed short here would be a
   different structure that happens to be the same size. */
typedef struct
{
        p16 code;
        p8 jt;
        p8 jf;
        p32 k;
} floodlight_instruction;

typedef struct
{
        p16 count;
        floodlight_instruction address_to filter;
} floodlight_program;

#define FLOODLIGHT_REFUSED 8

static fn floodlight_confine(const p32 address_to numbers, positive count)
{
        floodlight_instruction filter[6 + FLOODLIGHT_REFUSED];
        floodlight_program program;
        positive at = 0;
        positive i;

        if (!count)
                return;

        /* Clamped before the jumps are worked out, not while they are being
           written: every jump below is measured from `count`, so a count the
           loop quietly truncated would leave every one of them pointing past
           the end of the filter. */
        if (count > FLOODLIGHT_REFUSED)
                count = FLOODLIGHT_REFUSED;

        /* Arrived on the architecture this filter was written for, or refused
           outright: a call through another ABI reaches a different table. */
        filter[at++] = (floodlight_instruction){BPF_LOAD_WORD, 0, 0, SECCOMP_DATA_ARCH};
        filter[at++] = (floodlight_instruction){BPF_JUMP_EQUAL, 1, 0, FLOODLIGHT_AUDIT_ARCH};
        filter[at++] = (floodlight_instruction){BPF_RETURN, 0, 0, SECCOMP_RET_ERRNO_EPERM};

        filter[at++] = (floodlight_instruction){BPF_LOAD_WORD, 0, 0, SECCOMP_DATA_NR};

        for (i = 0; i < count; i++)
                filter[at++] = (floodlight_instruction){
                    BPF_JUMP_EQUAL, (p8)(count - i), 0, numbers[i]};

        filter[at++] = (floodlight_instruction){BPF_RETURN, 0, 0, SECCOMP_RET_ALLOW};
        filter[at++] = (floodlight_instruction){BPF_RETURN, 0, 0, SECCOMP_RET_ERRNO_EPERM};

        program.count = (p16)at;
        program.filter = filter;

        /* Without this a filter needs privilege to install. With it the
           kernel also refuses to grant any through this process's execs,
           which is the property that makes the filter worth installing. */
        if (system_call_5(syscall(prctl), PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
                return;

        system_call_3(syscall(seccomp), SECCOMP_SET_MODE_FILTER, 0,
                      (positive)address_of program);
}

/*
        Whether this applet has to be confined, asked before the fork that
        would make confining it safe.

        The shell runs the last command of a -c in its own process rather than
        forking for it, which is right for a builtin and wrong for one that is
        about to have a filter locked onto it for good: the filter would outlive
        the applet and take the shell's own exec with it, including whatever an
        EXIT trap was going to run. So an applet that needs confining does not
        take that path.
*/
static bool floodlight_confines(string_address name)
{
        return !floodlight_may(name, FLOODLIGHT_SPAWN, floodlight_built_in(name)) ||
               !floodlight_may(name, FLOODLIGHT_NETWORK, true);
}

/* Everything the register refuses this applet, as one filter. */
static fn floodlight_apply(string_address name)
{
        p32 refused[FLOODLIGHT_REFUSED];
        positive count = 0;

        if (!floodlight_may(name, FLOODLIGHT_SPAWN, floodlight_built_in(name)))
        {
                refused[count++] = (p32)syscall(execve);
                refused[count++] = (p32)syscall(execveat);
        }

        if (!floodlight_may(name, FLOODLIGHT_NETWORK, true))
        {
                refused[count++] = (p32)syscall(socket);
                refused[count++] = (p32)syscall(connect);
        }

        floodlight_confine(refused, count);
}

/*
        Running one applet.

        `own_process` says this process exists to run this applet and nothing
        after it, which is what makes it safe to lock a seccomp filter onto.
        It is asked for rather than assumed, because a filter cannot be taken
        off again: the build tool includes this file and runs uname, mkdir and
        find inside its own process, and confining find there left the tool
        unable to exec the compiler it was about to run -- a build that failed
        with no error, because a refused exec says EPERM and prints nothing.

        So the answer is opt-in. A caller that says nothing gets no filter, and
        the callers that say yes are the three that exit immediately after.
*/
static b32 shell_tool_call_in(positive which, bool own_process)
{
        string_address name = shell_tools[which].name;
        b32 answered;

        /* Refused outright, before it runs at all. Safe everywhere: it stops
           the applet rather than changing what this process may do later. */
        if (!floodlight_may(name, FLOODLIGHT_RUN, true))
                return string_report(log_error, 126,
                                     "%s: refused by floodlight\n", name);

        /* And confined, before it reads the data that would drive it. */
        if (own_process)
                floodlight_apply(name);

        log_failure_reset();
        answered = shell_tools[which].function() & 0xff;
        log_flush();

        if (log_failed() && !answered)
                answered = 1;

        return answered;
}

/* The ordinary way in: this process goes on to do other things. */
static b32 shell_tool_call(positive which)
{
        return shell_tool_call_in(which, false);
}

/*
        The one name in the table, found without reading the rest of them.

        string_table_find compares the name against every entry, and each
        comparison is a read of a string somewhere else in the image. This
        asks the two byte key first and only follows the pointer when the key
        matches, which for a name that is not a tool's -- the ordinary case,
        since a shell is what this binary usually is -- means the table is
        walked without leaving this array at all.

        The length is taken once. A name longer than a byte can hold is not
        any tool's, and stopping on it here keeps the comparison below from
        having to describe what it would mean.
*/
static positive shell_tool_key_find(string_address name)
{
        positive length = string_length(name);
        p8 first = (p8)name[0];

        if (length > 255)
                return SHELL_TOOLS;

        for (positive at = 0; at < SHELL_TOOLS; at++)
        {
                if (shell_tool_key[at][0] != first ||
                    shell_tool_key[at][1] != (p8)length)
                        continue;

                /* The key already agreed about the first byte and the
                   length, so the terminator is what the length says it is
                   and comparing it again would prove nothing. */
                if (!memory_compare(name, shell_tools[at].name, length))
                        return at;
        }

        return SHELL_TOOLS;
}

/*
        Run as the tool the binary was called as, if it was called as one.

        Returns what it answered, or -1 when the name is not a tool's and this
        is an ordinary shell after all. Nothing is forked: this process is the
        invocation, and it is about to end.
*/
/*
        The applet a name asks for, run.

        own_process is the caller saying this process exists to run this and
        nothing after it, which is what makes it safe to lock a seccomp filter
        on. Both callers reach the same table and only one of them is finished
        afterwards: the shell's own main is here because the binary was invoked
        under an applet's name, while the build tool sets an argument vector,
        asks for a tool, and then carries on to run the compiler. Confining the
        second left the build unable to exec anything, and a refused exec says
        EPERM and prints nothing -- so the build failed with no error at all.
*/
static b32 shell_tool_named_in(string_address name, bool own_process)
{
        positive which;

        if (!name)
                return -1;

        /* One lookup in a process is cheaper than constructing the reusable
           index. Ordinary shell dispatch below is where repeated names use
           the index; argv[0] is asked only once.

           sh, shell, bash, dash and moonwater used to be spelled out here
           and rejected before the walk, because the walk compared the name
           against every tool's and each comparison read a string somewhere
           else in the image. It reads two bytes out of one array now, so
           skipping it saved a microsecond of nothing -- measured at 130 us a
           run against 129 without -- and none of those five is a tool, so
           the shortcut never decided anything the table would not have. */
        which = shell_tool_key_find(name);

        if (which == SHELL_TOOLS)
                return -1;

        return shell_tool_call_in(which, own_process);
}

/* Ordinary callers keep the process afterwards, so nothing is locked on. */
static b32 shell_tool_named(string_address name)
{
        return shell_tool_named_in(name, false);
}

b32 shell_tool_as_called()
{
        return shell_tool_named(shell_tool_name(program_argument(0)));
}

fn shell_tool_list(writer write)
{
        for (positive i = 0; i < SHELL_TOOLS; i++)
                string_format(write, TERM_BOLD " -  %s" TERM_RESET "\n",
                              shell_tools[i].name);
}

static PURE bool job_monitor();
fn job_execute_tool(positive which);

static bool shell_tool_run_hashed(string_address name, positive2 named)
{
        positive which = shell_tool_find_hashed(name, named);
        bipolar child = -1;
        positive status = 0;

        if (which == SHELL_TOOLS)
                return false;

        /* The tail command runs in the shell's own process. An applet the
           register confines must not, because the filter would stay on after
           it and take the shell's own exec with it. */
        if (shell_tail_command && !floodlight_confines(name))
        {
                program_arguments_use(shell_argv, (b32)shell_argc);
                shell_answer(shell_tool_call(which));
                return true;
        }

        // Under job control this utility is a job, which needs a process
        // group the spawn device has no way to put it in.
        if (job_monitor())
        {
                job_execute_tool(which);
                return true;
        }

        // Before the fork, or the child inherits a copy of what is waiting in
        // the buffer and writes it out a second time.
        log_flush();

        /* Spark starts the immutable multicall image without copying this
           resident shell. A stock kernel takes the direct-function fork. */
        child = shell_spawn_tool(shell_argv, -1, false);

        if (child < 0)
                child = system_fork();

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
                exit(shell_tool_call_in(which, true));
        }

        if (child < 0)
        {
                shell_answer(1);
                return true;
        }

        system_wait4_retry(child, address_of status, 0, null);
        shell_answer(wait_status_code(status));

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
fn history_leaving();

fn shell_trap_exit()
{
        positive action_room = 0;
        string_address action = trap_detach(0, address_of action_room);
        b32 leaving = shell_status;

        // Every way out of an interactive shell comes through here, which is
        // the only place a history file can be written once rather than at
        // each of them.
        if (!trap_child_context)
                history_leaving();

        if (trap_exit_inherited)
        {
                if (action)
                        memory_free(action, action_room);
                return;
        }

        if (!action || !string_get(action))
        {
                if (action)
                        memory_free(action, action_room);

                return;
        }

        // Taken away first, so a trap that leaves again does not run twice.
        parse_nest_enter();
        run_lines(action);
        parse_nest_leave();
        memory_free(action, action_room);

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
static bool shell_path_wanted(string_address value, positive name_length,
                              positive address_to wanted)
{
        path_walk walk = {value, null, 0, false};
        positive longest = 0;

        while (path_walk_next(address_of walk))
                if (walk.length > longest)
                        longest = walk.length;

        if (name_length > positive_max - 2 ||
            longest > positive_max - name_length - 2)
                return false;

        *wanted = longest + name_length + 2;
        return true;
}

static positive shell_source_depth;

static bipolar shell_source_direct(string_address name)
{
        bipolar handle;

        do
                handle = system_open_at(AT_FDCWD, name, FILE_READ);
        while (handle == -4);
        return handle;
}

static bipolar shell_source_open(string_address name,
                                  p8 address_to address_to found,
                                  positive address_to found_room,
                                  bool address_to no_room)
{
        string_address value;
        path_walk walk;

        if (!name || !string_get(name))
                return -1;

        if (string_first_of(name, '/'))
                return shell_source_direct(name);

        //      sourcepath off is cwd under bash, and nothing under posix:
        //      `. p` is not found even when p is in this directory.
        if (shell_bash_compat && !shell_shopt_on(SOURCEPATH))
                return shell_posix_on() ? -1 : shell_source_direct(name);

        value = env_get("PATH");

        if (!value)
                value = "/bin:/usr/bin:/";

        {
                positive wanted;

                if (!shell_path_wanted(value, string_length(name),
                                       address_of wanted) ||
                    !shell_room((address_any address_to)found, found_room,
                                wanted, 1))
                {
                        *no_room = true;
                        return -1;
                }
        }

        walk = (path_walk){value, null, 0, false};

        while (path_walk_next(address_of walk))
        {
                bipolar handle;

                if (!path_walk_join(*found, *found_room, walk.segment,
                                    walk.length, name, ""))
                        continue;

                handle = shell_source_direct(*found);

                if (handle >= 0)
                        return handle;
        }

        /* Bash's ordinary source policy falls back to the current directory.
           POSIX mode removes that fallback; dash only visits it when PATH
           explicitly contains a current-directory field. */
        return shell_bash_compat && !shell_posix_on()
                   ? shell_source_direct(name)
                   : -1;
}

static bipolar shell_source_read(bipolar handle,
                                  p8 address_to address_to text,
                                  positive address_to room,
                                  bool address_to no_room)
{
        byte_store store = {*text, *room, 0};
        bipolar result = file_store_read((positive)handle, address_of store);

        system_close(handle);
        *text = store.bytes;
        *room = store.room;
        if (result == -12)
                *no_room = shell_memory_failed = true;
        return result < 0 ? result : (bipolar)store.used;
}

/*
        set -v: what was read, written back before anything is done with it.

        Reading, not running, is what the option is about, so the echo sits
        where the reader hands a physical line over and the option is asked
        about again for each one: `set -v` halfway through reaches the line
        after it, in a file and in bash's command string alike. dash echoes
        no command string at all, because a string was not read from
        anywhere, and that is the whole of the difference between the two.

        A sourced file is read from somewhere, so both shells echo it and
        it is declared here, beside the reader that runs one, rather than
        beside the process reader in the entry file that is included last.
*/
static bool shell_verbose_from_string;

static fn shell_verbose_line(string_address line)
{
        if (shell_verbose_from_string && !shell_bash_compat)
                return;

        if (!(shell_options & SHELL_FLAG('v')))
                return;

        log_error(line, string_length(line));
        log_error("\n", 1);
        log_flush();
}

static b32 shell_source_execute(p8 address_to text, positive filled,
                                 bool startup)
{
        lex_frame frame;
        positive at = 0;
        positive syntax = shell_syntax_generation;

        lex_nest_enter(address_of frame);
        shell_source_depth++;
        while (at < filled)
        {
                p8 address_to newline = (p8 address_to)memory_first_of(
                    text + at, '\n', filled - at);
                positive stop = newline ? (positive)(newline - text) : filled;

                text[stop] = end;
                {
                        //      A file is a file however it was reached, so
                        //      the string rule that keeps dash quiet about
                        //      a -c command does not apply to its lines.
                        bool held = shell_verbose_from_string;

                        shell_verbose_from_string = false;
                        shell_verbose_line(text + at);
                        shell_verbose_from_string = held;
                }
                run_line(text + at);
                if (shell_syntax_generation != syntax)
                        break;
                if (exec_source_stop(startup ? address_of shell_status : null))
                        break;
                at = stop + 1;
        }
        shell_input_end();
        shell_source_depth--;
        exec_input_finish();
        lex_nest_leave(address_of frame);

        b32 failed = (b32)(shell_syntax_generation - syntax);
        shell_syntax_generation = syntax;
        return failed;
}

COLD fn shell_dot(writer write, string_address input)
{
        p8 address_to found = null;
        positive found_room = 0;
        p8 address_to source_text = null;
        positive source_room = 0;
        string_address path;
        bipolar got;
        positive filled;
        bool no_room = false;
        bipolar handle;
        positive first = 1;

        if (first < shell_argc && word_is(shell_argv[first], "--"))
                first++;
        //      A word that looks like an option is one, and neither shell
        //      has any here: this used to reach open() as a filename and
        //      report that it could not be read.
        else if (first < shell_argc &&
                 string_is(shell_argv[first], '-') &&
                 string_get(shell_argv[first] + 1))
        {
                // Two bytes: the shared formatter has no %c.
                p8 room[2];
                shell_option_walk walk = {first};
                p8 option;

                if (shell_option_letter(address_of walk, address_of option))
                {
                        exec_special_error_note();

                        if (shell_letter_bad(shell_argv[0], option))
                                string_format(log_error,
                                    "%s: usage: %s [-p path] filename "
                                    "[arguments]\n", shell_argv[0],
                                    shell_argv[0]);

                        return shell_answer(2);
                }
        }
        if (first >= shell_argc)
        {
                if (shell_bash_compat)
                {
                        log_error(".: filename argument required\n", 0);
                        exec_special_error_note();
                }
                return shell_answer(shell_bash_compat ? 2 : 0);
        }

        path = shell_argv[first];

        //      rbash: a name with a slash in it reaches outside whatever PATH
        //      was left, which is the whole of what the restriction holds.
        if (shell_restricted && string_first_of(path, '/'))
        {
                shell_diagnostic_where();
                return shell_answer(string_report(log_error, 1,
                                                  ".: %s: restricted\n", path));
        }

        handle = shell_source_open(path, address_of found, address_of found_room,
                                   address_of no_room);

        if (handle >= 0)
                got = shell_source_read(handle, address_of source_text,
                                        address_of source_room,
                                        address_of no_room);
        else
                got = handle;

        if (found)
                memory_free(found, found_room);

        if (no_room)
        {
                if (source_text)
                        memory_free(source_text, source_room);

                shell_answer(string_report(log_error, 2, "%s: no room\n", shell_argv[0]));
                shell_stop_when_scripted(2);

                return;
        }

        if (got < 0)
        {
                memory_free(source_text, source_room);
                shell_diagnostic_where();

                //      Bash names the file and what the open said, without
                //      its own name in front, until posix mode puts it back
                //      and calls the file not found instead. dash names
                //      itself and says what it could not do, in the words
                //      it uses for a redirect it could not open.
                {
                        bipolar code = got < 0 ? -got : got;
                        string_address why = system_error_message(code);

                        if (!why)
                                why = (string_address)
                                    "No such file or directory";

                        //      A name with no slash in it was looked for
                        //      along PATH and not found there, which is a
                        //      different thing from a file that would not
                        //      open, and both shells say so differently.
                        bool searched = !string_first_of(path, '/');

                        if (!shell_bash_compat)
                                string_format(log_error,
                                    searched ? "%s: %s: not found\n"
                                             : "%s: cannot open %s: %s\n",
                                    shell_argv[0], path,
                                    code == ERROR_NO_ENTRY
                                        ? (string_address) "No such file"
                                        : why);
                        else if (shell_posix_on() && searched)
                                string_format(log_error,
                                    "%s: %s: file not found\n",
                                    shell_argv[0], path);
                        else
                                string_format(log_error, "%s: %s\n", path,
                                              why);
                }

                /* The executor applies special-builtin fatality only to a
                   direct invocation. That distinction is essential here:
                   `command . missing` must report failure and continue. */
                exec_special_error_note();
                return shell_answer(shell_bash_compat ? 1 : 2);
        }

        filled = (positive)got;

        /*
                Operands after the name are the file's positional parameters
                and only the file's.

                A sourced file is not a function and keeps the caller's
                variables, but $1 inside it is what the caller wrote after the
                name -- and with nothing written the caller's own $1 stays,
                which is what a file sourced for its definitions is reading.
        */
        positive held_count = shell_parameter_count;
        positive held = EXPAND_NO_ROOM;

        if (shell_bash_compat && shell_argc > first + 1)
        {
                held = shell_parameters_save();

                if (held == EXPAND_NO_ROOM ||
                    !shell_parameters_restore_prepare(held_count) ||
                    !shell_parameters_set(shell_argv + first + 1,
                                          shell_argc - first - 1))
                {
                        if (held != EXPAND_NO_ROOM)
                                shell_parameter_stack_used = held;

                        memory_free(source_text, source_room);
                        return shell_answer(string_report(log_error, 2, "source: no room for arguments\n"));
                }
        }

        bool replaced_arguments = held != EXPAND_NO_ROOM;
        if (replaced_arguments)
                shell_parameters_replaced = false;
        b32 syntax = 0;

        /* The name as written: argv will be overwritten by commands inside
           the file, and bash's diagnostic $0 is this path, not the caller's. */
        {
                p8 address_to named = null;
                positive named_room = 0;
                positive named_length = string_length(path);
                string_address saved_file = shell_syntax_file;
                string_address saved_command = shell_syntax_command;

                if (!shell_array_room(named, named_room, named_length + 1))
                {
                        memory_free(source_text, source_room);
                        if (held != EXPAND_NO_ROOM)
                                shell_parameter_stack_used = held;
                        return shell_answer(string_report(log_error, 2,
                                                          "%s: no room\n",
                                                          shell_argv[0]));
                }

                memory_copy(named, path, named_length);
                named[named_length] = end;

                if (shell_bash_compat)
                {
                        shell_syntax_file = named;
                        shell_syntax_command = null;
                }
                else
                        shell_syntax_command = named;

                syntax = shell_source_execute(source_text, filled, false);

                shell_syntax_file = saved_file;
                shell_syntax_command = saved_command;
                memory_free(named, named_room);
        }

        memory_free(source_text, source_room);

        if (held != EXPAND_NO_ROOM && shell_parameters_replaced)
        {
                // Discard the saved caller copy, retaining the explicit set.
                shell_parameter_stack_used = held;
                held = EXPAND_NO_ROOM;
        }
        if (held != EXPAND_NO_ROOM &&
            !shell_parameters_restore(held, held_count))
        {
                log_error(str("source: no room to restore arguments\n"));
                log_flush();
                exit(2);
        }
        // A source with operands consumes the replacement marker at its own
        // boundary. Without operands, it shares the caller's parameter state.
        if (replaced_arguments)
                shell_parameters_replaced = false;

        if (syntax && (!shell_bash_compat ||
                       (!shell_command_reader_depth && syntax != 1)))
                exec_special_error_note();

        // The status of the last line it ran, which is already there.
        shell_answer(shell_status);
}

/* A background job remains known after the kernel has reaped it. POSIX lets a
   later wait recover that status, then requires the successful wait to forget
   it. A pipeline has several waitable children but one public identity: the
   PID of its last command. Keeping one flat row per child avoids a second
   allocation and lets the WNOHANG reaper record any stage directly. */
typedef struct
{
        bipolar job;
        bipolar pid;
        positive status;
        positive flags;
} shell_wait_entry;

static shell_wait_entry address_to shell_wait_table;
static positive shell_wait_room;
static positive shell_wait_count;

#define SHELL_WAIT_DONE 1
#define SHELL_WAIT_LAST 2
#define SHELL_WAIT_PIPEFAIL 4
#define SHELL_WAIT_INVERT 8

static PURE positive shell_wait_find_job(bipolar job)
{
        for (positive at = 0; at < shell_wait_count; at++)
                if (shell_wait_table[at].job == job)
                        return at;

        return shell_wait_count;
}

static positive shell_wait_find_child(bipolar pid)
{
        positive at = shell_wait_count;

        /* A reaped, unconsumed stage no longer reserves its numeric PID in
           the kernel. If it is reused, the newest live row owns the new wait
           result. */
        while (at)
        {
                at--;
                if (shell_wait_table[at].pid == pid &&
                    !(shell_wait_table[at].flags & SHELL_WAIT_DONE))
                        return at;
        }

        return shell_wait_count;
}

static fn shell_wait_drop(bipolar job)
{
        positive into = 0;

        for (positive at = 0; at < shell_wait_count; at++)
                if (shell_wait_table[at].job != job)
                        shell_wait_table[into++] = shell_wait_table[at];

        shell_wait_count = into;
}

bool shell_background_started(bipolar address_to children, positive count,
                              bool pipefail, bool invert)
{
        positive flags = (pipefail ? SHELL_WAIT_PIPEFAIL : 0) |
                         (invert ? SHELL_WAIT_INVERT : 0);
        bipolar job;

        if (!count || children[count - 1] <= 0)
                return false;

        job = children[count - 1];
        shell_background_last = job;

        /* A PID can be reused once its old process was reaped. Its new job is
           the identity POSIX makes available, so an unconsumed old result may
           no longer occupy that name. */
        shell_wait_drop(job);

        if (count > positive_max - shell_wait_count ||
            !shell_array_room(shell_wait_table, shell_wait_room, shell_wait_count + count))
                return false;

        for (positive at = 0; at < count; at++)
        {
                shell_wait_entry address_to entry =
                    shell_wait_table + shell_wait_count++;

                entry->job = job;
                entry->pid = children[at];
                entry->status = 0;
                entry->flags = flags |
                               (at + 1 == count ? SHELL_WAIT_LAST : 0);
        }

        return true;
}

fn shell_background_child()
{
        /* $! is part of the inherited shell environment. What a subshell
           cannot inherit is the parent's right to wait for those children. */
        shell_wait_count = 0;
}

static fn shell_background_reaped(bipolar pid, positive status)
{
        positive at = shell_wait_find_child(pid);

        // Here-document writers are children too, but never asynchronous jobs.
        if (at < shell_wait_count)
        {
                shell_wait_table[at].status = status;
                shell_wait_table[at].flags |= SHELL_WAIT_DONE;
        }
}

static bipolar shell_wait_call(bipolar pid, positive address_to status)
{
        bipolar got;

        trap_wait_restarting(false);
        got = system_call_4(syscall(wait4), pid, (positive)status, 0, 0);
        trap_wait_restarting(true);

        return got;
}

static b32 shell_wait_one(bipolar job, bool address_to interrupted)
{
        positive first = shell_wait_find_job(job);
        b32 status = 0;
        b32 rightmost_failure = 0;
        positive job_flags;

        address_to interrupted = false;

        if (first >= shell_wait_count)
                return 127;

        job_flags = shell_wait_table[first].flags;

        for (positive at = first; at < shell_wait_count; at++)
        {
                shell_wait_entry address_to entry = shell_wait_table + at;
                positive raw;
                bipolar got;
                b32 code;

                if (entry->job != job)
                        continue;

                if (!(entry->flags & SHELL_WAIT_DONE))
                {
                        do
                                got = shell_wait_call(entry->pid,
                                                      address_of raw);
                        while (got == -4 && !trap_waiting());

                        if (got == -4 && trap_waiting())
                        {
                                bipolar signal = trap_pending_number();

                                address_to interrupted = true;
                                return signal > 0 ? 128 + (b32)signal : 129;
                        }

                        if (got < 0)
                        {
                                shell_wait_drop(job);
                                return 127;
                        }

                        entry->status = raw;
                        entry->flags |= SHELL_WAIT_DONE;
                }

                code = wait_status_code(entry->status);
                if (code)
                        rightmost_failure = code;
                if (entry->flags & SHELL_WAIT_LAST)
                        status = code;
        }

        if ((job_flags & SHELL_WAIT_PIPEFAIL) && rightmost_failure)
                status = rightmost_failure;
        if (job_flags & SHELL_WAIT_INVERT)
                status = status ? 0 : 1;

        shell_wait_drop(job);
        return status;
}

/* wait: all known asynchronous children, or every PID operand in order. Job
   identifiers deliberately remain unsupported until the shell owns process
   groups and a controlling terminal; accepting %1 here would be a lie. */
COLD fn shell_wait(writer write, string_address input)
{
        b32 answer = 0;

        if (shell_argc < 2)
        {
                while (shell_wait_count)
                {
                        bool interrupted;

                        answer = shell_wait_one(shell_wait_table[0].job,
                                                address_of interrupted);
                        if (interrupted)
                                return shell_answer(answer);
                }

                return shell_answer(0);
        }

        for (positive at = 1; at < shell_argc; at++)
        {
                positive pid;
                bool interrupted;

                if (!string_digits_checked_exact(shell_argv[at], 10, address_of pid) ||
                    pid > (positive)b32_max)
                        return shell_answer(string_report(log_error, 2, "wait: Illegal number: %s\n",
                                      shell_argv[at]));

                answer = shell_wait_one((bipolar)pid,
                                        address_of interrupted);

                if (interrupted)
                        break;
        }

        shell_answer(answer);
}

COLD fn shell_jobs(writer write, string_address input);
fn shell_history(writer write, string_address input);
fn shell_fc(writer write, string_address input);
fn shell_fg(writer write, string_address input);
fn shell_bg(writer write, string_address input);
fn shell_disown(writer write, string_address input);
fn shell_suspend(writer write, string_address input);
fn shell_kill(writer write, string_address input);
fn job_wait(writer write, string_address input);
// The executor keeps the line a command was written on; $LINENO reads it.
PURE positive shell_line_now();

// caller reads the call frames, which live beside the executor.
fn shell_caller(writer write, string_address input);
fn shell_help(writer write, string_address input);
COLD fn shell_builtin_run(writer write, string_address input);
COLD fn shell_compgen(writer write, string_address input);
COLD fn shell_enable(writer write, string_address input);
COLD fn shell_which(writer write, string_address input);
fn shell_type(writer write, string_address input);
COLD fn shell_command_builtin(writer write, string_address input);
COLD fn shell_hash(writer write, string_address input);
COLD fn shell_ulimit(writer write, string_address input);

/*
        Bash's `let` is the command-shaped spelling of the arithmetic engine
        already used by (( ... )). Each operand is one expression, evaluated
        left to right, and the command answers for the value of the last one.
        Keeping this as a thin builtin avoids a second arithmetic grammar and
        gives assignments, increments and overflow exactly the same rules.
*/
COLD fn shell_let(writer write, string_address input)
{
        bipolar value = 0;
        positive at;

        (void)write;
        (void)input;

        if (shell_argc < 2)
                return shell_answer(1);

        at = 1;

        //      Bash's let reads a leading "--" as the end of its options.
        //      Handed to the arithmetic engine it is a decrement with
        //      nothing to decrement, which is not what was asked.
        if (word_is(shell_argv[at], "--"))
                at++;

        if (at >= shell_argc)
                return shell_answer(1);

        for (; at < shell_argc; at++)
                if (!exec_arithmetic_value(shell_argv[at], address_of value,
                                           "let"))
                        return shell_answer(exec_line_aborted() ? 2 : 1);

        shell_answer(value ? 0 : 1);
}

typedef fn(address_to shell_command_function)(writer write, string_address input);

typedef struct
{
        string_address name;
        shell_command_function function;
} shell_command;

/*
        complete, compopt and bind: taken, and doing almost nothing.

        Programmable completion needs a terminal and a reader that offers it,
        and this shell's line editor has neither. What these three can still
        do exactly is read their own words: an option none of them has is
        refused with the status a usage error carries, and bind says on every
        call what Bash says -- that there is no line editing to bind to.
        Everything past that is the completion machinery itself, which is not
        here and is pinned as absent rather than answered wrongly.
*/
static bool shell_completion_refused(string_address command,
                                     string_address usage,
                                     string_address letters,
                                     string_address valued,
                                     positive address_to first)
{
        // Two bytes: the shared formatter has no %c.
        p8 room[2];
        //      Only compopt reads a + word as options: to complete and bind
        //      a "+Z" is a name, and bash takes it for one without a word
        //      of complaint.
        shell_option_walk walk = {1, null, 0, word_is(command, "compopt")};
        p8 option;

        while (shell_option_letter(address_of walk, address_of option))
        {
                if (!string_first_of(letters, option))
                {
                        //      The sign the letter came under, not a dash
                        //      every time: compopt reads +o as readily as
                        //      -o and names back what it was given.
                        p8 said[3] = {walk.direction ? walk.direction : '-',
                                      option, end};

                        shell_answer(shell_option_refused(command, said,
                                                          usage));
                        return true;
                }

                if (string_first_of(valued, option) &&
                    !shell_option_argument(address_of walk))
                {
                        shell_diagnostic_where();
                        string_report(log_error, 2,
                                      "%s: -%s: option requires an argument\n",
                                      command,
                                      shell_option_spelled(room, option));
                        shell_answer(string_report(log_error, 2, "%s: usage: %s\n",
                                                   command, usage));
                        return true;
                }
        }

        address_to first = walk.index;
        return false;
}

static COLD fn shell_complete(writer write, string_address input)
{
        positive first = 1;

        (void)write;
        (void)input;

        if (shell_completion_refused(
                "complete",
                "complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] [-A action] "
                "[-G globpat] [-W wordlist] [-F function] [-C command] "
                "[-X filterpat] [-P prefix] [-S suffix] [name ...]",
                "abcdefgjksuvprDEIoAGWFCXPS", "oAGWFCXPS", address_of first))
                return;

        //      -o names one of a closed list, and Bash refuses a word that
        //      is not on it before it looks at anything else.
        for (positive at = 1; at + 1 < shell_argc; at++)
                if (word_is(shell_argv[at], "-o") ||
                    word_is(shell_argv[at], "+o"))
                {
                        string_address named = shell_argv[at + 1];

                        if (!word_is(named, "bashdefault") &&
                            !word_is(named, "default") &&
                            !word_is(named, "dirnames") &&
                            !word_is(named, "filenames") &&
                            !word_is(named, "noquote") &&
                            !word_is(named, "nosort") &&
                            !word_is(named, "nospace") &&
                            !word_is(named, "plusdirs"))
                                return shell_answer(string_report(
                                    log_error, 2,
                                    "complete: %s: invalid option name\n",
                                    named));
                }

        shell_answer(0);
}

static COLD fn shell_compopt(writer write, string_address input)
{
        positive first = 1;

        (void)write;
        (void)input;

        if (shell_completion_refused("compopt",
                                     "compopt [-o|+o option] [-DEI] [name ...]",
                                     "oDEI", "o", address_of first))
                return;

        //      No completion is being executed and no name has a
        //      specification, which is the pair of things Bash says here.
        if (first < shell_argc)
        {
                //      One line per name: bash walks them all and answers
                //      one at the end.
                while (first < shell_argc)
                {
                        shell_diagnostic_where();
                        string_format(log_error,
                            "compopt: %s: no completion specification\n",
                            shell_argv[first++]);
                }

                return shell_answer(1);
        }

        shell_answer(string_report(
            log_error, 1,
            "compopt: not currently executing completion function\n"));
}

static COLD fn shell_bind(writer write, string_address input)
{
        positive first = 1;

        (void)write;
        (void)input;

        //      Bash says this once per call before it does anything else,
        //      whatever the words are, when readline was never started.
        shell_diagnostic_where();
        log_error("bind: warning: line editing not enabled\n", 0);

        if (shell_completion_refused(
                "bind",
                "bind [-lpsvPSVX] [-m keymap] [-f filename] [-q name] "
                "[-u name] [-r keyseq] [-x keyseq:shell-command] "
                "[keyseq:readline-function or readline-command]",
                "lpsvPSVXmfqurx", "mfqurx", address_of first))
                return;

        shell_answer(0);
}

shell_command shell_commands[] = {
    //      Byte order, because `enable` writes this list as it
    //      stands and the reference writes a sorted one: '.' is
    //      0x2e and ':' is 0x3a, so the two of them led it wrong.
    {".", shell_dot},
    {":", shell_true},
    {"[", shell_test},
    {"alias", shell_alias},
    {"bg", shell_bg},
    {"bind", shell_bind},
    {"blkid", shell_blkid},
    {"break", shell_break},
    {"builtin", shell_builtin_run},
    {"caller", shell_caller},
    {"cd", shell_cd},
    {"clear", shell_clear},
    {"command", shell_command_builtin},
    {"compgen", shell_compgen},
    {"complete", shell_complete},
    {"compopt", shell_compopt},
    {"continue", shell_break},
    {"declare", shell_declare},
    {"dirs", shell_dirs},
    {"disown", shell_disown},
    {"echo", shell_echo},
    {"enable", shell_enable},
    {"eval", shell_eval},
    {"exec", shell_exec},
    {"exit", shell_exit},
    {"export", shell_export},
    {"false", shell_false},
    {"fc", shell_fc},
    {"fg", shell_fg},
    {"findfs", shell_findfs},
    {"findmnt", shell_findmnt},
    {"getopts", shell_getopts},
    {"hash", shell_hash},
    {"help", shell_help},
    {"history", shell_history},
    {"jobs", shell_jobs},
    {"kill", shell_kill},
    {"let", shell_let},
    {"local", shell_local},
    {"logout", shell_logout},
    {"mapfile", shell_mapfile},
    {"mount", shell_mount},
    {"mountpoint", shell_mountpoint},
    {"popd", shell_popd},
    {"poweroff", shell_poweroff},
    {"printf", shell_printf},
    {"pushd", shell_pushd},
    {"pwd", shell_pwd},
    {"read", shell_read},
    {"readarray", shell_mapfile},
    {"readonly", shell_readonly},
    {"reboot", shell_reboot},
    {"return", shell_return},
    {"set", shell_set},
    {"shift", shell_shift},
    {"shopt", shell_shopt},
    {"source", shell_dot},
    {"suspend", shell_suspend},
    {"test", shell_test},
    {"times", shell_times},
    {"trap", shell_trap},
    {"true", shell_true},
    {"type", shell_type},
    {"typeset", shell_declare},
    {"ulimit", shell_ulimit},
    {"umask", shell_umask},
    {"umount", shell_umount},
    {"unalias", shell_unalias},
    {"unset", shell_unset},
    {"wait", job_wait},
    {"which", shell_which},
    {null, null},
};

#define SHELL_COMMAND_COUNT ((array_count(shell_commands)) - 1)
#define SHELL_COMMAND_INDEX_ROOM 128

static shell_name_slot shell_command_index[SHELL_COMMAND_INDEX_ROOM];
_Static_assert(SHELL_COMMAND_COUNT < SHELL_COMMAND_INDEX_ROOM,
               "the command index needs a free slot for every builtin");
static bool shell_command_index_ready;

/* Disabled state follows registry identity, so aliases remain independent
   and repeated enable/disable cycles need no copied names or capacity limit. */
static bool shell_disabled[SHELL_COMMAND_COUNT];
static positive shell_disabled_count;

static positive shell_command_index_hashed(string_address name,
                                            positive2 named)
{
        return shell_name_index_find(
            name, shell_commands, sizeof(shell_commands[0]),
            SHELL_COMMAND_COUNT, shell_command_index,
            SHELL_COMMAND_INDEX_ROOM, address_of shell_command_index_ready,
            named);
}

static inline INLINE bool shell_builtin_disabled(string_address name)
{
        if (!shell_disabled_count)
                return false;
        positive which = shell_command_index_hashed(
            name, string_hash_33_length(name));
        return which < SHELL_COMMAND_COUNT && shell_disabled[which];
}

static shell_command address_to shell_command_named_hashed(string_address name,
                                                           positive2 named)
{
        positive which = shell_command_index_hashed(name, named);
        return which < SHELL_COMMAND_COUNT && !shell_disabled[which]
                   ? shell_commands + which : null;
}

bool shell_tool_only_here(string_address name, positive2 named)
{
        return shell_tool_find_hashed(name, named) != SHELL_TOOLS &&
               !shell_command_named_hashed(name, named);
}

/* Control builtins live in the executor because their result unwinds C
   frames, but command/type must report the same builtin namespace. */
static bool shell_command_builtin_here(string_address name, positive2 named)
{
        return shell_command_named_hashed(name, named) ||
               shell_tool_find_hashed(name, named) != SHELL_TOOLS ||
               (!shell_builtin_disabled(name) &&
                exec_control_builtin(name, false));
}

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

PURE string_address hash_find(string_address name)
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

//      Take one name out of the table. The bytes it owned stay where they
//      are: the store is filled forwards and a gap in it is cheaper than the
//      walk that would close it.
static bool hash_drop(string_address name)
{
        positive at = string_table_find(name, hash_name, sizeof(hash_name[0]),
                                        hash_count);

        if (at >= hash_count)
                return false;

        memory_copy(hash_name + at, hash_name + at + 1,
                    (hash_count - at - 1) * sizeof(hash_name[0]));
        memory_copy(hash_path + at, hash_path + at + 1,
                    (hash_count - at - 1) * sizeof(hash_path[0]));
        hash_count--;

        return true;
}

fn shell_hash(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        shell_option_walk walk = {1};
        p8 which;
        b32 bad = 0;
        bool as_commands = false;
        bool only_path = false;
        bool forget = false;
        bool reset = false;
        bool named_many = false;
        string_address given = null;
        p8 address_to found = null;
        positive found_room = 0;

        if (!shell_hashall_on())
                return shell_answer(string_report(log_error, 1, "hash: hashing disabled\n"));

        while (shell_option_letter(address_of walk, address_of which))
        {
                if (which == 'r')
                {
                        hash_forget();
                        reset = true;
                }
                else if (which == 'l')
                        as_commands = true;
                else if (which == 't')
                        only_path = true;
                else if (which == 'd')
                        forget = true;
                else if (which == 'p')
                {
                        given = shell_option_argument(address_of walk);
                        if (!given)
                                return shell_answer(string_report(log_error, 2, "hash: -p: option requires an "
                                                 "argument\n"));

                        //      rbash: naming a path for a command is the same
                        //      reach a slash in the name would have been.
                        if (shell_restricted)
                        {
                                shell_diagnostic_where();
                                return shell_answer(string_report(
                                    log_error, 1, "hash: %s: restricted\n",
                                    given));
                        }
                }
                else
                        return shell_answer(shell_letter_refused(
                            "hash", which,
                            "hash [-lr] [-p pathname] [-dt] [name ...]"));
        }

        positive index = walk.index;
        if (index >= shell_argc)
        {
                positive at = 0;

                //      Bash writes one sentence when the table is empty, and
                //      a script counting `hash 2>&1` counts it. That sentence
                //      is a listing, not a side-effect of -r: `hash -r` is
                //      silent, `hash -l` on an empty table is silent, and so
                //      is `set -o posix`. Dash writes nothing, which is the
                //      listing this shell keeps under a POSIX name.
                if (!hash_count)
                {
                        if (shell_bash_compat && !shell_posix_on() &&
                            !as_commands && !reset)
                                return shell_answer(string_report(
                                    log_error, 0, "hash: hash table empty\n"));
                        return shell_answer(0);
                }

                while (at < hash_count)
                {
                        if (as_commands)
                                string_format(write,
                                              "builtin hash -p %s %s\n",
                                              hash_path[at], hash_name[at]);
                        else
                                string_format(write, "%s\n", hash_path[at]);

                        at++;
                }

                return shell_answer(0);
        }

        // More than one name asked about needs saying which answer belongs
        // to which, and one does not.
        named_many = shell_argc - index > 1;

        while (index < shell_argc)
        {
                string_address name = shell_argv[index];
                bipolar located;

                // -p records what the caller already knows, without a walk.
                if (given)
                {
                        hash_drop(name);
                        hash_remember(name, given);
                        index++;
                        continue;
                }

                if (forget)
                {
                        if (!hash_drop(name))
                                bad = 1;

                        index++;
                        continue;
                }

                if (only_path)
                {
                        string_address known = hash_find(name);

                        if (known && named_many)
                                string_format(write, "%s\t%s\n", name, known);
                        else if (known)
                                string_format(write, "%s\n", known);
                        else
                        {
                                shell_diagnostic_where();
                                string_format(log_error,
                                              "hash: %s: not found\n", name);
                                bad = 1;
                        }

                        index++;
                        continue;
                }

                located = shell_find_in_path_alloc(name, address_of found,
                                                   address_of found_room);

                if (located < 0)
                {
                        bad = 2;
                        log_error(str("hash: no room\n"));
                        break;
                }

                if (located != 1)
                {
                        bad = 1;
                        shell_diagnostic_where();
                        string_format(log_error, "hash: %s: not found\n",
                                      name);
                }

                index++;
        }

        if (found)
                memory_free(found, found_room);

        shell_answer(bad);
}

/*
        Where a bare command name is actually found.

        Shared with the shell itself, which needs the same answer before it can
        run anything typed without a slash -- and had no way to ask, so every
        program had to be named by its full path.
*/
static b32 shell_find_in_path_mode(string_address name, p8 address_to into,
                                   positive room, positive access,
                                   bool use_hash, string_address value)
{
        path_walk walk;

        if (name == null || !string_get(name) || !room)
                return false;

        if (string_first_of(name, '/'))
        {
                positive name_length = string_length(name);

                if (system_access_at(AT_FDCWD, name, access))
                        return false;

                if (name_length >= room)
                        return false;

                memory_copy_end(into, name, name_length);
                return true;
        }

        {
                string_address known = use_hash && shell_hashall_on()
                                           ? hash_find(name) : null;

                if (known)
                {
                        positive known_length = string_length(known);

                        if (known_length >= room)
                                return false;

                        memory_copy_end(into, known, known_length);
                        return true;
                }
        }

        if (value == null && !(value = env_get("PATH")))
                value = "/bin:/usr/bin:/";

        walk = (path_walk){value, null, 0, false};

        while (path_walk_next(address_of walk))
        {
                if (!path_walk_join(into, room, walk.segment, walk.length,
                                    name, ""))
                        continue;

                if (system_access_at(AT_FDCWD, into, access))
                        continue;

                // Remembered only as the executor's answer: a query asks
                // with access 0 and may name a file nobody could run.
                if (use_hash && shell_hashall_on() && access == ACCESS_EXECUTE)
                        hash_remember(name, into);

                return true;
        }

        return false;
}

/*
        A complete pathname may be longer than any command buffer. Size from
        the actual inputs and keep allocation failure distinct from "not
        found", so callers never turn memory pressure into a plausible 127.
*/
static bipolar shell_find_in_path_alloc_mode(string_address name,
                                              p8 address_to address_to into,
                                              positive address_to room,
                                              positive access, bool query,
                                              string_address fixed_path)
{
        string_address value;
        string_address known;
        positive name_length;
        positive wanted;

        if (!name || !string_get(name))
                return 0;

        name_length = string_length(name);

        if (string_first_of(name, '/'))
        {
                if (name_length == positive_max)
                        return -1;

                wanted = name_length + 1;
        }
        else if (!fixed_path && shell_hashall_on() &&
                 (known = hash_find(name)))
        {
                positive known_length = string_length(known);

                if (known_length == positive_max)
                        return -1;

                wanted = known_length + 1;
        }
        else
        {
                value = fixed_path ? fixed_path : env_get("PATH");

                if (!value)
                        value = "/bin:/usr/bin:/";

                if (!shell_path_wanted(value, name_length, address_of wanted))
                        return -1;

                /*
                        A guest binary lives under /bowls/NAME/usr/bin, which
                        is longer than any PATH component, and the launcher
                        is /bowls/bin/NAME.
                */
                {
                        positive guest = sizeof(BOWL_ROOT_PREFIX) + 64 +
                                         sizeof("/usr/sbin/") + name_length;

                        if (guest > wanted)
                                wanted = guest;
                }
        }

        if (!shell_room((address_any address_to)into, room, wanted, 1))
                return -1;

        /*
                A query asks where the name is, not whether it could run:
                type and command -v name the first file of that name, as the
                reference shell's do, and read the table the same as the
                executor does. What a query finds is not written into it,
                though: the table is the executor's answer to "what runs",
                and a name remembered from a query put a file nobody could
                run in front of the one that would have.
        */
        if (shell_find_in_path_mode(name, *into, *room, query ? 0 : access,
                                    !fixed_path, fixed_path))
                return 1;

        if (!query && shell_find_in_path_mode(name, *into, *room, 0, false,
                                              fixed_path))
                return 2;

        if (!fixed_path && !string_first_of(name, '/') &&
            bowl_fill_command(name, *into, *room))
                return 1;

        return 0;
}

/*
        The grammar words.

        They are not in the builtin table -- the parser knows them and nothing
        looks them up -- but `type` and `command -V` have to name them, so the
        list is written down once here rather than rebuilt from the parser's
        own tests.
*/
//      The same names in the order Bash writes them, which is the order
//      compgen -A keyword offers them in. The table above is sorted for the
//      lookup; a listing is not a lookup.
static string_address shell_keywords_listed[] = {
    "if",   "then", "else", "elif",  "fi",   "case",   "esac", "for",
    "select", "while", "until", "do", "done", "in",    "function", "time",
    "{",    "}",    "!",    "[[",    "]]",   "coproc", null,
};

static string_address shell_keywords[] = {
    "!",    "[[",   "]]",    "case",  "coproc",   "do",   "done", "elif",
    "else", "esac", "fi",    "for",   "function", "if",   "in",   "select",
    "then", "time", "until", "while", "{",        "}",    null,
};

#define SHELL_KEYWORDS (array_count(shell_keywords) - 1)

static COLD PURE bool shell_keyword_here(string_address name)
{
        return string_table_find(name, shell_keywords,
                                 sizeof(shell_keywords[0]),
                                 SHELL_KEYWORDS) < SHELL_KEYWORDS;
}

/*
        An alias is only a name to report when the shell would expand one.

        A non-interactive shell with expand_aliases off does not answer
        `type -t ll` with "alias" even with the alias in the table, because it
        would not run it either. Saying otherwise promises a name the parser
        then ignores.
*/
static COLD PURE bool shell_alias_visible(string_address name)
{
        return shell_shopt_on(EXPAND_ALIASES) && alias_lookup(name) != null;
}

#define SHELL_KIND_NAME 0
#define SHELL_KIND_TERSE 1
#define SHELL_KIND_LONG 2

static COLD fn shell_command_kind_written(writer write, string_address name,
                                          string_address kind, b32 style)
{
        if (style == SHELL_KIND_TERSE)
                string_format(write, "%s\n", kind);
        else if (style == SHELL_KIND_LONG)
        {
                //      "cd is a shell builtin" and "if is a shell keyword"
                //      are spelled the same by both references; a function
                //      is not. Bash says "f is a function" and dash says
                //      "f is a shell function", so the word "shell" is the
                //      personality's and not the kind's.
                if (shell_bash_compat && word_is(kind, "function"))
                        string_format(write, "%s is a function\n", name);
                else
                        string_format(write, "%s is a shell %s\n", name, kind);
        }
        else
                string_format(write, "%s\n", name);
}

/* `type -a` asks for every executable spelling in PATH, not merely the first
   one the ordinary command lookup returns. Keep that uncommon walk here so
   the executor and every non--a query retain their indexed first-hit path. */
static COLD bool shell_type_path_written(writer write, string_address name,
                                         string_address path, bool terse,
                                         bool path_only)
{
        file_facts facts;

        if (system_access_at(AT_FDCWD, path, ACCESS_EXECUTE) ||
            !test_facts(path, address_of facts, true) ||
            (facts.mode & MODE_FORMAT) == MODE_DIRECTORY)
                return false;

        if (terse)
                string_format(write, "file\n");
        else if (path_only)
                string_format(write, "%s\n", path);
        else
                string_format(write, "%s is %s\n", name, path);

        return true;
}

static COLD bipolar shell_type_paths(writer write, string_address name,
                                     p8 address_to address_to found,
                                     positive address_to found_room,
                                     bool terse, bool path_only)
{
        string_address value = env_get("PATH");
        path_walk walk;
        positive name_length = string_length(name);
        positive wanted;
        bipolar count = 0;
        bool empty = false;

        if (string_first_of(name, '/'))
                return shell_type_path_written(write, name, name, terse,
                                               path_only);

        if (!value && shell_bash_compat)
                return 0;
        if (!value)
                value = "/bin:/usr/bin:/";
        if (name_length > positive_max - 3 ||
            !shell_path_wanted(value, name_length, address_of wanted))
                return -1;

        /* An empty PATH component is printed as ./name, which needs one byte
           beyond the ordinary empty-segment join. The allocator rounds today,
           but its caller must still state the full bound. */
        if (wanted < name_length + 3)
                wanted = name_length + 3;
        if (!shell_room((address_any address_to)found, found_room, wanted, 1))
                return -1;

        walk = (path_walk){value, null, 0, false};

        while (path_walk_next(address_of walk))
        {
                string_address segment = walk.length
                                             ? walk.segment
                                             : (string_address)".";
                positive segment_length = walk.length ? walk.length : 1;

                /* Bash's all-path iterator treats each adjacent pair of
                   empty components as one current-directory entry. */
                if (!walk.length)
                {
                        empty = !empty;
                        if (!empty)
                                continue;
                }
                else
                        empty = false;

                if (!path_walk_join(*found, *found_room, segment,
                                    segment_length, name, "") ||
                    !shell_type_path_written(write, name, *found, terse,
                                             path_only))
                        continue;
                count++;
        }

        return count;
}

/*
        type: what a name would run.

        In the order the shell would actually try them, which is the only
        useful answer -- a grep on the path is not the grep that runs. -t
        names the kind in one word, -a says every place a name is, -p and -P
        want the file alone, and -f looks past the functions.
*/
#define SHELL_QUERY_ALL 1
#define SHELL_QUERY_PATH 2
#define SHELL_QUERY_FORCE_PATH 4
#define SHELL_QUERY_NO_FUNCTIONS 8
#define SHELL_QUERY_COMMAND 16
#define SHELL_QUERY_STANDARD_PATH 32

/* One namespace/PATH query, with the frontends retaining their option and
   status policies. command gives keywords precedence over aliases and asks
   whether any operand was found; type reports each missing operand. */
static inline INLINE b32 shell_query(writer write, positive index, b32 flags,
                                     b32 style)
{
        bool command = (flags & SHELL_QUERY_COMMAND) != 0;
        bool every = (flags & SHELL_QUERY_ALL) != 0;
        bool path_only = (flags & SHELL_QUERY_PATH) != 0;
        bool no_functions = (flags & SHELL_QUERY_NO_FUNCTIONS) != 0;
        bool terse = style == SHELL_KIND_TERSE;
        p8 address_to found = null;
        positive found_room = 0;
        b32 bad = 0;
        bool any = false;

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                bool keyword = shell_keyword_here(name);
                bool alias = (!command || !keyword) && shell_alias_visible(name);
                // These four bits are namespace precedence, not attributes.
                b32 kinds = keyword;
                bool matched = false;
                bipolar located;

                // A first-hit query must not walk the later namespaces once
                // an earlier one answered. -a alone needs every live kind.
                if (every || !(keyword || alias))
                {
                        positive2 named = string_hash_33_length(name);
                        bool special = exec_special_builtin(name);
                        bool builtin = special && shell_command_builtin_here(name, named);
                        bool function = !no_functions && (every || !builtin) &&
                                        exec_function_here_hashed(name, named);
                        kinds |= (builtin << 1) | (function << 2);
                        if (!special && (every || !function) &&
                            shell_command_builtin_here(name, named))
                                kinds |= 8;
                }

                //      -a asks for every place the name is, so a builtin or
                //      an alias standing in front of the files does not stop
                //      the walk: "type -a -p echo" writes both echoes.
                if (path_only && !(flags & SHELL_QUERY_FORCE_PATH) &&
                    (alias || kinds))
                {
                        //      -a asks for every place the name is, so a
                        //      builtin or an alias standing in front of the
                        //      files does not stop the walk: "type -a -p
                        //      echo" writes both echoes. The name was still
                        //      found, whether or not a path is written for it.
                        if (!every)
                                continue;

                        matched = true;
                }

                //      -P asks for a file and nothing else, so the
                //      namespaces are not written even when the answer is a
                //      single word rather than a path.
                if (!path_only && !(flags & SHELL_QUERY_FORCE_PATH))
                {
                        if (alias)
                        {
                                if (terse)
                                        string_format(write, "alias\n");
                                else if (style == SHELL_KIND_NAME)
                                        string_format(write, "alias %s='%s'\n",
                                                      name, alias_lookup(name));
                                else
                                        string_format(write,
                                            "%s is aliased to `%s'\n",
                                            name, alias_lookup(name));
                                matched = true;
                                if (!every)
                                        goto found_name;
                        }

                        while (kinds)
                        {
                                b32 kind = every ? kinds & -kinds : kinds;
                                shell_command_kind_written(
                                    write, name,
                                    kind == 1 ? (string_address)"keyword"
                                    : kind == 4 ? (string_address)"function"
                                                : (string_address)"builtin",
                                    style);
                                matched = true;
                                if (!every)
                                        goto found_name;
                                kinds &= kinds - 1;
                        }
                }

                located = every
                    ? shell_type_paths(write, name, address_of found,
                                       address_of found_room, terse, path_only)
                    : flags & SHELL_QUERY_STANDARD_PATH
                        ? shell_find_in_standard_path_alloc(
                              name, address_of found, address_of found_room, true)
                        : shell_find_in_path_query_alloc(
                              name, address_of found, address_of found_room);

                if (located < 0)
                {
                        string_format(log_error, "%s: no room\n",
                                      command ? "command" : "type");
                        bad = 2;
                        break;
                }
                if (located)
                {
                        if (!every)
                        {
                                if (terse)
                                        string_format(write, "file\n");
                                else if (path_only || style == SHELL_KIND_NAME)
                                        string_format(write, "%s\n", found);
                                else
                                        string_format(write, "%s is %s\n",
                                                      name, found);
                        }
                        goto found_name;
                }
                if (matched)
                        goto found_name;

                //      Bash says a name it could not find on the
                //      diagnostic channel and answers one; dash writes it
                //      where the answers go and answers 127. Following dash
                //      in all three personalities put the line in the wrong
                //      stream for two of them.
                //      -t and -p/-P ask for one word and nothing else, so
                //      neither says anything when there is no answer; -a and
                //      -f do say it, which is where this used to stay quiet.
                if (command ? style == SHELL_KIND_LONG
                            : !terse && !path_only &&
                              !(flags & SHELL_QUERY_FORCE_PATH))
                        if (shell_bash_compat)
                        {
                                shell_diagnostic_where();
                                string_format(log_error, "type: %s: not found\n",
                                              name);
                        }
                        else
                                string_format(write, "%s: not found\n", name);
                if (!command)
                        bad = shell_bash_compat ||
                              terse || path_only || no_functions || every
                                  ? 1 : 127;
                continue;

        found_name:
                any = true;
        }

        if (found)
                memory_free(found, found_room);
        return bad ? bad : !command || any ? 0 : shell_bash_compat ? 1 : 127;
}

COLD fn shell_type(writer write, string_address input)
{
        shell_option_walk walk = {1};
        positive index;
        bool terse = false;
        bool every = false;
        bool path_only = false;
        bool force_path = false;
        bool no_functions = false;
        p8 which;

        while (shell_option_letter(address_of walk, address_of which))
        {
                //      -t on one side and -p and -P on the other ask for
                //      two different single answers, and the last of them
                //      wins. -P leaves behind the demand for a file, which
                //      a -t after it does not take away: "type -aPt cd"
                //      finds no file called cd and says nothing at all.
                if (which == 't')
                {
                        terse = true;
                        path_only = false;
                }
                else if (which == 'a')
                        every = true;
                else if (which == 'p')
                {
                        path_only = true;
                        terse = false;
                }
                else if (which == 'P')
                {
                        path_only = true;
                        force_path = true;
                        terse = false;
                }
                else if (which == 'f')
                        no_functions = true;
                else
                {
                        p8 said[2] = {which, end};

                        return shell_answer(shell_letter_refused(
                            "type", which, "type [-afptP] name [name ...]"));
                }
        }

        index = walk.index;

        if (index >= shell_argc)
                return shell_answer(0);

        shell_answer(shell_query(
            write, index, (every ? SHELL_QUERY_ALL : 0) |
                          (path_only ? SHELL_QUERY_PATH : 0) |
                          (force_path ? SHELL_QUERY_FORCE_PATH : 0) |
                          (no_functions ? SHELL_QUERY_NO_FUNCTIONS : 0),
            terse ? SHELL_KIND_TERSE : SHELL_KIND_LONG));
}

/*
        command: run a name as the shell would, and never as a function.

        command -v prints what would run rather than running it, which is what
        a script uses to ask whether something is there at all.
*/
fn shell_command_builtin(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        shell_option_walk walk = {1};
        positive index;
        bool only_say = false;
        bool at_length = false;
        bool standard_path = false;
        p8 option;

        while (shell_option_letter(address_of walk, address_of option))
        {
                if (option == 'v')
                        only_say = true;
                else if (option == 'V')
                {
                        only_say = true;
                        at_length = true;
                }
                else if (option == 'p')
                {
                        //      rbash: -p is the standard PATH, which is a way
                        //      back to the directories the restriction took.
                        if (shell_restricted)
                        {
                                shell_diagnostic_where();
                                return shell_answer(string_report(
                                    log_error, 1, "command: -p: restricted\n"));
                        }

                        standard_path = true;
                }
                else
                        return shell_answer(shell_letter_refused(
                            "command", option,
                            "command [-pVv] command [arg ...]"));
        }

        index = walk.index;

        if (index >= shell_argc)
                return shell_answer(0);

        if (only_say)
                return shell_answer(shell_query(
                    write, index, SHELL_QUERY_COMMAND |
                                  (standard_path ? SHELL_QUERY_STANDARD_PATH : 0),
                    at_length ? SHELL_KIND_LONG : SHELL_KIND_NAME));

        // Running it is the executor's business, and it is told to skip the
        // function table by the words it is handed.
        {
                memory_copy(shell_argv, shell_argv + index,
                            (positive)(shell_argc - index) *
                                sizeof(shell_argv[0]));

                shell_argc -= index;
                shell_argv[shell_argc] = null;
        }

        if (!shell_builtin_disabled(shell_argv[0]) &&
            exec_control_builtin(shell_argv[0], true))
                return;

        {
                bool tail = shell_tail_command;
                bool reader = word_is(shell_argv[0], "eval") ||
                              word_is(shell_argv[0], ".") ||
                              word_is(shell_argv[0], "source");

                if (reader)
                        shell_command_reader_depth++;

                bool found = shell_builtin(
                    shell_arguments(), string_hash_33_length(shell_argv[0]));

                if (reader)
                {
                        shell_command_reader_depth--;
                        exec_command_reader_finish();
                }
                if (found)
                {
                        shell_tail_command = tail;
                        return;
                }

                shell_tail_command = tail;
        }

        {
                string_address name = shell_argv[0];
                p8 address_to found = null;
                positive found_room = 0;
                bipolar located = standard_path
                                    ? shell_find_in_standard_path_alloc(
                                          name, address_of found,
                                          address_of found_room, false)
                                    : shell_find_in_path_alloc(
                                          name, address_of found,
                                          address_of found_room);

                if (located < 0)
                {
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "command"));
                }

                if (!located)
                {
                        if (found)
                                memory_free(found, found_room);

                        {
                                shell_diagnostic_where();

                                //      The line a missing command gets, and
                                //      command does not put its own name in
                                //      front of it.
                                return shell_answer(string_report(log_error,
                                    127,
                                    shell_bash_compat
                                        ? "%s: command not found\n"
                                        : "%s: not found\n",
                                    name));
                        }
                }

                if (located == 2)
                {
                        memory_free(found, found_room);
                        return shell_answer(string_report(log_error, 126, "command: %s: cannot run\n", name));
                }

                {
                        string_address address_to saved_argv = shell_argv;
                        positive saved_argc = shell_argc;

                        if (!bowl_wrap_command(found, shell_directory,
                                               address_of shell_argv,
                                               address_of shell_argc))
                                shell_argv[0] = found;

                        if (shell_tail_command)
                                shell_thread_instance_mode(true);
                        else
                                shell_execute_command();
                        shell_argv = saved_argv;
                        shell_argc = saved_argc;
                        shell_argv[0] = name;
                        memory_free(found, found_room);
                }
        }
}

fn shell_which(writer write, string_address input)
{
        p8 address_to found = null;
        positive found_room = 0;
        positive index = 1;
        bool every = false;
        b32 answer = 0;

        (void)input;

        /*
                The GNU program's option surface, so that a script handing
                these over is answered rather than told its options are
                names. Only --all changes what is written; the rest are
                about a terminal, a dot or a tilde in PATH, or a list of
                aliases and functions read from the input -- none of which
                a builtin asked from a script has to look at. An option the
                program does not have is named and the walk carries on,
                which is what the GNU program does with it.
        */
        for (; index < shell_argc; index++)
        {
                string_address word = shell_argv[index];

                if (word_is(word, "--"))
                {
                        index++;
                        break;
                }

                if (string_not(word, '-') || !string_get(word + 1))
                        break;

                //      The ones that are recognised and do nothing. A list
                //      rather than ten comparisons written out, because a
                //      list is all it is: nothing here is decided, only
                //      spelled.
                static string_address const accepted[] = {
                    "-i",           "--read-alias",     "--skip-alias",
                    "--read-functions", "--skip-functions",
                    "--skip-dot",   "--skip-tilde",
                    "--show-dot",   "--show-tilde",     "--tty-only",
                };

                if (word_is(word, "-a") || word_is(word, "--all"))
                        every = true;
                else if (string_table_find(word, accepted, sizeof(accepted[0]),
                                           array_count(accepted)) ==
                         array_count(accepted))
                        string_format(log_error,
                                      "which: invalid option -- '%s'\n",
                                      word + 1);
        }

        if (index >= shell_argc)
        {
                log_error("Usage: which [options] [--] COMMAND [...]\n", 0);
                return shell_answer(255);
        }

        for (; index < shell_argc; index++)
        {
                string_address name = shell_argv[index];
                bipolar located;

                // Before the path, because that is the order the shell runs
                // them in: a grep on the path is not the grep that would run.
                if (shell_command_builtin_here(name,
                                               string_hash_33_length(name)))
                {
                        string_format(write, "%s: shell builtin\n", name);
                        continue;
                }

                located = every
                    ? shell_type_paths(write, name, address_of found,
                                       address_of found_room, false, true)
                    : shell_find_in_path_alloc(name, address_of found,
                                               address_of found_room);

                if (located < 0)
                {
                        if (found)
                                memory_free(found, found_room);

                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "which"));
                }

                if (located)
                {
                        if (!every)
                                string_format(write, "%s\n", found);

                        continue;
                }

                //      The GNU program says which PATH it looked along, on
                //      the diagnostic channel, and answers one.
                string_format(log_error, "which: no %s in (%s)\n", name,
                              env_get("PATH") ? env_get("PATH") : (string_address)"");
                answer = 1;
        }

        if (found)
                memory_free(found, found_room);

        shell_answer(answer);
}

/*
        ulimit.

        prlimit64 rather than getrlimit, because getrlimit is not on the
        riscv64 table at all and prlimit64 is on all three. The kernel counts
        in bytes and in seconds; the shell has been quoting file sizes in five
        hundred and twelve byte blocks and memory in kilobytes since long
        before either of us, so each limit carries the number it is divided by.
*/
typedef struct
{
        string_address name;
        string_address bash_line;
        p8 letter;
        p8 resource;
        positive step;
} shell_limit;

static shell_limit shell_limits[] = {
    {"time(seconds)", null, 't', 0, 1},
    {"file(blocks)", null, 'f', 1, 512},
    {"data(kbytes)", null, 'd', 2, 1024},
    {"stack(kbytes)", null, 's', 3, 1024},
    {"coredump(blocks)", null, 'c', 4, 512},
    {"memory(kbytes)", null, 'm', 5, 1024},
    {"locked memory(kbytes)", null, 'l', 8, 1024},
    {"process", null, 'p', 6, 1},
    {"nofiles", null, 'n', 7, 1},
    {"vmemory(kbytes)", null, 'v', 9, 1024},
    {"locks", null, 'w', 10, 1},
    {"rtprio", null, 'r', 14, 1},
    {null, null, 0, 0, 0},
};

/*
        The letters Bash has and the reference shell does not.

        Kept out of the table above because that table is what `ulimit -a`
        prints, and the reference shell's -a is what this shell's is compared
        against. A letter here is a resource a script can read and set by name
        without appearing in a listing that would then disagree.

        The last three name resources Linux has no number for. Bash takes the
        letters, so they are taken, and the resource that answers for them is
        one the kernel refuses -- which reads as "unlimited" and sets nothing,
        rather than quietly standing for some other limit.
*/
#define SHELL_LIMIT_NONE 255
#define SHELL_LIMIT_PIPE 254

static shell_limit shell_extra_limits[] = {
    {"nice", null, 'e', 13, 1},
    {"sigpending", null, 'i', 11, 1},
    {"msgqueue(bytes)", null, 'q', 12, 1},
    {"processes", null, 'u', 6, 1},
    {"locks", null, 'x', 10, 1},
    {"rttime", null, 'R', 15, 1},
    {"kqueues", null, 'k', SHELL_LIMIT_NONE, 1},
    {"pipesize", null, 'P', SHELL_LIMIT_NONE, 1},
    {"threads", null, 'T', SHELL_LIMIT_NONE, 1},
    {null, null, 0, 0, 0},
};

/* Bash's Linux surface has a different -p resource and 1024-byte -c/-f
   scales. The exact left sides also make `ulimit -a` useful to scripts that
   consume Bash's stable report, while the compact dash table above stays
   byte-for-byte unchanged outside Bash compatibility mode. */
static shell_limit shell_bash_limits[] = {
    {"real-time non-blocking time",
     "real-time non-blocking time  (microseconds, -R) ", 'R', 15, 1},
    {"core file size", "core file size              (blocks, -c) ",
     'c', 4, 1024},
    {"data seg size", "data seg size               (kbytes, -d) ",
     'd', 2, 1024},
    {"scheduling priority", "scheduling priority                 (-e) ",
     'e', 13, 1},
    {"file size", "file size                   (blocks, -f) ",
     'f', 1, 1024},
    {"pending signals", "pending signals                     (-i) ",
     'i', 11, 1},
    {"max locked memory", "max locked memory           (kbytes, -l) ",
     'l', 8, 1024},
    {"max memory size", "max memory size             (kbytes, -m) ",
     'm', 5, 1024},
    {"open files", "open files                          (-n) ",
     'n', 7, 1},
    {"pipe size", "pipe size                (512 bytes, -p) ",
     'p', SHELL_LIMIT_PIPE, 512},
    {"POSIX message queues", "POSIX message queues         (bytes, -q) ",
     'q', 12, 1},
    {"real-time priority", "real-time priority                  (-r) ",
     'r', 14, 1},
    {"stack size", "stack size                  (kbytes, -s) ",
     's', 3, 1024},
    {"cpu time", "cpu time                   (seconds, -t) ",
     't', 0, 1},
    {"max user processes", "max user processes                  (-u) ",
     'u', 6, 1},
    {"virtual memory", "virtual memory              (kbytes, -v) ",
     'v', 9, 1024},
    {"file locks", "file locks                          (-x) ",
     'x', 10, 1},
    {null, null, 0, 0, 0},
};

static PURE positive shell_limit_step(shell_limit address_to limit)
{
        return shell_posix_on() &&
                       (limit->resource == 1 || limit->resource == 4)
                   ? 512 : limit->step;
}

fn shell_limit_said(writer write, shell_limit address_to limit, bool hard)
{
        ul_limit_pair pair;
        p64 value;

        if (limit->resource == SHELL_LIMIT_PIPE)
                return string_format(write, "8\n");

        if (ul_prlimit(0, limit->resource, null, address_of pair) < 0)
                return string_format(write, "unlimited\n");

        value = hard ? pair.hard : pair.soft;

        if (value == UL_LIMIT_INFINITE)
                return string_format(write, "unlimited\n");

        positive_to_string(write, (positive)(value / shell_limit_step(limit)));
        write("\n", 1);
}

/* The name in front of a value, for -a and for every listing that names
   more than one resource and so has to say which value belongs to which. */
static COLD fn shell_limit_label(writer write, shell_limit address_to limit,
                                 bool bash)
{
        if (bash)
                write(limit->bash_line, string_length(limit->bash_line));
        else
        {
                string_to_field(write, limit->name, 20, ' ', true);
                write(" ", 1);
        }
}

fn shell_limit_listed(writer write, bool bash, bool hard)
{
        shell_limit address_to limit = bash ? shell_bash_limits : shell_limits;

        while (limit->name)
        {
                shell_limit_label(write, limit, bash);
                shell_limit_said(write, limit, hard);
                limit++;
        }
}

fn shell_ulimit(writer write, string_address input)
{
        positive index = 1;
        bool hard = false;
        bool soft = false;
        bool listed = false;
        //      Every resource the words name, not merely the last of them:
        //      `ulimit -d -f` reports both, each behind its own name.
        shell_limit address_to picked[32];
        positive picked_count = 0;
        b32 answer = 0;
        shell_limit address_to chosen = null;
        shell_limit address_to limit;

        //      Every resource at once and a value for one of them is a
        //      contradiction dash refuses outright; Bash reads the list and
        //      pays the value no attention.
        for (positive at = 1; at < shell_argc; at++)
                if (!string_is(shell_argv[at], '-') && !shell_bash_compat)
                {
                        for (positive back = 1; back < at; back++)
                                if (string_is(shell_argv[back], '-') &&
                                    string_first_of(shell_argv[back] + 1, 'a'))
                                        return shell_answer(string_report(
                                            log_error, 2,
                                            "ulimit: too many arguments\n"));
                        break;
                }

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                //      "--" is the end of the options, not a resource
                //      called "-": all three shells then report the
                //      default resource.
                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

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

                        if (shell_bash_compat)
                        {
                                limit = shell_bash_limits;

                                while (limit->name && limit->letter != which)
                                        limit++;
                        }
                        else
                        {
                                limit = shell_limits;

                                while (limit->name && limit->letter != which)
                                        limit++;

                                if (!limit->name)
                                {
                                        limit = shell_extra_limits;

                                        while (limit->name &&
                                               limit->letter != which)
                                                limit++;
                                }
                        }

                        if (!limit->name)
                        {
                                return shell_answer(shell_letter_refused(
                                    "ulimit", which,
                                    "ulimit [-SHabcdefiklmnpqrstuvxPRT] "
                                    "[limit]"));
                        }

                        chosen = limit;

                        if (picked_count < array_count(picked))
                                picked[picked_count++] = limit;
                }

                index++;
        }

        if (listed)
        {
                shell_limit_listed(write, shell_bash_compat, hard);

                return shell_answer(0);
        }

        // No resource named is the file size, which is what every shell means
        // by a bare ulimit.
        if (!chosen)
        {
                chosen = shell_bash_compat ? shell_bash_limits + 4
                                           : shell_limits + 1;
                picked[0] = chosen;
                picked_count = 1;
        }

        //      Reporting or setting each resource named is Bash's; the
        //      reference shell keeps only the last of them, and answers with
        //      a bare value that has no name in front of it.
        if (!shell_bash_compat)
        {
                picked[0] = chosen;
                picked_count = 1;
        }

        //      One value and no more: dash counts the operands it was
        //      left with before it reads any of them, and bash reads the
        //      first and pays no attention to the rest.
        if (!shell_bash_compat && index + 1 < shell_argc)
        {
                shell_diagnostic_where();

                return shell_answer(string_report(log_error, 2,
                    "ulimit: too many arguments\n"));
        }

        if (index >= shell_argc)
        {
                for (positive at = 0; at < picked_count; at++)
                {
                        // One resource answers with a bare value; several
                        // have to say which value belongs to which.
                        if (picked_count > 1)
                                shell_limit_label(write, picked[at],
                                                  shell_bash_compat);

                        shell_limit_said(write, picked[at], hard);
                }

                return shell_answer(0);
        }

        for (positive at = 0; at < picked_count; at++)
        {
                ul_limit_pair pair;
                p64 value;

                chosen = picked[at];

                //      A resource that cannot take the value is reported and
                //      the rest are still set, which is what bash does with a
                //      list that names the pipe buffer among others.
                if (chosen->resource == SHELL_LIMIT_PIPE)
                {
                        answer = string_report(log_error,
                                               shell_bash_compat ? 1 : 2,
                            "ulimit: pipe size: cannot modify limit\n");
                        continue;
                }

                if (ul_prlimit(0, chosen->resource, null, address_of pair) < 0)
                {
                        answer = 1;
                        continue;
                }

                if (word_is(shell_argv[index], "unlimited"))
                        value = UL_LIMIT_INFINITE;
                else if (shell_bash_compat &&
                         word_is(shell_argv[index], "soft"))
                        value = pair.soft;
                else if (shell_bash_compat &&
                         word_is(shell_argv[index], "hard"))
                        value = pair.hard;
                else
                {
                        bool good;
                        bipolar asked = shell_signed(shell_argv[index], address_of good);

                        if (!good ||
                            (shell_bash_compat &&
                             (asked < 0 ||
                              (positive)asked >
                                  (UL_LIMIT_INFINITE - 1) /
                                      shell_limit_step(chosen))))
                        {
                                shell_answer(shell_bash_compat ? 1 : 2);

                                shell_diagnostic_where();

                                return string_format(
                                    log_error,
                                    shell_bash_compat
                                        ? "ulimit: %s: invalid number\n"
                                        : "ulimit: bad number\n",
                                    shell_argv[index]);
                        }

                        value = (p64)asked * shell_limit_step(chosen);
                }

                // Neither said means both, which is the only way a script can
                // lower a ceiling it will never be allowed to raise again.
                if (hard || !soft)
                        pair.hard = value;

                if (soft || !hard)
                        pair.soft = value;

                {
                        bipolar told = ul_prlimit(0, chosen->resource,
                                                  address_of pair, null);

                        if (told < 0)
                        {
                                //      bash names the limit and what the
                                //      kernel said; dash says only that it
                                //      could not, with the reason after it
                                //      in brackets.
                                string_address why =
                                    system_error_message(-told);

                                if (!why)
                                        why = (string_address)
                                            "Operation not permitted";

                                answer = shell_bash_compat ? 1 : 2;
                                shell_diagnostic_where();

                                if (shell_bash_compat)
                                        string_format(log_error,
                                            "ulimit: %s: cannot modify "
                                            "limit: %s\n", chosen->name, why);
                                else
                                        string_format(log_error,
                                            "ulimit: error setting limit "
                                            "(%s)\n", why);

                                continue;
                        }
                }
        }

        shell_answer(answer);
}

/*
        builtin: run the builtin behind a name, whatever else has that name.

        A function that wraps a builtin needs a way to reach the thing it
        wraps, and `command` is not it -- command would find the function
        again through PATH if the name happened to be a program too.
*/
fn shell_builtin_run(writer write, string_address input)
{
        bool tail = shell_tail_command;
        // Two bytes: the shared formatter has no %c, so a letter is spelled.
        p8 room[2];
        shell_option_walk walk = {1};
        p8 option;
        positive index;

        (void)write;
        (void)input;

        if (shell_argc < 2)
                return shell_answer(0);

        //      builtin takes no options of its own, so the only word the
        //      walk can hand back is one it should refuse; "--" ends them
        //      and the name behind it is the builtin to run.
        while (shell_option_letter(address_of walk, address_of option))
        {
                return shell_answer(shell_letter_refused(
                    "builtin", option, "builtin [shell-builtin [arg ...]]"));
        }

        index = walk.index;

        if (index >= shell_argc)
                return shell_answer(0);

        memory_copy(shell_argv, shell_argv + index,
                    (positive)(shell_argc - index + 1) * sizeof(shell_argv[0]));
        shell_argc -= index;

        if (!shell_builtin_disabled(shell_argv[0]) &&
            exec_control_builtin(shell_argv[0], true))
                return;

        if (shell_builtin(shell_arguments(),
                          string_hash_33_length(shell_argv[0])))
        {
                shell_tail_command = tail;
                return;
        }

        shell_tail_command = tail;
        shell_diagnostic_where();
        string_format(log_error, "builtin: %s: not a shell builtin\n",
                      shell_argv[0]);
        shell_answer(1);
}

/*
        enable: which builtins the shell admits to having.

        -n takes a name away, so that the file on PATH runs instead; naming it
        again gives it back. -a and -p list, which is what a script asking
        what it is running on reads.
*/
fn shell_enable(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        shell_option_walk walk = {1};
        p8 which;
        bool off = false;
        bool every = false;
        b32 bad = 0;

        while (shell_option_letter(address_of walk, address_of which))
        {
                if (which == 'n')
                        off = true;
                else if (which == 'p')
                        continue;
                else if (which == 'a')
                        every = true;
                else if (which == 'f' || which == 'd' || which == 's')
                {
                        // Dynamic builtin loading is not supported.
                        return shell_answer(string_report(log_error, 2, "enable: not supported\n"));
                }
                else
                        return shell_answer(shell_letter_refused(
                            "enable", which,
                            "enable [-a] [-dnps] [-f filename] [name ...]"));
        }

        positive index = walk.index;
        if (index >= shell_argc)
        {
                shell_command address_to command = shell_commands;

                while (command->name)
                {
                        bool here = !shell_disabled[command - shell_commands];

                        if (here == !off || every)
                                string_format(write, "enable %s%s\n",
                                              here ? "" : "-n ",
                                              command->name);

                        command++;
                }

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                positive at = shell_command_index_hashed(
                    name, string_hash_33_length(name));

                if (at >= SHELL_COMMAND_COUNT)
                {
                        shell_diagnostic_where();
                        string_format(log_error,
                                      "enable: %s: not a shell builtin\n",
                                      name);
                        bad = 1;
                        continue;
                }

                if (shell_disabled[at] != off)
                {
                        shell_disabled[at] = off;
                        if (off) shell_disabled_count++;
                        else shell_disabled_count--;
                }
        }

        shell_answer(bad);
}

/*
        compgen: the names a completion would offer.

        Without a terminal there is nothing to complete, but a script that
        asks what functions or variables exist is asking a question the shell
        can answer, and it is the one use of compgen that works in a pipe.
*/
static string_address compgen_prefix;
static positive compgen_prefix_length;
static positive compgen_shown;
//      -X is the filter, -P and -S the two ends written around whatever
//      survives it. A leading ! in the filter keeps the matches instead of
//      dropping them, which is the one place compgen spells a negation.
static string_address compgen_reject;
static string_address compgen_before;
static string_address compgen_after;

static COLD fn compgen_offer(writer write, string_address name)
{
        positive length = string_length(name);

        if (compgen_prefix_length &&
            (length < compgen_prefix_length ||
             memory_compare(name, compgen_prefix, compgen_prefix_length)))
                return;

        if (compgen_reject && string_get(compgen_reject))
        {
                bool keep = string_is(compgen_reject, '!');
                bool hit = shell_match(compgen_reject + keep, name);

                if (hit != keep)
                        return;
        }

        if (compgen_before)
                write(compgen_before, string_length(compgen_before));
        write(name, length);
        if (compgen_after)
                write(compgen_after, string_length(compgen_after));
        write("\n", 1);
        compgen_shown++;
}

static COLD fn compgen_variable(writer write, string_address name, positive length,
                           b32 mark)
{
        p8 held[256];

        (void)mark;

        if (length >= sizeof(held))
                return;

        memory_copy_apart(held, name, length);
        held[length] = end;
        compgen_offer(write, held);
}

static COLD fn compgen_function(writer write, string_address name,
                                positive length, b32 mark)
{
        (void)length;
        (void)mark;
        compgen_offer(write, name);
}

fn shell_compgen(writer write, string_address input)
{
        positive index = 1;
        bool functions = false;
        bool variables = false;
        bool builtins = false;
        bool aliases = false;
        bool commands = false;
        bool files = false;
        bool directories = false;
        bool keywords = false;
        //      Bash answers zero for a compgen given no option at all and
        //      one for an option that generated nothing, so the two have to
        //      be told apart.
        bool optioned = false;
        string_address words = null;

        compgen_prefix = null;
        compgen_prefix_length = 0;
        compgen_shown = 0;
        compgen_reject = null;
        compgen_before = null;
        compgen_after = null;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                p8 which = shell_argv[index][1];
                string_address value = null;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                optioned = true;

                if (which == 'A' || which == 'W' || which == 'P' ||
                    which == 'S' || which == 'X' || which == 'F' ||
                    which == 'C' || which == 'G')
                {
                        if (string_get(shell_argv[index] + 2))
                                value = shell_argv[index] + 2;
                        else if (index + 1 < shell_argc)
                                value = shell_argv[++index];
                        else
                                return shell_answer(2);
                }

                if (which == 'A')
                {
                        //      Bash's closed list of actions. One that is
                        //      not on it is a usage error before anything
                        //      is generated.
                        static const string_address actions[] = {
                            "alias", "arrayvar", "binding", "builtin",
                            "command", "directory", "disabled", "enabled",
                            "export", "file", "function", "group",
                            "helptopic", "hostname", "job", "keyword",
                            "running", "service", "setopt", "shopt",
                            "signal", "stopped", "user", "variable"};
                        bool known = false;

                        for (positive at = 0; at < array_count(actions); at++)
                                if (word_is(value, actions[at]))
                                        known = true;

                        if (!known)
                                return shell_answer(string_report(
                                    log_error, 2,
                                    "compgen: %s: invalid action name\n",
                                    value));

                        if (word_is(value, "keyword"))
                                keywords = true;
                        else if (word_is(value, "function"))
                                functions = true;
                        else if (word_is(value, "variable"))
                                variables = true;
                        else if (word_is(value, "builtin"))
                                builtins = true;
                        else if (word_is(value, "alias"))
                                aliases = true;
                        else if (word_is(value, "command"))
                                commands = true;
                        else if (word_is(value, "file"))
                                files = true;
                        else if (word_is(value, "directory"))
                                directories = true;
                }
                else if (which == 'W')
                        words = value;
                else if (which == 'P')
                        compgen_before = value;
                else if (which == 'S')
                        compgen_after = value;
                else if (which == 'X')
                        compgen_reject = value;
                else if (which == 'v')
                        variables = true;
                else if (which == 'b')
                        builtins = true;
                else if (which == 'a')
                        aliases = true;
                else if (which == 'c')
                        commands = true;
                else if (which == 'f')
                        files = true;
                else if (which == 'd')
                        directories = true;
                else if (which != 'A' && which != 'W' && which != 'P' &&
                         which != 'S' && which != 'X' && which != 'F' &&
                         which != 'C' && which != 'G' && which != 'o' &&
                         which != 'V' && which != 'e' && which != 'g' &&
                         which != 'j' && which != 'k' && which != 's' &&
                         which != 'u')
                {
                        // Two bytes: the shared formatter has no %c.
                        p8 room[2];

                        return shell_answer(shell_letter_refused(
                            "compgen", which,
                            "compgen [-V varname] [-abcdefgjksuv] "
                            "[-o option] [-A action] [-G globpat] "
                            "[-W wordlist] [-F function] [-C command] "
                            "[-X filterpat] [-P prefix] [-S suffix] [word]"));
                }

                index++;
        }

        if (index < shell_argc)
        {
                compgen_prefix = shell_argv[index];
                compgen_prefix_length = string_length(compgen_prefix);
        }

        if (functions || commands)
        {
                if (!shell_inventory_sorted(write, 0, compgen_function, true,
                                            false))
                        return shell_answer(string_report(log_error, 2, "%s: no room\n", "compgen"));
        }

        if (keywords)
                for (positive at = 0; shell_keywords_listed[at]; at++)
                        compgen_offer(write, shell_keywords_listed[at]);

        if (aliases || commands)
                for (positive at = 0; at < alias_count; at++)
                        compgen_offer(write, alias_table[at].name);

        if (builtins || commands)
        {
                //      Bash offers them in order. The registry is in the
                //      order the builtins were written down, which is not
                //      an order anybody reading a completion list expects.
                string_address sorted[SHELL_COMMAND_COUNT];
                positive count = 0;

                for (shell_command address_to command = shell_commands;
                     command->name; command++)
                {
                        positive at = count++;

                        while (at && string_compare(sorted[at - 1],
                                                    command->name) > 0)
                        {
                                sorted[at] = sorted[at - 1];
                                at--;
                        }

                        sorted[at] = command->name;
                }

                for (positive at = 0; at < count; at++)
                        compgen_offer(write, sorted[at]);
        }

        if (variables)
                shell_inventory_sorted(write, 0, compgen_variable, false, false);

        if (files || directories)
        {
                p8 block[2048];
                bipolar directory = system_open_at(AT_FDCWD,
                                                   (string_address) ".",
                                                   FILE_READ | O_DIRECTORY);

                positive have = 0, at = 0;
                bipolar error = 0;
                struct linux_dirent64 address_to entry;
                while (directory >= 0 &&
                       (entry = file_directory_next(
                            directory, block, sizeof(block), address_of have,
                            address_of at, address_of error)))
                {
                        if (entry->d_name[0] == '.')
                                continue;
                        if (directories && !files && entry->d_type != 4)
                                continue;
                        compgen_offer(write, (string_address)entry->d_name);
                }

                if (directory >= 0)
                        system_close(directory);
        }

        //      The word list is generated after every other source, which
        //      is the order Bash writes them in when both were asked for.
        if (words)
        {
                p8 held[1024];
                positive at = 0;

                while (string_get(words))
                {
                        if (string_is(words, ' ') || string_is(words, '\t'))
                        {
                                words++;
                                continue;
                        }

                        at = 0;

                        while (string_get(words) && string_not(words, ' ') &&
                               string_not(words, '\t') && at + 1 < sizeof(held))
                                held[at++] = string_get(words++);

                        held[at] = end;
                        compgen_offer(write, held);
                }
        }

        //      Nothing was asked for, so nothing missing: Bash answers
        //      one for an action that matched nothing and zero for a compgen
        //      that named no action at all.
        shell_answer(compgen_shown || !optioned ? 0 : 1);
}

/*
        The prompt, with the escapes a prompt is written in.

        A prompt is a small language of its own -- \u for who is typing, \w
        for where, \$ for whether they are root -- and a script that sets PS1
        writes it in that language and not in bytes. Nothing else in the shell
        reads it, so it is expanded where it is printed and never stored.

        \[ and \] mark a run that takes no room on the line. The editor here
        does not measure the prompt, so they are dropped rather than counted,
        which is what they are for either way.
*/
static COLD fn shell_prompt_directory(writer write, bool whole)
{
        string_address path = shell_directory;
        string_address home = env_get("HOME");
        positive home_length = home ? string_length(home) : 0;
        positive length = string_length(path);

        if (!whole)
        {
                string_address last = string_last_of(path, '/');

                if (last && string_get(last + 1))
                        return write(last + 1, string_length(last + 1));

                return write(path, length);
        }

        if (home_length > 1 && length >= home_length &&
            !memory_compare(path, home, home_length) &&
            (length == home_length || path[home_length] == '/'))
        {
                write("~", 1);

                return write(path + home_length, length - home_length);
        }

        write(path, length);
}

COLD fn shell_prompt_written(writer write, string_address text)
{
        while (string_get(text))
        {
                p8 value = string_get(text++);
                p8 letter;

                if (value != '\\')
                {
                        write(address_of value, 1);
                        continue;
                }

                letter = string_get(text);

                if (!letter)
                {
                        write("\\", 1);
                        return;
                }

                text++;

                switch (letter)
                {
                case 'u':
                {
                        p8 name[64];
                        positive id = (positive)system_call_1(syscall(getuid),
                                                              0);

                        if (file_user_name(id, name, sizeof(name)) &&
                            string_get(name))
                                write(name, string_length(name));
                        else
                        {
                                p8 written[24];

                                write(written, positive_into_string(written,
                                                                    id));
                        }

                        break;
                }

                case 'h':
                case 'H':
                {
                        string_address named = shell_machine_name();
                        string_address stop = letter == 'h'
                                                ? string_first_of(named, '.')
                                                : null;

                        write(named, stop ? (positive)(stop - named)
                                          : string_length(named));
                        break;
                }

                case 'w': shell_prompt_directory(write, true); break;
                case 'W': shell_prompt_directory(write, false); break;

                case '$':
                        write(system_call_1(syscall(geteuid), 0) ? "$" : "#",
                              1);
                        break;

                case 'd':
                        date_shape(write,
                                   shell_clock_seconds(SHELL_CLOCK_REALTIME,
                                                       null),
                                   (string_address) "%a %b %d");
                        break;

                case 't':
                        date_shape(write,
                                   shell_clock_seconds(SHELL_CLOCK_REALTIME,
                                                       null),
                                   (string_address) "%H:%M:%S");
                        break;

                case 'A':
                        date_shape(write,
                                   shell_clock_seconds(SHELL_CLOCK_REALTIME,
                                                       null),
                                   (string_address) "%H:%M");
                        break;

                case 'n': write("\n", 1); break;
                case 'r': write("\r", 1); break;
                case 'a': write("\a", 1); break;
                case 'e': write("\033", 1); break;
                case 's': write("sh", 2); break;
                case 'v': write("5.3", 3); break;
                case 'V': write("5.3.15", 6); break;
                case '\\': write("\\", 1); break;

                //      A run that occupies no columns. Nothing here counts
                //      columns, so the markers themselves are all there is to
                //      drop.
                case '[':
                case ']': break;

                default:
                        write("\\", 1);
                        write(address_of letter, 1);
                        break;
                }
        }
}

/*
        The prompt this shell prints, which is PS1 when a script has set one.

        The built-in prompt stays the default rather than Bash's, because it
        is what this shell has always printed and nothing in a script depends
        on the bytes of a prompt it did not set.
*/
COLD fn shell_prompt_write(writer write, bool more)
{
        string_address text = env_get(more ? "PS2" : "PS1");

        if (!text)
                return write(more ? "> " : PROMPT, more ? 2 : sizeof(PROMPT) - 1);

        shell_prompt_written(write, text);
}

fn shell_help(writer write, string_address input)
{
        // Two bytes each: the formatter has no %c, so an option letter is
        // spelled here and named as a string.
        p8 room[2];
        positive index = 1;
        b32 answer = 0;

        //      -d, -s and -m are the shapes bash's help takes; any other
        //      letter is an error with the same status the reference gives.
        if (index < shell_argc && string_is(shell_argv[index], '-') &&
            string_get(shell_argv[index] + 1) &&
            !word_is(shell_argv[index], "--"))
        {
                for (string_address letter = shell_argv[index] + 1;
                     string_get(letter); letter++)
                        if (!string_is(letter, 'd') && !string_is(letter, 's') &&
                            !string_is(letter, 'm'))
                        {
                                return shell_answer(shell_letter_refused(
                                    "help", string_get(letter),
                                    "help [-dms] [pattern ...]"));
                        }

                index++;
        }

        if (index < shell_argc && word_is(shell_argv[index], "--"))
                index++;

        //      A topic that names no builtin is a failure, as it is in bash.
        if (index < shell_argc)
        {
                for (; index < shell_argc; index++)
                        //      A dash on its own is an option word with no
                        //      letters in it, not a topic to look for.
                        if (!word_is(shell_argv[index], "-") &&
                            !shell_command_builtin_here(
                                shell_argv[index],
                                string_hash_33_length(shell_argv[index])))
                        {
                                shell_diagnostic_where();

                                //      The whole line bash writes, down to
                                //      the two spaces and the three places
                                //      it sends the reader to look.
                                string_format(log_error,
                                    "help: no help topics match `%s'.  "
                                    "Try `help help' or `man -k %s' or "
                                    "`info %s'.\n",
                                    shell_argv[index], shell_argv[index],
                                    shell_argv[index]);
                                answer = 1;
                        }

                return shell_answer(answer);
        }

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
