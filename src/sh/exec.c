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

static bool exec_tested;
static bool exec_forked;
static bool exec_asynchronous;

fn shell_trap_exit();
fn exec_traps();

fn exec_child_began()
{
        exec_forked = true;
        shell_background_child();
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
static bool exec_source_stop()
{
        if (!exec_signal)
                return false;

        if (exec_signal == EXEC_SIGNAL_RETURN)
                exec_signal = EXEC_SIGNAL_NONE;

        return true;
}

static fn exec_errexit(b32 status)
{
        if (exec_line_aborted() || !status || exec_tested ||
            !(shell_options & SHELL_ERREXIT))
                return;

        shell_status = status;

        if (!exec_forked)
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

#define F_DUPFD_CLOEXEC 1030
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
        // Every byte that means nothing in a body. The run from one that
        // does to the next is one copy rather than a decision per byte.
        static b8 plain[STRING_SET_BYTES];
        static bool plain_ready;
        positive start = token_used;
        positive at = 0;

        if (!plain_ready)
        {
                memory_fill(plain + 1, 1, STRING_SET_BYTES - 1);
                plain['\\'] = 0;
                plain['$'] = 0;
                plain_ready = true;
        }

        while (at < length)
        {
                p8 value = string_get(body + at);
                positive run = string_span_max(body + at, length - at, plain);

                if (run)
                {
                        token_push_bytes(body + at, run);
                        at += run;
                        continue;
                }

                if (value == '\\' && at + 1 < length)
                {
                        p8 next = string_get(body + at + 1);

                        // A backslash carries the three bytes that would
                        // otherwise expand, and takes a newline out with
                        // itself: an unquoted body joins lines the way the
                        // command language does, and the pair used to stay
                        // in the body as two bytes.
                        if (next == '\n')
                        {
                                at += 2;
                                continue;
                        }

                        if (next == '$' || next == '`' || next == '\\')
                        {
                                token_push(next);
                                at += 2;
                                continue;
                        }
                }

                if (value == '$')
                {
                        string_address expanded;
                        positive expanded_length;
                        bool expanded_overflow;

                        at = (positive)(shell_expand_here_dollar(
                                            body + at, address_of expanded,
                                            address_of expanded_length,
                                            address_of expanded_overflow) -
                                        body);

                        if (exec_line_aborted())
                                break;

                        if (expanded_overflow)
                                token_overflow = true;

                        token_push_bytes(expanded, expanded_length);

                        continue;
                }

                token_push(value);
                at++;
        }

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

        exec_redirect_status = 0;

        for (at = 0; at < node->redirect_count; at++)
        {
                parse_redirect address_to want = parse_redirects + node->redirect + at;
                // A here-document's word is its delimiter, which only ever
                // has its quotes taken off: nothing in it runs, in POSIX or
                // in dash, and the body was matched against it when the
                // line was read. Expanding it here ran the substitution in
                // "cat <<$(x)" once per command, for nothing.
                string_address target = want->op == OP_DLESS
                                            ? want->text
                                            : shell_expand_word(want->text);
                bipolar opened = -1;
                b32 redirect_mark = exec_save_count;
                bool both = want->op == OP_ANDGREAT || want->op == OP_ANDDGREAT;

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
                        // A redirection is part of the shell language, not a
                        // command reporting an ordinary false result. dash
                        // uses status two for an open/duplication failure.
                        exec_redirect_status = 2;
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
                                exec_redirect_status = 2;
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
static positive exec_function_room;
static positive exec_function_count;
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

static b32 exec_call(positive slot)
{
        b32 body = exec_functions[slot].body;
        positive saved_count = shell_parameter_count;
        positive saved;
        b32 status;

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
        status = exec_node(body);
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

static bool exec_assign(string_address address_to word_at,
                        positive name_length, positive name_hash, bool append)
{
        string_address word = address_to word_at;
        string_address name_end = word + name_length;
        string_address mark = name_end + append;
        string_address old;
        string_address made = word;
        bool answer;

        if (string_get(mark) != '=')
                return false;

        address_to name_end = end;

        if (env_readonly(word))
        {
                string_format(exec_error, "%s: is read only\n", word);
                address_to name_end = append ? '+' : '=';
                expand_fatal();
                return false;
        }

        old = append ? env_get(word) : null;

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

        answer = env_assign_hashed_span(
            word, name_length, name_hash,
            append ? made + name_length + 1 : mark + 1);
        address_to name_end = append ? '+' : '=';

        if (answer && append)
                address_to word_at = made;

        return answer;
}

/*
        The fifteen names POSIX calls special.

        What makes them special here is only that an assignment written in
        front of one outlives it: "x=1 export y" leaves x set and "x=1 cat"
        does not. Everything else about them is the same as any other builtin.
*/
static bool exec_special_builtin(string_address name)
{
        static string_address names[] = {
            ":", ".", "break", "continue", "eval", "exec", "exit", "export",
            "readonly", "return", "set", "shift", "times", "trap", "unset",
        };

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
typedef struct
{
        string_address name;
        positive name_length;
        string_address value;
        bool exported;
} exec_kept_value;

static bool exec_keep_value(exec_kept_value address_to kept, string_address word)
{
        positive length = (positive)(string_first_of_or_end(word, '=') - word);
        string_address value;

        if (length && word[length - 1] == '+')
                length--;

        kept->name = shell_store_take(address_of exec_store, length + 1);

        if (!kept->name)
                return false;

        string_copy_max_end(kept->name, word, length);
        kept->name_length = length;

        value = env_get_span(kept->name, length);
        kept->value = null;
        kept->exported = env_export_active_span(kept->name, length);

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
        A control-flow operand that has to fit in b32 without wrapping.

        string_digits_exact deliberately accepts an arbitrarily long decimal
        spelling and wraps its accumulator; file sizes and similar callers
        impose their own policy afterward. That is not enough here, because a
        huge return operand wrapping to zero is silent success. Strip zeroes
        with the floor routine, reject values wider than INT_MAX before the
        conversion, then let the exact parser prove every remaining byte.
*/
static bool exec_control_number(string_address word, bool allow_zero,
                                b32 address_to answer)
{
        static p8 maximum[] = "2147483647";
        positive length = string_length(word);
        positive zeroes = memory_span_byte(word, '0', length);
        positive significant = length - zeroes;
        positive parsed;

        if (significant > 10 ||
            (significant == 10 &&
             string_compare_max(word + zeroes, maximum, 10) > 0) ||
            !string_digits_exact(word, address_of parsed) ||
            (!allow_zero && !parsed))
                return false;

        *answer = (b32)parsed;
        return true;
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

        if (initial == ':' && !string_get(name + 1))
        {
                shell_status = 0;
                return 0;
        }

        if (exec_control_builtin(name, true))
                return shell_status;

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

        slot = exec_function_slot(name, named);

        if (slot != positive_max)
        {
                shell_tail_command = false;
                return exec_call(slot);
        }

        {
                bool tail = shell_tail_command;

                if (shell_builtin(null, named))
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
        b32 status;
        b32 at;
        bool bare_exec;
        shell_words arguments;

        //      argv grows with the line. A command's words are whatever the
        //      expansions made of them, and a directory may hold any number of
        //      names, so there is nothing sensible to clamp this to.
        shell_words_bind(address_of arguments, address_of shell_argv,
                         address_of shell_argv_room);

        token_used = 0;
        token_overflow = false;
        // With no command name, POSIX makes the command's status that of the
        // last command substitution it performed. Each substitution updates
        // this while the words and redirect targets below are expanded.
        shell_substitution_status = 0;

        /* Assignment right hand sides are expanded and assigned from left to
           right: `a=one b=$a` gives b the new value. Keep those provisional
           values out of command-word expansion, then let the ordinary
           persistence/export path below apply them for real. */
        while (leading < node->word_count &&
               (parse_word_flags[node->word + leading] &
                PARSE_WORD_ASSIGNMENT))
                leading++;

        if (leading)
        {
                expanded_kept = (exec_kept_value address_to)shell_store_take(
                    address_of exec_store,
                    (positive)leading * sizeof(expanded_kept[0]));

                if (!expanded_kept)
                {
                        status = 2;
                        goto fail;
                }
        }

        for (at = 0; at < node->word_count; at++)
        {
                b32 word_index = node->word + at;
                string_address word = parse_words[word_index];
                p8 word_flags = parse_word_flags[word_index];
                bool literal = word_flags & PARSE_WORD_LITERAL;
                bool assignment = word_flags & PARSE_WORD_ASSIGNMENT;
                bool leading = count == first && assignment;

                /*
                        Leading assignments and declaration operands are both
                        expanded whole. Only a leading assignment is applied
                        provisionally so the next one can see its value.
                */
                if (leading || (assignment && word_index >= declaration_from))
                {
                        positive value_at =
                            parse_word_name_lengths[word_index] + 1 +
                            ((word_flags & PARSE_WORD_APPEND) != 0);

                        if (!shell_words_add(address_of arguments,
                                             literal ? word
                                                     : shell_expand_assignment(
                                                           word, value_at)))
                                break;

                        count = (b32)arguments.count;

                        if (exec_line_aborted())
                                break;

                        if (leading)
                        {
                                string_address trial = shell_argv[first];

                                if (!exec_keep_value(
                                        expanded_kept + expanded_count,
                                        trial) ||
                                    !exec_assign(
                                        address_of trial,
                                        parse_word_name_lengths[word_index],
                                        parse_word_name_hashes[word_index],
                                        (word_flags & PARSE_WORD_APPEND) != 0))
                                {
                                        status = 2;
                                        goto fail;
                                }

                                expanded_count++;
                                first++;
                        }

                        continue;
                }

                if (expanded_count)
                {
                        exec_put_back(expanded_kept, expanded_count);
                        expanded_count = 0;
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

        if (exec_line_aborted())
        {
                status = shell_status;
                goto fail;
        }

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
        if (first && first != count)
        {
                bool special = exec_special_builtin(shell_argv[first]);
                bool save = !special;

                if (special)
                        for (at = 0; at < first; at++)
                                if (parse_word_flags[node->word + at] &
                                    PARSE_WORD_APPEND)
                                {
                                        save = true;
                                        break;
                                }

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
                                if (special &&
                                    !(parse_word_flags[node->word + at] &
                                      PARSE_WORD_APPEND))
                                        continue;

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

        for (at = 0; at < first; at++)
                if (!exec_assign(shell_argv + at,
                                 parse_word_name_lengths[node->word + at],
                                 parse_word_name_hashes[node->word + at],
                                 (parse_word_flags[node->word + at] &
                                  PARSE_WORD_APPEND) != 0))
                {
                        status = 2;
                        goto fail;
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
            (!exec_special_builtin(shell_argv[first]) ||
             word_is(shell_argv[first], "exec")))
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

        // exec with nothing to run is there for its redirections, and those
        // belong to the shell from here on. Decided before anything runs: a
        // function body or a sourced file run by this command leaves its own
        // last command in argv, and "exec 3>/dev/null" in a function made
        // the redirections on the call permanent as well.
        bare_exec = node->redirect_count && shell_argc == 1 &&
                    word_is(shell_argv[0], "exec");

        log_failure_reset();

        status = exec_dispatch(node->word + first);
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
static string_address address_to exec_fields;
static positive exec_fields_room;

// An expansion that aborted the line leaves nothing to run: what was taken
// goes back, and the answer is the status the abort carries.
static b32 exec_aborted(shell_mark mark)
{
        shell_store_rewind(address_of exec_store, mark);
        shell_status = 2;

        return 2;
}

static b32 exec_for(b32 index)
{
        parse_node address_to node = parse_nodes + index;
        string_address name = parse_words[node->word];
        shell_mark mark = shell_store_mark(address_of exec_store);
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

        if (exec_line_aborted())
                return exec_aborted(mark);

        for (at = 0; at < count; at++)
        {
                if (!env_assign(name, exec_items[at]))
                {
                        string_format(exec_error,
                                      env_readonly(name)
                                          ? "%s: is read only\n"
                                          : "%s: cannot assign\n",
                                      name);
                        status = 2;
                        expand_fatal();
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
                matched = regex_search(text, string_length(text), 0);

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
                                value = shell_match(right, left);
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

        for (item = node->left; item; item = parse_nodes[item].next)
        {
                b32 at;

                for (at = 0; at < parse_nodes[item].word_count; at++)
                {
                        string_address pattern;

                        token_used = 0;
                        pattern = shell_expand_pattern(
                            parse_words[parse_nodes[item].word + at]);

                        if (exec_line_aborted())
                                return exec_aborted(mark);

                        if (!shell_match(pattern, subject))
                                continue;

                        // A matched item with nothing in it ran nothing and
                        // answered with whatever came before the case; an
                        // empty list of commands succeeds.
                        status = parse_nodes[item].right
                                     ? exec_node(parse_nodes[item].right)
                                     : 0;
                        shell_store_rewind(address_of exec_store, mark);

                        return status;
                }
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

        log_flush();
        child = shell_clone();

        if (child == 0)
        {
                b32 status;

                exec_asynchronous = background;
                trap_default_all();
                exec_child_signals(background, background);
                exec_child_began();

                /* The async environment is already a subshell. Turning an
                   explicit (...) node into its equivalent group avoids a
                   second process whose PID would not be $!. */
                if (background && parse_nodes[index].kind == NODE_SUBSHELL)
                        parse_nodes[index].kind = NODE_GROUP;

                shell_tail_command = background &&
                                     parse_nodes[index].kind == NODE_SIMPLE;

                status = exec_node(index);
                log_flush();
                exit(status);
        }

        return child;
}

/*
        A pipeline.

        Every stage gets a process of its own, because a builtin on either end
        of a pipe has to have its own fd 1 and its own fd 0. The parent closes
        both ends of every pipe it made before it waits: a write end still open
        here is an end of file the reader never sees, and the whole shell
        stops.
*/
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

        if (count > positive_max / sizeof(children[0]) ||
            !shell_array_room(children, children_room, count))
        {
                string_format(exec_error, "No room for pipeline\n");
                return 2;
        }

        while (child && started < count)
        {
                b32 ends[2];
                bipolar made;
                bool last = started + 1 >= count || !parse_nodes[child].next;

                ends[0] = -1;
                ends[1] = -1;

                if (!last && system_pipe(ends, 0) < 0)
                {
                        spawn_failed = true;
                        break;
                }

                log_flush();
                made = shell_clone();

                if (made == 0)
                {
                        trap_default_all();
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
                        log_flush();
                        exit(status);
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

                children[started++] = made;
                child = parse_nodes[child].next;
        }

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

        for (at = 0; at < started; at++)
        {
                b32 got = exec_child_status(children[at]);

                if (got)
                        rightmost_failure = got;

                if (at + 1 == started)
                        status = got;
        }

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
        parse_node address_to node;
        shell_mark expanded;
        b32 mark;
        b32 status;

        if (!index)
                return shell_status;

        node = parse_nodes + index;

        if (node->kind == NODE_SIMPLE)
        {
                expanded = shell_store_mark(address_of expand_store);

                status = exec_simple(index);
                shell_store_rewind(address_of expand_store, expanded);
                shell_status = status;
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
        else if (node->kind == NODE_CFOR)
                status = exec_cfor(index);
        else if (node->kind == NODE_CASE)
                status = exec_case(index);
        else if (node->kind == NODE_SUBSHELL)
                status = exec_child_status(
                    exec_spawn_node(parse_nodes[index].left, false));
        else
                status = exec_node(node->left);

        exec_redirect_restore(mark);
        shell_store_rewind(address_of expand_store, expanded);
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
        if (!exec_depth)
                shell_background_reap();

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
