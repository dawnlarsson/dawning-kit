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
bool exec_function_here_hashed(string_address name, positive2 named);
bool exec_function_unset(string_address name);
static bool exec_line_aborted();
static bool exec_source_stop();
bool shell_builtin(string_address arguments, positive2 named);
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

        if (!fallback || (positive)fallback >= (positive)-4095)
                return answered;

        fallback[0] = (string_address)"/proc/self/exe";
        fallback[1] = path;

        for (positive at = 1; at < count; at++)
                fallback[at + 1] = arguments[at];

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

/* A name inside a larger word -- the array of an element -- asked about
   without a terminated copy of it being made first. */
static PURE bool env_readonly_span(const_string name, positive length)
{
        for (positive at = 0; at < readonly_count; at++)
                if (string_length(readonly_name[at]) == length &&
                    !memory_compare(readonly_name[at], (address_any)name,
                                    length))
                        return true;

        return false;
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
                return slot;
        }

        if (array_table_count >= (positive)0x7ffffffe ||
            !shell_array_room(array_tables, array_table_room,
                              array_table_count + 1))
                return 0;

        slot = (b32)(array_table_count + 1);
        array_tables[array_table_count] = (array_table){0};
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

        for (positive at = 0; at < table->count; at++)
                env_cell_drop(table->element[at].text);

        table->count = 0;
        table->next_free = array_table_free;
        array_table_free = slot;
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

        for (positive step = 0; step < left; step++)
                table->element[at + step] = table->element[at + step + 1];

        table->count--;
}

/*
        An element written, made or replaced.

        The cell holds KEY=VALUE for a keyed element and VALUE alone for a
        subscripted one, which is the same shape a variable's own text has
        and lets both reuse the one free list. Appending reads the old value
        out of the cell it is about to leave, so the copy happens before the
        old cell is handed back.
*/
static COLD bool array_element_write(array_table address_to table, positive at,
                                bool making, positive key,
                                const_string key_text, positive key_length,
                                const_string value, positive value_length,
                                bool append)
{
        positive prefix = key_length ? key_length + 1 : 0;
        positive old_length = 0;
        string_address old = null;
        env_cell address_to cell;
        p8 address_to into;

        if (!making && append)
        {
                old = array_element_value(table->element + at);
                old_length = table->element[at].value_length;
        }

        if (value_length > positive_max - old_length - prefix - 1)
                return false;

        cell = env_cell_take(prefix + old_length + value_length + 1);

        if (!cell)
                return false;

        into = (p8 address_to)(cell + 1);

        if (key_length)
        {
                memory_copy(into, (address_any)key_text, key_length);
                into[key_length] = '=';
        }

        if (old_length)
                memory_copy(into + prefix, old, old_length);

        memory_copy_end(into + prefix + old_length, (address_any)value,
                        value_length);

        if (making)
        {
                positive left = table->count - at;

                if (!shell_array_room(table->element, table->room,
                                      table->count + 1))
                {
                        env_cell_drop((string_address)into);
                        return false;
                }

                for (positive step = 0; step < left; step++)
                        table->element[table->count - step] =
                            table->element[table->count - step - 1];

                table->count++;
        }
        else
                env_cell_drop(table->element[at].text);

        table->element[at].key = key;
        table->element[at].key_length = key_length;
        table->element[at].value_length = old_length + value_length;
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
               (variable->permanent || variable->temporary);
}

static fn env_variable_drop(positive index)
{
        positive left = shell_var_count - index - 1;
        env_variable dropped = shell_vars[index];

        // Every path remembered was an answer about a PATH that is now gone,
        // the same as when it is assigned over.
        if (dropped.name_length == 4 &&
            memory_is_4(dropped.text, 'P', 'A', 'T', 'H'))
                hash_forget();

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
        record->temporary = 0;
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

static PURE bool env_export_active_span(const_string name, positive length)
{
        positive found = env_find_span(name, length);

        return found < shell_var_count && (shell_vars[found].permanent ||
                                           shell_vars[found].temporary);
}

/*
        The value, the export state and the kind of one name, in one probe.

        Saving a name before a command in front of it changes it needs all
        three, and asking three times meant hashing and probing three times
        for every assignment prefix on every command line.
*/
string_address env_saved_state(const_string name, positive length,
                               bool address_to exported, p8 address_to kind)
{
        positive found = env_find_span(name, length);

        if (found >= shell_var_count)
        {
                address_to exported = false;
                address_to kind = 0;
                return null;
        }

        address_to exported = shell_vars[found].permanent ||
                              shell_vars[found].temporary != 0;
        address_to kind = shell_vars[found].attributes;

        if (!env_variable_has_value(shell_vars + found))
                return null;

        return shell_vars[found].text + length + 1;
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

                if (!entry->permanent && !entry->temporary &&
                    !entry->declared && !env_variable_has_value(entry))
                        env_variable_drop(found);
        }

        if (export_mark)
                shell_envp_dirty = true;
}

#define env_declare_restore(name, enabled)                                  \
        env_mark_restore((name), (enabled), false)

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

#define env_export_restore(name, enabled)                                   \
        env_mark_restore((name), (enabled), true)

/*
        An element assignment in front of a command exports nothing.

        Bash does not put arrays in the environment at all, so `a[1]=v cmd`
        has no name to hand over; taking one here would make a variable
        called a[1] which nothing could ever read back.
*/
static PURE bool env_assignment_element(string_address assignment,
                                        positive length)
{
        string_address bracket = string_first_of(assignment, '[');

        return bracket && (positive)(bracket - assignment) < length;
}

static bool env_export_temporary(string_address assignment)
{
        positive length = (positive)(string_first_of_or_end(assignment, '=') -
                                     assignment);
        env_variable address_to entry;

        if (env_assignment_element(assignment, length))
                return true;

        entry = env_export_take(assignment, length);

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
        positive found;

        if (env_assignment_element(assignment, length))
                return;

        found = env_find_span(assignment, length);

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
                if (env_variable_exports(shell_vars + at))
                        count++;

        if (!shell_array_room(shell_envp, shell_envp_room, count + 1))
                return null;

        count = 0;

        for (positive at = 0; at < shell_var_count; at++)
                if (env_variable_exports(shell_vars + at))
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
static bool env_write(const_string name, const_string value, bool assignment);

// A nameref that names itself, or a ring of them, is a name with no variable
// behind it rather than a shell that never answers.
static positive env_nameref_depth;
PURE string_address env_get(const_string name);
bool env_set(const_string name, const_string value);
bool env_assign(const_string name, const_string value);
fn env_unset(string_address name);

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
                return true;
        }

        if (shell_vars[found].owned)
                env_cell_drop(shell_vars[found].text);

        shell_vars[found].text = entry;
        shell_vars[found].hash = hash;
        shell_vars[found].name_length = length;
        shell_vars[found].value_length = string_length(mark + 1);
        shell_vars[found].temporary = 0;
        shell_vars[found].owned = false;
        shell_vars[found].permanent = true;
        shell_vars[found].declared = true;

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
        string_address defaults[] = {"PATH=/bin:/usr/bin:/bowls/bin:/",
                                     "SHELL=/bin/sh",
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

/*
        Follow a nameref to the record it stands for.

        Kept out of line and iterative on purpose: reading a variable is one
        of the two hottest things this shell does, and a recursive hop
        between the reader and itself is enough to stop it being inlined
        where it is read. A ring of namerefs is a name with nothing behind
        it rather than a shell that never answers.
*/
static COLD positive env_nameref_index(positive index)
{
        for (positive step = 0; step < 16; step++)
        {
                string_address target =
                    shell_vars[index].text + shell_vars[index].name_length + 1;
                positive length = shell_vars[index].value_length;
                positive next = env_find_span(target, length);

                if (next >= shell_var_count ||
                    !env_variable_has_value(shell_vars + next))
                        return shell_var_count;

                if (!(shell_vars[next].attributes & SHELL_ARRAY_NAMEREF))
                        return next;

                index = next;
        }

        return shell_var_count;
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

        // Reading a nameref is reading the name it holds. The bit is in the
        // record already loaded, so a scalar pays one predicted branch.
        if (shell_vars[index].attributes & SHELL_ARRAY_NAMEREF)
        {
                index = env_nameref_index(index);

                if (index >= shell_var_count)
                        return null;

                length = shell_vars[index].name_length;
        }

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

/*
        What an attribute makes of the bytes on their way in.

        -i, -l and -u act once, where the value is stored, which is why
        declare -p shows what they made of it and not what was written. The
        arithmetic answer is a small fixed spelling; a folded one is as long
        as the value and takes arena bytes the caller copies out of at once.
*/
#define ENV_ATTRIBUTE_VALUE                                                  \
        (SHELL_ARRAY_INTEGER | SHELL_ARRAY_LOWER | SHELL_ARRAY_UPPER)

static COLD string_address env_attribute_value(p8 attributes,
                                               const_string value)
{
        positive length = string_length(env_reading(value));
        p8 address_to made;

        if (attributes & SHELL_ARRAY_INTEGER)
        {
                bipolar answer = arith_evaluate(env_reading(value));

                made = shell_store_take(address_of expand_store, 32);

                if (!made)
                        return null;

                made[bipolar_into_string(made, arith_bad ? 0 : answer)] = end;

                return made;
        }

        made = shell_store_take(address_of expand_store, length + 1);

        if (!made)
                return null;

        memory_copy_end(made, env_reading(value), length);

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
                                     const_string address_to shaped)
{
        p8 attributes = shell_vars[idx].attributes;

        if ((attributes & SHELL_ARRAY_NAMEREF) &&
            env_variable_has_value(shell_vars + idx) &&
            env_nameref_depth < 16)
        {
                bool answer;

                env_nameref_depth++;
                answer = env_write(shell_vars[idx].text + name_len + 1, value,
                                   assignment);
                env_nameref_depth--;

                return answer ? 1 : 2;
        }

        if (attributes & ENV_ATTRIBUTE_VALUE)
        {
                value = env_attribute_value(attributes, value);

                if (!value)
                        return 2;

                address_to shaped = value;
        }

        if (attributes & SHELL_ARRAY_ASSOCIATIVE)
                return shell_array_set(name, name_len, "0", 1, value, false)
                           ? 1
                           : 2;

        return 0;
}

static bool env_write_hashed_span(const_string name, positive name_len,
                                  positive hash, const_string value,
                                  bool assignment)
{
        bool allexport = assignment && (shell_options & SHELL_FLAG('a'));

        if (!name || !value)
                return false;

        // Every path remembered was an answer about the old PATH.
        if (name_len == 4 && memory_is_4(name, 'P', 'A', 'T', 'H'))
                hash_forget();

        positive idx = env_find_hashed_span(name, name_len, hash);

        // RANDOM and SECONDS have nowhere to be written, so an assignment to
        // one moves the state behind it and stores nothing. Asked only where
        // no record was found, which is where they always are.
        if (idx == shell_var_count &&
            shell_dynamic_assign(name, name_len, value))
                return true;

        // Attributes are a property of the name, so they act before any byte
        // is stored, and every one of them is out of line.
        if (idx < shell_var_count && shell_vars[idx].attributes)
        {
                b32 done = env_write_attributed(idx, name, name_len, value,
                                                assignment,
                                                address_of value);

                if (done)
                        return done == 1;

                idx = env_find_hashed_span(name, name_len, hash);
        }

        positive value_len = string_length(env_reading(value));
        positive needed = name_len + 1 + value_len + 1;

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
                env_record_append((string_address)(cell + 1), hash, name_len,
                                  value_len, true, allexport);

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

COLD PURE p8 shell_variable_attributes(const_string name, positive length)
{
        positive found = env_find_span(name, length);

        return found < shell_var_count ? shell_vars[found].attributes : 0;
}

COLD bool shell_variable_attribute_set(const_string name, positive length, p8 set,
                                  p8 clear)
{
        env_variable address_to variable = env_export_take(name, length);

        if (!variable)
                return false;

        if (set & SHELL_ARRAY_EITHER)
        {
                if (!variable->array)
                {
                        b32 slot = array_table_take();

                        if (!slot)
                                return false;

                        variable->array = slot;
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

        return true;
}

COLD positive shell_array_length(const_string name, positive length)
{
        positive found = env_find_span(name, length);
        env_variable address_to variable;

        if (found >= shell_var_count && shell_frames_wanted(name, length))
                found = env_find_span(name, length);

        if (found >= shell_var_count)
                return 0;

        variable = shell_vars + found;

        return (variable->array ? array_tables[variable->array - 1].count : 0) +
               (env_variable_has_value(variable) ? 1 : 0);
}

// What ${a[-1]} counts back from. Bash names the last element by the largest
// subscript in use and not by how many there are, so a hole does not move it.
COLD PURE positive shell_array_highest(const_string name, positive length)
{
        positive found = env_find_span(name, length);
        array_table address_to table;

        if (found >= shell_var_count)
                return 0;

        table = array_table_of(shell_vars + found);

        return table && table->count ? table->element[table->count - 1].key : 0;
}

COLD positive shell_array_items(const_string name, positive length,
                           shell_array_item address_to items, positive room)
{
        positive found = env_find_span(name, length);
        env_variable address_to variable;
        array_table address_to table;
        positive count = 0;

        if (found >= shell_var_count && shell_frames_wanted(name, length))
                found = env_find_span(name, length);

        if (found >= shell_var_count)
                return 0;

        variable = shell_vars + found;
        table = array_table_of(variable);

        if (env_variable_has_value(variable))
        {
                if (count < room)
                {
                        items[count].index = 0;
                        items[count].key = null;
                        items[count].key_length = 0;
                        items[count].value = variable->text + length + 1;
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
        positive found = env_find_span(name, length);
        env_variable address_to variable;
        array_table address_to table;
        positive at;

        if (found >= shell_var_count && shell_frames_wanted(name, length))
                found = env_find_span(name, length);

        if (found >= shell_var_count)
                return null;

        variable = shell_vars + found;
        table = array_table_of(variable);

        if (variable->attributes & SHELL_ARRAY_ASSOCIATIVE)
        {
                if (!table)
                        return null;

                at = array_keyed_place(
                    table, memory_hash_33((address_any)key, key_length), key,
                    key_length);

                if (at >= table->count)
                        return null;

                if (value_length)
                        address_to value_length =
                            table->element[at].value_length;

                return array_element_value(table->element + at);
        }

        {
                positive index = array_index_of(key, key_length);

                if (!index)
                {
                        if (!env_variable_has_value(variable))
                                return null;

                        if (value_length)
                                address_to value_length =
                                    variable->value_length;

                        return variable->text + length + 1;
                }

                if (!table)
                        return null;

                at = array_place(table, index);

                if (at >= table->count || table->element[at].key != index)
                        return null;

                if (value_length)
                        address_to value_length =
                            table->element[at].value_length;

                return array_element_value(table->element + at);
        }
}

/*
        Subscript zero of an indexed array is the variable's own value.

        Writing it is therefore the ordinary scalar write, and appending to
        it the ordinary scalar append, which is what keeps a=(x); a[0]+=y
        and a=x agreeing about where the bytes live.
*/
static COLD bool array_scalar_write(const_string name, positive length,
                               positive hash, const_string value, bool append)
{
        shell_mark held;
        string_address old;
        positive old_length;
        positive add_length;
        p8 address_to joined;
        bool answer;

        if (!append)
                return env_write_hashed_span(name, length, hash, value, true);

        old = env_get_hashed_span(name, length, hash, address_of old_length);

        if (!old)
                return env_write_hashed_span(name, length, hash, value, true);

        add_length = string_length(env_reading(value));

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

        memory_copy(joined, old, old_length);
        memory_copy_end(joined + old_length, env_reading(value), add_length);
        answer = env_write_hashed_span(name, length, hash, joined, true);
        shell_store_rewind(address_of expand_store, held);

        return answer;
}

COLD bool shell_array_set(const_string name, positive length, const_string key,
                     positive key_length, const_string value, bool append)
{
        positive hash = env_name_hash(name, length);
        env_variable address_to variable =
            env_export_take_hashed(name, length, hash);
        array_table address_to table;
        bool keyed;
        positive index = 0;
        positive at;

        if (!variable)
                return false;

        keyed = (variable->attributes & SHELL_ARRAY_ASSOCIATIVE) != 0;

        if (!keyed)
        {
                // A subscript on a name nobody declared declares it indexed,
                // which is what `a[5]=w` on an unknown name means in Bash.
                variable->attributes |=
                    SHELL_ARRAY_INDEXED | SHELL_ARRAY_ASSIGNED;
                variable->declared = true;
                index = array_index_of(key, key_length);

                if (!index)
                        return array_scalar_write(name, length, hash, value,
                                                  append);
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
        table = array_table_of(variable);

        if (keyed)
        {
                positive key_hash =
                    memory_hash_33((address_any)key, key_length);

                at = array_keyed_place(table, key_hash, key, key_length);

                return array_element_write(table, at, at >= table->count,
                                           key_hash, key, key_length, value,
                                           string_length(env_reading(value)),
                                           append);
        }

        at = array_place(table, index);

        return array_element_write(
            table, at, at >= table->count || table->element[at].key != index,
            index, null, 0, value, string_length(env_reading(value)), append);
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
        positive found = env_find_span(name, length);
        env_variable address_to variable;
        array_table address_to table;

        if (found >= shell_var_count)
                return true;

        variable = shell_vars + found;
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
COLD bool shell_array_words(const_string name, positive length,
                       string_address address_to words, positive count)
{
        p8 written[32];

        if (!shell_variable_attribute_set(name, length,
                                          SHELL_ARRAY_INDEXED |
                                              SHELL_ARRAY_ASSIGNED,
                                          SHELL_ARRAY_ASSOCIATIVE) ||
            !shell_array_clear(name, length))
                return false;

        for (positive at = 0; at < count; at++)
                if (!shell_array_set(name, length, written,
                                     bipolar_into_string(written, (bipolar)at),
                                     words[at], false))
                        return false;

        return true;
}

COLD bool shell_array_numbers(const_string name, positive length,
                         bipolar address_to values, positive count)
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
                number[bipolar_into_string(number, values[at])] = end;

                if (!shell_array_set(name, length, written,
                                     bipolar_into_string(written, (bipolar)at),
                                     number, false))
                        return false;
        }

        return true;
}

COLD bool shell_array_forget(const_string name, positive length, const_string key,
                        positive key_length)
{
        positive found = env_find_span(name, length);
        env_variable address_to variable;
        array_table address_to table;
        positive at;

        if (found >= shell_var_count)
                return true;

        variable = shell_vars + found;
        table = array_table_of(variable);

        if (variable->attributes & SHELL_ARRAY_ASSOCIATIVE)
        {
                if (!table)
                        return true;

                at = array_keyed_place(
                    table, memory_hash_33((address_any)key, key_length), key,
                    key_length);

                if (at < table->count)
                        array_element_forget(table, at);

                return true;
        }

        {
                positive index = array_index_of(key, key_length);

                /*
                        Subscript zero is the variable's own value, so losing
                        it is the transition an exported name that has no
                        value yet already stands for: the name remains and
                        the value does not. Inherited text may not be written
                        through, so that case takes a cell of its own.
                */
                if (!index)
                {
                        if (!env_variable_has_value(variable))
                                return true;

                        if (variable->owned)
                                variable->text[length] = end;
                        else
                        {
                                env_cell address_to cell =
                                    env_cell_take(length + 1);

                                if (!cell)
                                        return false;

                                memory_copy_end((p8 address_to)(cell + 1),
                                                (address_any)name, length);
                                variable->text = (string_address)(cell + 1);
                                variable->owned = true;
                        }

                        variable->value_length = 0;
                        shell_envp_dirty = true;

                        return true;
                }

                if (!table)
                        return true;

                at = array_place(table, index);

                if (at < table->count && table->element[at].key == index)
                        array_element_forget(table, at);

                return true;
        }
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

        if (length == 13 &&
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

positive shell_line_number;
positive shell_subshell_depth;

COLD string_address shell_dynamic_value(const_string name, positive length,
                                        positive hash,
                                        positive address_to value_length)
{
        string_address text = env_reading(name);

        (void)hash;

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

                if (!memory_compare((address_any)text, "LINENO", 6))
                        return shell_dynamic_number(shell_line_number,
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
                break;

        case 12:
                if (!memory_compare((address_any)text, "BASH_VERSION", 12))
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

                write("\"", 1);
                write(value, quotes);
                write("\"", 1);
                value += quotes;

                if (!string_get(value))
                        return;
        }
}

// Twenty places said this sentence and left two behind: what a builtin says
// when the store is full, and the status it answers with.
static fn shell_no_room(string_address command)
{
        string_format(shell_diagnostic, "%s: no room\n", command);
        shell_answer(2);
}

// An assignment a readonly name refused stops a script where it stands,
// which is what the reference shell does for a special builtin.
static fn shell_readonly_refused(string_address name, positive length)
{
        shell_diagnostic(name, length);
        shell_diagnostic(": is read only\n", 0);
        expand_fatal();
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

/*
        The next option letter, and where the words stop being options.

        Six builtins walked their -abc words with the same loop, and one of
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

static bool shell_option_letter(shell_option_walk address_to walk,
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

// The one walk over every name the shell holds, in order. It lives with
// declare below, which is what it was written for; set, export and readonly
// list through it too so that the four agree on the order.
typedef fn(address_to shell_name_writer)(writer write, string_address name,
                                         positive length, b32 mark);
static bool shell_names_sorted(writer write, b32 mark,
                               shell_name_writer written);

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

#define SHELL_ASSIGNER(function, readonly_text, failed_text)                 \
        static bool function(string_address name, string_address value)     \
        {                                                                    \
                if (env_assign(name, value))                                 \
                        return true;                                         \
                string_format(shell_diagnostic,                             \
                              env_readonly(name) ? (readonly_text)           \
                                                 : (failed_text), name);     \
                return false;                                                \
        }

SHELL_ASSIGNER(shell_cd_variable, "cd: %s: is read only\n",
               "cd: cannot assign %s\n")
SHELL_ASSIGNER(read_set, "read: %s is readonly\n", "read: no room for %s\n")
#undef SHELL_ASSIGNER

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

        if (system_change_directory(wanted))
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
                                                 variables_set))
                                {
                                        if (walk.length)
                                                address_to say = true;

                                        return true;
                                }
                        }
                }
        }

        path_join(candidate, sizeof(candidate), shell_directory,
                  shell_cd_target);

        return shell_cd_try(candidate, physical, physical_named,
                            variables_set);
}

COLD fn shell_cd(writer write, string_address input)
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

                if (word_is(word, "--"))
                {
                        index++;
                        break;
                }

                // -1 is the entry one from the bottom, not an option word,
                // so a digit ends the option scan.
                if (word[1] >= '0' && word[1] <= '9')
                        break;

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
                                p8 said[2] = {letter, end};

                                string_format(shell_diagnostic,
                                              "dirs: -%s: invalid option\n",
                                              said);
                                return shell_answer(2);
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

                if (!shell_dirstack_index(shell_argv[index], count,
                                          address_of wanted))
                {
                        string_format(shell_diagnostic, "dirs: %s: %s\n",
                                      shell_argv[index],
                                      "directory stack index out of range");
                        return shell_answer(1);
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

        return shell_cd_try(where, false, address_of physical_named,
                            address_of variables_set);
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

        // With no operand the top two are exchanged, which needs something
        // under the top to exchange with.
        if (shell_argc < 2)
        {
                if (count < 2)
                {
                        shell_diagnostic("pushd: no other directory\n", 0);
                        return shell_answer(1);
                }

                for (positive at = 0; at < count; at++)
                        rotated[at] = list[at];

                rotated[0] = list[1];
                rotated[1] = list[0];
        }
        else if (shell_dirstack_index(shell_argv[1], count, address_of index))
        {
                if (!index)
                {
                        shell_dirstack_listed(write, false, false, false);
                        return shell_answer(0);
                }

                // A rotation names a new top and drops nothing, so the walk
                // goes once round the list from the entry asked for.
                for (positive at = 0; at < count; at++)
                        rotated[at] = list[(at + index) % count];
        }
        else if (string_is(shell_argv[1], '+') || string_is(shell_argv[1], '-'))
        {
                string_format(shell_diagnostic,
                              "pushd: %s: directory stack index out of range\n",
                              shell_argv[1]);
                return shell_answer(1);
        }
        else
        {
                // The directory the shell is in is about to be written over
                // by the move, and it is the entry being pushed.
                string_copy_max_end(previous, shell_directory,
                                    sizeof(previous) - 1);
                string_copy_max_end(wanted, shell_argv[1], sizeof(wanted) - 1);

                rotated[0] = previous;

                for (positive at = 1; at < count; at++)
                        rotated[at] = list[at];

                if (!shell_dirstack_move(wanted))
                {
                        string_format(shell_diagnostic,
                                      "pushd: %s: no such directory\n",
                                      shell_argv[1]);
                        return shell_answer(1);
                }

                if (!shell_dirstack_write(rotated, count))
                {
                        shell_diagnostic("pushd: directory stack full\n", 0);
                        return shell_answer(1);
                }

                shell_dirstack_listed(write, false, false, false);

                return shell_answer(0);
        }

        string_copy_max_end(wanted, rotated[0], sizeof(wanted) - 1);

        // Written before the move, because the kept part is read out of the
        // pool the move does not touch and the top is already in hand.
        if (!shell_dirstack_write(rotated + 1, count - 1))
        {
                shell_diagnostic("pushd: directory stack full\n", 0);
                return shell_answer(1);
        }

        if (!shell_dirstack_move(wanted))
        {
                string_format(shell_diagnostic,
                              "pushd: %s: no such directory\n", wanted);
                return shell_answer(1);
        }

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

        if (count < 2)
        {
                shell_diagnostic("popd: directory stack empty\n", 0);
                return shell_answer(1);
        }

        if (shell_argc > 1 &&
            !shell_dirstack_index(shell_argv[1], count, address_of index))
        {
                string_format(shell_diagnostic,
                              "popd: %s: directory stack index out of range\n",
                              shell_argv[1]);
                return shell_answer(1);
        }

        for (positive at = 0; at < count; at++)
                if (at != index)
                        kept[used++] = list[at];

        // Removing the top is the only one that moves the shell; taking an
        // entry out from under it leaves it where it is.
        if (!index)
        {
                string_copy_max_end(wanted, kept[0], sizeof(wanted) - 1);

                if (!shell_dirstack_move(wanted))
                {
                        string_format(shell_diagnostic,
                                      "popd: %s: no such directory\n", wanted);
                        return shell_answer(1);
                }
        }

        if (!shell_dirstack_write(kept + 1, used - 1))
        {
                shell_diagnostic("popd: directory stack full\n", 0);
                return shell_answer(1);
        }

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
        echo.

        The escapes are read, which is what the reference shell does and what
        Bash's xpg_echo asks for; -E stops that and -e starts it again. Bash
        takes a whole run of option words and the reference shell takes one
        and prints the rest, so one is what is taken here -- the two disagree
        about `echo -n -n x` and this shell has always answered as the
        reference does.
*/
fn shell_echo(writer write, string_address input)
{
        positive index = 1;
        bool newline = true;
        bool escapes = true;

        printf_cut = false;

        if (index < shell_argc && string_is(shell_argv[index], '-') &&
            string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                while (string_is(letter, 'n') || string_is(letter, 'e') ||
                       string_is(letter, 'E'))
                        letter++;

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
        }

        for (positive first = index; index < shell_argc; index++)
        {
                if (index != first)
                        write(" ", 1);

                if (!escapes)
                {
                        write(shell_argv[index],
                              string_length(shell_argv[index]));
                        continue;
                }

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
        p8 address_to found = null;
        positive found_room = 0;
        string_address address_to environment;
        static string_address empty_environment[1];
        p8 login_name[256];
        string_address named = null;
        bool clear = false;
        bool login = false;
        positive index = 1;
        bipolar located;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;
                bool wanted = false;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

                        if (which == 'c')
                                clear = true;
                        else if (which == 'l')
                                login = true;
                        else if (which == 'a')
                        {
                                wanted = true;
                                break;
                        }
                        else
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "exec: -%s: invalid option\n",
                                              said);
                                // A special builtin's failure ends a script,
                                // but these three options are Bash's and Bash
                                // leaves the script standing.
                                return shell_answer(2);
                        }
                }

                index++;

                if (!wanted)
                        continue;

                if (string_get(letter))
                        named = letter;
                else if (index < shell_argc)
                        named = shell_argv[index++];
                else
                {
                        shell_diagnostic("exec: -a: option requires an "
                                         "argument\n", 0);
                        return shell_answer(2);
                }
        }

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

        located = shell_find_in_path_alloc(shell_argv[1], address_of found,
                                           address_of found_room);

        if (located < 0)
        {
                if (found)
                        memory_free(found, found_room);

                shell_no_room("exec");
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

        environment = clear ? empty_environment : shell_environment();
        if (!environment)
        {
                memory_free(found, found_room);
                shell_answer(2);
                string_format(shell_diagnostic, "exec: no room for environment\n");
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

COLD fn shell_pwd(writer write, string_address input)
{
        p8 out_buffer[4096];
        bool physical = shell_argc > 1 && word_is(shell_argv[1], "-P");

        if (!physical && shell_directory_holds())
                return string_format(write, "%s\n", shell_directory);

        shell_here(out_buffer, sizeof(out_buffer));

        string_format(write, "%s\n", out_buffer);
}

fn shell_trap_exit();

DEAD_END COLD fn shell_exit(writer write, string_address input)
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

COLD fn shell_reboot(writer write, string_address input)
{
        shell_stop(write, REBOOT_RESTART);
}

COLD fn shell_poweroff(writer write, string_address input)
{
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
    {null, 0},
};

#define SHELL_EXTRA_OPTIONS (array_count(shell_extra_options) - 1)
#define SHELL_EXTRA_ERRTRACE 0
#define SHELL_EXTRA_FUNCTRACE 1

static positive shell_extra_state;

static PURE bool shell_extra_on(positive which)
{
        return (shell_extra_state & ((positive)1 << which)) != 0;
}

static bool shell_extra_told(string_address word, bool on)
{
        positive index = string_table_find(word, shell_extra_options,
                                           sizeof(shell_extra_options[0]),
                                           SHELL_EXTRA_OPTIONS);

        if (index >= SHELL_EXTRA_OPTIONS)
                return false;

        if (on)
                shell_extra_state |= (positive)1 << index;
        else
                shell_extra_state &= ~((positive)1 << index);

        return true;
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
        (array_count(shell_option_names))
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
                        if (shell_option_names[index].value == letter)
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
        {
                shell_options |= SHELL_FLAG('i');

                // Somebody is watching, so job control is on: that is what
                // makes control-Z a job rather than a stopped shell.
                shell_option_told(SHELL_OPTION_MONITOR, true);
        }
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
                return shell_extra_told(word, on);

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

static COLD fn shell_shopt_padded(writer write, string_address name,
                             positive width, bool on)
{
        positive length = string_length(name);

        write(name, length);

        while (length++ < width)
                write(" ", 1);

        write("\t", 1);
        write(on ? "on\n" : "off\n", on ? 3 : 4);
}

static COLD fn shell_shopt_said(writer write, positive which, bool as_commands)
{
        bool on = shell_shopt_index_on(which);

        if (as_commands)
        {
                write(on ? "shopt -s " : "shopt -u ", 9);
                string_format(write, "%s\n", shell_shopt_names[which]);
                return;
        }

        shell_shopt_padded(write, shell_shopt_names[which], 20, on);
}

static COLD fn shell_shopt_option_said(writer write, positive which,
                                  bool as_commands)
{
        bool on = shell_option_on(which);

        if (as_commands)
        {
                write(on ? "set -o " : "set +o ", 7);
                string_format(write, "%s\n", shell_option_names[which].name);
                return;
        }

        shell_shopt_padded(write, shell_option_names[which].name, 15, on);
}

COLD fn shell_shopt(writer write, string_address input)
{
        positive index = 1;
        bool set = false;
        bool unset = false;
        bool quiet = false;
        bool as_commands = false;
        bool set_options = false;
        bool all_on = true;
        bool bad = false;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

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
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "shopt: -%s: invalid option\n",
                                              said);
                                return shell_answer(2);
                        }
                }

                index++;
        }

        // Both directions at once has no answer, so it is refused rather
        // than resolved into whichever was written last.
        if (set && unset)
        {
                shell_diagnostic(
                    "shopt: cannot set and unset shell options simultaneously\n",
                    0);
                return shell_answer(1);
        }

        if (index >= shell_argc)
        {
                positive count = set_options ? SHELL_OPTION_NAMES
                                             : SHELL_SHOPT_NAMES;

                // -q with nothing to ask about is a question with no
                // subject, and Bash answers yes to it.
                if (quiet)
                        return shell_answer(0);

                for (positive at = 0; at < count; at++)
                {
                        bool on = set_options ? shell_option_on(at)
                                              : shell_shopt_index_on(at);

                        // A bare -s or -u asks for the names in that state
                        // and not for a change to every one of them.
                        if ((set && !on) || (unset && on))
                                continue;

                        if (set_options)
                                shell_shopt_option_said(write, at,
                                                        as_commands);
                        else
                                shell_shopt_said(write, at, as_commands);
                }

                return shell_answer(quiet && !all_on ? 1 : 0);
        }

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                positive which = set_options
                                   ? string_table_find(
                                         name, shell_option_names,
                                         sizeof(shell_option_names[0]),
                                         SHELL_OPTION_NAMES)
                                   : shell_shopt_find(name);

                if (which >= (set_options ? SHELL_OPTION_NAMES
                                          : SHELL_SHOPT_NAMES))
                {
                        string_format(shell_diagnostic,
                                      "shopt: %s: invalid shell option name\n",
                                      name);
                        bad = true;
                        continue;
                }

                if (set || unset)
                {
                        if (set_options)
                                shell_option_told(which, set);
                        else if (set)
                                shell_shopt_state |= (positive)1 << which;
                        else
                                shell_shopt_state &= ~((positive)1 << which);

                        continue;
                }

                all_on = all_on &&
                         (set_options ? shell_option_on(which)
                                      : shell_shopt_index_on(which));

                if (quiet)
                        continue;

                if (set_options)
                        shell_shopt_option_said(write, which, as_commands);
                else
                        shell_shopt_said(write, which, as_commands);
        }

        if (bad)
                return shell_answer(1);

        shell_answer(set || unset ? 0 : (all_on ? 0 : 1));
}

// A bare set is the variables as lines the shell could be fed: sorted, and
// the value quoted, which is what the reference shell prints and what a
// script that saves its state to a file is counting on.
static fn shell_set_written(writer write, string_address name,
                            positive length, b32 mark)
{
        positive found = env_find_span(name, length);

        (void)mark;

        if (found >= shell_var_count ||
            !env_variable_has_value(shell_vars + found))
                return;

        write(name, length);
        write("=", 1);
        shell_quoted(write, shell_vars[found].text + length + 1);
        write("\n", 1);
}

COLD fn shell_set(writer write, string_address input)
{
        positive index = 1;
        bool operands = false;

        if (shell_argc < 2)
        {
                if (!shell_names_sorted(write, 0, shell_set_written))
                        return shell_no_room("set");

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

                                        // A name nobody has ends the script
                                        // the same as a letter nobody has:
                                        // set is a special builtin, and the
                                        // reference shell leaves 2 behind.
                                        if (!shell_option_named(shell_argv[++index], on))
                                        {
                                                shell_diagnostic(
                                                    on ? "set: Illegal option -o "
                                                       : "set: Illegal option +o ",
                                                    23);
                                                string_format(shell_diagnostic,
                                                              "%s\n",
                                                              shell_argv[index]);
                                                shell_answer(2);
                                                expand_fatal();
                                                return;
                                        }

                                        letter++;
                                        continue;
                                }

                                positive option;

                                for (option = 0; option < SHELL_OPTION_NAMES;
                                     option++)
                                        if (shell_option_names[option].value == value)
                                                break;

                                if (option < SHELL_OPTION_NAMES)
                                        shell_option_told(option, on);
                                else if (shell_extra_letter(value, on))
                                        ;
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

COLD fn shell_unset(writer write, string_address input)
{
        shell_option_walk walk = {1};
        positive index;
        bool functions = false;
        p8 letter;

        while (shell_option_letter(address_of walk, address_of letter))
        {
                if (letter == 'f')
                        functions = true;
                else if (letter == 'v')
                        functions = false;
                else
                {
                        // A special builtin, so a letter it does not have
                        // ends the script, as the reference shell's does.
                        string_format(shell_diagnostic,
                                      "unset: Illegal option -%c\n", letter);
                        shell_answer(2);
                        expand_fatal();
                        return;
                }
        }

        index = walk.index;

        while (index < shell_argc)
        {
                string_address word = shell_argv[index];
                positive word_length = string_length(word);
                string_address bracket =
                    functions ? null : string_first_of(word, '[');

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

                        if (env_readonly_span(word, base))
                        {
                                shell_readonly_refused(word, base);
                                return;
                        }

                        key = shell_expand_subscript(word, base, bracket + 1,
                                                     word_length - base - 2,
                                                     address_of key_length);

                        if (!key ||
                            !shell_array_forget(word, base, key, key_length))
                        {
                                shell_no_room("unset");
                                return;
                        }

                        index++;
                        continue;
                }

                if (!shell_valid_name(word, word_length))
                {
                        shell_bad_name("unset", word, word_length);
                        return;
                }

                if (!functions && env_readonly(word))
                {
                        shell_readonly_refused(word, word_length);
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
        // What kind of name it was, and the elements it held. An array
        // local has to come back as the array it was and not as the string
        // its element zero happened to be.
        p8 attributes;
        positive element_from;
        positive element_count;
} shell_local_entry;

// One saved element: the key, a nul, then the value. Both are terminated
// because putting the array back names each element, and the live cells are
// gone by then.
typedef struct
{
        string_address text;
        positive key_length;
} local_element;

static local_element address_to local_elements;
static positive local_element_room;
static positive local_element_count;

static shell_local_entry address_to local_table;
static positive local_room;
static positive local_count;
static positive local_initialized;
static positive address_to local_from;
static positive local_from_room;
static positive local_depth;

static COLD bool local_keep_array(shell_local_entry address_to entry,
                             string_address name, positive length)
{
        positive count = shell_array_length(name, length);
        shell_mark held = shell_store_mark(address_of expand_store);
        shell_array_item address_to items;
        p8 written[32];
        bool answer = true;

        if (!count)
                return true;

        items = (shell_array_item address_to)shell_store_take(
            address_of expand_store, count * sizeof(items[0]));

        if (!items || !shell_array_room(local_elements, local_element_room,
                                        local_element_count + count))
        {
                shell_store_rewind(address_of expand_store, held);
                return false;
        }

        shell_array_items(name, length, items, count);

        for (positive at = 0; at < count; at++)
        {
                string_address key = items[at].key;
                positive key_length = items[at].key_length;
                env_cell address_to cell;

                if (!key)
                {
                        key_length = bipolar_into_string(
                            written, (bipolar)items[at].index);
                        key = written;
                }

                cell = env_cell_take(key_length +
                                     items[at].value_length + 2);

                if (!cell)
                {
                        answer = false;
                        break;
                }

                memory_copy_end((p8 address_to)(cell + 1), key, key_length);
                memory_copy_end((p8 address_to)(cell + 1) + key_length + 1,
                                items[at].value, items[at].value_length);
                local_elements[local_element_count].text =
                    (string_address)(cell + 1);
                local_elements[local_element_count].key_length = key_length;
                local_element_count++;
                entry->element_count++;
        }

        shell_store_rewind(address_of expand_store, held);

        return answer;
}

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
                string_address name;
                positive length;
                p8 saved;

                at--;
                name = local_table[at].text;
                length = local_table[at].name_length;
                saved = local_table[at].attributes;

                /*
                        An array is put back element by element, because the
                        clear in front of that is what makes leaving the
                        function a replacement rather than a merge. A name
                        that was not an array cannot be left as one either,
                        which is the same clear with nothing to put back.
                */
                if ((saved & SHELL_ARRAY_EITHER) ||
                    (shell_variable_attributes(name, length) &
                     SHELL_ARRAY_EITHER))
                {
                        shell_array_clear(name, length);
                        shell_variable_attribute_set(name, length, saved,
                                                     (p8)~saved);

                        for (positive one = 0;
                             one < local_table[at].element_count; one++)
                        {
                                local_element address_to kept =
                                    local_elements +
                                    local_table[at].element_from + one;

                                shell_array_set(name, length, kept->text,
                                                kept->key_length,
                                                kept->text +
                                                    kept->key_length + 1,
                                                false);
                        }

                        if (!(saved & SHELL_ARRAY_EITHER) &&
                            !local_table[at].present)
                                env_unset(name);
                }
                else if (!local_table[at].present)
                        env_unset(name);
                else
                {
                        env_set(name, name + length + 1);

                        // What the function declared about the name goes
                        // with the function, so a global that was a plain
                        // string is one again.
                        if (saved || shell_variable_attributes(name, length))
                                shell_variable_attribute_set(name, length,
                                                             saved,
                                                             (p8)~saved);
                }

                for (positive one = 0; one < local_table[at].element_count;
                     one++)
                        env_cell_drop(
                            local_elements[local_table[at].element_from + one]
                                .text);

                local_element_count = local_table[at].element_from;

                env_export_restore(name, local_table[at].exported);
                env_declare_restore(name, local_table[at].declared);
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
        local_table[local_count].attributes =
            variable ? variable->attributes : 0;
        local_table[local_count].element_from = local_element_count;
        local_table[local_count].element_count = 0;

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

        if ((local_table[local_count].attributes & SHELL_ARRAY_EITHER) &&
            !local_keep_array(local_table + local_count, name, name_length))
                return -1;

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

#define DECLARE_EXPORT 1
#define DECLARE_READONLY 2
#define DECLARE_PRINT 4
#define DECLARE_GLOBAL 8
// The attributes that live on the variable rather than beside it. They are
// held in the same order the option letters take, so that turning a letter
// into a bit is a table and not a ladder.
#define DECLARE_ATTRIBUTE 16

typedef struct
{
        positive index;
        b32 set;
        b32 clear;
        p8 attributes_set;
        p8 attributes_clear;
} shell_declare_state;

static PURE p8 shell_declare_attribute(p8 letter)
{
        return letter == 'a'   ? SHELL_ARRAY_INDEXED
               : letter == 'A' ? SHELL_ARRAY_ASSOCIATIVE
               : letter == 'i' ? SHELL_ARRAY_INTEGER
               : letter == 'l' ? SHELL_ARRAY_LOWER
               : letter == 'u' ? SHELL_ARRAY_UPPER
               : letter == 'n' ? SHELL_ARRAY_NAMEREF
                               : 0;
}

static bool shell_declare_options(shell_declare_state address_to state)
{
        shell_option_walk walk = {state->index, null, 0, true};
        p8 value;

        while (shell_option_letter(address_of walk, address_of value))
        {
                p8 direction = walk.direction;
                p8 attribute = shell_declare_attribute(value);
                b32 flag;

                flag = value == 'x' ? DECLARE_EXPORT
                       : value == 'r' ? DECLARE_READONLY
                       : value == 'p' && direction == '-' ? DECLARE_PRINT
                       : value == 'g' && direction == '-' ? DECLARE_GLOBAL
                       : attribute ? DECLARE_ATTRIBUTE
                                   : 0;

                if (!flag)
                {
                        string_format(shell_diagnostic,
                                      "%s: %c%c: invalid option\n",
                                      shell_argv[0], direction, value);
                        shell_answer(2);
                        return false;
                }

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

        state->index = walk.index;
        state->set &= ~state->clear;
        state->attributes_set &= (p8)~state->attributes_clear;
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

// A subscript is written bare when it could be typed back bare, and quoted
// the way a value is when it could not. Bash draws the line at a name.
static COLD fn shell_declare_key(writer write, string_address key, positive length)
{
        for (positive at = 0; at < length; at++)
                if (!expand_name_character(key[at]))
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
        // show nothing, so the difference is kept rather than tidied.
        if (keyed)
                write(" ", 1);

        write(")", 1);
        shell_store_rewind(address_of expand_store, held);
}

static bool shell_declare_print_one(writer write, string_address name,
                                    positive length, b32 filter)
{
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
                // The order Bash prints them in, which is not the order they
                // can be given in and is what a listing has to match.
                if (attributes & SHELL_ARRAY_INDEXED)
                        write("a", 1);
                if (attributes & SHELL_ARRAY_ASSOCIATIVE)
                        write("A", 1);
                if (attributes & SHELL_ARRAY_INTEGER)
                        write("i", 1);
                if (attributes & SHELL_ARRAY_NAMEREF)
                        write("n", 1);
                if (readonly)
                        write("r", 1);
                if (exported)
                        write("x", 1);
                if (attributes & SHELL_ARRAY_LOWER)
                        write("l", 1);
                if (attributes & SHELL_ARRAY_UPPER)
                        write("u", 1);
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
        own. A readonly name with no value is not in the vector at all and is
        added from its own table, so readonly -p can list it.
*/
static bool shell_names_sorted(writer write, b32 mark,
                               shell_name_writer written)
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
                written(write, names[at], string_length(names[at]), mark);

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

static bool shell_declare_print_all(writer write, b32 filter)
{
        return shell_names_sorted(write, filter, shell_declare_written);
}

/*
        A value written the way `declare` with no operands writes one.

        Bash quotes here only when the value has something in it that would
        not survive being read back as a word, so `a=b` comes out bare and
        `a b` comes out quoted. `set` quotes everything and is compared
        against the reference shell, which does the same; this is Bash's
        listing and its own rule.
*/
static COLD PURE bool shell_listing_quoted(string_address value)
{
        static const p8 wanted[] = " \t\n'\"\\|&;()<>!{}*?[]$`^";

        while (string_get(value))
        {
                p8 byte = string_get(value++);

                if (byte < ' ' || byte == 127 ||
                    string_first_of((string_address)wanted, byte))
                        return true;
        }

        return false;
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

        if (shell_listing_quoted(value))
                shell_quoted(write, value);
        else
                write(value, string_length(value));

        write("\n", 1);
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

COLD fn shell_local(writer write, string_address input)
{
        shell_declare_state state = {1};
        bool failed = false;

        if (!local_depth)
        {
                shell_diagnostic("local: not in a function\n", 0);
                expand_fatal();
                return;
        }

        // local takes declare's attribute letters, and a nameref given one
        // is the whole point of local -n ref=$1.
        if (!shell_declare_options(address_of state))
                return;

        while (state.index < shell_argc)
        {
                string_address word = shell_argv[state.index++];
                string_address mark = string_first_of(word, '=');
                bool append = mark && mark > word && string_is(mark - 1, '+');
                string_address name_end = mark ? mark - append : null;
                positive length = mark ? (positive)(name_end - word)
                                       : string_length(word);
                p8 delimiter = mark ? string_get(name_end) : 0;
                bool compound = mark && string_is(mark + 1, '(');

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

                if ((state.attributes_set || state.attributes_clear) &&
                    !shell_variable_attribute_set(word, length,
                                                  state.attributes_set,
                                                  state.attributes_clear))
                {
                        shell_no_room("local");
                        failed = true;
                }
                else if (mark && compound)
                {
                        positive body = string_length(mark + 1);

                        if (!shell_compound_assign(word, length, mark + 2,
                                                   body > 2 ? body - 2 : 0,
                                                   append))
                        {
                                shell_no_room("local");
                                failed = true;
                        }
                }
                else if (mark && !shell_declare_assign(word, mark + 1, append))
                {
                        // A readonly name is the usual reason and reads
                        // nothing like running out of room.
                        if (env_readonly(word))
                        {
                                shell_diagnostic(word, length);
                                shell_diagnostic(": is read only\n", 0);
                                shell_answer(2);
                        }
                        else
                                shell_no_room("local");

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
                else if (state.set & DECLARE_PRINT)
                        failed = !shell_declare_print_all(write, state.set);
                else
                {
                        /*
                                Named nothing and not asked for -p, so this is
                                the listing and not the print: Bash writes the
                                variables the way `set` does, as assignments a
                                shell could be fed, rather than as the declare
                                commands that would rebuild their attributes.
                        */
                        failed = !shell_names_sorted(write, 0,
                                                     shell_declare_listed);
                }

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
                bool compound = mark && string_is(mark + 1, '(');
                p8 held_attributes;
                bool readonly;

                if (!shell_valid_name(word, length))
                {
                        shell_bad_name(shell_argv[0], word, length);
                        return;
                }

                /*
                        An array has one kind for its whole life. Bash
                        refuses to reinterpret the subscripts it already
                        holds rather than answering to both spellings.
                */
                held_attributes = shell_variable_attributes(word, length);

                if ((state.attributes_set & SHELL_ARRAY_EITHER) &&
                    (held_attributes & SHELL_ARRAY_EITHER) &&
                    (held_attributes & SHELL_ARRAY_EITHER) !=
                        (state.attributes_set & SHELL_ARRAY_EITHER))
                {
                        string_format(
                            shell_diagnostic,
                            "%s: %s: cannot convert %s to %s array\n",
                            shell_argv[0], word,
                            (held_attributes & SHELL_ARRAY_ASSOCIATIVE)
                                ? "associative"
                                : "indexed",
                            (state.attributes_set & SHELL_ARRAY_ASSOCIATIVE)
                                ? "associative"
                                : "indexed");
                        failed = true;
                        continue;
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

                // The kind is decided before the value is written, because
                // an associative array reads its subscripts as bytes and an
                // indexed one as arithmetic, and the value about to be
                // assigned is full of subscripts.
                if ((state.attributes_set || state.attributes_clear) &&
                    !shell_variable_attribute_set(word, length,
                                                  state.attributes_set,
                                                  state.attributes_clear))
                        goto no_room;

                if (mark && readonly)
                {
                        address_to name_end = delimiter;
                        shell_readonly_refused(word, length);
                        return;
                }
                else if (mark && compound)
                {
                        positive body = string_length(mark + 1);

                        if (!shell_compound_assign(word, length, mark + 2,
                                                   body > 2 ? body - 2 : 0,
                                                   append))
                                goto no_room;
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
                if (mark)
                        address_to name_end = delimiter;
                return shell_no_room("declare");
        }

        shell_answer(failed ? 1 : 0);
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
                shell_quoted(write, variable->text + length + 1);
        }

        write("\n", 1);
}

static COLD fn shell_marked(writer write, p8 mark)
{
        string_address command = mark == DECLARE_EXPORT ? "export"
                                                        : "readonly";
        bool listed;
        positive index = shell_declaration_options(address_of listed);

        if (listed && index >= shell_argc)
        {
                if (!shell_names_sorted(write, mark, shell_marked_written))
                        return shell_no_room(command);

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
                        shell_bad_name(command, word, length);
                        return;
                }

                // The name on its own while it is looked up and marked; the
                // word is argv's and goes back the way it was.
                if (value)
                        address_to value = end;

                if (value && env_readonly(word))
                {
                        address_to value = '=';
                        shell_readonly_refused(word, length);
                        return;
                }

                // A name on its own is already marked here: every value
                // assigned to it later inherits the attribute.
                kept = (!value || env_assign(word, value + 1)) &&
                       (mark == DECLARE_EXPORT ? env_export_mark(word)
                                               : readonly_add(word, length));

                if (value)
                        address_to value = '=';

                if (!kept)
                        return shell_no_room(command);
        }

        shell_answer(0);
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
                                string_format(shell_diagnostic,
                                              "%s: Illegal number: %s\n",
                                              shell_argv[0], value);
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

        // Bash's second spelling of =, and the one scripts reach for because
        // [[ ]] wants it. POSIX has only the single one.
        if (first == '=' && second == '=' && !string_get(word + 2))
                return TEST_SAME;

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

/*
        Where printf -v gathers what it would otherwise have written.

        Its own store, because %b gathers into printf_hold below and the two
        would otherwise be the same bytes: a width on a %b rewinds that store
        to nothing, which would throw away everything the format had already
        produced.
*/
static p8 address_to printf_kept;
static positive printf_kept_room;
static positive printf_kept_used;

static fn printf_keeper(address_any data, positive length)
{
        if (length > positive_max - printf_kept_used ||
            !shell_room((address_any address_to)address_of printf_kept,
                        address_of printf_kept_room,
                        printf_kept_used + length, 1))
        {
                if (!printf_cut)
                        shell_diagnostic("printf: no room\n", 0);

                printf_status = 2;
                printf_cut = true;
                return;
        }

        memory_copy_apart(printf_kept + printf_kept_used, data, length);
        printf_kept_used += length;
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

static PURE bool printf_took_argument()
{
        return printf_argument < shell_argc;
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
        bool control = false;

        if (!string_get(text))
                return write("''", 2);

        while (string_get(step))
        {
                p8 value = string_get(step++);

                if (value < ' ' || value == 127)
                        control = true;
        }

        if (control)
        {
                write("$'", 2);

                for (step = text; string_get(step); step++)
                {
                        p8 value = string_get(step);
                        p8 letter = 0;

                        if (value == '\n')
                                letter = 'n';
                        else if (value == '\t')
                                letter = 't';
                        else if (value == '\r')
                                letter = 'r';
                        else if (value == 7)
                                letter = 'a';
                        else if (value == 8)
                                letter = 'b';
                        else if (value == 12)
                                letter = 'f';
                        else if (value == 11)
                                letter = 'v';
                        else if (value == 27)
                                letter = 'E';
                        else if (value == '\'' || value == '\\')
                                letter = value;

                        if (letter)
                        {
                                p8 pair[2] = {'\\', letter};

                                write(pair, 2);
                                continue;
                        }

                        if (value < ' ' || value == 127)
                        {
                                p8 octal[5] = {'\\', '0', '0', '0', 0};

                                octal[1] = (p8)('0' + (value >> 6));
                                octal[2] = (p8)('0' + ((value >> 3) & 7));
                                octal[3] = (p8)('0' + (value & 7));
                                write(octal, 4);
                                continue;
                        }

                        write(address_of value, 1);
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
                 positive width, bipolar precision, bool left, bool zero,
                 bool alternate)
{
        positive style = sign | ((positive)upper << 26) |
                         ((positive)left << 27) | ((positive)zero << 28);

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
fn printf_not_a_number(string_address word)
{
        string_format(shell_diagnostic, "printf: %s: expected numeric value\n", word);
        printf_status = 1;
}

/*
        The number an integer conversion reads out of its argument.

        strtoimax's grammar, which is what the reference shell reads with:
        blanks in front, a sign, 0x for sixteen and a leading 0 for eight --
        and before any of that, a quote, which means the byte after it. An
        empty or missing argument is zero and no complaint. What the digits
        leave behind is a complaint and not a refusal: the number read is
        printed, and "not completely converted" is said afterwards with the
        status, the same as no digits at all is zero and "expected numeric
        value". A conversion that refused every one of those printed nothing
        for "$maybe_empty" and set the status where every other shell did not.
*/
static bipolar printf_integer(string_address word)
{
        string_address at = word;
        positive magnitude;
        positive used = 0;
        bool negative = false;

        if (!word)
                return 0;

        at += string_span(at, string_set_blanks);

        if (!string_get(at))
                return 0;

        if (string_is(at, '\'') || string_is(at, '"'))
                return (bipolar)string_get(at + 1);

        if (string_is(at, '-'))
        {
                negative = true;
                at++;
        }
        else if (string_is(at, '+'))
                at++;

        if (string_is(at, '0') && (string_is(at + 1, 'x') || string_is(at + 1, 'X')))
        {
                magnitude = string_digits_hexadecimal_max(at + 2, positive_max,
                                                          address_of used);

                // "0x" with nothing after it is the zero and then an x.
                if (used)
                        at += 2 + used;
                else
                {
                        magnitude = 0;
                        used = 1;
                        at++;
                }
        }
        else if (string_is(at, '0'))
        {
                magnitude = string_digits_octal_max(at, positive_max,
                                                    address_of used);
                at += used;
        }
        else
        {
                magnitude = string_digits_max(at, positive_max, address_of used);
                at += used;
        }

        if (!used)
        {
                printf_not_a_number(word);
                return 0;
        }

        if (string_get(at))
        {
                string_format(shell_diagnostic,
                              "printf: %s: not completely converted\n", word);
                printf_status = 1;
        }

        return bipolar_from_magnitude(magnitude, negative);
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
                return 0.0;

        if (string_is(at, '\'') || string_is(at, '"'))
                return (decimal)(positive)string_get(at + 1);

        value = string_to_decimal(at, address_of stopped);

        if (stopped == at)
        {
                printf_not_a_number(word);
                return 0.0;
        }

        if (string_get(stopped))
        {
                string_format(shell_diagnostic,
                              "printf: %s: not completely converted\n", word);
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
                bool left;
                bool zero;
                bool plus;
                bool space;
                bool alternate;
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

                {
                        positive flags = conversion_flags_take(address_of step);

                        left = (flags & CONVERSION_FLAG_LEFT) != 0;
                        zero = (flags & CONVERSION_FLAG_ZERO) != 0;
                        plus = (flags & CONVERSION_FLAG_PLUS) != 0;
                        space = (flags & CONVERSION_FLAG_SPACE) != 0;
                        alternate = (flags & CONVERSION_FLAG_ALTERNATE) != 0;
                }

                if (string_is(step, '*'))
                {
                        bipolar asked = printf_integer(printf_next());

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
                                precision = printf_integer(printf_next());

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

                                // \c ends the output, but what stood in front
                                // of it is still written, in its field; the
                                // loop stops on the flag afterwards.
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
                                string_format(shell_diagnostic,
                                              "printf: %%(%s: invalid directive\n",
                                              shape);
                                printf_status = 2;
                                printf_cut = true;
                                continue;
                        }

                        step++;
                        when = printf_took_argument()
                                 ? printf_integer(printf_next())
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
                        bipolar value = printf_integer(printf_next());
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
                        bipolar value = printf_integer(printf_next());
                        positive base = conversion == 'o' ? 8 : (conversion == 'u' ? 10 : 16);

                        printf_number(write, (positive)value, 0, base, conversion == 'X',
                                      width, precision, left, zero, alternate);
                        continue;
                }

                /*
                        Not %a and %A. The standard layer's field writer has
                        no hexadecimal float in it, and it answers one of
                        these with the decimal spelling instead -- a wrong
                        number, where refusing the directive is merely a
                        missing one. They stay refused until it grows the
                        path, which is the same place awk would read it from.
                */
                if (conversion == 'f' || conversion == 'F' ||
                    conversion == 'e' || conversion == 'E' ||
                    conversion == 'g' || conversion == 'G')
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

                        spec.flags = (left ? FORMAT_FLAG_LEFT : 0) |
                                     (plus ? FORMAT_FLAG_PLUS : 0) |
                                     (space ? FORMAT_FLAG_SPACE : 0) |
                                     (alternate ? FORMAT_FLAG_ALTERNATE : 0) |
                                     (zero ? FORMAT_FLAG_ZERO : 0);
                        spec.width = width;
                        spec.precision = precision;
                        spec.conversion = conversion;

                        format_decimal_field(address_of sink,
                                             printf_decimal(printf_next()),
                                             address_of spec);
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
        string_address into = null;
        positive first = 1;

        // -v names a variable to fill instead of a descriptor to write down,
        // which is how a script formats a value without a subshell.
        if (shell_argc > 1 && word_is(shell_argv[1], "-v"))
        {
                if (shell_argc < 3)
                {
                        shell_diagnostic("printf: -v: option requires an "
                                         "argument\n", 0);
                        return shell_answer(2);
                }

                into = shell_argv[2];
                first = 3;
        }

        if (shell_argc <= first)
                return shell_answer(2);

        format = shell_argv[first];
        printf_argument = first + 1;
        printf_cut = false;
        printf_status = 0;

        if (into)
        {
                printf_kept_used = 0;
                write = printf_keeper;
        }

        while (1)
        {
                printf_took = false;
                printf_one(write, format);

                // A format with no conversion in it would otherwise run for as
                // long as there were arguments left.
                if (printf_cut || printf_argument >= shell_argc || !printf_took)
                        break;
        }

        if (into)
        {
                if (!shell_room((address_any address_to)address_of printf_kept,
                                address_of printf_kept_room,
                                printf_kept_used + 1, 1))
                {
                        shell_diagnostic("printf: no room\n", 0);
                        return shell_answer(2);
                }

                printf_kept[printf_kept_used] = end;

                if (!env_assign(into, printf_kept))
                {
                        string_format(shell_diagnostic,
                                      "printf: %s: cannot assign\n", into);
                        return shell_answer(1);
                }
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

// A tenth of a second at a time is close enough for a timeout measured in
// whole seconds, and it keeps the wait in one place.
/*
        The moment read -t gives up, and whether a byte arrived before it.

        The timeout is on the whole line, not on each byte of it: measured
        again from every byte, a writer sending one byte every N seconds
        kept a read -t N waiting forever. So the deadline is fixed once, on
        the monotonic clock, and every wait is for what is left of it.
*/
#define READ_CLOCK_MONOTONIC 1

static fn read_deadline(bipolar tenths, timespec address_to deadline)
{
        system_call_2(syscall(clock_gettime), READ_CLOCK_MONOTONIC,
                      (positive)deadline);

        deadline->tv_sec += (p64)(tenths / 10);
        deadline->tv_nsec += (p64)(tenths % 10) * 100000000;

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

/*
        read -s: the bytes arrive and the terminal does not show them.

        Only a terminal has an echo to turn off. On a pipe the first call
        fails and there is nothing to put back, which is why this is a pair of
        calls and a flag rather than a mode the shell has to remember -- and
        why a test that reads down a pipe cannot see whether it works.
*/
static bool read_echo_off(b32 descriptor, edit_terminal_modes address_to held)
{
        edit_terminal_modes quiet;

        if (system_control(descriptor, EDIT_TCGETS, held) != 0)
                return false;

        quiet = address_to held;
        quiet.behaviour &= ~EDIT_LOCAL_ECHO;

        return system_control(descriptor, EDIT_TCSETS, address_of quiet) == 0;
}

static fn read_echo_back(b32 descriptor, edit_terminal_modes address_to held)
{
        system_control(descriptor, EDIT_TCSETS, held);
}

// The fields of one read line, when they are going into an array rather than
// into a name each. The vector is the builtin's own and is reused.
static string_address address_to read_words;
static positive read_words_room;

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
        bipolar tenths = -1;
        timespec deadline;
        p8 stop_at = '\n';
        bool exact = false;
        bool hidden = false;
        bool quieted = false;
        edit_terminal_modes quiet_held;
        b32 descriptor = 0;
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

                        // -s turns the terminal's echo off and -e asks
                        // for line editing; neither takes a value, and on a
                        // pipe neither has anything to do.
                        if (which == 'r' || which == 's' || which == 'e')
                        {
                                raw = raw || which == 'r';
                                hidden = hidden || which == 's';
                                letter++;
                                continue;
                        }

                        if (which != 'p' && which != 'n' && which != 'N' &&
                            which != 'd' && which != 't' && which != 'a' &&
                            which != 'u' && which != 'i')
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

                        if (which == 'a')
                        {
                                if (!shell_valid_name(value,
                                                      string_length(value)))
                                {
                                        string_format(
                                            shell_diagnostic,
                                            "read: bad variable name: %s\n",
                                            value);
                                        return shell_answer(2);
                                }

                                array_name = value;
                        }
                        else if (which == 'p')
                                shell_diagnostic(value, 0);
                        else if (which == 'i')
                        {
                                // The editor would put it in front of what is
                                // typed. Nothing is typed down a pipe, so the
                                // option is taken and its value is not.
                        }
                        else if (which == 'u')
                        {
                                bool good;
                                bipolar asked = shell_signed(value, address_of good);

                                if (!good || asked < 0)
                                {
                                        string_format(shell_diagnostic,
                                                      "read: bad descriptor: %s\n",
                                                      value);
                                        return shell_answer(1);
                                }

                                descriptor = (b32)asked;
                        }
                        else if (which == 'n' || which == 'N')
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
                                exact = which == 'N';
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

        /*
                -t 0 asks whether a line could be read, not for one.

                Zero seconds is not a timeout a read can meet: Bash answers
                for whether the descriptor is ready and reads nothing, which
                is the only way a script can poll without consuming.
        */
        if (tenths == 0)
        {
                timespec none = {0, 0};

                return shell_answer(
                    descriptor_wait_readable(descriptor, address_of none, null) > 0
                        ? 0
                        : 1);
        }

        if (tenths > 0)
                read_deadline(tenths, address_of deadline);

        if (hidden)
                quieted = read_echo_off(descriptor, address_of quiet_held);

        while (!(limited && read_length >= limit))
        {
                p8 value;

                if (read_length == positive_max || !read_reserve(read_length + 2))
                {
                        if (quieted)
                                read_echo_back(descriptor,
                                               address_of quiet_held);

                        shell_diagnostic("read: no room\n", 0);
                        return shell_answer(2);
                }

                if (tenths > 0 && !read_waited(descriptor, address_of deadline))
                {
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

                // -N counts bytes and nothing else: neither the delimiter
                // nor a backslash ends or joins anything.
                if (!exact && value == stop_at)
                        break;

                if (!exact && !raw && value == '\\')
                {
                        p8 next;

                        got = system_read_once(descriptor, address_of next, 1);

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

        // Put the terminal back before any name is assigned: every path out
        // of here from now on is a return.
        if (quieted)
        {
                read_echo_back(descriptor, address_of quiet_held);

                // The newline the terminal did not show, so the next prompt
                // does not land on the same line as what was typed.
                shell_diagnostic("\n", 1);
        }

        if (!array_name && names >= shell_argc)
        {
                if (!read_set("REPLY", read_line))
                        return shell_answer(2);

                return shell_answer(failed ? 2 : ended ? 1 : 0);
        }

        /*
                -N hands the bytes over whole.

                What it read is a count of bytes and not a line, so there are
                no fields in it to cut: the first name takes all of it and the
                rest are cleared, which is what Bash does.
        */
        if (exact)
        {
                if (!array_name && !read_set(shell_argv[names], read_line))
                        return shell_answer(2);

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
                                if (!read_set(shell_argv[name], ""))
                                        return shell_answer(2);

                return shell_answer(failed ? 2
                                           : (ended && read_length < limit) ? 1
                                                                            : 0);
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

        /*
                read -a takes the whole line as fields of one array, so the
                names loop below never runs. The cutting is the same cutting:
                a blank run is one boundary and a delimiter that is not blank
                is a boundary of its own.
        */
        if (array_name)
        {
                positive count = 0;

                while (at < read_length)
                {
                        positive begin;

                        while (at < read_length && read_blank(ifs, at))
                                at++;

                        if (at >= read_length)
                                break;

                        begin = at;

                        while (at < read_length && !read_separates(ifs, at))
                                at++;

                        if (!shell_array_room(read_words, read_words_room,
                                              count + 1))
                        {
                                shell_diagnostic("read: no room\n", 0);
                                return shell_answer(2);
                        }

                        read_words[count++] = read_line + begin;

                        if (at < read_length)
                        {
                                positive after = at;

                                while (after < read_length &&
                                       read_blank(ifs, after))
                                        after++;

                                if (after < read_length &&
                                    read_separates(ifs, after))
                                {
                                        after++;

                                        while (after < read_length &&
                                               read_blank(ifs, after))
                                                after++;
                                }

                                read_line[at] = end;
                                at = after;
                        }
                }

                if (!shell_array_words(array_name, string_length(array_name),
                                       read_words, count))
                {
                        shell_diagnostic("read: no room\n", 0);
                        return shell_answer(2);
                }

                return shell_answer(failed ? 2 : ended ? 1 : 0);
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
        mapfile, and readarray which is the same command under its other name.

        A whole input read into one array, one element per delimited record.
        Every option is about which records and where they land: -s skips
        some, -n stops after some, -O says which subscript the first one
        takes, -u says which descriptor to read, -d changes what ends a
        record and -t leaves that byte off the element.

        The input is read whole before any element is made, because a record
        is only a record once its delimiter has been seen and the array is
        replaced rather than appended to.
*/
static p8 address_to mapfile_text;
static positive mapfile_room;

COLD fn shell_mapfile(writer write, string_address input)
{
        string_address name = (string_address) "MAPFILE";
        positive index = 1;
        positive used = 0;
        positive at = 0;
        positive seen = 0;
        positive count = 0;
        positive wanted = 0;
        positive skip = 0;
        positive origin = 0;
        positive name_length;
        p8 delimiter = '\n';
        p8 written[32];
        bool trim = false;
        b32 from = 0;

        (void)input;

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
                        bool good;
                        bipolar asked;

                        if (which == 't')
                        {
                                trim = true;
                                letter++;
                                continue;
                        }

                        if (which != 'n' && which != 's' && which != 'O' &&
                            which != 'd' && which != 'u')
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "%s: bad option: -%s\n",
                                              shell_argv[0], said);
                                return shell_answer(2);
                        }

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
                                p8 said[2] = {which, end};

                                string_format(
                                    shell_diagnostic,
                                    "%s: option -%s wants a value\n",
                                    shell_argv[0], said);
                                return shell_answer(2);
                        }

                        if (which == 'd')
                        {
                                delimiter = string_get(value);
                                continue;
                        }

                        asked = shell_signed(value, address_of good);

                        if (!good || asked < 0)
                        {
                                string_format(shell_diagnostic,
                                              "%s: %s: bad number\n",
                                              shell_argv[0], value);
                                return shell_answer(2);
                        }

                        if (which == 'n')
                                wanted = (positive)asked;
                        else if (which == 's')
                                skip = (positive)asked;
                        else if (which == 'O')
                                origin = (positive)asked;
                        else
                                from = (b32)asked;
                }

                index++;
        }

        if (index < shell_argc)
                name = shell_argv[index++];

        name_length = string_length(name);

        if (index < shell_argc || !shell_valid_name(name, name_length))
        {
                string_format(shell_diagnostic, "%s: %s: bad array name\n",
                              shell_argv[0], name);
                return shell_answer(2);
        }

        while (true)
        {
                bipolar got;

                if (used > positive_max - 4098 ||
                    !shell_room((address_any address_to)address_of mapfile_text,
                                address_of mapfile_room, used + 4097, 1))
                {
                        shell_diagnostic("mapfile: no room\n", 0);
                        return shell_answer(2);
                }

                got = system_read_once(from, mapfile_text + used, 4096);

                if (got <= 0)
                        break;

                used += (positive)got;
        }

        mapfile_text[used] = end;

        // -O adds to what is there rather than replacing it, which is the
        // one way this command does not start from an empty array.
        if (!shell_variable_attribute_set(name, name_length,
                                          SHELL_ARRAY_INDEXED |
                                              SHELL_ARRAY_ASSIGNED,
                                          SHELL_ARRAY_ASSOCIATIVE) ||
            (!origin && !shell_array_clear(name, name_length)))
        {
                shell_diagnostic("mapfile: no room\n", 0);
                return shell_answer(2);
        }

        while (at < used)
        {
                positive begin = at;
                positive stop;
                p8 held;

                while (at < used && mapfile_text[at] != delimiter)
                        at++;

                stop = at;

                if (at < used)
                        at++;

                // Without -t the delimiter is part of the record, so the
                // terminator goes after it -- over the first byte of the
                // next record, which is put back before that one is read.
                if (!trim)
                        stop = at;

                if (seen++ < skip)
                        continue;

                if (wanted && count >= wanted)
                        break;

                held = mapfile_text[stop];
                mapfile_text[stop] = end;

                if (!shell_array_set(name, name_length, written,
                                     bipolar_into_string(
                                         written, (bipolar)(origin + count)),
                                     mapfile_text + begin, false))
                {
                        mapfile_text[stop] = held;
                        shell_diagnostic("mapfile: no room\n", 0);
                        return shell_answer(2);
                }

                mapfile_text[stop] = held;
                count++;
        }

        shell_answer(0);
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

                        // OPTERR=0 asks for the answer without the
                        // complaint, which is what a script that prints its
                        // own usage sets before the loop.
                        if (getopts_complains())
                                string_format(shell_diagnostic,
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
                                    shell_diagnostic,
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

        The symbolic form is chmod's grammar read against what a file is
        allowed, and the mask is the other way round -- "u=rwx" says the
        owner keeps everything, which is nothing masked off. So the allowance
        goes through the reader chmod uses, X and the copied classes
        included, and is inverted on the way out. The reference shell departs
        from chmod twice: it does not know t, which no mask could hold
        anyway, and a copied class is what the mask allowed before the
        command rather than what the clauses so far have made.
*/
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

COLD fn shell_times(writer write, string_address input)
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
bipolar trap_number(string_address word)
{
        // Linux's own second spelling of 29; the reference shell and kill
        // both say IO and both take POLL as well.
        static const named_byte aliases[] = {{"POLL", 29}};
        p8 upper[16];
        positive length;
        positive index = 0;
        bool good;
        bipolar value;

        if (!word)
                return -1;

        length = string_length_max(word, sizeof(upper));

        if (length < sizeof(upper))
        {
                memory_copy_apart(upper, word, length + 1);
                memory_to_upper_ascii(upper, length);

                string_address name = upper;

                if (string_is(name, 'S') && string_is(name + 1, 'I') &&
                    string_is(name + 2, 'G'))
                        name += 3;

                index = string_table_find(name, trap_names,
                                          sizeof(trap_names[0]), TRAP_NAMES);

                if (index < TRAP_NAMES)
                        return (bipolar)index;

                index = string_table_find(name, trap_condition_names,
                                          sizeof(trap_condition_names[0]), 3);

                if (index < 3)
                        return (bipolar)(TRAP_ERR + index);

                for (index = 0; index < array_count(aliases); index++)
                        if (string_equals(name, aliases[index].name))
                                return aliases[index].value;
        }

        value = shell_signed(word, address_of good);

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
        else if (number >= TRAP_ERR && number <= TRAP_DEBUG)
                string_format(write, "%s", trap_condition_names[number -
                                                                TRAP_ERR]);
        else
                positive_to_string(write, number);

        write("\n", 1);
}

static bool trap_unsigned(string_address word)
{
        positive value;

        return string_digits_exact(word, address_of value);
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
                else if (number <= 49)
                {
                        // The real-time signals are named by their distance
                        // from each end, which is how every tool that prints
                        // them writes them down.
                        write("RTMIN", 5);

                        if (number > 34)
                        {
                                write("+", 1);
                                positive_to_string(write, number - 34);
                        }
                }
                else
                {
                        write("RTMAX", 5);

                        if (number < TRAP_NUMBER_MAX)
                        {
                                write("-", 1);
                                positive_to_string(write,
                                                   TRAP_NUMBER_MAX - number);
                        }
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

        if (index < shell_argc && word_is(shell_argv[index], "-l"))
        {
                trap_listed(write);

                return shell_answer(0);
        }

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

                        for (positive number = TRAP_ERR;
                             number <= TRAP_DEBUG; number++)
                        {
                                string_address recorded = trap_action(number);

                                if (recorded)
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
                                        string_format(shell_diagnostic,
                                                      "trap: invalid signal: %s\n",
                                                      shell_argv[index - 1]);
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
                                if (!recorded && number > TRAP_NUMBER_MAX)
                                        continue;

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

                for (positive number = TRAP_ERR; number <= TRAP_DEBUG;
                     number++)
                {
                        string_address recorded = trap_action(number);

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

                if (number < 0 || number > TRAP_CONDITION_MAX)
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

COLD fn shell_alias(writer write, string_address input)
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

COLD fn shell_unalias(writer write, string_address input)
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
COLD fn shell_eval(writer write, string_address input)
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

                // Every line of it, not the first: eval "$(cmd)" is the
                // idiom, and what cmd printed has as many lines as it likes.
                run_lines(eval_storage);
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

static shell_tool shell_tools[] = {
#ifdef SHELL_NO_UTILITIES
#define SHELL_TOOL_GENERAL(name, function)
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#define SHELL_TOOL_MONITOR(name, function)
#else
#define SHELL_TOOL_GENERAL(name, function) {#name, function},
#ifdef SHELL_NO_UTIL_LINUX
#define SHELL_TOOL_UTIL_BIN(name, function)
#define SHELL_TOOL_UTIL_SBIN(name, function)
#else
#define SHELL_TOOL_UTIL_BIN(name, function) {#name, function},
#define SHELL_TOOL_UTIL_SBIN(name, function) {#name, function},
#endif
#ifdef SHELL_NO_MONITOR
#define SHELL_TOOL_MONITOR(name, function)
#else
#define SHELL_TOOL_MONITOR(name, function) {#name, function},
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
#undef SHELL_TOOL_MONITOR
#undef SHELL_TOOL_GENERAL
    {null, null},
};

#define SHELL_TOOLS (array_count(shell_tools) - 1)
#define SHELL_TOOL_INDEX_ROOM 128

static shell_name_slot shell_tool_index[SHELL_TOOL_INDEX_ROOM];
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

        if (shell_tail_command)
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
fn history_leaving();

fn shell_trap_exit()
{
        positive action_room = 0;
        string_address action = trap_detach(0, address_of action_room);
        b32 leaving = shell_status;

        // Every way out of an interactive shell comes through here, which is
        // the only place a history file can be written once rather than at
        // each of them.
        history_leaving();

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
        {
                bipolar handle;

                do
                        handle = system_open_at(AT_FDCWD,
                                               name, FILE_READ);
                while (handle == -4);

                return handle;
        }

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

                do
                        handle = system_open_at(AT_FDCWD,
                                               *found, FILE_READ);
                while (handle == -4);

                if (handle >= 0)
                        return handle;
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
                        system_close(handle);
                        return got;
                }

                if (!got)
                        break;

                used += (positive)got;
        }

        system_close(handle);

        if (*no_room)
                return -1;

        (*text)[used] = end;
        return (bipolar)used;
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
        positive at = 0;
        bool no_room = false;
        bipolar handle;

        if (shell_argc < 2)
                return shell_answer(2);

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

                /*
                        Bash's spelling is Bash's answer: `source` on a file
                        that is not there leaves 1 behind and the script goes
                        on. The dot is POSIX's, and POSIX makes it a special
                        builtin whose failure ends the script.
                */
                if (word_is(shell_argv[0], "source"))
                        return shell_answer(1);

                // Two, as the reference shell answers: the failure is the
                // special builtin's own and not the file's.
                shell_answer(2);

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

        if (shell_argc > 2)
        {
                held = shell_parameters_save();

                if (held == EXPAND_NO_ROOM ||
                    !shell_parameters_restore_prepare(held_count) ||
                    !shell_parameters_set(shell_argv + 2, shell_argc - 2))
                {
                        if (held != EXPAND_NO_ROOM)
                                shell_parameter_stack_used = held;

                        memory_free(source_text, source_room);
                        string_format(shell_diagnostic,
                                      "source: no room for arguments\n");

                        return shell_answer(2);
                }
        }

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

        if (held != EXPAND_NO_ROOM &&
            !shell_parameters_restore(held, held_count))
        {
                string_format(shell_diagnostic,
                              "source: no room to restore arguments\n");
                log_flush();
                exit(2);
        }

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

COLD fn shell_jobs(writer write, string_address input);
fn shell_history(writer write, string_address input);
fn shell_fc(writer write, string_address input);
fn shell_fg(writer write, string_address input);
fn shell_bg(writer write, string_address input);
fn shell_disown(writer write, string_address input);
fn shell_suspend(writer write, string_address input);
fn shell_kill(writer write, string_address input);
fn job_wait(writer write, string_address input);
fn shell_help(writer write, string_address input);
COLD fn shell_bind(writer write, string_address input);
COLD fn shell_builtin_run(writer write, string_address input);
COLD fn shell_compgen(writer write, string_address input);
COLD fn shell_complete(writer write, string_address input);
COLD fn shell_compopt(writer write, string_address input);
COLD fn shell_enable(writer write, string_address input);
COLD fn shell_which(writer write, string_address input);
fn shell_type(writer write, string_address input);
COLD fn shell_command_builtin(writer write, string_address input);
COLD fn shell_hash(writer write, string_address input);
COLD fn shell_ulimit(writer write, string_address input);
bool exec_control_builtin(string_address name, bool run);

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
    {"bg", shell_bg},
    {"bind", shell_bind},
    {"blkid", shell_blkid},
    {"builtin", shell_builtin_run},
    {"compgen", shell_compgen},
    {"complete", shell_complete},
    {"compopt", shell_compopt},
    {"cd", shell_cd},
    {"clear", shell_clear},
    {"command", shell_command_builtin},
    {"declare", shell_declare},
    {"disown", shell_disown},
    {"dirs", shell_dirs},
    {"echo", shell_echo},
    {"enable", shell_enable},
    {"eval", shell_eval},
    {"exec", shell_exec},
    {"exit", shell_exit},
    {"false", shell_false},
    {"fc", shell_fc},
    {"fg", shell_fg},
    {"findfs", shell_findfs},
    {"findmnt", shell_findmnt},
    {"getopts", shell_getopts},
    {"hash", shell_hash},
    {"history", shell_history},
    {"jobs", shell_jobs},
    {"kill", shell_kill},
    {"let", shell_let},
    {"mount", shell_mount},
    {"mountpoint", shell_mountpoint},
    {"popd", shell_popd},
    {"poweroff", shell_poweroff},
    {"pushd", shell_pushd},
    {"printf", shell_printf},
    {"pwd", shell_pwd},
    {"read", shell_read},
    {"mapfile", shell_mapfile},
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
    {"type", shell_type},
    {"typeset", shell_declare},
    {"true", shell_true},
    {"ulimit", shell_ulimit},
    {"umask", shell_umask},
    {"umount", shell_umount},
    {"unalias", shell_unalias},
    {"unset", shell_unset},
    {"wait", job_wait},
    {"which", shell_which},
    {"help", shell_help},
    {"local", shell_local},
    {"export", shell_export},
    {null, null},
};

#define SHELL_COMMAND_COUNT ((array_count(shell_commands)) - 1)
#define SHELL_COMMAND_INDEX_ROOM 128

static shell_name_slot shell_command_index[SHELL_COMMAND_INDEX_ROOM];
static bool shell_command_index_ready;

/*
        The builtins a script has switched off.

        `enable -n echo` makes the shell forget it has one, so that the file
        on PATH is what runs. Kept as a short list of names and asked about
        only when the list is not empty, which is what keeps the ordinary
        dispatch at one comparison against zero.
*/
#define SHELL_DISABLED_MAX 32
#define SHELL_DISABLED_BYTES 512

static string_address shell_disabled[SHELL_DISABLED_MAX];
static p8 shell_disabled_pool[SHELL_DISABLED_BYTES];
static positive shell_disabled_count;
static positive shell_disabled_used;

static inline INLINE PURE bool shell_builtin_disabled(string_address name)
{
        return shell_disabled_count &&
               string_table_find(name, shell_disabled,
                                 sizeof(shell_disabled[0]),
                                 shell_disabled_count) < shell_disabled_count;
}

static shell_command address_to shell_command_named_hashed(string_address name,
                                                           positive2 named)
{
        positive which = shell_name_index_find(
            name, shell_commands, sizeof(shell_commands[0]),
            SHELL_COMMAND_COUNT, shell_command_index,
            SHELL_COMMAND_INDEX_ROOM, address_of shell_command_index_ready,
            named);

        if (which >= SHELL_COMMAND_COUNT)
                return null;

        // A name switched off is a name the shell does not have, for
        // dispatch, for type and for command alike.
        return shell_builtin_disabled(name) ? null : shell_commands + which;
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
        positive index = 1;
        b32 bad = 0;
        bool as_commands = false;
        bool only_path = false;
        bool forget = false;
        bool named_many = false;
        string_address given = null;
        p8 address_to found = null;
        positive found_room = 0;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

                        if (which == 'r')
                                hash_forget();
                        else if (which == 'l')
                                as_commands = true;
                        else if (which == 't')
                                only_path = true;
                        else if (which == 'd')
                                forget = true;
                        else if (which == 'p')
                        {
                                if (string_get(letter))
                                        given = letter;
                                else if (index + 1 < shell_argc)
                                        given = shell_argv[++index];
                                else
                                {
                                        shell_diagnostic("hash: -p: option "
                                                         "requires an "
                                                         "argument\n", 0);
                                        return shell_answer(2);
                                }

                                letter = (string_address) "";
                        }
                        else
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "hash: -%s: invalid option\n",
                                              said);
                                return shell_answer(2);
                        }
                }

                index++;
        }

        if (index >= shell_argc)
        {
                positive at = 0;

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
                                string_format(shell_diagnostic,
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
                        string_format(shell_diagnostic, "hash: no room\n");
                        break;
                }

                if (located != 1)
                {
                        bad = 1;
                        string_format(shell_diagnostic, "hash: %s: not found\n",
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
                string_address known = use_hash ? hash_find(name) : null;

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
                if (use_hash && access == ACCESS_EXECUTE)
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

        return 0;
}

/*
        The grammar words.

        They are not in the builtin table -- the parser knows them and nothing
        looks them up -- but `type` and `command -V` have to name them, so the
        list is written down once here rather than rebuilt from the parser's
        own tests.
*/
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

/*
        type: what a name would run.

        In the order the shell would actually try them, which is the only
        useful answer -- a grep on the path is not the grep that runs. -t
        names the kind in one word, -a says every place a name is, -p and -P
        want the file alone, and -f looks past the functions.
*/
COLD fn shell_type(writer write, string_address input)
{
        b32 index = 1;
        b32 bad = 0;
        bool terse = false;
        bool every = false;
        bool path_only = false;
        bool force_path = false;
        bool no_functions = false;
        p8 address_to found = null;
        positive found_room = 0;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

                        if (which == 't')
                                terse = true;
                        else if (which == 'a')
                                every = true;
                        else if (which == 'p')
                                path_only = true;
                        else if (which == 'P')
                        {
                                path_only = true;
                                force_path = true;
                        }
                        else if (which == 'f')
                                no_functions = true;
                        else
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "type: -%s: invalid option\n",
                                              said);
                                return shell_answer(2);
                        }
                }

                index++;
        }

        if (index >= shell_argc)
                return shell_answer(0);

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                positive2 named = string_hash_33_length(name);
                bool alias = shell_alias_visible(name);
                bool keyword = shell_keyword_here(name);
                bool function = !no_functions &&
                                exec_function_here_hashed(name, named);
                bool builtin = shell_command_builtin_here(name, named);
                bool any = false;
                bipolar located;

                // -p wants a file and nothing else; only -P looks for one
                // behind a name the shell would answer itself.
                if (path_only && !force_path &&
                    (alias || keyword || function || builtin))
                        continue;

                if (!path_only)
                {
                        if (alias)
                        {
                                if (terse)
                                        string_format(write, "alias\n");
                                else
                                        string_format(
                                            write, "%s is aliased to `%s'\n",
                                            name, alias_lookup(name));

                                any = true;

                                if (!every)
                                        continue;
                        }

                        if (keyword)
                        {
                                string_format(write,
                                              terse ? "keyword\n"
                                                    : "%s is a shell keyword\n",
                                              name);
                                any = true;

                                if (!every)
                                        continue;
                        }

                        if (function)
                        {
                                string_format(write,
                                              terse ? "function\n"
                                                    : "%s is a shell function\n",
                                              name);
                                any = true;

                                if (!every)
                                        continue;
                        }

                        if (builtin)
                        {
                                string_format(write,
                                              terse ? "builtin\n"
                                                    : "%s is a shell builtin\n",
                                              name);
                                any = true;

                                if (!every)
                                        continue;
                        }
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
                        if (terse)
                                string_format(write, "file\n");
                        else if (path_only)
                                string_format(write, "%s\n", found);
                        else
                                string_format(write, "%s is %s\n", name, found);

                        any = true;
                        continue;
                }

                if (any)
                        continue;

                // -t, -f and the two path forms say nothing about a name
                // they have no answer for; the plain form says so out loud.
                // The three that stay quiet are Bash's own and answer as Bash
                // does, which is one and not the reference shell's hundred
                // and twenty-seven.
                if (!terse && !path_only && !no_functions)
                        string_format(write, "%s: not found\n", name);

                bad = terse || path_only || no_functions ? 1 : 127;
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
                        positive2 named = string_hash_33_length(name);
                        bipolar located;

                        // Before the builtins, because a grammar word is what
                        // the parser sees first and `command -V if` has to
                        // say so rather than call it missing.
                        if (shell_keyword_here(name))
                        {
                                string_format(write,
                                              at_length
                                                ? "%s is a shell keyword\n"
                                                : "%s\n",
                                              name);
                                any = true;
                                continue;
                        }

                        if (shell_alias_visible(name))
                        {
                                if (at_length)
                                        string_format(
                                            write, "%s is aliased to `%s'\n",
                                            name, alias_lookup(name));
                                else
                                        string_format(write, "alias %s='%s'\n",
                                                      name,
                                                      alias_lookup(name));

                                any = true;
                                continue;
                        }

                        if (exec_function_here_hashed(name, named))
                        {
                                string_format(write,
                                              at_length
                                                ? "%s is a shell function\n"
                                                : "%s\n",
                                              name);
                                any = true;
                                continue;
                        }

                        if (shell_command_builtin_here(name, named))
                        {
                                string_format(write,
                                              at_length
                                                ? "%s is a shell builtin\n"
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

                if (shell_builtin(shell_arguments(),
                                  string_hash_33_length(shell_argv[0])))
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
        if (shell_command_builtin_here(input,
                                       string_hash_33_length(input)))
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

static shell_limit shell_bash_limits[] = {
    {"nice", 'e', 13, 1},
    {"sigpending", 'i', 11, 1},
    {"msgqueue(bytes)", 'q', 12, 1},
    {"processes", 'u', 6, 1},
    {"locks", 'x', 10, 1},
    {"rttime", 'R', 15, 1},
    {"kqueues", 'k', SHELL_LIMIT_NONE, 1},
    {"pipesize", 'P', SHELL_LIMIT_NONE, 1},
    {"threads", 'T', SHELL_LIMIT_NONE, 1},
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
                                limit = shell_bash_limits;

                                while (limit->name && limit->letter != which)
                                        limit++;
                        }

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

/*
        builtin: run the builtin behind a name, whatever else has that name.

        A function that wraps a builtin needs a way to reach the thing it
        wraps, and `command` is not it -- command would find the function
        again through PATH if the name happened to be a program too.
*/
fn shell_builtin_run(writer write, string_address input)
{
        bool tail = shell_tail_command;

        (void)write;
        (void)input;

        if (shell_argc < 2)
                return shell_answer(0);

        memory_copy(shell_argv, shell_argv + 1,
                    (positive)shell_argc * sizeof(shell_argv[0]));
        shell_argc--;

        if (exec_control_builtin(shell_argv[0], true))
                return;

        if (shell_builtin(shell_arguments(),
                          string_hash_33_length(shell_argv[0])))
        {
                shell_tail_command = tail;
                return;
        }

        shell_tail_command = tail;
        string_format(shell_diagnostic, "builtin: %s: not a shell builtin\n",
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
        positive index = 1;
        bool off = false;
        bool as_commands = false;
        bool every = false;
        b32 bad = 0;

        while (index < shell_argc && string_is(shell_argv[index], '-') &&
               string_get(shell_argv[index] + 1))
        {
                string_address letter = shell_argv[index] + 1;

                if (word_is(shell_argv[index], "--"))
                {
                        index++;
                        break;
                }

                while (string_get(letter))
                {
                        p8 which = string_get(letter++);

                        if (which == 'n')
                                off = true;
                        else if (which == 'p')
                                as_commands = true;
                        else if (which == 'a')
                                every = true;
                        else if (which == 'f' || which == 'd' || which == 's')
                        {
                                // Loading a builtin out of a shared object is
                                // a thing this shell cannot do and will not
                                // pretend to.
                                string_format(shell_diagnostic,
                                              "enable: not supported\n");
                                return shell_answer(2);
                        }
                        else
                        {
                                p8 said[2] = {which, end};

                                string_format(shell_diagnostic,
                                              "enable: -%s: invalid option\n",
                                              said);
                                return shell_answer(2);
                        }
                }

                index++;
        }

        if (index >= shell_argc)
        {
                shell_command address_to command = shell_commands;

                while (command->name)
                {
                        bool here = !shell_builtin_disabled(command->name);

                        if (here != !off || every)
                                string_format(write, "enable %s%s\n",
                                              here ? "" : "-n ",
                                              command->name);

                        command++;
                }

                (void)as_commands;

                return shell_answer(0);
        }

        while (index < shell_argc)
        {
                string_address name = shell_argv[index++];
                positive length = string_length(name);
                positive at;

                if (!shell_command_named_hashed(name,
                                                string_hash_33_length(name)) &&
                    !shell_builtin_disabled(name))
                {
                        string_format(shell_diagnostic,
                                      "enable: %s: not a shell builtin\n",
                                      name);
                        bad = 1;
                        continue;
                }

                at = string_table_find(name, shell_disabled,
                                       sizeof(shell_disabled[0]),
                                       shell_disabled_count);

                if (!off)
                {
                        if (at < shell_disabled_count)
                        {
                                memory_copy(shell_disabled + at,
                                            shell_disabled + at + 1,
                                            (shell_disabled_count - at - 1) *
                                                sizeof(shell_disabled[0]));
                                shell_disabled_count--;
                        }

                        continue;
                }

                if (at < shell_disabled_count)
                        continue;

                if (shell_disabled_count >= SHELL_DISABLED_MAX ||
                    shell_disabled_used + length + 1 > SHELL_DISABLED_BYTES)
                {
                        shell_diagnostic("enable: too many\n", 0);
                        bad = 1;
                        continue;
                }

                shell_disabled[shell_disabled_count++] =
                    shell_disabled_pool + shell_disabled_used;
                memory_copy(shell_disabled_pool + shell_disabled_used, name,
                            length + 1);
                shell_disabled_used += length + 1;
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

static COLD fn compgen_offer(writer write, string_address name)
{
        positive length = string_length(name);

        if (compgen_prefix_length &&
            (length < compgen_prefix_length ||
             memory_compare(name, compgen_prefix, compgen_prefix_length)))
                return;

        write(name, length);
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
        string_address words = null;

        compgen_prefix = null;
        compgen_prefix_length = 0;
        compgen_shown = 0;

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
                        if (word_is(value, "function"))
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

                index++;
        }

        if (index < shell_argc)
        {
                compgen_prefix = shell_argv[index];
                compgen_prefix_length = string_length(compgen_prefix);
        }

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

        if (functions || commands)
        {
                p8 held[256];
                positive at = 0;

                while (exec_function_named(at++, held, sizeof(held)))
                        compgen_offer(write, held);
        }

        if (aliases || commands)
                for (positive at = 0; at < alias_count; at++)
                        compgen_offer(write, alias_table[at].name);

        if (builtins || commands)
        {
                shell_command address_to command = shell_commands;

                while (command->name)
                        compgen_offer(write, (command++)->name);
        }

        if (variables)
                shell_names_sorted(write, 0, compgen_variable);

        if (files || directories)
        {
                p8 block[2048];
                bipolar directory = system_open_at(AT_FDCWD,
                                                   (string_address) ".",
                                                   FILE_READ | O_DIRECTORY);

                while (directory >= 0)
                {
                        bipolar got = system_read_directory(directory, block,
                                                            sizeof(block));
                        p8 address_to step = block;

                        if (got <= 0)
                                break;

                        while (step < block + got)
                        {
                                struct linux_dirent64 address_to entry =
                                    (struct linux_dirent64 address_to)step;

                                step += entry->d_reclen;

                                if (entry->d_name[0] == '.')
                                        continue;

                                if (directories && !files &&
                                    entry->d_type != 4)
                                        continue;

                                compgen_offer(write,
                                              (string_address)entry->d_name);
                        }
                }

                if (directory >= 0)
                        system_close(directory);
        }

        shell_answer(compgen_shown ? 0 : 1);
}

/*
        complete, compopt and bind: taken, and doing nothing.

        Programmable completion needs a terminal and a reader that offers it,
        and this shell's line editor has neither. A profile that sets a
        hundred completions must still get to its last line, so the names are
        here and answer the way Bash answers a shell with no completion loaded.
*/
fn shell_complete(writer write, string_address input)
{
        (void)write;
        (void)input;

        shell_answer(0);
}

fn shell_compopt(writer write, string_address input)
{
        (void)write;
        (void)input;

        // No completion is being executed, which is the one thing compopt
        // needs and the reason Bash answers one here too.
        shell_answer(1);
}

fn shell_bind(writer write, string_address input)
{
        (void)write;
        (void)input;

        shell_answer(0);
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
