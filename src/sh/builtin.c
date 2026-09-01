#include "../compiler_memory.c"

const positive page_size = 4096;


// A diagnostic bypasses the builtin's output writer and goes to stderr.
#define shell_diagnostic log_error

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
bool exec_function_here(string_address name);
bool exec_function_unset(string_address name);
static bool exec_line_aborted();
static bool exec_source_stop();
bool shell_builtin(string_address arguments);
string_address shell_arguments();
fn shell_execute_command();
bipolar shell_spawn_tool(string_address address_to arguments,
                         b32 output, bool quiet);
fn parse_nest_enter();
fn parse_nest_leave();
static bool exec_arithmetic_value(string_address text,
                                  bipolar address_to value);

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
        bipolar answered = system_call_3(syscall(execve), (positive)path,
                                         (positive)arguments,
                                         (positive)environment);
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

        if (!fallback || (positive)fallback >= (positive)-4095)
                return answered;

        fallback[0] = (string_address)"/proc/self/exe";
        fallback[1] = path;

        for (positive at = 1; at < count; at++)
                fallback[at + 1] = arguments[at];

        fallback[count + 1] = null;

        answered = system_call_3(syscall(execve),
                                 (positive)fallback[0],
                                 (positive)fallback,
                                 (positive)environment);

        if (answered < 0)
        {
                fallback[0] = (string_address)"/bin/sh";
                answered = system_call_3(syscall(execve),
                                         (positive)fallback[0],
                                         (positive)fallback,
                                         (positive)environment);
        }

        memory_free(fallback, bytes);
        return answered;
}

#define SHELL_DIRECTORY_MAX 4096

extern p8 address_to shell_directory;

bipolar shell_find_in_path_alloc(string_address name,
                                 p8 address_to address_to into,
                                 positive address_to room);
bipolar shell_find_in_path_query_alloc(string_address name,
                                       p8 address_to address_to into,
                                       positive address_to room);
static bipolar shell_find_in_standard_path_alloc(string_address name,
                                                  p8 address_to address_to into,
                                                  positive address_to room,
                                                  bool query);
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
        Names an assignment may no longer touch. Held apart from the values so
        that readonly can be spoken about a name that has none yet.
*/
static shell_store readonly_storage;
static string_address address_to readonly_name;
static positive readonly_room;
static positive readonly_count;

PURE bool env_readonly(const_string name)
{
        return readonly_count &&
               string_table_find((string_address)name, readonly_name,
                                 sizeof(readonly_name[0]), readonly_count) <
                   readonly_count;
}

static bool readonly_add(string_address name, positive length)
{
        string_address kept;

        if (env_readonly(name))
                return true;

        if (!shell_array_room(readonly_name, readonly_room, readonly_count + 1))
                return false;

        kept = shell_store_take(address_of readonly_storage, length + 1);

        if (!kept)
                return false;

        memory_copy_end(kept, name, length);
        readonly_name[readonly_count++] = kept;

        return true;
}

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
        positive temporary;
        bool owned;
        bool permanent;
        bool declared;
} env_variable;

static env_variable address_to shell_vars;
static positive shell_vars_room;
static positive shell_var_count;

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

        while (table[at].index_plus_one)
        {
                if (table[at].index_plus_one == positive_max &&
                    tombstone == slots)
                        tombstone = at;

                at = (at + 1) & (slots - 1);
        }

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

static fn env_variable_drop(positive index)
{
        positive left = shell_var_count - index - 1;
        env_variable dropped = shell_vars[index];

        if (env_index_slots)
                name_index_remove(env_index, env_index_slots, dropped.hash,
                                  index, shell_var_count,
                                  address_of env_index_tombstones);

        if (dropped.owned)
                env_cell_drop(dropped.text);

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

        shell_vars[shell_var_count].text = (string_address)(cell + 1);
        shell_vars[shell_var_count].hash = hash;
        shell_vars[shell_var_count].name_length = length;
        shell_vars[shell_var_count].value_length = 0;
        shell_vars[shell_var_count].temporary = 0;
        shell_vars[shell_var_count].owned = true;
        shell_vars[shell_var_count].permanent = false;
        shell_vars[shell_var_count].declared = true;

        if (!env_index_slots || shell_var_count + 1 > env_index_slots / 2)
                env_index_rebuild(shell_var_count + 1);
        else
                name_index_put(env_index, env_index_slots, hash, length,
                               shell_var_count,
                               address_of env_index_tombstones);

        return shell_vars + shell_var_count++;
}

static env_variable address_to env_export_take(const_string name,
                                                positive length)
{
        return env_export_take_hashed(name, length,
                                      env_name_hash(name, length));
}

static PURE bool env_export_active_span(const_string name, positive length)
{
        positive found = env_find_span(name, length);

        return found < shell_var_count && (shell_vars[found].permanent ||
                                           shell_vars[found].temporary);
}

PURE bool env_exported(string_address name)
{
        return name && env_export_active_span(name, string_length(name));
}

static bool env_declare(string_address name, positive length)
{
        env_variable address_to entry = env_export_take(name, length);

        if (!entry)
                return false;

        entry->declared = true;
        return true;
}

static fn env_declare_restore(string_address name, bool declared)
{
        positive length = string_length(name);
        positive found = env_find_span(name, length);

        if (declared)
        {
                if (found < shell_var_count)
                        shell_vars[found].declared = true;
                else
                        env_declare(name, length);

                return;
        }

        if (found < shell_var_count)
        {
                shell_vars[found].declared = false;

                if (!shell_vars[found].permanent &&
                    !shell_vars[found].temporary &&
                    !env_variable_has_value(shell_vars + found))
                        env_variable_drop(found);
        }
}

static bool env_export_mark_span(const_string name, positive length)
{
        env_variable address_to entry = env_export_take(name, length);

        if (!entry)
                return false;

        entry->permanent = true;
        shell_envp_dirty = true;
        return true;
}

static bool env_export_mark(string_address name)
{
        return env_export_mark_span(name, string_length(name));
}

static fn env_export_restore(string_address name, bool exported)
{
        positive length = string_length(name);
        positive hash = env_name_hash(name, length);
        positive found = env_find_hashed_span(name, length, hash);

        if (exported)
        {
                env_variable address_to entry =
                    found < shell_var_count
                        ? shell_vars + found
                        : env_export_take_hashed(name, length, hash);

                if (entry)
                        entry->permanent = true;
        }
        else if (found < shell_var_count)
        {
                shell_vars[found].permanent = false;

                if (!shell_vars[found].temporary &&
                    !shell_vars[found].declared &&
                    !env_variable_has_value(shell_vars + found))
                        env_variable_drop(found);
        }

        shell_envp_dirty = true;
}

static bool env_export_temporary(string_address assignment)
{
        positive length = (positive)(string_first_of_or_end(assignment, '=') -
                                     assignment);
        env_variable address_to entry = env_export_take(assignment, length);

        if (!entry)
                return false;

        entry->temporary++;
        shell_envp_dirty = true;
        return true;
}

static fn env_export_release(string_address assignment)
{
        positive length = (positive)(string_first_of_or_end(assignment, '=') -
                                     assignment);
        positive found = env_find_span(assignment, length);

        if (found >= shell_var_count)
                return;

        if (shell_vars[found].temporary)
                shell_vars[found].temporary--;

        if (!shell_vars[found].temporary && !shell_vars[found].permanent &&
            !shell_vars[found].declared &&
            !env_variable_has_value(shell_vars + found))
                env_variable_drop(found);

        shell_envp_dirty = true;
}

string_address address_to shell_environment()
{
        static string_address empty[1];
        positive count = 0;

        if (!shell_envp_dirty)
                return shell_envp ? shell_envp : empty;

        for (positive at = 0; at < shell_var_count; at++)
                if (env_variable_has_value(shell_vars + at) &&
                    (shell_vars[at].permanent || shell_vars[at].temporary))
                        count++;

        if (!shell_array_room(shell_envp, shell_envp_room, count + 1))
                return null;

        count = 0;

        for (positive at = 0; at < shell_var_count; at++)
                if (env_variable_has_value(shell_vars + at) &&
                    (shell_vars[at].permanent || shell_vars[at].temporary))
                        shell_envp[count++] = shell_vars[at].text;

        shell_envp[count] = null;
        shell_envp_dirty = false;
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
static bool env_assign_hashed_span(const_string name, positive name_len,
                                   positive hash, const_string value);
PURE string_address env_get(const_string name);
bool env_set(const_string name, const_string value);
bool env_assign(const_string name, const_string value);

/* Adopt a process-lifetime assignment without copying its bytes. */
static bool env_borrow_assignment(string_address entry, bool replace)
{
        string_address mark = string_first_of(entry, '=');
        positive length;
        positive hash;
        positive found;
        bool added = false;

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

                found = shell_var_count++;
                added = true;
        }
        else if (shell_vars[found].owned)
                env_cell_drop(shell_vars[found].text);

        shell_vars[found].text = entry;
        shell_vars[found].hash = hash;
        shell_vars[found].name_length = length;
        shell_vars[found].value_length = string_length(mark + 1);
        shell_vars[found].temporary = 0;
        shell_vars[found].owned = false;
        shell_vars[found].permanent = true;
        shell_vars[found].declared = true;

        if (added)
        {
                if (!env_index_slots || shell_var_count > env_index_slots / 2)
                        env_index_rebuild(shell_var_count);
                else
                        name_index_put(env_index, env_index_slots, hash,
                                       length, found,
                                       address_of env_index_tombstones);
        }

        return true;
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
                // Duplicate names are legal; keep the old last-one-wins
                // behavior without creating a second index entry.
                env_borrow_assignment(process_environment[at], true);

        // Programs live at the root of the image, so it is on the path.
        string_address defaults[] = {"PATH=/bin:/usr/bin:/", "SHELL=/bin/sh",
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

        if (value_length)
                address_to value_length = shell_vars[index].value_length;

        return shell_vars[index].text + length + 1;
}

PURE string_address env_get_span(const_string name, positive length)
{
        if (name == null)
                return null;

        return env_get_hashed_span(name, length,
                                   env_name_hash(name, length), null);
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

static bool env_write_hashed_span(const_string name, positive name_len,
                                  positive hash, const_string value,
                                  bool assignment)
{
        bool allexport = assignment && (shell_options & SHELL_FLAG('a'));

        if (!name || !value)
                return false;

        // Every path remembered was an answer about the old PATH.
        if (name_len == 4 && !memory_compare(name, "PATH", 4))
                hash_forget();

        positive value_len = string_length(env_reading(value));
        positive needed = name_len + 1 + value_len + 1;

        positive idx = env_find_hashed_span(name, name_len, hash);
        env_cell address_to cell;

        if (idx == shell_var_count && !env_table_room(shell_var_count + 1))
                return false;

        if (idx < shell_var_count && shell_vars[idx].owned)
        {
                cell = ((env_cell address_to)shell_vars[idx].text) - 1;

                if (cell->room >= needed)
                {
                        memory_copy((p8 address_to)(cell + 1), env_reading(name), name_len);
                        ((p8 address_to)(cell + 1))[name_len] = '=';
                        memory_copy_end((p8 address_to)(cell + 1) + name_len + 1,
                                        env_reading(value), value_len);
                        shell_vars[idx].value_length = value_len;
                        shell_vars[idx].declared = true;
                        if (allexport)
                                shell_vars[idx].permanent = true;
                        if (!shell_envp_dirty &&
                            (shell_vars[idx].permanent ||
                             shell_vars[idx].temporary))
                                shell_envp_dirty = true;
                        return true;
                }
        }

        cell = env_cell_take(needed);

        if (!cell)
                return false;

        memory_copy((p8 address_to)(cell + 1), env_reading(name), name_len);
        ((p8 address_to)(cell + 1))[name_len] = '=';
        memory_copy_end((p8 address_to)(cell + 1) + name_len + 1,
                        env_reading(value), value_len);

        if (idx < shell_var_count)
        {
                string_address old = shell_vars[idx].text;
                bool owned = shell_vars[idx].owned;

                shell_vars[idx].text = (string_address)(cell + 1);
                shell_vars[idx].value_length = value_len;
                shell_vars[idx].owned = true;
                shell_vars[idx].declared = true;
                if (owned)
                        env_cell_drop(old);
        }
        else
        {
                shell_vars[shell_var_count].text = (string_address)(cell + 1);
                shell_vars[shell_var_count].hash = hash;
                shell_vars[shell_var_count].name_length = name_len;
                shell_vars[shell_var_count].value_length = value_len;
                shell_vars[shell_var_count].temporary = 0;
                shell_vars[shell_var_count].owned = true;
                shell_vars[shell_var_count].permanent = allexport;
                shell_vars[shell_var_count].declared = true;
                shell_var_count++;

                if (!env_index_slots || shell_var_count > env_index_slots / 2)
                        env_index_rebuild(shell_var_count);
                else
                        name_index_put(env_index, env_index_slots, hash,
                                       name_len, shell_var_count - 1,
                                       address_of env_index_tombstones);
        }

        if (allexport)
                shell_vars[idx].permanent = true;

        if (!shell_envp_dirty &&
            (shell_vars[idx].permanent || shell_vars[idx].temporary))
                shell_envp_dirty = true;

        return true;
}

static bool env_write(const_string name, const_string value, bool assignment)
{
        positive2 answer;

        if (!name || !value || env_readonly(name))
                return false;

        answer = string_hash_33_length(env_reading(name));
        return env_write_hashed_span(name, answer.y, answer.x, value,
                                     assignment);
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

// string_to_positive scans backwards from the end of the string, so it reads
// "0.5" as 5 and anything with a trailing space as 0. Arguments arrive as
// whole words here and have to be read forwards.
positive shell_number(string_address input)
{
        return input ? string_digits(input, 0) : 0;
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

static bool shell_valid_name(string_address name, positive length)
{
        if (!length || (string_get(name) >= '0' && string_get(name) <= '9'))
                return false;

        for (positive at = 0; at < length; at++)
                if (!expand_name_character(string_get(name + at)))
                        return false;

        return true;
}

static fn shell_bad_name(string_address command, string_address name,
                         positive length)
{
        string_format(shell_diagnostic, "%s: bad variable name: ", command);
        shell_diagnostic(name, length);
        shell_diagnostic("\n", 1);
        expand_fatal();
}

static positive shell_declaration_options(bool address_to listed)
{
        positive index = 1;

        address_to listed = shell_argc < 2;

        while (index < shell_argc && word_is(shell_argv[index], "-p"))
        {
                address_to listed = true;
                index++;
        }

        if (index < shell_argc && word_is(shell_argv[index], "--"))
                index++;

        return index;
}

fn shell_export(writer write, string_address input)
{
        bool listed;
        positive index = shell_declaration_options(address_of listed);

        if (listed && index >= shell_argc)
        {
                for (positive at = 0; at < shell_var_count; at++)
                {
                        env_variable address_to variable = shell_vars + at;

                        if (!variable->permanent)
                                continue;

                        write("export ", 7);
                        write(variable->text, variable->name_length);

                        if (env_variable_has_value(variable))
                        {
                                write("=", 1);
                                shell_quoted(write, variable->text +
                                                         variable->name_length +
                                                         1);
                        }

                        write("\n", 1);
                }

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index++];
                string_address mark = string_first_of(word, '=');
                positive length = mark ? (positive)(mark - word)
                                       : string_length(word);

                if (!shell_valid_name(word, length))
                {
                        shell_bad_name("export", word, length);
                        return;
                }

                // A name on its own is already exported here: every variable
                // assigned to it later inherits the export attribute.
                if (!mark)
                {
                        if (!env_export_mark(word))
                        {
                                shell_diagnostic("export: no room\n", 0);
                                return shell_answer(2);
                        }

                        continue;
                }

                address_to mark = end;

                if (env_readonly(word))
                {
                        string_format(shell_diagnostic, "%s: is read only\n", word);
                        address_to mark = '=';
                        expand_fatal();
                        return;
                }

                if (!env_set(word, mark + 1) || !env_export_mark(word))
                {
                        address_to mark = '=';
                        shell_diagnostic("export: no room\n", 0);
                        return shell_answer(2);
                }

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
/* Four bytes immediately before the logical directory spell its assignment
   name. An empty-environment shell can therefore publish the getcwd result as
   `PWD=value` without a second buffer or allocation. On the first cd or PWD
   assignment the ordinary borrowed-record COW path takes ownership. */
static p8 shell_directory_assignment[SHELL_DIRECTORY_MAX + 4];
p8 address_to shell_directory = shell_directory_assignment + 4;
static p8 shell_directory_was[SHELL_DIRECTORY_MAX];

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

static bool shell_cd_variable(string_address name, string_address value)
{
        if (env_assign(name, value))
                return true;

        string_format(shell_diagnostic,
                      env_readonly(name) ? "cd: %s: is read only\n"
                                         : "cd: cannot assign %s\n",
                      name);
        return false;
}

bool shell_directory_moved(string_address logical)
{
        string_copy_max_end(shell_directory_was, shell_directory,
                            sizeof(shell_directory_was) - 1);

        string_copy_max_end(shell_directory, logical, SHELL_DIRECTORY_MAX - 1);

        return shell_cd_variable("OLDPWD", shell_directory_was) &&
               shell_cd_variable("PWD", shell_directory);
}

static p8 shell_cd_target[4096];

bool shell_cd_try(string_address candidate, bool physical,
                  bool address_to physical_named,
                  bool address_to variables_set)
{
        p8 wanted[4096];

        string_copy_max_end(wanted, candidate, sizeof(wanted) - 1);

        if (!physical)
                shell_path_tidy(wanted);

        if (system_call_1(syscall(chdir), (positive)wanted))
                return false;

        address_to physical_named = !physical || shell_here(wanted, sizeof(wanted));

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
bool shell_cd_walk(bool physical, bool address_to say,
                   bool address_to physical_named,
                   bool address_to variables_set)
{
        p8 candidate[4096];

        if (shell_cd_target[0] == '/')
                return shell_cd_try(shell_cd_target, physical, physical_named,
                                    variables_set);

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

                        string_copy_max_end(search, value, sizeof(search) - 1);
                        segment = search;

                        while (segment)
                        {
                                string_address next = string_cut(segment, ':');

                                path_join(candidate, sizeof(candidate),
                                          string_get(segment) ? segment
                                                              : shell_directory,
                                          shell_cd_target);

                                if (shell_cd_try(candidate, physical,
                                                 physical_named,
                                                 variables_set))
                                {
                                        address_to say = true;
                                        return true;
                                }

                                segment = next;
                        }
                }
        }

        path_join(candidate, sizeof(candidate), shell_directory,
                  shell_cd_target);

        return shell_cd_try(candidate, physical, physical_named,
                            variables_set);
}

fn shell_cd(writer write, string_address input)
{
        positive index = 1;
        bool physical = false;
        bool error_if_unnamed = false;
        bool physical_named = true;
        bool variables_set = true;
        string_address name = null;
        bool say = false;

        if (!shell_directory_holds())
        {
                shell_here(shell_directory, SHELL_DIRECTORY_MAX);
                env_assign("PWD", shell_directory);
        }

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address option = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(option))
                {
                        p8 letter = string_get(option++);

                        if (letter == 'L')
                                physical = false;
                        else if (letter == 'P')
                                physical = true;
                        else if (letter == 'e')
                                error_if_unnamed = true;
                        else
                        {
                                string_format(shell_diagnostic,
                                              "cd: bad option: -%c\n", letter);
                                return shell_answer(2);
                        }
                }

                index++;
        }

        if (index < shell_argc)
                name = shell_argv[index++];

        if (index < shell_argc)
        {
                shell_diagnostic("cd: too many arguments\n", 0);
                return shell_answer(2);
        }

        if (!name)
        {
                name = env_get("HOME");

                if (!name || !string_get(name))
                        return shell_answer(0);
        }
        else if (!string_get(name))
        {
                shell_diagnostic("cd: empty directory\n", 0);
                return shell_answer(2);
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
        string_copy_max_end(shell_cd_target, name, sizeof(shell_cd_target) - 1);

        if (!shell_cd_walk(physical, address_of say,
                           address_of physical_named,
                           address_of variables_set))
        {
                shell_answer(2);

                return string_format(shell_diagnostic, "cd: can't cd to %s\n",
                                     shell_cd_target);
        }

        if (!variables_set)
                return shell_answer(2);

        if (say)
                string_format(write, "%s\n", shell_directory);

        shell_answer(error_if_unnamed && physical && !physical_named ? 1 : 0);
}

fn shell_clear(writer write, string_address input)
{
        write(str(TERM_CLEAR_SCREEN));
}

// echo and printf %b share the shell escape language. The implementation sits
// with printf below; these two flags also let \c stop echo's remaining words
// and final newline.
static bool printf_cut;
static bool printf_in_b;
fn printf_escaped(writer write, string_address text);

fn shell_echo(writer write, string_address input)
{
        positive index = 1;
        bool newline = true;

        printf_cut = false;

        while (index < shell_argc && word_is(shell_argv[index], "-n"))
        {
                newline = false;
                index++;
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

fn shell_exec(writer write, string_address input)
{
        p8 address_to found = null;
        positive found_room = 0;
        string_address address_to environment;
        bipolar located;

        // With nothing to run, exec is only there for the redirections that
        // were already applied to get here.
        if (shell_argc < 2)
                return shell_answer(0);

        located = shell_find_in_path_alloc(shell_argv[1], address_of found,
                                           address_of found_room);

        if (located < 0)
        {
                if (found)
                        memory_free(found, found_room);

                shell_answer(2);
                string_format(shell_diagnostic, "exec: no room\n");
                shell_stop_when_scripted(2);

                return;
        }

        if (!located)
        {
                if (found)
                        memory_free(found, found_room);

                shell_answer(127);
                string_format(shell_diagnostic, "exec: %s: not found\n",
                              shell_argv[1]);
                shell_stop_when_scripted(127);

                return;
        }

        environment = shell_environment();
        if (!environment)
        {
                memory_free(found, found_room);
                shell_answer(2);
                string_format(shell_diagnostic, "exec: no room for environment\n");
                shell_stop_when_scripted(2);

                return;
        }

        log_flush();

        // From argv[1] on, so the new program is named by what it was asked
        // for and not by the word "exec".
        shell_exec_file(found, shell_argv + 1, shell_argc - 1, environment);

        memory_free(found, found_room);
        shell_answer(126);
        string_format(shell_diagnostic, "exec: %s: cannot run\n", shell_argv[1]);
        shell_stop_when_scripted(126);
}







#define STORAGE_ADAPTER(name, command)                                      \
        fn shell_##name(writer output, string_address input)                \
        {                                                                   \
                (void)input;                                                \
                shell_answer(command(shell_argc, shell_argv, output,        \
                                     shell_diagnostic));                    \
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

DEAD_END fn shell_exit(writer write, string_address input)
{
        bipolar exit_code = shell_status_entering;
        bool good = true;

        if (shell_argc > 1)
        {
                exit_code = shell_signed(shell_argv[1], address_of good);

                // dash accepts the optional plus sign and wraps non-negative
                // values to one byte, but a negative or non-number is an
                // illegal operand and terminates the shell with status 2.
                if (!good || exit_code < 0)
                {
                        string_format(shell_diagnostic, "exit: Illegal number: %s\n",
                                      shell_argv[1]);
                        exit_code = 2;
                }
                else
                        exit_code &= 0xff;
        }

        shell_status = (b32)exit_code;
        shell_trap_exit();

        log_flush();

        exit(exit_code);
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
#define ACCESS_EXECUTE 1

// The shell's own ioctl, spelled here because shell.c names it after this file
// has already been read.
#define BUILTIN_TCGETS 0x5401u

PURE bool word_is(string_address word, string_address text)
{
        return word && !string_compare(word, text);
}

static fn env_unset_span(string_address name, positive length)
{
        positive index = env_find_span(name, length);

        if (index < shell_var_count)
                env_variable_drop(index);
}

fn env_unset(string_address name)
{
        env_unset_span(name, string_length(env_reading(name)));
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
typedef struct
{
        string_address name;
        p8 letter;
} shell_option;

static shell_option shell_option_names[] = {
    {"errexit", 'e'},    {"noglob", 'f'},   {"ignoreeof", 'I'},
    {"interactive", 'i'}, {"monitor", 'm'}, {"noexec", 'n'},
    {"stdin", 's'},      {"xtrace", 'x'},   {"verbose", 'v'},
    {"vi", 0},           {"emacs", 0},      {"noclobber", 'C'},
    {"allexport", 'a'},  {"notify", 'b'},   {"nounset", 'u'},
    {"nolog", 0},        {"pipefail", 0},   {"debug", 0},
    {null, 0},
};

#define SHELL_OPTION_NAMES \
        (sizeof(shell_option_names) / sizeof(shell_option_names[0]))
#define SHELL_OPTION_MONITOR 4
#define SHELL_OPTION_NOCLOBBER 11
#define SHELL_OPTION_PIPEFAIL 16

static positive shell_options_named;

PURE bool shell_option_on(positive index)
{
        if (shell_option_names[index].letter >= 'a' &&
            shell_option_names[index].letter <= 'z')
                return (shell_options & SHELL_FLAG(shell_option_names[index].letter)) != 0;

        return (shell_options_named & ((positive)1 << index)) != 0;
}

fn shell_option_told(positive index, bool on)
{
        // A monitor bit without process groups and terminal ownership would
        // advertise job control the executor does not have.
        if (index == SHELL_OPTION_MONITOR && on)
                return;

        if (shell_option_names[index].letter >= 'a' &&
            shell_option_names[index].letter <= 'z')
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
        static p8 flags[16];
        static p8 order[] = "ubaCvxsiImfne";
        positive into = 0;

        for (positive at = 0; string_get(order + at); at++)
        {
                p8 letter = order[at];
                positive index;

                for (index = 0; index < SHELL_OPTION_NAMES; index++)
                        if (shell_option_names[index].letter == letter)
                                break;

                if (index < SHELL_OPTION_NAMES && shell_option_on(index))
                        flags[into++] = letter;
        }

        flags[into] = end;
        return flags;
}

// Entry mode supplies the initial s/i state. From this point on they are
// ordinary set options: `set +s` and `set +i` must also disappear from `$-`.
fn shell_options_started(bool interactive)
{
        if (string_first_of(shell_option_flags, 's'))
                shell_options |= SHELL_FLAG('s');

        if (interactive)
                shell_options |= SHELL_FLAG('i');
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
PURE bool shell_pipefail()
{
        return (shell_options_named &
                ((positive)1 << SHELL_OPTION_PIPEFAIL)) != 0;
}

PURE bool shell_noclobber()
{
        return (shell_options_named &
                ((positive)1 << SHELL_OPTION_NOCLOBBER)) != 0;
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
                        string_to_field(write, shell_option_names[index].name,
                                        15, ' ', true);

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
                for (positive at = 0; at < shell_var_count; at++)
                        if (env_variable_has_value(shell_vars + at))
                                string_format(write, "%s\n",
                                              shell_vars[at].text);

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

                                positive option;

                                for (option = 0; option < SHELL_OPTION_NAMES;
                                     option++)
                                        if (shell_option_names[option].letter == value)
                                                break;

                                if (option < SHELL_OPTION_NAMES)
                                        shell_option_told(option, on);
                                else
                                {
                                        p8 said[2] = {value, end};

                                        string_format(shell_diagnostic,
                                                      "set: Illegal option %s%s\n",
                                                      on ? "-" : "+", said);
                                        shell_answer(2);
                                        expand_fatal();
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
        {
                shell_diagnostic("set: no room for arguments\n", 0);
                return shell_answer(2);
        }

        shell_answer(0);
}

fn shell_shift(writer write, string_address input)
{
        positive amount = 1;

        if (shell_argc > 1)
        {
                bool good;
                bipolar asked = shell_signed(shell_argv[1], address_of good);

                if (!good || asked < 0)
                {
                        string_format(shell_diagnostic, "shift: Illegal number: %s\n",
                                      shell_argv[1]);
                        expand_fatal();
                        return;
                }

                amount = (positive)asked;
        }

        if (amount > shell_parameter_count)
        {
                shell_diagnostic("shift: can't shift that many\n", 0);
                expand_fatal();
                return;
        }

        shell_parameters_shift(amount);

        shell_answer(0);
}

fn shell_unset(writer write, string_address input)
{
        positive index = 1;
        bool functions = false;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                string_address option = shell_argv[index] + 1;

                while (string_get(option))
                {
                        if (string_get(option) == 'f')
                                functions = true;
                        else if (string_get(option) == 'v')
                                functions = false;
                        else
                                return shell_answer(2);

                        option++;
                }

                index++;
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];

                if (!shell_valid_name(word, string_length(word)))
                {
                        shell_bad_name("unset", word, string_length(word));
                        return;
                }

                if (!functions && env_readonly(word))
                {
                        string_format(shell_diagnostic, "%s: is read only\n", word);
                        expand_fatal();
                        return;
                }

                if (functions)
                        exec_function_unset(word);
                else
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
typedef struct
{
        p8 address_to text;
        positive name_length;
        positive value_length;
        bool exported;
        bool declared;
        bool present;
} shell_local_entry;

static shell_local_entry address_to local_table;
static positive local_room;
static positive local_count;
static positive local_initialized;
static positive address_to local_from;
static positive local_from_room;
static positive local_depth;

bool shell_local_enter()
{
        if (local_depth == positive_max ||
            !shell_array_room(local_from, local_from_room, local_depth + 1))
        {
                string_format(shell_diagnostic, "No room for function locals\n");
                return false;
        }

        local_from[local_depth] = local_count;
        local_depth++;
        return true;
}

fn shell_local_leave()
{
        positive at;

        if (!local_depth)
                return;

        local_depth--;

        at = local_count;

        // Backwards, so that a name saved twice ends on the value it had
        // before the first of them.
        while (at > local_from[local_depth])
        {
                at--;

                if (!local_table[at].present)
                        env_unset(local_table[at].text);
                else
                        env_set(local_table[at].text,
                                local_table[at].text +
                                    local_table[at].name_length + 1);

                env_export_restore(local_table[at].text,
                                   local_table[at].exported);
                env_declare_restore(local_table[at].text,
                                    local_table[at].declared);
        }

        local_count = local_from[local_depth];
}

static bool local_text_room(shell_local_entry address_to entry,
                            positive used, positive wanted)
{
        env_cell address_to old = entry->text
                                      ? ((env_cell address_to)entry->text) - 1
                                      : null;
        env_cell address_to made;

        if (old && old->room >= wanted)
                return true;

        made = env_cell_take(wanted);

        if (!made)
                return false;

        if (used)
                memory_copy(made + 1, entry->text, used);
        if (old)
                env_cell_drop(entry->text);
        entry->text = (p8 address_to)(made + 1);
        return true;
}

// -1 is allocation failure, zero was already local in this frame, and one is
// the first declaration here. Callers need that distinction because Bash
// `declare x` hides an outer value but a second declaration keeps the local.
static b32 local_remember(string_address name)
{
        positive begin = local_depth ? local_from[local_depth - 1] : 0;
        positive name_length;
        positive value_length = 0;
        positive wanted;
        positive found;
        positive2 name_info;
        env_variable address_to variable;

        // Twice in one function is once. Without this a local in a loop fills
        // the table an iteration at a time.
        for (positive at = begin; at < local_count; at++)
                if (!string_compare(local_table[at].text, name))
                        return 0;

        if (local_count == positive_max ||
            !shell_array_room(local_table, local_room, local_count + 1))
                return -1;

        if (local_count == local_initialized)
        {
                local_table[local_count].text = null;
                local_initialized++;
        }

        name_info = string_hash_33_length(name);
        name_length = name_info.y;
        found = env_find_hashed_span(name, name_length, name_info.x);
        variable = found < shell_var_count ? shell_vars + found : null;
        local_table[local_count].exported =
            variable && (variable->permanent || variable->temporary);
        local_table[local_count].declared = variable && variable->declared;
        local_table[local_count].present =
            variable && env_variable_has_value(variable);

        if (local_table[local_count].present)
                value_length = variable->value_length;

        if (name_length == positive_max ||
            value_length > positive_max - name_length - 2)
                return -1;

        wanted = name_length + 1 +
                 (local_table[local_count].present ? value_length + 1 : 0);

        if (!local_text_room(local_table + local_count, 0, wanted))
                return -1;

        memory_copy_end(local_table[local_count].text, name, name_length);
        local_table[local_count].name_length = name_length;
        local_table[local_count].value_length = value_length;

        if (local_table[local_count].present)
                memory_copy_end(local_table[local_count].text + name_length + 1,
                                variable->text + name_length + 1,
                                value_length);

        local_count++;

        return 1;
}

static PURE shell_local_entry address_to local_saved_global(string_address name)
{
        for (positive at = 0; at < local_count; at++)
                if (!string_compare(local_table[at].text, name))
                        return local_table + at;

        return null;
}

static bool local_saved_assign(shell_local_entry address_to entry,
                               string_address value, bool append)
{
        positive old_length = append && entry->present ? entry->value_length : 0;
        positive add_length = string_length(value);
        positive prefix = entry->name_length + 1;

        if (old_length > positive_max - add_length - 1 ||
            prefix > positive_max - old_length - add_length - 1 ||
            !local_text_room(entry, prefix + old_length,
                             prefix + old_length + add_length + 1))
                return false;

        memory_copy_end(entry->text + prefix + old_length, value, add_length);
        entry->value_length = old_length + add_length;
        entry->present = true;
        entry->declared = true;
        return true;
}

fn shell_local(writer write, string_address input)
{
        positive index = 1;
        bool failed = false;

        if (!local_depth)
        {
                shell_diagnostic("local: not in a function\n", 0);
                expand_fatal();
                return;
        }

        while (index < shell_argc)
        {
                string_address word = shell_argv[index++];
                string_address mark = string_first_of(word, '=');
                string_address name_end = mark;
                positive length = mark ? (positive)(name_end - word)
                                       : string_length(word);
                p8 delimiter = mark ? string_get(name_end) : 0;

                if (!shell_valid_name(word, length))
                {
                        shell_diagnostic("local: bad name\n", 0);
                        shell_answer(2);
                        failed = true;
                        break;
                }

                if (mark)
                        address_to name_end = end;

                if (local_remember(word) < 0)
                {
                        shell_diagnostic("local: too many\n", 0);
                        shell_answer(2);
                        failed = true;
                        if (mark)
                                address_to name_end = delimiter;
                        break;
                }

                if (mark && !env_assign(word, mark + 1))
                {
                        shell_diagnostic("local: no room\n", 0);
                        shell_answer(2);
                        failed = true;
                }

                if (mark)
                        address_to name_end = delimiter;

                if (failed)
                        break;
        }

        if (!failed)
                shell_answer(0);
}

#define DECLARE_EXPORT 1
#define DECLARE_READONLY 2
#define DECLARE_PRINT 4
#define DECLARE_GLOBAL 8

typedef struct
{
        positive index;
        p8 set;
        p8 clear;
} shell_declare_state;

static bool shell_declare_options(shell_declare_state address_to state)
{
        while (state->index < shell_argc)
        {
                string_address word = shell_argv[state->index];
                p8 direction = string_get(word);
                string_address option;

                if ((direction != '-' && direction != '+') ||
                    !string_get(word + 1))
                        break;

                if (word_is(word, "--"))
                {
                        state->index++;
                        break;
                }

                option = word + 1;

                while (string_get(option))
                {
                        p8 value = string_get(option++);
                        p8 flag;

                        flag = value == 'x' ? DECLARE_EXPORT
                               : value == 'r' ? DECLARE_READONLY
                               : value == 'p' && direction == '-' ? DECLARE_PRINT
                               : value == 'g' && direction == '-' ? DECLARE_GLOBAL
                                                                  : 0;

                        if (!flag)
                        {
                                string_format(shell_diagnostic,
                                              "%s: %c%c: invalid option\n",
                                              shell_argv[0], direction, value);
                                shell_answer(2);
                                return false;
                        }

                        if (direction == '-')
                                state->set |= flag;
                        else
                                state->clear |= flag;
                }

                state->index++;
        }

        state->set &= (p8)~state->clear;
        return true;
}

static fn shell_declare_quoted(writer write, string_address value)
{
        bool control = false;
        string_address at = value;

        while (string_get(at))
        {
                p8 byte = string_get(at++);

                if (byte < ' ' || byte == 127)
                {
                        control = true;
                        break;
                }
        }

        write(control ? "$'" : "\"", 2 - !control);

        while (string_get(value))
        {
                p8 byte = string_get(value++);

                if (control)
                {
                        if (byte == '\n')
                                write("\\n", 2);
                        else if (byte == '\r')
                                write("\\r", 2);
                        else if (byte == '\t')
                                write("\\t", 2);
                        else if (byte < ' ' || byte == 127)
                        {
                                p8 octal[4] = {'\\',
                                               (p8)('0' + (byte >> 6)),
                                               (p8)('0' + ((byte >> 3) & 7)),
                                               (p8)('0' + (byte & 7))};

                                write(octal, sizeof(octal));
                        }
                        else
                        {
                                if (byte == '\\' || byte == '\'')
                                        write("\\", 1);
                                write(address_of byte, 1);
                        }
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

static bool shell_declare_print_one(writer write, string_address name,
                                    positive length, p8 filter)
{
        positive found = env_find_span(name, length);
        env_variable address_to variable =
            found < shell_var_count ? shell_vars + found : null;
        bool readonly = env_readonly((const_string)name);
        bool exported = variable && variable->permanent;

        if ((!variable || !variable->declared) && !readonly)
                return false;

        if ((filter & DECLARE_EXPORT) && !exported)
                return false;
        if ((filter & DECLARE_READONLY) && !readonly)
                return false;

        write("declare -", 9);

        if (!readonly && !exported)
                write("-", 1);
        else
        {
                if (readonly)
                        write("r", 1);
                if (exported)
                        write("x", 1);
        }

        write(" ", 1);
        write(name, length);

        if (variable && env_variable_has_value(variable))
        {
                write("=", 1);
                shell_declare_quoted(write,
                                     variable->text + length + 1);
        }

        write("\n", 1);
        return true;
}

static bool shell_declare_print_all(writer write, p8 filter)
{
        positive count = shell_var_count;
        shell_mark held = shell_store_mark(address_of expand_store);
        string_address address_to names;

        if (readonly_count > positive_max - count ||
            count + readonly_count > positive_max / sizeof(names[0]))
                return false;

        names = (string_address address_to)shell_store_take(
            address_of expand_store,
            (count + readonly_count) * sizeof(names[0]));

        if (!names && count + readonly_count)
                goto failed;

        count = 0;

        for (positive at = 0; at < shell_var_count; at++)
        {
                positive length = shell_vars[at].name_length;
                p8 address_to name = shell_store_take(address_of expand_store,
                                                        length + 1);

                if (!name)
                        goto failed;

                memory_copy_end(name, shell_vars[at].text, length);
                names[count++] = name;
        }

        for (positive at = 0; at < readonly_count; at++)
        {
                positive length = string_length(readonly_name[at]);

                if (env_find_span(readonly_name[at], length) >= shell_var_count)
                        names[count++] = readonly_name[at];
        }

        if (!expand_sort_names(names, count))
                goto failed;

        for (positive at = 0; at < count; at++)
                shell_declare_print_one(write, names[at],
                                        string_length(names[at]), filter);

        shell_store_rewind(address_of expand_store, held);
        return true;

failed:
        shell_store_rewind(address_of expand_store, held);
        return false;
}

static bool shell_declare_assign(string_address name, string_address value,
                                 bool append)
{
        shell_mark held;
        p8 address_to joined;
        string_address old;
        positive old_length;
        positive add_length;
        bool answer;

        if (!append)
                return env_assign(name, value);

        old = env_get(name);
        old_length = old ? string_length(old) : 0;
        add_length = string_length(value);

        if (old_length > positive_max - add_length - 1)
                return false;

        held = shell_store_mark(address_of expand_store);
        joined = shell_store_take(address_of expand_store,
                                  old_length + add_length + 1);

        if (!joined)
        {
                shell_store_rewind(address_of expand_store, held);
                return false;
        }

        if (old_length)
                memory_copy(joined, old, old_length);
        memory_copy_end(joined + old_length, value, add_length);
        answer = env_assign(name, joined);
        shell_store_rewind(address_of expand_store, held);
        return answer;
}

static fn shell_declare(writer write, string_address input)
{
        shell_declare_state state = {1};
        bool failed = false;

        (void)input;

        if (!shell_declare_options(address_of state))
                return;

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
                                        string_format(shell_diagnostic,
                                                      "%s: %s: not found\n",
                                                      shell_argv[0], name);
                                        failed = true;
                                }
                        }
                }
                else
                        failed = !shell_declare_print_all(write, state.set);

                shell_answer(failed ? 1 : 0);
                return;
        }

        while (state.index < shell_argc)
        {
                string_address word = shell_argv[state.index++];
                string_address mark = string_first_of(word, '=');
                bool append = mark && mark > word && string_is(mark - 1, '+');
                string_address name_end = mark ? mark - append : null;
                positive length = mark ? (positive)(name_end - word)
                                       : string_length(word);
                bool scoped = local_depth && !(state.set & DECLARE_GLOBAL);
                shell_local_entry address_to saved_global = null;
                p8 delimiter = mark ? string_get(name_end) : 0;
                bool readonly;

                if (!shell_valid_name(word, length))
                {
                        shell_bad_name(shell_argv[0], word, length);
                        return;
                }

                if (mark)
                        address_to name_end = end;

                readonly = env_readonly(word);
                saved_global = state.set & DECLARE_GLOBAL
                                   ? local_saved_global(word)
                                   : null;

                /* Dynamic locals are stacked newest-last. The earliest entry
                   for a name owns the saved global underneath every active
                   local. Update that stable slot and leave the live local
                   alone; the ordinary unwind publishes it at global scope. */
                if (saved_global)
                {
                        if ((state.set & DECLARE_READONLY) ||
                            ((state.clear & DECLARE_READONLY) &&
                             readonly) || (mark && readonly))
                        {
                                string_format(shell_diagnostic,
                                              "%s: %s: readonly variable\n",
                                              shell_argv[0], word);
                                failed = true;
                        }
                        else if (mark && !local_saved_assign(
                                                 saved_global, mark + 1,
                                                 append))
                                goto no_room;
                        else
                        {
                                saved_global->declared = true;
                                if (state.clear & DECLARE_EXPORT)
                                        saved_global->exported = false;
                                if (state.set & DECLARE_EXPORT)
                                        saved_global->exported = true;
                        }

                        goto next;
                }

                if (scoped && readonly)
                {
                        string_format(shell_diagnostic,
                                      "%s: %s: readonly variable\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }

                /* readonly_name is process-global today. Marking a dynamic
                   local there would make the attribute survive the function,
                   which is worse than refusing the unsupported scoped form. */
                if (scoped && (state.set & DECLARE_READONLY))
                {
                        string_format(shell_diagnostic,
                                      "%s: %s: local readonly unsupported\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }

                if ((state.clear & DECLARE_READONLY) && readonly)
                {
                        string_format(shell_diagnostic,
                                      "%s: %s: readonly variable\n",
                                      shell_argv[0], word);
                        failed = true;
                        goto next;
                }

                if (scoped)
                {
                        b32 fresh = local_remember(word);

                        if (fresh < 0)
                                goto no_room;

                        if (fresh)
                        {
                                env_unset(word);
                        }
                }

                if (mark && readonly)
                {
                        string_format(shell_diagnostic, "%s: is read only\n",
                                      word);
                        address_to name_end = delimiter;
                        expand_fatal();
                        return;
                }
                else if (mark ? !shell_declare_assign(word, mark + 1, append)
                              : !env_declare(word, length))
                        goto no_room;

                if (state.clear & DECLARE_EXPORT)
                        env_export_restore(word, false);
                if ((state.set & DECLARE_EXPORT) && !env_export_mark(word))
                        failed = true;
                if ((state.set & DECLARE_READONLY) &&
                    !readonly_add(word, length))
                        failed = true;

        next:
                if (mark)
                        address_to name_end = delimiter;
                continue;

        no_room:
                shell_diagnostic("declare: no room\n", 0);
                if (mark)
                        address_to name_end = delimiter;
                return shell_answer(2);
        }

        shell_answer(failed ? 1 : 0);
}

fn shell_readonly(writer write, string_address input)
{
        bool listed;
        positive index = shell_declaration_options(address_of listed);

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

                if (!shell_valid_name(word, length))
                {
                        shell_bad_name("readonly", word, length);
                        return;
                }

                if (mark)
                {
                        bool set;

                        address_to mark = end;

                        if (env_readonly(word))
                        {
                                string_format(shell_diagnostic, "%s: is read only\n", word);
                                address_to mark = '=';
                                expand_fatal();
                                return;
                        }

                        set = env_assign(word, mark + 1);

                        if (!set || !readonly_add(word, length))
                        {
                                address_to mark = '=';
                                shell_diagnostic("readonly: no room\n", 0);
                                return shell_answer(2);
                        }

                        address_to mark = '=';
                }
                else if (!readonly_add(word, length))
                {
                        shell_diagnostic("readonly: no room\n", 0);
                        return shell_answer(2);
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
PURE b64 test_modified(file_facts address_to facts)
{
        return facts->modified.seconds;
}

PURE p32 test_modified_fraction(file_facts address_to facts)
{
        return facts->modified.nanoseconds;
}

PURE bool test_is_unary(string_address word)
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

PURE positive test_is_binary(string_address word)
{
        p8 first;
        p8 second;

        if (!word)
                return 0;

        first = string_get(word);
        second = string_get(word + 1);

        /* Every binary operator is one or three bytes. Decode that fixed
           grammar directly: a chain of generic NUL comparisons made the
           overwhelmingly common -lt walk across =, !=, -eq and -ne first on
           every loop condition. This is shell syntax, not a reusable byte
           primitive, and its irreducible work is the operator's three bytes. */
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

        if (first == '!' && second == '=' && !string_get(word + 2))
                return TEST_DIFFERENT;

        if (first == '-' && string_get(word + 2) && !string_get(word + 3))
        {
                p16 pair = ((p16)second << 8) | string_get(word + 2);

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

        if (kind == TEST_AFTER)
                return string_compare(left, right) > 0;

        first = shell_signed(left, address_of first_good);
        second = shell_signed(right, address_of second_good);

        if (!first_good || !second_good)
        {
                test_bad = true;
                return false;
        }

        return test_ordered(kind, first, second);
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
static b32 printf_status;

static p8 address_to printf_hold;
static positive printf_hold_room;
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

        memory_fill(printf_plain + 1, 1, STRING_SET_BYTES - 1);
        memory_fill(printf_text + 1, 1, STRING_SET_BYTES - 1);

        printf_plain['\\'] = printf_plain['%'] = 0;
        printf_text['\\'] = 0;
        printf_sets_ready = true;
}

static fn printf_holder(address_any data, positive length)
{
        if (length > positive_max - printf_held ||
            !shell_room((address_any address_to)address_of printf_hold,
                        address_of printf_hold_room, printf_held + length, 1))
        {
                if (!printf_cut)
                        shell_diagnostic("printf: no room\n", 0);

                printf_status = 2;
                printf_cut = true;
                return;
        }

        memory_copy_apart(printf_hold + printf_held, data, length);
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
                if (string_is(step, '0'))
                        step++;

                positive used;
                positive number = string_digits_octal_escape_max(
                    step, 3, address_of used);

                step += used;

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
        positive style = sign | ((positive)upper << 26) |
                         ((positive)left << 27) | ((positive)zero << 28);

        positive_to_base_field(write, magnitude, base, width, precision, style);
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
                        string_address word = printf_next();
                        bipolar asked = shell_signed(word, address_of good);

                        if (!good)
                                printf_not_a_number(word);

                        // A negative width is the minus flag written out long.
                        if (asked < 0)
                        {
                                left = true;
                                width = (positive)0 - (positive)asked;
                        }
                        else
                                width = (positive)asked;

                        step++;
                }
                else
                {
                        positive used;

                        width = string_digits(step, address_of used);
                        step += used;
                }

                if (string_is(step, '.'))
                {
                        step++;
                        precision = 0;

                        if (string_is(step, '*'))
                        {
                                bool good;
                                string_address word = printf_next();

                                precision = shell_signed(word, address_of good);

                                if (!good)
                                        printf_not_a_number(word);

                                if (precision < 0)
                                        precision = -1;

                                step++;
                        }
                        else
                        {
                                positive used;

                                precision = (bipolar)string_digits(step, address_of used);
                                step += used;
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

                                if (printf_cut)
                                        break;

                                value = printf_hold;
                                length = printf_held;
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
                        positive length = string_get(value) ? 1 : 0;

                        writer_field(write, value, length, width, ' ', left);

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

                        positive magnitude = (positive)value;

                        if (value < 0)
                                magnitude = (positive)0 - magnitude;

                        printf_number(write, magnitude,
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
static bool read_reserve(positive want)
{
        return shell_room((address_any address_to)address_of read_line,
                          address_of read_line_room, want, 1) &&
               shell_room((address_any address_to)address_of read_literal,
                          address_of read_literal_room, want, 1);
}

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

// A tenth of a second at a time is close enough for a timeout measured in
// whole seconds, and it keeps the wait in one place.
bool read_waited(bipolar tenths)
{
        timespec span = {tenths / 10, (tenths % 10) * 100000000};

        return descriptor_wait_readable(0, address_of span, null) > 0;
}

static bool read_set(string_address name, string_address value)
{
        if (env_assign(name, value))
                return true;

        string_format(shell_diagnostic,
                      env_readonly(name) ? "read: %s is readonly\n"
                                         : "read: no room for %s\n",
                      name);
        return false;
}

fn shell_read(writer write, string_address input)
{
        bool raw = false;
        positive index = 1;
        positive at = 0;
        positive names;
        bool ended = false;
        bool failed = false;
        bool limited = false;
        positive limit = 0;
        bipolar tenths = -1;
        p8 stop_at = '\n';
        string_address ifs;
        p8 ifs_default[] = " \t\n";

        read_length = 0;

        if (!read_reserve(1))
        {
                shell_diagnostic("read: no room\n", 0);
                return shell_answer(2);
        }

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
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "read: bad option: -%s\n", said);
                                return shell_answer(2);
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
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "read: option -%s wants a value\n",
                                              said);
                                return shell_answer(2);
                        }

                        if (which == 'p')
                                shell_diagnostic(value, 0);
                        else if (which == 'n')
                        {
                                bool good;
                                bipolar asked = shell_signed(value, address_of good);

                                if (!good || asked < 0)
                                {
                                        string_format(shell_diagnostic,
                                                      "read: bad count: %s\n", value);
                                        return shell_answer(1);
                                }

                                limited = true;
                                limit = (positive)asked;
                        }
                        else if (which == 'd')
                                stop_at = string_get(value);
                        else
                        {
                                bool good;
                                bipolar asked = shell_signed(value, address_of good);

                                if (!good || asked < 0 || asked > bipolar_max / 10)
                                {
                                        string_format(shell_diagnostic,
                                                      "read: bad timeout: %s\n", value);
                                        return shell_answer(1);
                                }

                                tenths = asked * 10;
                        }
                }

                index++;
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
                        shell_diagnostic("read: REPLY is readonly\n", 0);
                        return shell_answer(2);
                }
        }
        else
        {
                for (positive name = names; name < shell_argc; name++)
                {
                        positive length = string_length(shell_argv[name]);

                        if (!shell_valid_name(shell_argv[name], length))
                        {
                                string_format(shell_diagnostic,
                                              "read: bad variable name: %s\n",
                                              shell_argv[name]);
                                return shell_answer(2);
                        }

                }
        }

        while (!(limited && read_length >= limit))
        {
                p8 value;

                if (read_length == positive_max || !read_reserve(read_length + 2))
                {
                        shell_diagnostic("read: no room\n", 0);
                        return shell_answer(2);
                }

                if (tenths >= 0 && !read_waited(tenths))
                {
                        ended = true;
                        break;
                }

                bipolar got = system_call_3(syscall(read), 0,
                                             (positive)address_of value, 1);

                if (got != 1)
                {
                        if (got < 0)
                                failed = true;
                        else
                                ended = true;
                        break;
                }

                if (value == stop_at)
                        break;

                if (!raw && value == '\\')
                {
                        p8 next;

                        got = system_call_3(syscall(read), 0,
                                            (positive)address_of next, 1);

                        if (got != 1)
                        {
                                if (got < 0)
                                        failed = true;
                                else
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
                if (!read_set("REPLY", read_line))
                        return shell_answer(2);

                return shell_answer(failed ? 2 : ended ? 1 : 0);
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
                            !shell_room((address_any address_to)address_of read_ifs,
                                        address_of read_ifs_room, length + 1, 1))
                        {
                                shell_diagnostic("read: no room\n", 0);
                                return shell_answer(2);
                        }

                        memory_copy(read_ifs, value, length + 1);
                        ifs = read_ifs;
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
                        if (!read_set(shell_argv[names], read_line + begin))
                                return shell_answer(2);

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

                if (!read_set(shell_argv[names], read_line + begin))
                        return shell_answer(2);

                names++;
        }

        shell_answer(failed ? 2 : ended ? 1 : 0);
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
fn shell_getopts_done(string_address name, positive next, bool assigned)
{
        getopts_offset = -1;

        if (!env_set_number("OPTIND", next + 1))
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

        if (!env_set_number("OPTIND", next + 1))
                assigned = false;
        if (!env_assign(name, said))
                assigned = false;

        shell_answer(assigned ? 0 : 2);
}

fn shell_getopts(writer write, string_address input)
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

        if (shell_argc < 3)
                return shell_answer(2);

        options = shell_argv[1];
        name = shell_argv[2];
        silent = string_is(options, ':');

        if (!shell_valid_name(name, string_length(name)))
                return shell_answer(2);

        if (shell_argc > 3)
        {
                positive index = 3;

                if (!shell_array_room(shell_getopts_list, shell_getopts_room, shell_argc - 3 + 1))
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

        optind = shell_number(env_get("OPTIND"));

        if (optind < 1)
                optind = 1;

        next = optind - 1;

        // Where the last call stopped inside a word it had not finished. Only
        // believable while OPTIND still names the word after that one.
        if (optind > 1 && optind - 2 < count && getopts_offset >= 0 &&
            (positive)getopts_offset <= string_length(shell_getopts_list[optind - 2]))
        {
                word = shell_getopts_list[optind - 2];
                step = word + getopts_offset;
        }

        if (!step || !string_get(step))
        {
                word = next < count ? shell_getopts_list[next] : null;
                step = word;

                if (!step || string_not(step, '-') || !string_get(step + 1))
                        return shell_getopts_done(name, next,
                                                  getopts_optarg(null));

                step++;
                next++;

                if (string_is(step, '-') && !string_get(step + 1))
                        return shell_getopts_done(name, next,
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
                        string_format(shell_diagnostic,
                                      "getopts: illegal option -- %s\n", value);
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
                        string_format(shell_diagnostic,
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

        bool assigned = getopts_optarg(null);

        return shell_getopts_answer(name, value, word, step, next, assigned);
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
                        positive used;
                        positive value = string_digits_octal_max(
                            word, (positive)-1, address_of used);

                        word += used;

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

        if (ticks < 0)
                ticks = 0;

        seconds = (positive)ticks / CLOCK_TICKS;
        fraction = ((positive)ticks % CLOCK_TICKS) * (CLOCK_PLACES / CLOCK_TICKS);

        positive_to_string(write, seconds / 60);
        write("m", 1);
        positive_to_string(write, seconds % 60);
        write(".", 1);

        p8 fraction_text[6];
        positive fraction_length =
            positive_into_padded(fraction_text, fraction, 6, '0');

        write(fraction_text, fraction_length);

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
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "POLL", "PWR", "SYS",
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

PURE bool trap_ignored(positive number)
{
        for (positive at = 0; at < trap_count; at++)
                if (trap_table[at].number == number)
                        return !string_get(trap_table[at].action);

        return false;
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
        positive index = 0;

        while (index < trap_count)
        {
                if (trap_table[index].number == number)
                {
                        string_address action = trap_table[index].action;

                        if (room)
                                *room = trap_table[index].action_room;

                        memory_copy(trap_table + index, trap_table + index + 1,
                                    (trap_count - index - 1) *
                                        sizeof(trap_table[0]));

                        trap_count--;
                        return action;
                }

                index++;
        }

        return null;
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
        positive index = 0;

        while (index < trap_count)
        {
                if (trap_table[index].number == number)
                        return trap_table[index].action;

                index++;
        }

        return null;
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

        if (number < TRAP_NAMES - 1)
                string_format(write, "%s", trap_names[number]);
        else
                positive_to_string(write, number);

        write("\n", 1);
}

static bool trap_unsigned(string_address word)
{
        positive value;

        return string_digits_exact(word, address_of value);
}

fn shell_trap(writer write, string_address input)
{
        positive index = 1;
        string_address action;
        b32 answer = 0;
        bool print = false;

        if (index < shell_argc && word_is(shell_argv[index], "-p"))
        {
                print = true;
                index++;
        }

        if (index < shell_argc && word_is(shell_argv[index], "--"))
                index++;

        if (print)
        {
                if (index >= shell_argc)
                {
                        // All conditions the shell accepts, excluding the two
                        // signals POSIX permits trap -p to omit.
                        for (positive number = 0; number < TRAP_NAMES - 1;
                             number++)
                        {
                                string_address recorded;

                                if (number == 9 || number == 19)
                                        continue;

                                recorded = trap_action(number);

                                if (!recorded && number &&
                                    shell_was_ignored(number))
                                        recorded = (string_address) "";

                                trap_write_condition(write, number, recorded);
                        }
                }
                else
                {
                        while (index < shell_argc)
                        {
                                bipolar number = trap_number(shell_argv[index++]);

                                if (number < 0 || number >= TRAP_NAMES - 1)
                                {
                                        string_format(shell_diagnostic,
                                                      "trap: invalid signal: %s\n",
                                                      shell_argv[index - 1]);
                                        answer = 1;
                                        continue;
                                }

                                string_address recorded =
                                    trap_action((positive)number);

                                if (!recorded && number &&
                                    shell_was_ignored((b32)number))
                                        recorded = (string_address) "";

                                trap_write_condition(write, (positive)number,
                                                     recorded);
                        }
                }

                return shell_answer(answer);
        }

        if (index >= shell_argc)
        {
                // Without -p only non-default conditions are listed.  Query
                // inherited dispositions as well as the explicit table: an
                // ignored-on-entry signal has never needed a table entry.
                for (positive number = 0; number < TRAP_NAMES - 1; number++)
                {
                        string_address recorded = trap_action(number);

                        if (!recorded && number && shell_was_ignored((b32)number))
                                recorded = (string_address) "";

                        if (recorded)
                                trap_write_condition(write, number, recorded);
                }

                return shell_answer(0);
        }

        // An unsigned first operand is the historical reset form: every word
        // is a condition, including that first one.  Otherwise it is action.
        if (trap_unsigned(shell_argv[index]))
                action = null;
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

                if (number < 0 || number >= TRAP_NAMES - 1)
                {
                        string_format(log_error, "trap: invalid signal: %s\n",
                                      shell_argv[index - 1]);
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
                if (number && !deaf)
                {
                        if (!action)
                                shell_default((b32)number);
                        else if (!string_get(action))
                                shell_ignore((b32)number);
                        else
                                shell_catch((b32)number);
                }

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

bool alias_record(string_address name, positive name_length, string_address value)
{
        positive value_length = string_length(env_reading(value));
        positive index;
        string_address kept_name;
        string_address kept_value;

        if (name_length == positive_max || value_length == positive_max)
                return false;

        kept_name = shell_map(name_length + 1);
        kept_value = shell_map(value_length + 1);

        if (!kept_name || !kept_value)
        {
                if (kept_name)
                        memory_free(kept_name, name_length + 1);

                if (kept_value)
                        memory_free(kept_value, value_length + 1);

                return false;
        }

        string_copy_max_end(kept_name, name, name_length);
        memory_copy(kept_value, value, value_length + 1);
        index = string_table_find(kept_name, alias_table,
                                  sizeof(alias_table[0]), alias_count);

        if (index < alias_count)
        {
                memory_free(kept_name, name_length + 1);
                memory_free(alias_table[index].value,
                            alias_table[index].value_room);
                alias_table[index].value = kept_value;
                alias_table[index].value_room = value_length + 1;
                return true;
        }

        if (!shell_array_room(alias_table, alias_room, alias_count + 1))
        {
                memory_free(kept_name, name_length + 1);
                memory_free(kept_value, value_length + 1);
                return false;
        }
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
                        if (!alias_record(word, mark - word, mark + 1))
                                answer = 1;

                        index++;
                        continue;
                }

                {
                        positive at = string_table_find(word, alias_table,
                                                        sizeof(alias_table[0]),
                                                        alias_count);

                        if (at < alias_count)
                                alias_written(write, at);
                        else
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
                        for (at = 0; at < alias_count; at++)
                        {
                                memory_free(alias_table[at].name,
                                            alias_table[at].name_room);
                                memory_free(alias_table[at].value,
                                            alias_table[at].value_room);
                        }

                        alias_count = 0;
                        index++;
                        continue;
                }

                at = string_table_find(word, alias_table, sizeof(alias_table[0]),
                                       alias_count);

                // A name that was never an alias is something the script asked
                // for and did not get, which POSIX has this say so.
                if (at >= alias_count)
                        status = 1;

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
fn shell_eval(writer write, string_address input)
{
        p8 address_to eval_storage = null;
        positive eval_room = 0;
        positive used = 0;
        positive index = 1;
        bool room = true;

        if (shell_argc < 2 || !run_line)
                return shell_answer(0);

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

                if (!shell_room((address_any address_to)address_of eval_storage,
                                address_of eval_room, wanted, 1))
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

                string_format(shell_diagnostic, "eval: no room\n");
                shell_answer(2);
                shell_stop_when_scripted(2);

                return;
        }

        eval_storage[used] = end;

        /* The nested line gets independent lexer storage and parser marks. */
        {
                lex_frame frame;

                lex_nest_enter(address_of frame);

                run_line(eval_storage);
                shell_input_end();

                lex_nest_leave(address_of frame);
        }

        memory_free(eval_storage, eval_room);

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
                                      positive room, bool address_to ready)
{
        positive length;
        positive hash;
        positive at;

        if (!address_to ready)
        {
                shell_name_index_build(table, stride, count, slots, room);
                address_to ready = true;
        }

        {
                positive2 answer = string_hash_33_length(name);

                hash = answer.x;
                length = answer.y;
        }
        at = hash & (room - 1);

        for (positive probes = 0; probes < room; probes++)
        {
                shell_name_slot address_to slot = slots + at;

                if (!slot->index_plus_one)
                        return count;

                if (slot->hash == hash && slot->length == length)
                {
                        positive index = slot->index_plus_one - 1;
                        string_address candidate =
                            *(string_address address_to)((p8 address_to)table +
                                                          index * stride);

                        if (!memory_compare(name, candidate, length))
                                return index;
                }

                at = (at + 1) & (room - 1);
        }

        return count;
}

static shell_tool shell_tools[] = {
#ifdef SHELL_NO_UTILITIES
#define SHELL_TOOL_GENERAL(name, function)
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#else
#define SHELL_TOOL_GENERAL(name, function) {#name, function},
#ifdef SHELL_NO_UTIL_LINUX
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#else
#define SHELL_TOOL_UTIL_BIN(name, function) {#name, function},
#define SHELL_TOOL_UTIL_SBIN(name, function) {#name, function},
#endif
#endif
#ifdef SHELL_UTILITY_PROGRAM
#define SHELL_TOOL_SYSTEM(name, function)
#else
#define SHELL_TOOL_SYSTEM(name, function) {#name, function},
#endif
#define SHELL_TOOL(category, name, function) \
        SHELL_TOOL_##category(name, function)
#include "tools.inc"
#undef SHELL_TOOL
#undef SHELL_TOOL_SYSTEM
#undef SHELL_TOOL_UTIL_SBIN
#undef SHELL_TOOL_UTIL_BIN
#undef SHELL_TOOL_GENERAL
    {null, null},
};

#define SHELL_TOOLS (sizeof(shell_tools) / sizeof(shell_tools[0]) - 1)
#define SHELL_TOOL_INDEX_ROOM 128

static shell_name_slot shell_tool_index[SHELL_TOOL_INDEX_ROOM];
static bool shell_tool_index_ready;

static positive shell_tool_find(string_address name)
{
        return shell_name_index_find(name, shell_tools, sizeof(shell_tools[0]),
                                     SHELL_TOOLS, shell_tool_index,
                                     SHELL_TOOL_INDEX_ROOM,
                                     address_of shell_tool_index_ready);
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

static b32 shell_tool_call(positive which)
{
        b32 answered;

        log_failure_reset();
        answered = shell_tools[which].function() & 0xff;
        log_flush();

        if (log_failed() && !answered)
                answered = 1;

        return answered;
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

        /* Installed shell entry names are overwhelmingly more common than a
           multicall utility entry. Reject their exact short spellings before
           walking the one-shot tool table; utility lookup remains unchanged. */
        if ((string_is(name, 's') && string_is(name + 1, 'h') &&
             !string_get(name + 2)) ||
            (string_is(name, 's') && string_is(name + 1, 'h') &&
             string_is(name + 2, 'e') && string_is(name + 3, 'l') &&
             string_is(name + 4, 'l') && !string_get(name + 5)) ||
            (string_is(name, 'b') && string_is(name + 1, 'a') &&
             string_is(name + 2, 's') && string_is(name + 3, 'h') &&
             !string_get(name + 4)))
                return -1;

        /* One lookup in a process is cheaper than constructing the reusable
           index. Ordinary shell dispatch below is where repeated names use
           the index; argv[0] is asked only once. */
        which = string_table_find(name, shell_tools, sizeof(shell_tool),
                                  SHELL_TOOLS);

        if (which == SHELL_TOOLS)
                return -1;

        return shell_tool_call(which);
}

// Whether a name is one of the utilities, without running it.
bool shell_tool_here(string_address name)
{
        return shell_tool_find(name) != SHELL_TOOLS;
}

fn shell_tool_list(writer write)
{
        for (positive i = 0; i < SHELL_TOOLS; i++)
                string_format(write, TERM_BOLD " -  %s" TERM_RESET "\n",
                              shell_tools[i].name);
}

static bool shell_tool_run(string_address name)
{
        positive which = shell_tool_find(name);
        bipolar child = -1;
        positive status = 0;

        if (which == SHELL_TOOLS)
                return false;

        if (shell_tail_command)
        {
                program_arguments_use(shell_argv, (b32)shell_argc);
                shell_answer(shell_tool_call(which));
                return true;
        }

        // Before the fork, or the child inherits a copy of what is waiting in
        // the buffer and writes it out a second time.
        log_flush();

        /* Spark starts the immutable multicall image without copying this
           resident shell. A stock kernel takes the direct-function fork. */
        child = shell_spawn_tool(shell_argv, -1, false);

        if (child < 0)
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
                exit(shell_tool_call(which));
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
fn shell_trap_exit()
{
        positive action_room = 0;
        string_address action = trap_detach(0, address_of action_room);
        b32 leaving = shell_status;

        if (!action || !run_line || !string_get(action))
        {
                if (action)
                        memory_free(action, action_room);

                return;
        }

        // Taken away first, so a trap that leaves again does not run twice.
        parse_nest_enter();
        run_line(action);
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
        string_address segment = value;
        positive longest = 0;

        while (1)
        {
                string_address next = string_first_of(segment, ':');
                positive length = next ? (positive)(next - segment)
                                       : string_length(segment);

                if (length > longest)
                        longest = length;

                if (!next)
                        break;

                segment = next + 1;
        }

        if (name_length > positive_max - 2 ||
            longest > positive_max - name_length - 2)
                return false;

        *wanted = longest + name_length + 2;
        return true;
}

static bipolar shell_source_open(string_address name,
                                  p8 address_to address_to found,
                                  positive address_to found_room,
                                  bool address_to no_room)
{
        string_address value;
        string_address segment;
        positive name_length;

        if (!name || !string_get(name))
                return -1;

        if (string_first_of(name, '/'))
        {
                bipolar handle;

                do
                        handle = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)name, FILE_READ, 0);
                while (handle == -4);

                return handle;
        }

        value = env_get("PATH");

        if (!value)
                value = "/bin:/usr/bin:/";

        name_length = string_length(name);

        {
                positive wanted;

                if (!shell_path_wanted(value, name_length,
                                       address_of wanted) ||
                    !shell_room((address_any address_to)found, found_room,
                                wanted, 1))
                {
                        *no_room = true;
                        return -1;
                }
        }

        segment = value;

        while (1)
        {
                string_address next = string_first_of(segment, ':');
                positive length = next ? (positive)(next - segment)
                                       : string_length(segment);
                positive used = 0;
                bipolar handle;

                if (length)
                {
                        memory_copy(*found, segment, length);
                        used = length;

                        if ((*found)[used - 1] != '/')
                                (*found)[used++] = '/';
                }

                string_copy(*found + used, name);

                do
                        handle = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)*found, FILE_READ, 0);
                while (handle == -4);

                if (handle >= 0)
                        return handle;

                if (!next)
                        break;

                segment = next + 1;
        }

        return -1;
}

static bipolar shell_source_read(bipolar handle,
                                  p8 address_to address_to text,
                                  positive address_to room,
                                  bool address_to no_room)
{
        positive used = 0;

        while (1)
        {
                bipolar got;

                if (!*room || used == *room - 1)
                {
                        positive wanted;

                        if (*room && *room > positive_max / 2)
                        {
                                *no_room = true;
                                break;
                        }

                        wanted = *room ? *room * 2 : 4096;

                        if (!shell_room((address_any address_to)text, room,
                                        wanted, 1))
                        {
                                *no_room = true;
                                break;
                        }
                }

                got = system_read_retry((positive)handle, *text + used,
                                        *room - used - 1);

                if (got < 0)
                {
                        system_call_1(syscall(close), (positive)handle);
                        return got;
                }

                if (!got)
                        break;

                used += (positive)got;
        }

        system_call_1(syscall(close), (positive)handle);

        if (*no_room)
                return -1;

        (*text)[used] = end;
        return (bipolar)used;
}

fn shell_dot(writer write, string_address input)
{
        p8 address_to found = null;
        positive found_room = 0;
        p8 address_to source_text = null;
        positive source_room = 0;
        string_address path;
        bipolar got;
        positive filled;
        positive at = 0;
        bool no_room = false;
        bipolar handle;

        if (shell_argc < 2 || !run_line)
                return shell_answer(shell_argc < 2 ? 2 : 0);

        path = shell_argv[1];
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

                string_format(shell_diagnostic, "%s: no room\n", shell_argv[0]);
                shell_answer(2);
                shell_stop_when_scripted(2);

                return;
        }

        if (got < 0)
        {
                memory_free(source_text, source_room);
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
                shell_stop_when_scripted(1);

                return;
        }

        filled = (positive)got;

        {
                lex_frame frame;

                lex_nest_enter(address_of frame);

                while (at < filled)
                {
                        p8 address_to newline = (p8 address_to)memory_first_of(
                            source_text + at, '\n', filled - at);
                        positive stop = newline ? (positive)(newline - source_text) : filled;

                        source_text[stop] = end;
                        run_line(source_text + at);

                        if (exec_source_stop())
                                break;

                        at = stop + 1;
                }

                shell_input_end();

                lex_nest_leave(address_of frame);
        }

        memory_free(source_text, source_room);

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

#define SHELL_WAIT_NO_HANG 1
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

fn shell_background_reap()
{
        positive status;
        bipolar pid;

        while ((pid = system_call_4(syscall(wait4), (positive)-1,
                                    (positive)address_of status,
                                    SHELL_WAIT_NO_HANG, 0)) > 0)
                shell_background_reaped(pid, status);
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
fn shell_wait(writer write, string_address input)
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

                if (!string_digits_exact(shell_argv[at], address_of pid) ||
                    pid > (positive)bipolar_max)
                {
                        string_format(shell_diagnostic,
                                      "wait: Illegal number: %s\n",
                                      shell_argv[at]);
                        return shell_answer(2);
                }

                answer = shell_wait_one((bipolar)pid,
                                        address_of interrupted);

                if (interrupted)
                        break;
        }

        shell_answer(answer);
}

fn shell_help(writer write, string_address input);
fn shell_which(writer write, string_address input);
fn shell_type(writer write, string_address input);
fn shell_command_builtin(writer write, string_address input);
fn shell_hash(writer write, string_address input);
fn shell_ulimit(writer write, string_address input);
bool exec_control_builtin(string_address name, bool run);

/*
        Bash's `let` is the command-shaped spelling of the arithmetic engine
        already used by (( ... )). Each operand is one expression, evaluated
        left to right, and the command answers for the value of the last one.
        Keeping this as a thin builtin avoids a second arithmetic grammar and
        gives assignments, increments and overflow exactly the same rules.
*/
fn shell_let(writer write, string_address input)
{
        bipolar value = 0;
        positive at;

        (void)write;
        (void)input;

        if (shell_argc < 2)
                return shell_answer(1);

        for (at = 1; at < shell_argc; at++)
                if (!exec_arithmetic_value(shell_argv[at], address_of value))
                        return shell_answer(exec_line_aborted() ? 2 : 1);

        shell_answer(value ? 0 : 1);
}

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
    {"blkid", shell_blkid},
    {"cd", shell_cd},
    {"clear", shell_clear},
    {"command", shell_command_builtin},
    {"declare", shell_declare},
    {"echo", shell_echo},
    {"eval", shell_eval},
    {"exec", shell_exec},
    {"exit", shell_exit},
    {"false", shell_false},
    {"findfs", shell_findfs},
    {"findmnt", shell_findmnt},
    {"getopts", shell_getopts},
    {"hash", shell_hash},
    {"let", shell_let},
    {"mount", shell_mount},
    {"mountpoint", shell_mountpoint},
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
    {"typeset", shell_declare},
    {"true", shell_true},
    {"ulimit", shell_ulimit},
    {"umask", shell_umask},
    {"umount", shell_umount},
    {"unalias", shell_unalias},
    {"unset", shell_unset},
    {"wait", shell_wait},
    {"which", shell_which},
    {"help", shell_help},
    {"local", shell_local},
    {"export", shell_export},
    {null, null},
};

#define SHELL_COMMAND_COUNT ((sizeof(shell_commands) / sizeof(shell_commands[0])) - 1)
#define SHELL_COMMAND_INDEX_ROOM 128

static shell_name_slot shell_command_index[SHELL_COMMAND_INDEX_ROOM];
static bool shell_command_index_ready;

static shell_command address_to shell_command_named(string_address name)
{
        positive which = shell_name_index_find(
            name, shell_commands, sizeof(shell_commands[0]),
            SHELL_COMMAND_COUNT, shell_command_index,
            SHELL_COMMAND_INDEX_ROOM, address_of shell_command_index_ready);

        return which < SHELL_COMMAND_COUNT ? shell_commands + which : null;
}

bool shell_command_here(string_address name)
{
        return shell_command_named(name) != null;
}

/* Control builtins live in the executor because their result unwinds C
   frames, but command/type must report the same builtin namespace. */
static bool shell_command_builtin_here(string_address name)
{
        return shell_command_named(name) || shell_tool_here(name) ||
               exec_control_builtin(name, false);
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

fn shell_hash(writer write, string_address input)
{
        positive index = 1;
        b32 bad = 0;
        p8 address_to found = null;
        positive found_room = 0;

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
                bipolar located = shell_find_in_path_alloc(shell_argv[index],
                                                           address_of found,
                                                           address_of found_room);

                if (located < 0)
                {
                        bad = 2;
                        string_format(shell_diagnostic, "hash: no room\n");
                        break;
                }

                if (located != 1)
                {
                        bad = 1;
                        string_format(shell_diagnostic, "hash: %s: not found\n",
                                      shell_argv[index]);
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
        string_address segment;
        positive name_length;

        if (name == null || !string_get(name) || !room)
                return false;

        if (string_first_of(name, '/'))
        {
                if (system_call_4(syscall(faccessat), AT_FDCWD, (positive)name,
                                  access, 0))
                        return false;

                if (string_length(name) >= room)
                        return false;

                string_copy(into, name);
                return true;
        }

        {
                string_address known = use_hash ? hash_find(name) : null;

                if (known)
                {
                        if (string_length(known) >= room)
                                return false;

                        string_copy(into, known);
                        return true;
                }
        }

        if (value == null && !(value = env_get("PATH")))
                value = "/bin:/usr/bin:/";

        segment = value;
        name_length = string_length(env_reading(name));

        while (1)
        {
                string_address next = string_first_of(segment, ':');
                positive length = next ? (positive)(next - segment)
                                       : string_length(segment);

                if (!length && name_length < room)
                {
                        string_copy(into, name);

                        if (!system_call_4(syscall(faccessat), AT_FDCWD,
                                           (positive)into, access, 0))
                        {
                                if (use_hash)
                                        hash_remember(name, into);
                                return true;
                        }
                }
                else if (length && name_length <= positive_max - 2 &&
                         length <= positive_max - name_length - 2 &&
                         length + name_length + 2 <= room)
                {
                        memory_copy(into, segment, length);
                        into[length] = end;

                        if (into[length - 1] != '/')
                                into[length++] = '/';

                        string_copy(into + length, name);

                        if (!system_call_4(syscall(faccessat), AT_FDCWD,
                                           (positive)into, access, 0))
                        {
                                if (use_hash)
                                        hash_remember(name, into);
                                return true;
                        }
                }

                if (!next)
                        break;

                segment = next + 1;
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
                                              bool query,
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
        else if (!fixed_path && (known = hash_find(name)))
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
        }

        if (!shell_room((address_any address_to)into, room, wanted, 1))
                return -1;

        if (shell_find_in_path_mode(name, *into, *room,
                                    query ? 0 : ACCESS_EXECUTE,
                                    !fixed_path, fixed_path))
                return 1;

        if (!query && shell_find_in_path_mode(name, *into, *room, 0, false,
                                              fixed_path))
                return 2;

        return 0;
}

bipolar shell_find_in_path_alloc(string_address name,
                                 p8 address_to address_to into,
                                 positive address_to room)
{
        return shell_find_in_path_alloc_mode(name, into, room, false, null);
}

bipolar shell_find_in_path_query_alloc(string_address name,
                                       p8 address_to address_to into,
                                       positive address_to room)
{
        return shell_find_in_path_alloc_mode(name, into, room, true, null);
}

static bipolar shell_find_in_standard_path_alloc(string_address name,
                                                  p8 address_to address_to into,
                                                  positive address_to room,
                                                  bool query)
{
        return shell_find_in_path_alloc_mode(name, into, room, query,
                                             "/bin:/usr/bin");
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
        p8 address_to found = null;
        positive found_room = 0;

        if (shell_argc < 2)
                return shell_answer(0);

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                bipolar located;

                if (shell_command_builtin_here(name))
                {
                        string_format(write, "%s is a shell builtin\n", name);
                        continue;
                }

                if (exec_function_here && exec_function_here(name))
                {
                        string_format(write, "%s is a shell function\n", name);
                        continue;
                }

                located = shell_find_in_path_query_alloc(name, address_of found,
                                                         address_of found_room);

                if (located < 0)
                {
                        string_format(shell_diagnostic, "type: no room\n");
                        bad = 2;
                        break;
                }

                if (located)
                {
                        string_format(write, "%s is %s\n", name, found);
                        continue;
                }

                // On standard output, as POSIX says of type and as the
                // reference shell does: it is an answer, not a complaint.
                string_format(write, "%s: not found\n", name);
                bad = 127;
        }

        if (found)
                memory_free(found, found_room);

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
        bool standard_path = false;

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
                        if (string_get(letter) == 'v')
                                only_say = true;
                        else if (string_get(letter) == 'V')
                        {
                                only_say = true;
                                at_length = true;
                        }
                        else if (string_get(letter) == 'p')
                                standard_path = true;
                        else
                                break;

                        letter++;
                }

                index++;
        }

        if (index >= shell_argc)
                return shell_answer(0);

        if (only_say)
        {
                p8 address_to found = null;
                positive found_room = 0;
                b32 bad = 0;
                bool any = false;

                while (index < shell_argc)
                {
                        string_address name = shell_argv[index++];
                        bipolar located;

                        if (shell_command_builtin_here(name))
                        {
                                string_format(write,
                                              at_length
                                                ? "%s is a shell builtin\n"
                                                : "%s\n",
                                              name);
                                any = true;
                                continue;
                        }

                        if (exec_function_here && exec_function_here(name))
                        {
                                string_format(write,
                                              at_length
                                                ? "%s is a shell function\n"
                                                : "%s\n",
                                              name);
                                any = true;
                                continue;
                        }

                        located = standard_path
                                    ? shell_find_in_standard_path_alloc(
                                          name, address_of found,
                                          address_of found_room, true)
                                    : shell_find_in_path_query_alloc(
                                          name, address_of found,
                                          address_of found_room);

                        if (located < 0)
                        {
                                string_format(shell_diagnostic,
                                              "command: no room\n");
                                bad = 2;
                                break;
                        }

                        if (located)
                        {
                                if (at_length)
                                        string_format(write, "%s is %s\n",
                                                      name, found);
                                else
                                        string_format(write, "%s\n", found);
                                any = true;
                        }
                        else
                        {
                                if (at_length)
                                        string_format(write, "%s: not found\n",
                                                      name);

                        }
                }

                if (found)
                        memory_free(found, found_room);

                return shell_answer(bad ? bad : (any ? 0 : 127));
        }

        // Running it is the executor's business, and it is told to skip the
        // function table by the words it is handed.
        {
                memory_copy(shell_argv, shell_argv + index,
                            (positive)(shell_argc - index) *
                                sizeof(shell_argv[0]));

                shell_argc -= index;
                shell_argv[shell_argc] = null;
        }

        if (exec_control_builtin(shell_argv[0], true))
                return;

        {
                bool tail = shell_tail_command;

                /* A named builtin may run eval or dot and therefore owns more
                   than one command. A multicall utility is one disposable
                   operation and keeps the direct stage. */
                if (shell_command_named(shell_argv[0]))
                        shell_tail_command = false;

                if (shell_builtin(shell_arguments()))
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
                        string_format(shell_diagnostic, "command: no room\n");
                        return shell_answer(2);
                }

                if (!located)
                {
                        if (found)
                                memory_free(found, found_room);

                        string_format(shell_diagnostic, "command: %s: not found\n",
                                      name);
                        return shell_answer(127);
                }

                if (located == 2)
                {
                        memory_free(found, found_room);
                        string_format(shell_diagnostic,
                                      "command: %s: cannot run\n", name);
                        return shell_answer(126);
                }

                shell_argv[0] = found;
                if (shell_tail_command)
                        shell_thread_instance_mode(true);
                else
                        shell_execute_command();
                shell_argv[0] = name;
                memory_free(found, found_room);
        }
}

fn shell_which(writer write, string_address input)
{
        p8 address_to found = null;
        positive found_room = 0;
        bipolar located;

        if (input == null)
                return shell_diagnostic(str("which: missing operand\n"));

        // Before the path, because that is the order the shell runs them in:
        // a grep on the path is not the grep that would run.
        if (shell_command_builtin_here(input))
                return string_format(write, "%s: shell builtin\n", input);

        located = shell_find_in_path_alloc(input, address_of found,
                                           address_of found_room);

        if (located < 0)
        {
                string_format(shell_diagnostic, "which: no room\n");
                return shell_answer(2);
        }

        if (located == 1)
        {
                string_format(write, "%s\n", found);
                memory_free(found, found_room);
                return;
        }

        if (found)
                memory_free(found, found_room);

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

fn shell_limit_said(writer write, shell_limit address_to limit, bool hard)
{
        ul_limit_pair pair;
        p64 value;

        if (ul_prlimit(0, limit->resource, null, address_of pair) < 0)
                return string_format(write, "unlimited\n");

        value = hard ? pair.hard : pair.soft;

        if (value == UL_LIMIT_INFINITE)
                return string_format(write, "unlimited\n");

        positive_to_string(write, (positive)(value / limit->step));
        write("\n", 1);
}

fn shell_limit_listed(writer write)
{
        shell_limit address_to limit = shell_limits;

        while (limit->name)
        {
                string_to_field(write, limit->name, 20, ' ', true);

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
                ul_limit_pair pair;
                p64 value;

                if (ul_prlimit(0, chosen->resource, null, address_of pair) < 0)
                        return shell_answer(1);

                if (word_is(shell_argv[index], "unlimited"))
                        value = UL_LIMIT_INFINITE;
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

                if (ul_prlimit(0, chosen->resource, address_of pair, null) < 0)
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
