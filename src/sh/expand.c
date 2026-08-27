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

#include "../library.c"

/*
        What the files beside this one own.

        Named rather than reached for, so this file can be included on either
        side of builtin.c without the two of them needing each other first.
*/
string_address env_get(const_string name);
bool env_set(const_string name, const_string value);
fn run_line(string_address line);
fn parse_reset_all();

extern writer shell_output;
extern positive shell_output_file;
extern b32 shell_status;

/*
        What the last command substitution answered.

        Not $? -- a substitution inside a command word must not change what the
        rest of that same command sees, or "echo $(false)$?" reports on itself.
        An assignment whose whole right hand side is a substitution is the one
        place the number is wanted, and that is the shell's to notice.
*/
b32 shell_substitution_status;

#define EXPAND_WORK 8192
#define EXPAND_ARENA 32768
#define EXPAND_NAME 128
#define EXPAND_VALUE 1024
#define EXPAND_DEPTH 16

#define GLOB_RESULTS 256
#define GLOB_BYTES 8192
#define GLOB_PATH 1024
#define GLOB_DEPTH 24

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

static p8 expand_text[EXPAND_WORK];
static p8 expand_mark[EXPAND_WORK];
static positive expand_length;
static bool expand_overflow;
static bool expand_quoted_seen;
static positive expand_depth;

static p8 expand_arena[EXPAND_ARENA];
static positive expand_arena_used;

// The line is over and every word it made is dead with it.
fn shell_expand_reset()
{
        expand_arena_used = 0;
}

// One short of the end, because trimming writes a terminator at the length.
static fn expand_push(p8 value, p8 mark)
{
        if (expand_length + 1 >= EXPAND_WORK)
        {
                expand_overflow = true;
                return;
        }

        expand_mark[expand_length] = mark;
        expand_text[expand_length++] = value;
}

static fn expand_push_string(string_address text, p8 mark)
{
        while (text && string_get(text))
                expand_push(string_get(text++), mark);
}

static fn expand_complain(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        system_call_3(syscall(write), stderr, (positive)data, length);
}

static bool expand_name_character(p8 value)
{
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') ||
               value == '_';
}

static fn expand_copy_bounded(p8 address_to into, string_address from, positive limit)
{
        positive used = 0;

        while (from && string_get(from) && used + 1 < limit)
                into[used++] = string_get(from++);

        into[used] = end;
}

static positive expand_number_out(bipolar value, p8 address_to into)
{
        p8 digits[24];
        positive at = 0;
        positive used = 0;
        positive magnitude;

        if (value < 0)
        {
                into[used++] = '-';
                magnitude = (positive)(-value);
        }
        else
                magnitude = (positive)value;

        if (!magnitude)
                digits[at++] = '0';

        while (magnitude)
        {
                digits[at++] = (p8)('0' + magnitude % 10);
                magnitude /= 10;
        }

        while (at)
                into[used++] = digits[--at];

        into[used] = end;

        return used;
}

static bipolar expand_number_in(string_address text)
{
        bipolar value = 0;
        bool negative = false;

        while (string_is(text, ' ') || string_is(text, '\t'))
                text++;

        if (string_is(text, '-') || string_is(text, '+'))
        {
                negative = string_is(text, '-');
                text++;
        }

        while (string_get(text) >= '0' && string_get(text) <= '9')
                value = value * 10 + (string_get(text++) - '0');

        return negative ? -value : value;
}

/*
        A bracket set: where it ends, and whether a byte is in it.

        A [ with no ] anywhere after it is a plain [ and not a set at all, which
        is why the end is found first and the membership asked second.
*/
static string_address expand_set_end(string_address at)
{
        string_address step = at + 1;

        if (string_is(step, '!') || string_is(step, '^'))
                step++;

        // The first ] is a member, not the close.
        if (string_is(step, ']'))
                step++;

        while (string_get(step) && string_not(step, ']'))
                step++;

        return string_get(step) ? step : null;
}

static bool expand_in_set(string_address at, string_address stop, p8 value)
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
                p8 low = string_get(step);

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
bool shell_match(string_address pattern, string_address text)
{
        while (string_get(pattern))
        {
                p8 value = string_get(pattern);

                if (value == '*')
                {
                        pattern++;

                        // A star at the end takes whatever is left of it.
                        if (!string_get(pattern))
                                return true;

                        while (1)
                        {
                                if (shell_match(pattern, text))
                                        return true;

                                if (!string_get(text))
                                        return false;

                                text++;
                        }
                }

                if (value == '[')
                {
                        string_address stop = expand_set_end(pattern);

                        if (stop)
                        {
                                if (!string_get(text))
                                        return false;

                                if (!expand_in_set(pattern, stop, string_get(text)))
                                        return false;

                                pattern = stop + 1;
                                text++;
                                continue;
                        }
                }

                if (!string_get(text))
                        return false;

                if (value == '?')
                {
                        pattern++;
                        text++;
                        continue;
                }

                if (value == '\\' && string_get(pattern + 1))
                        value = string_get(++pattern);

                if (value != string_get(text))
                        return false;

                pattern++;
                text++;
        }

        return string_get(text) == end;
}

#define EXPAND_PARAMETERS 64
#define EXPAND_PARAMETER_BYTES 4096

string_address shell_parameter[EXPAND_PARAMETERS + 1];
positive shell_parameter_count;
string_address shell_script_name = (string_address) "sh";
string_address shell_option_flags = (string_address) "s";

static p8 shell_parameter_bytes[EXPAND_PARAMETER_BYTES];
static p8 shell_parameter_staging[EXPAND_PARAMETER_BYTES];

/*
        Where set -- and shift keep their words.

        The copy goes through a staging block because "set -- $@" hands back the
        very bytes it is about to be written over.
*/
bool shell_parameters_set(string_address address_to words, positive count)
{
        positive used = 0;
        positive index = 0;
        positive at;

        if (count > EXPAND_PARAMETERS)
                count = EXPAND_PARAMETERS;

        while (index < count)
        {
                positive length = string_length(words[index]) + 1;

                if (used + length > EXPAND_PARAMETER_BYTES)
                        break;

                memory_copy(shell_parameter_staging + used, words[index], length);
                used += length;
                index++;
        }

        memory_copy(shell_parameter_bytes, shell_parameter_staging, used);

        shell_parameter_count = index;
        used = 0;

        for (at = 0; at < index; at++)
        {
                shell_parameter[at] = shell_parameter_bytes + used;
                used += string_length(shell_parameter_bytes + used) + 1;
        }

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
#define EXPAND_PARAMETER_STACK 16384
#define EXPAND_NO_ROOM ((positive)-1)

static p8 shell_parameter_stack[EXPAND_PARAMETER_STACK];
static positive shell_parameter_stack_used;

positive shell_parameters_save()
{
        positive mark = shell_parameter_stack_used;
        positive used = 0;
        positive at;

        for (at = 0; at < shell_parameter_count; at++)
                used += string_length(shell_parameter[at]) + 1;

        if (mark + used > EXPAND_PARAMETER_STACK)
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

fn shell_parameters_restore(positive mark, positive count)
{
        string_address words[EXPAND_PARAMETERS];
        positive at = mark;
        positive index;

        if (mark == EXPAND_NO_ROOM)
                return;

        if (count > EXPAND_PARAMETERS)
                count = EXPAND_PARAMETERS;

        for (index = 0; index < count; index++)
        {
                words[index] = shell_parameter_stack + at;
                at += string_length(shell_parameter_stack + at) + 1;
        }

        shell_parameters_set(words, count);
        shell_parameter_stack_used = mark;
}

fn shell_parameters_shift(positive count)
{
        positive index = 0;

        if (count > shell_parameter_count)
                count = shell_parameter_count;

        while (index + count < shell_parameter_count)
        {
                shell_parameter[index] = shell_parameter[index + count];
                index++;
        }

        shell_parameter_count -= count;
        shell_parameter[shell_parameter_count] = null;
}

/*
        IFS unset is the default three; IFS set to nothing splits nothing at
        all, and collapsing the two is how a script that clears IFS on purpose
        gets its fields taken apart anyway.
*/
static string_address expand_ifs()
{
        string_address value = env_get((const_string) "IFS");

        return value ? value : (string_address) " \t\n";
}

/*
        IFS as one byte per byte value, built once a word.

        Splitting asks about every byte it walks over and the answer is the same
        every time; asking env_get for it each time turned a field into a linear
        walk of the environment per character.
*/
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

static bool expand_in_ifs(p8 value)
{
        return expand_ifs_set[value] != 0;
}

static bool expand_ifs_blank(p8 value)
{
        return expand_ifs_blank_set[value] != 0;
}

// What a parameter stands for, and whether it stands for anything at all.
static bool expand_value_of(string_address name, p8 address_to into, positive limit)
{
        p8 first = string_get(name);

        into[0] = end;

        if (first >= '0' && first <= '9')
        {
                positive which = 0;

                while (string_get(name) >= '0' && string_get(name) <= '9')
                        which = which * 10 + (string_get(name++) - '0');

                if (!which)
                {
                        expand_copy_bounded(into, shell_script_name, limit);
                        return true;
                }

                if (which > shell_parameter_count)
                        return false;

                expand_copy_bounded(into, shell_parameter[which - 1], limit);
                return true;
        }

        if (string_get(name + 1) == end)
        {
                if (first == '#')
                {
                        expand_number_out((bipolar)shell_parameter_count, into);
                        return true;
                }

                if (first == '?')
                {
                        expand_number_out((bipolar)shell_status, into);
                        return true;
                }

                if (first == '$')
                {
                        expand_number_out(system_call_1(syscall(getpid), 0), into);
                        return true;
                }

                // No job control here, so the last background job is nothing
                // rather than a number that would be a lie.
                if (first == '!')
                        return true;

                if (first == '-')
                {
                        expand_copy_bounded(into, shell_option_flags, limit);
                        return true;
                }

                if (first == '@' || first == '*')
                {
                        string_address ifs = expand_ifs();
                        p8 between = string_get(ifs);
                        positive used = 0;
                        positive at;

                        for (at = 0; at < shell_parameter_count; at++)
                        {
                                string_address from = shell_parameter[at];

                                if (at && between && used + 1 < limit)
                                        into[used++] = between;

                                while (string_get(from) && used + 1 < limit)
                                        into[used++] = string_get(from++);
                        }

                        into[used] = end;
                        return true;
                }
        }

        {
                string_address value = env_get(name);

                if (!value)
                        return false;

                expand_copy_bounded(into, value, limit);
                return true;
        }
}

/*
        A parameter pushed with the marks that decide its fate later.

        $@ is the one form that makes its own field boundaries, quoted or not:
        they go in as a byte of their own so that splitting cannot miss them and
        "$@" keeps a parameter with a space in it whole.
*/
static bool expand_push_parameter(string_address name, bool quoted)
{
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        p8 value[EXPAND_VALUE];

        if (string_is(name, '@') && string_get(name + 1) == end)
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

        if (!expand_value_of(name, value, sizeof(value)))
                return false;

        expand_push_string(value, mark);

        return true;
}

static fn expand_into(string_address text, bool quoted);
static string_address expand_double(string_address step);
static string_address expand_dollar(string_address step, bool quoted);
static string_address expand_backtick(string_address step, bool quoted);

/*
        A nested word -- the tail of ${x-...}, a trimming pattern, the body of
        an arithmetic expression -- run through the whole expander and lifted
        back out, leaving the working buffer exactly as it was found.

        A pattern gets a backslash in front of every byte that was quoted, so
        that ${x%"*"} strips one star and not everything.
*/
static positive expand_capture(string_address text, bool quoted, p8 address_to into,
                               positive limit, bool as_pattern)
{
        positive at = expand_length;
        bool held = expand_quoted_seen;
        positive used = 0;
        positive step;

        expand_into(text, quoted);

        for (step = at; step < expand_length; step++)
        {
                p8 value = expand_text[step];

                if (as_pattern && expand_mark[step] == MARK_QUOTED &&
                    (value == '*' || value == '?' || value == '[' || value == '\\'))
                {
                        if (used + 1 < limit)
                                into[used++] = '\\';
                }

                if (used + 1 < limit)
                        into[used++] = value;
        }

        into[used] = end;
        expand_length = at;
        expand_quoted_seen = held;

        return used;
}

/*
        Arithmetic.

        A cursor in a file scope rather than one passed down, because the whole
        expression is already expanded by the time it gets here: nothing inside
        can start another arithmetic, so there is nothing to be reentrant for.
*/
static string_address arith_at;

static fn arith_space()
{
        while (string_is(arith_at, ' ') || string_is(arith_at, '\t') ||
               string_is(arith_at, '\n'))
                arith_at++;
}

static bipolar arith_or();

// Writing a name back, which every assigning form ends with.
static bipolar arith_store(string_address name, bipolar value)
{
        p8 written[32];

        expand_number_out(value, written);
        env_set(name, written);

        return value;
}

// x++ and x--, answering with what the name held before.
static bipolar arith_step(string_address name, bipolar by)
{
        p8 held[EXPAND_VALUE];
        bipolar was;

        if (!expand_value_of(name, held, sizeof(held)))
                held[0] = end;

        was = expand_number_in(held);
        arith_store(name, was + by);

        return was;
}

// What the operator in front of the = does. Division by zero answers with the
// left hand side rather than trapping, which is what the plain / does here.
static bipolar arith_combine(p8 op, bipolar left, bipolar right)
{
        switch (op)
        {
        case '+': return left + right;
        case '-': return left - right;
        case '*': return left * right;
        case '/': return right ? left / right : left;
        case '%': return right ? left % right : left;
        case '&': return left & right;
        case '|': return left | right;
        case '^': return left ^ right;
        case 'l': return left << right;
        case 'r': return left >> right;
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
                value = arith_or();
                arith_space();

                if (string_is(arith_at, ')'))
                        arith_at++;

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

        if (string_is(arith_at, '-'))
        {
                arith_at++;
                return -arith_primary();
        }

        if (string_is(arith_at, '+'))
        {
                arith_at++;
                return arith_primary();
        }

        if (string_get(arith_at) >= '0' && string_get(arith_at) <= '9')
        {
                while (string_get(arith_at) >= '0' && string_get(arith_at) <= '9')
                        value = value * 10 + (string_get(arith_at++) - '0');

                return value;
        }

        /*
                A name with no dollar in front of it, which is the one place in
                the language where that reads a variable.
        */
        if (expand_name_character(string_get(arith_at)))
        {
                p8 name[EXPAND_NAME];
                p8 held[EXPAND_VALUE];
                positive length = 0;

                while (expand_name_character(string_get(arith_at)) && length < EXPAND_NAME - 1)
                        name[length++] = string_get(arith_at++);

                name[length] = end;
                arith_space();

                if (string_is(arith_at, '=') && string_get(arith_at + 1) != '=')
                {
                        arith_at++;

                        return arith_store(name, arith_or());
                }

                /*
                        The compound forms, which read the name as well as
                        write it: += and its nine relatives, and ++ and --.

                        Longest first, or <<= is < followed by <= and x >>= 1
                        halves nothing. The ones ending in = are all "read,
                        combine, write" and share the tail below; ++ and --
                        answer with the value from before they changed it,
                        which is what a post-increment means.
                */
                {
                        p8 op = 0;
                        b32 skip = 0;

                        if (string_is(arith_at, '+') && string_is(arith_at + 1, '+'))
                        {
                                arith_at += 2;
                                return arith_step(name, 1);
                        }

                        if (string_is(arith_at, '-') && string_is(arith_at + 1, '-'))
                        {
                                arith_at += 2;
                                return arith_step(name, -1);
                        }

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

                                if (!expand_value_of(name, held, sizeof(held)))
                                        held[0] = end;

                                was = expand_number_in(held);

                                return arith_store(name, arith_combine(op, was, arith_or()));
                        }
                }

                if (!expand_value_of(name, held, sizeof(held)))
                        return 0;

                return expand_number_in(held);
        }

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
                        value = value * arith_primary();
                        continue;
                }

                if (string_is(arith_at, '/') || string_is(arith_at, '%'))
                {
                        bool remainder = string_is(arith_at, '%');
                        bipolar divisor;

                        arith_at++;
                        divisor = arith_primary();

                        // A shell that divides by zero should answer nothing,
                        // not take the machine's fault for it.
                        if (!divisor)
                                return 0;

                        value = remainder ? value % divisor : value / divisor;
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
                        value = value + arith_multiply();
                        continue;
                }

                if (string_is(arith_at, '-'))
                {
                        arith_at++;
                        value = value - arith_multiply();
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
                        value = value << arith_add();
                        continue;
                }

                if (string_is(arith_at, '>') && string_is(arith_at + 1, '>') &&
                    !string_is(arith_at + 2, '='))
                {
                        arith_at += 2;
                        value = value >> arith_add();
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
static bipolar arith_bit_and()
{
        bipolar value = arith_equal();

        while (1)
        {
                arith_space();

                if (!string_is(arith_at, '&') || string_is(arith_at + 1, '&') ||
                    string_is(arith_at + 1, '='))
                        return value;

                arith_at++;
                value = value & arith_equal();
        }
}

static bipolar arith_bit_xor()
{
        bipolar value = arith_bit_and();

        while (1)
        {
                arith_space();

                if (!string_is(arith_at, '^') || string_is(arith_at + 1, '='))
                        return value;

                arith_at++;
                value = value ^ arith_bit_and();
        }
}

static bipolar arith_bit_or()
{
        bipolar value = arith_bit_xor();

        while (1)
        {
                arith_space();

                if (!string_is(arith_at, '|') || string_is(arith_at + 1, '|') ||
                    string_is(arith_at + 1, '='))
                        return value;

                arith_at++;
                value = value | arith_bit_xor();
        }
}

static bipolar arith_and()
{
        bipolar value = arith_bit_or();

        while (1)
        {
                bipolar right;

                arith_space();

                if (!string_is(arith_at, '&') || string_get(arith_at + 1) != '&')
                        return value;

                // Both sides are read whatever the left one said: skipping the
                // right side would leave the cursor in the middle of it.
                arith_at += 2;
                right = arith_bit_or();
                value = (value && right);
        }
}

static bipolar arith_or()
{
        bipolar value = arith_and();

        while (1)
        {
                bipolar right;

                arith_space();

                if (!string_is(arith_at, '|') || string_get(arith_at + 1) != '|')
                        return value;

                arith_at += 2;
                right = arith_and();
                value = (value || right);
        }
}

static bipolar arith_evaluate(string_address text)
{
        arith_at = text;

        return arith_or();
}

/*
        The ) that closes a $( ... ), with quotes and nesting counted; nothing
        when the word runs out first, in which case the $ was only a $.
*/
static string_address expand_paren_end(string_address at)
{
        positive depth = 1;

        while (string_get(at))
        {
                p8 value = string_get(at);

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        p8 quote = value;

                        at++;

                        while (string_get(at) && string_not(at, quote))
                        {
                                if (quote == '"' && string_is(at, '\\') && string_get(at + 1))
                                        at++;

                                at++;
                        }

                        if (string_get(at))
                                at++;

                        continue;
                }

                if (value == '(')
                        depth++;

                if (value == ')')
                {
                        depth--;

                        if (!depth)
                                return at;
                }

                at++;
        }

        return null;
}

static string_address expand_brace_end(string_address at)
{
        positive depth = 1;

        while (string_get(at))
        {
                p8 value = string_get(at);

                if (value == '\\' && string_get(at + 1))
                {
                        at += 2;
                        continue;
                }

                if (value == '\'' || value == '"')
                {
                        p8 quote = value;

                        at++;

                        while (string_get(at) && string_not(at, quote))
                        {
                                if (quote == '"' && string_is(at, '\\') && string_get(at + 1))
                                        at++;

                                at++;
                        }

                        if (string_get(at))
                                at++;

                        continue;
                }

                if (value == '{')
                        depth++;

                if (value == '}')
                {
                        depth--;

                        if (!depth)
                                return at;
                }

                at++;
        }

        return null;
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
        bipolar child;
        positive status = 0;

        // Whatever this shell has buffered belongs to this shell, and a fork
        // with it still in hand prints all of it a second time.
        log_flush();

        if (system_call_2(syscall(pipe2), (positive)address_of channel, 0) < 0)
                return;

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                system_call_1(syscall(close), (positive)channel[0]);
                system_call_3(syscall(dup3), (positive)channel[1], stdout, 0);
                system_call_1(syscall(close), (positive)channel[1]);

                shell_output = log;
                shell_output_file = 0;

                // The parser still holds the line this substitution is a word
                // of. In here that is somebody else's half-read sentence, and
                // feeding it another one makes a syntax error out of both.
                parse_reset_all();
                run_line(command);
                log_flush();

                system_call_1(syscall(exit_group), (positive)shell_status);
        }

        system_call_1(syscall(close), (positive)channel[1]);

        if (child > 0)
        {
                while (1)
                {
                        p8 block[512];
                        bipolar got = system_call_3(syscall(read), (positive)channel[0],
                                                    (positive)block, sizeof(block));
                        bipolar at;

                        if (got <= 0)
                                break;

                        for (at = 0; at < got; at++)
                                expand_push(block[at], mark);
                }
        }

        system_call_1(syscall(close), (positive)channel[0]);

        if (child > 0)
                system_call_4(syscall(wait4), (positive)child, (positive)address_of status, 0, 0);

        // The newlines at the end go, and only the ones at the end: that is the
        // single piece of editing a substitution is allowed.
        while (expand_length > start && expand_text[expand_length - 1] == '\n')
                expand_length--;

        if (child > 0)
                shell_substitution_status = (b32)(status >> 8 & 0xff);
}

static string_address expand_command(string_address step, bool quoted)
{
        string_address inner = step + 2;
        string_address stop = expand_paren_end(inner);
        p8 text[EXPAND_WORK / 2];
        positive length;

        // No closing paren inside this word: the lexer stopped short of it, and
        // a dollar with nothing it can begin is a dollar.
        if (!stop)
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        length = (positive)(stop - inner);

        if (length >= sizeof(text))
                length = sizeof(text) - 1;

        memory_copy(text, inner, length);
        text[length] = end;

        expand_run(text, quoted);

        return stop + 1;
}

static string_address expand_backtick(string_address step, bool quoted)
{
        p8 text[EXPAND_WORK / 2];
        positive length = 0;
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

        step++;

        while (string_get(step) && string_not(step, '`'))
        {
                // Inside backticks a backslash only hides the next byte when
                // that byte is one that backticks care about.
                if (string_is(step, '\\') &&
                    (string_get(step + 1) == '`' || string_get(step + 1) == '\\' ||
                     string_get(step + 1) == '$'))
                        step++;

                if (length + 1 < sizeof(text))
                        text[length++] = string_get(step);

                step++;
        }

        text[length] = end;

        if (string_get(step))
                step++;

        expand_run(text, quoted);

        return step;
}

static string_address expand_arithmetic(string_address step, bool quoted)
{
        string_address inner = step + 3;
        string_address stop = expand_paren_end(inner);
        p8 text[EXPAND_VALUE];
        p8 ready[EXPAND_VALUE];
        p8 written[32];
        positive length;

        if (!stop || string_get(stop + 1) != ')')
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        length = (positive)(stop - inner);

        if (length >= sizeof(text))
                length = sizeof(text) - 1;

        memory_copy(text, inner, length);
        text[length] = end;

        // What was written with a dollar in front takes its turn first; what is
        // left over is arithmetic, where a bare name is a value too.
        expand_capture(text, true, ready, sizeof(ready), false);

        expand_number_out(arith_evaluate(ready), written);
        expand_push_string(written, quoted ? MARK_QUOTED : MARK_FIELD);

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

/*
        ${ ... } in every form POSIX gives it.

        The name comes first, then the colon that decides whether empty counts
        as unset, then the operator, then the word -- and the word is a word, so
        it is expanded the same way as any other and can hold another of these.
*/
static string_address expand_braced(string_address step, bool quoted)
{
        p8 name[EXPAND_NAME];
        p8 word[EXPAND_VALUE];
        p8 value[EXPAND_VALUE];
        p8 mark = quoted ? MARK_QUOTED : MARK_FIELD;
        positive length = 0;
        bool want_length = false;
        bool colon = false;
        bool doubled = false;
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

        seen = string_get(step);

        if (seen == '@' || seen == '*' || seen == '#' || seen == '?' ||
            seen == '$' || seen == '!' || seen == '-')
        {
                name[0] = seen;
                name[1] = end;
                length = 1;
                step++;
        }
        else if (seen >= '0' && seen <= '9')
        {
                while (string_get(step) >= '0' && string_get(step) <= '9' &&
                       length < EXPAND_NAME - 1)
                        name[length++] = string_get(step++);

                name[length] = end;
        }
        else
        {
                while (expand_name_character(string_get(step)) && length < EXPAND_NAME - 1)
                        name[length++] = string_get(step++);

                name[length] = end;
        }

        seen = string_get(step);

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

        {
                positive room = (positive)(close - step);

                if (room >= sizeof(word))
                        room = sizeof(word) - 1;

                memory_copy(word, step, room);
                word[room] = end;
        }

        if (want_length)
        {
                p8 written[32];
                positive count;

                if (!expand_value_of(name, value, sizeof(value)))
                        count = 0;
                else
                        count = string_length(value);

                expand_number_out((bipolar)count, written);
                expand_push_string(written, mark);

                return close + 1;
        }

        if (operation == '#' || operation == '%')
        {
                p8 pattern[EXPAND_VALUE];
                positive start = expand_length;

                expand_push_parameter(name, quoted);
                expand_capture(word, quoted, pattern, sizeof(pattern), true);
                expand_trim(start, pattern, operation == '#', doubled);

                return close + 1;
        }

        {
                bool present = expand_value_of(name, value, sizeof(value));
                bool blank = present && value[0] == end;
                bool missing;

                // $@ and $* are unset when there are no parameters, not set to
                // the empty string they would join to.
                if (string_is(name, '@') || string_is(name, '*'))
                {
                        present = shell_parameter_count != 0;
                        blank = false;
                }

                missing = !present || (colon && blank);

                if (operation == '-')
                {
                        if (missing)
                                expand_into(word, quoted);
                        else
                                expand_push_parameter(name, quoted);

                        return close + 1;
                }

                if (operation == '=')
                {
                        if (missing)
                        {
                                p8 made[EXPAND_VALUE];

                                expand_capture(word, quoted, made, sizeof(made), false);
                                env_set(name, made);
                                expand_push_string(made, mark);
                        }
                        else
                                expand_push_parameter(name, quoted);

                        return close + 1;
                }

                if (operation == '+')
                {
                        if (!missing)
                                expand_into(word, quoted);

                        return close + 1;
                }

                if (operation == '?')
                {
                        if (missing)
                        {
                                p8 said[EXPAND_VALUE];

                                expand_capture(word, quoted, said, sizeof(said), false);
                                string_format(expand_complain, "%s: %s\n", name,
                                              said[0] ? said : (string_address) "parameter not set");
                                shell_status = 1;

                                return close + 1;
                        }

                        expand_push_parameter(name, quoted);

                        return close + 1;
                }

                expand_push_parameter(name, quoted);
        }

        return close + 1;
}

static string_address expand_simple(string_address step, bool quoted)
{
        p8 name[EXPAND_NAME];
        positive length = 0;
        p8 seen;

        step++;
        seen = string_get(step);

        if (seen == '?' || seen == '#' || seen == '$' || seen == '!' ||
            seen == '-' || seen == '@' || seen == '*' ||
            (seen >= '0' && seen <= '9'))
        {
                name[0] = seen;
                name[1] = end;
                expand_push_parameter(name, quoted);

                return step + 1;
        }

        while (expand_name_character(string_get(step)) && length < EXPAND_NAME - 1)
                name[length++] = string_get(step++);

        // A dollar in front of nothing that could be a name is a dollar.
        if (!length)
        {
                expand_push('$', MARK_PLAIN);
                return step;
        }

        name[length] = end;
        expand_push_parameter(name, quoted);

        return step;
}

static string_address expand_dollar(string_address step, bool quoted)
{
        p8 next = string_get(step + 1);
        string_address result;

        // Deep enough. Something is expanding itself and the stack is the only
        // thing that would notice.
        if (expand_depth >= EXPAND_DEPTH)
        {
                expand_push('$', MARK_PLAIN);
                return step + 1;
        }

        expand_depth++;

        if (next == '(' && string_get(step + 2) == '(')
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

static string_address expand_double(string_address step)
{
        expand_quoted_seen = true;
        step++;

        while (string_get(step) && string_not(step, '"'))
        {
                p8 seen = string_get(step);

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

static fn expand_into(string_address text, bool quoted)
{
        string_address step = text;

        while (string_get(step))
        {
                p8 seen = string_get(step);

                if (seen == '\\' && string_get(step + 1))
                {
                        step++;
                        expand_push(string_get(step++), MARK_QUOTED);
                        continue;
                }

                if (seen == '\'' && !quoted)
                {
                        expand_quoted_seen = true;
                        step++;

                        while (string_get(step) && string_not(step, '\''))
                                expand_push(string_get(step++), MARK_QUOTED);

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

                expand_push(seen, quoted ? MARK_QUOTED : MARK_PLAIN);
                step++;
        }
}

/*
        A tilde, and only at the very front of a word.

        What HOME says, and no splitting afterwards: a home directory with a
        space in it is still one directory.
*/
static string_address expand_tilde(string_address step)
{
        string_address home;

        // ~name wants a password file, and there is none on this machine.
        if (string_not(step + 1, end) && string_not(step + 1, '/'))
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

        expand_length = 0;
        expand_overflow = false;
        expand_quoted_seen = false;
        expand_depth = 0;

        if (string_is(word, '~'))
                step = expand_tilde(word);

        expand_into(step, false);
}

static string_address glob_result[GLOB_RESULTS];
static p8 glob_bytes[GLOB_BYTES];
static positive glob_used;
static positive glob_count;

static bool glob_magic(string_address pattern)
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

        if (glob_count >= GLOB_RESULTS || glob_used + length > GLOB_BYTES)
                return false;

        memory_copy(glob_bytes + glob_used, path, length);
        glob_result[glob_count++] = glob_bytes + glob_used;
        glob_used += length;

        return true;
}

static bool glob_exists(string_address path)
{
        p8 facts[256];

        return system_call_5(syscall(statx), AT_FDCWD, (positive)path,
                             0x100, 0, (positive)facts) == 0;
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
                return;

        // A run of slashes belongs to the prefix, not to any component.
        while (string_is(pattern, '/'))
        {
                if (used + 1 < GLOB_PATH)
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

        while (string_get(pattern) && string_not(pattern, '/') && length < GLOB_PATH - 2)
        {
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

                        if (out + 1 < GLOB_PATH)
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

                directory = system_call_3(syscall(openat), AT_FDCWD,
                                          (positive)(used ? prefix : (string_address) "."),
                                          FILE_READ | O_DIRECTORY);

                if (directory < 0)
                        return;

                while (1)
                {
                        bipolar got = system_call_3(syscall(getdents64), (positive)directory,
                                                    (positive)block, sizeof(block));
                        p8 address_to step = block;

                        if (got <= 0)
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

                                while (named[at] && out + 1 < GLOB_PATH)
                                        prefix[out++] = named[at++];

                                glob_walk(prefix, out, rest, depth + 1);
                        }
                }

                system_call_1(syscall(close), (positive)directory);
        }
}

// POSIX asks for the matches in order, and a script that reads a directory
// twice should be told the same story both times.
static fn glob_sort()
{
        positive at;

        for (at = 1; at < glob_count; at++)
        {
                string_address holding = glob_result[at];
                positive back = at;

                while (back && string_compare(glob_result[back - 1], holding) > 0)
                {
                        glob_result[back] = glob_result[back - 1];
                        back--;
                }

                glob_result[back] = holding;
        }
}

static string_address expand_keep_string(string_address text)
{
        positive room = string_length(text) + 1;
        string_address result;

        if (room > EXPAND_ARENA)
                return (string_address) "";

        // Nothing here outlives the line it came from, so a full arena starts
        // over rather than refusing the word.
        if (expand_arena_used + room > EXPAND_ARENA)
                expand_arena_used = 0;

        result = expand_arena + expand_arena_used;
        memory_copy(result, text, room);
        expand_arena_used += room;

        return result;
}

static string_address expand_keep(positive at, positive stop)
{
        positive room = stop - at + 1;
        string_address result;

        if (room > EXPAND_ARENA)
                return (string_address) "";

        if (expand_arena_used + room > EXPAND_ARENA)
                expand_arena_used = 0;

        result = expand_arena + expand_arena_used;
        memory_copy(result, expand_text + at, stop - at);
        result[stop - at] = end;
        expand_arena_used += room;

        return result;
}

/*
        One field, out of the working buffer and into the arena -- and through
        the filesystem on the way if anything unquoted in it was magic.

        This is the last place the marks exist: what leaves here is bytes.
*/
static positive expand_emit(positive at, positive stop, string_address address_to out,
                            positive limit, positive count)
{
        p8 pattern[GLOB_PATH];
        positive used = 0;
        positive index;
        bool magic = false;

        if (count >= limit)
                return count;

        for (index = at; index < stop; index++)
        {
                p8 value = expand_text[index];
                bool special = value == '*' || value == '?' || value == '[';

                if (expand_mark[index] == MARK_QUOTED)
                {
                        if ((special || value == '\\') && used + 2 < sizeof(pattern))
                                pattern[used++] = '\\';
                }
                else if (special)
                        magic = true;

                if (used + 1 < sizeof(pattern))
                        pattern[used++] = value;
        }

        pattern[used] = end;

        if (magic)
        {
                p8 built[GLOB_PATH];

                glob_count = 0;
                glob_used = 0;
                glob_walk(built, 0, pattern, 0);

                if (glob_count)
                {
                        glob_sort();

                        for (index = 0; index < glob_count && count < limit; index++)
                                out[count++] = expand_keep_string(glob_result[index]);

                        return count;
                }
        }

        out[count] = expand_keep(at, stop);

        return count + 1;
}

/*
        Field splitting.

        Only bytes that came out of an unquoted expansion can be a separator, so
        the marks do the whole of the deciding. A run of IFS whitespace is one
        separator; anything else in IFS is a separator on its own, with the
        whitespace around it swallowed.
*/
static positive expand_split(string_address address_to out, positive limit)
{
        positive count = 0;
        positive at = 0;
        positive start;

        expand_ifs_prepare();

        // A word that expanded to nothing at all is no field, unless some part
        // of it was quoted: "" is an empty argument and $nosuch is no argument.
        if (!expand_length)
                return expand_quoted_seen ? expand_emit(0, 0, out, limit, 0) : 0;

        while (at < expand_length && expand_mark[at] == MARK_FIELD &&
               expand_ifs_blank(expand_text[at]))
                at++;

        // Nothing but separators: a word of blanks out of a variable is no
        // word at all.
        if (at >= expand_length)
                return 0;

        start = at;

        while (at < expand_length)
        {
                if (expand_mark[at] == MARK_BREAK)
                {
                        count = expand_emit(start, at, out, limit, count);
                        at++;
                        start = at;
                        continue;
                }

                if (expand_mark[at] == MARK_FIELD && expand_in_ifs(expand_text[at]))
                {
                        count = expand_emit(start, at, out, limit, count);

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
                                return count;

                        continue;
                }

                at++;
        }

        return expand_emit(start, expand_length, out, limit, count);
}

/*
        One lexed word, expanded whole, and the fields it became written out.

        The answer is not one word. $@ makes as many as there are parameters, an
        unquoted variable with a space in it makes two, a glob makes as many as
        the directory holds, and an unset variable on its own makes none at all
        -- which is the difference between "rm $file" deleting one thing and
        deleting the working directory.
*/
positive shell_expand_fields(string_address word, string_address address_to out, positive limit)
{
        expand_word(word);

        // A word that did not fit is a word that came out shorter than it was
        // meant to be, and a shell that says nothing about that hands the wrong
        // file name to whatever runs next.
        if (expand_overflow)
                string_format(expand_complain, "Expansion too long: %s\n", word);

        return expand_split(out, limit);
}

/*
        The same word, kept whole.

        A redirection target and the right hand side of an assignment are the
        two places POSIX does not split and does not glob, and this is what they
        are supposed to call.
*/
string_address shell_expand_word(string_address word)
{
        expand_word(word);

        if (expand_overflow)
                string_format(expand_complain, "Expansion too long: %s\n", word);

        return expand_keep(0, expand_length);
}
