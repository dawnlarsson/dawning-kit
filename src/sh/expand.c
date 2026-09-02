/*
        Expansion.

        POSIX fixes the order and the order is the substance of it: tilde, then
        parameter, then command substitution and arithmetic, then field
        splitting, then pathname expansion, then quote removal. A star that came
        out of an unquoted variable is a glob and a star that came out of "$x" is
        a star, and nothing but that order tells the two apart -- which is why
        every byte here carries a mark from the moment it is produced until the
        fields are handed back, and why quote removal is not a pass at all but a
        thing that has already happened by the time anybody looks.

        Nothing allocates. One arena holds the words a line expands to and is
        emptied when the next line begins.
*/

#include "../compiler_memory.c"

/*
        What the files beside this one own.

        Named rather than reached for, so this file can be included on either
        side of builtin.c without the two of them needing each other first.
*/
PURE string_address env_get(const_string name);
string_address env_get_hashed_span(const_string name, positive length,
                                   positive hash,
                                   positive address_to value_length);
positive env_names_prefix(string_address prefix, positive length,
                          string_address address_to names, positive room);
PURE bool env_readonly(const_string name);
bool env_assign(const_string name, const_string value);
fn run_line(string_address line);
fn parse_reset_all();
fn shell_trap_exit();
fn exec_child_began();
COLD fn exec_expand_fatal();
string_address shell_flags_current();
bool shell_tool_only_here(string_address name, positive2 named);
bool exec_function_here_hashed(string_address name, positive2 named);
string_address alias_lookup(string_address name);
bipolar shell_spawn_tool(string_address address_to arguments,
                         b32 output, bool quiet);

extern b32 shell_status;
extern b32 shell_is_interactive;

// The set flags, one bit per letter. Only -f is anybody's business here:
// it says a pattern is a word and not a question about the filesystem.
extern positive shell_options;

#define SHELL_NO_GLOB ((positive)1 << ('f' - 'a'))

/*
        What the last command substitution answered.

        Not $? -- a substitution inside a command word must not change what the
        rest of that same command sees, or "echo $(false)$?" reports on itself.
        An assignment whose whole right hand side is a substitution is the one
        place the number is wanted, and that is the shell's to notice.
*/
b32 shell_substitution_status;

#define EXPAND_DEPTH 64
#define EXPAND_LOCAL_NAME 128
#define EXPAND_LOCAL_TEXT 1024

// Linux accepts a pathname of 4095 bytes in one syscall. Walking beyond this
// floor would require directory-relative opens; never quietly shorten it.
#define GLOB_PATH 4096
#define GLOB_DEPTH 64

/*
        What a byte is allowed to become later.

        PLAIN   literal and unquoted: a glob character, never a separator
        FIELD   came out of an unquoted expansion: globs and splits
        QUOTED  was quoted or escaped: neither, whatever it looks like
        BREAK   a field boundary $@ put there on purpose
*/
#define MARK_PLAIN 0
#define MARK_FIELD 1
#define MARK_QUOTED 2
#define MARK_BREAK 3

#define EXPAND_PARAMETER_INDIRECT 1
#define EXPAND_PARAMETER_MISSING 2

//      One word being built, and what each of its bytes is allowed to become.
//      "$@" against a directory's worth of parameters is a single word, so
//      this grows with it. Everything here is reached by index, never by an
//      address kept across a push, so the two blocks may move.
static p8 address_to expand_text;
static positive expand_text_room;
static p8 address_to expand_mark;
static positive expand_mark_room;
static positive expand_length;
static bool expand_overflow;
static bool expand_quoted_seen;
static bool expand_failed;
static bool expand_name_at_empty;
static bool expand_explicit_empty;
static positive expand_depth;

static inline INLINE fn expand_fail_state()
{
        expand_overflow = true;
        expand_failed = true;
}

/*
        Where a finished word lives.

        argv points in here and argv is handed to execve, so these bytes may
        not move once given out: the store chains another block on instead of
        reallocating. It used to be a fixed arena that started over from the
        beginning when it filled, which did not truncate a long line so much as
        quietly write its later words on top of its earlier ones.
*/
static shell_store expand_store;

// The line is over and every word it made is dead with it.
fn shell_expand_reset()
{
        shell_store_reset(address_of expand_store);
}

//      Room for want bytes in both halves at once, since a byte and its mark
//      are always written together.
#define expand_room(want)                                                    \
        shell_byte_pair_room(expand_text, expand_text_room, expand_mark,     \
                             expand_mark_room, (want))

// One short of the end, because trimming writes a terminator at the length.
static fn expand_push(p8 value, p8 mark)
{
        if (!expand_room(expand_length + 2))
        {
                expand_overflow = true;
                return;
        }

        expand_mark[expand_length] = mark;
        expand_text[expand_length++] = value;
}

// A run that all comes out the same way, which is a copy and a fill.
static fn expand_push_run(string_address text, positive length, p8 mark)
{
        if (!expand_room(expand_length + length + 2))
        {
                expand_overflow = true;
                return;
        }

        memory_copy_apart(expand_text + expand_length, text, length);
        memory_fill(expand_mark + expand_length, mark, length);
        expand_length += length;
}

static fn expand_push_string(string_address text, p8 mark)
{
        if (text)
                expand_push_run(text, string_length(text), mark);
}

/*
        What a byte can be without anything having to look at it.

        plain    outside quotes: not an escape, a quote, a dollar or a backtick
        inside   within a double quote, where the single quote is a byte again
*/
static b8 expand_plain_set[STRING_SET_BYTES];
static b8 expand_inside_set[STRING_SET_BYTES];
static b8 expand_literal_set[STRING_SET_BYTES];
static b32 expand_sets_ready;

static fn expand_sets_prepare()
{
        if (expand_sets_ready)
                return;

        memory_fill(expand_plain_set + 1, 1, STRING_SET_BYTES - 1);
        memory_fill(expand_inside_set + 1, 1, STRING_SET_BYTES - 1);
        memory_fill(expand_literal_set + 1, 1, STRING_SET_BYTES - 1);

        {
                static const string_address inside = "\\\"$`";

                for (positive i = 0; inside[i]; i++)
                        expand_plain_set[inside[i]] = expand_inside_set[inside[i]] = 0;
        }

        expand_plain_set['\''] = 0;

        {
                static const string_address changes = "'\"\\$`*?[{~";

                for (positive i = 0; changes[i]; i++)
                        expand_literal_set[changes[i]] = 0;
        }

        expand_sets_ready = true;
}

/*
        Start an expansion with nothing carried over from the last one.

        Three entry points begin here -- a word, an arithmetic body, and the
        dollar in a here-document -- and each of them cleared the same six
        pieces of state in the same order. A seventh piece added to two of the
        three is the bug that shape invites: the third then sees whatever the
        second left behind, which is a wrong answer that depends on what ran
        before it.
*/
static fn expand_begin()
{
        expand_sets_prepare();

        expand_length = 0;
        expand_overflow = false;
        expand_quoted_seen = false;
        expand_failed = false;
        expand_name_at_empty = false;
        expand_explicit_empty = false;
        expand_depth = 0;
}

/*
        Whether expansion is provably the identity operation.

        The lexer has already made blanks and operators token boundaries. If
        none of the bytes that can quote, substitute, glob, brace-expand or
        begin tilde expansion is present, the parser's stable word is already
        the final field. string_span_max is the hardware-floor set scan; this
        function adds only the shell-specific policy.
*/
bool shell_expand_literal(string_address word, positive length)
{
        expand_sets_prepare();

        /* A bracket starts a glob only when another bracket can close it.
           The overwhelmingly common lone `[` is the test builtin name and
           expansion is provably the identity operation for that one byte. */
        return string_span_max(word, length, expand_literal_set) == length ||
               (length == 1 && string_is(word, '['));
}

static COLD fn expand_complain(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        system_write_once(standard_error_descriptor, data, length);
}

#define expand_name_character(value) (byte_is_alnum(value) || (value) == '_')
#define expand_assignable_name(name)                                        \
        (byte_is_alpha(string_get(name)) || string_is((name), '_'))

// A scalar parameter name after Bash's indirect ${!name}. Arrays have their
// own grammar and representation and are deliberately not smuggled in here.
static bool expand_parameter_name(string_address name, positive length)
{
        p8 first = string_get(name);
        bool numeric;

        if (!length)
                return false;

        if (length == 1 && (first == '@' || first == '*' || first == '#' ||
                            first == '?' || first == '$' || first == '!' ||
                            first == '-'))
                return true;

        numeric = first >= '0' && first <= '9';

        if (!numeric && !expand_assignable_name(name))
                return false;

        for (positive at = 1; at < length; at++)
        {
                p8 value = string_get(name + at);

                if (!expand_name_character(value) ||
                    (numeric && (value < '0' || value > '9')))
                        return false;
        }

        return true;
}

/*
        Keep the common case on the stack and spill only a long value into the
        stable line store. This is one policy for names, operator words and
        arithmetic bodies instead of three bounded copies with three subtly
        different truncation points.
*/
static string_address expand_hold(string_address text, positive length,
                                  p8 address_to scratch, positive scratch_room)
{
        p8 address_to held;

        if (length < scratch_room)
                held = scratch;
        else
                held = shell_store_take(address_of expand_store, length + 1);

        if (!held)
        {
                expand_fail_state();
                return null;
        }

        memory_copy_end(held, text, length);
        return held;
}

/*
        A number in the three bases C gives it and the shell inherits: 0x is
        sixteen, a leading zero is eight, anything else is ten. Ten was the
        only one read, so $((0x10)) came out as nothing and a mode written
        0644 as six hundred and forty four.

        The cursor is handed back one past the last digit, because arithmetic
        reads a literal out of the middle of an expression and a variable is
        read whole -- and both have to agree about what 010 is worth.
*/
static positive expand_base_positive(string_address address_to at,
                                     bool address_to valid, positive limit)
{
        string_address step = address_to at;
        positive value = 0;
        positive base = 10;
        positive used = 0;

        address_to valid = true;

        if (string_is(step, '0') &&
            (string_is(step + 1, 'x') || string_is(step + 1, 'X')))
        {
                step += 2;
                base = 16;
        }
        else if (string_is(step, '0') && string_get(step + 1) >= '0' &&
                 string_get(step + 1) <= '9')
        {
                step++;
                base = 8;
        }

        /* The limit and base do not change with the digit. Computing their
           quotient once keeps hardware division out of the character loop;
           the remainder is the largest final digit accepted at the cutoff. */
        positive cutoff = limit / base;
        positive last = limit % base;

        while (1)
        {
                positive digit = digit_known(string_get(step), 16);

                if (digit >= base)
                        break;

                /*
                        Arithmetic is a signed machine word. dash saturates a
                        literal that does not fit instead of letting the input
                        helper wrap its unsigned accumulator and hand a
                        plausible negative value to the expression. Consume
                        every digit after saturation so the parser still lands
                        at the real operator or end of the expression.
                */
                if (value > cutoff || (value == cutoff && digit > last))
                        value = limit;
                else
                        value = value * base + digit;

                used++;
                step++;
        }

        if (!used)
                address_to valid = false;

        address_to at = step;

        return value;
}

static bipolar expand_base_number(string_address address_to at, bool address_to valid)
{
        return (bipolar)expand_base_positive(at, valid, (positive)bipolar_max);
}

/*
        A bracket set: where it ends, and whether a byte is in it.

        A [ with no ] anywhere after it is a plain [ and not a set at all, which
        is why the end is found first and the membership asked second.
*/
static PURE string_address expand_set_end(string_address at)
{
        string_address step = at + 1;

        if (string_is(step, '!') || string_is(step, '^'))
                step++;

        // The first ] is a member, not the close.
        if (string_is(step, ']'))
                step++;

        while (string_get(step) && string_not(step, ']'))
        {
                string_address past = byte_class_end(step, null);

                step = past ? past : step + 1;
        }

        return string_get(step) ? step : null;
}

static PURE bool expand_in_set(string_address at, string_address stop, p8 value)
{
        string_address step = at + 1;
        bool invert = false;
        bool found = false;

        if (string_is(step, '!') || string_is(step, '^'))
        {
                invert = true;
                step++;
        }

        while (step < stop)
        {
                p8 low;

                {
                        string_address past = byte_class_end(step, null);

                        if (past && past <= stop)
                        {
                                if (byte_class_holds(
                                            byte_class_index(step + 2,
                                                             (positive)(past - step - 4)),
                                            value))
                                        found = true;

                                step = past;
                                continue;
                        }
                }

                low = string_get(step);

                if (low == '\\' && step + 1 < stop)
                        low = string_get(++step);

                if (step + 2 < stop && string_is(step + 1, '-'))
                {
                        p8 high = string_get(step + 2);

                        if (value >= low && value <= high)
                                found = true;

                        step += 3;
                        continue;
                }

                if (value == low)
                        found = true;

                step++;
        }

        return invert ? !found : found;
}

/*
        A glob against a string, whole.

        This is the matcher for case, for the four trimming forms, and for every
        component of a path -- one pattern language, matched one way, so that a
        case arm and a glob cannot disagree about what a star is.
*/
PURE bool shell_match(string_address pattern, string_address text)
{
        string_address star = null;
        string_address back = null;
        p8 behind = 0;

        while (string_get(text))
        {
                p8 want = string_get(pattern);
                string_address stop = null;

                if (want == '*')
                {
                        star = ++pattern;
                        back = text;

                        /*
                                A plain byte behind the star is the only place
                                what follows can begin, so a backtrack goes
                                there rather than trying every position on the
                                way.
                        */
                        behind = string_get(pattern);

                        if (behind == '*' || behind == '?' || behind == '[' ||
                            behind == '\\')
                                behind = 0;

                        continue;
                }

                if (want == '[')
                        stop = expand_set_end(pattern);

                if (stop)
                {
                        if (expand_in_set(pattern, stop, string_get(text)))
                        {
                                pattern = stop + 1;
                                text++;
                                continue;
                        }
                }
                else
                {
                        bool escaped = false;

                        // A [ with no ] after it is a plain [.
                        if (want == '\\' && string_get(pattern + 1))
                        {
                                want = string_get(++pattern);
                                escaped = true;
                        }

                        if (want && ((!escaped && want == '?') ||
                                     want == string_get(text)))
                        {
                                pattern++;
                                text++;
                                continue;
                        }
                }

                /*
                        Nothing matched here, so the last star takes one byte
                        more. Walking rather than calling: a pattern of many
                        stars against a long name tries every arrangement of
                        them when each star recurses, and only the arrangement
                        it is standing in when the star is a mark.
                */
                if (!star)
                        return false;

                pattern = star;
                text = ++back;

                if (behind)
                {
                        text = string_first_of_or_end(text, behind);

                        if (!string_get(text))
                                return false;

                        back = text;
                }
        }

        while (string_is(pattern, '*'))
                pattern++;

        return string_get(pattern) == end;
}

//      set -- takes as many words as it is given, which after a glob may be
//      every name in a directory, so neither the table nor the bytes behind it
//      is allowed a fixed size.
string_address address_to shell_parameter;
static positive shell_parameter_room;
positive shell_parameter_count;
string_address shell_script_name = (string_address) "sh";
// Entry-only flags seed the options `set` can subsequently change. A no-arg
// shell reads standard input and begins with s; a script file resets it and
// -c has its own entry marker.
string_address shell_option_flags = (string_address) "s";

static p8 address_to shell_parameter_bytes;
static positive shell_parameter_bytes_room;
static p8 address_to shell_parameter_staging;
static positive shell_parameter_staging_room;

/*
        Where set -- and shift keep their words.

        The copy goes through a staging block because "set -- $@" hands back the
        very bytes it is about to be written over.
*/
bool shell_parameters_set(string_address address_to words, positive count)
{
        static string_address empty[1];
        positive used = 0;
        positive index = 0;
        positive at;
        positive need = 0;

        /* Entry with no operands is already the complete empty state. Keep
           the three growable stores untouched until set/function arguments
           actually need them. Once allocated, an empty `set --` retains its
           reusable table rather than rebinding it to static storage. */
        if (!count && !shell_parameter_room)
        {
                shell_parameter = empty;
                shell_parameter_count = 0;
                return true;
        }

        if (count > positive_max - 2 ||
            !shell_array_room(shell_parameter, shell_parameter_room, count + 2))
                return false;

        /* Measure before changing the live table, so allocation failure leaves
           the old positional parameters intact.  The bytes still pass through
           staging because set -- $@ may overlap the live parameter store. */
        for (at = 0; at < count; at++)
        {
                positive length = string_length(words[at]);

                if (length == positive_max ||
                    need > positive_max - length - 1)
                        return false;

                need += length + 1;
        }

        if (need == positive_max ||
            !shell_byte_pair_room(shell_parameter_staging,
                                  shell_parameter_staging_room,
                                  shell_parameter_bytes,
                                  shell_parameter_bytes_room, need + 1))
                return false;

        while (index < count)
        {
                positive length = string_length(words[index]) + 1;

                memory_copy(shell_parameter_staging + used, words[index],
                            length);
                shell_parameter[index] = shell_parameter_bytes + used;
                used += length;
                index++;
        }

        memory_copy(shell_parameter_bytes, shell_parameter_staging, used);

        shell_parameter_count = index;

        shell_parameter[index] = null;

        return index == count;
}

/*
        The parameters as they stand, put aside.

        A function is given its own and hands these back when it returns, and a
        function may call a function, so what is put aside is a stack of bytes
        rather than one spare copy. The pointers are not worth saving: they all
        point into the one block that the next set is about to write over.
*/
#define EXPAND_NO_ROOM ((positive)-1)

//      A function is given its own parameters and hands these back when it
//      returns, and functions nest, so what is put aside is a stack of bytes
//      that grows with the depth rather than a fixed one.
static p8 address_to shell_parameter_stack;
static positive shell_parameter_stack_room;
static positive shell_parameter_stack_used;

positive shell_parameters_save()
{
        positive mark = shell_parameter_stack_used;
        positive used = 0;
        positive at;

        for (at = 0; at < shell_parameter_count; at++)
        {
                positive length = string_length(shell_parameter[at]) + 1;

                if (used > positive_max - length)
                        return EXPAND_NO_ROOM;

                used += length;
        }

        if (used == positive_max || mark > positive_max - used - 1 ||
            !shell_room((address_any address_to)address_of shell_parameter_stack,
                        address_of shell_parameter_stack_room, mark + used + 1, 1))
                return EXPAND_NO_ROOM;

        for (at = 0; at < shell_parameter_count; at++)
        {
                positive length = string_length(shell_parameter[at]) + 1;

                memory_copy(shell_parameter_stack + shell_parameter_stack_used,
                            shell_parameter[at], length);
                shell_parameter_stack_used += length;
        }

        return mark;
}

static string_address address_to shell_restore_words;
static positive shell_restore_room;

bool shell_parameters_restore_prepare(positive count)
{
        return count != positive_max &&
               shell_array_room(shell_restore_words, shell_restore_room, count + 1);
}

bool shell_parameters_restore(positive mark, positive count)
{
        positive at = mark;
        positive index;

        if (mark == EXPAND_NO_ROOM)
                return false;

        if (!shell_parameters_restore_prepare(count))
                return false;

        for (index = 0; index < count; index++)
        {
                shell_restore_words[index] = shell_parameter_stack + at;
                at += string_length(shell_parameter_stack + at) + 1;
        }

        if (!shell_parameters_set(shell_restore_words, count))
                return false;

        shell_parameter_stack_used = mark;
        return true;
}

fn shell_parameters_shift(positive count)
{
        if (count > shell_parameter_count)
                count = shell_parameter_count;

        memory_copy(shell_parameter, shell_parameter + count,
                    (shell_parameter_count - count) *
                        sizeof(shell_parameter[0]));

        shell_parameter_count -= count;
        shell_parameter[shell_parameter_count] = null;
}

/*
        IFS unset is the default three; IFS set to nothing splits nothing at
        all, and collapsing the two is how a script that clears IFS on purpose
        gets its fields taken apart anyway.
*/
static PURE string_address expand_ifs()
{
        /* djb2("IFS"). The name and its extent are invariant, so sending it
           through the NUL-scanning hash path on every split only rediscovers
           the same three bytes. */
        string_address value = env_get_hashed_span(
            (const_string) "IFS", 3, 193458887, null);

        return value ? value : (string_address) " \t\n";
}

/*
        IFS as one byte per byte value, built once a word.

        Splitting asks about every byte it walks over and the answer is the same
        every time; asking env_get for it each time turned a field into a linear
        walk of the environment per character.
*/
// Read at startup, from where nothing has forked yet.
positive expand_shell_pid;
bipolar shell_background_last;

fn shell_pid_ensure()
{
        if (!expand_shell_pid)
                expand_shell_pid =
                    (positive)system_call_1(syscall(getpid), 0);
}

/* A child may perform its first $$ expansion after the clone. Capture the
   shell's identity on the parent side while leaving no-fork startup lazy. */
bipolar shell_clone()
{
        shell_pid_ensure();
        return system_fork();
}

static b8 expand_ifs_set[256];
static b8 expand_ifs_blank_set[256];

static fn expand_ifs_prepare()
{
        string_address ifs = expand_ifs();

        memory_fill(expand_ifs_set, 0, sizeof(expand_ifs_set));
        memory_fill(expand_ifs_blank_set, 0, sizeof(expand_ifs_blank_set));

        while (string_get(ifs))
        {
                p8 value = string_get(ifs++);

                expand_ifs_set[value] = 1;

                if (value == ' ' || value == '\t' || value == '\n')
                        expand_ifs_blank_set[value] = 1;
        }
}

static PURE bool expand_in_ifs(p8 value)
{
        return expand_ifs_set[value] != 0;
}

static PURE bool expand_ifs_blank(p8 value)
{
        return expand_ifs_blank_set[value] != 0;
}

/*
        What a parameter stands for, and whether it stands for anything at
        all. Ordinary variables and positional parameters already live at
        stable addresses and are returned directly. Only the numeric special
        parameters need scratch space.

        $* and unquoted $@ need one joined string. It comes from the line's
        stable store, sized from the parameters rather than from a fixed
        expansion buffer.
*/
static string_address expand_value_of(string_address name, p8 address_to scratch,
                                      bool address_to present,
                                      positive address_to value_length)
{
        p8 first = string_get(name);

        address_to present = true;
        if (value_length)
                address_to value_length = 0;
        scratch[0] = end;

        if (first >= '0' && first <= '9')
        {
                positive which = string_digits(name, null);

                if (!which)
                {
                        if (value_length)
                                address_to value_length =
                                    string_length(shell_script_name);
                        return shell_script_name;
                }

                if (which > shell_parameter_count)
                {
                        address_to present = false;
                        return null;
                }

                if (value_length)
                        address_to value_length =
                            string_length(shell_parameter[which - 1]);

                return shell_parameter[which - 1];
        }

        if (string_get(name + 1) == end)
        {
                if (first == '#')
                {
                        positive length = bipolar_into_string(
                            scratch, (bipolar)shell_parameter_count);
                        if (value_length)
                                address_to value_length = length;
                        return scratch;
                }

                if (first == '?')
                {
                        positive length = bipolar_into_string(
                            scratch, (bipolar)shell_status);
                        if (value_length)
                                address_to value_length = length;
                        return scratch;
                }

                if (first == '$')
                {
                        /*
                                The shell's pid, and not this process's.

                                A subshell is a fork, and asking the kernel
                                here answered with the fork -- so "( kill $$ )"
                                signalled the subshell instead of the shell,
                                which is the opposite of what POSIX says $$
                                is. Read once, before anything can fork.
                        */
                        shell_pid_ensure();

                        positive length = bipolar_into_string(
                            scratch, (bipolar)expand_shell_pid);
                        if (value_length)
                                address_to value_length = length;
                        return scratch;
                }

                if (first == '!')
                {
                        if (shell_background_last <= 0)
                        {
                                address_to present = false;
                                return null;
                        }

                        positive length = bipolar_into_string(
                            scratch, shell_background_last);
                        if (value_length)
                                address_to value_length = length;
                        return scratch;
                }

                if (first == '-')
                {
                        string_address flags = shell_flags_current();

                        if (value_length)
                                address_to value_length = string_length(flags);
                        return flags;
                }

                if (first == '@' || first == '*')
                {
                        string_address ifs = expand_ifs();
                        p8 between = string_get(ifs);
                        positive room = 1;
                        positive used = 0;
                        positive at;
                        p8 address_to into;

                        for (at = 0; at < shell_parameter_count; at++)
                        {
                                positive run = string_length(shell_parameter[at]);

                                if (room > (positive)-1 - run - (at && between ? 1 : 0))
                                {
                                        address_to present = false;
                                        return null;
                                }

                                room += run + (at && between ? 1 : 0);
                        }

                        into = shell_store_take(address_of expand_store, room);

                        if (!into)
                        {
                                address_to present = false;
                                return null;
                        }

                        for (at = 0; at < shell_parameter_count; at++)
                        {
                                positive run = string_length(shell_parameter[at]);

                                if (at && between)
                                        into[used++] = between;

                                memory_copy_apart(into + used, shell_parameter[at], run);
                                used += run;
                        }

                        into[used] = end;
                        if (value_length)
                                address_to value_length = used;
                        return into;
                }
        }

        {
                positive2 answer = string_hash_33_length(name);
                string_address value = env_get_hashed_span(
                    name, answer.y, answer.x, value_length);

                if (!value)
                {
                        address_to present = false;
                        return null;
                }

                return value;
        }
}

static COLD fn expand_fatal();
static COLD fn expand_fatal_status(b32 status);

/*
        A parameter pushed with the marks that decide its fate later.

        "$@" is the one form that makes its own field boundaries: they go in
        as a byte of their own so that splitting cannot miss them, and a
        parameter with a space in it stays whole.

        Unquoted, $@ and $* do not. Both join on the first byte of IFS and the
        join is taken apart again, which is why set -- "" a is one field and
        not two -- an empty parameter joins to nothing and splits to nothing.
        Only when IFS is empty is there no byte to join on, and there the
        boundaries have to be put in or every parameter runs together.
*/
static bool expand_push_parameter_as(string_address name, bool quoted,
                                     b32 mode)
{
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        p8 scratch[32];
        string_address value;
        positive value_length;
        bool present;
        bool all = string_get(name + 1) == end &&
                   (string_is(name, '@') || string_is(name, '*'));

        if (!(mode & EXPAND_PARAMETER_MISSING) && all &&
            (quoted ? string_is(name, '@') : !string_get(expand_ifs())))
        {
                positive at;

                for (at = 0; at < shell_parameter_count; at++)
                {
                        if (at)
                                expand_push(' ', MARK_BREAK);

                        expand_push_string(shell_parameter[at], mark);
                }

                return shell_parameter_count != 0;
        }

        if (mode & EXPAND_PARAMETER_MISSING)
        {
                present = false;
                value = null;
                value_length = 0;
        }
        else
                value = expand_value_of(name, scratch, address_of present,
                                        address_of value_length);

        if (!present)
        {
                if (shell_options & ((positive)1 << ('u' - 'a')))
                {
                        string_format(expand_complain, "%s: parameter not set\n", name);
                        if (mode & EXPAND_PARAMETER_INDIRECT)
                                expand_fatal_status(1);
                        else
                                expand_fatal();
                }

                return false;
        }

        expand_push_run(value, value_length, mark);

        return true;
}

static bool expand_push_parameter(string_address name, bool quoted)
{
        return expand_push_parameter_as(name, quoted, 0);
}

/*
        The exact one-byte trim at the hardware floor.

        ${name#?} and ${name%?} are common ways to consume one byte from a
        shell value. The general pattern path first copies the whole value and
        its mark bytes, captures `?`, invokes the matcher, then moves the
        surviving value back over the byte it removed. For an ordinary named
        variable and this exact pattern, the result is already a span of the
        stable environment value. A bounded hash and the value length metadata
        select that span without a scan or a temporary copy.

        This deliberately stays byte-oriented, matching the shell matcher it
        replaces. Special and positional parameters retain their existing
        path because their value may have to be formatted or joined first.
*/
static fn expand_push_named_trim_one(string_address name, positive name_length,
                                     bool prefix, bool quoted)
{
        positive value_length;
        string_address value = env_get_hashed_span(
            name, name_length, memory_hash_33(name, name_length),
            address_of value_length);

        if (!value)
        {
                if (shell_options & ((positive)1 << ('u' - 'a')))
                {
                        string_format(expand_complain,
                                      "%s: parameter not set\n", name);
                        expand_fatal();
                }

                return;
        }

        if (value_length)
        {
                if (prefix)
                        value++;

                value_length--;
        }

        expand_push_run(value, value_length,
                        quoted ? MARK_QUOTED : MARK_FIELD);
}

static fn expand_into(string_address text, bool quoted, p8 plain,
                      bool assignment);
static string_address expand_double(string_address step);
static string_address expand_dollar(string_address step, bool quoted);
static string_address expand_backtick(string_address step, bool quoted);
static bool expand_sort_names(string_address address_to names, positive count);

/*
        A nested word -- the tail of ${x-...}, a trimming pattern, the body of
        an arithmetic expression -- run through the whole expander and lifted
        back out, leaving the working buffer exactly as it was found.

        A pattern gets a backslash in front of every byte that was quoted, so
        that ${x%"*"} strips one star and not everything.
*/
#define EXPAND_CAPTURE_TEXT 0
#define EXPAND_CAPTURE_PATTERN 1
#define EXPAND_CAPTURE_REPLACEMENT 2

static string_address expand_capture(string_address text, bool quoted, b32 mode)
{
        positive at = expand_length;
        bool held = expand_quoted_seen;
        bool held_name_at = expand_name_at_empty;
        bool held_explicit = expand_explicit_empty;
        positive room = 1;
        positive step;
        positive used = 0;
        p8 address_to into;

        expand_into(text, quoted, MARK_PLAIN, false);

        for (step = at; step < expand_length; step++)
        {
                p8 value = expand_text[step];

                if (room == positive_max)
                        break;

                room++;

                if (expand_mark[step] == MARK_QUOTED &&
                    ((mode == EXPAND_CAPTURE_PATTERN &&
                      (value == '*' || value == '?' || value == '[' || value == '\\')) ||
                     (mode == EXPAND_CAPTURE_REPLACEMENT && value == '&')))
                        room++;
        }

        if (step != expand_length || !(into = shell_store_take(address_of expand_store,
                                                                room)))
        {
                expand_fail_state();
                expand_length = at;
                expand_quoted_seen = held;
                expand_name_at_empty = held_name_at;
                expand_explicit_empty = held_explicit;
                return null;
        }

        for (step = at; step < expand_length; step++)
        {
                p8 value = expand_text[step];

                if (expand_mark[step] == MARK_QUOTED &&
                    ((mode == EXPAND_CAPTURE_PATTERN &&
                      (value == '*' || value == '?' || value == '[' || value == '\\')) ||
                     (mode == EXPAND_CAPTURE_REPLACEMENT && value == '&')))
                        into[used++] = '\\';

                into[used++] = value;
        }

        into[used] = end;
        expand_length = at;
        expand_quoted_seen = held;
        expand_name_at_empty = held_name_at;
        expand_explicit_empty = held_explicit;

        return into;
}

/*
        Arithmetic.

        A cursor in a file scope rather than one passed down, because the whole
        expression is already expanded by the time it gets here: nothing inside
        can start another arithmetic, so there is nothing to be reentrant for.
*/
static string_address arith_at;

/*
        What the expression could not answer.

        Nothing was ever wrong here: a divide by zero was zero, a missing
        operand was whatever had been read so far, and a name holding a word
        was a name holding nothing. All three handed a number to the script
        that the script never computed. dash stops on each of them and leaves
        2 behind, and so does this.
*/
static bool arith_bad;

// Parsing always reaches the far end of a logical or conditional expression,
// but an untaken arm is grammar only: it must not read or write variables or
// raise evaluation errors such as division by zero.
static bool arith_active;
static bool arith_bash_mode;
static bool arith_nounset;
static bool arith_unset;

static PURE inline INLINE string_address arith_skip_space(string_address at)
{
        while (string_is(at, ' ') || string_is(at, '\t') || string_is(at, '\n'))
                at++;

        return at;
}

static fn arith_space()
{
        arith_at = arith_skip_space(arith_at);
}

static bipolar arith_choose();

// Writing a name back, which every assigning form ends with.
static bipolar arith_store(string_address name, bipolar value)
{
        p8 written[32];

        if (!arith_active)
                return 0;

        bipolar_into_string(written, value);
        if (!env_assign(name, written))
        {
                string_format(expand_complain,
                              env_readonly(name) ? "%s: is read only\n"
                                                 : "%s: cannot assign\n",
                              name);

                if (!arith_bash_mode)
                        expand_fatal();

                return arith_bash_mode ? 0 : value;
        }

        return value;
}

/*
        What a name is worth here.

        Unset and empty are both zero, and anything that is not a number is
        not a value at all -- x=bar came out as zero and the arithmetic
        carried on around it.
*/
static bipolar arith_value_of(string_address name)
{
        p8 scratch[32];
        bool present;
        bool valid;
        string_address step = expand_value_of(name, scratch,
                                               address_of present, null);
        string_address digits;
        positive magnitude;
        bool negative = false;

        if (!arith_active)
                return 0;

        if (!present)
        {
                if (arith_nounset)
                {
                        arith_bad = true;
                        arith_unset = true;
                }

                return 0;
        }

        step += string_span(step, string_set_blanks);

        if (!string_get(step))
                return 0;

        if (string_is(step, '-') || string_is(step, '+'))
        {
                negative = string_is(step, '-');
                step++;
        }

        digits = step;
        magnitude = expand_base_positive(address_of step, address_of valid,
                                         (positive)bipolar_max + 1);

        if (step == digits || !valid)
        {
                arith_bad = true;
                return 0;
        }

        step += string_span(step, string_set_blanks);

        if (string_get(step))
                arith_bad = true;

        if (!negative)
                return magnitude > (positive)bipolar_max
                           ? bipolar_max
                           : (bipolar)magnitude;

        if (magnitude == (positive)bipolar_max + 1)
                return (bipolar)((positive)bipolar_max + 1);

        return -(bipolar)magnitude;
}

/*
        The one division the machine will not do.

        A zero divisor faults, and so does the smallest number there is over
        minus one, because its opposite is not a number this width holds --
        $(( -9223372036854775808 / -1 )) killed the shell with SIGFPE.
*/
static bipolar arith_divide(bipolar left, bipolar right, bool remainder)
{
        if (!arith_active)
                return 0;

        if (!right || (right == -1 && left == bipolar_min))
        {
                arith_bad = true;
                return 0;
        }

        return remainder ? left % right : left / right;
}

// Shell arithmetic is the target machine word. Express wrapping through the
// unsigned type so compiler overflow assumptions cannot change that contract.
static CONST bipolar arith_addition(bipolar left, bipolar right)
{
        return (bipolar)((positive)left + (positive)right);
}

static CONST bipolar arith_subtraction(bipolar left, bipolar right)
{
        return (bipolar)((positive)left - (positive)right);
}

static CONST bipolar arith_product(bipolar left, bipolar right)
{
        return (bipolar)((positive)left * (positive)right);
}

static CONST bipolar arith_negate(bipolar value)
{
        return (bipolar)(0 - (positive)value);
}

static CONST bipolar arith_shift_left(bipolar left, bipolar right)
{
        positive count = (positive)right & (positive_bits - 1);

        return (bipolar)((positive)left << count);
}

static CONST bipolar arith_shift_right(bipolar left, bipolar right)
{
        positive count = (positive)right & (positive_bits - 1);

        return left >> count;
}

// What the operator in front of the = does.
static bipolar arith_combine(p8 op, bipolar left, bipolar right)
{
        if (!arith_active)
                return 0;

        switch (op)
        {
        case '+': return arith_addition(left, right);
        case '-': return arith_subtraction(left, right);
        case '*': return arith_product(left, right);
        case '/': return arith_divide(left, right, false);
        case '%': return arith_divide(left, right, true);
        case '&': return left & right;
        case '|': return left | right;
        case '^': return left ^ right;
        case 'l': return arith_shift_left(left, right);
        case 'r': return arith_shift_right(left, right);
        }

        return right;
}


static bipolar arith_primary()
{
        bipolar value = 0;

        arith_space();

        if (string_is(arith_at, '('))
        {
                arith_at++;
                value = arith_choose();
                arith_space();

                if (string_is(arith_at, ')'))
                        arith_at++;
                else
                        arith_bad = true;

                return value;
        }

        if (string_is(arith_at, '!'))
        {
                arith_at++;
                return !arith_primary();
        }

        if (string_is(arith_at, '~'))
        {
                arith_at++;
                return ~arith_primary();
        }

        if (arith_bash_mode &&
            ((string_is(arith_at, '+') && string_is(arith_at + 1, '+')) ||
             (string_is(arith_at, '-') && string_is(arith_at + 1, '-'))))
        {
                bool increment = string_is(arith_at, '+');
                string_address start;
                p8 name_local[EXPAND_LOCAL_NAME];
                string_address name;
                positive length = 0;
                bipolar value;

                arith_at += 2;
                arith_space();
                start = arith_at;

                while (expand_name_character(string_get(arith_at)))
                {
                        length++;
                        arith_at++;
                }

                name = expand_hold(start, length, name_local,
                                   sizeof(name_local));

                if (!length || !name)
                {
                        arith_bad = true;
                        return 0;
                }

                value = arith_value_of(name);
                value = increment ? arith_addition(value, 1)
                                  : arith_subtraction(value, 1);

                return arith_store(name, value);
        }

        if (string_is(arith_at, '-'))
        {
                arith_at++;
                return arith_negate(arith_primary());
        }

        if (string_is(arith_at, '+'))
        {
                arith_at++;
                return arith_primary();
        }

        if (string_get(arith_at) >= '0' && string_get(arith_at) <= '9')
        {
                bool valid;

                value = expand_base_number(address_of arith_at, address_of valid);

                if (!valid)
                        arith_bad = true;

                return value;
        }

        /*
                A name with no dollar in front of it, which is the one place in
                the language where that reads a variable.
        */
        if (expand_name_character(string_get(arith_at)))
        {
                string_address start = arith_at;
                p8 name_local[EXPAND_LOCAL_NAME];
                string_address name;
                positive length = 0;

                while (expand_name_character(string_get(arith_at)))
                {
                        length++;
                        arith_at++;
                }

                name = expand_hold(start, length, name_local,
                                   sizeof(name_local));

                if (!name)
                {
                        arith_bad = true;
                        return 0;
                }
                arith_space();

                if (string_is(arith_at, '=') && string_get(arith_at + 1) != '=')
                {
                        arith_at++;

                        return arith_store(name, arith_choose());
                }

                /*
                        The compound forms, which read the name as well as
                        write it: += and its nine relatives.

                        Longest first, or <<= is < followed by <= and x >>= 1
                        halves nothing. They are all "read, combine, write"
                        and share the tail below. Postfix ++ and -- are not in
                        the POSIX grammar; leaving them for the final cursor
                        check rejects them instead of silently adding a
                        non-portable side effect.
                */
                {
                        p8 op = 0;
                        b32 skip = 0;

                        if (string_is(arith_at, '<') && string_is(arith_at + 1, '<') &&
                            string_is(arith_at + 2, '='))
                        {
                                op = 'l';
                                skip = 3;
                        }
                        else if (string_is(arith_at, '>') && string_is(arith_at + 1, '>') &&
                                 string_is(arith_at + 2, '='))
                        {
                                op = 'r';
                                skip = 3;
                        }
                        else if (string_is(arith_at + 1, '=') &&
                                 string_first_of((string_address) "+-*/%&|^",
                                                 string_get(arith_at)))
                        {
                                op = string_get(arith_at);
                                skip = 2;
                        }

                        if (op)
                        {
                                bipolar was;

                                arith_at += skip;

                                was = arith_value_of(name);

                                return arith_store(name,
                                                   arith_combine(op, was, arith_choose()));
                        }

                        if (arith_bash_mode &&
                            ((string_is(arith_at, '+') && string_is(arith_at + 1, '+')) ||
                             (string_is(arith_at, '-') && string_is(arith_at + 1, '-'))))
                        {
                                bool increment = string_is(arith_at, '+');
                                bipolar was = arith_value_of(name);

                                arith_at += 2;
                                arith_store(name,
                                            increment ? arith_addition(was, 1)
                                                      : arith_subtraction(was, 1));
                                return was;
                        }
                }

                return arith_value_of(name);
        }

        // A byte that starts no value at all, which is where a missing
        // operand lands: $((1 + )) answered 1 and $((2 ** 3)) answered 0.
        arith_bad = true;

        return 0;
}

static bipolar arith_multiply()
{
        bipolar value = arith_primary();

        while (1)
        {
                arith_space();

                if (string_is(arith_at, '*'))
                {
                        arith_at++;
                        value = arith_product(value, arith_primary());
                        continue;
                }

                if (string_is(arith_at, '/') || string_is(arith_at, '%'))
                {
                        bool remainder = string_is(arith_at, '%');

                        arith_at++;
                        value = arith_divide(value, arith_primary(), remainder);
                        continue;
                }

                return value;
        }
}

static bipolar arith_add()
{
        bipolar value = arith_multiply();

        while (1)
        {
                arith_space();

                if (string_is(arith_at, '+'))
                {
                        arith_at++;
                        value = arith_addition(value, arith_multiply());
                        continue;
                }

                if (string_is(arith_at, '-'))
                {
                        arith_at++;
                        value = arith_subtraction(value, arith_multiply());
                        continue;
                }

                return value;
        }
}

/*
        The shifts, between the sums and the comparisons.

        There was no level here at all, so 1 << 4 was read as 1 < (< 4) and
        answered with something that was not sixteen. The comparisons below
        were already written to step around << and >>, which is what made the
        gap invisible: they declined to match and nothing else did.
*/
static bipolar arith_shift()
{
        bipolar value = arith_add();

        while (1)
        {
                arith_space();

                if (string_is(arith_at, '<') && string_is(arith_at + 1, '<') &&
                    !string_is(arith_at + 2, '='))
                {
                        arith_at += 2;
                        value = arith_shift_left(value, arith_add());
                        continue;
                }

                if (string_is(arith_at, '>') && string_is(arith_at + 1, '>') &&
                    !string_is(arith_at + 2, '='))
                {
                        arith_at += 2;
                        value = arith_shift_right(value, arith_add());
                        continue;
                }

                return value;
        }
}

static bipolar arith_compare()
{
        bipolar value = arith_shift();

        while (1)
        {
                arith_space();

                if (string_is(arith_at, '<') && string_get(arith_at + 1) == '=')
                {
                        arith_at += 2;
                        value = value <= arith_shift();
                        continue;
                }

                if (string_is(arith_at, '>') && string_get(arith_at + 1) == '=')
                {
                        arith_at += 2;
                        value = value >= arith_shift();
                        continue;
                }

                if (string_is(arith_at, '<') && string_get(arith_at + 1) != '<')
                {
                        arith_at++;
                        value = value < arith_shift();
                        continue;
                }

                if (string_is(arith_at, '>') && string_get(arith_at + 1) != '>')
                {
                        arith_at++;
                        value = value > arith_shift();
                        continue;
                }

                return value;
        }
}

static bipolar arith_equal()
{
        bipolar value = arith_compare();

        while (1)
        {
                arith_space();

                if (string_is(arith_at, '=') && string_get(arith_at + 1) == '=')
                {
                        arith_at += 2;
                        value = value == arith_compare();
                        continue;
                }

                if (string_is(arith_at, '!') && string_get(arith_at + 1) == '=')
                {
                        arith_at += 2;
                        value = value != arith_compare();
                        continue;
                }

                return value;
        }
}

/*
        The bitwise three, in the order POSIX puts them: & binds tighter than
        ^, which binds tighter than |, and all three are looser than == and
        tighter than &&. None of them existed, so a & b was read as a and then
        stopped, quietly, at the ampersand.

        Each declines the doubled form, because && and || are the levels above
        and reading one here would take half of it.
*/
#define ARITH_BIT_LEVEL(name, lower, character, operation, doubled)           \
        static bipolar name()                                                \
        {                                                                    \
                bipolar value = lower();                                     \
                                                                             \
                while (1)                                                    \
                {                                                            \
                        arith_space();                                       \
                                                                             \
                        if (!string_is(arith_at, character) ||               \
                            ((doubled) &&                                    \
                             string_is(arith_at + 1, character)) ||          \
                            string_is(arith_at + 1, '='))                    \
                                return value;                                \
                                                                             \
                        arith_at++;                                          \
                        value = value operation lower();                     \
                }                                                            \
        }

ARITH_BIT_LEVEL(arith_bit_and, arith_equal, '&', &, true)
ARITH_BIT_LEVEL(arith_bit_xor, arith_bit_and, '^', ^, false)
ARITH_BIT_LEVEL(arith_bit_or, arith_bit_xor, '|', |, true)

#define ARITH_LOGICAL_LEVEL(name, lower, byte, wanted, operation)            \
        static bipolar name()                                               \
        {                                                                    \
                bipolar value = lower();                                    \
                                                                             \
                while (true)                                                 \
                {                                                            \
                        arith_space();                                        \
                        if (!string_is(arith_at, (byte)) ||                  \
                            string_get(arith_at + 1) != (byte))              \
                                return value;                                \
                                                                             \
                        arith_at += 2;                                        \
                        bool active = arith_active;                          \
                        arith_active = active && (wanted);                   \
                        bipolar right = lower();                             \
                        arith_active = active;                               \
                        value = active ? (value operation right) : 0;        \
                }                                                            \
        }

ARITH_LOGICAL_LEVEL(arith_and, arith_bit_or, '&', value, &&)
ARITH_LOGICAL_LEVEL(arith_or, arith_and, '|', !value, ||)
#undef ARITH_LOGICAL_LEVEL

/*
        The ternary, looser than the two logical levels and tighter than an
        assignment. There was no level here at all, so $((1 ? 2 : 3)) stopped
        after the condition and answered with it.

        Both arms are read whichever one is wanted, for the same reason && and
        || read both sides: the cursor has to come out of the far end of the
        expression, and skipping an arm leaves it in the middle of one.
*/
static bipolar arith_choose()
{
        bipolar value = arith_or();
        bipolar taken;
        bipolar left;
        bool active;

        arith_space();

        if (!string_is(arith_at, '?'))
                return value;

        arith_at++;
        active = arith_active;
        arith_active = active && value;
        taken = arith_choose();
        arith_space();

        if (string_is(arith_at, ':'))
                arith_at++;
        else
                arith_bad = true;

        arith_active = active && !value;
        left = arith_choose();
        arith_active = active;

        return active ? (value ? taken : left) : 0;
}

/*
        The control-loop kernel is a name, one add/subtract, and a literal:

                i=$((i + 1))

        Sending that through every precedence level costs more than the
        lookup and addition themselves. Recognize only the complete, exact
        shape before reading the name, so a failed probe cannot duplicate an
        assignment, increment, nounset error, or other arithmetic side
        effect. Everything richer stays with the grammar below.
*/
static bool arith_simple_addition(string_address text,
                                  bipolar address_to answer)
{
        string_address at = text;
        string_address name_start;
        string_address number_at;
        p8 name_local[EXPAND_LOCAL_NAME];
        string_address name;
        positive name_length;
        bipolar right;
        bool valid;
        p8 op;

        at = arith_skip_space(at);

        if (!((string_get(at) >= 'a' && string_get(at) <= 'z') ||
              (string_get(at) >= 'A' && string_get(at) <= 'Z') ||
              string_is(at, '_')))
                return false;

        name_start = at;
        while (expand_name_character(string_get(at)))
                at++;
        name_length = at - name_start;

        at = arith_skip_space(at);

        op = string_get(at);
        if (op != '+' && op != '-')
                return false;
        at++;

        at = arith_skip_space(at);

        if (string_get(at) < '0' || string_get(at) > '9')
                return false;

        number_at = at;
        right = expand_base_number(address_of at, address_of valid);

        at = arith_skip_space(at);

        if (!valid || at == number_at || string_get(at))
                return false;

        name = expand_hold(name_start, name_length, name_local,
                           sizeof(name_local));
        if (!name)
        {
                arith_bad = true;
                address_to answer = 0;
                return true;
        }

        address_to answer = op == '+'
                                  ? arith_addition(arith_value_of(name), right)
                                  : arith_subtraction(arith_value_of(name), right);
        return true;
}

static bipolar arith_evaluate(string_address text)
{
        bipolar value;

        arith_bad = false;
        arith_active = true;
        arith_at = text;
        arith_space();

        // An arithmetic expansion contains an expression, not an optional
        // expression. Empty input used to turn into a plausible zero.
        if (!string_get(arith_at))
        {
                arith_bad = true;
                return 0;
        }

        if (arith_simple_addition(text, address_of value))
                return value;

        value = arith_choose();
        arith_space();

        // Bash arithmetic commands and C-style for clauses accept the comma
        // operator. Keep it out of POSIX $((...)), where dash rejects it, but
        // evaluate every Bash operand left-to-right and return the final one.
        while (arith_bash_mode && string_is(arith_at, ','))
        {
                arith_at++;
                value = arith_choose();
                arith_space();
        }

        // Every byte has to belong to the grammar. This catches comma and
        // postfix increment/decrement instead of returning the left prefix.
        if (string_get(arith_at))
                arith_bad = true;

        return value;
}

/*
        The interior of ((...)) has the same parameter, command and quote
        expansion as arithmetic expansion, but it is a command token rather
        than a word. Capture it whole: no field splitting or pathname lookup.
*/
static string_address shell_expand_arithmetic_text(string_address text)
{
        expand_begin();

        return expand_capture(text, true, EXPAND_CAPTURE_TEXT);
}

/*
        Where the quoted run that opens here ends, one byte past its close.

        A single quoted run ends at the next quote and a backslash inside it is
        an ordinary byte; a double quoted run lets a backslash carry the byte
        behind it, so \" does not close one. A run that is never closed ends at
        the terminating null, and every caller's loop ends there too.

        Three scanners below walk a word looking for one byte that is theirs --
        the bracket that closes an expansion, the / that separates a
        replacement, the } that closes a brace list -- and none of them may
        find it inside quotes. Written out three times, the three had to be
        kept level by hand.
*/
static PURE string_address expand_quoted_run(string_address at, p8 quote)
{
        at++;

        while (string_get(at) && string_not(at, quote))
                at += quote == '"' && string_is(at, '\\') && string_get(at + 1)
                          ? 2
                          : 1;

        return string_get(at) ? at + 1 : at;
}

// A dollar-single-quoted run has already been kept whole by the lexer.  Its
// escaped quote is not the closing quote, so syntax scanners use the same end
// rule as the lexer rather than the ordinary single-quote rule above.
static string_address expand_dollar_quoted_run(string_address at)
{
        string_address stop = lex_dollar_quote_end(at + 2);

        return string_get(stop) ? stop + 1 : stop;
}

/*
        The bracket that closes a $( ... ) or a ${ ... }, with quotes and
        nesting counted; nothing when the word runs out first, in which case
        the $ was only a $.
*/
static PURE string_address expand_bracket_end(string_address at, p8 open, p8 close)
{
        positive depth = 1;

        while (string_get(at))
        {
                p8 value = string_get(at);

                if (value == '$' && string_is(at + 1, '\''))
                {
                        at = expand_dollar_quoted_run(at);
                        continue;
                }

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        at = expand_quoted_run(at, value);
                        continue;
                }

                if (value == open)
                        depth++;

                if (value == close && !--depth)
                        return at;

                at++;
        }

        return null;
}

#define expand_paren_end(at) expand_bracket_end((at), '(', ')')
#define expand_brace_end(at) expand_bracket_end((at), '{', '}')

/*
        Whether this process is a substitution's child.

        Its exit ends a word, not the shell, so what the shell does on the way
        out is not its to do: running the exit trap in here wrote the trap's
        output down the substitution pipe and folded it into the word.
*/
static bool expand_in_substitution;

/*
        The no-shell command-substitution path.

        A literal multicall utility needs neither the outer shell's address
        space nor another parser. Strip only quoting whose contents are
        provably literal, then let Spark attach the existing pipe to a fresh
        /shell utility. Anything with expansion, globbing, operators, an
        alias, or a function declines this path and keeps the full shell.
*/
static string_address address_to expand_tool_argv;
static positive expand_tool_argv_room;

/* Remove quoting from one word which has no expansion. Long literal runs use
   the same hardware-floor byte-set scans as ordinary expansion. */
static p8 address_to expand_tool_word(string_address word, positive length,
                                      p8 address_to out)
{
        string_address at = word;
        string_address stop = word + length;

        expand_sets_prepare();

        while (at < stop)
        {
                positive run = string_span_max(
                    at, (positive)(stop - at), expand_literal_set);

                if (run)
                {
                        memory_copy_apart(out, at, run);
                        out += run; at += run;
                }

                if (at == stop)
                        break;

                p8 value = *at++;

                if (value == '\'')
                {
                        string_address close = (string_address)memory_first_of(
                            at, '\'', (positive)(stop - at));

                        if (!close)
                                return null;

                        run = (positive)(close - at);
                        memory_copy_apart(out, at, run);
                        out += run; at = close + 1;
                }
                else if (value == '"')
                {
                        while (true)
                        {
                                run = string_span_max(
                                    at, (positive)(stop - at), expand_inside_set);
                                if (run)
                                {
                                        memory_copy_apart(out, at, run);
                                        out += run; at += run;
                                }

                                if (at == stop ||
                                    (value = *at++) == '$' || value == '`')
                                        return null;
                                if (value == '"')
                                        break;
                                if (at == stop)
                                        return null;

                                value = *at++;
                                if (value == '$' || value == '`' ||
                                    value == '"' || value == '\\')
                                        *out++ = value;
                                else if (value != '\n')
                                {
                                        *out++ = '\\';
                                        *out++ = value;
                                }
                        }
                }
                else if (value == '\\' && at < stop)
                {
                        value = *at++;
                        if (value != '\n')
                                *out++ = value;
                }
                else
                        return null; /* expansion, brace expansion or glob */
        }

        return out;
}

static bipolar expand_tool_direct(string_address command, b32 output)
{
        static p8 address_to text;
        static positive text_room;
        b32 count;
        bipolar child = -1;
        positive2 named;
        bool quiet = false;
        p8 address_to out;

        /* Execution only retains the parser's stable copy of its tokens. The
           lexer scratch is therefore free to reuse without a nested mapping. */
        count = lex_line(command);

        // The end token stops short at a top-level newline or comment.
        if (count <= 0 || string_get(command + lex_tokens[count].at))
                goto done;

        if (count >= 3)
        {
                lex_token address_to descriptor = lex_tokens + count - 3;
                lex_token address_to redirect = descriptor + 1;
                lex_token address_to target = redirect + 1;

                if (descriptor->kind == LEX_WORD && descriptor->length == 1 &&
                    string_is(descriptor->text, '2') &&
                    redirect->kind == LEX_OPERATOR && redirect->op == OP_GREAT &&
                    target->kind == LEX_WORD && target->length == 9 &&
                    !memory_compare(target->text, "/dev/null", 9) &&
                    descriptor->at + descriptor->length == redirect->at &&
                    redirect->at + redirect->length == target->at)
                {
                        quiet = true;
                        count -= 3;
                }
        }

        if (!count ||
            !shell_room((address_any address_to)address_of text,
                        address_of text_room, string_length(command) + 1, 1) ||
            !shell_array_room(expand_tool_argv, expand_tool_argv_room, (positive)count + 1))
                goto done;

        out = text;

        for (b32 at = 0; at < count; at++)
        {
                if (lex_tokens[at].kind != LEX_WORD)
                        goto done;

                expand_tool_argv[at] = out;
                out = expand_tool_word(lex_tokens[at].text,
                                       lex_tokens[at].length, out);

                if (!out)
                        goto done;

                *out++ = 0;
        }

        expand_tool_argv[count] = null;
        named = string_hash_33_length(expand_tool_argv[0]);

        if (!shell_tool_only_here(expand_tool_argv[0], named) ||
            exec_function_here_hashed(expand_tool_argv[0], named) ||
            alias_lookup(expand_tool_argv[0]))
                goto done;

        child = shell_spawn_tool(expand_tool_argv, output, quiet);

done:
        return child;
}

/*
        A command substitution.

        The text runs in a child whose standard output is a pipe, and what it
        wrote becomes the bytes. Reading to the end before waiting is what keeps
        a command that writes more than a pipe holds from wedging both sides.
*/
static fn expand_run(string_address command, bool quoted)
{
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        positive start = expand_length;
        b32 channel[2];
        bipolar child = -1;
        positive status = 0;

        // Whatever this shell has buffered belongs to this shell, and a fork
        // with it still in hand prints all of it a second time.
        log_flush();

        if (system_pipe(address_of channel, 0) < 0)
                return;

        child = expand_tool_direct(command, channel[1]);

        if (child < 0)
                child = shell_clone();

        if (child == 0)
        {
                exec_child_began();

                system_close(channel[0]);
                system_duplicate(channel[1],
                              standard_output_descriptor, 0);
                system_close(channel[1]);

                expand_in_substitution = true;

                // The parser still holds the line this substitution is a word
                // of. In here that is somebody else's half-read sentence, and
                // feeding it another one makes a syntax error out of both.
                parse_reset_all();

                /*
                        A substitution is not one line. run_line is, and it was
                        handed the whole body -- so everything after the first
                        newline was dropped, and a here-document or a loop
                        written inside $( ) produced nothing at all.

                        The body is this child's own copy, so the newlines are
                        turned into terminators in place rather than into a
                        second buffer.
                */
                if (!string_first_of(command, '\n'))
                {
                        shell_tail_line_requested = true;
                        run_line(command);
                }
                else
                {
                        string_address at = command;

                        while (string_get(at))
                        {
                                string_address stop = string_first_of_or_end(at, '\n');

                                if (string_get(stop))
                                {
                                        address_to stop = end;
                                        stop++;
                                }

                                if (string_get(at))
                                        run_line(at);

                                at = stop;
                        }
                }

                log_flush();

                system_call_1(syscall(exit_group), (positive)shell_status);
        }

        system_close(channel[1]);

        if (child > 0)
        {
                while (1)
                {
                        p8 block[512];
                        bipolar got = system_read_retry((positive)channel[0],
                                                        block, sizeof(block));

                        if (got <= 0)
                                break;

                        expand_push_run(block, (positive)got, mark);
                }
        }

        system_close(channel[0]);

        if (child > 0)
                system_wait4_retry(child, address_of status, 0, null);

        // The newlines at the end go, and only the ones at the end: that is the
        // single piece of editing a substitution is allowed.
        while (expand_length > start && expand_text[expand_length - 1] == '\n')
                expand_length--;

        if (child > 0)
                shell_substitution_status = wait_status_code(status);
}

static string_address expand_command(string_address step, bool quoted)
{
        string_address inner = step + 2;
        string_address stop = expand_paren_end(inner);
        p8 address_to text;
        positive length;

        // No closing paren inside this word: the lexer stopped short of it, and
        // a dollar with nothing it can begin is a dollar.
        if (!stop)
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        length = (positive)(stop - inner);

        //      Out of the line's own store, so a substitution inside a
        //      substitution gets its own room rather than sharing one buffer.
        text = shell_store_take(address_of expand_store, length + 1);

        if (!text)
        {
                expand_overflow = true;
                return stop + 1;
        }

        memory_copy_end(text, inner, length);

        expand_run(text, quoted);

        return stop + 1;
}

static string_address expand_backtick(string_address step, bool quoted)
{
        p8 address_to text;
        positive length = 0;
        positive room;
        string_address look = step + 1;

        while (string_get(look) && string_not(look, '`'))
        {
                if (string_is(look, '\\') && string_get(look + 1))
                        look++;

                look++;
        }

        if (!string_get(look))
        {
                expand_push('`', MARK_PLAIN);
                return step + 1;
        }

        //      The scan above already found the closing backtick, so the
        //      most this can need is the distance to it.
        room = (positive)(look - step) + 1;
        text = shell_store_take(address_of expand_store, room);

        if (!text)
        {
                expand_overflow = true;
                return look + 1;
        }

        step++;

        while (string_get(step) && string_not(step, '`'))
        {
                // Inside backticks a backslash only hides the next byte when
                // that byte is one that backticks care about.
                if (string_is(step, '\\') &&
                    (string_get(step + 1) == '`' || string_get(step + 1) == '\\' ||
                     string_get(step + 1) == '$'))
                        step++;

                if (length + 1 < room)
                        text[length++] = string_get(step);

                step++;
        }

        text[length] = end;

        if (string_get(step))
                step++;

        expand_run(text, quoted);

        return step;
}

/*
        The two things that stop a script where it stands.

        ${x:?} exists to stop, and arithmetic that cannot be answered has
        nothing to hand back -- saying so and carrying on is the one thing
        either must not do. dash leaves 2 behind and runs the exit trap on the
        way out. A terminal is the exception, because there is somebody there
        to type the line again.
*/
static COLD fn expand_fatal_status(b32 status)
{
        shell_status = status;

        if (shell_is_interactive)
        {
                expand_failed = true;
                exec_expand_fatal();
                return;
        }

        if (!expand_in_substitution)
                shell_trap_exit();

        log_flush();
        system_call_1(syscall(exit_group), status);
}

static COLD fn expand_fatal()
{
        expand_fatal_status(2);
}

/*
        The word did not fit, so nothing that depends on it can go on.

        Eight places at the bottom of this file notice a full store, and every
        one of them said this sentence and then called that fatal, in that
        order. Both halves matter: the sentence names the word, and the fatal
        is what stops a script rather than letting it act on a truncated one.
*/
static COLD fn expand_too_long(string_address word)
{
        string_format(expand_complain, "Expansion too long: %s\n", word);
        expand_fatal();
}

static string_address expand_arithmetic(string_address step, bool quoted)
{
        string_address inner = step + 3;
        string_address stop = expand_paren_end(inner);
        p8 text_local[EXPAND_LOCAL_TEXT];
        string_address text;
        string_address ready;
        p8 written[32];
        positive length;

        if (!stop || string_get(stop + 1) != ')')
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        length = (positive)(stop - inner);

        text = expand_hold(inner, length, text_local, sizeof(text_local));

        if (!text)
                return stop + 2;

        // What was written with a dollar in front takes its turn first; what is
        // left over is arithmetic, where a bare name is a value too.
        ready = expand_capture(text, true, false);

        // A nested expansion already diagnosed the whole word.  In an
        // interactive shell that diagnosis returns here instead of exiting
        // the process, so nothing after it may parse the partial capture or
        // perform one of its side effects.
        if (expand_failed)
                return stop + 2;

        {
                bipolar value = arith_evaluate(ready);
                positive written_length;

                if (arith_bad)
                {
                        string_format(expand_complain,
                                      "arithmetic: %s\n", ready);
                        expand_fatal();

                        return stop + 2;
                }

                written_length = bipolar_into_string(written, value);
                expand_push_run(written, written_length,
                                quoted ? MARK_QUOTED : MARK_FIELD);
        }

        return stop + 2;
}

/*
        ${x#pat} and its three relatives, done in place on the value that was
        just pushed. The cut lengths are tried in the order the form asks for
        and the first that matches wins, which is all that shortest and longest
        mean.
*/
static fn expand_trim(positive start, string_address pattern, bool prefix, bool longest)
{
        positive length = expand_length - start;
        positive cut = 0;
        bool found = false;
        positive at;

        for (at = 0; at <= length; at++)
        {
                positive size = longest ? (length - at) : at;
                p8 held;
                bool hit;

                if (prefix)
                {
                        held = expand_text[start + size];
                        expand_text[start + size] = end;
                        hit = shell_match(pattern, expand_text + start);
                        expand_text[start + size] = held;
                }
                else
                {
                        held = expand_text[expand_length];
                        expand_text[expand_length] = end;
                        hit = shell_match(pattern, expand_text + expand_length - size);
                        expand_text[expand_length] = held;
                }

                if (hit)
                {
                        cut = size;
                        found = true;
                        break;
                }
        }

        if (!found || !cut)
                return;

        if (prefix)
        {
                memory_copy(expand_text + start, expand_text + start + cut, length - cut);
                memory_copy(expand_mark + start, expand_mark + start + cut, length - cut);
        }

        expand_length -= cut;
}

//      The slash separating a Bash replacement pattern from its replacement.
//      A slash hidden by a quote, a backslash or a nested expansion belongs to
//      that word and is not the separator of the outer ${.../.../...}.
static PURE string_address expand_replace_separator(string_address at)
{
        while (string_get(at))
        {
                p8 value = string_get(at);

                if (value == '$' && string_is(at + 1, '\''))
                {
                        at = expand_dollar_quoted_run(at);
                        continue;
                }

                if (value == '/')
                        return at;

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        at = expand_quoted_run(at, value);
                        continue;
                }

                if (value == '$' && string_is(at + 1, '{'))
                {
                        string_address stop = expand_brace_end(at + 2);

                        if (stop)
                        {
                                at = stop + 1;
                                continue;
                        }
                }

                if (value == '$' && string_is(at + 1, '('))
                {
                        string_address stop = expand_paren_end(at + 2);

                        if (stop)
                        {
                                at = stop + 1;
                                continue;
                        }
                }

                at++;
        }

        return null;
}

//      Whether pattern matches exactly size bytes at source + at. The source
//      is our private copy, so terminating one candidate in place avoids a
//      fresh allocation for every possible match.
static bool expand_replace_match(p8 address_to source, positive at,
                                 positive size, string_address pattern)
{
        p8 held = source[at + size];
        bool matched;

        source[at + size] = end;
        matched = shell_match(pattern, source + at);
        source[at + size] = held;

        return matched;
}

// Bash's patsub_replacement option is on by default: an unquoted ampersand in
// the replacement is the bytes that matched, while a quoted one is literal.
// expand_capture records the quoted one as \& so that distinction survives
// the nested expansion buffer.
static fn expand_replace_push(string_address replacement,
                              string_address matched, positive match_length,
                              p8 mark)
{
        string_address at = replacement;
        string_address run = at;

        while (string_get(at))
        {
                if (string_is(at, '\\') && string_is(at + 1, '&'))
                {
                        expand_push_run(run, (positive)(at - run), mark);
                        expand_push('&', mark);
                        at += 2;
                        run = at;
                        continue;
                }

                if (string_is(at, '&'))
                {
                        expand_push_run(run, (positive)(at - run), mark);
                        expand_push_run(matched, match_length, mark);
                        at++;
                        run = at;
                        continue;
                }

                at++;
        }

        expand_push_run(run, (positive)(at - run), mark);
}

/*
        Bash ${name/pattern/replacement}.

        The first match is the leftmost one and the match at that position is
        the longest one accepted by the glob. A doubled slash repeats that
        search over the remainder. # and % immediately after the operator
        anchor the match to the beginning or end respectively.
*/
static fn expand_replace(string_address name, string_address pattern_text,
                         string_address replacement_text, bool quoted,
                         bool global, b32 parameter_mode)
{
        positive expansion_start = expand_length;
        positive length;
        p8 address_to source;
        string_address pattern;
        string_address replacement;
        positive at = 0;
        positive copied = 0;
        p8 anchor = 0;
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;

        // This also applies nounset and the special-parameter rules before
        // the value is lifted out of the shared expansion buffer.
        expand_push_parameter_as(name, quoted, parameter_mode);

        if (expand_failed)
                return;

        length = expand_length - expansion_start;
        source = shell_store_take(address_of expand_store, length + 1);

        if (!source)
        {
                expand_fail_state();
                expand_length = expansion_start;
                return;
        }

        memory_copy_end(source, expand_text + expansion_start, length);
        expand_length = expansion_start;

        pattern = expand_capture(pattern_text, false, EXPAND_CAPTURE_PATTERN);
        replacement = expand_capture(replacement_text, false,
                                     EXPAND_CAPTURE_REPLACEMENT);

        if (expand_failed || !pattern || !replacement)
                return;

        if (string_is(pattern, '#') || string_is(pattern, '%'))
                anchor = string_get(pattern++);

        // An empty pattern does not designate a match in Bash parameter
        // replacement; the value passes through unchanged.
        if (!string_get(pattern))
        {
                expand_push_run(source, length, mark);
                return;
        }

        while (at <= length)
        {
                positive begin = at;
                positive size = 0;
                bool found = false;

                if (anchor == '#')
                        begin = 0;

                for (; begin <= length; begin++)
                {
                        positive largest = length - begin;

                        if (anchor == '%' && begin < at)
                                continue;

                        for (size = largest + 1; size;)
                        {
                                size--;

                                if (anchor == '%' && begin + size != length)
                                        continue;

                                if (expand_replace_match(source, begin, size,
                                                         pattern))
                                {
                                        found = true;
                                        break;
                                }
                        }

                        if (found || anchor == '#')
                                break;
                }

                if (!found)
                        break;

                expand_push_run(source + copied, begin - copied, mark);
                expand_replace_push(replacement, source + begin, size, mark);
                copied = begin + size;

                if (!global || anchor)
                        break;

                // A zero-width match must still advance through the source;
                // otherwise a global replacement never reaches its end.
                if (!size && copied < length)
                {
                        expand_push(source[copied], mark);
                        copied++;
                }

                at = copied;
        }

        expand_push_run(source + copied, length - copied, mark);
}

// The length separator in ${name:offset:length}. A colon paired with a
// top-level arithmetic ?: belongs to the offset; parentheses and nested
// expansions keep all of their colons inside too.
static PURE string_address expand_substring_separator(string_address at)
{
        positive parentheses = 0;
        positive choices = 0;

        while (string_get(at))
        {
                p8 value = string_get(at);

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '$' && string_is(at + 1, '{'))
                {
                        string_address stop = expand_brace_end(at + 2);

                        if (stop)
                        {
                                at = stop + 1;
                                continue;
                        }
                }

                if (value == '(')
                        parentheses++;
                else if (value == ')' && parentheses)
                        parentheses--;
                else if (!parentheses && value == '?')
                        choices++;
                else if (!parentheses && value == ':' && choices)
                        choices--;
                else if (!parentheses && value == ':')
                        return at;

                at++;
        }

        return null;
}

static fn expand_substring(string_address name, string_address expression,
                           bool quoted, b32 parameter_mode)
{
        string_address separator = expand_substring_separator(expression);
        string_address offset_text = expression;
        string_address length_text = null;
        string_address ready;
        bipolar offset = 0;
        bipolar wanted = 0;
        bool has_length = separator != null;
        positive expansion_start = expand_length;
        positive length;
        positive begin;
        positive finish;

        // ${@:...} and ${*:...} slice an array of positional parameters, not
        // the joined byte string. Keep rejecting that distinct operation
        // until parameter arrays have a representation of their own.
        if ((string_is(name, '@') || string_is(name, '*')) && !string_get(name + 1))
        {
                string_format(expand_complain, "%s: bad substitution\n", name);
                if (parameter_mode & EXPAND_PARAMETER_INDIRECT)
                        expand_fatal_status(1);
                else
                        expand_fatal();
                return;
        }

        if (separator)
        {
                separator[0] = end;
                length_text = separator + 1;
        }

        if (string_get(offset_text))
        {
                ready = expand_capture(offset_text, true, EXPAND_CAPTURE_TEXT);

                if (expand_failed)
                        return;

                offset = arith_evaluate(ready);

                if (arith_bad)
                {
                        string_format(expand_complain, "arithmetic: %s\n", ready);
                        expand_fatal();
                        return;
                }
        }
        else if (!has_length)
        {
                string_format(expand_complain, "%s: bad substitution\n", name);
                expand_fatal();
                return;
        }

        if (has_length)
        {
                ready = expand_capture(length_text, true, EXPAND_CAPTURE_TEXT);

                if (expand_failed)
                        return;

                wanted = arith_evaluate(ready);

                if (arith_bad)
                {
                        string_format(expand_complain, "arithmetic: %s\n", ready);
                        expand_fatal();
                        return;
                }
        }

        expand_push_parameter_as(name, quoted, parameter_mode);

        if (expand_failed)
                return;

        length = expand_length - expansion_start;

        if (offset < 0)
                begin = offset < -(bipolar)length
                            ? length
                            : (positive)((bipolar)length + offset);
        else
                begin = (positive)offset < length ? (positive)offset : length;

        finish = length;

        if (has_length)
        {
                if (wanted >= 0)
                        finish = (positive)wanted < length - begin
                                     ? begin + (positive)wanted
                                     : length;
                else if (wanted < -(bipolar)length ||
                         (positive)((bipolar)length + wanted) < begin)
                {
                        string_format(expand_complain,
                                      "%s: substring expression < 0\n", name);
                        expand_length = expansion_start;
                        expand_fatal();
                        return;
                }
                else
                        finish = (positive)((bipolar)length + wanted);
        }

        memory_copy(expand_text + expansion_start,
                    expand_text + expansion_start + begin, finish - begin);
        memory_copy(expand_mark + expansion_start,
                    expand_mark + expansion_start + begin, finish - begin);
        expand_length = expansion_start + finish - begin;
}

static fn expand_case_change(string_address name, string_address pattern_text,
                             bool quoted, bool upper, bool every,
                             b32 parameter_mode)
{
        positive start = expand_length;
        bool default_pattern = !string_get(pattern_text);
        string_address pattern;
        p8 one[2] = {0, 0};
        positive count;

        expand_push_parameter_as(name, quoted, parameter_mode);

        if (expand_failed)
                return;

        pattern = string_get(pattern_text)
                      ? expand_capture(pattern_text, false,
                                       EXPAND_CAPTURE_PATTERN)
                      : (string_address) "?";

        if (expand_failed || !pattern)
                return;

        count = every ? expand_length - start
                      : (expand_length > start ? 1 : 0);

        // With no explicit pattern the doubled forms select every byte. The
        // scalar loop stays cheaper for short shell values; beyond that the
        // bounded architecture loop wins without touching the parallel mark
        // array.
        if (default_pattern && every && count >= 32)
        {
                if (upper)
                        memory_to_upper_ascii(expand_text + start, count);
                else
                        memory_to_lower_ascii(expand_text + start, count);
                return;
        }

        for (positive at = 0; at < count; at++)
        {
                p8 value = expand_text[start + at];

                one[0] = value;

                if (!shell_match(pattern, one))
                        continue;

                expand_text[start + at] =
                    upper ? byte_to_upper(value) : byte_to_lower(value);
        }
}

/* Bash ${!prefix@} is a field list; ${!prefix*} is the same sorted names
   joined by the first IFS byte. The environment table supplies set names,
   and the expander owns the terminated copies needed by the shared sorter. */
static fn expand_push_names(string_address prefix, positive prefix_length,
                            p8 form, bool quoted)
{
        positive count = env_names_prefix(prefix, prefix_length, null, 0);
        string_address address_to names;
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        p8 between;

        if (!count)
        {
                if (form == '@')
                        expand_name_at_empty = true;
                return;
        }

        between = form == '@' ? ' ' : string_get(expand_ifs());

        if (count > positive_max / sizeof(names[0]) ||
            !(names = (string_address address_to)shell_store_take(
                  address_of expand_store, count * sizeof(names[0]))) ||
            env_names_prefix(prefix, prefix_length, names, count) != count)
        {
                expand_fail_state();
                return;
        }

        for (positive at = 0; at < count; at++)
        {
                positive length = (positive)(string_first_of(names[at], '=') -
                                               names[at]);
                p8 address_to kept = shell_store_take(address_of expand_store,
                                                       length + 1);

                if (!kept)
                {
                        expand_fail_state();
                        return;
                }

                memory_copy_end(kept, names[at], length);
                names[at] = kept;
        }

        if (!expand_sort_names(names, count))
        {
                expand_fail_state();
                return;
        }

        for (positive at = 0; at < count; at++)
        {
                if (at)
                        if (between)
                                expand_push(between,
                                            form == '@' ? MARK_BREAK : mark);

                expand_push_string(names[at], mark);
        }
}

/*
        ${ ... } in every form POSIX gives it.

        The name comes first, then the colon that decides whether empty counts
        as unset, then the operator, then the word -- and the word is a word, so
        it is expanded the same way as any other and can hold another of these.
*/
static string_address expand_braced(string_address step, bool quoted)
{
        string_address name_start;
        p8 name_local[EXPAND_LOCAL_NAME];
        p8 word_local[EXPAND_LOCAL_TEXT];
        p8 indirect_scratch[32];
        string_address name;
        string_address word;
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        positive length = 0;
        bool want_length = false;
        b32 parameter_mode = 0;
        bool colon = false;
        bool doubled = false;
        p8 name_list = 0;
        p8 operation = 0;
        string_address close = expand_brace_end(step + 2);
        p8 seen;

        if (!close)
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        step += 2;

        // ${#} is how many parameters there are; ${#x} is how long one is.
        if (string_is(step, '#') && string_not(step + 1, '}'))
        {
                want_length = true;
                step++;
        }

        /*
                Bash ${!name} expands name once and uses that value as the
                parameter to read. ${!} itself remains the ordinary special
                parameter. A trailing * or @ is consumed separately below
                only when it ends an ordinary-name prefix.
        */
        if (string_is(step, '!') && step + 1 < close)
        {
                parameter_mode = EXPAND_PARAMETER_INDIRECT;
                step++;
        }

        seen = string_get(step);
        name_start = step;

        if (seen == '@' || seen == '*' || seen == '#' || seen == '?' ||
            seen == '$' || seen == '!' || seen == '-')
        {
                length = 1;
                step++;
        }
        else if (seen >= '0' && seen <= '9')
        {
                while (string_get(step) >= '0' && string_get(step) <= '9')
                {
                        length++;
                        step++;
                }
        }
        else
        {
                while (expand_name_character(string_get(step)))
                {
                        length++;
                        step++;
                }
        }

        name = expand_hold(name_start, length, name_local,
                           sizeof(name_local));

        if (!name)
        {
                expand_fail_state();
                return close + 1;
        }

        seen = string_get(step);

        if ((parameter_mode & EXPAND_PARAMETER_INDIRECT) &&
            expand_assignable_name(name) && step + 1 == close &&
            (seen == '@' || seen == '*'))
        {
                name_list = seen;
                seen = string_get(++step);
        }

        if (seen == ':')
        {
                colon = true;
                step++;
                seen = string_get(step);
        }

        if (seen == '-' || seen == '=' || seen == '?' || seen == '+')
        {
                operation = seen;
                step++;
        }
        else if (colon)
                operation = ':';
        /*
                A colon says one of those four is coming, and nothing else.
                ${x:1:1} is a substring in ksh and in three shells after it
                and in no part of POSIX, and here it quietly handed back the
                whole value -- which is the one answer that is wrong whichever
                of the two the script was written against. dash refuses it and
                so does this.
        */
        else if (!colon && (seen == '%' || seen == '#'))
        {
                operation = seen;
                step++;

                if (string_is(step, seen))
                {
                        doubled = true;
                        step++;
                }
        }
        else if (!colon && seen == '/')
        {
                operation = seen;
                step++;

                if (string_is(step, '/'))
                {
                        doubled = true;
                        step++;
                }
        }
        else if (!colon && (seen == '^' || seen == ','))
        {
                operation = seen;
                step++;

                if (string_is(step, seen))
                {
                        doubled = true;
                        step++;
                }
        }

        /*
                Nothing after a name is harmless unless it is one of the
                operators parsed above. In particular, / and ^ introduce
                Bash substitutions this shell does not implement. Returning
                the unmodified value made ${x//X/-} and ${x^^} look as though
                they had worked, with plausible but wrong data. Refuse every
                unknown suffix just as dash does. Length form has no operator
                tail of its own, and a colon must introduce one of :- := :?
                or :+.
        */
        if (!length || ((parameter_mode & EXPAND_PARAMETER_INDIRECT) &&
                        length == 1 &&
                        (string_is(name, '!') || string_is(name, '$'))) ||
            (colon && !operation) ||
            (!operation && step != close) ||
            (want_length && (operation || name_list)))
        {
                string_format(expand_complain, "%s: bad substitution\n", name);
                if (parameter_mode & EXPAND_PARAMETER_INDIRECT)
                        expand_fatal_status(1);
                else
                        expand_fatal();

                return close + 1;
        }

        {
                positive room = (positive)(close - step);

                word = expand_hold(step, room, word_local,
                                   sizeof(word_local));

                if (!word)
                        return close + 1;
        }

        if (name_list)
        {
                expand_push_names(name, length, name_list, quoted);
                return close + 1;
        }

        if (parameter_mode & EXPAND_PARAMETER_INDIRECT)
        {
                string_address source = name;
                positive target_length;
                bool present;

                name = expand_value_of(source, indirect_scratch,
                                       address_of present,
                                       address_of target_length);

                // An absent positional parameter is itself an unset indirect
                // value; an absent ordinary variable is an invalid expansion.
                if (!present)
                {
                        p8 first = string_get(source);

                        if (first < '0' || first > '9')
                        {
                                string_format(expand_complain,
                                              "%s: invalid indirect expansion\n",
                                              source);
                                expand_fatal_status(1);
                                return close + 1;
                        }

                        parameter_mode |= EXPAND_PARAMETER_MISSING;
                        name = source;
                        target_length = length;
                }
                else if (!target_length &&
                         ((string_is(source, '@') || string_is(source, '*')) &&
                          !string_get(source + 1) && !shell_parameter_count))
                {
                        parameter_mode |= EXPAND_PARAMETER_MISSING;
                        name = source;
                        target_length = length;
                }
                else if (!expand_parameter_name(name, target_length))
                {
                        string_format(expand_complain,
                                      "%s: invalid variable name\n", name);
                        expand_fatal_status(1);
                        return close + 1;
                }

                if (!(parameter_mode & EXPAND_PARAMETER_MISSING))
                {
                        // The operator word may assign the source variable
                        // before the target is read or written. Keep the
                        // resolved name outside mutable environment storage.
                        name = expand_hold(name, target_length, name_local,
                                           sizeof(name_local));

                        if (!name)
                                return close + 1;
                }

                length = target_length;
        }

        if (want_length)
        {
                p8 written[32];
                p8 scratch[32];
                bool present;
                positive count = 0;
                expand_value_of(name, scratch, address_of present,
                                address_of count);

                if (!present)
                        count = 0;

                expand_push_run(written,
                                bipolar_into_string(written, (bipolar)count),
                                mark);

                return close + 1;
        }

        if (operation == '#' || operation == '%')
        {
                string_address pattern;
                positive start = expand_length;

                if (!(parameter_mode & EXPAND_PARAMETER_INDIRECT) &&
                    !doubled && string_is(word, '?') &&
                    string_is(word + 1, end) &&
                    ((string_get(name) >= 'a' && string_get(name) <= 'z') ||
                     (string_get(name) >= 'A' && string_get(name) <= 'Z') ||
                     string_is(name, '_')))
                {
                        expand_push_named_trim_one(name, length,
                                                   operation == '#', quoted);
                        return close + 1;
                }

                expand_push_parameter_as(name, quoted, parameter_mode);

                /*
                        The pattern is not inside the quotes around the whole
                        of this. A star in it is a star whether or not the
                        substitution stands in double quotes; handing the
                        outer quoting in marked every byte of the pattern
                        quoted, so the star was escaped and the only prefix
                        that ever matched was a literal one.
                */
                pattern = expand_capture(word, false, true);

                if (expand_failed)
                        return close + 1;

                expand_trim(start, pattern, operation == '#', doubled);

                return close + 1;
        }

        if (operation == '/')
        {
                string_address separator = expand_replace_separator(word);
                string_address pattern = word;
                string_address replacement = (string_address) "";

                if (separator)
                {
                        separator[0] = end;
                        replacement = separator + 1;
                }

                expand_replace(name, pattern, replacement, quoted, doubled,
                               parameter_mode);

                return close + 1;
        }

        if (operation == ':')
        {
                expand_substring(name, word, quoted, parameter_mode);
                return close + 1;
        }

        if (operation == '^' || operation == ',')
        {
                expand_case_change(name, word, quoted, operation == '^',
                                   doubled, parameter_mode);
                return close + 1;
        }

        {
                p8 scratch[32];
                bool present;
                string_address value = (parameter_mode & EXPAND_PARAMETER_MISSING)
                                           ? null
                                           : expand_value_of(name, scratch,
                                                             address_of present,
                                                             null);

                if (parameter_mode & EXPAND_PARAMETER_MISSING)
                        present = false;
                bool blank = present && value[0] == end;
                bool missing = !present || (colon && blank);

                if (operation == '-')
                {
                        if (missing)
                                expand_into(word, quoted, MARK_FIELD, false);
                        else
                                expand_push_parameter(name, quoted);

                        return close + 1;
                }

                if (operation == '=')
                {
                        if (missing)
                        {
                                string_address made;

                                if ((parameter_mode & EXPAND_PARAMETER_INDIRECT) &&
                                    !expand_assignable_name(name))
                                {
                                        string_format(
                                            expand_complain,
                                            "%s: invalid indirect expansion\n",
                                            name);
                                        expand_fatal_status(1);
                                        return close + 1;
                                }

                                made = expand_capture(word, quoted, false);

                                if (expand_failed)
                                        return close + 1;

                                if (!env_assign(name, made))
                                {
                                        string_format(
                                            expand_complain,
                                            env_readonly(name)
                                                ? "%s: is read only\n"
                                                : "%s: cannot assign\n",
                                            name);
                                        expand_fatal();
                                        return close + 1;
                                }
                                expand_push_string(made, mark);
                        }
                        else
                                expand_push_parameter_as(name, quoted,
                                                         parameter_mode);

                        return close + 1;
                }

                if (operation == '+')
                {
                        if (!missing)
                                expand_into(word, quoted, MARK_FIELD, false);

                        return close + 1;
                }

                if (operation == '?')
                {
                        if (missing)
                        {
                                string_address said;

                                said = expand_capture(word, quoted, false);

                                if (expand_failed)
                                        return close + 1;

                                string_format(expand_complain, "%s: %s\n", name,
                                              said[0] ? said : (string_address) "parameter not set");

                                if (parameter_mode & EXPAND_PARAMETER_INDIRECT)
                                        expand_fatal_status(1);
                                else
                                        expand_fatal();

                                return close + 1;
                        }

                        expand_push_parameter_as(name, quoted, parameter_mode);

                        return close + 1;
                }

                expand_push_parameter_as(name, quoted, parameter_mode);
        }

        return close + 1;
}

static string_address expand_simple(string_address step, bool quoted)
{
        string_address start;
        p8 name_local[EXPAND_LOCAL_NAME];
        string_address name;
        positive length = 0;
        p8 seen;

        step++;
        start = step;
        seen = string_get(step);

        if (seen == '?' || seen == '#' || seen == '$' || seen == '!' ||
            seen == '-' || seen == '@' || seen == '*' ||
            (seen >= '0' && seen <= '9'))
        {
                p8 special[2];

                special[0] = seen;
                special[1] = end;
                expand_push_parameter(special, quoted);

                return step + 1;
        }

        while (expand_name_character(string_get(step)))
        {
                length++;
                step++;
        }

        // A dollar in front of nothing that could be a name is a dollar.
        if (!length)
        {
                expand_push('$', MARK_PLAIN);
                return step;
        }

        name = expand_hold(start, length, name_local,
                           sizeof(name_local));

        if (!name)
                return step;
        expand_push_parameter(name, quoted);

        return step;
}

/*
        POSIX.1-2024 dollar-single-quotes.

        Every byte produced here is quoted: blanks do not split and pattern
        characters do not glob.  C strings cannot carry a null byte.  POSIX
        explicitly permits discarding a null escape and the remaining bytes
        through the closing quote, which is the policy used here; an adjacent
        piece after the quote is still expanded normally.
*/
static string_address expand_dollar_single(string_address step)
{
        string_address at = step + 2;
        bool discard = false;

        expand_quoted_seen = true;

        while (string_get(at) && string_not(at, '\''))
        {
                p8 value = string_get(at++);

                if (value != '\\')
                {
                        if (!discard)
                                expand_push(value, MARK_QUOTED);
                        continue;
                }

                value = string_get(at);

                if (!value)
                        break;

                if (value >= '0' && value <= '7')
                {
                        positive used;
                        positive number = string_digits_octal_escape_max(
                            at, 3, address_of used);

                        at += used;
                        value = (p8)number;
                }
                else if (value == 'x')
                {
                        positive used;
                        positive number = string_digits_hexadecimal_escape_max(
                            at + 1, 2, address_of used);

                        if (!used)
                        {
                                if (!discard)
                                {
                                        expand_push('\\', MARK_QUOTED);
                                        expand_push(value, MARK_QUOTED);
                                }
                                at++;
                                continue;
                        }

                        at += used + 1;
                        value = (p8)number;
                }
                else if (value == 'c' && string_get(at + 1))
                {
                        value = string_get(at + 1);

                        // The operand backslash is itself escaped in source:
                        // \c\\ denotes FS, not a control backslash followed by
                        // another escape.
                        if (value == '\\' && string_is(at + 2, '\\'))
                                at++;

                        value = value == '?' ? 127 : value & 31;
                        at += 2;
                }
                else
                {
                        at++;
                        p8 escaped = byte_simple_escape(value);

                        if (value == 'e')
                                value = 27;
                        else if (escaped)
                                value = escaped;
                        else if (value != '\\' && value != '\'' && value != '"')
                        {
                                // Unspecified by POSIX: preserve the two
                                // bytes, matching the common shell answer.
                                if (!discard)
                                        expand_push('\\', MARK_QUOTED);
                        }
                }

                if (!value)
                        discard = true;
                else if (!discard)
                        expand_push(value, MARK_QUOTED);
        }

        return string_get(at) ? at + 1 : at;
}

static string_address expand_dollar(string_address step, bool quoted)
{
        p8 next = string_get(step + 1);
        string_address result;

        // Deep enough. Something is expanding itself and the stack is the only
        // thing that would notice.
        if (expand_depth >= EXPAND_DEPTH)
        {
                string_format(expand_complain, "Expansion nested too deeply\n");
                expand_fatal();
                return step + 1;
        }

        expand_depth++;

        if (next == '\'')
                result = expand_dollar_single(step);
        else if (next == '(' && string_get(step + 2) == '(')
                result = expand_arithmetic(step, quoted);
        else if (next == '(')
                result = expand_command(step, quoted);
        else if (next == '{')
                result = expand_braced(step, quoted);
        else
                result = expand_simple(step, quoted);

        expand_depth--;

        return result;
}

/*
        One dollar expansion in a here-document.

        A here body does not remove quotes or split fields, so it cannot use
        shell_expand_word. The dollar grammar itself is exactly the quoted
        grammar above, including command/arithmetic expansion and rejection of
        unsupported braced operators. Its bytes are copied into the command's
        existing token arena, where the literal runs around it already live.
*/
RETURNS_NONNULL string_address shell_expand_here_dollar(string_address step,
                                        string_address address_to text,
                                        positive address_to length,
                                        bool address_to overflow)
{
        string_address result;

        expand_begin();

        // Dollar-single-quotes have no quoting role in a here-document body;
        // like a single quote there, the bytes are literal.  Return only the
        // dollar and let the here-body walker copy the following quote/run.
        if (string_is(step + 1, '\''))
        {
                expand_push('$', MARK_QUOTED);
                result = step + 1;
        }
        else
                result = expand_dollar(step, true);

        address_to text = expand_text;
        address_to length = expand_failed ? 0 : expand_length;
        address_to overflow = expand_overflow;

        return result;
}

static string_address expand_double(string_address step)
{
        expand_quoted_seen = true;

        if (string_is(step + 1, '"'))
                expand_explicit_empty = true;

        step++;

        while (!expand_failed && string_get(step) && string_not(step, '"'))
        {
                positive run = string_span(step, expand_inside_set);
                p8 seen;

                if (run)
                {
                        expand_push_run(step, run, MARK_QUOTED);
                        step += run;
                        continue;
                }

                seen = string_get(step);

                if (seen == '\\')
                {
                        p8 next = string_get(step + 1);

                        // Only these four are hidden by a backslash in double
                        // quotes. In front of anything else it is a backslash.
                        if (next == '"' || next == '\\' || next == '$' || next == '`')
                        {
                                expand_push(next, MARK_QUOTED);
                                step += 2;
                                continue;
                        }

                        expand_push(seen, MARK_QUOTED);
                        step++;
                        continue;
                }

                if (seen == '$')
                {
                        // An enclosing double quote makes $'...' literal.
                        // This guard lives here, rather than in expand_dollar,
                        // because a ${...word...} can carry quoted output while
                        // dollar-single-quotes in word still retain syntax.
                        if (string_is(step + 1, '\''))
                        {
                                expand_push('$', MARK_QUOTED);
                                step++;
                                continue;
                        }

                        step = expand_dollar(step, true);
                        continue;
                }

                if (seen == '`')
                {
                        step = expand_backtick(step, true);
                        continue;
                }

                expand_push(seen, MARK_QUOTED);
                step++;
        }

        if (string_get(step))
                step++;

        return step;
}

/*
        A word, expanded in place.

        plain is what a byte nothing quoted comes out as. In the word the
        lexer handed over it is PLAIN, because the lexer has already put every
        separator between words; in the tail of ${x-a b} it is FIELD, because
        those bytes are the result of an expansion and an unquoted expansion
        splits -- ${nosuch-D E} used to be one field holding a space.
*/
static string_address expand_tilde(string_address step, bool assignment);

static fn expand_into(string_address text, bool quoted, p8 plain,
                      bool assignment)
{
        string_address step = text;
        bool tilde = assignment;

        while (!expand_failed && string_get(step))
        {
                positive run = string_span(step, quoted ? expand_inside_set
                                                        : expand_plain_set);
                p8 seen;

                if (assignment && !quoted)
                {
                        positive at = 0;

                        while (at < run)
                        {
                                if (string_is(step + at, '~') &&
                                    (!at ? tilde : string_is(step + at - 1, ':')))
                                        break;

                                at++;
                        }

                        run = at;
                }

                if (run)
                {
                        expand_push_run(step, run, quoted ? MARK_QUOTED : plain);
                        tilde = assignment && string_is(step + run - 1, ':');
                        step += run;
                        continue;
                }

                seen = string_get(step);

                if (assignment && !quoted && tilde && seen == '~')
                {
                        string_address after = expand_tilde(step, true);

                        if (after != step)
                        {
                                step = after;
                                tilde = false;
                                continue;
                        }
                }

                tilde = false;

                if (seen == '\\' && string_get(step + 1))
                {
                        step++;
                        expand_push(string_get(step++), MARK_QUOTED);
                        continue;
                }

                if (seen == '\'' && !quoted)
                {
                        string_address stop;

                        expand_quoted_seen = true;
                        step++;
                        stop = string_first_of_or_end(step, '\'');

                        if (stop == step)
                                expand_explicit_empty = true;

                        expand_push_run(step, (positive)(stop - step), MARK_QUOTED);
                        step = stop;

                        if (string_get(step))
                                step++;

                        continue;
                }

                if (seen == '"')
                {
                        step = expand_double(step);
                        continue;
                }

                if (seen == '$')
                {
                        step = expand_dollar(step, quoted);
                        continue;
                }

                if (seen == '`')
                {
                        step = expand_backtick(step, quoted);
                        continue;
                }

                expand_push(seen, quoted ? MARK_QUOTED : plain);
                step++;
        }
}

/*
        A tilde, and only at the very front of a word.

        What HOME says, and no splitting afterwards: a home directory with a
        space in it is still one directory.
*/
static string_address expand_tilde(string_address step, bool assignment)
{
        string_address home;

        // ~name wants a password file, and there is none on this machine.
        if (string_not(step + 1, end) && string_not(step + 1, '/') &&
            (!assignment || string_not(step + 1, ':')))
                return step;

        home = env_get((const_string) "HOME");

        if (!home)
                return step;

        expand_push_string(home, MARK_QUOTED);

        return step + 1;
}

static fn expand_word(string_address word)
{
        string_address step = word;

        expand_begin();

        if (string_is(word, '~'))
                step = expand_tilde(word, false);

        expand_into(step, false, MARK_PLAIN, false);

        /*
                A word that is only "$@" and has no parameters behind it is no
                word: f "$@" hands a function nothing, where it used to hand
                it one empty argument and $# came back 1. The quotes are what
                make "" an empty argument everywhere else, and they still do
                -- "a$@b" and "$nosuch$@" are both an empty field here as they
                are in dash. This is the one shape that disappears.
        */
        if (!expand_length &&
            ((!shell_parameter_count &&
              (string_equals(word, "\"$@\"") ||
               string_equals(word, "\"${@}\""))) ||
             (expand_name_at_empty && !expand_explicit_empty)))
                expand_quoted_seen = false;
}

static bool expand_word_ready(string_address word)
{
        expand_word(word);

        // An interactive fatal marks the word failed and returns so the
        // reader can recover on its next line. Do not mistake that empty,
        // aborted result for an allocation failure while emitting fields.
        if (expand_failed)
                return false;

        if (!expand_overflow)
                return true;

        expand_too_long(word);
        return false;
}

//      A directory may hold any number of names, so the answer to a pattern
//      is not allowed a ceiling either. The table of matches may move; the
//      names themselves may not, since the table points at them.
static string_address address_to glob_result;
static positive glob_room;
static string_address address_to expand_sort_room;
static positive expand_sort_room_count;
static shell_store glob_store;
static positive glob_count;
static bool glob_failed;

static PURE bool glob_magic(string_address pattern)
{
        while (string_get(pattern))
        {
                if (string_is(pattern, '\\') && string_get(pattern + 1))
                {
                        pattern += 2;
                        continue;
                }

                if (string_is(pattern, '*') || string_is(pattern, '?') ||
                    string_is(pattern, '['))
                        return true;

                pattern++;
        }

        return false;
}

static bool glob_add(string_address path)
{
        positive length = string_length(path) + 1;
        p8 address_to bytes;

        if (!shell_array_room(glob_result, glob_room, glob_count + 1))
        {
                glob_failed = true;
                return false;
        }

        bytes = shell_store_take(address_of glob_store, length);

        if (!bytes)
        {
                glob_failed = true;
                return false;
        }

        memory_copy(bytes, path, length);
        glob_result[glob_count++] = bytes;

        return true;
}

static bool glob_exists(string_address path)
{
        p8 facts[256];

        return system_stat_at(AT_FDCWD, path, 0x100, 0, facts) == 0;
}

/*
        One pattern against the filesystem, a component at a time.

        A component with nothing magic in it is joined on without a look; one
        that has is read out of the directory above it. Only paths that are
        really there come back, which is what makes a pattern that matches
        nothing stay a pattern.
*/
static fn glob_walk(p8 address_to prefix, positive used, string_address pattern, positive depth)
{
        p8 component[GLOB_PATH];
        positive length = 0;
        string_address rest;

        if (depth >= GLOB_DEPTH)
        {
                glob_failed = true;
                return;
        }

        // A run of slashes belongs to the prefix, not to any component.
        while (string_is(pattern, '/'))
        {
                if (used + 1 >= GLOB_PATH)
                {
                        glob_failed = true;
                        return;
                }

                prefix[used++] = '/';

                pattern++;
        }

        if (!string_get(pattern))
        {
                prefix[used] = end;

                if (used && glob_exists(prefix))
                        glob_add(prefix);

                return;
        }

        while (string_get(pattern) && string_not(pattern, '/'))
        {
                if (length + 2 >= GLOB_PATH)
                {
                        glob_failed = true;
                        return;
                }

                if (string_is(pattern, '\\') && string_get(pattern + 1))
                        component[length++] = string_get(pattern++);

                component[length++] = string_get(pattern++);
        }

        component[length] = end;
        rest = pattern;

        if (!glob_magic(component))
        {
                positive out = used;
                positive at = 0;

                // The backslashes were only ever there to hide the magic; the
                // name on disk does not have them.
                while (at < length)
                {
                        if (component[at] == '\\' && at + 1 < length)
                                at++;

                        if (out + 1 >= GLOB_PATH)
                        {
                                glob_failed = true;
                                return;
                        }

                        prefix[out++] = component[at];

                        at++;
                }

                glob_walk(prefix, out, rest, depth + 1);
                return;
        }

        {
                p8 block[2048];
                bipolar directory;

                prefix[used] = end;

                directory = system_open_at(AT_FDCWD,
                                          (used ? prefix : (string_address) "."),
                                          FILE_READ | O_DIRECTORY);

                if (directory < 0)
                        return;

                while (1)
                {
                        bipolar got = system_read_directory(
                            directory, block, sizeof(block));
                        p8 address_to step = block;

                        if (got < 0)
                        {
                                glob_failed = true;
                                break;
                        }

                        if (!got)
                                break;

                        while (step < block + got)
                        {
                                struct linux_dirent64 address_to entry =
                                    (struct linux_dirent64 address_to)step;
                                string_address named = (string_address)entry->d_name;
                                positive out = used;
                                positive at = 0;

                                step += entry->d_reclen;

                                // A leading dot is only ever matched on purpose.
                                if (named[0] == '.' && component[0] != '.')
                                        continue;

                                if (!shell_match(component, named))
                                        continue;

                                while (named[at])
                                {
                                        if (out + 1 >= GLOB_PATH)
                                        {
                                                glob_failed = true;
                                                break;
                                        }

                                        prefix[out++] = named[at++];
                                }

                                if (glob_failed)
                                        break;

                                glob_walk(prefix, out, rest, depth + 1);

                                if (glob_failed)
                                        break;
                        }

                        if (glob_failed)
                                break;
                }

                system_close(directory);
        }
}

// POSIX asks for the matches in order, and a script that reads a directory
// twice should be told the same story both times.
static bool expand_sort_names(string_address address_to names, positive count)
{
        if (count < 2)
                return true;

        if (!shell_array_room(expand_sort_room, expand_sort_room_count, count))
                return false;

        string_address address_to source = array_merge_sort(
            names, expand_sort_room, count, string_compare);

        if (source != names)
                memory_copy(names, source, count * sizeof(string_address));

        return true;
}

static inline INLINE string_address expand_keep_bytes(string_address text,
                                                       positive length)
{
        string_address result = shell_store_take(address_of expand_store,
                                                 length + 1);

        if (!result)
        {
                expand_fail_state();
                return (string_address) "";
        }

        memory_copy_end(result, text, length);

        return result;
}

/*
        One field, out of the working buffer and into the arena -- and through
        the filesystem on the way if anything unquoted in it was magic.

        This is the last place the marks exist: what leaves here is bytes.
*/
static bool expand_emit(positive at, positive stop, shell_words address_to out)
{
        p8 address_to pattern;
        positive room = 1;
        positive used = 0;
        positive index;
        bool magic = false;

        /*
                A bracket only makes a word a pattern if it closes.

                POSIX is plain that an unterminated '[' is a literal
                character, and nothing here enforced it. So the commonest
                command in any script -- the test builtin, whose other name
                is '[' -- was a pattern, and every single one of them opened
                a directory and read it to the end looking for a file called
                that. A loop written with '[' ran six times slower than the
                identical loop written with 'test': 0.590s against 0.099s
                over three hundred thousand turns, and one and a quarter
                million system calls against thirty three.

                What is allowed to close it comes from the same page. After
                the '[' an optional '!' or '^' does not end it, and a ']'
                standing immediately after either of those is itself literal
                -- "[]]" is the bracket expression that matches a bracket. So
                the first ']' that can close is the one after that, and the
                three states below are which of those places the scan is
                standing in.
        */
        positive bracket = 0;

        for (index = at; index < stop; index++)
        {
                if (room == positive_max)
                        break;

                room++;

                if (expand_mark[index] == MARK_QUOTED &&
                    (expand_text[index] == '*' || expand_text[index] == '?' ||
                     expand_text[index] == '[' || expand_text[index] == '\\'))
                        room++;
        }

        pattern = index == stop
                      ? shell_store_take(address_of expand_store, room)
                      : null;

        if (!pattern)
        {
                expand_fail_state();
                return false;
        }

        for (index = at; index < stop; index++)
        {
                p8 value = expand_text[index];
                bool special = value == '*' || value == '?' || value == '[';

                if (expand_mark[index] == MARK_QUOTED)
                {
                        if (special || value == '\\')
                                pattern[used++] = '\\';
                }
                else if (value == '*' || value == '?')
                        magic = true;
                else if (bracket == 0)
                {
                        if (value == '[')
                                bracket = 1;
                }
                else if (bracket == 1 && (value == '!' || value == '^'))
                        bracket = 2;
                else if (bracket == 3 && value == ']')
                        magic = true;
                else
                        bracket = 3;

                pattern[used++] = value;
        }

        pattern[used] = end;

        // set -f was kept and never asked about, so a script that turned
        // globbing off to hold a pattern still had it read the directory.
        if (magic && !(shell_options & SHELL_NO_GLOB))
        {
                p8 built[GLOB_PATH];

                glob_count = 0;
                glob_failed = false;
                shell_store_reset(address_of glob_store);
                glob_walk(built, 0, pattern, 0);

                if (glob_failed)
                {
                        expand_fail_state();
                        return false;
                }

                if (glob_count)
                {
                if (!expand_sort_names(glob_result, glob_count))
                        {
                                expand_fail_state();
                                return false;
                        }

                        for (index = 0; index < glob_count; index++)
                        {
                                string_address name = glob_result[index];
                                string_address kept = expand_keep_bytes(
                                    name, string_length(name));

                                if (expand_failed || !shell_words_add(out, kept))
                                {
                                        expand_fail_state();
                                        return false;
                                }
                        }

                        return true;
                }
        }

        {
                string_address kept = expand_keep_bytes(expand_text + at,
                                                        stop - at);

                if (expand_failed || !shell_words_add(out, kept))
                {
                        expand_fail_state();
                        return false;
                }
        }

        return true;
}

/*
        Field splitting.

        Only bytes that came out of an unquoted expansion can be a separator, so
        the marks do the whole of the deciding. A run of IFS whitespace is one
        separator; anything else in IFS is a separator on its own, with the
        whitespace around it swallowed.
*/
static positive expand_split(shell_words address_to out)
{
        positive at = 0;
        positive start;

        expand_ifs_prepare();

        // A word that expanded to nothing at all is no field, unless some part
        // of it was quoted: "" is an empty argument and $nosuch is no argument.
        if (!expand_length)
        {
                if (expand_quoted_seen)
                        expand_emit(0, 0, out);

                return out->count;
        }

        while (at < expand_length && expand_mark[at] == MARK_FIELD &&
               expand_ifs_blank(expand_text[at]))
                at++;

        // Nothing but separators: a word of blanks out of a variable is no
        // word at all.
        if (at >= expand_length)
                return out->count;

        start = at;

        while (at < expand_length)
        {
                if (expand_mark[at] == MARK_BREAK)
                {
                        expand_emit(start, at, out);
                        at++;
                        start = at;
                        continue;
                }

                if (expand_mark[at] == MARK_FIELD && expand_in_ifs(expand_text[at]))
                {
                        expand_emit(start, at, out);

                        while (at < expand_length && expand_mark[at] == MARK_FIELD &&
                               expand_ifs_blank(expand_text[at]))
                                at++;

                        if (at < expand_length && expand_mark[at] == MARK_FIELD &&
                            expand_in_ifs(expand_text[at]))
                        {
                                at++;

                                while (at < expand_length && expand_mark[at] == MARK_FIELD &&
                                       expand_ifs_blank(expand_text[at]))
                                        at++;
                        }

                        start = at;

                        // Separators at the end make no empty field after them.
                        if (at >= expand_length)
                                return out->count;

                        continue;
                }

                at++;
        }

        expand_emit(start, expand_length, out);

        return out->count;
}

static PURE string_address expand_brace_comma(string_address at,
                                         string_address close)
{
        positive depth = 0;

        while (at < close)
        {
                p8 value = string_get(at);

                if (value == '$' && string_is(at + 1, '\''))
                {
                        at = expand_dollar_quoted_run(at);
                        continue;
                }

                if (value == '\\' && at + 1 < close)
                {
                        at += 2;
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        p8 quote = value;

                        at++;
                        while (at < close && string_not(at, quote))
                                at += quote == '"' && string_is(at, '\\') &&
                                              at + 1 < close
                                          ? 2
                                          : 1;
                        if (at < close)
                                at++;
                        continue;
                }

                if (value == '$' && string_is(at + 1, '{'))
                {
                        string_address stop = expand_brace_end(at + 2);

                        if (stop && stop < close)
                        {
                                at = stop + 1;
                                continue;
                        }
                }

                if (value == '{')
                        depth++;
                else if (value == '}' && depth)
                        depth--;
                else if (!depth && value == ',')
                        return at;

                at++;
        }

        return null;
}

static bool expand_brace_number(string_address text, positive length,
                                bipolar address_to value,
                                positive address_to width,
                                bool address_to padded)
{
        positive at = 0;
        positive magnitude = 0;
        bool minus = false;
        positive limit;

        if (at < length && text[at] == '-')
        {
                minus = true;
                at++;
        }

        if (at >= length)
                return false;

        address_to width = length;
        address_to padded = at + 1 < length && text[at] == '0';
        limit = minus ? (positive)bipolar_max + 1 : (positive)bipolar_max;

        for (; at < length; at++)
        {
                positive digit = text[at] - '0';

                if (text[at] < '0' || text[at] > '9' ||
                    magnitude > (limit - digit) / 10)
                        return false;

                magnitude = magnitude * 10 + digit;
        }

        address_to value = bipolar_from_magnitude(magnitude, minus);

        return true;
}

static positive expand_brace_number_text(p8 address_to out, bipolar value,
                                         positive width, bool padded)
{
        p8 made[32];
        positive length = bipolar_into_string(made, value);
        positive zeros;
        positive at = 0;
        positive from = 0;

        if (!padded || length >= width)
        {
                memory_copy(out, made, length);
                return length;
        }

        if (made[0] == '-')
        {
                out[at++] = '-';
                from = 1;
        }

        zeros = width - length;
        memory_fill(out + at, '0', zeros);
        at += zeros;
        memory_copy(out + at, made + from, length - from);

        return at + length - from;
}

static positive shell_expand_braces(string_address word,
                                    shell_words address_to out);

static positive expand_brace_made(string_address word,
                                  string_address open,
                                  string_address close,
                                  string_address middle,
                                  positive middle_length,
                                  shell_words address_to out)
{
        positive prefix = (positive)(open - word);
        positive suffix = string_length(close + 1);
        p8 address_to made;

        if (prefix > positive_max - middle_length ||
            prefix + middle_length > positive_max - suffix - 1 ||
            !(made = shell_store_take(address_of expand_store,
                                      prefix + middle_length + suffix + 1)))
        {
                expand_fail_state();
                return out->count;
        }

        memory_copy(made, word, prefix);
        memory_copy(made + prefix, middle, middle_length);
        memory_copy_end(made + prefix + middle_length, close + 1, suffix);

        return shell_expand_braces(made, out);
}

static bool expand_brace_range(string_address word, string_address open,
                               string_address close, shell_words address_to out)
{
        string_address first_dots = null;
        string_address second_dots = null;
        string_address at = open + 1;
        positive depth = 0;
        bipolar first_number, last_number, step_number = 0;
        positive first_width = 0, last_width = 0;
        bool first_padded = false, last_padded = false;
        bool numeric;

        while (at + 1 < close)
        {
                if (string_is(at, '{'))
                        depth++;
                else if (string_is(at, '}') && depth)
                        depth--;
                else if (!depth && string_is(at, '.') && string_is(at + 1, '.'))
                {
                        if (!first_dots)
                                first_dots = at;
                        else if (!second_dots)
                                second_dots = at;
                        else
                                return false;

                        at += 2;
                        continue;
                }

                at++;
        }

        if (!first_dots || first_dots == open + 1 || first_dots + 2 == close ||
            (second_dots && second_dots + 2 == close))
                return false;

        numeric = expand_brace_number(open + 1,
                                      (positive)(first_dots - open - 1),
                                      address_of first_number,
                                      address_of first_width,
                                      address_of first_padded) &&
                  expand_brace_number(first_dots + 2,
                                      (positive)((second_dots ? second_dots : close) -
                                                 first_dots - 2),
                                      address_of last_number,
                                      address_of last_width,
                                      address_of last_padded);

        if (second_dots)
        {
                positive ignored_width;
                bool ignored_padded;

                if (!expand_brace_number(second_dots + 2,
                                         (positive)(close - second_dots - 2),
                                         address_of step_number,
                                         address_of ignored_width,
                                         address_of ignored_padded))
                        return false;
        }

        if (numeric)
        {
                bipolar step;
                bipolar current = first_number;
                positive width = first_width > last_width ? first_width : last_width;
                bool padded = first_padded || last_padded;

                if (step_number == bipolar_min)
                        return false;

                step = step_number < 0 ? -step_number : step_number;
                if (!step)
                        step = 1;
                if (first_number > last_number)
                        step = -step;

                while ((step > 0 && current <= last_number) ||
                       (step < 0 && current >= last_number))
                {
                        p8 made[32];
                        positive length = expand_brace_number_text(
                            made, current, width, padded);

                        expand_brace_made(word, open, close, made, length, out);

                        if (expand_failed || current == last_number ||
                            (step > 0 && current > bipolar_max - step) ||
                            (step < 0 && current < bipolar_min - step))
                                break;

                        current += step;
                }

                return true;
        }

        if (first_dots == open + 2 &&
            first_dots + 3 == (second_dots ? second_dots : close))
        {
                bipolar first = string_get(open + 1);
                bipolar last = string_get(first_dots + 2);
                bipolar magnitude;
                bipolar step;

                if (step_number == bipolar_min)
                        return false;

                magnitude = step_number < 0 ? -step_number : step_number;
                if (!magnitude)
                        magnitude = 1;
                step = first <= last ? magnitude : -magnitude;

                for (bipolar current = first;
                     step > 0 ? current <= last : current >= last;
                     current += step)
                {
                        p8 made = (p8)current;

                        expand_brace_made(word, open, close, address_of made, 1,
                                          out);

                        if (expand_failed || current == last)
                                break;
                }

                return true;
        }

        return false;
}

static positive shell_expand_braces(string_address word,
                                    shell_words address_to out)
{
        string_address open = word;

        while (string_get(open))
        {
                string_address close;
                string_address comma;
                string_address piece;

                if (string_is(open, '\\') && string_get(open + 1))
                {
                        open += 2;
                        continue;
                }

                if (string_is(open, '$') && string_is(open + 1, '\''))
                {
                        open = expand_dollar_quoted_run(open);
                        continue;
                }

                if (string_is(open, '\'') || string_is(open, '"'))
                {
                        open = expand_quoted_run(open, string_get(open));
                        continue;
                }

                if (string_is(open, '$') && string_is(open + 1, '{'))
                {
                        close = expand_brace_end(open + 2);
                        open = close ? close + 1 : open + 1;
                        continue;
                }

                if (string_not(open, '{') ||
                    !(close = expand_brace_end(open + 1)))
                {
                        open++;
                        continue;
                }

                comma = expand_brace_comma(open + 1, close);

                if (!comma)
                {
                        if (expand_brace_range(word, open, close, out))
                                return out->count;

                        open++;
                        continue;
                }

                piece = open + 1;

                while (piece <= close)
                {
                        comma = expand_brace_comma(piece, close);
                        expand_brace_made(word, open, close, piece,
                                          (positive)((comma ? comma : close) - piece),
                                          out);
                        if (expand_failed || !comma)
                                return out->count;
                        piece = comma + 1;
                }

                return out->count;
        }

        if (!expand_word_ready(word))
                return out->count;

        return expand_split(out);
}

/*
        One lexed word, expanded whole, and the fields it became written out.

        The answer is not one word. $@ makes as many as there are parameters, an
        unquoted variable with a space in it makes two, a glob makes as many as
        the directory holds, and an unset variable on its own makes none at all
        -- which is the difference between "rm $file" deleting one thing and
        deleting the working directory.
*/
positive shell_expand_fields(string_address word, shell_words address_to out)
{
        positive count = shell_expand_braces(word, out);

        if (expand_overflow)
        {
                expand_too_long(word);
        }

        return count;
}

/*
        The same word, kept whole.

        A redirection target and the right hand side of an assignment are the
        two places POSIX does not split and does not glob, and this is what they
        are supposed to call.
*/
RETURNS_NONNULL string_address shell_expand_word(string_address word)
{
        string_address result;

        if (!expand_word_ready(word))
                return (string_address) "";

        result = expand_keep_bytes(expand_text, expand_length);

        if (expand_overflow)
        {
                expand_too_long(word);
                return (string_address) "";
        }

        return result;
}

/*
        A declaration operand is an assignment even though it follows the
        command name. Keep it whole like a leading assignment, and recognize
        the additional tilde-prefix positions after '=' and unquoted ':'.
*/
RETURNS_NONNULL string_address shell_expand_assignment(string_address word, positive value_at)
{
        string_address result;

        expand_begin();
        expand_push_run(word, value_at, MARK_PLAIN);
        expand_into(word + value_at, false, MARK_PLAIN, true);

        result = expand_keep_bytes(expand_text, expand_length);

        if (expand_overflow)
        {
                expand_too_long(word);
                return (string_address) "";
        }

        return result;
}

/*
        A case pattern, kept whole and with its quote marks translated into
        matcher escapes before those marks disappear.

        Ordinary word expansion deliberately returns only bytes. That made a
        star from "$p" indistinguishable from an unquoted star, and made an
        escaped star active again. The matcher already uses backslash for a
        literal metacharacter, so preserving that distinction needs no second
        pattern language.
*/
RETURNS_NONNULL string_address shell_expand_pattern(string_address word)
{
        positive room = 1;
        positive at;
        p8 address_to result;
        positive used = 0;

        if (!expand_word_ready(word))
                return (string_address) "";

        for (at = 0; at < expand_length; at++)
        {
                p8 value = expand_text[at];

                room++;

                if (expand_mark[at] == MARK_QUOTED &&
                    (value == '*' || value == '?' || value == '[' || value == '\\'))
                        room++;
        }

        result = shell_store_take(address_of expand_store, room);

        if (!result)
        {
                expand_overflow = true;
                expand_too_long(word);
                return (string_address) "";
        }

        for (at = 0; at < expand_length; at++)
        {
                p8 value = expand_text[at];

                if (expand_mark[at] == MARK_QUOTED &&
                    (value == '*' || value == '?' || value == '[' || value == '\\'))
                        result[used++] = '\\';

                result[used++] = value;
        }

        result[used] = end;

        return result;
}

// Quoted pieces of a [[ string =~ regex ]] right hand side are literal.
// Translate that byte-level distinction into ERE backslashes before the
// expander's quote marks disappear.
static CONST bool expand_regex_metacharacter(p8 value)
{
        return value == '.' || value == '^' || value == '$' || value == '*' ||
               value == '+' || value == '?' || value == '(' || value == ')' ||
               value == '[' || value == ']' || value == '{' || value == '}' ||
               value == '|' || value == '\\';
}

RETURNS_NONNULL string_address shell_expand_regex(string_address word)
{
        positive room = 1;
        positive at;
        positive used = 0;
        p8 address_to result;

        if (!expand_word_ready(word))
                return (string_address) "";

        for (at = 0; at < expand_length; at++)
        {
                if (room == positive_max)
                        break;

                room++;

                if (expand_mark[at] == MARK_QUOTED &&
                    expand_regex_metacharacter(expand_text[at]))
                {
                        if (room == positive_max)
                                break;

                        room++;
                }
        }

        if (at != expand_length ||
            !(result = shell_store_take(address_of expand_store, room)))
        {
                expand_overflow = true;
                expand_too_long(word);
                return (string_address) "";
        }

        for (at = 0; at < expand_length; at++)
        {
                p8 value = expand_text[at];

                if (expand_mark[at] == MARK_QUOTED &&
                    expand_regex_metacharacter(value))
                        result[used++] = '\\';

                result[used++] = value;
        }

        result[used] = end;
        return result;
}
