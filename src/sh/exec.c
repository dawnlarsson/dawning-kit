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
#define EXEC_SIGNAL_FATAL 4

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
#define SHELL_NOEXEC ((positive)1 << ('n' - 'a'))

static bool exec_tested;
static bool exec_forked;
static bool exec_asynchronous;
static bool exec_pipe_status_pending;
static b32 exec_pipe_status_value;
static b32 exec_return_previous;
/* Set only by a builtin path that diagnosed an invocation error. A nonzero
   status alone is not enough: eval, return, dot and trap can all answer
   nonzero without the POSIX special-builtin fatality applying. */
static bool exec_special_error;

static fn exec_special_error_note()
{
        exec_special_error = true;
}

/*
        Which line the command now running was written on.

        The reader's own count is where the input has got to, which is not the
        same thing at all inside a function: a body written twenty lines ago
        is running now, and both a call frame and $LINENO want the line it was
        written on rather than the line that called it.
*/
static b32 exec_line;

fn shell_trap_exit();
fn exec_traps();
fn job_forget();
fn trap_child_began();
static b32 exec_child_status(bipolar child);
static b32 job_wait_foreground(positive number);
static fn exec_pipe_status_publish(bipolar address_to values, positive count);

fn exec_child_began()
{
        exec_forked = true;
        trap_child_began();
        // One more shell between this process and the one the script began
        // in, which is the whole of what $BASH_SUBSHELL counts.
        shell_subshell_depth++;

        //      What a process substitution left with the shell belongs to the
        //      shell. A fork inherits the list and none of the children on
        //      it, so it would ask about processes that are not its own and
        //      close descriptors somebody else is still handing out.
        shell_substitutions_forget();
        shell_background_child();
}

static DEAD_END fn exec_child_leave(b32 status)
{
        shell_status = status;
        shell_trap_exit();
        log_flush();
        exit(status);
}

/*
        An expansion error at a terminal ends this input line, not the shell.

        A forked pipeline, subshell or command substitution can leave outright:
        its parent is the interactive shell that has to survive. In the shell
        itself the signal is carried through the executor like return and
        break, except that no construct is allowed to consume it. The reader
        clears it when the next top-level input line begins.
*/
COLD fn exec_expand_fatal()
{
        if (exec_forked)
        {
                log_flush();
                system_call_1(syscall(exit_group), 2);
        }

        exec_signal = EXEC_SIGNAL_FATAL;
        exec_signal_level = 0;
}

static fn exec_line_begin()
{
        if (exec_signal == EXEC_SIGNAL_FATAL)
        {
                exec_signal = EXEC_SIGNAL_NONE;

                // A trap that arrived during the failed expansion belongs
                // between input lines: not to the tail of the aborted one,
                // and not after the first command of the next one.
                exec_traps();
        }
}

static PURE bool exec_line_aborted()
{
        return exec_signal == EXEC_SIGNAL_FATAL;
}

/*
        A sourced file is a control-flow boundary of its own. Return stops at
        that boundary; break and continue must remain set so the caller's loop
        can consume them. Without this check shell_dot read another physical
        line, exec_program reset the signal, and every command after the
        control builtin ran anyway.
*/
static bool exec_source_stop(b32 address_to startup_status)
{
        if (!exec_signal)
                return false;

        if (exec_signal == EXEC_SIGNAL_RETURN)
        {
                if (startup_status)
                        *startup_status = exec_return_previous;
                exec_signal = EXEC_SIGNAL_NONE;
        }

        return true;
}

/*
        One of the three conditions the executor raises itself.

        Run the way a caught signal's action is run and with the same care:
        the status the condition was raised on is what the action reads and
        what the shell goes back to afterwards, and a condition raised inside
        an action is not raised again -- a DEBUG trap whose action is a
        command would otherwise never stop.
*/
static bool exec_condition_inside;

static COLD fn exec_trap_condition(positive number)
{
        string_address action = trap_action(number);
        b32 kept_status = shell_status;
        b32 kept_signal = exec_signal;
        b32 kept_level = exec_signal_level;
        bool kept_tested = exec_tested;

        if (!action || !string_get(action) || exec_condition_inside ||
            exec_line_aborted())
                return;

        exec_condition_inside = true;
        exec_signal = EXEC_SIGNAL_NONE;
        exec_tested = false;
        parse_nest_enter();
        run_lines(action);
        shell_input_end();
        parse_nest_leave();
        exec_condition_inside = false;

        shell_status = kept_status;
        exec_signal = kept_signal;
        exec_signal_level = kept_level;
        exec_tested = kept_tested;
}

/*
        Whether a condition reaches inside a function.

        Bash keeps DEBUG and RETURN out of functions unless functrace is set
        and ERR out of them unless errtrace is, so that a trap written for the
        script does not fire once per line of every library it sources. A
        subshell is the same question asked of a fork: without errtrace the
        ERR trap belongs to the shell that set it, so "( false )" raises it
        once in the parent and not again in the child.
*/
static PURE bool exec_condition_reaches(positive option)
{
        return (!exec_function_depth && !exec_forked) ||
               shell_extra_on(option);
}

static fn exec_errexit(b32 status)
{
        if (exec_line_aborted() || !status || exec_tested)
                return;

        // Where errexit would leave is where the ERR trap runs, whether or
        // not errexit is on: the two ask the same question of the same
        // command.
        if (trap_err_here && exec_condition_reaches(SHELL_EXTRA_ERRTRACE))
                exec_trap_condition(TRAP_ERR);

        if (!(shell_options & SHELL_ERREXIT))
                return;

        shell_status = status;

        if (exec_forked)
                exec_child_leave(status);

        shell_trap_exit();
        log_flush();
        exit(status);
}


#define REDIRECT_SAVE_MAX 64
//      What a command keeps while it is being built. argv and the saved
//      assignments point in here, so like the expansion store these bytes may
//      never move once handed out.
static shell_store exec_store;
static p8 exec_nothing[1];

// A diagnostic bypasses the buffered output writer and goes to stderr.
#define exec_error log_error

/*
        Job control.

        A job is what one line started: a single command, or a whole pipeline,
        remembered under the number a person sees in brackets. What each of its
        children answered with is already kept in the wait table beside `wait`,
        so a row here holds only what that table has no opinion about -- the
        number, the process group, whether it is stopped, and the text to print
        back.

        A background job gets a row whether or not `set -m` is on, because
        `jobs` and `$!` are answers every shell gives. What the monitor option
        adds is a process group per job and, at a terminal, handing that group
        the terminal and taking it back afterwards.
*/
#define JOB_SIGNAL_CONTINUE 18
#define JOB_SIGNAL_STOP 19
#define JOB_SIGNAL_STOP_KEY 20
#define JOB_SIGNAL_TTY_INPUT 21
#define JOB_SIGNAL_TTY_OUTPUT 22

#define JOB_NO_HANG 1
#define JOB_UNTRACED 2
#define JOB_CONTINUED 8

// The two terminal ioctls are the whole of a shell's claim on a terminal.
// x86 and asm-generic agree on both numbers, which is why they are spelled
// here rather than asked of a header this tree does not have -- the same
// reason TCGETS is spelled beside the prompt.
#define JOB_TERMINAL_GET_GROUP 0x540Fu
#define JOB_TERMINAL_SET_GROUP 0x5410u

#define JOB_RUNNING 0
#define JOB_STOPPED 1
#define JOB_FINISHED 2

// Where the status word sits in a listing, which is bash's column and not a
// number of this shell's choosing: a person reads the two side by side.
#define JOB_STATUS_WIDTH 27

typedef struct
{
        positive number;
        // The last child, which is what $! publishes and what the wait table
        // files every stage of the job under.
        bipolar last;
        bipolar group;
        positive state;
        // The raw wait status of the last stage, kept for the notice that
        // says how the job ended.
        positive status;
        positive stopped_by;
        bool reported;
        bool background;
        bool nohup;
        p8 address_to text;
        positive text_room;
} job_entry;

static job_entry address_to job_table;
static positive job_room;
static positive job_count;
static positive job_numbered;
static positive job_current;
static positive job_previous;

// The line the executor is running, for the jobs a rendering of the words
// cannot describe. Set by the reader, because that is the only place the
// bytes a person actually typed are still together.
string_address exec_current_line;

// The descriptor the terminal is reached through, and whether this shell may
// hand it over. A script has process groups without a terminal, so the two
// are separate questions.
static b32 job_terminal = -1;
static bipolar job_shell_group;
static bool job_terminal_owned;
static bool job_monitor_ready;

// Set by the handler and read by the executor. A signal may arrive between
// any two instructions, so the compiler is told the value can change under it.
#define SIGNAL_CHILD 17
static volatile bool job_child_news;
static bool job_child_watching;

static fn job_child_watch();

/*
        Whether this process is the shell that owns the jobs.

        A subshell is inside somebody else's job already. Giving its commands
        groups of their own would split that job in two, and handing one of
        them the terminal would take it from the job the person is looking at
        -- so a fork answers no, and its pipelines go back to being spawned
        rather than forked.
*/
static PURE bool job_monitor()
{
        return !exec_forked && shell_option_on(SHELL_OPTION_MONITOR);
}

static bipolar job_group_of(bipolar process)
{
        return system_call_1(syscall(getpgid), (positive)process);
}

static bipolar job_group_set(bipolar process, bipolar group)
{
        return system_call_2(syscall(setpgid), (positive)process,
                             (positive)group);
}

static bipolar job_signal(bipolar target, positive number)
{
        return system_call_2(syscall(kill), (positive)target, number);
}

static bipolar job_terminal_group()
{
        b32 group = 0;

        if (job_terminal < 0 ||
            system_control(job_terminal, JOB_TERMINAL_GET_GROUP,
                           address_of group) < 0)
                return -1;

        return group;
}

/*
        Handing the terminal over, and taking it back.

        tcsetpgrp from a process outside the terminal's foreground group is
        itself a SIGTTOU, which would stop the shell in the middle of starting
        the job it is handing over to. Ignoring that signal for as long as the
        monitor is on is why the call needs no guard.
*/
static fn job_terminal_give(bipolar group)
{
        b32 wanted = (b32)group;

        if (!job_terminal_owned || job_terminal < 0 || group <= 0)
                return;

        system_control(job_terminal, JOB_TERMINAL_SET_GROUP,
                       address_of wanted);
}

/*
        The monitor, turned on.

        Job control asks three things of the shell itself: a process group of
        its own, so that a job's group is never also the shell's; the
        terminal; and deafness to the three signals that would otherwise stop
        it while it arranges either. A script gets the first and neither of
        the other two, which is the whole of what `set -m` means with nobody
        watching.
*/
static fn job_monitor_start()
{
        bipolar own;

        if (job_monitor_ready)
                return;

        job_monitor_ready = true;
        job_shell_group = job_group_of(0);

        if (!shell_is_interactive)
                return;

        shell_ignore(JOB_SIGNAL_TTY_OUTPUT);
        shell_ignore(JOB_SIGNAL_TTY_INPUT);
        shell_ignore(JOB_SIGNAL_STOP_KEY);

        job_terminal = 0;
        own = system_call_1(syscall(getpid), 0);

        /* A shell sharing a group takes one of its own. Failing is not fatal:
           it keeps working and simply never arbitrates the terminal, which is
           what a shell started without one already does. */
        if (job_shell_group != own && job_group_set(0, own) == 0)
                job_shell_group = own;

        job_terminal_owned = job_terminal_group() >= 0;
        job_terminal_give(job_shell_group);
}

// `set +m`: the next job is started the POSIX way, and the shell stops
// answering for a terminal it no longer arbitrates.
static fn job_monitor_stop()
{
        job_monitor_ready = false;
        job_terminal_owned = false;
        job_terminal = -1;
}

fn job_monitor_told(bool on)
{
        if (on)
                job_monitor_start();
        else
                job_monitor_stop();
}

static positive job_find_number(positive number)
{
        for (positive at = 0; at < job_count; at++)
                if (job_table[at].number == number)
                        return at;

        return job_count;
}

static positive job_find_last(bipolar last)
{
        for (positive at = 0; at < job_count; at++)
                if (job_table[at].last == last)
                        return at;

        return job_count;
}

/*
        Which job `%+` and `%-` mean.

        The most recently started or stopped job is current and the one before
        it is previous. When a job leaves, the previous one takes its place and
        the highest-numbered job that is neither becomes previous -- which is
        the order the marks are printed in and the order `fg` with no operand
        follows.
*/
static fn job_mark(positive number)
{
        if (job_current == number)
                return;

        job_previous = job_current;
        job_current = number;
}

static fn job_marks_settle()
{
        if (job_current && job_find_number(job_current) == job_count)
        {
                job_current = job_previous;
                job_previous = 0;
        }

        if (!job_current || job_find_number(job_current) == job_count)
        {
                job_current = 0;

                for (positive at = 0; at < job_count; at++)
                        if (job_table[at].number > job_current)
                                job_current = job_table[at].number;
        }

        if (job_previous == job_current ||
            (job_previous && job_find_number(job_previous) == job_count))
                job_previous = 0;

        if (!job_previous)
                for (positive at = 0; at < job_count; at++)
                {
                        positive number = job_table[at].number;

                        if (number != job_current && number > job_previous)
                                job_previous = number;
                }
}

static fn job_drop_at(positive at)
{
        if (at >= job_count)
                return;

        if (job_table[at].text)
                memory_free(job_table[at].text, job_table[at].text_room);

        job_count--;

        for (positive step = at; step < job_count; step++)
                job_table[step] = job_table[step + 1];

        job_marks_settle();

        // A shell with no jobs left starts numbering again, which is what
        // makes the first job of the next command [1] rather than [57].
        if (!job_count)
        {
                job_numbered = 0;
                job_current = 0;
                job_previous = 0;
        }
}

// A subshell inherits $! but not the right to wait for anybody, so it must
// not inherit a table whose numbers name processes that are not its children.
fn job_forget()
{
        while (job_count)
                job_drop_at(job_count - 1);

        job_numbered = 0;
        job_current = 0;
        job_previous = 0;
}

/*
        The text a job is listed under.

        The parser keeps words, not the line they came from, so the line is
        written again out of them: a simple command is its own words, and a
        pipeline is its stages with a bar between. Anything else -- a loop, a
        subshell, a group -- has no word list of its own, and the source line
        it came from is closer to what a person typed than any rendering this
        could invent for it.
*/
static positive job_text_add(p8 address_to address_to into,
                             positive address_to room, positive used,
                             string_address text, positive length)
{
        if (!text || !shell_room((address_any address_to)into, room,
                                 used + length + 1, 1))
                return used;

        memory_copy_apart(address_to into + used, text, length);
        used += length;
        (address_to into)[used] = end;

        return used;
}

static positive job_text_line(p8 address_to address_to into,
                              positive address_to room, positive used)
{
        positive length;

        if (!exec_current_line)
                return used;

        length = string_length(exec_current_line);

        // The ampersand that made this a job is not part of the command, and
        // a listing puts its own back when the job is still running.
        while (length && (exec_current_line[length - 1] == ' ' ||
                          exec_current_line[length - 1] == '\t' ||
                          exec_current_line[length - 1] == '&'))
                length--;

        return job_text_add(into, room, used, exec_current_line, length);
}

static positive job_text_node(p8 address_to address_to into,
                              positive address_to room, positive used,
                              b32 node, b32 depth)
{
        b32 kind = parse_nodes[node].kind;
        b32 child;

        if (kind == NODE_SIMPLE)
        {
                for (b32 at = 0; at < parse_nodes[node].word_count; at++)
                {
                        b32 word = parse_nodes[node].word + at;

                        if (at)
                                used = job_text_add(into, room, used,
                                                    (string_address) " ", 1);

                        used = job_text_add(into, room, used,
                                            parse_words[word],
                                            parse_word_lengths[word]);
                }

                return used;
        }

        if (kind == NODE_PIPELINE)
        {
                if (parse_nodes[node].flags)
                        used = job_text_add(into, room, used,
                                            (string_address) "! ", 2);

                for (child = parse_nodes[node].left; child;
                     child = parse_nodes[child].next)
                {
                        used = job_text_node(into, room, used, child,
                                             depth + 1);

                        if (parse_nodes[child].next)
                                used = job_text_add(into, room, used,
                                                    (string_address) " | ", 3);
                }

                return used;
        }

        if (kind == NODE_SUBSHELL || kind == NODE_GROUP)
        {
                bool braces = kind == NODE_GROUP;

                used = job_text_add(into, room, used,
                                    braces ? (string_address) "{ "
                                           : (string_address) "( ",
                                    2);

                for (child = parse_nodes[node].left; child;
                     child = parse_nodes[child].next)
                {
                        used = job_text_node(into, room, used, child,
                                             depth + 1);

                        if (parse_nodes[child].next)
                                used = job_text_add(into, room, used,
                                                    (string_address) "; ", 2);
                }

                return job_text_add(into, room, used,
                                    braces ? (string_address) "; }"
                                           : (string_address) " )",
                                    3 - (braces ? 0 : 1));
        }

        if (kind == NODE_LIST || kind == NODE_ANDOR)
        {
                for (child = parse_nodes[node].left; child;
                     child = parse_nodes[child].next)
                {
                        b32 op = parse_nodes[child].op;

                        if (child != parse_nodes[node].left)
                                used = job_text_add(
                                    into, room, used,
                                    op == OP_AND_IF
                                        ? (string_address) " && "
                                        : op == OP_OR_IF
                                              ? (string_address) " || "
                                              : (string_address) "; ",
                                    op == OP_AND_IF || op == OP_OR_IF ? 4 : 2);

                        used = job_text_node(into, room, used, child,
                                             depth + 1);
                }

                return used;
        }

        /* A loop, a case or an if has no rendering here worth inventing. The
           line it came from is what a person typed, and it is only the whole
           truth when the job is the whole line. */
        return depth ? used : job_text_line(into, room, used);
}

/*
        A job, remembered.

        Its children are already in the wait table under the last of them, so
        this adds only the row that names them collectively. Whether the job is
        in the background is the caller's to say, because a stopped foreground
        job is a job too and must not be listed with an ampersand it never had.
*/
static positive job_started(bipolar address_to children, positive count,
                            bipolar group, b32 node, bool chain,
                            bool background)
{
        job_entry address_to entry;
        positive at;

        if (!count || children[count - 1] <= 0)
                return 0;

        /* A process identifier is reusable once the kernel has reaped its
           last owner. The new job owns the name; a stale row still carrying
           it does not. */
        at = job_find_last(children[count - 1]);

        if (at < job_count)
                job_drop_at(at);

        if (!shell_array_room(job_table, job_room, job_count + 1))
                return 0;

        entry = job_table + job_count++;

        entry->number = ++job_numbered;
        entry->last = children[count - 1];
        entry->group = group;
        entry->state = JOB_RUNNING;
        entry->status = 0;
        entry->stopped_by = 0;
        entry->reported = true;
        entry->background = background;
        entry->nohup = false;
        entry->text = null;
        entry->text_room = 0;

        job_child_watch();

        if (node < 0)
        {
                positive used = 0;

                /* A command already expanded into argv has no node to walk,
                   and the words it is about to run describe it better than
                   the ones it was written with. */
                for (positive word = 0; word < shell_argc; word++)
                {
                        if (word)
                                used = job_text_add(address_of entry->text,
                                                    address_of entry->text_room,
                                                    used,
                                                    (string_address) " ", 1);

                        used = job_text_add(address_of entry->text,
                                            address_of entry->text_room, used,
                                            shell_argv[word],
                                            string_length(shell_argv[word]));
                }
        }
        else if (node && !chain)
                job_text_node(address_of entry->text,
                              address_of entry->text_room, 0, node, 0);
        else if (node)
        {
                positive used = 0;
                b32 stage = node;

                /* A pipeline reaches this as its list of stages rather than
                   as the node above them, because that is what the executor
                   was handed and what its children were made from. */
                while (stage)
                {
                        used = job_text_node(address_of entry->text,
                                             address_of entry->text_room,
                                             used, stage,
                                             parse_nodes[node].next ? 1 : 0);
                        stage = parse_nodes[stage].next;

                        if (stage)
                                used = job_text_add(address_of entry->text,
                                                    address_of entry->text_room,
                                                    used,
                                                    (string_address) " | ", 3);
                }
        }

        job_mark(entry->number);

        return entry->number;
}

/*
        Retaining a job's children without publishing $!.

        A background job is what `$!` names; a foreground one that stopped is
        not, and bash agrees -- control-Z does not change the value a script
        reads back. The wait table is the same table either way, so the value
        is put back rather than the retention being written twice.
*/
static bool job_retain(bipolar address_to children, positive count,
                       bool pipefail, bool invert, bool publish)
{
        bipolar kept = shell_background_last;
        bool held = shell_background_started(children, count, pipefail,
                                             invert);

        if (!publish)
                shell_background_last = kept;

        return held;
}

// Whether the wait table still owes an answer for this job at all, and how
// many of its children the kernel has yet to speak about.
static positive job_rows(bipolar last)
{
        positive rows = 0;

        for (positive at = 0; at < shell_wait_count; at++)
                if (shell_wait_table[at].job == last)
                        rows++;

        return rows;
}

static positive job_running_children(bipolar last)
{
        positive left = 0;

        for (positive at = 0; at < shell_wait_count; at++)
                if (shell_wait_table[at].job == last &&
                    !(shell_wait_table[at].flags & SHELL_WAIT_DONE))
                        left++;

        return left;
}

/*
        The identifiers of a foreground pipeline, kept while its answers are
        being written over them.

        PIPESTATUS is published out of the vector the children were started
        in, so by the time a stop is noticed the identifiers are gone -- and a
        pipeline that stopped is a job, which is named by its children and not
        by what they answered.
*/
static bipolar address_to job_held;
static positive job_held_room;

static bool job_hold(bipolar address_to children, positive count)
{
        if (!count || !shell_array_room(job_held, job_held_room, count))
                return false;

        memory_copy_apart(job_held, children, count * sizeof(children[0]));

        return true;
}

static bipolar job_first_child(job_entry address_to entry)
{
        for (positive at = 0; at < shell_wait_count; at++)
                if (shell_wait_table[at].job == entry->last)
                        return shell_wait_table[at].pid;

        return entry->group > 0 ? entry->group : entry->last;
}

/*
        One child changed, and what that means for the job holding it.

        A stop is not a death. Filing it in the wait table would let `wait`
        answer for a process that is still there and would take the job off
        the screen while it is stopped on it, so only an exit or a signal is
        handed on.
*/
static fn job_child_changed(bipolar pid, positive status)
{
        bool stopped = (status & 0xff) == 0x7f;
        bool continued = (status & 0xffff) == 0xffff;
        positive at;

        for (at = 0; at < job_count; at++)
                if (job_table[at].last == pid ||
                    (job_table[at].group > 0 &&
                     job_group_of(pid) == job_table[at].group))
                        break;

        if (stopped)
        {
                if (at < job_count && job_table[at].state != JOB_STOPPED)
                {
                        job_table[at].state = JOB_STOPPED;
                        job_table[at].stopped_by = (status >> 8) & 0xff;
                        job_table[at].reported = false;
                        job_mark(job_table[at].number);
                }

                return;
        }

        if (continued)
        {
                if (at < job_count && job_table[at].state == JOB_STOPPED)
                        job_table[at].state = JOB_RUNNING;

                return;
        }

        shell_background_reaped(pid, status);

        // A here-document writer is a child too, and belongs to no job.
        at = job_find_last(pid);

        if (at < job_count)
                job_table[at].status = status;

        for (at = 0; at < job_count; at++)
        {
                job_entry address_to entry = job_table + at;

                if (entry->state == JOB_FINISHED || !job_rows(entry->last) ||
                    job_running_children(entry->last))
                        continue;

                entry->state = JOB_FINISHED;
                entry->reported = false;
        }
}

/*
        Everything the kernel has to say, taken without waiting.

        WUNTRACED and WCONTINUED are what make a stopped job visible to a
        shell that is not waiting for it: without them a control-Z in a
        pipeline stage looks like nothing at all until somebody asks.
*/
fn job_reap()
{
        positive status;
        bipolar pid;

        job_child_news = false;

        while ((pid = system_call_4(syscall(wait4), (positive)-1,
                                    (positive)address_of status,
                                    JOB_NO_HANG | JOB_UNTRACED |
                                        JOB_CONTINUED,
                                    0)) > 0)
                job_child_changed(pid, status);
}

/*
        Sweeping only when there is something to sweep.

        A shell that asks the kernel between commands learns late: a
        substitution reads the table in a fork of this process and can only
        know what the fork already knew, so `$(jobs)` reported a job that had
        finished two commands ago as running. Asking before every command
        instead is a system call per command for as long as anything is in the
        background, which a loop beside a background job pays for every turn.

        SIGCHLD is the answer to both. The handler does nothing but say that
        there is news, so the sweep itself stays where it can safely walk the
        tables, and a shell with nothing to hear makes no call at all.
*/
static fn job_child_arrived(b32 number)
{
        (void)number;

        job_child_news = true;
}

static fn job_child_watch()
{
        if (job_child_watching)
                return;

        job_child_watching = true;
        system_signal_install(SIGNAL_CHILD, (positive)job_child_arrived,
                              SIGNAL_CATCH_FLAGS, SIGNAL_CATCH_RESTORER,
                              null);
}

fn job_notice()
{
        if (!job_child_news || !job_count)
                return;

        job_reap();
}

/*
        What a signal is called when a job ends by one.

        These are the descriptions a person reads next to the job number, and
        they are the C library's rather than the shell's own short names, which
        is why "TERM" appears in `kill -l` and "Terminated" appears here.
*/
static string_address job_signal_words[] = {
    null,          "Hangup",
    "Interrupt",   "Quit",
    "Illegal instruction",
    "Trace/breakpoint trap",
    "Aborted",     "Bus error",
    "Floating point exception",
    "Killed",      "User defined signal 1",
    "Segmentation fault",
    "User defined signal 2",
    "Broken pipe", "Alarm clock",
    "Terminated",  null,
    "Child exited", "Continued",
    "Stopped (signal)",
    "Stopped",     "Stopped (tty input)",
    "Stopped (tty output)",
    "Urgent I/O condition",
    "CPU time limit exceeded",
    "File size limit exceeded",
    "Virtual timer expired",
    "Profiling timer expired",
    "Window changed",
    "I/O possible",
    "Power failure",
    "Bad system call",
};

#define JOB_SIGNAL_WORDS (array_count(job_signal_words))

static positive job_signal_named(positive number, p8 address_to into)
{
        if (number < JOB_SIGNAL_WORDS && job_signal_words[number])
        {
                string_copy(into, job_signal_words[number]);
                return string_length(job_signal_words[number]);
        }

        string_copy(into, "Signal ");
        positive_into_string(into + 7, number);

        return string_length(into);
}

/*
        The status column of a listing.

        A stopped job is spelled twice over: the job's own summary says
        "Stopped", and the per-process detail `jobs -l` prints says which
        signal stopped it. Both are kept because both are what a person sees
        when the two commands are put side by side.
*/
static fn job_status_text(job_entry address_to entry, bool detailed,
                          p8 address_to into)
{
        positive code;

        if (entry->state == JOB_RUNNING)
        {
                string_copy(into, "Running");
                return;
        }

        if (entry->state == JOB_STOPPED)
        {
                positive by = entry->stopped_by;

                if (detailed && by && by != JOB_SIGNAL_STOP_KEY)
                {
                        job_signal_named(by, into);
                        return;
                }

                string_copy(into, "Stopped");
                return;
        }

        if ((entry->status & 0x7f) && (entry->status & 0xff) != 0x7f)
        {
                positive length = job_signal_named(entry->status & 0x7f, into);

                if (entry->status & 0x80)
                        string_copy(into + length, " (core dumped)");

                return;
        }

        code = (entry->status >> 8) & 0xff;

        if (!code)
        {
                string_copy(into, "Done");
                return;
        }

        string_copy(into, "Exit ");
        positive_into_string(into + 5, code);
}

static string_address job_mark_of(job_entry address_to entry)
{
        if (entry->number == job_current)
                return (string_address) "+";

        if (entry->number == job_previous)
                return (string_address) "-";

        return (string_address) " ";
}

// One letter, as a string, because the format writer knows s, p, b and f and
// nothing else -- and a diagnostic that drops the letter it is complaining
// about is worse than no diagnostic.
static string_address job_letter(p8 letter)
{
        static p8 held[2];

        held[0] = letter;
        held[1] = end;

        return held;
}

/*
        One job, on one line.

        An ampersand is not part of what was typed: it is how a listing says
        the job is still in the background, so it belongs to jobs that are
        running there and to no others.
*/
static fn job_line(writer write, job_entry address_to entry, bool detailed)
{
        p8 status[64];

        string_format(write, "[%p]%s", entry->number, job_mark_of(entry));

        if (detailed)
                string_format(write, " %b ", job_first_child(entry));
        else
                string_format(write, "  ");

        job_status_text(entry, detailed, status);
        string_to_field(write, status, JOB_STATUS_WIDTH, ' ', true);

        string_format(write, "%s", entry->text ? (string_address)entry->text
                                               : (string_address) "");

        if (entry->state == JOB_RUNNING && entry->background)
                string_format(write, " &");

        string_format(write, "\n");
}

/*
        What has changed since anybody last looked.

        A terminal is told as soon as the shell is between commands; a script
        is told nothing, because its output is somebody else's input and a
        line about job 1 in the middle of it is a bug. What a script gets
        instead is the same notice when it asks, through `jobs`.
*/
fn job_report()
{
        positive at = 0;

        if (!shell_is_interactive)
                return;

        while (at < job_count)
        {
                job_entry address_to entry = job_table + at;

                if (entry->reported)
                {
                        at++;
                        continue;
                }

                job_line(log, entry, false);
                entry->reported = true;

                if (entry->state != JOB_FINISHED)
                {
                        at++;
                        continue;
                }

                shell_wait_drop(entry->last);
                job_drop_at(at);
        }

        log_flush();
}

/*
        The job an operand names.

        Bash accepts six spellings and so does this: a number, the current job
        as `%%` or `%+`, the previous one as `%-`, a command prefix, and a
        substring after `%?`. A prefix or substring matching two jobs is
        ambiguous rather than one of them, because guessing which of two
        running commands to kill is not a service.
*/
#define JOB_SPEC_NONE 0
#define JOB_SPEC_FOUND 1
#define JOB_SPEC_UNKNOWN 2
#define JOB_SPEC_AMBIGUOUS 3

static positive job_specified(string_address word, positive address_to found)
{
        string_address text = word;
        positive matches = 0;
        positive number = 0;
        bool substring = false;

        address_to found = job_count;

        if (!word || !string_get(word))
        {
                address_to found = job_find_number(job_current);
                return job_current && address_to found < job_count
                           ? JOB_SPEC_FOUND
                           : JOB_SPEC_UNKNOWN;
        }

        if (string_get(text) == '%')
                text++;

        if (!string_get(text) || string_get(text) == '%' ||
            string_get(text) == '+')
        {
                address_to found = job_find_number(job_current);
                return job_current && address_to found < job_count
                           ? JOB_SPEC_FOUND
                           : JOB_SPEC_UNKNOWN;
        }

        if (string_get(text) == '-' && !string_get(text + 1))
        {
                address_to found = job_find_number(job_previous);
                return job_previous && address_to found < job_count
                           ? JOB_SPEC_FOUND
                           : JOB_SPEC_UNKNOWN;
        }

        if (string_digits_exact(text, address_of number))
        {
                address_to found = job_find_number(number);
                return address_to found < job_count ? JOB_SPEC_FOUND
                                                    : JOB_SPEC_UNKNOWN;
        }

        if (string_get(text) == '?')
        {
                substring = true;
                text++;
        }

        for (positive at = 0; at < job_count; at++)
        {
                string_address have = job_table[at].text
                                          ? (string_address)job_table[at].text
                                          : (string_address) "";
                bool hit =
                    substring
                        ? string_find(have, text) != null
                        : !string_compare_max(have, text,
                                              string_length(text));

                if (!hit)
                        continue;

                matches++;
                address_to found = at;
        }

        if (matches == 1)
                return JOB_SPEC_FOUND;

        address_to found = job_count;

        return matches ? JOB_SPEC_AMBIGUOUS : JOB_SPEC_UNKNOWN;
}

static b32 job_specified_complaint(string_address command,
                                   string_address word, positive answer)
{
        if (answer == JOB_SPEC_AMBIGUOUS)
        {
                string_format(shell_diagnostic, "%s: %s: ambiguous job spec\n",
                              command, word);
                return 1;
        }

        string_format(shell_diagnostic, "%s: %s: no such job\n", command,
                      word ? word : (string_address) "current");

        return 1;
}

// Job control that was never turned on has no job to name, and saying so is
// more use than a listing that is empty for a different reason.
static bool job_control_missing(writer write, string_address command)
{
        (void)write;

        if (job_monitor())
                return false;

        string_format(shell_diagnostic, "%s: no job control\n", command);
        shell_answer(1);

        return true;
}

fn shell_jobs(writer write, string_address input)
{
        shell_option_walk walk = {1};
        bool detailed = false;
        bool identifiers = false;
        bool running_only = false;
        bool stopped_only = false;
        bool changed_only = false;
        p8 letter;

        (void)input;

        while (shell_option_letter(address_of walk, address_of letter))
                switch (letter)
                {
                case 'l':
                        detailed = true;
                        break;
                case 'p':
                        identifiers = true;
                        break;
                case 'r':
                        running_only = true;
                        break;
                case 's':
                        stopped_only = true;
                        break;
                case 'n':
                        changed_only = true;
                        break;
                default:
                        string_format(shell_diagnostic,
                                      "jobs: -%s: invalid option\n",
                                      job_letter(letter));
                        return shell_answer(2);
                }

        job_reap();

        if (walk.index < shell_argc)
        {
                b32 answer = 0;

                for (positive at = walk.index; at < shell_argc; at++)
                {
                        positive found;
                        positive told = job_specified(shell_argv[at],
                                                      address_of found);

                        if (told != JOB_SPEC_FOUND)
                        {
                                answer = job_specified_complaint(
                                    (string_address) "jobs", shell_argv[at],
                                    told);
                                continue;
                        }

                        if (identifiers)
                                string_format(write, "%b\n",
                                              job_first_child(job_table +
                                                              found));
                        else
                                job_line(write, job_table + found, detailed);

                        job_table[found].reported = true;
                }

                return shell_answer(answer);
        }

        for (positive at = 0; at < job_count;)
        {
                job_entry address_to entry = job_table + at;
                bool show = true;

                if (running_only && entry->state != JOB_RUNNING)
                        show = false;
                if (stopped_only && entry->state != JOB_STOPPED)
                        show = false;
                if (changed_only && entry->reported)
                        show = false;

                if (show && identifiers)
                        string_format(write, "%b\n", job_first_child(entry));
                else if (show)
                        job_line(write, entry, detailed);

                if (show)
                        entry->reported = true;

                /* A finished job is news exactly once. Reporting it is also
                   forgetting it, which is why `jobs` twice over shows it and
                   then does not. */
                if (show && entry->state == JOB_FINISHED)
                {
                        shell_wait_drop(entry->last);
                        job_drop_at(at);
                        continue;
                }

                at++;
        }

        shell_answer(0);
}

fn shell_fg(writer write, string_address input)
{
        positive found;
        positive told;
        job_entry address_to entry;

        (void)input;

        if (job_control_missing(write, (string_address) "fg"))
                return;

        job_reap();

        told = job_specified(shell_argc > 1 ? shell_argv[1] : null,
                             address_of found);

        if (told != JOB_SPEC_FOUND)
                return shell_answer(job_specified_complaint(
                    (string_address) "fg",
                    shell_argc > 1 ? shell_argv[1] : null, told));

        entry = job_table + found;
        entry->background = false;
        job_mark(entry->number);

        string_format(write, "%s\n",
                      entry->text ? (string_address)entry->text
                                  : (string_address) "");
        log_flush();

        if (entry->state == JOB_STOPPED)
        {
                entry->state = JOB_RUNNING;
                job_signal(entry->group > 0 ? -entry->group : entry->last,
                           JOB_SIGNAL_CONTINUE);
        }

        shell_answer(job_wait_foreground(entry->number));
}

fn shell_bg(writer write, string_address input)
{
        b32 answer = 0;
        positive at = 1;

        (void)input;

        if (job_control_missing(write, (string_address) "bg"))
                return;

        job_reap();

        do
        {
                string_address word = at < shell_argc ? shell_argv[at] : null;
                positive found;
                positive told = job_specified(word, address_of found);
                job_entry address_to entry;

                if (told != JOB_SPEC_FOUND)
                {
                        answer = job_specified_complaint(
                            (string_address) "bg", word, told);
                        continue;
                }

                entry = job_table + found;

                if (entry->state == JOB_RUNNING && entry->background)
                {
                        string_format(shell_diagnostic,
                                      "bg: job %p already in background\n",
                                      entry->number);
                        answer = 1;
                        continue;
                }

                entry->state = JOB_RUNNING;
                entry->background = true;
                job_mark(entry->number);
                job_signal(entry->group > 0 ? -entry->group : entry->last,
                           JOB_SIGNAL_CONTINUE);

                string_format(write, "[%p]%s %s &\n", entry->number,
                              job_mark_of(entry),
                              entry->text ? (string_address)entry->text
                                          : (string_address) "");
        } while (++at < shell_argc);

        shell_answer(answer);
}

/*
        A job the shell stops keeping.

        Forgetting is the whole of it: the row goes, the wait table's rows go
        with it, and the process carries on with nobody left to report for it.
        `-h` is the exception and keeps the row, because what it asks for is
        not forgetting but an exemption from the hangup a leaving shell sends.
*/
fn shell_disown(writer write, string_address input)
{
        shell_option_walk walk = {1};
        bool all = false;
        bool running_only = false;
        bool keep = false;
        b32 answer = 0;
        p8 letter;

        (void)write;
        (void)input;

        while (shell_option_letter(address_of walk, address_of letter))
                switch (letter)
                {
                case 'a':
                        all = true;
                        break;
                case 'r':
                        running_only = true;
                        break;
                case 'h':
                        keep = true;
                        break;
                default:
                        string_format(shell_diagnostic,
                                      "disown: -%s: invalid option\n",
                                      job_letter(letter));
                        return shell_answer(2);
                }

        job_reap();

        if (all || running_only || walk.index >= shell_argc)
        {
                for (positive at = 0; at < job_count;)
                {
                        if (running_only &&
                            job_table[at].state != JOB_RUNNING)
                        {
                                at++;
                                continue;
                        }

                        if (!all && !running_only &&
                            job_table[at].number != job_current)
                        {
                                at++;
                                continue;
                        }

                        if (keep)
                        {
                                job_table[at].nohup = true;
                                at++;
                                continue;
                        }

                        shell_wait_drop(job_table[at].last);
                        job_drop_at(at);
                }

                return shell_answer(0);
        }

        for (positive at = walk.index; at < shell_argc; at++)
        {
                positive found;
                positive told = job_specified(shell_argv[at],
                                              address_of found);

                if (told != JOB_SPEC_FOUND)
                {
                        answer = job_specified_complaint(
                            (string_address) "disown", shell_argv[at], told);
                        continue;
                }

                if (keep)
                {
                        job_table[found].nohup = true;
                        continue;
                }

                shell_wait_drop(job_table[found].last);
                job_drop_at(found);
        }

        shell_answer(answer);
}

/*
        The shell, stopped by its own hand.

        Only a shell that has job control has anywhere to be stopped back to:
        without it there is no other foreground group to hand the terminal to
        and nobody who would ever continue this one.
*/
fn shell_suspend(writer write, string_address input)
{
        (void)write;
        (void)input;

        if (!job_monitor() || !shell_is_interactive)
        {
                string_format(shell_diagnostic,
                              "suspend: cannot suspend: no job control\n");
                return shell_answer(1);
        }

        log_flush();
        job_signal(-job_shell_group, JOB_SIGNAL_STOP);

        shell_answer(0);
}

/*
        A foreground job, waited for.

        The wait is the one every foreground command gets, except that a stop
        is an answer too: the job stays in the table, the shell takes the
        terminal back, and the number the caller reads is the one POSIX gives
        a command that stopped.
*/
static b32 job_wait_foreground(positive number)
{
        positive at = job_find_number(number);
        b32 status = shell_status;
        bipolar last;

        if (at >= job_count)
                return status;

        last = job_table[at].last;
        job_terminal_give(job_table[at].group);

        while (true)
        {
                positive raw = 0;
                bipolar got;

                at = job_find_number(number);

                if (at >= job_count || job_table[at].state != JOB_RUNNING ||
                    !job_running_children(last))
                        break;

                trap_wait_restarting(false);
                got = system_call_4(syscall(wait4), (positive)-1,
                                    (positive)address_of raw, JOB_UNTRACED, 0);
                trap_wait_restarting(true);

                if (got == -4)
                {
                        if (trap_waiting())
                                break;

                        continue;
                }

                if (got <= 0)
                        break;

                job_child_changed(got, raw);
        }

        job_terminal_give(job_shell_group);

        at = job_find_number(number);

        if (at >= job_count)
                return status;

        if (job_table[at].state == JOB_STOPPED)
        {
                job_table[at].reported = true;
                job_line(log, job_table + at, false);
                log_flush();

                return 128 + (b32)job_table[at].stopped_by;
        }

        {
                bool interrupted;

                status = shell_wait_one(last, address_of interrupted);
                at = job_find_number(number);

                if (at < job_count)
                        job_drop_at(at);
        }

        return status;
}

/*
        A foreground child under the monitor, waited for.

        No job is made unless one is needed. A command that runs to the end
        was never a job: it took no number, `jobs` never mentioned it, and the
        next background command is still [1]. The number is taken at the
        moment it stops, which is also the moment it becomes something `fg`
        can name.
*/
static b32 job_foreground_wait(bipolar child, bipolar group, b32 node)
{
        positive raw = 0;
        positive stopped_by;
        positive number;
        b32 answer;

        job_terminal_give(group);

        while (true)
        {
                bipolar got;

                trap_wait_restarting(false);
                got = system_call_4(syscall(wait4), (positive)child,
                                    (positive)address_of raw, JOB_UNTRACED, 0);
                trap_wait_restarting(true);

                if (got == -4)
                {
                        bipolar signal;

                        if (!trap_waiting())
                                continue;

                        signal = trap_pending_number();
                        job_terminal_give(job_shell_group);

                        return signal > 0 ? 128 + (b32)signal : 129;
                }

                if (got < 0)
                {
                        job_terminal_give(job_shell_group);

                        return 1;
                }

                break;
        }

        job_terminal_give(job_shell_group);

        if ((raw & 0xff) != 0x7f)
                return wait_status_code(raw);

        stopped_by = (raw >> 8) & 0xff;
        answer = 128 + (b32)stopped_by;

        if (!job_retain(address_of child, 1, false, false, false))
                return answer;

        number = job_started(address_of child, 1, group, node, false, false);

        if (!number)
                return answer;

        {
                positive at = job_find_number(number);

                job_table[at].state = JOB_STOPPED;
                job_table[at].stopped_by = stopped_by;
                job_table[at].reported = true;
                job_line(log, job_table + at, false);
                log_flush();
        }

        return answer;
}

/*
        One foreground child under the monitor.

        The spawn device would be quicker and cannot be used here. A spawned
        stage never runs a line of this shell's code, so the only side that
        could put it in a process group of its own is this one -- and by the
        time the request has returned the child may already have exec'd, at
        which point setpgid is refused. Both sides racing to the same answer is
        what makes the group certain, and only a fork has two sides.
*/
static fn job_execute_foreground()
{
        bipolar child;

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                job_group_set(0, 0);
                shell_default(JOB_SIGNAL_STOP_KEY);
                shell_default(JOB_SIGNAL_TTY_INPUT);
                shell_default(JOB_SIGNAL_TTY_OUTPUT);
                shell_thread_instance();
        }

        if (child < 0)
        {
                shell_execute_command();
                return;
        }

        job_group_set(child, child);

        shell_status = job_foreground_wait(child, child, -1);
}

/*
        A utility of this image, under the monitor.

        The argument is the foreground command's above: the group has to be
        raced from both sides and only a fork has two, so the spawn device is
        not used here. The image is already resident, so the child calls the
        utility rather than loading one, and what the fork costs over the
        spawn buys a `sleep` that control-Z can stop.
*/
fn job_execute_tool(positive which)
{
        bipolar child;

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                job_group_set(0, 0);
                trap_default_all();
                shell_default(SIGNAL_INTERRUPT);
                shell_default(SIGNAL_QUIT);
                shell_default(JOB_SIGNAL_STOP_KEY);
                shell_default(JOB_SIGNAL_TTY_INPUT);
                shell_default(JOB_SIGNAL_TTY_OUTPUT);
                exec_child_began();
                program_arguments_use(shell_argv, (b32)shell_argc);
                exit(shell_tool_call(which));
        }

        if (child < 0)
                return shell_answer(1);

        job_group_set(child, child);

        shell_answer(job_foreground_wait(child, child, -1));
}

/*
        kill, once an operand names a job.

        Everything else stays with the utility: the recorded answers about
        signal names, numbers and refusals are its, and a second parser beside
        it would be a second set of them. What the shell adds is the one thing
        a utility in another process cannot know -- what `%1` means, and that
        under job control it means a process group rather than one process.
*/
static bool job_kill_specified()
{
        for (positive at = 1; at < shell_argc; at++)
                if (string_get(shell_argv[at]) == '%')
                        return true;

        return false;
}

fn shell_kill(writer write, string_address input)
{
        bipolar number = 15;
        positive at = 1;
        b32 answer = 0;

        (void)write;
        (void)input;

        if (!job_kill_specified())
        {
                positive2 named = string_hash_33_length(shell_argv[0]);

                if (!shell_tool_run_hashed(shell_argv[0], named))
                        shell_answer(127);

                return;
        }

        while (at < shell_argc)
        {
                string_address word = shell_argv[at];

                if (string_get(word) != '-' || !string_get(word + 1))
                        break;

                if (string_get(word + 1) == '-' && !string_get(word + 2))
                {
                        at++;
                        break;
                }

                if (string_get(word + 1) == 's' && !string_get(word + 2))
                {
                        if (++at >= shell_argc)
                        {
                                string_format(shell_diagnostic,
                                              "kill: -s needs a signal\n");
                                return shell_answer(2);
                        }

                        number = kill_number(shell_argv[at]);
                }
                else
                        number = kill_number(word + 1);

                if (number < 0)
                {
                        string_format(shell_diagnostic,
                                      "kill: invalid signal\n");
                        return shell_answer(2);
                }

                at++;
        }

        if (at >= shell_argc)
        {
                string_format(shell_diagnostic, "kill: no process named\n");
                return shell_answer(2);
        }

        job_reap();

        for (; at < shell_argc; at++)
        {
                string_address word = shell_argv[at];
                bipolar target;
                positive found;
                positive told;

                if (string_get(word) != '%')
                {
                        if (job_signal(string_to_bipolar(word),
                                       (positive)number) < 0)
                        {
                                string_format(shell_diagnostic,
                                              "kill: %s: no such process\n",
                                              word);
                                answer = 1;
                        }

                        continue;
                }

                told = job_specified(word, address_of found);

                if (told != JOB_SPEC_FOUND)
                {
                        answer = job_specified_complaint(
                            (string_address) "kill", word, told);
                        continue;
                }

                target = job_table[found].group > 0
                             ? -job_table[found].group
                             : job_table[found].last;

                if (job_signal(target, (positive)number) < 0)
                {
                        string_format(shell_diagnostic,
                                      "kill: %s: no such job\n", word);
                        answer = 1;
                }
        }

        shell_answer(answer);
}

// A job whose children the wait table no longer owes an answer for is not a
// job any more: somebody waited for it and POSIX says a successful wait
// forgets what it waited for.
static fn job_prune()
{
        for (positive at = 0; at < job_count;)
                if (!job_rows(job_table[at].last))
                        job_drop_at(at);
                else
                        at++;
}

/*
        wait, once there are jobs to name.

        The plain forms are the ones already written beside the wait table and
        stay there. What a job table adds is a `%1` operand, the answer POSIX
        gives for a job that stopped rather than finished, and `-n` -- which is
        not a wait for anybody in particular but for whoever ends first.
*/
static b32 job_wait_job(positive found, string_address into,
                        bool address_to interrupted)
{
        bipolar last = job_table[found].last;
        b32 answer;

        if (into)
                env_set_number(into, (positive)last);

        answer = shell_wait_one(last, interrupted);
        found = job_find_last(last);

        if (found < job_count)
                job_drop_at(found);

        return answer;
}

/*
        The next job to end, whichever it turns out to be.

        Nothing is named, so the wait is for any child at all and the table is
        asked afterwards which job that was. A job that merely stopped has not
        ended, so the wait goes round again -- unless the caller said -f, which
        is the option for wanting the end rather than the next change.
*/
static b32 job_wait_next(bool force, string_address into)
{
        while (true)
        {
                positive raw = 0;
                bipolar got;
                bool interrupted;

                for (positive at = 0; at < job_count; at++)
                        if (job_table[at].state == JOB_FINISHED)
                                return job_wait_job(at, into,
                                                    address_of interrupted);

                if (!force)
                        for (positive at = 0; at < job_count; at++)
                                if (job_table[at].state == JOB_STOPPED &&
                                    !job_table[at].reported)
                                {
                                        job_table[at].reported = true;

                                        return 128 +
                                               (b32)job_table[at].stopped_by;
                                }

                if (!shell_wait_count)
                        return 127;

                trap_wait_restarting(false);
                got = system_call_4(syscall(wait4), (positive)-1,
                                    (positive)address_of raw,
                                    force ? 0 : JOB_UNTRACED, 0);
                trap_wait_restarting(true);

                if (got == -4)
                {
                        bipolar signal;

                        if (!trap_waiting())
                                continue;

                        signal = trap_pending_number();

                        return signal > 0 ? 128 + (b32)signal : 129;
                }

                if (got <= 0)
                        return 127;

                job_child_changed(got, raw);
        }
}

fn job_wait(writer write, string_address input)
{
        bool next = false;
        bool force = false;
        string_address into = null;
        positive first = 1;
        b32 answer = 0;
        bool interrupted = false;

        while (first < shell_argc)
        {
                string_address word = shell_argv[first];
                positive at;

                if (string_get(word) != '-' || !string_get(word + 1))
                        break;

                if (string_get(word + 1) == '-' && !string_get(word + 2))
                {
                        first++;
                        break;
                }

                for (at = 1; string_get(word + at); at++)
                {
                        p8 letter = string_get(word + at);

                        if (letter == 'n')
                                continue;

                        if (letter == 'f')
                                continue;

                        if (letter == 'p')
                                break;

                        string_format(shell_diagnostic,
                                      "wait: -%s: invalid option\n",
                                      job_letter(letter));

                        return shell_answer(2);
                }

                for (at = 1; string_get(word + at); at++)
                {
                        p8 letter = string_get(word + at);

                        if (letter == 'n')
                        {
                                next = true;
                                continue;
                        }

                        if (letter == 'f')
                        {
                                force = true;
                                continue;
                        }

                        if (string_get(word + at + 1))
                        {
                                into = word + at + 1;
                                break;
                        }

                        if (first + 1 >= shell_argc)
                        {
                                string_format(shell_diagnostic,
                                              "wait: -p wants a name\n");

                                return shell_answer(2);
                        }

                        into = shell_argv[++first];
                        break;
                }

                first++;
        }

        job_reap();

        if (next)
                return shell_answer(job_wait_next(force, into));

        if (first >= shell_argc)
        {
                while (true)
                {
                        positive at;

                        for (at = 0; at < job_count; at++)
                                if (job_table[at].state != JOB_STOPPED ||
                                    force)
                                        break;

                        if (at >= job_count)
                                break;

                        answer = job_wait_job(at, into,
                                              address_of interrupted);

                        if (interrupted)
                                return shell_answer(answer);
                }

                /* A retained child that never became a job -- one started
                   before the table could hold it -- is still owed an answer,
                   and the plain wait beside the table is the one that gives
                   it. Only when nothing is stopped: waiting for a stopped
                   process is waiting forever. */
                if (shell_wait_count && !job_count)
                {
                        shell_wait(write, input);
                        return;
                }

                job_prune();

                return shell_answer(0);
        }

        for (positive at = first; at < shell_argc; at++)
        {
                string_address word = shell_argv[at];
                positive found;
                positive pid;

                if (string_get(word) == '%')
                {
                        if (job_specified(word, address_of found) !=
                            JOB_SPEC_FOUND)
                        {
                                string_format(shell_diagnostic,
                                              "wait: %s: no such job\n", word);

                                return shell_answer(127);
                        }
                }
                else
                {
                        if (!string_digits_exact(word, address_of pid) ||
                            pid > (positive)bipolar_max)
                        {
                                string_format(shell_diagnostic,
                                              "wait: Illegal number: %s\n",
                                              word);

                                return shell_answer(2);
                        }

                        found = job_find_last((bipolar)pid);

                        if (found >= job_count)
                        {
                                if (into)
                                        env_set_number(into, pid);

                                answer = shell_wait_one((bipolar)pid,
                                                        address_of interrupted);
                                job_prune();

                                if (interrupted)
                                        break;

                                continue;
                        }
                }

                if (!force && job_table[found].state == JOB_STOPPED)
                {
                        string_format(shell_diagnostic,
                                      "wait: job %p is stopped\n",
                                      job_table[found].number);
                        answer = 128 + (b32)job_table[found].stopped_by;
                        continue;
                }

                answer = job_wait_job(found, into, address_of interrupted);

                if (interrupted)
                        break;
        }

        shell_answer(answer);
}

/*
        A forked foreground child under the monitor.

        A subshell is a job like any other while it is in front: it has its own
        process group, so it has to be given the terminal, and it can stop,
        so the shell has to be able to say so and get its prompt back.
*/
static b32 job_foreground_child(bipolar child, b32 node)
{
        if (child < 0)
                return 1;

        return job_foreground_wait(child, child, node);
}

/*
        The history, and the one store there is of it.

        Two stores would have been the obvious shape, because two things want
        one: the line editor for its arrow keys, and `fc` for something to
        edit. They are not two stores here, and the reason is that they were
        never even two halves of one process. The editor's ring is in the
        terminal emulator -- src/sh/term.c, which draws the screen and
        assembles the line -- and it reaches this shell down a pseudo-terminal
        as finished lines. Nothing in this address space can see it.

        So the store is here, where the shell reads its lines; `history` and
        `fc` are both written against it; and HISTFILE is the only place the
        two ends can ever meet, which is exactly what it is for.
*/
#define HISTORY_DEFAULT 500
#define HISTORY_SLURP 65536

static p8 address_to address_to history_text;
static positive history_text_room;

// How much was taken for each line, because the store gives them back one at
// a time and a mapping has to be returned with the size it was asked for.
static positive address_to history_bytes;
static positive history_bytes_room;

static positive history_used;

// What the oldest line held is numbered. Trimming takes from the front, so
// the numbers go on rising while the store stays the size it was told to be.
static positive history_first = 1;

// How much of the store has already been given to the file, so that -a
// appends what is new rather than everything again.
static positive history_saved;

static PURE positive history_number(string_address name, positive fallback)
{
        string_address value = env_get(name);
        positive number;

        if (!value || !string_get(value) ||
            !string_digits_exact(value, address_of number))
                return fallback;

        return number;
}

static PURE string_address history_file()
{
        string_address path = env_get((const_string) "HISTFILE");

        return path && string_get(path) ? path : null;
}

static fn history_drop_at(positive at)
{
        if (at >= history_used)
                return;

        if (history_text[at])
                memory_free(history_text[at], history_bytes[at]);

        history_used--;

        for (positive step = at; step < history_used; step++)
        {
                history_text[step] = history_text[step + 1];
                history_bytes[step] = history_bytes[step + 1];
        }

        if (!at)
                history_first++;

        if (history_saved > at)
                history_saved--;
}

static fn history_trim()
{
        positive limit = history_number((string_address) "HISTSIZE",
                                        HISTORY_DEFAULT);

        while (history_used > limit)
                history_drop_at(0);
}

static bool history_hold(string_address text, positive length)
{
        p8 address_to copy;

        if (!shell_array_room(history_text, history_text_room,
                              history_used + 1) ||
            !shell_array_room(history_bytes, history_bytes_room,
                              history_used + 1))
                return false;

        copy = (p8 address_to)shell_map(length + 1);

        if (!copy)
                return false;

        memory_copy_apart(copy, text, length);
        copy[length] = end;

        history_text[history_used] = copy;
        history_bytes[history_used] = length + 1;
        history_used++;

        return true;
}

/* History expansion is a reader transform over the same entries history and
   fc use.  These are reusable output workspaces, not another history table. */
static byte_store history_expanded;
static byte_store history_piece;
static byte_store history_changed;
static byte_store history_percent;
static byte_store history_sub_old;
static byte_store history_sub_new;
static positive history_percent_number;
static bool history_percent_known;
static bool history_substitution_known;

#define HISTORY_EXPAND_ERROR (-1)
#define HISTORY_EXPAND_RUN 0
#define HISTORY_EXPAND_PRINT 1

static bool history_store_add(byte_store address_to store,
                              string_address text, positive length)
{
        if (length > positive_max - store->used - 1 ||
            !byte_store_reserve(store, store->used + length + 1, 256))
                return false;

        memory_copy_apart(store->bytes + store->used, text, length);
        store->used += length;
        store->bytes[store->used] = end;
        return true;
}

static bool history_store_byte(byte_store address_to store, p8 value)
{
        return history_store_add(store, address_of value, 1);
}

static PURE bool history_event_end(p8 value)
{
        return !value || value == ' ' || value == '\t' || value == '\n' ||
               value == ':' || value == '=' || value == '(' || value == ')' ||
               value == ';' || value == '&' || value == '|' || value == '-' ||
               value == '\'' || value == '"' || value == '\\';
}

/* `%` names the word containing the last `?text?` match.  Find that word
   with the shell lexer so quote/operator boundaries stay the same as every
   other history word designator. */
static bool history_percent_set(string_address event, string_address wanted,
                                positive wanted_length)
{
        lex_frame frame;
        b32 count;
        bool answer = false;

        history_percent.used = 0;
        lex_nest_enter(address_of frame);
        count = lex_line(event);

        if (count >= 0)
                for (b32 at = 0; at < count; at++)
                        if (lex_tokens[at].kind != LEX_OPERATOR &&
                            memory_search(lex_tokens[at].text,
                                          lex_tokens[at].length, wanted,
                                          wanted_length))
                        {
                                answer = history_store_add(
                                    address_of history_percent,
                                    lex_tokens[at].text,
                                    lex_tokens[at].length);
                                break;
                        }

        lex_nest_leave(address_of frame);
        history_percent_known = answer;
        return answer;
}

static bool history_event_at(string_address bang,
                             string_address address_to after,
                             positive address_to found)
{
        string_address at = bang + 1;
        positive index = history_used;
        positive number = 0;

        if (!history_used)
                goto missing;

        if (string_is(at, '!'))
        {
                index = history_used - 1;
                at++;
        }
        else if (string_is(at, '-') && string_get(at + 1) >= '0' &&
                 string_get(at + 1) <= '9')
        {
                at++;
                while (string_get(at) >= '0' && string_get(at) <= '9')
                {
                        positive digit = string_get(at++) - '0';

                        if (number > (positive_max - digit) / 10)
                                goto missing;
                        number = number * 10 + digit;
                }

                if (!number || number > history_used)
                        goto missing;
                index = history_used - number;
        }
        else if (string_get(at) >= '0' && string_get(at) <= '9')
        {
                while (string_get(at) >= '0' && string_get(at) <= '9')
                {
                        positive digit = string_get(at++) - '0';

                        if (number > (positive_max - digit) / 10)
                                goto missing;
                        number = number * 10 + digit;
                }

                if (number < history_first ||
                    number - history_first >= history_used)
                        goto missing;
                index = number - history_first;
        }
        else if (string_is(at, '?'))
        {
                string_address wanted = ++at;
                string_address close = string_first_of(at, '?');
                positive wanted_length;

                if (close)
                        at = close + 1;
                else
                        while (!history_event_end(string_get(at)))
                                at++;

                wanted_length = (positive)((close ? close : at) - wanted);
                if (!wanted_length)
                        goto missing;

                for (positive look = history_used; look;)
                {
                        look--;
                        if (memory_search(history_text[look],
                                          string_length(history_text[look]),
                                          wanted, wanted_length))
                        {
                                index = look;
                                break;
                        }
                }

                if (index >= history_used ||
                    !history_percent_set(history_text[index], wanted,
                                         wanted_length))
                        goto missing;
                if (index > positive_max - history_first)
                        goto missing;
                history_percent_number = history_first + index;
        }
        else if (string_is(at, '%'))
        {
                if (!history_percent_known)
                        goto missing;
                if (history_percent_number < history_first ||
                    history_percent_number - history_first >= history_used)
                        goto missing;
                index = history_percent_number - history_first;
                at++;
        }
        else if (string_is(at, '^') || string_is(at, '$') ||
                 string_is(at, '*'))
                index = history_used - 1;
        else
        {
                string_address prefix = at;
                positive length;

                while (!history_event_end(string_get(at)))
                        at++;
                length = (positive)(at - prefix);
                if (!length)
                        goto missing;

                for (positive look = history_used; look;)
                {
                        look--;
                        if (!string_compare_max(history_text[look], prefix,
                                                length))
                        {
                                index = look;
                                break;
                        }
                }
        }

        if (index >= history_used)
                goto missing;

        address_to after = at;
        address_to found = index;
        return true;

missing:
        shell_diagnostic("bash: ", 6);
        shell_diagnostic(bang, (positive)(at - bang));
        shell_diagnostic(": event not found\n", 0);
        return false;
}

enum
{
        HISTORY_WORD_ALL,
        HISTORY_WORD_ONE,
        HISTORY_WORD_RANGE,
        HISTORY_WORD_ARGS,
        HISTORY_WORD_LAST,
        HISTORY_WORD_PERCENT,
        HISTORY_WORD_RANGE_PENULTIMATE
};

static bool history_words(string_address event, b32 kind, positive first,
                          positive last)
{
        lex_frame frame;
        b32 count;
        positive words = 0;
        positive written = 0;
        bool answer = true;

        history_piece.used = 0;
        if (kind == HISTORY_WORD_PERCENT)
                return history_percent_known &&
                       history_store_add(address_of history_piece,
                                         history_percent.bytes,
                                         history_percent.used);
        if (kind == HISTORY_WORD_ALL)
                return history_store_add(address_of history_piece, event,
                                         string_length(event));

        lex_nest_enter(address_of frame);
        count = lex_line(event);

        if (count < 0)
        {
                answer = false;
                goto leave;
        }

        for (b32 at = 0; at < count; at++)
                if (lex_tokens[at].kind != LEX_OPERATOR)
                        words++;

        if (kind == HISTORY_WORD_LAST)
                first = last = words ? words - 1 : 0;
        else if (kind == HISTORY_WORD_ARGS)
        {
                first = 1;
                last = words ? words - 1 : 0;
        }
        else if (kind == HISTORY_WORD_RANGE && last == positive_max)
                last = words ? words - 1 : 0;
        else if (kind == HISTORY_WORD_RANGE_PENULTIMATE)
                last = words > 1 ? words - 2 : 0;

        if ((kind == HISTORY_WORD_ONE || kind == HISTORY_WORD_LAST) &&
            first >= words)
                answer = false;
        else if ((kind == HISTORY_WORD_RANGE ||
                  kind == HISTORY_WORD_RANGE_PENULTIMATE) &&
                 (first >= words || last >= words || last < first))
                answer = false;
        else if (kind == HISTORY_WORD_ARGS && words <= 1)
                goto leave;

        if (!answer)
                goto leave;

        words = 0;
        for (b32 at = 0; at < count; at++)
        {
                if (lex_tokens[at].kind == LEX_OPERATOR)
                        continue;

                if (words >= first && words <= last)
                {
                        if (written++ &&
                            !history_store_byte(address_of history_piece, ' '))
                        {
                                answer = false;
                                break;
                        }

                        if (!history_store_add(address_of history_piece,
                                               lex_tokens[at].text,
                                               lex_tokens[at].length))
                        {
                                answer = false;
                                break;
                        }
                }
                words++;
        }

leave:
        lex_nest_leave(address_of frame);
        return answer;
}

static bool history_substitute(string_address old, positive old_length,
                               string_address replacement,
                               positive replacement_length, bool global)
{
        positive copied = 0;
        bool changed = false;

        if (!old_length)
                return false;

        history_changed.used = 0;
        while (copied <= history_piece.used)
        {
                string_address found = (string_address)memory_search(
                    history_piece.bytes + copied,
                    history_piece.used - copied, old, old_length);

                if (!found)
                        break;

                positive at = (positive)(found - history_piece.bytes);
                if (!history_store_add(address_of history_changed,
                                       history_piece.bytes + copied,
                                       at - copied) ||
                    !history_store_add(address_of history_changed,
                                       replacement, replacement_length))
                        return false;

                copied = at + old_length;
                changed = true;
                if (!global)
                        break;
        }

        if (!changed)
                return false;

        if (!history_store_add(address_of history_changed,
                               history_piece.bytes + copied,
                               history_piece.used - copied))
                return false;

        {
                byte_store held = history_piece;

                history_piece = history_changed;
                history_changed = held;
        }
        return true;
}

static bool history_sub_field(byte_store address_to into,
                              string_address address_to at, p8 delimiter,
                              bool replacement, bool closing_required)
{
        string_address step = address_to at;

        into->used = 0;
        while (string_get(step) && !string_is(step, delimiter))
        {
                p8 value = string_get(step++);

                if (value == '\\' && string_get(step) &&
                    (string_is(step, delimiter) || string_is(step, '\\') ||
                     (replacement && string_is(step, '&'))))
                        value = string_get(step++);
                else if (replacement && value == '&')
                {
                        if (!history_store_add(into, history_sub_old.bytes,
                                               history_sub_old.used))
                                return false;
                        continue;
                }

                if (!history_store_byte(into, value))
                        return false;
        }

        if (string_is(step, delimiter))
                step++;
        else if (closing_required)
                return false;

        address_to at = step;
        return true;
}

/* Read and retain one :s expression.  Keeping the normalized old and new
   fields is both smaller and safer than retaining pointers into a reader
   buffer, and gives :& the same substitution without another history copy. */
static bool history_substitution_read(string_address modifier,
                                      string_address address_to after)
{
        p8 delimiter = string_get(modifier + 1);
        string_address at = modifier + 2;

        if (!delimiter ||
            !history_sub_field(address_of history_sub_old, address_of at,
                               delimiter, false, true) ||
            !history_sub_old.used ||
            !history_sub_field(address_of history_sub_new, address_of at,
                               delimiter, true, false))
                return false;

        history_substitution_known = true;
        address_to after = at;
        return true;
}

static bool history_substitution_apply(bool global)
{
        return history_substitution_known &&
               history_substitute(history_sub_old.bytes,
                                  history_sub_old.used,
                                  history_sub_new.bytes,
                                  history_sub_new.used, global);
}

static bool history_piece_path(p8 modifier)
{
        positive start = 0;
        positive length = history_piece.used;
        p8 address_to slash;
        p8 address_to dot;

        if (modifier == 'h' || modifier == 't')
        {
                slash = (p8 address_to)memory_last_of(history_piece.bytes,
                                                      '/', length);
                if (modifier == 'h')
                {
                        if (!slash)
                        {
                                history_changed.used = 0;
                                if (!history_store_add(
                                        address_of history_changed,
                                        (string_address)".", 1))
                                        return false;
                                goto replace;
                        }
                        length = (positive)(slash - history_piece.bytes);
                        if (!length)
                                length = 1;
                }
                else if (slash)
                {
                        start = (positive)(slash + 1 - history_piece.bytes);
                        length -= start;
                }
        }
        else
        {
                positive component;

                slash = (p8 address_to)memory_last_of(history_piece.bytes,
                                                      '/', length);
                component =
                    slash ? (positive)(slash + 1 - history_piece.bytes) : 0;
                dot = (p8 address_to)memory_last_of(
                    history_piece.bytes + component, '.', length - component);
                if (modifier == 'r')
                {
                        if (dot)
                                length = (positive)(dot - history_piece.bytes);
                }
                else
                {
                        if (!dot)
                        {
                                start = length;
                                length = 0;
                        }
                        else
                        {
                                start = (positive)(dot - history_piece.bytes);
                                length -= start;
                        }
                }
        }

        history_changed.used = 0;
        if (!history_store_add(address_of history_changed,
                               history_piece.bytes + start, length))
                return false;

replace:
        {
                byte_store held = history_piece;
                history_piece = history_changed;
                history_changed = held;
        }
        return true;
}

static bool history_quote_one(byte_store address_to into,
                              string_address text, positive length)
{
        if (!history_store_byte(into, '\''))
                return false;

        for (positive at = 0; at < length; at++)
                if (string_get(text + at) == '\'')
                {
                        if (!history_store_add(into,
                                               (string_address)"'\\''", 4))
                                return false;
                }
                else if (!history_store_byte(into, string_get(text + at)))
                        return false;

        return history_store_byte(into, '\'');
}

static bool history_piece_quote(bool split)
{
        positive at = 0;
        bool written = false;

        history_changed.used = 0;
        while (at < history_piece.used)
        {
                positive start = at;

                if (split)
                {
                        while (start < history_piece.used &&
                               (history_piece.bytes[start] == ' ' ||
                                history_piece.bytes[start] == '\t' ||
                                history_piece.bytes[start] == '\n'))
                                start++;
                        at = start;
                        while (at < history_piece.used &&
                               history_piece.bytes[at] != ' ' &&
                               history_piece.bytes[at] != '\t' &&
                               history_piece.bytes[at] != '\n')
                                at++;
                }
                else
                        at = history_piece.used;

                if (start == at && split)
                        break;
                if (written++ &&
                    !history_store_byte(address_of history_changed, ' '))
                        return false;
                if (!history_quote_one(address_of history_changed,
                                       history_piece.bytes + start,
                                       at - start))
                        return false;
        }

        if (!written &&
            !history_quote_one(address_of history_changed,
                               (string_address)"", 0))
                return false;

        {
                byte_store held = history_piece;
                history_piece = history_changed;
                history_changed = held;
        }
        return true;
}

static bool history_word_designator(string_address address_to at,
                                    b32 address_to kind,
                                    positive address_to first,
                                    positive address_to last)
{
        string_address step = address_to at;
        bool colon = string_is(step, ':');
        p8 value = string_get(step + colon);
        positive number = 0;

        if (value != '^' && value != '$' && value != '*' && value != '%' &&
            value != '-' &&
            !(value >= '0' && value <= '9'))
                return true;

        step += colon;
        if (value == '^')
        {
                address_to kind = HISTORY_WORD_ONE;
                address_to first = address_to last = 1;
                step++;
        }
        else if (value == '$')
        {
                address_to kind = HISTORY_WORD_LAST;
                step++;
        }
        else if (value == '*')
        {
                address_to kind = HISTORY_WORD_ARGS;
                step++;
        }
        else if (value == '%')
        {
                address_to kind = HISTORY_WORD_PERCENT;
                step++;
        }
        else if (value == '-')
        {
                address_to kind = HISTORY_WORD_RANGE_PENULTIMATE;
                address_to first = 0;
                step++;

                if (string_get(step) >= '0' && string_get(step) <= '9')
                {
                        positive end_word = 0;

                        while (string_get(step) >= '0' &&
                               string_get(step) <= '9')
                        {
                                positive digit = string_get(step++) - '0';
                                if (end_word > (positive_max - digit) / 10)
                                        return false;
                                end_word = end_word * 10 + digit;
                        }
                        address_to kind = HISTORY_WORD_RANGE;
                        address_to last = end_word;
                }
        }
        else
        {
                while (string_get(step) >= '0' && string_get(step) <= '9')
                {
                        positive digit = string_get(step++) - '0';

                        if (number > (positive_max - digit) / 10)
                                return false;
                        number = number * 10 + digit;
                }

                address_to kind = HISTORY_WORD_ONE;
                address_to first = address_to last = number;
                if (string_is(step, '-'))
                {
                        positive end_word = 0;

                        step++;
                        if (string_is(step, '$'))
                        {
                                end_word = positive_max;
                                step++;
                        }
                        else
                        {
                                if (string_get(step) < '0' ||
                                    string_get(step) > '9')
                                {
                                        address_to kind =
                                            HISTORY_WORD_RANGE_PENULTIMATE;
                                        address_to last = 0;
                                        goto word_done;
                                }
                                while (string_get(step) >= '0' &&
                                       string_get(step) <= '9')
                                {
                                        positive digit =
                                            string_get(step++) - '0';
                                        if (end_word >
                                            (positive_max - digit) / 10)
                                                return false;
                                        end_word = end_word * 10 + digit;
                                }
                        }
                        address_to kind = HISTORY_WORD_RANGE;
                        address_to last = end_word;
                }
                else if (string_is(step, '*'))
                {
                        address_to kind = HISTORY_WORD_RANGE;
                        address_to last = positive_max;
                        step++;
                }
        }

word_done:
        address_to at = step;
        return true;
}

/* Expand one interactive physical line.  The caller remembers the returned
   text, so `history` and `fc` see exactly what was executed. */
b32 history_expand_line(string_address line,
                        string_address address_to expanded)
{
        string_address at = line;
        string_address run = line;
        bool single = false;
        bool double_quote = false;
        bool changed = false;
        bool print_only = false;

        address_to expanded = line;
        if (!shell_histexpand_on() || parse_here_open())
                return HISTORY_EXPAND_RUN;

        history_expanded.used = 0;

        /* The interactive shorthand for the previous event's first
           substitution.  It is the same transform as !!:s, not a separate
           lookup or execution path. */
        if (string_is(line, '^'))
        {
                string_address after = line + 1;

                if (!history_used ||
                    !history_sub_field(address_of history_sub_old,
                                       address_of after, '^', false, true) ||
                    !history_sub_old.used ||
                    !history_sub_field(address_of history_sub_new,
                                       address_of after, '^', true, false))
                        goto bad_event;
                history_substitution_known = true;

                if (!history_words(history_text[history_used - 1],
                                   HISTORY_WORD_ALL, 0, positive_max) ||
                    !history_substitution_apply(false) ||
                    !history_store_add(address_of history_expanded,
                                       history_piece.bytes,
                                       history_piece.used) ||
                    !history_store_add(address_of history_expanded, after,
                                       string_length(after)))
                        goto bad_event;

                address_to expanded = history_expanded.bytes;
                string_format(shell_diagnostic, "%s\n",
                              history_expanded.bytes);
                log_flush();
                return HISTORY_EXPAND_RUN;
        }

        while (string_get(at))
        {
                /* Most input is neither quoting nor history syntax.  Let the
                   shared byte-set scanner skip that run in one architecture-
                   tuned pass instead of paying four scalar branches per
                   ordinary byte. */
                string_address special =
                    string_first_of_set(at, (string_address) "\\\\'\"!");
                p8 value;

                if (!special)
                {
                        at += string_length(at);
                        break;
                }

                at = special;
                value = string_get(at);

                if (!single && value == '\\' && string_get(at + 1))
                {
                        p8 carried = string_get(at + 1);

                        /* Outside quotes a backslash carries every byte. In
                           double quotes it carries only the bytes that the
                           shell itself treats specially, plus ! for history.
                           Either way an escaped quote cannot change this
                           scanner's quote state. */
                        if (!double_quote || carried == '!' ||
                            carried == '"' || carried == '\\' ||
                            carried == '$' || carried == '\n')
                                at += 2;
                        else
                                at++;
                        continue;
                }

                if (value == '\'' && !double_quote)
                {
                        single = !single;
                        at++;
                        continue;
                }

                if (value == '"' && !single)
                {
                        double_quote = !double_quote;
                        at++;
                        continue;
                }

                if (value != '!' || single ||
                    (double_quote && shell_posix_on()) ||
                    !string_get(at + 1) ||
                    string_get(at + 1) == ' ' ||
                    string_get(at + 1) == '\t' ||
                    string_get(at + 1) == '=' ||
                    string_get(at + 1) == '(')
                {
                        at++;
                        continue;
                }

                if (!history_store_add(address_of history_expanded, run,
                                       (positive)(at - run)))
                        goto no_room;

                {
                        string_address event;
                        positive event_at;
                        b32 word_kind = string_is(at + 1, '%')
                                            ? HISTORY_WORD_PERCENT
                                            : HISTORY_WORD_ALL;
                        positive first = 0;
                        positive last = positive_max;

                        if (string_is(at + 1, '#'))
                        {
                                event = at + 2;
                                if (!history_word_designator(
                                        address_of event,
                                        address_of word_kind,
                                        address_of first,
                                        address_of last))
                                        goto bad_event;

                                history_changed.used = 0;
                                if (!history_store_add(
                                        address_of history_changed, line,
                                        (positive)(at - line)) ||
                                    !history_words(history_changed.bytes,
                                                   word_kind, first, last))
                                        goto no_room;
                        }
                        else if (!history_event_at(at, address_of event,
                                                   address_of event_at) ||
                                 !history_word_designator(
                                     address_of event,
                                     address_of word_kind,
                                     address_of first, address_of last) ||
                                 !history_words(history_text[event_at],
                                                word_kind, first, last))
                                goto bad_event;

                        while (string_is(event, ':'))
                        {
                                string_address modifier = event + 1;
                                bool global = false;

                                if (string_is(modifier, 'g') &&
                                    (string_is(modifier + 1, 's') ||
                                     string_is(modifier + 1, '&')))
                                {
                                        global = true;
                                        modifier++;
                                }

                                if (string_is(modifier, 'p'))
                                {
                                        print_only = true;
                                        event = modifier + 1;
                                        continue;
                                }

                                if (string_is(modifier, '&'))
                                {
                                        if (!history_substitution_apply(
                                                global))
                                                goto bad_event;
                                        event = modifier + 1;
                                        continue;
                                }

                                if (string_is(modifier, 'h') ||
                                    string_is(modifier, 't') ||
                                    string_is(modifier, 'r') ||
                                    string_is(modifier, 'e'))
                                {
                                        if (!history_piece_path(
                                                string_get(modifier)))
                                                goto no_room;
                                        event = modifier + 1;
                                        continue;
                                }

                                if (string_is(modifier, 'q') ||
                                    string_is(modifier, 'x'))
                                {
                                        if (!history_piece_quote(
                                                string_is(modifier, 'x')))
                                                goto no_room;
                                        event = modifier + 1;
                                        continue;
                                }

                                if (!string_is(modifier, 's') ||
                                    !string_get(modifier + 1))
                                        goto unsupported_modifier;

                                if (!history_substitution_read(
                                        modifier, address_of event) ||
                                    !history_substitution_apply(global))
                                        goto bad_event;
                        }

                        if (!history_store_add(address_of history_expanded,
                                               history_piece.bytes,
                                               history_piece.used))
                                goto no_room;

                        changed = true;
                        at = event;
                        run = at;
                }
        }

        if (!changed)
                return HISTORY_EXPAND_RUN;

        if (!history_store_add(address_of history_expanded, run,
                               (positive)(at - run)))
                goto no_room;

        address_to expanded = history_expanded.bytes;
        string_format(shell_diagnostic, "%s\n", history_expanded.bytes);
        log_flush();
        return print_only ? HISTORY_EXPAND_PRINT : HISTORY_EXPAND_RUN;

bad_event:
        return HISTORY_EXPAND_ERROR;
unsupported_modifier:
        string_format(shell_diagnostic,
                      "bash: history expansion: unsupported modifier\n");
        log_flush();
        return HISTORY_EXPAND_ERROR;
no_room:
        string_format(shell_diagnostic, "bash: history expansion: no room\n");
        shell_status = 1;
        return HISTORY_EXPAND_ERROR;
}

/*
        Whether a line is worth remembering, which is not the shell's opinion.

        HISTCONTROL and HISTIGNORE are how a person says what their own
        history is for: a password typed after a space, a loop of the same
        command, a `ls` they will never want back. All three are checked
        before the copy is taken, because the point of them is that the line
        never enters the store at all.
*/
static bool history_wanted(string_address text, positive length)
{
        string_address control = env_get((const_string) "HISTCONTROL");
        string_address ignore = env_get((const_string) "HISTIGNORE");
        bool space = false;
        bool dedupe = false;
        bool erase = false;

        if (!length)
                return false;

        for (string_address at = control; at && string_get(at);)
        {
                string_address stop = string_first_of_or_end(at, ':');
                positive span = (positive)(stop - at);

                if (span == 11 && !memory_compare(at, "ignorespace", 11))
                        space = true;
                else if (span == 10 && !memory_compare(at, "ignoredups", 10))
                        dedupe = true;
                else if (span == 9 && !memory_compare(at, "erasedups", 9))
                        erase = true;
                else if (span == 10 && !memory_compare(at, "ignoreboth", 10))
                {
                        space = true;
                        dedupe = true;
                }

                at = string_get(stop) ? stop + 1 : stop;
        }

        if (space && (string_get(text) == ' ' || string_get(text) == '\t'))
                return false;

        if (dedupe && history_used &&
            !string_compare(history_text[history_used - 1], text))
                return false;

        for (string_address at = ignore; at && string_get(at);)
        {
                static p8 address_to pattern;
                static positive pattern_room;
                string_address stop = string_first_of_or_end(at, ':');
                positive span = (positive)(stop - at);

                if (!shell_room((address_any address_to)address_of pattern,
                                address_of pattern_room, span + 1, 1))
                        break;

                memory_copy_apart(pattern, at, span);
                pattern[span] = end;

                if (span && shell_match(pattern, text))
                        return false;

                at = string_get(stop) ? stop + 1 : stop;
        }

        if (erase)
                for (positive at = 0; at < history_used;)
                        if (!string_compare(history_text[at], text))
                                history_drop_at(at);
                        else
                                at++;

        return true;
}

/*
        One line, as it was typed.

        The reader calls this and nothing else does: an eval, a sourced file
        and a trap action are lines this shell wrote for itself, and a history
        of them is a history of the shell rather than of the person.
*/
fn history_remember(string_address line)
{
        positive length;

        if (!line)
                return;

        length = string_length(line);

        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                length--;

        {
                positive at = 0;

                while (at < length && (line[at] == ' ' || line[at] == '\t'))
                        at++;

                if (at == length)
                        return;
        }

        {
                static p8 address_to held;
                static positive held_room;

                if (!shell_room((address_any address_to)address_of held,
                                address_of held_room, length + 1, 1))
                        return;

                memory_copy_apart(held, line, length);
                held[length] = end;

                if (!history_wanted(held, length))
                        return;

                history_hold(held, length);
        }

        history_trim();
}

// The whole of a file, however long it is. A history file is lines, and a
// reader that stopped at a fixed size would silently lose the oldest of them.
static p8 address_to history_slurp(string_address path,
                                   positive address_to length)
{
        static p8 address_to held;
        static positive held_room;
        positive used = 0;
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ);

        if (handle < 0)
                return null;

        for (;;)
        {
                bipolar got;

                if (!shell_room((address_any address_to)address_of held,
                                address_of held_room,
                                used + HISTORY_SLURP + 1, 1))
                        break;

                got = system_read_retry(handle, held + used, HISTORY_SLURP);

                if (got <= 0)
                        break;

                used += (positive)got;
        }

        system_close(handle);

        if (!held)
                return null;

        held[used] = end;
        address_to length = used;

        return held;
}

static positive history_read(string_address path, positive skip)
{
        positive length = 0;
        p8 address_to text = history_slurp(path, address_of length);
        positive at = 0;
        positive seen = 0;

        if (!text)
                return 0;

        while (at < length)
        {
                positive stop = at;

                while (stop < length && text[stop] != '\n')
                        stop++;

                if (stop > at && seen++ >= skip)
                        history_hold(text + at, stop - at);

                at = stop + 1;
        }

        history_trim();

        return seen;
}

static bool history_write(string_address path, positive from, bool append)
{
        bipolar handle = system_open_at_mode(
            AT_FDCWD, path, append ? FILE_APPEND : FILE_WRITE, 0600);

        if (handle < 0)
        {
                string_format(shell_diagnostic, "history: %s: cannot write\n",
                              path);
                return false;
        }

        for (positive at = from; at < history_used; at++)
        {
                system_write_all((positive)handle, history_text[at],
                                 string_length(history_text[at]));
                system_write_all((positive)handle, "\n", 1);
        }

        system_close(handle);
        history_saved = history_used;

        return true;
}

// What was there before this session, at a terminal and nowhere else: a
// script's history is a history nobody will ever read back.
fn history_start()
{
        string_address path;

        if (!shell_is_interactive)
                return;

        path = history_file();

        if (path)
                history_read(path, 0);

        history_saved = history_used;
}

fn history_leaving()
{
        string_address path;
        positive limit;

        /* A subshell of an interactive shell is still interactive, and it did
           not read any of these lines: letting it write the file would have a
           `( exit )` decide what the session remembers. */
        if (!shell_is_interactive || exec_forked)
                return;

        path = history_file();

        if (!path)
                return;

        limit = history_number((string_address) "HISTFILESIZE",
                               HISTORY_DEFAULT);

        while (history_used > limit)
                history_drop_at(0);

        history_write(path, 0, false);
}

/*
        A line of a listing, in whichever of the two shapes was asked for.

        `history` right-justifies the number in five columns and follows it
        with two spaces; `fc -l` writes the number, a tab and a space, and
        `fc -ln` writes the tab and space with no number in front. They are
        not the same layout and never were, so both are here rather than one
        of them being made to stand for the other.
*/
static fn history_listed(writer write, positive at)
{
        p8 shown[24];

        positive_into_string(shown, history_first + at);
        writer_field(write, shown, string_length(shown), 5, ' ', false);
        string_format(write, "  %s\n", history_text[at]);
}

static fn history_listed_fc(writer write, positive at, bool numbered)
{
        if (numbered)
                string_format(write, "%p", history_first + at);

        string_format(write, "\t %s\n", history_text[at]);
}

fn shell_history(writer write, string_address input)
{
        positive show = history_used;
        positive at = 1;
        string_address path = history_file();

        (void)input;

        while (at < shell_argc && string_get(shell_argv[at]) == '-' &&
               string_get(shell_argv[at] + 1))
        {
                p8 letter = string_get(shell_argv[at] + 1);
                string_address named = at + 1 < shell_argc ? shell_argv[at + 1]
                                                           : null;

                switch (letter)
                {
                case 'c':
                        while (history_used)
                                history_drop_at(history_used - 1);

                        history_first = 1;
                        history_saved = 0;
                        at++;
                        continue;

                case 'd':
                {
                        bipolar offset;

                        if (!named)
                        {
                                string_format(shell_diagnostic,
                                              "history: -d wants an offset\n");
                                return shell_answer(2);
                        }

                        offset = string_to_bipolar(named);

                        if (offset < 0)
                                offset += (bipolar)(history_first +
                                                    history_used);
                        offset -= (bipolar)history_first;

                        if (offset < 0 || (positive)offset >= history_used)
                        {
                                string_format(shell_diagnostic,
                                              "history: %s: not in the"
                                              " history\n",
                                              named);
                                return shell_answer(1);
                        }

                        history_drop_at((positive)offset);

                        return shell_answer(0);
                }

                case 'a':
                case 'n':
                case 'r':
                case 'w':
                {
                        string_address where = named ? named : path;

                        if (!where)
                        {
                                string_format(shell_diagnostic,
                                              "history: no history file\n");
                                return shell_answer(1);
                        }

                        if (letter == 'a')
                                return shell_answer(
                                    history_write(where, history_saved, true)
                                        ? 0
                                        : 1);

                        if (letter == 'w')
                                return shell_answer(
                                    history_write(where, 0, false) ? 0 : 1);

                        if (letter == 'r')
                        {
                                history_read(where, 0);
                                return shell_answer(0);
                        }

                        history_read(where, history_saved);

                        return shell_answer(0);
                }

                case 's':
                {
                        positive used = 0;
                        static p8 address_to joined;
                        static positive joined_room;

                        for (positive word = at + 1; word < shell_argc; word++)
                        {
                                positive length =
                                    string_length(shell_argv[word]);

                                if (!shell_room(
                                        (address_any address_to)address_of joined,
                                        address_of joined_room,
                                        used + length + 2, 1))
                                        return shell_answer(1);

                                if (used)
                                        joined[used++] = ' ';

                                memory_copy_apart(joined + used,
                                                  shell_argv[word], length);
                                used += length;
                                joined[used] = end;
                        }

                        if (used)
                                history_hold(joined, used);

                        return shell_answer(0);
                }

                default:
                        string_format(shell_diagnostic,
                                      "history: -%s: invalid option\n",
                                      job_letter(letter));
                        return shell_answer(2);
                }
        }

        if (at < shell_argc)
        {
                positive wanted;

                if (!string_digits_exact(shell_argv[at], address_of wanted))
                {
                        string_format(shell_diagnostic,
                                      "history: %s: numeric argument"
                                      " required\n",
                                      shell_argv[at]);
                        return shell_answer(1);
                }

                if (wanted < show)
                        show = wanted;
        }

        for (positive line = history_used - show; line < history_used; line++)
                history_listed(write, line);

        shell_answer(0);
}

/*
        fc, which is the history with an editor attached.

        The command being run is not part of what it operates on: `fc -l` is
        entered before it runs, like every other line, and a person asking for
        the last sixteen commands does not mean this one. Bash draws the same
        line, which is why the count below stops one short.
*/
static PURE positive history_range_count()
{
        return history_used ? history_used - 1 : 0;
}

static bool history_locate(string_address word, positive fallback,
                           positive address_to found)
{
        positive count = history_range_count();
        positive digits;
        bipolar offset;

        address_to found = fallback;

        if (!word)
                return true;

        if (string_get(word) == '-' ||
            string_digits_exact(word, address_of digits))
        {
                offset = string_to_bipolar(word);

                if (offset < 0)
                        offset += (bipolar)count;
                else
                        offset -= (bipolar)history_first;

                if (offset < 0)
                        offset = 0;

                if ((positive)offset >= count)
                        offset = count ? (bipolar)count - 1 : 0;

                address_to found = (positive)offset;

                return count != 0;
        }

        for (positive at = count; at;)
        {
                at--;

                if (!string_compare_max(history_text[at], word,
                                        string_length(word)))
                {
                        address_to found = at;
                        return true;
                }
        }

        return false;
}

/*
        A remembered line, run again.

        Nested the way eval nests: the parser is standing in the middle of the
        `fc` that asked for this, and a line fed to it without its own lexer
        storage and parser marks is a second sentence written over the first.
*/
static fn history_run_text(writer write, string_address text)
{
        lex_frame frame;

        string_format(write, "%s\n", text);
        log_flush();

        lex_nest_enter(address_of frame);
        run_lines(text);
        shell_input_end();
        lex_nest_leave(address_of frame);
}

static b32 history_edit(writer write, string_address editor, positive first,
                        positive last)
{
        static p8 path[64];
        static p8 address_to command;
        static positive command_room;
        positive length;
        bipolar handle;

        string_copy(path, "/tmp/mw-fc.");
        positive_into_string(path + 11,
                             (positive)system_call_1(syscall(getpid), 0));

        handle = system_open_at_mode(AT_FDCWD, path, FILE_WRITE, 0600);

        if (handle < 0)
        {
                string_format(shell_diagnostic, "fc: cannot open %s\n", path);
                return 1;
        }

        for (positive at = first; at <= last && at < history_used; at++)
        {
                system_write_all((positive)handle, history_text[at],
                                 string_length(history_text[at]));
                system_write_all((positive)handle, "\n", 1);
        }

        system_close(handle);

        length = string_length(editor) + string_length(path) + 2;

        if (!shell_room((address_any address_to)address_of command,
                        address_of command_room, length, 1))
                return 1;

        string_copy(command, editor);
        string_copy(command + string_length(editor), " ");
        string_copy(command + string_length(editor) + 1, path);

        {
                lex_frame frame;

                lex_nest_enter(address_of frame);
                run_lines(command);
                shell_input_end();
                lex_nest_leave(address_of frame);
        }

        {
                positive text_length = 0;
                p8 address_to text = history_slurp(path, address_of text_length);
                positive at = 0;

                system_call_3(syscall(unlinkat), (positive)(bipolar)AT_FDCWD,
                              (positive)path, 0);

                if (!text)
                        return 1;

                while (at < text_length)
                {
                        positive stop = at;

                        while (stop < text_length && text[stop] != '\n')
                                stop++;

                        text[stop] = end;

                        if (stop > at)
                                history_run_text(write, text + at);

                        at = stop + 1;
                }
        }

        return shell_status;
}

fn shell_fc(writer write, string_address input)
{
        positive count = history_range_count();
        positive at = 1;
        bool listing = false;
        bool numbered = true;
        bool reversed = false;
        bool again = false;
        string_address editor = null;
        string_address replace = null;
        positive first;
        positive last;

        (void)input;

        while (at < shell_argc && string_get(shell_argv[at]) == '-' &&
               string_get(shell_argv[at] + 1))
        {
                p8 letter = string_get(shell_argv[at] + 1);

                if (letter == '-' && !string_get(shell_argv[at] + 2))
                {
                        at++;
                        break;
                }

                if (letter == 'e')
                {
                        if (at + 1 >= shell_argc)
                        {
                                string_format(shell_diagnostic,
                                              "fc: -e wants an editor\n");
                                return shell_answer(2);
                        }

                        editor = shell_argv[++at];

                        // `fc -e -` is how the option spelling asks for the
                        // re-execution `fc -s` asks for by name.
                        if (!string_compare(editor, (string_address) "-"))
                        {
                                again = true;
                                editor = null;
                        }

                        at++;
                        continue;
                }

                for (positive step = 1; string_get(shell_argv[at] + step);
                     step++)
                        switch (string_get(shell_argv[at] + step))
                        {
                        case 'l':
                                listing = true;
                                break;
                        case 'n':
                                numbered = false;
                                break;
                        case 'r':
                                reversed = true;
                                break;
                        case 's':
                                again = true;
                                break;
                        default:
                                string_format(shell_diagnostic,
                                              "fc: -%s: invalid option\n",
                                              job_letter(string_get(
                                                  shell_argv[at] + step)));
                                return shell_answer(2);
                        }

                at++;
        }

        if (again && at < shell_argc && string_first_of(shell_argv[at], '='))
                replace = shell_argv[at++];

        if (!count)
        {
                // Nothing to work on is not a failure when nothing was asked
                // for either: a shell with no history lists none and says so
                // by saying nothing.
                if (listing && at >= shell_argc)
                        return shell_answer(0);

                string_format(shell_diagnostic, "fc: no command found\n");

                return shell_answer(1);
        }

        if (!history_locate(at < shell_argc ? shell_argv[at] : null,
                            again ? count - 1
                                  : listing ? (count > 16 ? count - 16 : 0)
                                            : count - 1,
                            address_of first))
        {
                string_format(shell_diagnostic, "fc: %s: no such command\n",
                              shell_argv[at]);
                return shell_answer(1);
        }

        if (at < shell_argc)
                at++;

        if (!history_locate(at < shell_argc ? shell_argv[at] : null,
                            again ? first : listing ? count - 1 : first,
                            address_of last))
        {
                string_format(shell_diagnostic, "fc: %s: no such command\n",
                              shell_argv[at]);
                return shell_answer(1);
        }

        if (last < first)
        {
                positive held = first;

                first = last;
                last = held;
                reversed = !reversed;
        }

        if (listing)
        {
                if (reversed)
                        for (positive line = last + 1; line > first;)
                                history_listed_fc(write, --line, numbered);
                else
                        for (positive line = first; line <= last; line++)
                                history_listed_fc(write, line, numbered);

                return shell_answer(0);
        }

        if (again)
        {
                static p8 address_to built;
                static positive built_room;
                string_address text = history_text[first];

                if (!replace)
                {
                        history_run_text(write, text);
                        return;
                }

                {
                        string_address split = string_first_of(replace, '=');
                        positive old_length = (positive)(split - replace);
                        string_address new_text = split + 1;
                        string_address where;
                        positive prefix;

                        /* What is being replaced is the front of the operand,
                           which is not a string of its own -- its equals sign
                           is still attached. Comparing that many bytes at
                           each position finds it without the operand having
                           to be cut up first. */
                        for (where = text; string_get(where); where++)
                                if (!string_compare_max(where, replace,
                                                        old_length))
                                        break;

                        if (!old_length || !string_get(where))
                        {
                                history_run_text(write, text);
                                return;
                        }

                        prefix = (positive)(where - text);

                        if (!shell_room(
                                (address_any address_to)address_of built,
                                address_of built_room,
                                string_length(text) + string_length(new_text) +
                                    1,
                                1))
                                return shell_answer(1);

                        memory_copy_apart(built, text, prefix);
                        string_copy(built + prefix, new_text);
                        string_copy(built + prefix + string_length(new_text),
                                    where + old_length);
                        history_run_text(write, built);
                }

                return;
        }

        if (!editor)
                editor = env_get((const_string) "FCEDIT");

        if (!editor || !string_get(editor))
                editor = env_get((const_string) "EDITOR");

        if (!editor || !string_get(editor))
                editor = (string_address) "ed";

        shell_answer(history_edit(write, editor, first, last));
}

static string_address exec_arena_copy(string_address text)
{
        positive length = string_length(text) + 1;
        string_address into = shell_store_take(address_of exec_store, length);

        if (!into)
                return exec_nothing;

        memory_copy(into, text, length);

        return into;
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
static b32 exec_redirect_status;
/* Shared with for/select list expansion below. Redirect fields are consumed
   before execution reaches another node, so one retained pointer table serves
   both without another growth path. */
static string_address address_to exec_fields;
static positive exec_fields_room;

#define F_DUPFD_CLOEXEC 1030

// The longest name a coprocess pair may be called, which is what the NAME_PID
// buffer beside it is sized from.
#define EXEC_COPROC_NAME 128
// A save may not occupy a descriptor this command is going to redirect. An
// open duplication source is occupied already; a closed one is detected later
// by exec_saved_fd_is, so neither kind needs to force every save above it.
static PURE bool exec_redirect_target_is(parse_node address_to node, b32 fd)
{
        for (b32 at = 0; at < node->redirect_count; at++)
        {
                parse_redirect address_to want = parse_redirects + node->redirect + at;

                if (want->fd == fd ||
                    ((want->op == OP_ANDGREAT || want->op == OP_ANDDGREAT) && fd == 2))
                        return true;
        }

        return false;
}

static PURE bool exec_saved_fd_is(b32 fd)
{
        for (b32 at = 0; at < exec_save_count; at++)
                if (exec_saves[at].saved == fd)
                        return true;

        return false;
}

static bipolar exec_save_duplicate(b32 fd, parse_node address_to node, b32 floor)
{
        for (;;)
        {
                bipolar saved = system_call_3(syscall(fcntl), fd,
                                               F_DUPFD_CLOEXEC, floor);

                if (saved < 0)
                        return saved;

                if (!exec_redirect_target_is(node, (b32)saved))
                        return saved;

                system_close(saved);

                if (saved >= 0x7ffffffe)
                        return -1;

                floor = (b32)saved + 1;
        }
}

static bool exec_save_fd(b32 fd, parse_node address_to node)
{
        bipolar saved;
        bool closed = false;

        if (exec_save_count >= REDIRECT_SAVE_MAX)
        {
                string_format(exec_error, "Too many redirections\n");
                return false;
        }

        saved = exec_save_duplicate(fd, node, 10);

        if (saved == -ERROR_BAD_DESCRIPTOR)
                closed = true;
        else if (saved < 0)
        {
                saved = exec_save_duplicate(fd, node, 3);

                if (saved == -ERROR_BAD_DESCRIPTOR)
                        closed = true;
        }

        // EBADF says there was nothing to restore. EINVAL/EMFILE say the
        // original is live but cannot be saved, and must never be treated as
        // a closed descriptor -- doing that closes it during restoration.
        if (!closed && saved < 0)
        {
                string_format(exec_error, "Cannot preserve descriptor %p\n",
                              (positive)fd);
                return false;
        }

        exec_saves[exec_save_count].fd = fd;
        exec_saves[exec_save_count].saved = closed ? -1 : (b32)saved;
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
                        system_duplicate(saved->saved, saved->fd, 0);
                        system_close(saved->saved);
                        continue;
                }

                system_close(saved->fd);
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
                        system_close(saved->saved);
        }
}

// A failed redirect may have closed fd 2 already. Put this redirect's saved
// value back long enough for the diagnostic; normal reverse restoration still
// owns and closes the save afterward. A save belonging to an earlier redirect
// or an enclosing group is deliberately below mark: restoring either would
// bypass `2>/file` or let a diagnostic leak through an outer `2>/dev/null`.
static fn exec_redirect_diagnostic_restore(b32 mark)
{
        for (b32 at = exec_save_count; at > mark; at--)
        {
                exec_saved_fd address_to saved = exec_saves + at - 1;

                if (saved->fd != 2)
                        continue;

                if (saved->saved >= 0)
                        system_duplicate(saved->saved, 2, 0);

                return;
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

        if (!shell_expand_document(token_push_bytes, body, length, false))
                token_overflow |= expand_overflow;

        address_to out = token_storage + start;

        return token_used - start;
}

/*
        Expand a here-document outside the shell process.

        Parameter assignment in a here body belongs to the context executing
        the redirected command, not to the parent shell. More importantly, an
        expansion error ends that context with status two and does not become
        the interactive shell's recoverable line signal. The child writes the
        bounded result back; the parent collects it before making the pipe the
        command will read.
*/
static bool exec_here_expand_isolated(string_address body, positive length,
                                      string_address address_to out,
                                      positive address_to out_length)
{
        positive start = token_used;
        positive filled = 0;
        positive raw_status = 0;
        b32 ends[2];
        bipolar child;

        if (system_pipe(ends, 0) < 0)
                return false;

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                string_address expanded;
                positive made;

                system_close(ends[0]);
                exec_child_began();
                trap_default_all();
                token_overflow = false;

                made = exec_here_expand(body, length, address_of expanded);

                if (token_overflow)
                {
                        string_format(exec_error, "Here-document too long\n");
                        log_flush();
                        system_call_1(syscall(exit_group), 2);
                }

                if (system_write_all(ends[1], expanded, made) != made)
                        system_call_1(syscall(exit_group), 1);

                system_call_1(syscall(exit_group), 0);
        }

        system_close(ends[1]);

        if (child < 0)
        {
                system_close(ends[0]);
                return false;
        }

        //      A here-document is as long as it is. Take another page of
        //      room whenever the last one filled, rather than deciding in
        //      advance how much of it is allowed to arrive.
        for (;;)
        {
                bipolar got;

                if (!token_room(start + filled + 4096))
                {
                        token_overflow = true;
                        break;
                }

                got = system_read_retry(ends[0], token_storage + start + filled,
                                        token_storage_room - start - filled - 1);

                if (got <= 0)
                        break;

                filled += (positive)got;
        }

        system_close(ends[0]);

        system_wait4_retry(child, address_of raw_status, 0, null);

        exec_redirect_status = wait_status_code(raw_status);

        if (exec_redirect_status)
                return false;

        token_used = start + filled;
        address_to out = token_storage + start;
        address_to out_length = filled;

        return true;
}

/*
        A here-document body, reaching the command through a pipe.

        A body that fits in the pipe is written and forgotten about. A longer
        one cannot be: nothing is draining the pipe yet, so the write would
        block against a reader that has not started. A child is left holding
        the writing end instead, which is the shape every shell settles on.
*/
#define PIPE_HOLDS 60000

static bipolar exec_here_pipe(string_address body, positive length)
{
        b32 ends[2];
        bipolar child;

        if (system_pipe(ends, 0) < 0)
                return -1;

        if (length <= PIPE_HOLDS)
        {
                bipolar wrote = length ? system_write_once(ends[1], body, length)
                                       : 0;

                if (wrote == (bipolar)length)
                {
                        system_close(ends[1]);

                        return ends[0];
                }

                // A signal can cut the write short, and a write nobody
                // looked at left the command reading half a body. What is
                // left goes the way a long body does, from a child that can
                // wait for the reader.
                if (wrote > 0)
                {
                        body += wrote;
                        length -= (positive)wrote;
                }
        }

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                system_close(ends[0]);
                trap_default_all();

                system_write_all(ends[1], body, length);

                exit(0);
        }

        system_close(ends[1]);

        if (child < 0)
        {
                system_close(ends[0]);
                return -1;
        }

        return ends[0];
}

// Open an output redirect without replacing an existing regular file when
// noclobber is active. O_EXCL alone is too broad: POSIX still permits devices,
// pipes and sockets, and `set -C; echo x >/dev/null` is ordinary practice.
// The fallback opens without O_TRUNC and inspects that descriptor, so a path
// changing between a separate stat and open cannot make us truncate the file
// we had just decided to protect.
static bipolar exec_output_open(string_address target, bool force)
{
        bipolar opened;

        if (force || !shell_noclobber())
                return system_open_at_mode(AT_FDCWD,
                                     target, FILE_WRITE, 0666);

        opened = system_open_at_mode(AT_FDCWD, target,
                               FILE_WRITE | FILE_EXCLUSIVE, 0666);

        if (opened != -ERROR_EXISTS)
                return opened;

        opened = system_open_at(AT_FDCWD, target, 01);

        if (opened >= 0)
        {
                file_facts facts;

                if (!file_look(opened, "", AT_EMPTY_PATH, address_of facts) ||
                    (facts.mode & MODE_FORMAT) == MODE_FILE)
                {
                        system_close(opened);
                        opened = -ERROR_EXISTS;
                }
        }

        return opened;
}

static bool exec_redirect_apply(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        b32 at;

        /* Bash makes a failed redirection an ordinary failure and continues
           a non-interactive list; dash treats the language error as status
           two. Keep the policy at the one shared redirect boundary: every
           open, dup, save and here-document failure below reaches it. */
        exec_redirect_status = shell_bash_compat ? 1 : 2;

        for (at = 0; at < node->redirect_count; at++)
        {
                parse_redirect address_to want = parse_redirects + node->redirect + at;

                /* A successful prior here-document records zero. Restore the
                   policy before beginning the next independent redirect. */
                exec_redirect_status = shell_bash_compat ? 1 : 2;
                // A here-document's word is its delimiter, which only ever
                // has its quotes taken off: nothing in it runs, in POSIX or
                // in dash, and the body was matched against it when the
                // line was read. Expanding it here ran the substitution in
                // "cat <<$(x)" once per command, for nothing.
                string_address target = want->text;
                bipolar opened = -1;
                b32 redirect_mark = exec_save_count;
                bool both = want->op == OP_ANDGREAT || want->op == OP_ANDDGREAT;

                if (want->op == OP_HERESTRING)
                        target = shell_expand_word(want->text);
                else if (want->op != OP_DLESS)
                {
                        shell_words fields;
                        b32 expanded;

                        shell_words_bind(address_of fields,
                                         address_of exec_fields,
                                         address_of exec_fields_room);
                        expanded = shell_expand_redirect(want->text,
                                                         address_of fields,
                                                         address_of target);
                        if (expanded < 0)
                                return false;
                        if (!expanded)
                        {
                                string_format(exec_error,
                                              "%s: ambiguous redirect\n",
                                              want->text);
                                exec_redirect_status = 1;
                                return false;
                        }
                }

                if (exec_line_aborted())
                        return false;

                /*
                        The descriptor is put aside before anything is opened.

                        open hands back the lowest free descriptor, which is
                        the one being redirected whenever that one was closed.
                        Saving after the open therefore recorded the file that
                        had just arrived as "what was there before", and put it
                        back instead of closing it.
                */
                if (both)
                {
                        if (!exec_save_fd(1, node) || !exec_save_fd(2, node))
                                return false;

                        system_close(1);
                        system_close(2);
                }
                else
                {
                        if (!exec_save_fd(want->fd, node))
                                return false;

                        // Descriptor duplication lands with dup3 below, so
                        // its target stays live until that atomic replacement.
                        // This also preserves the source == target no-op.
                        if (want->op != OP_GREATAND && want->op != OP_LESSAND)
                                system_close(want->fd);
                }

                if (want->op == OP_DLESS)
                {
                        string_address body = want->kept
                                                  ? parse_kept_text + want->body
                                                  : here_text + want->body;
                        positive length = want->body_length;

                        if (!want->raw)
                        {
                                if (!exec_here_expand_isolated(body, length,
                                                               address_of body,
                                                               address_of length))
                                        return false;
                        }

                        opened = exec_here_pipe(body, length);
                }
                else if (want->op == OP_HERESTRING)
                {
                        positive length = string_length(target);
                        p8 address_to body;

                        if (length > positive_max - 2)
                                return false;

                        body = shell_store_take(address_of exec_store,
                                                length + 2);

                        if (!body)
                                return false;

                        memory_copy(body, target, length);
                        body[length++] = '\n';
                        body[length] = end;
                        opened = exec_here_pipe(body, length);
                }
                else if (want->op == OP_GREATAND || want->op == OP_LESSAND)
                {
                        positive source;

                        if (string_is(target, '-') && string_is(target + 1, end))
                        {
                                system_close(want->fd);
                                continue;
                        }

                        if (!string_digits_exact(target, address_of source) ||
                            source >= 0x7fffffff ||
                            exec_saved_fd_is((b32)source))
                        {
                                exec_redirect_diagnostic_restore(redirect_mark);
                                string_format(exec_error,
                                              "Cannot redirect descriptor: %s\n",
                                              target);
                                return false;
                        }

                        if ((b32)source == want->fd)
                                continue;

                        opened = system_duplicate(source, want->fd, 0);
                }
                else if (want->op == OP_LESS)
                        opened = system_open_at(AT_FDCWD,
                                               target, FILE_READ);
                else if (want->op == OP_DGREAT || want->op == OP_ANDDGREAT)
                        opened = system_open_at_mode(AT_FDCWD,
                                               target, FILE_APPEND, 0666);
                else if (want->op == OP_LESSGREAT)
                        opened = system_open_at_mode(AT_FDCWD,
                                               target,
                                               FILE_READ_WRITE | FILE_CREATE, 0666);
                else
                        opened = exec_output_open(target,
                                                  want->op == OP_CLOBBER);

                if (opened < 0)
                {
                        exec_redirect_diagnostic_restore(redirect_mark);
                        string_format(exec_error, "Cannot redirect: %s\n", target);
                        return false;
                }

                log_flush();

                /*
                        &>file is >file followed by 2>&1, as one indivisible
                        redirect. Duplicating one open file description also
                        keeps stdout and stderr on one shared file position.
                        &>> differs only in the open flags above.
                */
                if (both)
                {
                        if (opened != 1)
                        {
                                if (system_duplicate(opened, 1, 0) < 0)
                                {
                                        system_close(opened);
                                        return false;
                                }

                                system_close(opened);
                        }

                        if (system_duplicate(1, 2, 0) < 0)
                                return false;

                        continue;
                }

                // dup3 onto the descriptor it was handed is an error rather
                // than the no-op dup2 makes of it, and open answers with
                // exactly that descriptor when it was the lowest one free.
                if (opened != want->fd)
                {
                        if (system_duplicate(opened, want->fd, 0) < 0)
                        {
                                system_close(opened);
                                exec_redirect_diagnostic_restore(redirect_mark);
                                string_format(exec_error,
                                              "Cannot redirect descriptor: %p\n",
                                              (positive)want->fd);
                                return false;
                        }

                        system_close(opened);
                }
        }

        return true;
}

// What a command whose redirections could not be made answers with: what the
// expansion that aborted the line left, else what the redirect said, else
// plain failure.
static b32 exec_redirect_failed_status()
{
        return exec_line_aborted() ? 2
               : exec_redirect_status ? exec_redirect_status
                                      : 1;
}

typedef struct
{
        p8 address_to name;
        positive name_room;
        positive name_hash;
        positive name_length;
        b32 body;
        // Where this body sits in the kept arenas, so that redefining it can
        // hand the space back rather than leaving it behind.
        parse_marks from;
        parse_marks to;
        // How many calls of it are on the stack. A body being walked is not
        // given back, because the next definition would be written over it.
        positive active;
} exec_function;

static exec_function address_to exec_functions;

/*
        The name of the function in one slot, for compgen.

        By index and into the caller's buffer, because the table lives here
        and the only thing that walks it lives in builtin.c, which is included
        before this file is.
*/
bool exec_function_named(positive slot, p8 address_to into, positive room);
static positive exec_function_room;
static positive exec_function_count;

bool exec_function_named(positive slot, p8 address_to into, positive room)
{
        if (slot >= exec_function_count || !exec_functions[slot].name ||
            exec_functions[slot].name_length >= room)
                return false;

        memory_copy_apart(into, exec_functions[slot].name,
                          exec_functions[slot].name_length);
        into[exec_functions[slot].name_length] = end;

        return true;
}
static positive exec_function_recent = positive_max;

// Whether a name is a function, which direct command substitution asks.
bool exec_function_here_hashed(string_address name, positive2 named);

static PURE inline INLINE bool exec_function_matches(
    positive index, string_address name, positive hash, positive length)
{
        return exec_functions[index].name_hash == hash &&
               exec_functions[index].name_length == length &&
               !memory_compare(exec_functions[index].name, name, length);
}

// The slot of a defined function, or positive_max when the name is not one.
static positive exec_function_slot(string_address name, positive2 named)
{
        positive index;

        if (exec_function_recent < exec_function_count &&
            exec_functions[exec_function_recent].body &&
            exec_function_matches(exec_function_recent, name,
                                  named.x, named.y))
                return exec_function_recent;

        for (index = 0; index < exec_function_count; index++)
        {
                if (index == exec_function_recent || !exec_functions[index].body)
                        continue;

                if (exec_function_matches(index, name, named.x, named.y))
                {
                        exec_function_recent = index;
                        return index;
                }
        }

        return positive_max;
}

static b32 exec_function_find(string_address name, positive2 named)
{
        positive slot = exec_function_slot(name, named);

        return slot == positive_max ? 0 : exec_functions[slot].body;
}

bool exec_function_here_hashed(string_address name, positive2 named)
{
        return exec_function_find(name, named) != 0;
}

bool exec_function_unset(string_address name)
{
        positive2 named = string_hash_33_length(name);
        positive slot;

        for (slot = 0; slot < exec_function_count; slot++)
        {
                if (!exec_function_matches(slot, name, named.x, named.y))
                        continue;

                if (!exec_functions[slot].body)
                        return false;

                // A function unsetting itself is still running its body.
                if (!exec_functions[slot].active)
                        parse_release(address_of exec_functions[slot].from,
                                      address_of exec_functions[slot].to);

                exec_functions[slot].body = 0;
                if (exec_function_recent == slot)
                        exec_function_recent = positive_max;
                return true;
        }

        return false;
}

static b32 exec_node(b32 index);
/* Keep the large central dispatcher off A53 erratum 843419's page-edge
   ADRP/load shape.  This alignment removes a 4 KiB linker veneer island. */
static ARM64_ERRATUM_ALIGN b32 exec_node_kind(b32 index);

static COLD b32 exec_function_no_room(string_address name)
{
        string_format(exec_error, "No room for function: %s\n", name);
        shell_status = 1;

        return 1;
}

static b32 exec_define(b32 index)
{
        string_address name = parse_words[parse_nodes[index].word];
        parse_marks before;
        parse_marks after;
        bool released = false;
        b32 body;
        positive slot;
        positive2 named = string_hash_33_length(name);
        positive name_length = named.y;

        for (slot = 0; slot < exec_function_count; slot++)
        {
                if (exec_function_matches(slot, name, named.x, named.y))
                        break;
        }

        if (slot == exec_function_count)
        {
                // An unset slot has no live tree and can carry a new name --
                // unless a call of what it held is still on the stack and
                // will count itself out of the slot on the way back.
                for (slot = 0; slot < exec_function_count; slot++)
                        if (!exec_functions[slot].body &&
                            !exec_functions[slot].active)
                                break;

                if (slot == exec_function_count)
                {
                        if (exec_function_count == positive_max ||
                            !shell_room((address_any address_to)
                                          address_of exec_functions,
                                        address_of exec_function_room,
                                        exec_function_count + 1,
                                        sizeof(exec_functions[0])))
                                return exec_function_no_room(name);

                        exec_functions[slot].name = null;
                        exec_functions[slot].name_room = 0;
                        exec_functions[slot].name_hash = 0;
                        exec_functions[slot].name_length = 0;
                        exec_functions[slot].body = 0;
                        exec_functions[slot].active = 0;
                        exec_function_count++;
                }

                if (name_length == positive_max ||
                    !shell_room((address_any address_to)
                                  address_of exec_functions[slot].name,
                                address_of exec_functions[slot].name_room,
                                name_length + 1, 1))
                        return exec_function_no_room(name);

                string_copy(exec_functions[slot].name, name);
                exec_functions[slot].name_hash = named.x;
                exec_functions[slot].name_length = named.y;
        }

        /*
                The body this one replaces, given back where it can be.

                The kept arenas are a stack, so only the last definition taken
                can be handed back -- which is the one a script redefining a
                function in a loop keeps making, and the reason such a script
                used to run the arena out and then walk over what was left.

                A body still being run is not handed back at all. It is the
                last one taken when a function redefines itself from inside,
                and the new body was copied over the tree the executor was
                standing in: the rest of the old body ran from the new one.
                That block is left behind instead, which is what a function
                that keeps replacing itself costs.
        */
        if (exec_functions[slot].body && !exec_functions[slot].active)
                released = parse_release(address_of exec_functions[slot].from,
                                         address_of exec_functions[slot].to);

        // Into locals, because a keep that fails must leave the slot saying
        // exactly what it said before: half the new marks beside half the old
        // ones describes a block that was never taken, and giving that back
        // hands away whatever was kept in between.
        parse_mark(address_of before);
        body = parse_keep(parse_nodes[index].right, address_of after);

        if (!body)
        {
                // What was there was written over by the attempt, so saying
                // the name is gone is the honest answer.
                if (released)
                        exec_functions[slot].body = 0;

                return exec_function_no_room(name);
        }

        exec_functions[slot].from = before;
        exec_functions[slot].to = after;
        exec_functions[slot].body = body;
        exec_function_recent = slot;
        shell_status = 0;

        return 0;
}

/*
        FUNCNAME, BASH_SOURCE and BASH_LINENO.

        Three arrays that describe the call stack, innermost first. What a
        call costs is a name pushed on a stack; the arrays themselves are
        built the first time something asks for one, because writing three
        arrays on the way into every function cost more than the call did
        and a script that never mentions them should not pay for them.

        BASH_LINENO answers where each call was written, which is the line
        the reader was on when the frame was pushed. BASH_SOURCE names the
        script, which is the only source this shell has.
*/
// One entry and not two arrays: a call would otherwise ask twice whether
// there was room, and a call is the thing being counted.
typedef struct
{
        string_address name;
        positive line;
} exec_frame;

static exec_frame address_to exec_frames;
static positive exec_frame_room;
static positive exec_frame_count;
static bool exec_frames_published;
static bool exec_frames_standing;

static COLD fn exec_frames_forget()
{
        env_unset("FUNCNAME");
        env_unset("BASH_SOURCE");
        env_unset("BASH_LINENO");
        exec_frames_standing = false;
}

static COLD fn exec_frames_publish()
{
        shell_mark held = shell_store_mark(address_of exec_store);
        string_address address_to walked;
        bipolar address_to lines;

        exec_frames_published = true;

        if (!exec_frame_count)
        {
                exec_frames_forget();
                return;
        }

        exec_frames_standing = true;

        walked = (string_address address_to)shell_store_take(
            address_of exec_store,
            exec_frame_count * sizeof(walked[0]));
        lines = (bipolar address_to)shell_store_take(
            address_of exec_store, exec_frame_count * sizeof(lines[0]));

        if (!walked || !lines)
        {
                shell_store_rewind(address_of exec_store, held);
                return;
        }

        // Innermost first, which is the opposite of the order they were
        // pushed in and the order every script that reads them expects.
        for (positive at = 0; at < exec_frame_count; at++)
        {
                walked[at] = exec_frames[exec_frame_count - at - 1].name;
                lines[at] = (bipolar)exec_frames[exec_frame_count - at - 1].line;
        }

        shell_array_words("FUNCNAME", 8, walked, exec_frame_count);
        shell_array_numbers("BASH_LINENO", 11, lines, exec_frame_count);

        for (positive at = 0; at < exec_frame_count; at++)
                walked[at] = shell_script_name;

        shell_array_words("BASH_SOURCE", 11, walked, exec_frame_count);
        shell_store_rewind(address_of exec_store, held);
}

/*
        caller: where the function this is running in was called from.

        With no operand it is the line and the source; with a number it is
        that many frames further out and the name of the function there as
        well, which is what a script printing a backtrace walks. Outside a
        function there is no frame to describe and the answer is a failure.
*/
PURE positive shell_line_now()
{
        return (positive)exec_line;
}

/*
        Whether a name is in the environment children inherit.

        Every other attribute is a bit of the variable's attribute byte and
        shell_variable_attributes hands that over; this one is a field of the
        entry beside it, and the entries live in the file above this one.
*/
PURE bool shell_variable_exported(const_string name, positive length)
{
        positive found = env_find_span(name, length);

        return found < shell_var_count && shell_vars[found].permanent;
}

fn shell_caller(writer write, string_address input)
{
        positive want = 0;
        bool numbered = shell_argc > 1;
        p8 shown[32];
        positive written;

        if (numbered && !string_digits_exact(shell_argv[1], address_of want))
        {
                string_format(shell_diagnostic, "caller: %s: invalid number\n",
                              shell_argv[1]);
                shell_answer(2);
                return;
        }

        if (want >= exec_frame_count)
        {
                shell_answer(1);
                return;
        }

        written = bipolar_into_string(
            shown, (bipolar)exec_frames[exec_frame_count - want - 1].line);
        write(shown, written);
        write(" ", 1);

        //      The function one frame further out than the one asked about,
        //      which is the shell itself once the frames run out.
        if (numbered)
        {
                string_address named =
                    want + 1 < exec_frame_count
                        ? exec_frames[exec_frame_count - want - 2].name
                        : (string_address) "main";

                write(named, 0);
                write(" ", 1);
        }

        /* Bash calls an unnamed input source NULL. Keep the real path for a
           named script, while stdin and -c must not expose argv[0] as though
           it were the file containing the function. */
        write(string_get(shell_option_flags)
                  ? (string_address) "NULL"
                  : shell_script_name,
              0);
        write("\n", 1);

        shell_answer(0);
}

/*
        Something has asked for one of the three, so now they are made.

        Only a lookup that has already missed reaches this, which is where
        the three of them always miss until a function is running. A name
        that is not one of them costs two length tests, and the caller is
        told whether looking again is worth anything.
*/
COLD bool shell_frames_wanted(const_string name, positive length)
{
        if (exec_frames_published || !exec_frame_count)
                return false;

        if ((length != 8 || memory_compare((address_any)name, "FUNCNAME", 8)) &&
            (length != 11 ||
             (memory_compare((address_any)name, "BASH_SOURCE", 11) &&
              memory_compare((address_any)name, "BASH_LINENO", 11))))
                return false;

        exec_frames_publish();

        return true;
}

static b32 exec_call(positive slot)
{
        b32 body = exec_functions[slot].body;
        positive saved_count = shell_parameter_count;
        positive saved;
        b32 status;
        bool saved_replaced = shell_parameters_replaced;

        if (exec_function_depth == 0x7fffffff)
        {
                string_format(exec_error, "Too deep\n");
                shell_status = 1;
                return 1;
        }

        if (!shell_local_enter())
        {
                shell_status = 1;
                return 1;
        }

        saved = shell_parameters_save();
        if (saved == EXPAND_NO_ROOM ||
            !shell_parameters_restore_prepare(saved_count) ||
            !shell_parameters_set(shell_argv + 1,
                                  shell_argc > 0 ? shell_argc - 1 : 0))
        {
                if (saved != EXPAND_NO_ROOM)
                        shell_parameter_stack_used = saved;
                shell_local_leave();
                string_format(exec_error, "No room for function arguments\n");
                shell_status = 1;
                return 1;
        }

        // By index and not by address: a definition made inside the body can
        // grow the table, and the table may move when it does.
        exec_function_depth++;
        exec_functions[slot].active++;

        if (shell_array_room(exec_frames, exec_frame_room,
                             exec_frame_count + 1))
        {
                // Where the call was written, which is the line of the
                // command making it and not the line the reader is on.
                exec_frames[exec_frame_count].line = (positive)exec_line;
                exec_frames[exec_frame_count++].name =
                    exec_functions[slot].name;
                exec_frames_published = false;
        }

        status = exec_node(body);

        // The function is still standing while its RETURN trap runs, which is
        // what lets the action read FUNCNAME and the status it is returning.
        if (trap_return_here && shell_extra_on(SHELL_EXTRA_FUNCTRACE))
        {
                shell_status = status;
                exec_trap_condition(TRAP_RETURN);
        }

        if (exec_frame_count)
        {
                exec_frame_count--;
                exec_frames_published = false;

                // The three only exist while a function does, and they were
                // only ever made if something read them.
                if (!exec_frame_count && exec_frames_standing)
                        exec_frames_forget();
        }

        exec_functions[slot].active--;
        shell_local_leave();
        exec_function_depth--;

        // return leaves the function and nothing further out.
        if (exec_signal == EXEC_SIGNAL_RETURN)
                exec_signal = EXEC_SIGNAL_NONE;

        if (!shell_parameters_restore(saved, saved_count))
        {
                string_format(exec_error,
                              "No room to restore function arguments\n");
                shell_status = 2;
                /* Restore storage was reserved before the function ran, and
                   the caller's byte/table capacities cannot have shrunk.
                   Continuing would expose the callee's $@ as caller state;
                   make an invariant violation terminal instead. */
                log_flush();
                exit(2);
        }

        shell_parameters_replaced = saved_replaced;
        return status;
}

/*
        set -x: the command about to run, written out.

        After the words are expanded, so what is traced is what runs, and
        before anything is redirected, so a command that sends its own errors
        somewhere does not send the trace there with them. PS4 goes in front,
        which is what a script marking its own depth changes.
*/
#define SHELL_XTRACE ((positive)1 << ('x' - 'a'))

static fn exec_trace(b32 count)
{
        string_address prefix;
        b32 at;

        if (!(shell_options & SHELL_XTRACE))
                return;

        prefix = env_get("PS4");
        exec_error(prefix ? prefix : (string_address) "+ ", 0);

        for (at = 0; at < count; at++)
        {
                if (at)
                        exec_error((string_address) " ", 1);

                exec_error(shell_argv[at], 0);
        }

        exec_error((string_address) "\n", 1);
}

/*
        Where one word of a compound assignment ends.

        a=(x "y z" $v) reaches the executor as a single word, because the
        lexer had to keep the parentheses with the name to know that they
        were not a subshell. Cutting it up again needs only the boundaries --
        a blank that is not inside quoting or a substitution -- since
        everything else about each piece is the ordinary word expansion that
        every other word gets.
*/
static COLD PURE string_address exec_compound_end(string_address at)
{
        while (string_get(at))
        {
                p8 value = string_get(at);
                string_address stop;

                if (value == ' ' || value == '\t' || value == '\n')
                        break;

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '$' && string_is(at + 1, '\''))
                {
                        at = expand_dollar_quoted_run(at);
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        at = expand_quoted_run(at, value);
                        continue;
                }

                if (value == '`')
                {
                        stop = string_first_of(at + 1, '`');
                        at = stop ? stop + 1 : at + 1;
                        continue;
                }

                if (value == '$' &&
                    (string_is(at + 1, '(') || string_is(at + 1, '{')))
                {
                        stop = string_is(at + 1, '(')
                                   ? expand_paren_end(at + 2)
                                   : expand_brace_end(at + 2);
                        at = stop ? stop + 1 : at + 2;
                        continue;
                }

                at++;
        }

        return at;
}

static string_address address_to exec_compound_word;
static positive exec_compound_room;

/*
        NAME=(...) and NAME+=(...).

        Bash replaces an array rather than merging into one, so a plain
        assignment empties it first; an append carries on past the largest
        subscript in use. A piece spelled [key]=value places itself and moves
        the running subscript to just after where it landed, which is what
        makes a=(x [5]=w y) put y at six.
*/
COLD bool shell_compound_assign(string_address name, positive name_length,
                           string_address body, positive body_length,
                           bool append)
{
        shell_mark held = shell_store_mark(address_of exec_store);
        const_string resolved_name;
        positive resolved_length;
        bool keyed;
        string_address at = body;
        string_address stop = body + body_length;
        positive next = 0;
        bool answer = true;

        if (!shell_reference_resolve(name, name_length, address_of resolved_name,
                                     address_of resolved_length))
        {
                shell_store_rewind(address_of exec_store, held);
                return false;
        }

        name = (string_address)resolved_name;
        name_length = resolved_length;
        keyed = (shell_array_attributes(name, name_length) &
                      SHELL_ARRAY_ASSOCIATIVE) != 0;

        if (!shell_variable_attribute_set(
                name, name_length,
                (p8)((keyed ? SHELL_ARRAY_ASSOCIATIVE : SHELL_ARRAY_INDEXED) |
                     SHELL_ARRAY_ASSIGNED),
                0) ||
            (!append && !shell_array_clear(name, name_length)))
        {
                shell_store_rewind(address_of exec_store, held);
                return false;
        }

        if (append && shell_array_length(name, name_length))
                next = shell_array_highest(name, name_length) + 1;

        while (at < stop && answer)
        {
                string_address finish;
                string_address piece;
                string_address value;
                string_address shut = null;
                positive length;
                positive key_length = 0;
                p8 written[32];

                while (at < stop && (string_is(at, ' ') || string_is(at, '\t') ||
                                     string_is(at, '\n')))
                        at++;

                if (at >= stop)
                        break;

                finish = exec_compound_end(at);

                if (finish > stop)
                        finish = stop;

                length = (positive)(finish - at);
                piece = shell_store_take(address_of exec_store, length + 1);

                if (!piece)
                {
                        answer = false;
                        break;
                }

                memory_copy_end(piece, at, length);
                at = finish;

                if (string_is(piece, '['))
                        shut = expand_bracket_end(piece + 1, '[', ']');

                if (shut && string_is(shut + 1, '='))
                {
                        string_address key;
                        positive value_at =
                            (positive)(shut - piece) + 2;

                        // The subscript is resolved against the array it is
                        // being written into, so a keyed one stays bytes and
                        // an indexed one is arithmetic, exactly as it would
                        // be written on its own line.
                        key = shell_expand_subscript(name, name_length,
                                                     piece + 1,
                                                     (positive)(shut - piece) - 1,
                                                     address_of key_length);

                        if (!key)
                        {
                                answer = false;
                                break;
                        }

                        value = shell_expand_assignment(piece, value_at);
                        answer = shell_array_set(name, name_length, key,
                                                 key_length, value + value_at,
                                                 false);

                        if (!keyed)
                                next = array_index_of(key, key_length) + 1;

                        continue;
                }

                if (keyed)
                {
                        string_format(exec_error,
                                      "%s: must use subscript when assigning "
                                      "associative array\n",
                                      name);
                        answer = false;
                        break;
                }

                /* A bare piece is an ordinary word: $v with a space in it
                   becomes two elements, which is what field splitting is. */
                {
                        shell_words fields;
                        positive count;

                        shell_words_bind(address_of fields,
                                         address_of exec_compound_word,
                                         address_of exec_compound_room);
                        count = shell_expand_fields(piece, address_of fields);

                        for (positive one = 0; one < count && answer; one++)
                        {
                                key_length = positive_into_string(written,
                                                                  next++);
                                answer = shell_array_set(name, name_length,
                                                         written, key_length,
                                                         exec_compound_word[one],
                                                         false);
                        }
                }
        }

        shell_store_rewind(address_of exec_store, held);

        return answer;
}

static COLD bool exec_assignment_error(bool fatal)
{
        if (fatal)
        {
                expand_fatal_mode(0);
                return false;
        }

        /* Bash's ordinary mode diagnoses a rejected prefix assignment but
           still invokes the command with the old value. */
        shell_status = 1;
        return true;
}

static bool exec_assign(string_address address_to word_at,
                        positive name_length, positive name_hash, bool append,
                        bool compound, string_address prepared_name,
                        positive prepared_base_length, bool fatal_error)
{
        string_address word = address_to word_at;
        string_address name_end = word + name_length;
        string_address mark = name_end + append;
        string_address subscript;
        positive base_length;
        string_address old;
        string_address made = word;
        bool answer;

        if (string_get(mark) != '=')
                return false;

        address_to name_end = end;

        /*
                A compound value is a list and not a string, so it never went
                through assignment expansion on the way here: each element is
                expanded as the word it is, at the point it is placed.
        */
        if (compound)
        {
                positive length = string_length(mark + 1);

                if (env_assignment_readonly_hashed_span(
                        word, name_length, name_hash))
                {
                        string_format(exec_error, "%s: is read only\n", word);
                        address_to name_end = append ? '+' : '=';
                        return exec_assignment_error(fatal_error);
                }

                answer = shell_compound_assign(word, name_length, mark + 2,
                                              length > 2 ? length - 2 : 0,
                                              append);

                if (!answer && shell_reference_element(
                                   word, name_length, null, null, null, null))
                {
                        string_format(exec_error, "%s: cannot assign\n", word);
                        address_to name_end = append ? '+' : '=';
                        return exec_assignment_error(fatal_error);
                }
                address_to name_end = append ? '+' : '=';

                return answer;
        }

        /*
                An element assignment names its array in front of the
                bracket. That is the name readonly speaks about, and the name
                whose kind decides whether the subscript is arithmetic or
                bytes, so the bracket is closed off while either is asked.
        */
        subscript = string_first_of(word, '[');
        base_length = subscript ? (positive)(subscript - word) : name_length;

        if (subscript)
                address_to subscript = end;

        if (env_assignment_readonly_hashed_span(
                word, base_length,
                subscript ? memory_hash_33(word, base_length) : name_hash))
        {
                string_format(exec_error, "%s: is read only\n", word);

                if (subscript)
                        address_to subscript = '[';

                address_to name_end = append ? '+' : '=';
                return exec_assignment_error(fatal_error);
        }

        if (subscript && !prepared_base_length)
        {
                positive key_length;
                string_address key;

                address_to subscript = '[';
                key = shell_expand_subscript(word, base_length, subscript + 1,
                                             name_length - base_length - 2,
                                             address_of key_length);
                answer = key && shell_array_set(word, base_length, key,
                                                key_length, mark + 1, append);

                if (key && !answer)
                {
                        string_format(exec_error, "%s: cannot assign\n", word);

                        if (shell_reference_element(
                                word, base_length, null, null, null, null))
                        {
                                address_to name_end = append ? '+' : '=';
                                return exec_assignment_error(fatal_error);
                        }
                }

                address_to name_end = append ? '+' : '=';

                return answer;
        }

        if (subscript)
                address_to subscript = '[';

        old = append && prepared_base_length
                  ? shell_array_get(
                        prepared_name, prepared_base_length,
                        prepared_name + prepared_base_length + 1,
                        string_length(prepared_name + prepared_base_length + 1) -
                            1,
                        null)
                  : append ? env_get(word) : null;

        if (append && !old && !prepared_base_length)
                old = shell_reference_element_value(word, name_length, null);

        if (append)
        {
                positive old_length = old ? string_length(old) : 0;
                positive add_length = string_length(mark + 1);
                positive room;

                if (name_length > positive_max - old_length ||
                    name_length + old_length > positive_max - add_length ||
                    name_length + old_length + add_length > positive_max - 2)
                {
                        address_to name_end = '+';
                        return false;
                }

                room = name_length + old_length + add_length + 2;

                made = shell_store_take(address_of exec_store, room);

                if (!made)
                {
                        address_to name_end = '+';
                        return false;
                }

                memory_copy(made, word, name_length);
                made[name_length] = '=';
                if (old_length)
                        memory_copy(made + name_length + 1, old, old_length);
                memory_copy_end(made + name_length + 1 + old_length,
                                mark + 1, add_length);
        }

        if (prepared_base_length)
        {
                string_address key =
                    prepared_name + prepared_base_length + 1;
                positive key_length =
                    string_length(key) - 1;

                answer = shell_array_set(
                    prepared_name, prepared_base_length, key, key_length,
                    append ? made + name_length + 1 : mark + 1, false);
        }
        else
                answer = env_assign_hashed_span(
                    word, name_length, name_hash,
                    append ? made + name_length + 1 : mark + 1);
        address_to name_end = append ? '+' : '=';

        if (!answer && shell_reference_element(
                           word, subscript ? base_length : name_length,
                           null, null, null, null))
        {
                string_format(exec_error, "%s: cannot assign\n", word);
                return exec_assignment_error(fatal_error);
        }

        if (answer && append && !subscript)
                address_to word_at = made;

        return answer;
}

/* The fifteen POSIX names, plus Bash's source spelling while Bash POSIX mode
   is active. Outside that mode Bash deliberately lets functions precede the
   names and restores their prefix assignments; the sh personality retains
   the POSIX policy it has always had. */
static PURE bool exec_special_builtin(string_address name)
{
        static string_address names[] = {
            ":", ".", "break", "continue", "eval", "exec", "exit", "export",
            "readonly", "return", "set", "shift", "times", "trap", "unset",
        };

        if (shell_bash_compat && !shell_posix_on())
                return false;

        if (shell_bash_compat && word_is(name, "source"))
                return true;

        return string_table_find(name, names, sizeof(names[0]),
                                 array_count(names)) <
               array_count(names);
}

/*
        What a name was before the command in front of it changed it.

        The value is copied rather than pointed at: env_set can compact the
        block the value lives in, so a pointer taken before the assignment is
        a pointer into whatever moved there after it. A name that was not set
        is remembered as no value at all, which is what has to be put back.
*/
/*
        One saved element: the key, a nul, then the value.

        Both halves are terminated because putting the array back sets each
        element by name, and both halves have to outlive the clear that
        precedes that -- the live element cells are gone by then.
*/
typedef struct
{
        string_address text;
        positive key_length;
} exec_kept_element;

typedef struct
{
        string_address name;
        positive name_length;
        // How much of the name is the array, when the name is an element.
        positive base_length;
        string_address value;
        bool exported;
        bool promoted;
        /*
                A whole array, saved when the assignment in front of a
                command replaces one. A compound assignment does not add to
                what was there, so putting a scalar value back would leave
                every element it wrote behind it.
        */
        p8 attributes;
        bool compound;
        exec_kept_element address_to elements;
        positive element_count;
} exec_kept_value;

static exec_kept_value address_to exec_promotable;
static b32 exec_promotable_count;

/* An explicit export/readonly on this simple command adopts its prefix
   value. A declaration in a nested function or eval does not adopt the
   caller's environment, so dispatch scopes this pointer, not the variables. */
static bool exec_assignment_promote(const_string name, positive length)
{
        bool found = false;

        if (!shell_bash_compat)
                return false;
        for (b32 at = 0; at < exec_promotable_count; at++)
                if (exec_promotable[at].name_length == length &&
                    !memory_compare(exec_promotable[at].name, name, length))
                {
                        exec_promotable[at].promoted = true;
                        found = true;
                }
        return found;
}

static COLD bool exec_keep_array(exec_kept_value address_to kept)
{
        positive count = shell_array_length(kept->name, kept->name_length);
        shell_array_item address_to items;
        shell_mark held = shell_store_mark(address_of exec_store);
        p8 written[32];

        kept->elements = null;
        kept->element_count = 0;

        if (!count)
                return true;

        items = (shell_array_item address_to)shell_store_take(
            address_of exec_store, count * sizeof(items[0]));
        kept->elements = (exec_kept_element address_to)shell_store_take(
            address_of exec_store, count * sizeof(kept->elements[0]));

        if (!items || !kept->elements)
        {
                shell_store_rewind(address_of exec_store, held);
                return false;
        }

        shell_array_items(kept->name, kept->name_length, items, count);

        for (positive at = 0; at < count; at++)
        {
                string_address key = items[at].key;
                positive key_length = items[at].key_length;
                p8 address_to into;

                if (!key)
                {
                        key_length = bipolar_into_string(
                            written, (bipolar)items[at].index);
                        key = written;
                }

                into = shell_store_take(address_of exec_store,
                                        key_length + items[at].value_length + 2);

                if (!into)
                        return false;

                memory_copy_end(into, key, key_length);
                memory_copy_end(into + key_length + 1, items[at].value,
                                items[at].value_length);
                kept->elements[at].text = into;
                kept->elements[at].key_length = key_length;
        }

        kept->element_count = count;

        return true;
}

static COLD bool exec_keep_element(exec_kept_value address_to kept,
                                   string_address base, positive base_length,
                                   string_address subscript,
                                   positive subscript_length)
{
        positive key_length;
        string_address key = shell_expand_subscript(
            base, base_length, subscript, subscript_length,
            address_of key_length);
        string_address value;

        if (!key)
                return false;

        kept->name = shell_store_take(address_of exec_store,
                                      base_length + key_length + 3);

        if (!kept->name)
                return false;

        memory_copy(kept->name, base, base_length);
        kept->name[base_length] = '[';
        memory_copy(kept->name + base_length + 1, key, key_length);
        kept->name[base_length + 1 + key_length] = ']';
        kept->name[base_length + 2 + key_length] = end;
        kept->name_length = base_length + key_length + 2;
        kept->base_length = base_length;
        kept->exported = false;
        kept->value = null;
        kept->attributes = shell_array_attributes(base, base_length);
        kept->compound = false;
        kept->elements = null;
        kept->element_count = 0;

        value = shell_array_get(kept->name, base_length, key, key_length,
                                null);

        if (!value)
                return true;

        kept->value = exec_arena_copy(value);

        return kept->value != exec_nothing;
}

static bool exec_keep_value(exec_kept_value address_to kept, string_address word)
{
        positive length = (positive)(string_first_of_or_end(word, '=') - word);
        bool append;
        bool compound;
        string_address bracket;
        string_address value;

        kept->promoted = false;
        append = length && word[length - 1] == '+';

        if (append)
                length--;

        compound = string_is(word + length + append, '=') &&
                   string_is(word + length + append + 1, '(');

        bracket = string_first_of(word, '[');

        if (bracket && (positive)(bracket - word) >= length)
                bracket = null;

        /*
                An element is remembered by the name its subscript resolves
                to, because that is what putting it back has to name. The
                subscript is read here and again when the assignment is
                really made; a=(...) values and plain subscripts do not care,
                and Bash's own one-evaluation rule only shows in a subscript
                that assigns, which no script should be writing.
        */
        if (bracket)
                return exec_keep_element(
                    kept, word, (positive)(bracket - word), bracket + 1,
                    length - (positive)(bracket - word) - 2);

        value = env_saved_state(word, length, address_of kept->exported,
                                address_of kept->attributes);

        /* The common case stays the original one-probe save. A nameref alone
           takes the cold second probe needed to save the target that the
           provisional assignment will actually change. */
        if (kept->attributes & SHELL_ARRAY_NAMEREF)
        {
                const_string resolved_name;
                const_string resolved_subscript;
                positive resolved_length;
                positive resolved_subscript_length;

                if (shell_reference_element(
                        word, length, address_of resolved_name,
                        address_of resolved_length,
                        address_of resolved_subscript,
                        address_of resolved_subscript_length))
                        return exec_keep_element(
                            kept, (string_address)resolved_name,
                            resolved_length,
                            (string_address)resolved_subscript,
                            resolved_subscript_length);

                if (!shell_reference_resolve(
                        word, length, address_of resolved_name,
                        address_of resolved_length))
                        return false;

                word = (string_address)resolved_name;
                length = resolved_length;
                value = env_saved_state(word, length,
                                        address_of kept->exported,
                                        address_of kept->attributes);
        }

        kept->name = shell_store_take(address_of exec_store, length + 1);

        if (!kept->name)
                return false;

        string_copy_max_end(kept->name, word, length);
        kept->name_length = length;
        kept->base_length = 0;
        kept->elements = null;
        kept->element_count = 0;
        // Whether the value about to be written is a list. Nothing else says
        // that a scalar restore would leave elements standing behind it.
        kept->compound = compound;

        kept->value = null;

        if ((kept->attributes & SHELL_ARRAY_EITHER) &&
            !exec_keep_array(kept))
                return false;

        if (!value)
                return true;

        kept->value = exec_arena_copy(value);

        return kept->value != exec_nothing;
}

static COLD bool exec_put_back_attributes(exec_kept_value address_to kept)
{
        const_string name = kept->name;
        positive length = kept->base_length ? kept->base_length
                                            : kept->name_length;
        const_string resolved_name;
        positive resolved_length;
        p8 current;

        if (kept->base_length &&
            shell_reference_resolve(name, length, address_of resolved_name,
                                    address_of resolved_length))
        {
                name = resolved_name;
                length = resolved_length;
        }

        current = shell_variable_attributes(name, length);

        return current == kept->attributes ||
               shell_variable_attribute_set(
                   name, length, kept->attributes,
                   (p8)~kept->attributes);
}

static fn exec_put_back(exec_kept_value address_to kept, b32 count)
{
        while (count--)
        {
                if (kept[count].promoted)
                        continue;

                /* A declaration reached while the prefix was active belongs
                   to that command as surely as its value does. Restore the
                   complete saved byte before writing the old value, so a
                   newly-added readonly bit cannot refuse the restoration. */
                if (!exec_put_back_attributes(kept + count))
                        continue;

                if (kept[count].base_length)
                {
                        positive base = kept[count].base_length;
                        string_address key = kept[count].name + base + 1;
                        positive key_length =
                            kept[count].name_length - base - 2;

                        if (kept[count].value)
                                shell_array_set(kept[count].name, base, key,
                                                key_length, kept[count].value,
                                                false);
                        else
                                shell_array_forget(kept[count].name, base, key,
                                                   key_length);

                        continue;
                }

                /*
                        A name that was not an array cannot be left as one:
                        a compound assignment in front of a command wrote
                        elements that putting a scalar value back would not
                        reach. A name that was one is put back element by
                        element, because the clear that precedes it is what
                        makes replacing an array a replacement.
                */
                if ((kept[count].attributes & SHELL_ARRAY_EITHER) ||
                    (kept[count].compound &&
                     (shell_variable_attributes(kept[count].name,
                                                kept[count].name_length) &
                      SHELL_ARRAY_EITHER)))
                {
                        shell_array_clear(kept[count].name,
                                          kept[count].name_length);
                }

                // A name that was an array is left as one, empty or not.
                // Unsetting it here would take its kind with it and the next
                // assignment would read its subscripts the other way.
                if (kept[count].attributes & SHELL_ARRAY_EITHER)
                {
                        for (positive at = 0; at < kept[count].element_count;
                             at++)
                        {
                                exec_kept_element address_to one =
                                    kept[count].elements + at;

                                shell_array_set(kept[count].name,
                                                kept[count].name_length,
                                                one->text, one->key_length,
                                                one->text + one->key_length + 1,
                                                false);
                        }

                        env_export_restore(kept[count].name,
                                           kept[count].exported);
                        continue;
                }

                if (kept[count].value)
                        env_set(kept[count].name, kept[count].value);
                else
                        env_unset_span(kept[count].name,
                                       kept[count].name_length);

                env_export_restore(kept[count].name, kept[count].exported);
        }
}

static fn exec_release_assignments(string_address address_to assignments,
                                   b32 count)
{
        while (count--)
                env_export_release(assignments[count]);
}

static bool exec_declaration_name(b32 word)
{
        static string_address names[] = {
            "export", "readonly", "local", "declare", "typeset",
        };

        if (!(parse_word_flags[word] & PARSE_WORD_LITERAL))
                return false;

        return string_table_find(parse_words[word], names, sizeof(names[0]),
                                 array_count(names)) <
               array_count(names);
}

/*
        The assignment operands of a declaration utility use assignment
        expansion even though they follow the command name. Issue 8 makes
        command a declaration utility when the name it invokes is one; walk
        literal command chains and their options to find that boundary.
*/
static PURE b32 exec_declaration_from(parse_node address_to node)
{
        b32 at = node->word;
        b32 stop = at + node->word_count;

        while (at < stop &&
               (parse_word_flags[at] & PARSE_WORD_ASSIGNMENT))
                at++;

        while (at < stop)
        {
                if (exec_declaration_name(at))
                        return at + 1;

                if (!(parse_word_flags[at] & PARSE_WORD_LITERAL) ||
                    !word_is(parse_words[at], "command"))
                        return stop;

                at++;

                while (at < stop)
                {
                        string_address option;

                        if (!(parse_word_flags[at] & PARSE_WORD_LITERAL))
                                return stop;

                        option = parse_words[at];

                        if (word_is(option, "--"))
                        {
                                at++;
                                break;
                        }

                        if (string_not(option, '-') || !string_get(option + 1))
                                break;

                        option++;

                        while (string_get(option))
                        {
                                if (string_get(option) != 'p')
                                        return stop;

                                option++;
                        }

                        at++;
                }
        }

        return stop;
}

/*
        Control operands share the checked library scanner: signs, whitespace
        and overflow are parsed once. Dash then bounds the value to INT_MAX;
        Bash return accepts a signed machine word before truncating to status.
*/
static bool exec_control_integer(string_address word, bipolar address_to answer)
{
        // The common status/loop count is one digit: settle it in registers
        // before entering the shared sign/whitespace/overflow scanner.
        positive digit = (positive)(word[0] - '0');
        if (digit <= 9 && !word[1])
        {
                *answer = (bipolar)digit;
                return true;
        }
        string_address stopped;
        b32 overflow;
        bipolar parsed = string_to_number_checked(word, address_of stopped, 10,
                                                   address_of overflow);

        if (overflow || stopped == word)
                return false;
        while (byte_is_space(*stopped))
                stopped++;
        if (*stopped)
                return false;

        *answer = parsed;
        return true;
}

static bool exec_control_number(string_address word, bool allow_zero,
                                b32 address_to answer)
{
        bipolar parsed;

        if (!exec_control_integer(word, address_of parsed) || parsed < 0 ||
            parsed > 0x7fffffff || (!allow_zero && !parsed))
                return false;
        *answer = (b32)parsed;
        return true;
}

static COLD fn exec_return_bash()
{
        positive first = 1;
        bipolar value = shell_status;
        bool valid = true;

        if (first < shell_argc && word_is(shell_argv[first], "--"))
                first++;
        if (first < shell_argc)
        {
                valid = exec_control_integer(shell_argv[first], address_of value);
                if (!valid)
                {
                        string_format(exec_error,
                                      "return: %s: numeric argument required\n",
                                      shell_argv[first]);
                        value = 2;
                }
                else if (shell_argc > first + 1)
                {
                        exec_error("return: too many arguments\n", 0);
                        shell_status = 1;
                        if (!shell_is_interactive || exec_forked ||
                            string_is(shell_option_flags, 'c'))
                        {
                                shell_trap_exit();
                                log_flush();
                                exit(1);
                        }
                        exec_expand_fatal();
                        return;
                }
        }

        if (!exec_function_depth && !shell_source_depth)
        {
                exec_error("return: can only return from a function or sourced script\n", 0);
                shell_status = 2;
                return;
        }

        shell_status = (b32)((positive)value & 0xff);
        exec_signal = EXEC_SIGNAL_RETURN;
}

/* break, continue and return are executor operations, not ordinary C
   builtins: their result has to unwind the surrounding parse tree. Query mode
   exposes that same namespace to command/type without copying the name list. */
bool exec_control_builtin(string_address name, bool run)
{
        p8 initial = string_get(name);

        if ((initial == 'b' && !string_compare(name, "break")) ||
            (initial == 'c' && !string_compare(name, "continue")))
        {
                b32 levels = 1;

                if (!run)
                        return true;

                if (shell_argc > 1 &&
                    !exec_control_number(shell_argv[1], false,
                                         address_of levels))
                {
                        string_format(exec_error, "%s: Illegal number: %s\n",
                                      name, shell_argv[1]);
                        shell_status = 2;
                        exec_expand_fatal();
                        return true;
                }

                if (exec_loop_depth)
                {
                        if (levels > exec_loop_depth)
                                levels = exec_loop_depth;

                        exec_signal = initial == 'b'
                                          ? EXEC_SIGNAL_BREAK
                                          : EXEC_SIGNAL_CONTINUE;
                        exec_signal_level = levels;
                }

                shell_status = 0;
                return true;
        }

        if (initial != 'r' || string_compare(name, "return"))
                return false;

        if (!run)
                return true;

        exec_return_previous = shell_status;
        if (shell_bash_compat)
        {
                exec_return_bash();
                return true;
        }
        if (shell_argc > 1 &&
            !exec_control_number(shell_argv[1], true, address_of shell_status))
        {
                string_format(exec_error, "return: Illegal number: %s\n",
                              shell_argv[1]);
                shell_status = 2;
                exec_expand_fatal();
                return true;
        }

        exec_signal = EXEC_SIGNAL_RETURN;
        return true;
}

static b32 exec_dispatch(b32 command_word)
{
        /* PATH answers are transient only until exec/spawn has copied argv.
           Keep one movable room across commands instead of mapping and
           unmapping it for every external command in a loop.  No parser or
           expansion pointer refers into this room, and dispatch does not
           re-enter while shell_execute_command waits for the child. */
        static p8 address_to found;
        static positive found_room;
        string_address name = shell_argv[0];
        positive2 named;
        p8 initial = string_get(name);
        positive slot;
        bool special;
        bool colon = initial == ':' && !string_get(name + 1);

        if (colon && exec_special_builtin(name))
        {
                shell_status = 0;
                return 0;
        }

        shell_command_name_stable =
            parse_words[command_word] == name &&
            (parse_word_flags[command_word] & PARSE_WORD_LITERAL);

        if (shell_command_name_stable)
        {
                /* Leading assignments have already been consumed, so this
                   literal word's otherwise-unused assignment-name hash slot
                   can cache the complete command hash for every later pass
                   through a kept loop tree. */
                named.x = parse_word_name_hashes[command_word];
                named.y = parse_word_lengths[command_word];

                if (!named.x)
                {
                        named.x = memory_hash_33(name, named.y);
                        parse_word_name_hashes[command_word] = named.x;
                }

                shell_command_name_address = name;
                shell_command_name_length = named.y;
        }
        else
                named = string_hash_33_length(name);

        special = exec_special_builtin(name);

        /* POSIX special builtins precede functions. Bash's ordinary mode
           deliberately reverses that order, including for the three control
           builtins implemented in this file. */
        if (special)
        {
                if (exec_control_builtin(name, true))
                        return shell_status;

                {
                        bool tail = shell_tail_command;

                        if (shell_builtin(null, named))
                                return shell_status;
                        shell_tail_command = tail;
                }
        }

        slot = exec_function_slot(name, named);

        if (slot != positive_max)
        {
                shell_tail_command = false;
                return exec_call(slot);
        }

        if (colon)
        {
                shell_status = 0;
                return 0;
        }

        if (!special && exec_control_builtin(name, true))
                return shell_status;

        {
                bool tail = shell_tail_command;

                if (!special && shell_builtin(null, named))
                        return shell_status;
                shell_tail_command = tail;
        }

        {
                bipolar located = shell_find_in_path_alloc(name,
                                                            address_of found,
                                                            address_of found_room);

                if (located < 0)
                {
                        shell_status = 2;
                        string_format(exec_error, "%s: no room\n", name);
                        return shell_status;
                }

                if (located == 2)
                {
                        shell_status = 126;
                        string_format(exec_error, "%s: cannot run\n", name);
                        return shell_status;
                }

                if (located == 1)
                {
                        shell_argv[0] = found;
                        if (shell_tail_command)
                                shell_thread_instance_mode(exec_asynchronous);
                        else if (job_monitor())
                                job_execute_foreground();
                        else
                                shell_execute_command();
                        shell_argv[0] = name;
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
        bool action_fatal = false;
        bipolar number;

        if (exec_line_aborted() || !trap_waiting())
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
                // An action is source, however many lines of it there are,
                // and what it leaves unfinished is its own syntax error --
                // the same two calls eval makes. One line at a time used to
                // be one line only, and the second command of an action was
                // never run.
                run_lines(action);
                shell_input_end();
                parse_nest_leave();

                if (exec_line_aborted())
                {
                        action_fatal = true;
                        break;
                }
        }

        trap_entered(false);

        if (action_fatal)
        {
                shell_status = 2;
                exec_signal = EXEC_SIGNAL_FATAL;
                exec_signal_level = 0;
                exec_tested = kept_tested;
                return;
        }

        shell_status = kept_status;
        exec_signal = kept_signal;
        exec_signal_level = kept_level;
        exec_tested = kept_tested;
}

static COLD b32 exec_dispatch_scoped(b32 command_word,
                                     exec_kept_value address_to kept,
                                     b32 count)
{
        exec_kept_value address_to previous = exec_promotable;
        b32 previous_count = exec_promotable_count;
        b32 status;

        exec_promotable = kept;
        exec_promotable_count = count;
        status = exec_dispatch(command_word);
        exec_promotable = previous;
        exec_promotable_count = previous_count;
        return status;
}

/* Keyword assignments use the same leading-assignment executor. Partition
   indices, not parser-owned words: a cached function can run with -k both on
   and off, and recursive execution must never rewrite its parse tree. */
static COLD b32 address_to exec_keyword_order(parse_node address_to node,
                                              b32 address_to leading)
{
        b32 assignments = 0;
        b32 address_to order;

        for (b32 at = 0; at < node->word_count; at++)
                assignments += (parse_word_flags[node->word + at] &
                                  PARSE_WORD_ASSIGNMENT) != 0;
        if (assignments == *leading)
                return null;

        order = (b32 address_to)shell_store_take(
            address_of exec_store, (positive)node->word_count * sizeof(*order));
        if (!order)
        {
                *leading = -1;
                return null;
        }

        b32 next_assignment = 0;
        b32 next_argument = assignments;
        for (b32 at = 0; at < node->word_count; at++)
        {
                b32 word = node->word + at;
                order[(parse_word_flags[word] & PARSE_WORD_ASSIGNMENT)
                          ? next_assignment++ : next_argument++] = word;
        }
        *leading = assignments;
        return order;
}

static b32 exec_simple(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        exec_kept_value address_to kept = null;
        exec_kept_value address_to expanded_kept = null;
        string_address address_to assignments = null;
        shell_mark arena_mark = shell_store_mark(address_of exec_store);
        b32 kept_count = 0;
        b32 expanded_count = 0;
        b32 temporary_count = 0;
        b32 mark = exec_save_count;
        b32 count = 0;
        b32 first = 0;
        b32 declaration_from = exec_declaration_from(node);
        b32 leading = 0;
        b32 address_to word_order = null;
        b32 status;
        b32 at;
        bool bare_exec;
        bool assignments_only;
        bool fatal_assignment;
        bool special = false;
        bool fatal = false;
        shell_words arguments;

        //      argv grows with the line. A command's words are whatever the
        //      expansions made of them, and a directory may hold any number of
        //      names, so there is nothing sensible to clamp this to.
        shell_words_bind(address_of arguments, address_of shell_argv,
                         address_of shell_argv_room);

        // The line this command was written on, which caller and $LINENO
        // answer with for as long as it runs.
        exec_line = node->line;
        token_used = 0;
        token_overflow = false;
        // With no command name, POSIX makes the command's status that of the
        // last command substitution it performed. Each substitution updates
        // this while the words and redirect targets below are expanded.
        shell_substitution_status = 0;

        /* Expand command arguments before assignment right-hand sides. The
           reserved argv prefix is filled afterwards; provisional assignments
           still run left to right so `a=one b=$a` sees the preceding value. */
        while (leading < node->word_count &&
               (parse_word_flags[node->word + leading] &
                PARSE_WORD_ASSIGNMENT))
                leading++;

        if (shell_keyword_on())
                word_order = exec_keyword_order(node, address_of leading);

        if (leading < 0)
        {
                status = 2;
                goto fail;
        }

#define EXEC_WORD(at) (word_order ? word_order[(at)] : node->word + (at))

        if (leading)
        {
                expanded_kept = (exec_kept_value address_to)shell_store_take(
                    address_of exec_store,
                    (positive)leading * sizeof(expanded_kept[0]));

                if (!expanded_kept ||
                    !shell_array_room(shell_argv, shell_argv_room,
                                      (positive)leading + 2))
                {
                        status = 2;
                        goto fail;
                }
                arguments.count = (positive)leading;
                count = first = leading;
        }

        for (at = leading; at < node->word_count; at++)
        {
                b32 word_index = EXEC_WORD(at);
                string_address word = parse_words[word_index];
                p8 word_flags = parse_word_flags[word_index];
                bool literal = word_flags & PARSE_WORD_LITERAL;
                bool assignment = word_flags & PARSE_WORD_ASSIGNMENT;

                /*
                        Declaration operands are expanded whole; the ordinary
                        argument expansion must not split their right sides.
                */
                if (assignment && word_index >= declaration_from)
                {
                        positive value_at =
                            parse_word_name_lengths[word_index] + 1 +
                            ((word_flags & PARSE_WORD_APPEND) != 0);

                        if (!shell_words_add(
                                address_of arguments,
                                (literal ||
                                 (word_flags & PARSE_WORD_COMPOUND))
                                    ? word
                                    : shell_expand_assignment(word,
                                                              value_at)))
                                break;

                        count = (b32)arguments.count;

                        if (exec_line_aborted())
                                break;

                        continue;
                }

                if (literal)
                {
                        if (!shell_words_add(address_of arguments, word))
                                break;

                        count = (b32)arguments.count;
                }
                else
                        count = (b32)shell_expand_fields(word,
                                                         address_of arguments);

                if (exec_line_aborted())
                        break;
        }

        if (at != node->word_count && !exec_line_aborted())
        {
                status = 2;
                goto fail;
        }

        assignments_only = first == count;
        fatal_assignment = assignments_only || !shell_bash_compat ||
                           shell_posix_on();
        for (at = 0; at < leading && !exec_line_aborted(); at++)
        {
                b32 word_index = EXEC_WORD(at);
                p8 flags = parse_word_flags[word_index];
                string_address word = parse_words[word_index];
                positive substitution_generation =
                    shell_substitution_generation;
                positive value_at = parse_word_name_lengths[word_index] + 1 +
                                      ((flags & PARSE_WORD_APPEND) != 0);
                string_address trial =
                    (flags & (PARSE_WORD_LITERAL | PARSE_WORD_COMPOUND))
                        ? word : shell_expand_assignment(word, value_at);

                /* Bash's ordinary mode exposes each substitution answer to
                   the next assignment RHS. POSIX freezes the status from
                   before the simple command until all RHS expansions finish;
                   both modes still return the last substitution below. */
                if (shell_bash_compat && !shell_posix_on() &&
                    substitution_generation != shell_substitution_generation)
                        shell_status = shell_substitution_status;

                if (exec_line_aborted())
                        break;
                if (!trial ||
                    !exec_keep_value(expanded_kept + expanded_count, trial))
                {
                        status = 2;
                        goto fail;
                }
                expanded_count++;
                shell_argv[at] = trial;
                if (!exec_assign(address_of trial,
                                 parse_word_name_lengths[word_index],
                                 parse_word_name_hashes[word_index],
                                 (flags & PARSE_WORD_APPEND) != 0,
                                 (flags & PARSE_WORD_COMPOUND) != 0,
                                 expanded_kept[expanded_count - 1].name,
                                 expanded_kept[expanded_count - 1].base_length,
                                 fatal_assignment))
                {
                        status = shell_bash_compat ? 1 : 2;
                        goto fail;
                }
        }

        if (exec_line_aborted())
        {
                status = shell_status;
                goto fail;
        }

        /* With no command, these writes are already the final assignment.
           Keep the rollback snapshots for expansion failure, but do not
           restore and reapply successful values (or evaluate indices again).
           The original assignment words retain append syntax for tracing. */
        if (assignments_only)
                expanded_count = 0;
        exec_put_back(expanded_kept, expanded_count);
        expanded_count = 0;

        //      An empty command line never entered the loop, so the table
        //      may not exist yet to hold even the null that ends it.
        if (!count &&
            !shell_array_room(shell_argv, shell_argv_room, 2))
        {
                status = 2;
                goto fail;
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
        if (first != count)
                special = exec_special_builtin(shell_argv[first]);

        if (first && first != count)
        {
                bool save = !special;

                if (save)
                {
                        kept = (exec_kept_value address_to)shell_store_take(
                            address_of exec_store,
                            (positive)first * sizeof(kept[0]));

                        if (!kept)
                        {
                                status = 2;
                                goto fail;
                        }

                        for (at = 0; at < first; at++)
                        {
                                if (!exec_keep_value(kept + kept_count,
                                                     shell_argv[at]))
                                {
                                        status = 2;
                                        goto fail;
                                }

                                kept_count++;
                        }
                }
        }

        for (at = 0; !assignments_only && at < first; at++)
                if (!exec_assign(shell_argv + at,
                                 parse_word_name_lengths[EXEC_WORD(at)],
                                 parse_word_name_hashes[EXEC_WORD(at)],
                                 (parse_word_flags[EXEC_WORD(at)] &
                                  PARSE_WORD_APPEND) != 0,
                                 (parse_word_flags[EXEC_WORD(at)] &
                                  PARSE_WORD_COMPOUND) != 0,
                                 kept && kept_count == first
                                     ? kept[at].name
                                     : null,
                                 kept && kept_count == first
                                     ? kept[at].base_length
                                     : 0,
                                 fatal_assignment))
                {
                        status = shell_bash_compat ? 1 : 2;
                        goto fail;
                }

        /* POSIXLY_CORRECT is itself a mode switch. Bash decides whether its
           prefix persists using the mode after those writes: it stays for
           `POSIXLY_CORRECT=y :`, but is restored after an ordinary `true`.
           The pre-write snapshot remains necessary for the latter case; if
           the selected command has just become special, adopt every saved
           prefix instead of restoring it. */
        if (first != count)
        {
                bool active_special =
                    exec_special_builtin(shell_argv[first]);

                if (active_special && !special)
                        for (at = 0; at < kept_count; at++)
                                kept[at].promoted = true;

                special = active_special;
        }

        /*
                The assignment words, held apart from argv.

                They are wanted again when the command is over, to take the
                exports back, and argv is not theirs by then: a function body
                or a sourced file run by this command builds its own argv in
                the same table. Taken after the assignments have been made,
                because an append writes the joined word back into argv.
        */
        if (first && first != count)
        {
                assignments = (string_address address_to)shell_store_take(
                    address_of exec_store,
                    (positive)first * sizeof(assignments[0]));

                if (!assignments)
                {
                        status = 2;
                        goto fail;
                }

                for (at = 0; at < first; at++)
                        assignments[at] = shell_argv[at];
        }

        if (assignments &&
            (!special || word_is(shell_argv[first], "exec")))
                for (at = 0; at < first; at++)
                        if (!env_export_temporary(assignments[at]))
                        {
                                status = 2;
                                goto fail;
                        }
                        else
                                temporary_count++;

        exec_trace(count);

        if (node->redirect_count && !exec_redirect_apply(index))
        {
                exec_redirect_restore(mark);
                status = exec_redirect_failed_status();
                fatal = special;
                goto fail;
        }

        if (first == count)
        {
                if (node->redirect_count)
                        exec_redirect_restore(mark);
                shell_status = shell_substitution_status;
                shell_store_rewind(address_of exec_store, arena_mark);
                return shell_status;
        }

        // Include argv[count], the terminating null pointer.
        if (first)
                memory_copy(shell_argv, shell_argv + first,
                            (positive)(count - first + 1) *
                                sizeof(shell_argv[0]));

        shell_argc = count - first;

        // $_ is the last argument of the command before this one, which is
        // what taking it here and not after the run means: this command's
        // words are already expanded and have already read the old value.
        shell_last_argument_set(shell_argv[shell_argc - 1]);

        // exec with nothing to run is there for its redirections, and those
        // belong to the shell from here on. Decided before anything runs: a
        // function body or a sourced file run by this command leaves its own
        // last command in argv, and "exec 3>/dev/null" in a function made
        // the redirections on the call permanent as well.
        bare_exec = node->redirect_count && shell_argc == 1 &&
                    word_is(shell_argv[0], "exec");

        log_failure_reset();

        {
                bool previous_error = exec_special_error;
                bool builtin_error;

                exec_special_error = false;
                status = kept || exec_promotable
                             ? exec_dispatch_scoped(EXEC_WORD(first), kept,
                                                    kept_count)
                             : exec_dispatch(EXEC_WORD(first));
                builtin_error = exec_special_error;
                exec_special_error = previous_error;
                fatal = special && status && builtin_error;
        }
#undef EXEC_WORD
        log_flush();

        /* A write to a closed descriptor reaches the buffered writer at
           flush and leaves its sticky failure bit set. Asking the kernel
           whether stdout was closed before every command duplicated that
           answer and put an fcntl syscall in otherwise kernel-free loops. */
        if (log_failed() && !status)
                status = shell_status = 1;

        if (node->redirect_count)
        {
                if (bare_exec)
                        exec_redirect_forget(mark);
                else
                        exec_redirect_restore(mark);
        }

        exec_release_assignments(assignments, temporary_count);
        exec_put_back(kept, kept_count);
        shell_store_rewind(address_of exec_store, arena_mark);

        if (fatal)
                expand_fatal_status(status);

        return status;

        /*
                Nothing of a command that could not run outlives it: the
                exports it made, the values it wrote over, the arena it took.
                Nine ways out each said so in their own words, and one of
                them left the arena behind.
        */
fail:
        exec_release_assignments(assignments, temporary_count);
        exec_put_back(kept, kept_count);
        exec_put_back(expanded_kept, expanded_count);
        shell_store_rewind(address_of exec_store, arena_mark);
        shell_status = status;

        if (fatal)
                expand_fatal_status(status);

        return status;
}

// What a break or a continue means to the loop it lands in: go round again,
// stop, or hand it further out still.
static bool exec_loop_again()
{
        if (!exec_signal)
                return true;

        if (exec_signal == EXEC_SIGNAL_RETURN ||
            exec_signal == EXEC_SIGNAL_FATAL)
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

                /*
                        The condition is inside the loop as much as the body
                        is: a break in it ends this loop and a continue in it
                        asks the condition again, which is what dash does.
                        Counted outside, a break in the condition meant the
                        enclosing loop, and with no enclosing loop it meant
                        nothing and "while break" ran forever.
                */
                exec_tested = true;
                exec_loop_depth++;
                test = exec_node(node->left);
                exec_loop_depth--;
                exec_tested = tested;

                if (exec_line_aborted())
                {
                        status = 2;
                        break;
                }

                if (exec_signal)
                {
                        if (exec_loop_again())
                                continue;

                        break;
                }

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

//      What a for loop walks over, and the fields one of its words became.
//      Both live here rather than on the stack so they can grow and be reused
//      by the next loop instead of being sized for a guess.
static string_address address_to exec_items;
static positive exec_items_room;

// An expansion that aborted the line leaves nothing to run: what was taken
// goes back, and the answer is the status the abort carries.
static b32 exec_aborted(shell_mark mark)
{
        shell_store_rewind(address_of exec_store, mark);
        shell_status = 2;

        return 2;
}

/*
        What a for or a select walks over, in exec_items.

        Both take the same words in the same place and differ only in what
        they do with them afterwards, so the expansion is one function and
        the loop is two.
*/
static b32 exec_loop_items(parse_node address_to node)
{
        b32 count = 0;
        b32 at;

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
                        shell_words list;
                        positive made;
                        positive field;

                        shell_words_bind(address_of list, address_of exec_fields,
                                         address_of exec_fields_room);
                        made = shell_expand_fields(parse_words[node->word + at],
                                                   address_of list);

                        if (exec_line_aborted())
                        {
                                room = false;
                                break;
                        }

                        for (field = 0; field < made; field++)
                        {
                                string_address kept;

                                if (!shell_room((address_any address_to)
                                                    address_of exec_items,
                                                address_of exec_items_room,
                                                (positive)count + 1,
                                                sizeof(string_address)))
                                {
                                        room = false;
                                        break;
                                }

                                kept = exec_arena_copy(exec_fields[field]);

                                // An arena with nothing left in it gives back
                                // the empty string, and a loop that quietly
                                // ran over empty items is worse than one that
                                // stops where it ran out.
                                if (kept == exec_nothing)
                                {
                                        room = false;
                                        break;
                                }

                                exec_items[count++] = kept;
                        }
                }
        }
        else
        {
                for (at = 0; at < (b32)shell_parameter_count; at++)
                {
                        if (!shell_array_room(exec_items, exec_items_room, (positive)at + 1))
                                break;

                        exec_items[count++] = exec_arena_copy(shell_parameter[at]);
                }
        }

        return count;
}

static COLD b32 exec_loop_assignment_error(string_address name)
{
        string_format(exec_error,
                      env_readonly(name) ? "%s: is read only\n"
                                         : "%s: cannot assign\n",
                      name);

        if (shell_bash_compat && !shell_posix_on())
                return 1;

        if (shell_bash_compat)
        {
                expand_fatal_status(127);
                return 127;
        }

        expand_fatal();
        return 2;
}

static b32 exec_for(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        string_address name = parse_words[node->word];
        shell_mark mark = shell_store_mark(address_of exec_store);
        b32 count;
        b32 status = 0;
        b32 at;

        token_used = 0;
        count = exec_loop_items(node);

        if (exec_line_aborted())
                return exec_aborted(mark);

        for (at = 0; at < count; at++)
        {
                if (!env_assign(name, exec_items[at]))
                {
                        status = exec_loop_assignment_error(name);
                        break;
                }

                exec_loop_depth++;
                status = exec_node(node->right);
                exec_loop_depth--;

                if (!exec_loop_again())
                        break;
        }

        shell_store_rewind(address_of exec_store, mark);

        return status;
}

/*
        select: a numbered menu, a prompt, and a loop that runs the body once
        for every line somebody answers with.

        Everything it writes goes to standard error, so a script may take the
        body's output on a pipe while a person still sees the menu -- which is
        the whole reason the construct exists.
*/

// A line built whole before it is written. What a select writes is one
// menu somebody reads and what a time writes is one timing, not thirty
// writes with somebody else's output free to land between them.
static p8 address_to exec_built;
static positive exec_built_room;
static positive exec_built_used;

static bool exec_built_add(string_address text, positive length)
{
        if (!shell_array_room(exec_built, exec_built_room,
                              exec_built_used + length + 1))
                return false;

        memory_copy(exec_built + exec_built_used, text, length);
        exec_built_used += length;

        return true;
}

static bool exec_built_fill(p8 value, positive times)
{
        if (!shell_array_room(exec_built, exec_built_room,
                              exec_built_used + times + 1))
                return false;

        memory_fill(exec_built + exec_built_used, value, times);
        exec_built_used += times;

        return true;
}

/*
        The gap between one column and the next, written as tabs wherever a
        tab reaches further than the spaces would.

        This is what Bash writes, and a menu is bytes somebody reads, so the
        padding is part of the answer and not a matter of taste.
*/
static bool select_menu_indent(positive from, positive to)
{
        positive at = from;

        while (at < to)
        {
                if (at / 8 < to / 8)
                {
                        if (!exec_built_fill('\t', 1))
                                return false;

                        at = (at / 8 + 1) * 8;
                        continue;
                }

                if (!exec_built_fill(' ', to - at))
                        return false;

                at = to;
        }

        return true;
}

static CONST positive select_digits(positive value)
{
        positive width = 1;

        while (value >= 10)
        {
                value /= 10;
                width++;
        }

        return width;
}

/*
        How wide the menu is allowed to be.

        COLUMNS when it says something, and eighty otherwise, which is what
        Bash falls back to when it is not looking at a terminal.
*/
static positive select_width()
{
        string_address given = env_get((const_string) "COLUMNS");
        positive parsed;

        if (given && string_digits_exact(given, address_of parsed) && parsed)
                return parsed;

        return 80;
}

/*
        The menu itself.

        Every cell is as wide as the longest item plus the room its number
        takes, and the items go down the columns rather than across them. A
        list that would fit on one row is turned on its side and written one
        to a line, which is the shape nearly every menu has.
*/
static fn select_menu_write(b32 count)
{
        positive width = select_width();
        positive longest = 0;
        positive cell;
        positive columns;
        positive rows;
        positive numbered;
        positive first;
        positive row;
        b32 at;

        for (at = 0; at < count; at++)
        {
                positive length = string_length(exec_items[at]);

                if (length > longest)
                        longest = length;
        }

        numbered = select_digits((positive)count);
        cell = longest + numbered + 4;
        columns = width / cell;

        if (!columns)
                columns = 1;

        rows = (positive)count / columns + ((positive)count % columns ? 1 : 0);
        columns = (positive)count / rows + ((positive)count % rows ? 1 : 0);

        if (rows == 1)
        {
                rows = columns;
                columns = 1;
        }

        first = select_digits(rows);
        exec_built_used = 0;

        for (row = 0; row < rows; row++)
        {
                positive item = row;
                positive at_column = 0;

                while (1)
                {
                        p8 shown[32];
                        positive number = at_column ? numbered : first;
                        positive length = string_length(exec_items[item]);
                        positive written =
                            bipolar_into_string(shown, (bipolar)(item + 1));

                        if (written < number &&
                            !exec_built_fill(' ', number - written))
                                return;

                        if (!exec_built_add(shown, written) ||
                            !exec_built_add((string_address) ") ", 2) ||
                            !exec_built_add(exec_items[item], length))
                                return;

                        item += rows;

                        if (item >= (positive)count)
                                break;

                        if (!select_menu_indent(at_column + number + length + 2,
                                                at_column + cell))
                                return;

                        at_column += cell;
                }

                if (!exec_built_add((string_address) "\n", 1))
                        return;
        }

        log_error(exec_built, exec_built_used);
}

// The line somebody answered with, without its newline. A byte at a time,
// because whatever is behind it on the stream belongs to the next reader.
static p8 address_to select_reply;
static positive select_reply_room;
static positive select_reply_used;

static bool select_read()
{
        select_reply_used = 0;

        while (1)
        {
                p8 value;

                if (!shell_array_room(select_reply, select_reply_room,
                                      select_reply_used + 2))
                        return false;

                if (system_read_once(0, address_of value, 1) != 1)
                        break;

                if (value == '\n')
                {
                        select_reply[select_reply_used] = end;
                        return true;
                }

                select_reply[select_reply_used++] = value;
        }

        select_reply[select_reply_used] = end;

        // A last line with no newline behind it is still a line. Nothing at
        // all is the end of the input.
        return select_reply_used != 0;
}

/*
        Which item the answer names, or nothing.

        Blanks either side and a leading plus are read exactly as Bash reads
        any number; anything left over makes the whole line not a number, and
        the name is then set to the empty string rather than to an item.
*/
static PURE positive select_choice(b32 count)
{
        string_address at = select_reply;
        positive value = 0;
        bool any = false;

        at += string_span(at, string_set_blanks);

        if (string_is(at, '+'))
                at++;

        while (string_get(at) >= '0' && string_get(at) <= '9')
        {
                if (value <= (positive)count)
                        value = value * 10 + (positive)(string_get(at) - '0');

                any = true;
                at++;
        }

        at += string_span(at, string_set_blanks);

        if (!any || string_get(at) || !value || value > (positive)count)
                return 0;

        return value;
}

static b32 exec_select(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        string_address name = parse_words[node->word];
        shell_mark mark = shell_store_mark(address_of exec_store);
        b32 count;
        b32 status = 0;

        token_used = 0;
        count = exec_loop_items(node);

        if (exec_line_aborted())
                return exec_aborted(mark);

        // Nothing to choose from is not a menu nobody answered: no menu is
        // written, the body never runs, and the construct succeeds.
        if (!count)
        {
                shell_store_rewind(address_of exec_store, mark);
                return 0;
        }

        select_menu_write(count);

        while (1)
        {
                string_address prompt = env_get((const_string) "PS3");
                positive chosen;

                log_error(prompt ? prompt : (string_address) "#? ", 0);

                if (!select_read())
                {
                        /*
                                The end of the input ends the loop, and Bash
                                calls that a failure rather than an empty
                                answer.

                                The newline that closes the unanswered prompt
                                line goes to standard output, which is where
                                Bash puts it and the one thing about a select
                                that a script reading its body's output sees.
                        */
                        log((address_any) "\n", 1);
                        log_flush();
                        status = 1;
                        break;
                }

                // An empty line asks for the menu again and nothing else.
                if (!select_reply_used)
                {
                        select_menu_write(count);
                        continue;
                }

                if (!env_assign((const_string) "REPLY", select_reply))
                {
                        string_format(exec_error, "REPLY: cannot assign\n");
                        status = 2;
                        break;
                }

                chosen = select_choice(count);

                if (!env_assign(name, chosen ? exec_items[chosen - 1]
                                             : (string_address) ""))
                {
                        status = exec_loop_assignment_error(name);
                        break;
                }

                exec_loop_depth++;
                status = exec_node(node->right);
                exec_loop_depth--;

                if (!exec_loop_again())
                        break;
        }

        shell_store_rewind(address_of exec_store, mark);

        return status;
}

static bool exec_arithmetic_value(string_address text,
                                  bipolar address_to value)
{
        shell_mark mark = shell_store_mark(address_of expand_store);
        string_address ready = shell_expand_arithmetic_text(text);
        bool held = arith_bash_mode;

        if (!ready || expand_failed)
        {
                shell_store_rewind(address_of expand_store, mark);
                return false;
        }

        arith_bash_mode = true;
        address_to value = arith_evaluate(ready);
        arith_bash_mode = held;

        if (!arith_bad)
        {
                shell_store_rewind(address_of expand_store, mark);
                return true;
        }

        string_format(exec_error, "arithmetic: %s\n", ready);
        shell_store_rewind(address_of expand_store, mark);
        return false;
}

/*
        (( )) and [[ ]] arrive as one word, brackets and all. The closing pair
        is taken off for as long as the inside is being read and put back
        afterwards, because the word is the parser's: a kept loop body is
        read again next time round.
*/
static bool exec_bracket_strip(string_address whole,
                               positive address_to length, p8 address_to held)
{
        address_to length = string_length(whole);

        if (address_to length < 4)
                return false;

        address_to held = whole[address_to length - 2];
        whole[address_to length - 2] = end;

        return true;
}

static fn exec_bracket_restore(string_address whole, positive length, p8 held)
{
        whole[length - 2] = held;
}

static b32 exec_arithmetic_command(b32 index)
{
        string_address whole = parse_words[parse_nodes[index].word];
        positive length;
        bipolar value;
        b32 status;
        p8 held;

        if (!exec_bracket_strip(whole, address_of length, address_of held))
                return 1;

        if (!string_get(whole + 2 + string_span_of_set(whole + 2, " \t\n")))
                status = 1;
        else if (!exec_arithmetic_value(whole + 2, address_of value))
                status = exec_line_aborted() ? 2 : 1;
        else
                status = value ? 0 : 1;

        exec_bracket_restore(whole, length, held);

        return status;
}

// A C-for separator is a semicolon in the outer arithmetic grammar, not one
// inside grouping, a quote, ${...}, $(...), or a backtick substitution.
static PURE string_address exec_cfor_separator(string_address at)
{
        positive depth = 0;

        while (string_get(at))
        {
                p8 value = string_get(at);
                b32 skipped = lex_skip_held(address_of at);

                //      An unclosed quote leaves at on the terminating null,
                //      which is where running out of separators ends too.
                if (skipped == LEX_SKIP_UNCLOSED)
                        return at;

                if (skipped)
                        continue;

                if (value == '(')
                        depth++;
                else if (value == ')' && depth)
                        depth--;
                else if (value == ';' && !depth)
                        return at;

                at++;
        }

        return at;
}

static b32 exec_cfor(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        shell_mark arena = shell_store_mark(address_of exec_store);
        string_address whole = parse_words[node->word];
        positive length = string_length(whole);
        positive inner_length;
        p8 address_to expressions;
        string_address initialize;
        string_address condition;
        string_address update;
        string_address first;
        string_address second;
        bipolar value;
        b32 status = 0;

        if (length < 4)
        {
                status = 2;
                goto done;
        }

        inner_length = length - 4;
        expressions = shell_store_take(address_of exec_store, inner_length + 1);

        if (!expressions)
        {
                status = 2;
                goto done;
        }

        memory_copy_end(expressions, whole + 2, inner_length);
        first = exec_cfor_separator(expressions);

        if (!string_get(first))
        {
                string_format(exec_error, "arithmetic: expected two semicolons\n");
                status = 2;
                goto done;
        }

        address_to first = end;
        second = exec_cfor_separator(first + 1);

        if (!string_get(second))
        {
                string_format(exec_error, "arithmetic: expected two semicolons\n");
                status = 2;
                goto done;
        }

        address_to second = end;
        initialize = expressions;
        condition = first + 1;
        update = second + 1;

        if (string_get(initialize) &&
            !exec_arithmetic_value(initialize, address_of value))
        {
                status = exec_line_aborted() ? 2 : 1;
                goto done;
        }

        while (1)
        {
                if (string_get(condition))
                {
                        if (!exec_arithmetic_value(condition, address_of value))
                        {
                                status = exec_line_aborted() ? 2 : 1;
                                goto done;
                        }

                        if (!value)
                                break;
                }

                exec_loop_depth++;
                status = exec_node(node->right);
                exec_loop_depth--;

                if (!exec_loop_again())
                        break;

                if (string_get(update) &&
                    !exec_arithmetic_value(update, address_of value))
                {
                        status = exec_line_aborted() ? 2 : 1;
                        goto done;
                }
        }

done:
        shell_store_rewind(address_of exec_store, arena);
        return status;
}

/*
        Bash [[...]] has a word grammar of its own. In particular its && and
        || are not pipelines, and its operands expand without splitting or
        pathname lookup. Keep raw words here so operators produced by an
        expansion remain operands rather than turning into syntax afterward.
*/
static string_address address_to conditional_word;
static positive conditional_word_room;
static positive conditional_word_count;
static positive conditional_at;
static bool conditional_bad;
static bool conditional_active;

static bool conditional_add(string_address text, positive length)
{
        string_address kept;

        if (length == positive_max || conditional_word_count == positive_max ||
            !shell_array_room(conditional_word, conditional_word_room, conditional_word_count + 1))
                return false;

        kept = shell_store_take(address_of exec_store, length + 1);

        if (!kept)
                return false;

        memory_copy_end(kept, text, length);
        conditional_word[conditional_word_count++] = kept;
        return true;
}

static bool conditional_tokenize(string_address text)
{
        string_address at = text;

        conditional_word_count = 0;

        while (string_get(at))
        {
                string_address start;
                bool regex_operand;

                at += string_span_of_set(at, " \t\n");

                if (!string_get(at))
                        break;

                regex_operand = conditional_word_count &&
                                word_is(conditional_word[conditional_word_count - 1],
                                        "=~");

                if ((string_is(at, '&') && string_is(at + 1, '&')) ||
                    (string_is(at, '|') && string_is(at + 1, '|')))
                {
                        if (!conditional_add(at, 2))
                                return false;

                        at += 2;
                        continue;
                }

                if (!regex_operand &&
                    (string_is(at, '(') || string_is(at, ')')))
                {
                        if (!conditional_add(at, 1))
                                return false;

                        at++;
                        continue;
                }

                start = at;

                while (string_get(at) && !lex_is_space(string_get(at)))
                {
                        p8 value = string_get(at);

                        /*
                                An extended pattern group is one piece of the
                                word. [[ ]] reads them whether or not the
                                option is on, because what is in here is
                                matched when the command runs.
                        */
                        if (string_is(at + 1, '(') &&
                            (value == '?' || value == '*' || value == '+' ||
                             value == '@' || value == '!'))
                        {
                                string_address group = lex_nesting(at + 1);

                                if (group > at + 1)
                                {
                                        at = group;
                                        continue;
                                }
                        }

                        if ((value == '&' && string_is(at + 1, '&')) ||
                            (value == '|' && string_is(at + 1, '|')) ||
                            (!regex_operand &&
                             (value == '(' || value == ')')))
                                break;

                        if (value == '\\' && string_get(at + 1))
                        {
                                at += 2;
                                continue;
                        }

                        if (value == '\'' || value == '"')
                        {
                                string_address stop = lex_quote_end(at + 1, value);

                                if (!string_get(stop))
                                        return false;

                                at = stop + 1;
                                continue;
                        }

                        {
                                string_address inner = lex_nested_at(at);
                                string_address stop =
                                    inner ? lex_nesting(inner) : null;

                                if (stop && stop > inner)
                                {
                                        at = stop;
                                        continue;
                                }
                        }

                        at++;
                }

                if (at == start ||
                    !conditional_add(start, (positive)(at - start)))
                        return false;
        }

        return true;
}

static PURE bool conditional_is(string_address word)
{
        return conditional_at < conditional_word_count &&
               word_is(conditional_word[conditional_at], word);
}

static string_address conditional_expand(string_address word, bool pattern)
{
        return pattern ? shell_expand_pattern(word) : shell_expand_word(word);
}

static bool conditional_integer(positive kind, string_address left,
                                string_address right)
{
        bool held = arith_bash_mode;
        bool held_nounset = arith_nounset;
        bipolar first;
        bipolar second;

        arith_bash_mode = true;
        arith_nounset =
            (shell_options & ((positive)1 << ('u' - 'a'))) != 0;
        arith_unset = false;
        first = arith_evaluate(left);

        if (arith_bad)
        {
                arith_bash_mode = held;
                arith_nounset = held_nounset;
                conditional_bad = true;
                return false;
        }

        second = arith_evaluate(right);
        arith_bash_mode = held;
        arith_nounset = held_nounset;

        if (arith_bad)
        {
                conditional_bad = true;
                return false;
        }

        return test_ordered(kind, first, second);
}

static COLD fn conditional_nounset_fatal()
{
        string_format(exec_error, "arithmetic: parameter not set\n");
        shell_status = 1;

        if (shell_is_interactive)
        {
                exec_expand_fatal();
                return;
        }

        if (!expand_in_substitution)
                shell_trap_exit();

        log_flush();
        system_call_1(syscall(exit_group), 1);
}

/*
        The regex engine is shared by grep, sed, AWK, and the shell. A [[ =~ ]]
        compile is transient: restore both the selected program and every pool
        watermark afterward so repeated conditions cannot consume or retarget
        another builtin's cached programs.
*/
/*
        BASH_REMATCH: what matched, then one element per capture group.

        The engine's slot pairs are byte offsets into the text it was handed,
        so the substrings are cut while that text and those slots are still
        the ones this match left behind -- the caller puts the whole engine
        state back immediately afterwards. A group that took no part has no
        offsets, and Bash gives it an empty element rather than a hole.
*/
static COLD fn conditional_regex_captures(string_address text)
{
        shell_mark held = shell_store_mark(address_of expand_store);
        positive count = (positive)regex_group_count + 1;
        string_address address_to words =
            (string_address address_to)shell_store_take(
                address_of expand_store, count * sizeof(words[0]));

        if (!words)
                return;

        for (positive at = 0; at < count; at++)
        {
                positive from = regex_slots[at * 2];
                positive to = regex_slots[at * 2 + 1];
                p8 address_to made;

                if (from == positive_max || to == positive_max || from > to)
                {
                        words[at] = (string_address) "";
                        continue;
                }

                made = shell_store_take(address_of expand_store,
                                        to - from + 1);

                if (!made)
                {
                        shell_store_rewind(address_of expand_store, held);
                        return;
                }

                memory_copy_end(made, text + from, to - from);
                words[at] = made;
        }

        shell_array_words("BASH_REMATCH", 12, words, count);
        shell_store_rewind(address_of expand_store, held);
}

static bool conditional_regex_match(string_address text, string_address pattern,
                                    bool address_to valid)
{
        regex_program saved;
        b32 code_mark = regex_pool_used;
        b32 set_mark = regex_pool_sets;
        b32 first_mark = regex_first_used;
        b32 set_count = regex_set_count;
        bool escapes = regex_escapes;
        bool broken = regex_broken;
        positive stop = regex_stop_wanted;
        string_address saved_pattern = regex_pattern;
        positive saved_pattern_length = regex_pattern_length;
        positive saved_pattern_at = regex_pattern_at;
        string_address saved_text = regex_text;
        positive saved_text_length = regex_text_length;
        positive slots[REGEX_SLOT_MAX];
        p8 first[256];
        p8 last[256];
        p8 literal[REGEX_LITERAL_MAX];
        bool matched = false;

        regex_capture(address_of saved);
        memory_copy_apart(slots, regex_slots, sizeof(slots));
        memory_copy_apart(first, saved.state.first, sizeof(first));
        memory_copy_apart(last, saved.state.last, sizeof(last));
        memory_copy_apart(literal, saved.state.literal, sizeof(literal));

        address_to valid = regex_compile(pattern, true, false, false,
                                         REGEX_POLICY_DEFAULT);

        if (address_to valid)
        {
                matched = regex_search(text, string_length(text), 0);

                if (matched)
                        conditional_regex_captures(text);
        }

        regex_pool_used = code_mark;
        regex_pool_sets = set_mark;
        regex_first_used = first_mark;
        memory_copy_apart(saved.state.first, first, sizeof(first));
        memory_copy_apart(saved.state.last, last, sizeof(last));
        memory_copy_apart(saved.state.literal, literal, sizeof(literal));
        regex_select(address_of saved);
        regex_set_count = set_count;
        regex_escapes = escapes;
        regex_broken = broken;
        regex_stop_wanted = stop;
        regex_pattern = saved_pattern;
        regex_pattern_length = saved_pattern_length;
        regex_pattern_at = saved_pattern_at;
        regex_text = saved_text;
        regex_text_length = saved_text_length;
        memory_copy_apart(regex_slots, slots, sizeof(slots));
        return matched;
}

static bool conditional_expression();

static bool conditional_binary_ready()
{
        conditional_at++;

        if (conditional_at >= conditional_word_count)
        {
                conditional_bad = true;
                return false;
        }

        if (!conditional_active)
        {
                conditional_at++;
                return false;
        }

        return true;
}

static bool conditional_primary()
{
        string_address raw;

        if (conditional_at >= conditional_word_count)
        {
                conditional_bad = true;
                return false;
        }

        if (conditional_is("("))
        {
                bool value;

                conditional_at++;
                value = conditional_expression();

                if (!conditional_is(")"))
                        conditional_bad = true;
                else
                        conditional_at++;

                return value;
        }

        raw = conditional_word[conditional_at];

        if ((test_is_unary(raw) || word_is(raw, "-a") ||
             word_is(raw, "-v") || word_is(raw, "-o")) &&
            conditional_at + 1 < conditional_word_count)
        {
                string_address operand_raw = conditional_word[conditional_at + 1];
                string_address operand;
                bool value = false;

                conditional_at += 2;

                if (!conditional_active)
                        return false;

                operand = conditional_expand(operand_raw, false);

                if (expand_failed)
                        return false;

                if (word_is(raw, "-a"))
                        return test_unary('e', operand);

                if (word_is(raw, "-v"))
                        return env_get(operand) != null;

                if (word_is(raw, "-o"))
                {
                        positive option = string_table_find(
                            operand, shell_option_names,
                            sizeof(shell_option_names[0]), SHELL_OPTION_NAMES);

                        return option < SHELL_OPTION_NAMES &&
                               shell_option_on(option);
                }

                value = test_unary(string_get(raw + 1), operand);
                return value;
        }

        conditional_at++;

        if (conditional_at < conditional_word_count)
        {
                string_address op = conditional_word[conditional_at];
                positive kind = test_is_binary(op);
                bool pattern = word_is(op, "=") || word_is(op, "==") ||
                               word_is(op, "!=");

                if (word_is(op, "=="))
                        kind = TEST_SAME;

                if (word_is(op, "=~"))
                {
                        string_address left;
                        string_address right;
                        bool valid;
                        bool value;

                        if (!conditional_binary_ready())
                                return false;

                        left = conditional_expand(raw, false);
                        right = shell_expand_regex(
                            conditional_word[conditional_at++]);

                        if (expand_failed)
                                return false;

                        value = conditional_regex_match(left, right,
                                                        address_of valid);

                        if (!valid)
                                conditional_bad = true;

                        return value;
                }

                if (kind)
                {
                        string_address left;
                        string_address right;
                        bool value;

                        if (!conditional_binary_ready())
                                return false;

                        left = conditional_expand(raw, false);
                        right = conditional_expand(
                            conditional_word[conditional_at++], pattern);

                        if (expand_failed)
                                return false;

                        if (pattern)
                        {
                                /*
                                        Both of the things [[ ]] does that
                                        ordinary matching does not: it reads
                                        the extended groups whether or not the
                                        option is on, and it folds case under
                                        nocasematch. The two do not compose --
                                        the extended matcher has no folded
                                        path -- so an extended pattern matches
                                        case-sensitively even under
                                        nocasematch, which is the one
                                        combination this does not answer.
                                */
                                value = glob_extended_anywhere(right)
                                            ? shell_match_extended(right, left)
                                            : shell_match_folded(
                                                  right, left,
                                                  shell_shopt_on(NOCASEMATCH));
                                return word_is(op, "!=") ? !value : value;
                        }

                        if (kind >= TEST_EQUAL && kind <= TEST_GREATER_EQUAL)
                                return conditional_integer(kind, left, right);

                        test_bad = false;
                        value = test_compare(kind, left, right);

                        if (test_bad)
                                conditional_bad = true;

                        return value;
                }
        }

        if (!conditional_active)
                return false;

        raw = conditional_expand(raw, false);
        return !expand_failed && string_get(raw) != end;
}

static bool conditional_negation()
{
        if (conditional_is("!"))
        {
                conditional_at++;
                return !conditional_negation();
        }

        return conditional_primary();
}

#define CONDITIONAL_LOGICAL_LEVEL(name, lower, spelling, wanted, operation)  \
        static bool name()                                                  \
        {                                                                    \
                bool value = lower();                                       \
                                                                             \
                while (conditional_is(spelling))                            \
                {                                                            \
                        bool held = conditional_active;                      \
                                                                             \
                        conditional_at++;                                   \
                        conditional_active = held && (wanted);              \
                        bool other = lower();                               \
                        conditional_active = held;                          \
                        value = value operation other;                       \
                }                                                            \
                                                                             \
                return value;                                                \
        }

CONDITIONAL_LOGICAL_LEVEL(conditional_conjunction, conditional_negation,
                          "&&", value, &&)
CONDITIONAL_LOGICAL_LEVEL(conditional_expression, conditional_conjunction,
                          "||", !value, ||)
#undef CONDITIONAL_LOGICAL_LEVEL

static b32 exec_conditional(b32 index)
{
        shell_mark arena = shell_store_mark(address_of exec_store);
        string_address whole = parse_words[parse_nodes[index].word];
        positive length;
        p8 held;
        bool value = false;
        b32 status;

        if (!exec_bracket_strip(whole, address_of length, address_of held))
                return 2;

        conditional_bad = false;
        conditional_active = true;
        conditional_at = 0;
        expand_failed = false;
        arith_unset = false;

        if (!conditional_tokenize(whole + 2))
                conditional_bad = true;
        else if (conditional_word_count)
                value = conditional_expression();

        if (conditional_at != conditional_word_count || expand_failed)
                conditional_bad = true;

        if (arith_unset)
                conditional_nounset_fatal();

        exec_bracket_restore(whole, length, held);
        status = arith_unset ? 1 : conditional_bad ? 2 : value ? 0 : 1;
        shell_store_rewind(address_of exec_store, arena);
        return status;
}

static b32 exec_case(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        shell_mark mark = shell_store_mark(address_of exec_store);
        string_address subject;
        b32 item;
        b32 status = 0;

        token_used = 0;
        subject = shell_expand_word(parse_words[node->word]);

        if (exec_line_aborted())
                return exec_aborted(mark);

        subject = exec_arena_copy(subject);

        /*
                An item whose predecessor ended in ;& runs without being
                asked, which is the only way a body runs with none of its own
                patterns matching.
        */
        bool falling = false;

        for (item = node->left; item; item = parse_nodes[item].next)
        {
                b32 at;
                bool taken = falling;

                falling = false;

                for (at = 0; !taken && at < parse_nodes[item].word_count; at++)
                {
                        string_address pattern;

                        token_used = 0;
                        pattern = shell_expand_pattern(
                            parse_words[parse_nodes[item].word + at]);

                        if (exec_line_aborted())
                                return exec_aborted(mark);

                        // The structure is fall-through's: an item that
                        // matched is taken, and ;;& goes on testing. The
                        // match is [[ ]]'s, for the same reason case reads
                        // extended groups and folds case.
                        taken = glob_extended_anywhere(pattern)
                                    ? shell_match_extended(pattern, subject)
                                    : shell_match_folded(
                                          pattern, subject,
                                          shell_shopt_on(NOCASEMATCH));
                }

                if (!taken)
                        continue;

                // A matched item with nothing in it ran nothing and answered
                // with whatever came before the case; an empty list of
                // commands succeeds.
                status = parse_nodes[item].right
                             ? exec_node(parse_nodes[item].right)
                             : 0;

                if (parse_nodes[item].flags == CASE_FALL_THROUGH)
                {
                        falling = true;
                        continue;
                }

                // ;;& leaves the remaining patterns to be asked; ;; and the
                // last item of all are the end of the case.
                if (parse_nodes[item].flags != CASE_TEST_ON)
                        break;
        }

        shell_store_rewind(address_of exec_store, mark);

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

        if (system_wait4_retry(child, address_of state, 0, null) < 0)
                return 1;

        return wait_status_code(state);
}

/* Async commands inherit the interactive shell's ignored INT/QUIT state and,
   without job control, read /dev/null unless the command later supplies its
   own redirection. fd 0 is already the desired result from openat and must not
   be closed. A foreground child gets the default back only where the shell's
   own deafness was the reason for the ignore: not over a trap '' in the
   script, and not over an ignore this shell was started with. */
static fn exec_child_signals(bool background, bool null_input)
{
        if (!background)
        {
                shell_child_default(SIGNAL_INTERRUPT);
                shell_child_default(SIGNAL_QUIT);
                return;
        }

        shell_ignore(SIGNAL_INTERRUPT);
        shell_ignore(SIGNAL_QUIT);

        if (null_input)
        {
                bipolar null_handle = system_open_at(AT_FDCWD,
                                                    "/dev/null",
                                                    0);

                if (null_handle > 0)
                {
                        system_duplicate(null_handle, 0, 0);
                        system_close(null_handle);
                }
        }
}

static bipolar exec_spawn_node(b32 index, bool background)
{
        bipolar child;
        bool monitor = job_monitor();

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                b32 status;

                exec_asynchronous = background;
                trap_default_all();
                exec_child_signals(background, background);

                /* Raced from both sides, because either side alone loses:
                   the parent may reach setpgid after the child has exec'd,
                   and the child may be signalled before it has run at all. */
                if (monitor)
                {
                        job_group_set(0, 0);
                        shell_default(JOB_SIGNAL_STOP_KEY);
                        shell_default(JOB_SIGNAL_TTY_INPUT);
                        shell_default(JOB_SIGNAL_TTY_OUTPUT);
                }
                exec_child_began();

                /* A subshell is not the shell whose jobs those are, and its
                   own numbering starts at one again. A command substitution
                   is the exception and keeps them, because `$(jobs -p)` is
                   how a script asks this shell what it is running. */
                job_forget();

                /* The async environment is already a subshell. Turning an
                   explicit (...) node into its equivalent group avoids a
                   second process whose PID would not be $!. */
                if (background && parse_nodes[index].kind == NODE_SUBSHELL)
                        parse_nodes[index].kind = NODE_GROUP;

                shell_tail_command = background &&
                                     parse_nodes[index].kind == NODE_SIMPLE;

                status = exec_node(index);
                exec_child_leave(status);
        }

        if (monitor && child > 0)
                job_group_set(child, child);

        return child;
}

/*
        A pipeline.

        Ordinarily every stage gets a process of its own, because a builtin on
        either end of a pipe has to have its own fd 1 and fd 0. Bash lastpipe
        is the deliberate exception: without job control, an explicitly
        enabled final builtin/function/compound command runs here so that its
        state survives. The same stage loop and status vector serve both
        policies; an eligible external final stage keeps the direct spawn path.

        The parent closes both ends of every pipe it made before it waits: a
        write end still open here is an end of file the reader never sees, and
        the whole shell stops.
*/
/*
        A stage that can be spawned rather than forked, and its pid.

        Every stage of a pipeline is a separate process either way. The fork
        exists only so the child can arrange its own descriptors before
        replacing itself, and it copies a page table to do it. When the stage
        is an ordinary external command the descriptors can be named in the
        request instead, and nothing is copied.

        The conditions are conservative on purpose, because a stage that
        takes this path is expanded here rather than in a child, and anything
        whose expansion could be felt afterwards must not be. Literal words
        with no pattern bytes expand to themselves, so there is nothing to
        feel. A redirection, an assignment prefix, a function, a builtin or
        any word that is not exactly its own text sends the stage back to the
        fork, which is still correct and merely slower.

        Answers -1 for "not this stage", which is not an error: the caller
        forks as it always did.
*/
#define EXEC_PIPE_CLOSE_ON_EXEC 02000000
#define EXEC_STAGE_WORDS_MAX 64

static bipolar exec_stage_spawn(b32 index, b32 input, b32 output)
{
        static p8 address_to found;
        static positive found_room;
        parse_node address_to node = parse_nodes + index;
        string_address words[EXEC_STAGE_WORDS_MAX + 1];
        string_address name;
        positive2 named;
        b32 at;

        if (node->kind != NODE_SIMPLE || node->redirect_count ||
            !node->word_count || node->word_count > EXEC_STAGE_WORDS_MAX)
                return -1;

        for (at = 0; at < node->word_count; at++)
        {
                b32 word = node->word + at;
                string_address text = parse_words[word];

                if (!(parse_word_flags[word] & PARSE_WORD_LITERAL) ||
                    (parse_word_flags[word] & PARSE_WORD_ASSIGNMENT))
                        return -1;

                // A literal word still expands if it is a pattern, and a
                // leading tilde is a home directory. Neither may be answered
                // here, where the answer would be the word itself.
                if (string_first_of_set(text, "*?[~"))
                        return -1;

                words[at] = text;
        }

        words[node->word_count] = null;
        name = words[0];

        // A slash names the program outright; anything else has to prove it
        // is not something this shell would have run itself.
        if (!string_first_of(name, '/'))
        {
                named = string_hash_33_length(name);

                if (exec_function_slot(name, named) != positive_max ||
                    shell_command_named_hashed(name, named) ||
                    exec_control_builtin(name, false))
                        return -1;

                if (shell_find_in_path_alloc(name, address_of found,
                                             address_of found_room) != 1)
                        return -1;

                words[0] = found;
        }

        return shell_spawn_stage(words, input, output, -1);
}

/*
        coproc: a command running alongside this shell with a pipe each way.

        A pipeline hands one command's output to the next and then waits for
        both. A coprocess is the other arrangement -- it keeps running, and
        the shell writes to it and reads from it whenever it likes, which is
        what makes a long-lived helper process possible at all.

        The two descriptors are published as an array because that is what
        they are: NAME[0] is read from and NAME[1] is written to, and NAME_PID
        is who is at the other end.
*/
#define COPROC_FLOOR 60

// Out of the way of the script's own descriptors and out of the way of the
// commands it runs. A coprocess that could see its own pipe ends through
// somebody else's child would wait for an end of file that never came.
static b32 coproc_kept(b32 descriptor)
{
        bipolar moved = system_call_3(syscall(fcntl), (positive)descriptor,
                                      F_DUPFD_CLOEXEC, COPROC_FLOOR);

        if (moved < 0)
                return descriptor;

        system_close(descriptor);

        return (b32)moved;
}

static b32 exec_coproc(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        string_address name = parse_words[node->word];
        positive name_length = string_length(name);
        p8 pid_name[EXEC_COPROC_NAME + 5];
        p8 written[32];
        b32 into[2];
        b32 from[2];
        bipolar pair[2];
        bipolar child;

        if (name_length > EXEC_COPROC_NAME)
        {
                string_format(exec_error, "coproc: %s: name too long\n", name);
                return 1;
        }

        log_flush();

        if (system_pipe(address_of into, 0) < 0)
        {
                string_format(exec_error, "coproc: no pipe\n");
                return 1;
        }

        if (system_pipe(address_of from, 0) < 0)
        {
                system_close(into[0]);
                system_close(into[1]);
                string_format(exec_error, "coproc: no pipe\n");
                return 1;
        }

        child = exec_stage_spawn(node->left, into[0], from[1]);

        if (child < 0)
                child = shell_clone();

        if (child == 0)
        {
                trap_default_all();
                exec_asynchronous = true;
                exec_child_signals(true, false);
                exec_child_began();

                if (parse_nodes[node->left].kind == NODE_SUBSHELL)
                        parse_nodes[node->left].kind = NODE_GROUP;

                system_close(into[1]);
                system_close(from[0]);
                system_duplicate(into[0], standard_input_descriptor, 0);
                system_close(into[0]);
                system_duplicate(from[1], standard_output_descriptor, 0);
                system_close(from[1]);

                b32 status = exec_node(node->left);

                exec_child_leave(status);
        }

        system_close(into[0]);
        system_close(from[1]);

        if (child < 0)
        {
                system_close(into[1]);
                system_close(from[0]);
                string_format(exec_error, "coproc: cannot fork\n");
                return 1;
        }

        pair[0] = coproc_kept(from[0]);
        pair[1] = coproc_kept(into[1]);

        if (!shell_array_numbers(name, name_length, pair, 2))
                string_format(exec_error, "coproc: %s: cannot assign\n", name);

        memory_copy(pid_name, name, name_length);
        memory_copy_end(pid_name + name_length, (string_address) "_PID", 4);
        bipolar_into_string(written, child);

        if (!env_assign(pid_name, written))
                string_format(exec_error, "coproc: %s: cannot assign\n",
                              pid_name);

        // Registered the way a background command is, so that $! names it and
        // wait can be told what it answered.
        if (!shell_background_started(address_of child, 1, false, false))
                string_format(exec_error, "No room to retain coprocess\n");

        return 0;
}

static b32 exec_pipe(b32 first, positive count, bool background,
                     bool pipefail, bool invert)
{
        bipolar address_to children = null;
        positive children_room = 0;
        bipolar upstream = -1;
        b32 child = first;
        positive started = 0;
        b32 status = 0;
        positive at;
        bool spawn_failed = false;
        bool monitor = job_monitor();
        bool lastpipe = !background && !monitor &&
                        shell_shopt_on(LASTPIPE);
        bool lastpipe_ran = false;
        b32 lastpipe_status = 0;
        b32 lastpipe_mark = exec_save_count;
        bipolar group = 0;

        if (count > positive_max / sizeof(children[0]) ||
            !shell_array_room(children, children_room, count))
        {
                string_format(exec_error, "No room for pipeline\n");
                return 2;
        }

        /* Save before making a pipe: when the shell arrived with fd 0 closed,
           pipe may legitimately allocate that number. Saving at the final
           stage would then preserve the pipe instead of the original closed
           state. The save is CLOEXEC and so costs spawned stages no lifetime. */
        if (lastpipe)
        {
                b32 final = first;

                while (parse_nodes[final].next)
                        final = parse_nodes[final].next;

                if (!exec_save_fd(0, parse_nodes + final))
                {
                        memory_free(children,
                                    children_room * sizeof(children[0]));
                        return 2;
                }
        }

        while (child && started < count)
        {
                b32 ends[2];
                bipolar made;
                bool last = started + 1 >= count || !parse_nodes[child].next;

                ends[0] = -1;
                ends[1] = -1;

                /* Close-on-exec, because a spawned stage inherits a copy
                   of this shell's descriptors and a reader holding its own
                   write end waits for an end of file that never comes. The
                   forked path is unaffected: it duplicates the ends it wants
                   onto 0 and 1, and a duplicate does not carry the flag. */
                if (!last && system_pipe(ends, EXEC_PIPE_CLOSE_ON_EXEC) < 0)
                {
                        spawn_failed = true;
                        break;
                }

                log_flush();

                /* A spawned stage never runs a line of this shell, so only
                   the parent could put it in the job's group -- and by the
                   time the request has returned it may already have exec'd,
                   at which point setpgid is refused. Under the monitor the
                   stage is forked, which is slower and has two sides. */
                made = monitor
                           ? -1
                           : exec_stage_spawn(child, upstream,
                                              last ? -1 : ends[1]);

                /* A final command which was not an eligible literal external
                   is exactly the stateful lastpipe case. Run it before waiting
                   so a producer cannot fill the pipe against an idle reader.
                   Keep the caller's tested state: ! and conditional lists
                   suppress -e inside their pipeline, while an untested final
                   compound command must still stop at its first failing
                   simple command. Tail exec is suppressed because the shell
                   has pipeline bookkeeping left to do when the stage returns. */
                if (made < 0 && last && lastpipe)
                {
                        bool tail = shell_tail_command;

                        if (upstream >= 0 && upstream != 0)
                        {
                                if (system_duplicate(upstream, 0, 0) < 0)
                                {
                                        system_close(upstream);
                                        upstream = -1;
                                        lastpipe_status = 2;
                                        lastpipe_ran = true;
                                        string_format(exec_error,
                                                      "Cannot connect pipeline\n");
                                        break;
                                }

                                system_close(upstream);
                        }

                        upstream = -1;
                        shell_tail_command = false;
                        lastpipe_status = exec_node(child);
                        shell_tail_command = tail;
                        lastpipe_ran = true;
                        child = parse_nodes[child].next;
                        break;
                }

                if (made < 0)
                        made = shell_clone();

                if (made == 0)
                {
                        trap_default_all();

                        if (monitor)
                        {
                                job_group_set(0, group);
                                shell_default(JOB_SIGNAL_STOP_KEY);
                                shell_default(JOB_SIGNAL_TTY_INPUT);
                                shell_default(JOB_SIGNAL_TTY_OUTPUT);
                        }

                        if (!trap_ignored(SIGNAL_PIPE))
                                shell_default(SIGNAL_PIPE);
                        exec_asynchronous = background;
                        exec_child_signals(background,
                                           background && upstream < 0);
                        exec_child_began();
                        if (parse_nodes[child].kind == NODE_SUBSHELL)
                                parse_nodes[child].kind = NODE_GROUP;
                        shell_tail_command =
                            parse_nodes[child].kind == NODE_SIMPLE;

                        if (upstream >= 0)
                        {
                                system_duplicate(upstream, 0, 0);
                                system_close(upstream);
                        }

                        if (!last)
                        {
                                system_close(ends[0]);
                                system_duplicate(ends[1], 1, 0);
                                system_close(ends[1]);
                        }

                        status = exec_node(child);
                        exec_child_leave(status);
                }

                if (made < 0)
                {
                        if (upstream >= 0)
                                system_close(upstream);

                        if (!last)
                        {
                                system_close(ends[0]);
                                system_close(ends[1]);
                        }

                        upstream = -1;
                        spawn_failed = true;
                        break;
                }

                if (upstream >= 0)
                        system_close(upstream);

                upstream = -1;

                if (!last)
                {
                        system_close(ends[1]);
                        upstream = ends[0];
                }

                if (monitor)
                {
                        if (!group)
                                group = made;

                        job_group_set(made, group);
                }

                children[started++] = made;
                child = parse_nodes[child].next;
        }

        if (lastpipe)
                exec_redirect_restore(lastpipe_mark);

        if (upstream >= 0)
                system_close(upstream);

        if (background && !spawn_failed)
        {
                if (!shell_background_started(children, started, pipefail,
                                              invert))
                {
                        string_format(exec_error,
                                      "No room to retain background pipeline\n");
                        status = 2;
                }
                else
                        job_started(children, started, group, first, true,
                                    true);

                memory_free(children,
                            children_room * sizeof(children[0]));
                return status;
        }

        //
        //      Every stage is waited for either way, because a pipeline that
        //      left a child unreaped would leave a zombie per turn of a loop.
        //      What pipefail changes is which of the answers is kept.
        //
        //      pipefail: the status is that of the rightmost command to exit
        //      non-zero, or zero if none did. Walking left to right, the last non-zero
        //      seen is the rightmost one, so one variable holds it.
        //
        b32 rightmost_failure = 0;
        bool stopped = false;
        positive stopped_by = 0;

        if (monitor)
        {
                job_hold(children, started);
                job_terminal_give(group);
        }

        for (at = 0; at < started; at++)
        {
                b32 got;

                if (monitor)
                {
                        positive raw = 0;

                        if (system_wait4_retry(children[at], address_of raw,
                                               JOB_UNTRACED, null) < 0)
                                got = 1;
                        else if ((raw & 0xff) == 0x7f)
                        {
                                stopped = true;
                                stopped_by = (raw >> 8) & 0xff;
                                got = 128 + (b32)stopped_by;
                        }
                        else
                                got = wait_status_code(raw);
                }
                else
                        got = exec_child_status(children[at]);

                if (got)
                        rightmost_failure = got;

                if (at + 1 == started)
                        status = got;

                // Every stage's answer, in the order they were written
                // in. The child ids are not wanted for anything else once
                // the last of them has been waited for, so the answers go
                // back into the vector that held them.
                children[at] = got;
        }

        /* The parent-run stage has no pid to wait for, but it is still the
           rightmost logical stage and gets the final slot of PIPESTATUS. */
        if (lastpipe_ran)
        {
                children[started] = lastpipe_status;
                started++;
                status = lastpipe_status;

                if (lastpipe_status)
                        rightmost_failure = lastpipe_status;
        }

        if (monitor)
                job_terminal_give(job_shell_group);

        /* A pipeline stopped in front is a job from that moment on: it is
           listed, it is what `fg` means, and the number it answers with is the
           one POSIX gives a command that stopped rather than ended. */
        if (stopped && job_hold(job_held, started))
        {
                positive number =
                    job_retain(job_held, started, pipefail, invert, false)
                        ? job_started(job_held, started, group, first, true,
                                      false)
                        : 0;
                positive slot = number ? job_find_number(number) : job_count;

                memory_free(children, children_room * sizeof(children[0]));

                if (slot >= job_count)
                        return 128 + (b32)stopped_by;

                job_table[slot].state = JOB_STOPPED;
                job_table[slot].stopped_by = stopped_by;
                job_table[slot].reported = true;
                job_line(log, job_table + slot, false);
                log_flush();

                return 128 + (b32)stopped_by;
        }

        exec_pipe_status_pending = false;
        exec_pipe_status_publish(children, started);

        if (rightmost_failure && pipefail)
                status = rightmost_failure;

        if (spawn_failed)
        {
                string_format(exec_error, "Cannot start pipeline\n");
                status = 2;
        }

        memory_free(children, children_room * sizeof(children[0]));

        return status;
}

// How many commands a pipeline has, or positive_max when there is no
// counting them.
static positive exec_pipeline_count(b32 first)
{
        positive count = 0;

        for (b32 stage = first; stage; stage = parse_nodes[stage].next)
        {
                if (count == positive_max)
                {
                        string_format(exec_error, "Pipeline too long\n");
                        return positive_max;
                }

                count++;
        }

        return count;
}

/* A simple command always replaces Bash's PIPESTATUS, but almost no command
   reads it. Keep the one integer in registers/data until an environment or
   array reader actually asks; that reader calls the public materializer
   below. A real pipeline publishes its already-built vector directly. */
static fn exec_pipe_status_publish(bipolar address_to values, positive count)
{
        bool locked;

        /* `readonly PIPESTATUS` with no value stays an unset readonly array
           in Bash. An existing readonly vector remains shell-owned and is
           refreshed normally, so only the absent case refuses creation. */
        locked = env_readonly("PIPESTATUS");

        if (locked && !shell_array_length("PIPESTATUS", 10))
                return;

        /* Ordinary writes must respect readonly. This is the shell updating
           its own status register, which Bash permits once that register has
           a value; lift and restore only that attribute around the common
           array writer rather than adding a second internal write engine. */
        if (locked)
                shell_variable_attribute_set("PIPESTATUS", 10, 0,
                                             SHELL_ARRAY_READONLY);

        shell_status_array_numbers("PIPESTATUS", 10, values, count);

        if (locked)
                shell_variable_attribute_set("PIPESTATUS", 10,
                                             SHELL_ARRAY_READONLY, 0);
}

fn exec_pipe_status_wanted()
{
        bipolar value;

        if (!exec_pipe_status_pending)
                return;

        exec_pipe_status_pending = false;
        value = exec_pipe_status_value;
        exec_pipe_status_publish(address_of value, 1);
}

static fn exec_pipe_status_one(b32 status)
{
        exec_pipe_status_value = status;
        exec_pipe_status_pending = true;
}

/*
        The time reserved word.

        What it measures is a whole pipeline and not a command, which is why
        it is grammar and not a utility: an external /usr/bin/time can only
        ever time one program, and "time a | b" is two.

        Three numbers come out of it. The real one is the monotonic clock,
        which nothing else can move; the two processor ones are this shell's
        own use plus every child it has reaped, because the work a pipeline
        did in its stages is work the pipeline did.
*/

// The front of struct rusage, which is where the two times are. The rest is
// counters nobody here asks after, and the kernel writes all of it.
typedef struct
{
        p64 user_seconds;
        p64 user_microseconds;
        p64 system_seconds;
        p64 system_microseconds;
        p64 counters[32];
} time_usage;

#define TIME_USAGE_SELF 0
#define TIME_USAGE_CHILDREN (-1)
#define TIME_MICROSECONDS 1000000

typedef struct
{
        timespec real;
        positive user;
        positive system;
} time_reading;

static fn time_now(time_reading address_to into)
{
        time_usage self;
        time_usage children;

        memory_fill(address_of self, 0, sizeof(self));
        memory_fill(address_of children, 0, sizeof(children));
        memory_fill(into, 0, sizeof(address_to into));

        system_call_2(syscall(clock_gettime), READ_CLOCK_MONOTONIC,
                      (positive)address_of into->real);
        system_call_2(syscall(getrusage), (positive)TIME_USAGE_SELF,
                      (positive)address_of self);
        system_call_2(syscall(getrusage),
                      (positive)(bipolar)TIME_USAGE_CHILDREN,
                      (positive)address_of children);

        into->user = (positive)self.user_seconds * TIME_MICROSECONDS +
                     (positive)self.user_microseconds +
                     (positive)children.user_seconds * TIME_MICROSECONDS +
                     (positive)children.user_microseconds;
        into->system = (positive)self.system_seconds * TIME_MICROSECONDS +
                       (positive)self.system_microseconds +
                       (positive)children.system_seconds * TIME_MICROSECONDS +
                       (positive)children.system_microseconds;
}

// What went by between two readings, in microseconds. A clock that went
// backwards is nothing, rather than an enormous number.
static CONST positive time_apart(positive after, positive before)
{
        return after > before ? after - before : 0;
}

static PURE positive time_real_apart(time_reading address_to after,
                                     time_reading address_to before)
{
        bipolar seconds = (bipolar)after->real.tv_sec -
                          (bipolar)before->real.tv_sec;
        bipolar nanoseconds = (bipolar)after->real.tv_nsec -
                              (bipolar)before->real.tv_nsec;
        bipolar total = seconds * TIME_MICROSECONDS + nanoseconds / 1000;

        return total > 0 ? (positive)total : 0;
}

// Ten to the places, for every number of places a format may ask for.
static CONST positive time_scale(positive places)
{
        static const positive powers[] = {1,     10,     100, 1000,
                                          10000, 100000, 1000000};

        return powers[places];
}

/*
        One time, rounded to the places asked for and written out.

        Rounding rather than cutting is what Bash's printf does, and the
        difference shows at once: five microseconds at five places is one
        hundred-thousandth of a second and not nothing.
*/
static fn time_number(positive microseconds, positive places, bool minutes)
{
        positive scale = time_scale(places);
        positive divisor = TIME_MICROSECONDS / scale;
        positive scaled = (microseconds + divisor / 2) / divisor;
        positive whole = scaled / scale;
        positive fraction = scaled % scale;
        p8 shown[32];
        positive written;

        if (minutes)
        {
                written = bipolar_into_string(shown, (bipolar)(whole / 60));
                exec_built_add(shown, written);
                exec_built_add((string_address) "m", 1);
                whole %= 60;
        }

        written = bipolar_into_string(shown, (bipolar)whole);
        exec_built_add(shown, written);

        if (places)
        {
                exec_built_add((string_address) ".", 1);
                written = positive_into_padded(shown, fraction, places, '0');
                exec_built_add(shown, written);
        }

        if (minutes)
                exec_built_add((string_address) "s", 1);
}

/*
        TIMEFORMAT, written out.

        %R %U %S are the three times and %P the share of the elapsed time that
        went on a processor. A digit in front of the letter says how many
        places follow the point, an l asks for the minutes to be taken out in
        front, and %% is a percent. Everything else is a byte of the line.

        A letter that is none of those is refused and nothing at all is
        written, which is what Bash does: half a timing is worse than none,
        and a format nobody can read is a mistake somebody wants told.
*/
static bool time_formatted(string_address format, positive real,
                           positive user, positive system)
{
        string_address at = format;

        exec_built_used = 0;

        while (string_get(at))
        {
                positive places = 3;
                bool given = false;
                bool minutes = false;
                p8 letter;

                if (string_not(at, '%'))
                {
                        exec_built_add(at, 1);
                        at++;
                        continue;
                }

                at++;

                // A percent with nothing behind it is a percent.
                if (!string_get(at))
                {
                        exec_built_add((string_address) "%", 1);
                        break;
                }

                if (string_get(at) >= '0' && string_get(at) <= '9')
                {
                        places = (positive)(string_get(at) - '0');
                        given = true;
                        at++;

                        // Microseconds are as fine as the two clocks are, so
                        // asking for more places asks for zeroes.
                        if (places > 6)
                                places = 6;
                }

                if (string_is(at, 'l'))
                {
                        minutes = true;
                        at++;
                }

                letter = string_get(at);

                if (letter == 'R')
                        time_number(real, places, minutes);
                else if (letter == 'U')
                        time_number(user, places, minutes);
                else if (letter == 'S')
                        time_number(system, places, minutes);
                else if (letter == '%' && !given && !minutes)
                        exec_built_add((string_address) "%", 1);
                else if (letter == 'P' && !given && !minutes)
                {
                        /*
                                Two times over a third, in hundredths of a
                                percent so that the places come out of one
                                division. A third of nothing is nothing:
                                elapsed time can be under the clock's own
                                resolution and then there is no share to give.
                        */
                        positive spent = user + system;
                        positive parts = real ? spent * 10000 / real : 0;
                        p8 shown[32];
                        positive written =
                            bipolar_into_string(shown, (bipolar)(parts / 100));

                        exec_built_add(shown, written);
                        exec_built_add((string_address) ".", 1);
                        written = positive_into_padded(shown, parts % 100, 2,
                                                       '0');
                        exec_built_add(shown, written);
                }
                else
                {
                        p8 shown[2];

                        shown[0] = letter;
                        shown[1] = end;

                        string_format(
                            exec_error,
                            "TIMEFORMAT: `%s': invalid format character\n",
                            shown);

                        return false;
                }

                at++;
        }

        exec_built_add((string_address) "\n", 1);

        return true;
}

static fn time_written(bool posix, positive real, positive user,
                       positive system)
{
        string_address format = env_get((const_string) "TIMEFORMAT");

        if (posix)
                format = (string_address) "real %2R\nuser %2U\nsys %2S";
        else if (!format)
                format = (string_address) "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";

        // An empty TIMEFORMAT asks for no timing at all, which is not the
        // same as asking for the usual one.
        if (!string_get(format))
                return;

        if (!time_formatted(format, real, user, system))
                return;

        // Whatever the timed command wrote comes first. It has already
        // happened; only the buffer is holding it back.
        log_flush();
        log_error(exec_built, exec_built_used);
}

static b32 exec_time(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        bool tested = exec_tested;
        time_reading before;
        time_reading after;
        b32 status;

        time_now(address_of before);

        // A bang in front of a time inverts what the whole of it answers, so
        // nothing inside it is what errexit is looking at.
        if (node->op)
                exec_tested = true;

        status = node->left ? exec_node(node->left) : 0;

        exec_tested = tested;
        time_now(address_of after);

        time_written(node->flags,
                     time_real_apart(address_of after, address_of before),
                     time_apart(after.user, before.user),
                     time_apart(after.system, before.system));

        if (exec_line_aborted())
        {
                shell_status = 2;
                return 2;
        }

        if (node->op)
                status = status ? 0 : 1;

        shell_status = status;

        return status;
}

static b32 exec_pipeline(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        bool tested = exec_tested;
        positive count = exec_pipeline_count(node->left);
        b32 status;

        if (count == positive_max)
        {
                shell_status = 2;
                return 2;
        }

        if (node->flags)
                exec_tested = true;

        status = count > 1
                     ? exec_pipe(node->left, count, false, shell_pipefail(),
                                 false)
                     : exec_node(node->left);

        exec_tested = tested;

        if (exec_line_aborted())
        {
                shell_status = 2;
                return 2;
        }

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
        b32 body = parse_nodes[index].left;
        bipolar child;

        /* A singleton gets no semantic value from its background-marker
           AND-OR wrapper. Launching the body itself lets an external command
           tail-exec into the PID published as $!, and lets a pipeline publish
           the PID of its last stage as POSIX requires. */
        if (body && !parse_nodes[body].next)
        {
                if (parse_nodes[body].kind == NODE_PIPELINE)
                {
                        positive count =
                            exec_pipeline_count(parse_nodes[body].left);

                        if (count == positive_max)
                                return 2;

                        return exec_pipe(parse_nodes[body].left, count, true,
                                         shell_pipefail(),
                                         parse_nodes[body].flags);
                }

                index = body;
        }

        child = exec_spawn_node(index, true);

        if (child < 0)
                return 1;

        if (!shell_background_started(address_of child, 1, false, false))
        {
                string_format(exec_error,
                              "No room to retain background command\n");
                return 2;
        }

        job_started(address_of child, 1, job_monitor() ? child : 0, index,
                    false, true);

        return 0;
}

static b32 exec_list(b32 index)
{
        b32 child = parse_nodes[index].left;
        b32 status = 0;

        while (child)
        {
                bool background = parse_nodes[child].kind == NODE_ANDOR &&
                                  parse_nodes[child].flags;

                // Starting a command in the background is a command with a
                // status of its own, which is what $? reads next. Every
                // other kind of node writes shell_status on its way out.
                if (background)
                        status = shell_status = exec_background(child);
                else
                        status = exec_node(child);

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
        /* -n still parses the entire physical program before it gets here.
           Once a command in that program enables it, later sibling nodes are
           skipped as Bash and dash skip the rest of the already parsed list. */
        if (!shell_is_interactive && (shell_options & SHELL_NOEXEC))
                return shell_status;

        b32 status = exec_node_kind(index);

        /* Signals are rare and node boundaries are not. Keep the volatile
           byte read on the hot path, but do not enter exec_traps merely to
           discover that no handler has written it. Checking abort state is
           likewise unnecessary until there is a trap to run. */
        if (trap_caught && !trap_inside && !exec_line_aborted())
                exec_traps();

        return status;
}

static b32 exec_node_kind(b32 index)
{
        /* The count is read first and it is an ordinary word: a shell with
           nothing in the background never touches the volatile flag beside
           it, so the notice costs one load and a branch not taken. */
        if (job_count && job_child_news)
                job_notice();

        parse_node address_to node;
        shell_mark expanded;
        b32 mark;
        b32 status;

        if (!index)
                return shell_status;

        node = parse_nodes + index;

        /*
                What a process substitution opened belongs to the command that
                was handed the path, and to nothing inside it: a function given
                /dev/fd/N as an argument may not open it until its third
                command, so the mark is taken here and given back only when
                this whole command is done with.

                Until something makes one there is nothing to mark, which is
                every command of almost every script.
        */
        positive substitutions =
            expand_substitutions_ever ? expand_substitutions_count : 0;

        if (node->kind == NODE_SIMPLE)
        {
                // Before the words are expanded, which is where Bash runs it
                // and the only place the action can use argv of its own.
                if (trap_debug_here &&
                    exec_condition_reaches(SHELL_EXTRA_FUNCTRACE))
                        exec_trap_condition(TRAP_DEBUG);

                expanded = shell_store_mark(address_of expand_store);

                status = exec_simple(index);
                shell_store_rewind(address_of expand_store, expanded);

                if (expand_substitutions_ever &&
                    expand_substitutions_count != substitutions)
                        shell_substitutions_close(substitutions);

                shell_status = status;

                /* Bash exposes a pipeline vector only until the next simple
                   command. Expansion of that command has already read the old
                   vector; publishing its one answer here gives assignments
                   and commands the same lifetime rule. Do this before ERR and
                   EXIT traps so they see the failing command's answer. The
                   pipeline overwrites the transient value after a returning
                   parent-run lastpipe stage. */
                if (shell_bash_compat)
                        exec_pipe_status_one(status);

                exec_errexit(status);

                return status;
        }

        exec_line = node->line;

        if (node->kind == NODE_LIST)
                return exec_list(index);

        if (node->kind == NODE_ANDOR)
                return exec_and_or(index);

        if (node->kind == NODE_PIPELINE)
                return exec_pipeline(index);

        // A time carries no redirections of its own: what it measures does.
        if (node->kind == NODE_TIME)
                return exec_time(index);

        if (node->kind == NODE_FUNCTION)
                return exec_define(index);

        // Everything left is a compound command, and every one of them can
        // carry redirections of its own. Its expansions live until that
        // command returns: a for list and case subject span child commands,
        // while a redirect target dies as soon as its descriptor is open.
        expanded = shell_store_mark(address_of expand_store);
        mark = exec_save_count;
        token_used = 0;

        if (node->redirect_count && !exec_redirect_apply(index))
        {
                exec_redirect_restore(mark);
                shell_store_rewind(address_of expand_store, expanded);

                if (expand_substitutions_ever &&
                    expand_substitutions_count != substitutions)
                        shell_substitutions_close(substitutions);

                shell_status = exec_redirect_failed_status();
                return shell_status;
        }

        if (node->kind == NODE_ARITHMETIC)
                status = exec_arithmetic_command(index);
        else if (node->kind == NODE_CONDITIONAL)
                status = exec_conditional(index);
        else if (node->kind == NODE_IF)
                status = exec_if(index);
        else if (node->kind == NODE_WHILE)
                status = exec_loop(index, false);
        else if (node->kind == NODE_UNTIL)
                status = exec_loop(index, true);
        else if (node->kind == NODE_FOR)
                status = exec_for(index);
        else if (node->kind == NODE_SELECT)
                status = exec_select(index);
        else if (node->kind == NODE_COPROC)
                status = exec_coproc(index);
        else if (node->kind == NODE_CFOR)
                status = exec_cfor(index);
        else if (node->kind == NODE_CASE)
                status = exec_case(index);
        else if (node->kind == NODE_SUBSHELL)
        {
                bipolar made = exec_spawn_node(parse_nodes[index].left, false);

                status = job_monitor() ? job_foreground_child(made, index)
                                       : exec_child_status(made);
        }
        else
                status = exec_node(node->left);

        exec_redirect_restore(mark);
        shell_store_rewind(address_of expand_store, expanded);

        if (expand_substitutions_ever &&
            expand_substitutions_count != substitutions)
                shell_substitutions_close(substitutions);

        shell_status = status;

        if (node->kind == NODE_SUBSHELL || node->kind == NODE_ARITHMETIC ||
            node->kind == NODE_CONDITIONAL)
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
        shell_mark kept_arena = shell_store_mark(address_of exec_store);
        b32 kept_saves = exec_save_count;

        exec_signal = EXEC_SIGNAL_NONE;
        exec_signal_level = 0;

        // Reap without forgetting: wait still owes the status to the script.
        //
        // Only when something was started in the background. This runs at the
        // top of every complete command, so a script that never forked one
        // was paying a wait4 per line to be told it has no children.
        if (!exec_depth && (shell_wait_count || job_count))
        {
                job_reap();
                job_report();
        }

        exec_depth++;

        // A one-command list has no NODE_LIST wrapper. Its trailing ampersand
        // still belongs to the list, and must not become synchronous merely
        // because no second command followed it on the same physical line.
        if (root && parse_nodes[root].kind == NODE_ANDOR &&
            parse_nodes[root].flags)
                shell_status = exec_background(root);
        else
                exec_node(root);

        exec_depth--;

        shell_store_rewind(address_of exec_store, kept_arena);
        exec_save_count = kept_saves;
}
