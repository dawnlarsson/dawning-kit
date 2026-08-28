/*
        The shell's executor.

        One function per node kind, walking the tree the parser built. Nothing
        here re-reads the line and nothing here decides what a word means: the
        shape is settled and the expander is called on the words as they are
        needed, which is why a for loop expands its list once and its body
        every time round.

        break, continue and return are not returns from C. A break in the body
        of a loop is three or four calls below the loop it means, and every one
        of those frames has to unwind without running what came after it, so
        the state travels in exec_signal and every construct that can contain a
        command checks it before going on.
*/

#define EXEC_SIGNAL_NONE 0
#define EXEC_SIGNAL_BREAK 1
#define EXEC_SIGNAL_CONTINUE 2
#define EXEC_SIGNAL_RETURN 3

static b32 exec_signal;
static b32 exec_signal_level;
static b32 exec_loop_depth;
static b32 exec_function_depth;

/*
        set -e, and the places it does not reach.

        A command that fails ends the shell, unless somebody was going to look
        at the failure anyway: the condition of an if or a loop, everything but
        the last of an && or || list, and a pipeline whose status is inverted.
        POSIX names those three and nothing else, so the flag travels down the
        tree rather than being asked about at each node.

        exec_forked is what a child of a subshell or a pipeline sets. Leaving
        that way is not the shell leaving, and the exit trap belongs to the
        shell.
*/
#define SHELL_ERREXIT ((positive)1 << ('e' - 'a'))

static bool exec_tested;
static bool exec_forked;

fn shell_trap_exit();

fn exec_child_began()
{
        exec_forked = true;
}

static fn exec_errexit(b32 status)
{
        if (!status || exec_tested || !(shell_options & SHELL_ERREXIT))
                return;

        shell_status = status;

        if (!exec_forked)
                shell_trap_exit();

        log_flush();
        exit(status);
}

#define EXEC_ARENA 8192
#define PIPELINE_MAX 16
#define FUNCTION_MAX 64
#define FUNCTION_NAME 64
#define REDIRECT_SAVE_MAX 24
#define FUNCTION_DEPTH_MAX 64

static p8 exec_arena[EXEC_ARENA];
static positive exec_arena_used;
static p8 exec_nothing[1];

// A diagnostic is not output. dash writes these to standard error and a script
// that redirects one and not the other can tell, so this does not go through
// the buffered writer that everything else uses.
static fn exec_error(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        log_flush();
        system_call_3(syscall(write), stderr, (positive)data, length);
}

static string_address exec_arena_copy(string_address text)
{
        positive length = string_length(text) + 1;
        string_address into;

        if (exec_arena_used + length > EXEC_ARENA)
                return exec_nothing;

        into = exec_arena + exec_arena_used;
        memory_copy(into, text, length);
        exec_arena_used += length;

        return into;
}

/*
        Glob matching, for case and for nothing else here.

        A star tries every split of what is left, which is quadratic on a
        pattern of nothing but stars and linear on everything anybody writes.
*/
static bool exec_match(string_address pattern, string_address text)
{
        while (string_get(pattern))
        {
                p8 mark = string_get(pattern);

                if (mark == '*')
                {
                        while (string_get(pattern) == '*')
                                pattern++;

                        if (!string_get(pattern))
                                return true;

                        while (string_get(text))
                        {
                                if (exec_match(pattern, text))
                                        return true;

                                text++;
                        }

                        return exec_match(pattern, text);
                }

                if (!string_get(text))
                        return false;

                if (mark == '?')
                {
                        pattern++;
                        text++;
                        continue;
                }

                if (mark == '[')
                {
                        string_address scan = pattern + 1;
                        bool negate = false;
                        bool hit = false;
                        bool first = true;

                        if (string_get(scan) == '!' || string_get(scan) == '^')
                        {
                                negate = true;
                                scan++;
                        }

                        while (string_get(scan) && (string_get(scan) != ']' || first))
                        {
                                p8 low = string_get(scan);

                                first = false;

                                if (low == '\\' && string_get(scan + 1))
                                        low = string_get(++scan);

                                if (string_get(scan + 1) == '-' &&
                                    string_get(scan + 2) && string_get(scan + 2) != ']')
                                {
                                        if (string_get(text) >= low &&
                                            string_get(text) <= string_get(scan + 2))
                                                hit = true;

                                        scan += 3;
                                        continue;
                                }

                                if (low == string_get(text))
                                        hit = true;

                                scan++;
                        }

                        // A set nobody closed is a literal bracket, which is
                        // what every shell does with it.
                        if (!string_get(scan))
                        {
                                if (string_get(text) != '[')
                                        return false;

                                pattern++;
                                text++;
                                continue;
                        }

                        if (hit == negate)
                                return false;

                        pattern = scan + 1;
                        text++;
                        continue;
                }

                if (mark == '\\' && string_get(pattern + 1))
                        pattern++;

                if (string_get(pattern) != string_get(text))
                        return false;

                pattern++;
                text++;
        }

        return string_get(text) == end;
}

/*
        Redirection, as file descriptors and not as a writer.

        The shell used to swap the function its builtins printed through, which
        left a builtin redirected and a spawned program not, and neither of
        them redirected inside a pipeline. Moving fd 1 is what the child of a
        pipe needs anyway, so there is one mechanism instead of two, and the
        buffered output is flushed before the descriptor moves under it.
*/
typedef struct
{
        b32 fd;
        b32 saved;
} exec_saved_fd;

static exec_saved_fd exec_saves[REDIRECT_SAVE_MAX];
static b32 exec_save_count;

#define F_DUPFD_CLOEXEC 1030

static bool exec_save_fd(b32 fd)
{
        if (exec_save_count >= REDIRECT_SAVE_MAX)
                return false;

        exec_saves[exec_save_count].fd = fd;
        exec_saves[exec_save_count].saved =
            (b32)system_call_3(syscall(fcntl), fd, F_DUPFD_CLOEXEC, 10);
        exec_save_count++;

        return true;
}

static fn exec_redirect_restore(b32 mark)
{
        log_flush();

        while (exec_save_count > mark)
        {
                exec_saved_fd address_to saved = exec_saves + --exec_save_count;

                if (saved->saved >= 0)
                {
                        system_call_3(syscall(dup3), saved->saved, saved->fd, 0);
                        system_call_1(syscall(close), saved->saved);
                        continue;
                }

                system_call_1(syscall(close), saved->fd);
        }
}

// exec with nothing to run keeps its redirections, so what was put aside to
// undo them is dropped instead: holding the dup would keep the old file open
// for the rest of the line and never put it back.
static fn exec_redirect_forget(b32 mark)
{
        while (exec_save_count > mark)
        {
                exec_saved_fd address_to saved = exec_saves + --exec_save_count;

                if (saved->saved >= 0)
                        system_call_1(syscall(close), saved->saved);
        }
}

/*
        A here-document body with its parameters filled in.

        Not shell_expand_word: that also takes quotes off, and a quote in a
        here-document body is a quote. Only the expansions happen, and only
        when the delimiter was unquoted.
*/
static positive exec_here_expand(string_address body, positive length,
                                 string_address address_to out)
{
        positive start = token_used;
        positive at = 0;

        while (at < length)
        {
                p8 value = string_get(body + at);

                if (value == '\\' && at + 1 < length &&
                    (string_get(body + at + 1) == '$' ||
                     string_get(body + at + 1) == '`' ||
                     string_get(body + at + 1) == '\\'))
                {
                        at++;
                        token_push(string_get(body + at++));
                        continue;
                }

                if (value == '$')
                {
                        at = (positive)(shell_expand(body + at) - body);
                        continue;
                }

                token_push(value);
                at++;
        }

        address_to out = token_storage + start;

        return token_used - start;
}

// A here-document body reaches the command through a pipe. The body is written
// before anything reads it, so it has to fit what the pipe will hold without
// anybody draining it -- 64k on Linux, and this refuses beyond that rather
// than deadlocking against itself.
static bipolar exec_here_pipe(string_address body, positive length)
{
        b32 ends[2];

        if (length > 60000)
                return -1;

        if (system_call_2(syscall(pipe2), (positive)ends, 0) < 0)
                return -1;

        if (length)
                system_call_3(syscall(write), ends[1], (positive)body, length);

        system_call_1(syscall(close), ends[1]);

        return ends[0];
}

static bool exec_redirect_apply(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        b32 at;

        for (at = 0; at < node->redirect_count; at++)
        {
                parse_redirect address_to want = parse_redirects + node->redirect + at;
                string_address target = shell_expand_word(parse_words[want->word]);
                bipolar opened = -1;

                /*
                        The descriptor is put aside before anything is opened.

                        open hands back the lowest free descriptor, which is
                        the one being redirected whenever that one was closed.
                        Saving after the open therefore recorded the file that
                        had just arrived as "what was there before", and put it
                        back instead of closing it.
                */
                if (!exec_save_fd(want->fd))
                        return false;

                if (want->op == OP_DLESS)
                {
                        string_address body = want->kept
                                                  ? parse_kept_text + want->body
                                                  : here_text + want->body;
                        positive length = want->body_length;

                        if (!want->raw)
                                length = exec_here_expand(body, length, address_of body);

                        opened = exec_here_pipe(body, length);
                }
                else if (want->op == OP_GREATAND || want->op == OP_LESSAND)
                {
                        if (string_is(target, '-') && string_not(target + 1, end))
                        {
                                system_call_1(syscall(close), want->fd);
                                continue;
                        }

                        opened = system_call_1(syscall(dup), parse_number(target));
                }
                else if (want->op == OP_LESS)
                        opened = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)target, FILE_READ, 0);
                else if (want->op == OP_DGREAT)
                        opened = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)target, FILE_APPEND, 0666);
                else if (want->op == OP_LESSGREAT)
                        opened = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)target,
                                               FILE_READ_WRITE | FILE_CREATE, 0666);
                else
                        opened = system_call_4(syscall(openat), AT_FDCWD,
                                               (positive)target, FILE_WRITE, 0666);

                if (opened < 0)
                {
                        string_format(exec_error, "Cannot redirect: %s\n", target);
                        return false;
                }

                log_flush();

                // dup3 onto the descriptor it was handed is an error rather
                // than the no-op dup2 makes of it, and open answers with
                // exactly that descriptor when it was the lowest one free.
                if (opened != want->fd)
                {
                        system_call_3(syscall(dup3), opened, want->fd, 0);
                        system_call_1(syscall(close), opened);
                }
        }

        return true;
}

typedef struct
{
        p8 name[FUNCTION_NAME];
        b32 body;
        // Where this body sits in the kept arenas, so that redefining it can
        // hand the space back rather than leaving it behind.
        parse_marks from;
        parse_marks to;
} exec_function;

static exec_function exec_functions[FUNCTION_MAX];
static b32 exec_function_count;

// Whether a name is a function, which type asks and nothing else does.
bool exec_function_here(string_address name);

static b32 exec_function_find(string_address name)
{
        b32 index;

        for (index = 0; index < exec_function_count; index++)
        {
                if (!string_compare(exec_functions[index].name, name))
                        return exec_functions[index].body;
        }

        return 0;
}

bool exec_function_here(string_address name)
{
        return exec_function_find(name) != 0;
}

static b32 exec_node(b32 index);
static b32 exec_node_kind(b32 index);

static b32 exec_define(b32 index)
{
        string_address name = parse_words[parse_nodes[index].word];
        parse_marks before;
        bool released = false;
        b32 body;
        b32 slot;

        for (slot = 0; slot < exec_function_count; slot++)
        {
                if (!string_compare(exec_functions[slot].name, name))
                        break;
        }

        if (slot == exec_function_count)
        {
                if (exec_function_count >= FUNCTION_MAX)
                {
                        string_format(exec_error, "Too many functions\n");
                        shell_status = 1;
                        return 1;
                }

                exec_functions[slot].body = 0;
                exec_function_count++;
        }

        /*
                The body this one replaces, given back where it can be.

                The kept arenas are a stack, so only the last definition taken
                can be handed back -- which is the one a script redefining a
                function in a loop keeps making, and the reason such a script
                used to run the arena out and then walk over what was left.
        */
        if (exec_functions[slot].body)
                released = parse_release(address_of exec_functions[slot].from,
                                         address_of exec_functions[slot].to);

        parse_mark(address_of before);
        body = parse_keep(parse_nodes[index].right,
                          address_of exec_functions[slot].to);

        if (!body)
        {
                // What was there was written over by the attempt, so saying
                // the name is gone is the honest answer.
                if (released)
                        exec_functions[slot].body = 0;

                string_format(exec_error, "No room for function: %s\n", name);
                shell_status = 1;
                return 1;
        }

        exec_functions[slot].from = before;
        string_copy_max(exec_functions[slot].name, name, FUNCTION_NAME - 1);
        exec_functions[slot].body = body;
        shell_status = 0;

        return 0;
}

static b32 exec_call(b32 body)
{
        positive saved_count = shell_parameter_count;
        positive saved;
        b32 status;

        if (exec_function_depth >= FUNCTION_DEPTH_MAX)
        {
                string_format(exec_error, "Too deep\n");
                shell_status = 1;
                return 1;
        }

        saved = shell_parameters_save();
        shell_parameters_set(shell_argv + 1, shell_argc > 0 ? shell_argc - 1 : 0);

        exec_function_depth++;
        shell_local_enter();
        status = exec_node(body);
        shell_local_leave();
        exec_function_depth--;

        // return leaves the function and nothing further out.
        if (exec_signal == EXEC_SIGNAL_RETURN)
                exec_signal = EXEC_SIGNAL_NONE;

        shell_parameters_restore(saved, saved_count);

        return status;
}

static bool exec_is_assignment(string_address word)
{
        positive length = 0;

        while (shell_name_character(string_get(word + length)))
                length++;

        // A name cannot start with a digit, and 2=x is a command, not an
        // assignment.
        if (!length || (string_get(word) >= '0' && string_get(word) <= '9'))
                return false;

        return string_get(word + length) == '=';
}

static fn exec_assign(string_address word)
{
        p8 name[128];
        positive length = 0;

        while (shell_name_character(string_get(word + length)))
                length++;

        if (length >= sizeof(name))
                return;

        memory_copy(name, word, length);
        name[length] = end;

        env_set(name, word + length + 1);
}

/*
        The fifteen names POSIX calls special.

        What makes them special here is only that an assignment written in
        front of one outlives it: "x=1 export y" leaves x set and "x=1 cat"
        does not. Everything else about them is the same as any other builtin.
*/
static bool exec_special_builtin(string_address name)
{
        return word_is(name, ":") || word_is(name, ".") ||
               word_is(name, "break") || word_is(name, "continue") ||
               word_is(name, "eval") || word_is(name, "exec") ||
               word_is(name, "exit") || word_is(name, "export") ||
               word_is(name, "readonly") || word_is(name, "return") ||
               word_is(name, "set") || word_is(name, "shift") ||
               word_is(name, "times") || word_is(name, "trap") ||
               word_is(name, "unset");
}

/*
        What a name was before the command in front of it changed it.

        The value is copied rather than pointed at: env_set can compact the
        block the value lives in, so a pointer taken before the assignment is
        a pointer into whatever moved there after it. A name that was not set
        is remembered as no value at all, which is what has to be put back.
*/
typedef struct
{
        string_address name;
        string_address value;
} exec_kept_value;

#define ASSIGN_MAX 16

static bool exec_keep_value(exec_kept_value address_to kept, string_address word)
{
        positive length = 0;
        string_address value;

        while (shell_name_character(string_get(word + length)))
                length++;

        if (exec_arena_used + length + 1 > EXEC_ARENA)
                return false;

        kept->name = exec_arena + exec_arena_used;
        memory_copy(kept->name, word, length);
        kept->name[length] = end;
        exec_arena_used += length + 1;

        value = env_get(kept->name);
        kept->value = null;

        if (!value)
                return true;

        kept->value = exec_arena_copy(value);

        return kept->value != exec_nothing;
}

static fn exec_put_back(exec_kept_value address_to kept, b32 count)
{
        while (count--)
        {
                if (kept[count].value)
                        env_set(kept[count].name, kept[count].value);
                else
                        env_unset(kept[count].name);
        }
}

static b32 exec_dispatch()
{
        string_address name = shell_argv[0];
        b32 body;

        if (!string_compare(name, ":"))
        {
                shell_status = 0;
                return 0;
        }

        if (!string_compare(name, "break") || !string_compare(name, "continue"))
        {
                b32 levels = shell_argc > 1 ? parse_number(shell_argv[1]) : 1;

                if (levels < 1)
                        levels = 1;

                if (exec_loop_depth)
                {
                        if (levels > exec_loop_depth)
                                levels = exec_loop_depth;

                        exec_signal = !string_compare(name, "break")
                                          ? EXEC_SIGNAL_BREAK
                                          : EXEC_SIGNAL_CONTINUE;
                        exec_signal_level = levels;
                }

                shell_status = 0;
                return 0;
        }

        if (!string_compare(name, "return"))
        {
                if (shell_argc > 1)
                        shell_status = parse_number(shell_argv[1]);

                exec_signal = EXEC_SIGNAL_RETURN;
                return shell_status;
        }

        body = exec_function_find(name);

        if (body)
                return exec_call(body);

        /*
                A path, and only a path.

                This used to take any name beginning with a dot, which made
                "." itself a path -- so sourcing a file tried to execute it
                and came back with permission denied. What is meant here is
                ./name and ../name, which have a slash in them like every
                other path does.
        */
        if (string_is(name, '/') || string_first_of(name, '/'))
        {
                shell_execute_command();
                return shell_status;
        }

        if (shell_builtin(shell_arguments()))
                return shell_status;

        {
                static p8 found[768];

                if (shell_find_in_path(name, found, sizeof(found)))
                {
                        shell_argv[0] = found;
                        shell_execute_command();
                        return shell_status;
                }
        }

        shell_status = 127;
        string_format(exec_error, "%s: not found\n", name);

        return shell_status;
}

/*
        The traps that arrived, run.

        Between commands and nowhere else. The action is a line, so it goes
        through the parser, and the parser is only free once the command it
        interrupted has finished -- which is also the moment POSIX names.

        What the action leaves behind is put back: a trap does not change the
        status the interrupted command answered with, and a return or a break
        inside one belongs to the action and not to the loop it landed in.
*/
fn exec_traps()
{
        b32 kept_status = shell_status;
        b32 kept_signal = exec_signal;
        b32 kept_level = exec_signal_level;
        bool kept_tested = exec_tested;
        bipolar number;

        if (!trap_waiting() || !run_line)
                return;

        trap_entered(true);

        while ((number = trap_taken()) >= 0)
        {
                string_address action = trap_action((positive)number);

                if (!action || !string_get(action))
                        continue;

                exec_signal = EXEC_SIGNAL_NONE;
                exec_tested = false;
                parse_nest_enter();
                run_line(action);
                parse_nest_leave();
        }

        trap_entered(false);

        shell_status = kept_status;
        exec_signal = kept_signal;
        exec_signal_level = kept_level;
        exec_tested = kept_tested;
}

static b32 exec_simple(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        exec_kept_value kept[ASSIGN_MAX];
        positive arena_mark = exec_arena_used;
        b32 kept_count = 0;
        b32 mark = exec_save_count;
        b32 count = 0;
        b32 first = 0;
        b32 status;
        b32 at;

        token_used = 0;
        token_overflow = false;

        for (at = 0; at < node->word_count && count < MAX_TOKENS; at++)
        {
                string_address word = parse_words[node->word + at];

                /*
                        An assignment in front of a command is expanded whole:
                        x="a b" sets x to one value, not two words, and does
                        not glob. Only in front -- past the command name the
                        same text is an ordinary argument.
                */
                if (count == first && exec_is_assignment(word))
                {
                        shell_argv[count++] = shell_expand_word(word);
                        first++;
                        continue;
                }

                count += (b32)shell_expand_fields(word, shell_argv + count,
                                                  MAX_TOKENS - count);
        }

        shell_argv[count] = null;
        shell_argc = count;

        /*
                An assignment in front of a command covers that command only.

                It has to be visible to what runs -- a spawned program reads
                it out of the environment and a builtin reads it out of the
                same table -- so it is made and then unmade, rather than being
                handed over as an environment of its own. The exception is a
                special builtin, in front of which POSIX says the assignment
                stays; and assignments with no command after them are the
                command, so they stay too.
        */
        if (first && first != count && first <= ASSIGN_MAX &&
            !exec_special_builtin(shell_argv[first]))
        {
                for (at = 0; at < first; at++)
                {
                        if (exec_keep_value(kept + kept_count, shell_argv[at]))
                        {
                                kept_count++;
                                continue;
                        }

                        // Nowhere to remember it is a reason to leave it set,
                        // not a reason to put half of them back.
                        exec_arena_used = arena_mark;
                        kept_count = 0;
                        break;
                }
        }

        for (at = 0; at < first; at++)
                exec_assign(shell_argv[at]);

        if (!exec_redirect_apply(index))
        {
                exec_redirect_restore(mark);
                exec_put_back(kept, kept_count);
                exec_arena_used = arena_mark;
                shell_status = 1;
                return 1;
        }

        if (first == count)
        {
                exec_redirect_restore(mark);
                shell_status = 0;
                return 0;
        }

        for (at = first; at <= count; at++)
                shell_argv[at - first] = shell_argv[at];

        shell_argc = count - first;

        status = exec_dispatch();

        // exec with nothing to run is there for its redirections, and those
        // belong to the shell from here on.
        if (word_is(shell_argv[0], "exec") && shell_argc == 1)
                exec_redirect_forget(mark);
        else
                exec_redirect_restore(mark);

        exec_put_back(kept, kept_count);
        exec_arena_used = arena_mark;

        return status;
}

// What a break or a continue means to the loop it lands in: go round again,
// stop, or hand it further out still.
static bool exec_loop_again()
{
        if (!exec_signal)
                return true;

        if (exec_signal == EXEC_SIGNAL_RETURN)
                return false;

        if (exec_signal_level > 1)
        {
                exec_signal_level--;
                return false;
        }

        if (exec_signal == EXEC_SIGNAL_CONTINUE)
        {
                exec_signal = EXEC_SIGNAL_NONE;
                return true;
        }

        exec_signal = EXEC_SIGNAL_NONE;

        return false;
}

static b32 exec_loop(b32 index, bool until)
{
        parse_node address_to node = parse_nodes + index;
        b32 status = 0;

        while (1)
        {
                bool tested = exec_tested;
                b32 test;

                exec_tested = true;
                test = exec_node(node->left);
                exec_tested = tested;

                if (exec_signal)
                        break;

                if (until ? test == 0 : test != 0)
                        break;

                exec_loop_depth++;
                status = exec_node(node->right);
                exec_loop_depth--;

                if (!exec_loop_again())
                        break;
        }

        return status;
}

static b32 exec_for(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        string_address name = parse_words[node->word];
        string_address items[POSITIONAL_MAX * 2];
        positive mark = exec_arena_used;
        b32 count = 0;
        b32 status = 0;
        b32 at;

        token_used = 0;

        /*
                The list is expanded the way a command's arguments are.

                "for i in $x" walks the fields of x and "for i in *.c" walks
                the names on disk; expanding each word whole made a list of
                one item that happened to contain blanks and a pattern that
                was never asked about. The fields come back in storage the
                next word's expansion reuses, so each is copied out before
                the next one is asked for.
        */
        if (node->flags)
        {
                bool room = true;

                for (at = 1; at < node->word_count && room; at++)
                {
                        string_address fields[POSITIONAL_MAX];
                        positive made = shell_expand_fields(
                            parse_words[node->word + at], fields, POSITIONAL_MAX);
                        positive field;

                        for (field = 0; field < made; field++)
                        {
                                string_address kept;

                                if (count >= (b32)(sizeof(items) / sizeof(items[0])))
                                {
                                        room = false;
                                        break;
                                }

                                kept = exec_arena_copy(fields[field]);

                                // An arena with nothing left in it gives back
                                // the empty string, and a loop that quietly
                                // ran over empty items is worse than one that
                                // stops where it ran out.
                                if (kept == exec_nothing)
                                {
                                        room = false;
                                        break;
                                }

                                items[count++] = kept;
                        }
                }
        }
        else
        {
                for (at = 0; at < (b32)shell_parameter_count && count < (b32)(sizeof(items) / sizeof(items[0])); at++)
                        items[count++] = exec_arena_copy(shell_parameter[at]);
        }

        for (at = 0; at < count; at++)
        {
                env_set(name, items[at]);

                exec_loop_depth++;
                status = exec_node(node->right);
                exec_loop_depth--;

                if (!exec_loop_again())
                        break;
        }

        exec_arena_used = mark;

        return status;
}

static b32 exec_case(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        positive mark = exec_arena_used;
        string_address subject;
        b32 item;
        b32 status = 0;

        token_used = 0;
        subject = exec_arena_copy(shell_expand_word(parse_words[node->word]));

        for (item = node->left; item; item = parse_nodes[item].next)
        {
                b32 at;

                for (at = 0; at < parse_nodes[item].word_count; at++)
                {
                        string_address pattern;

                        token_used = 0;
                        pattern = shell_expand_word(
                            parse_words[parse_nodes[item].word + at]);

                        if (!exec_match(pattern, subject))
                                continue;

                        status = exec_node(parse_nodes[item].right);
                        exec_arena_used = mark;

                        return status;
                }
        }

        exec_arena_used = mark;

        return status;
}

static b32 exec_if(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        bool tested = exec_tested;
        b32 test;

        exec_tested = true;
        test = exec_node(node->left);
        exec_tested = tested;

        if (exec_signal)
                return test;

        if (test == 0)
                return exec_node(node->right);

        if (node->extra)
                return exec_node(node->extra);

        return 0;
}

static b32 exec_child_status(bipolar child)
{
        positive state = 0;

        if (child < 0)
                return 1;

        system_call_4(syscall(wait4), child, (positive)address_of state, 0, 0);

        return (b32)(state >> 8 & 0xff);
}

static b32 exec_subshell(b32 index)
{
        bipolar child;

        log_flush();
        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                b32 status;

                shell_default(SIGNAL_INTERRUPT);
                shell_default(SIGNAL_QUIT);
                trap_default_all();
                exec_forked = true;

                status = exec_node(parse_nodes[index].left);
                log_flush();
                exit(status);
        }

        return exec_child_status(child);
}

/*
        A pipeline.

        Every stage gets a process of its own, because a builtin on either end
        of a pipe has to have its own fd 1 and its own fd 0. The parent closes
        both ends of every pipe it made before it waits: a write end still open
        here is an end of file the reader never sees, and the whole shell
        stops.
*/
static b32 exec_pipe(b32 first, b32 count)
{
        bipolar children[PIPELINE_MAX];
        bipolar upstream = -1;
        b32 child = first;
        b32 started = 0;
        b32 status = 0;
        b32 at;

        if (count > PIPELINE_MAX)
                count = PIPELINE_MAX;

        while (child && started < count)
        {
                b32 ends[2];
                bipolar made;
                bool last = started + 1 >= count || !parse_nodes[child].next;

                ends[0] = -1;
                ends[1] = -1;

                if (!last && system_call_2(syscall(pipe2), (positive)ends, 0) < 0)
                        break;

                log_flush();
                made = system_call_2(syscall(clone), SIGCHLD, 0);

                if (made == 0)
                {
                        shell_default(SIGNAL_INTERRUPT);
                        shell_default(SIGNAL_QUIT);
                        trap_default_all();
                        exec_forked = true;

                        if (upstream >= 0)
                        {
                                system_call_3(syscall(dup3), upstream, 0, 0);
                                system_call_1(syscall(close), upstream);
                        }

                        if (!last)
                        {
                                system_call_1(syscall(close), ends[0]);
                                system_call_3(syscall(dup3), ends[1], 1, 0);
                                system_call_1(syscall(close), ends[1]);
                        }

                        status = exec_node(child);
                        log_flush();
                        exit(status);
                }

                if (upstream >= 0)
                        system_call_1(syscall(close), upstream);

                upstream = -1;

                if (!last)
                {
                        system_call_1(syscall(close), ends[1]);
                        upstream = ends[0];
                }

                children[started++] = made;
                child = parse_nodes[child].next;
        }

        if (upstream >= 0)
                system_call_1(syscall(close), upstream);

        for (at = 0; at < started; at++)
        {
                b32 got = exec_child_status(children[at]);

                if (at + 1 == started)
                        status = got;
        }

        return status;
}

static b32 exec_pipeline(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        bool tested = exec_tested;
        b32 count = 0;
        b32 child;
        b32 status;

        for (child = node->left; child; child = parse_nodes[child].next)
                count++;

        if (node->flags)
                exec_tested = true;

        status = count > 1 ? exec_pipe(node->left, count) : exec_node(node->left);

        exec_tested = tested;

        if (node->flags)
        {
                shell_status = status ? 0 : 1;

                return shell_status;
        }

        shell_status = status;
        exec_errexit(status);

        return status;
}

static b32 exec_and_or(b32 index)
{
        b32 child = parse_nodes[index].left;
        bool tested = exec_tested;
        b32 status = 0;

        while (child)
        {
                b32 op = parse_nodes[child].op;

                if (!(op == OP_AND_IF && status != 0) &&
                    !(op == OP_OR_IF && status == 0))
                {
                        exec_tested = parse_nodes[child].next ? true : tested;
                        status = exec_node(child);
                }

                if (exec_signal)
                        break;

                child = parse_nodes[child].next;
        }

        exec_tested = tested;

        return status;
}

static b32 exec_background(b32 index)
{
        bipolar child;

        log_flush();
        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                b32 status;

                shell_default(SIGNAL_INTERRUPT);
                shell_default(SIGNAL_QUIT);
                trap_default_all();
                exec_forked = true;

                status = exec_node(index);
                log_flush();
                exit(status);
        }

        return 0;
}

static b32 exec_list(b32 index)
{
        b32 child = parse_nodes[index].left;
        b32 status = 0;

        while (child)
        {
                status = parse_nodes[child].flags ? exec_background(child)
                                                  : exec_node(child);

                if (exec_signal)
                        break;

                child = parse_nodes[child].next;
        }

        return status;
}

/*
        Every node ends at a command boundary, which is where a trap that
        arrived is allowed to run. A simple command is the usual one; a
        subshell or a loop is one too, and checking only the simple ones left
        a trap waiting behind a "( ... )" until whatever came after it.
*/
static b32 exec_node(b32 index)
{
        b32 status = exec_node_kind(index);

        exec_traps();

        return status;
}

static b32 exec_node_kind(b32 index)
{
        parse_node address_to node;
        b32 mark;
        b32 status;

        if (!index)
                return shell_status;

        node = parse_nodes + index;

        if (node->kind == NODE_SIMPLE)
        {
                status = exec_simple(index);
                exec_errexit(status);

                return status;
        }

        if (node->kind == NODE_LIST)
                return exec_list(index);

        if (node->kind == NODE_ANDOR)
                return exec_and_or(index);

        if (node->kind == NODE_PIPELINE)
                return exec_pipeline(index);

        if (node->kind == NODE_FUNCTION)
                return exec_define(index);

        // Everything left is a compound command, and every one of them can
        // carry redirections of its own.
        mark = exec_save_count;
        token_used = 0;

        if (node->redirect_count && !exec_redirect_apply(index))
        {
                exec_redirect_restore(mark);
                shell_status = 1;
                return 1;
        }

        if (node->kind == NODE_IF)
                status = exec_if(index);
        else if (node->kind == NODE_WHILE)
                status = exec_loop(index, false);
        else if (node->kind == NODE_UNTIL)
                status = exec_loop(index, true);
        else if (node->kind == NODE_FOR)
                status = exec_for(index);
        else if (node->kind == NODE_CASE)
                status = exec_case(index);
        else if (node->kind == NODE_SUBSHELL)
                status = exec_subshell(index);
        else
                status = exec_node(node->left);

        exec_redirect_restore(mark);
        shell_status = status;

        if (node->kind == NODE_SUBSHELL)
                exec_errexit(status);

        return status;
}

#define WAIT_NO_HANG 1

static b32 exec_depth;

/*
        A tree, walked.

        eval, . and a trap action are programs run from inside one that is
        already running, and what they leave behind is not theirs to throw
        away: the arena holds the items of every for loop further out and the
        saved descriptors hold the redirections of every command further out.
        Both used to start over here, so an eval inside a loop walked over the
        value the loop was standing on and a redirection around it was never
        put back.
*/
fn exec_program(b32 root)
{
        positive kept_arena = exec_arena_used;
        b32 kept_saves = exec_save_count;

        exec_signal = EXEC_SIGNAL_NONE;
        exec_signal_level = 0;

        // Anything started with & is nobody's to wait for, and a zombie per
        // background command is a table full of them by the end of a script.
        if (!exec_depth)
        {
                positive state = 0;

                while (system_call_4(syscall(wait4), (positive)-1,
                                     (positive)address_of state, WAIT_NO_HANG, 0) > 0)
                        ;
        }

        exec_depth++;
        exec_node(root);
        exec_depth--;

        exec_arena_used = kept_arena;
        exec_save_count = kept_saves;
}
