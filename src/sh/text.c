#include "../library.c"

/*
        The text utilities, as one body of code.

        grep, sed, cut, tr, sort, uniq, head, tail, wc, tee, rev, nl and fold
        are thirteen programs and one file, because eleven of them are the
        same program with a different inner loop: read lines from a list of
        files or from standard input, decide something about each one, write
        bytes out. Writing that shape thirteen times is how thirteen slightly
        different bugs get written down.

        Every buffer here is a fixed array or a slice of one arena taken once
        from the kernel. Nothing grows, and the one place a bound is reached
        -- a line longer than TEXT_LINE_MAX -- truncates rather than walking
        off the end.

        The reference is the GNU tool on the machine, not the standard: the
        widths wc pads to, the "==> name <==" head prints and the six columns
        nl numbers in are all formatting nobody writes down twice the same
        way, so they are copied from the bytes the real tool emits.
*/

#define TEXT_OUT_MAX 65536
#define TEXT_READ_MAX 65536
#define TEXT_LINE_MAX (1 << 20)
#define TEXT_PATH_MAX 4096
#define TEXT_UNSET ((positive)-1)

static p8 text_out_buffer[TEXT_OUT_MAX];
static positive text_out_used;
static positive text_out_handle = 1;
static string_address text_name = "text";
static b32 text_status;

static fn text_write_raw(positive handle, address_any data, positive length)
{
        p8 address_to at = (p8 address_to)data;
        positive left = length;

        while (left)
        {
                bipolar wrote = system_call_3(syscall(write), handle, (positive)at, left);

                // A write that cannot make progress would spin here forever.
                if (wrote <= 0)
                        return;

                at += wrote;
                left -= (positive)wrote;
        }
}

static fn text_flush()
{
        if (!text_out_used)
                return;

        text_write_raw(text_out_handle, text_out_buffer, text_out_used);
        text_out_used = 0;
}

// Changing where output goes has to empty what was written for the old one.
static fn text_out_to(positive handle)
{
        if (handle == text_out_handle)
                return;

        text_flush();
        text_out_handle = handle;
}

static fn text_put(address_any data, positive length)
{
        if (length > TEXT_OUT_MAX)
        {
                text_flush();
                text_write_raw(text_out_handle, data, length);
                return;
        }

        if (text_out_used + length > TEXT_OUT_MAX)
                text_flush();

        memory_copy(text_out_buffer + text_out_used, data, length);
        text_out_used += length;
}

static fn text_put_character(p8 character)
{
        if (text_out_used == TEXT_OUT_MAX)
                text_flush();

        text_out_buffer[text_out_used++] = character;
}

static fn text_put_string(string_address value)
{
        text_put(value, string_length(value));
}

// Right aligned in width columns, which is what every count wc prints wants.
static fn text_put_number(positive value, positive width)
{
        p8 digits[24];
        positive have = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (width > have)
        {
                text_put_character(' ');
                width--;
        }

        while (have)
                text_put_character(digits[--have]);
}

static fn text_error_raw(string_address text)
{
        text_write_raw(2, text, string_length(text));
}

// "grep: nosuch.txt: No such file or directory", the shape every one of them
// uses, with the flush first so the complaint cannot land inside a line.
static fn text_error(string_address about, string_address reason)
{
        text_flush();
        text_error_raw(text_name);
        text_error_raw(": ");

        if (about)
        {
                text_error_raw(about);
                text_error_raw(": ");
        }

        text_error_raw(reason);
        text_error_raw("\n");
}

static b32 text_done(b32 code)
{
        text_flush();
        return code;
}

/*
        One arena, taken from the kernel the first time anything asks.

        sort holds every line at once and tail holds the last n of them, so
        those two need memory that is not a fixed array. Everything else here
        never touches it.
*/
#define TEXT_ARENA_BYTES (192u << 20)

static p8 address_to text_arena;
static positive text_arena_used;

static address_any text_arena_take(positive bytes)
{
        bytes = (bytes + 15) & ~(positive)15;

        if (!text_arena)
        {
                positive got = (positive)memory(TEXT_ARENA_BYTES);

                // mmap answers a failure as a small negative, not as null.
                if (!got || got >= (positive)-4095)
                {
                        text_error(null, "out of memory");
                        return null;
                }

                text_arena = (p8 address_to)got;
                text_arena_used = 0;
        }

        if (text_arena_used + bytes > TEXT_ARENA_BYTES)
        {
                text_error(null, "input too large");
                return null;
        }

        address_any at = text_arena + text_arena_used;
        text_arena_used += bytes;
        return at;
}

/*
        Reading.

        file_read takes an offset, and an offset on a pipe is not a position
        -- standard input under a pipeline is exactly that -- so the read
        syscall is made here directly and the descriptor comes from file_new
        only as a number.
*/
/*
        openat by hand, because file_new does not pass a mode.

        library.c's file_new puts the path and the flags where openat wants
        them and leaves the fourth register alone, so a file created through
        it gets whatever happened to be in r10 for its permissions -- measured
        here as ---------x on a file tee had just made. Every open below goes
        through this instead, with the mode spelled out.
*/
// FILE_WRITE already carries O_TRUNC, so a writer wants nothing added to it.
#define TEXT_WRITE (FILE_WRITE)
#define TEXT_APPEND (FILE_APPEND)

static bipolar text_open_handle(string_address path, positive flags, positive mode)
{
        return system_call_4(syscall(openat), (positive)(bipolar)AT_FDCWD,
                             (positive)path, flags, mode);
}

typedef struct
{
        positive handle;
        positive filled;
        positive position;
        bool finished;
        bool opened;
        p8 buffer[TEXT_READ_MAX];
} text_reader;

static text_reader text_input;
static p8 text_line[TEXT_LINE_MAX];
static positive text_line_length;
static bool text_line_ended;

static bool text_open(string_address path)
{
        text_input.filled = 0;
        text_input.position = 0;
        text_input.finished = false;
        text_input.opened = false;

        if (!path || (path[0] == '-' && path[1] == '\0'))
        {
                text_input.handle = 0;
                return true;
        }

        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
        {
                text_error(path, "No such file or directory");
                text_status = text_status ? text_status : 1;
                return false;
        }

        text_input.handle = (positive)handle;
        text_input.opened = true;
        return true;
}

static fn text_close()
{
        if (text_input.opened)
                system_call_1(syscall(close), text_input.handle);

        text_input.opened = false;
}

static bool text_fill()
{
        if (text_input.position < text_input.filled)
                return true;

        if (text_input.finished)
                return false;

        bipolar got = system_call_3(syscall(read), text_input.handle,
                                    (positive)text_input.buffer, TEXT_READ_MAX);

        if (got <= 0)
        {
                text_input.finished = true;
                return false;
        }

        text_input.filled = (positive)got;
        text_input.position = 0;
        return true;
}

// A line without its newline, and whether it had one. A file whose last line
// is unterminated is the reason the second answer exists: every tool here has
// to put back exactly what it was given.
static bool text_line_next()
{
        text_line_length = 0;
        text_line_ended = false;

        if (!text_fill())
                return false;

        for (;;)
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;
                string_address found = null;
                positive take = left;

                for (positive i = 0; i < left; i++)
                        if (at[i] == '\n')
                        {
                                found = at + i;
                                take = i;
                                break;
                        }

                positive room = TEXT_LINE_MAX - text_line_length;
                positive copy = take < room ? take : room;

                memory_copy(text_line + text_line_length, at, copy);
                text_line_length += copy;
                text_input.position += take;

                if (found)
                {
                        text_input.position++;
                        text_line_ended = true;
                        return true;
                }

                if (!text_fill())
                        return text_line_length != 0;
        }
}

static fn text_put_line()
{
        text_put(text_line, text_line_length);

        if (text_line_ended)
                text_put_character('\n');
}

static p8 text_lower(p8 character)
{
        return character >= 'A' && character <= 'Z' ? (p8)(character + 32) : character;
}

static bool text_digit(p8 character)
{
        return character >= '0' && character <= '9';
}

static bool text_blank(p8 character)
{
        return character == ' ' || character == '\t';
}

static bool text_space(p8 character)
{
        return character == ' ' || character == '\t' || character == '\n' ||
               character == '\v' || character == '\f' || character == '\r';
}

static bool text_word(p8 character)
{
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               text_digit(character) || character == '_';
}

// Returns false on anything that is not entirely digits, which is how every
// flag that takes a count tells a number from a filename.
static bool text_number_of(string_address value, positive address_to result)
{
        positive total = 0;
        positive seen = 0;

        if (!value)
                return false;

        for (positive i = 0; value[i]; i++)
        {
                if (!text_digit(value[i]))
                        return false;

                total = total * 10 + (positive)(value[i] - '0');
                seen++;
        }

        if (!seen)
                return false;

        address_to result = total;
        return true;
}

/*
        Arguments.

        program_argument counts the program's own name as the first, so
        everything below starts at one and text_argument_count is what stops
        the walk.
*/
static b32 text_argument_count;

static string_address text_argument(b32 which)
{
        return program_argument(which);
}

static fn text_begin(string_address name)
{
        text_name = name;
        text_argument_count = program_argument_count();
}

/*
        Regular expressions.

        A backtracking machine over a compiled program, which is what makes
        one engine answer both grep and sed: basic and extended syntax differ
        only in which characters need a backslash to mean something, so the
        parser takes a flag and the machine below never learns there were two
        dialects.

        The one thing worth saying about the shape: a quantified single
        character -- a*, [0-9]\{2,4\}, .* -- compiles to REPEAT rather than to
        a split and a jump. A split recurses once per repetition, so .* over a
        four thousand character line is four thousand stack frames; REPEAT
        consumes greedily in a loop and gives back one at a time, and the
        stack stays one deep. Groups still split, because a group can capture
        and a loop cannot say what it captured.

        What this is not: leftmost longest. POSIX says an alternation picks
        the longest branch that lets the whole match succeed; this picks the
        first branch that does, so a\|ab against "ab" matches "a" here and
        "ab" under GNU grep. Every other difference from the reference tools
        is a bug; that one is a choice.
*/

#define REGEX_CODE_MAX 4096
#define REGEX_SET_MAX 64
#define REGEX_GROUP_MAX 9
#define REGEX_SLOT_MAX ((REGEX_GROUP_MAX + 1) * 2)
#define REGEX_JUMPS_MAX 32
#define REGEX_REPEAT_MAX 255
#define REGEX_COUNT_MAX 32767

enum
{
        REGEX_DONE = 0,
        REGEX_CHAR,
        REGEX_ANY,
        REGEX_SET,
        REGEX_SPLIT,
        REGEX_JUMP,
        REGEX_SAVE,
        REGEX_BOL,
        REGEX_EOL,
        REGEX_BACK,
        REGEX_REPEAT,
        REGEX_EDGE
};

// What REGEX_EDGE is asking about at the position it stands on.
enum
{
        REGEX_EDGE_WORD = 0,
        REGEX_EDGE_NOT_WORD,
        REGEX_EDGE_START,
        REGEX_EDGE_STOP
};

typedef struct
{
        p8 code;
        p8 value;
        p8 kind;
        bool loop;
        b32 x;
        b32 y;
        b32 set;
        b32 low;
        b32 high;
} regex_instruction;

/*
        One pool for every pattern a program compiles.

        sed carries several regular expressions at once -- an address, another
        address, the one a substitution matches -- and the machine below reads
        exactly one. Rather than copying a program in before each line, which
        is a hundred kilobytes per line, the live program is two pointers into
        a pool and selecting one is two stores.
*/
static regex_instruction regex_store[REGEX_CODE_MAX];
static p8 regex_set_store[REGEX_SET_MAX][32];
static b32 regex_pool_used;
static b32 regex_pool_sets;

static regex_instruction address_to regex_code = regex_store;
static p8(address_to regex_sets)[32] = regex_set_store;
static b32 regex_length_code;
static b32 regex_set_count;
static b32 regex_group_count;
static bool regex_extended;
static bool regex_icase;
static bool regex_escapes;
static bool regex_broken;

static string_address regex_pattern;
static positive regex_pattern_length;
static positive regex_pattern_at;

static string_address regex_text;
static positive regex_text_length;
static positive regex_slots[REGEX_SLOT_MAX];
static positive regex_loop_at[REGEX_CODE_MAX];

/*
        What a match can start with.

        Without this the machine is asked the whole question at every byte of
        the input, which on thirteen megabytes is thirteen million calls that
        nearly all fail on their first instruction. The table below is every
        character that could begin a match, computed once from the compiled
        program, and the search skips to the next byte that is in it -- a
        table lookup per byte instead of an interpreter.

        It is not always knowable: a pattern that can match nothing at all, or
        one that starts with a backreference, has no such set, and then the
        search does what it did before.
*/
#define REGEX_FIRST_MAX 40
#define REGEX_LOOPS_KEPT 8

static p8 regex_first_store[REGEX_FIRST_MAX][256];
static p8 regex_last_store[REGEX_FIRST_MAX][256];
static b32 regex_first_used;
static p8 address_to regex_first = regex_first_store[0];
static p8 address_to regex_last = regex_last_store[0];
static bool regex_first_known;
static bool regex_last_known;
static bool regex_anchored;
static bool regex_alternates;
static positive regex_stop_wanted = TEXT_UNSET;
static positive regex_slot_used = REGEX_SLOT_MAX;
static b32 regex_loop_list[REGEX_LOOPS_KEPT];
static b32 regex_loop_count;
static p8 regex_visited[REGEX_CODE_MAX];

static bool regex_set_has(b32 which, p8 character)
{
        return (regex_sets[which][character >> 3] >> (character & 7)) & 1;
}

static fn regex_set_add(b32 which, p8 character)
{
        regex_sets[which][character >> 3] |= (p8)(1u << (character & 7));
}

static b32 regex_emit(p8 code)
{
        if (regex_length_code >= REGEX_CODE_MAX - 2)
        {
                regex_broken = true;
                return regex_length_code ? regex_length_code - 1 : 0;
        }

        b32 at = regex_length_code++;

        regex_code[at].code = code;
        regex_code[at].value = 0;
        regex_code[at].kind = 0;
        regex_code[at].loop = false;
        regex_code[at].x = 0;
        regex_code[at].y = 0;
        regex_code[at].set = -1;
        regex_code[at].low = 0;
        regex_code[at].high = -1;
        return at;
}

// Makes room for count instructions at "where", moving everything after it up
// and carrying every jump that pointed past the hole along with it.
static fn regex_insert(b32 where, b32 count)
{
        if (regex_length_code + count >= REGEX_CODE_MAX - 2)
        {
                regex_broken = true;
                return;
        }

        for (b32 i = regex_length_code - 1; i >= where; i--)
                regex_code[i + count] = regex_code[i];

        regex_length_code += count;

        for (b32 i = 0; i < regex_length_code; i++)
        {
                if (i >= where && i < where + count)
                        continue;

                //
                //      A target that is exactly the hole means one of two
                //      things, and which one depends on where it is read
                //      from. Inside the block that just moved it is a loop's
                //      jump back to its own head, and it has to follow the
                //      head up. From in front of the block it means "carry on
                //      into what follows", and what follows is now the
                //      instruction being inserted, so it must not move.
                //
                //      Shifting both was one bug: \(a\)*\(a\)* had the
                //      first star's exit pointing one past the second star's
                //      split, so a line matching neither could not fall out
                //      of the first one.
                //
                b32 lowest = i < where ? where + 1 : where;

                if (regex_code[i].x >= lowest)
                        regex_code[i].x += count;

                if (regex_code[i].y >= lowest)
                        regex_code[i].y += count;
        }

        for (b32 i = 0; i < count; i++)
        {
                regex_code[where + i].code = REGEX_JUMP;
                regex_code[where + i].value = 0;
                regex_code[where + i].kind = 0;
                regex_code[where + i].loop = false;
                regex_code[where + i].x = where + i + 1;
                regex_code[where + i].y = 0;
                regex_code[where + i].set = -1;
                regex_code[where + i].low = 0;
                regex_code[where + i].high = -1;
        }
}

// Appends another copy of [from, to), which is how a group repeated a counted
// number of times is spelled without a counter in the machine.
static fn regex_copy_block(b32 from, b32 to)
{
        b32 span = to - from;
        b32 shift = regex_length_code - from;

        if (regex_length_code + span >= REGEX_CODE_MAX - 2)
        {
                regex_broken = true;
                return;
        }

        for (b32 i = 0; i < span; i++)
        {
                regex_instruction copy = regex_code[from + i];

                if (copy.x >= from && copy.x <= to)
                        copy.x += shift;

                if (copy.y >= from && copy.y <= to)
                        copy.y += shift;

                regex_code[regex_length_code + i] = copy;
        }

        regex_length_code += span;
}

static p8 regex_peek()
{
        return regex_pattern_at < regex_pattern_length ? regex_pattern[regex_pattern_at] : 0;
}

static p8 regex_peek_at(positive ahead)
{
        return regex_pattern_at + ahead < regex_pattern_length
                   ? regex_pattern[regex_pattern_at + ahead]
                   : 0;
}

static bool regex_more()
{
        return regex_pattern_at < regex_pattern_length;
}

static b32 regex_parse_alternation();

static b32 regex_new_set()
{
        if (regex_pool_sets + regex_set_count >= REGEX_SET_MAX)
        {
                regex_broken = true;
                return 0;
        }

        b32 which = regex_set_count++;

        memory_fill(regex_sets[which], 0, 32);
        return which;
}

static fn regex_set_add_folded(b32 which, p8 character)
{
        regex_set_add(which, character);

        if (!regex_icase)
                return;

        if (character >= 'a' && character <= 'z')
                regex_set_add(which, (p8)(character - 32));

        if (character >= 'A' && character <= 'Z')
                regex_set_add(which, (p8)(character + 32));
}

static bool regex_named_class(b32 which, string_address name)
{
        for (b32 c = 0; c < 256; c++)
        {
                p8 character = (p8)c;
                bool wanted = false;

                if (string_compare(name, "alpha") == 0)
                        wanted = (character >= 'a' && character <= 'z') ||
                                 (character >= 'A' && character <= 'Z');
                else if (string_compare(name, "digit") == 0)
                        wanted = text_digit(character);
                else if (string_compare(name, "alnum") == 0)
                        wanted = text_digit(character) ||
                                 (character >= 'a' && character <= 'z') ||
                                 (character >= 'A' && character <= 'Z');
                else if (string_compare(name, "upper") == 0)
                        wanted = character >= 'A' && character <= 'Z';
                else if (string_compare(name, "lower") == 0)
                        wanted = character >= 'a' && character <= 'z';
                else if (string_compare(name, "space") == 0)
                        wanted = text_space(character);
                else if (string_compare(name, "blank") == 0)
                        wanted = text_blank(character);
                else if (string_compare(name, "print") == 0)
                        wanted = character >= 32 && character < 127;
                else if (string_compare(name, "graph") == 0)
                        wanted = character > 32 && character < 127;
                else if (string_compare(name, "cntrl") == 0)
                        wanted = character < 32 || character == 127;
                else if (string_compare(name, "punct") == 0)
                        wanted = character > 32 && character < 127 &&
                                 !text_digit(character) &&
                                 !(character >= 'a' && character <= 'z') &&
                                 !(character >= 'A' && character <= 'Z');
                else if (string_compare(name, "xdigit") == 0)
                        wanted = text_digit(character) ||
                                 (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F');
                else
                        return false;

                if (wanted)
                        regex_set_add_folded(which, character);
        }

        return true;
}

// [abc], [^a-z], [[:digit:]] -- and the two rules everybody forgets: a ] that
// comes first is a literal, and so is a - that comes first or last.
static b32 regex_parse_set()
{
        b32 which = regex_new_set();
        bool negate = false;
        bool first = true;

        if (regex_peek() == '^')
        {
                negate = true;
                regex_pattern_at++;
        }

        while (regex_more())
        {
                p8 character = regex_peek();

                if (character == ']' && !first)
                {
                        regex_pattern_at++;

                        if (negate)
                                for (b32 i = 0; i < 32; i++)
                                        regex_sets[which][i] = (p8)~regex_sets[which][i];

                        return which;
                }

                first = false;

                if (character == '[' && regex_peek_at(1) == ':')
                {
                        p8 name[16];
                        positive have = 0;
                        positive at = regex_pattern_at + 2;

                        while (at < regex_pattern_length && regex_pattern[at] != ':' && have < 15)
                                name[have++] = regex_pattern[at++];

                        name[have] = '\0';

                        if (at + 1 < regex_pattern_length && regex_pattern[at] == ':' &&
                            regex_pattern[at + 1] == ']' && regex_named_class(which, name))
                        {
                                regex_pattern_at = at + 2;
                                continue;
                        }
                }

                // POSIX says a backslash in a bracket expression is a
                // backslash. sed's own regular expressions say otherwise, and
                // [ \t] meaning blanks is written that way everywhere.
                if (regex_escapes && character == '\\' && regex_peek_at(1))
                {
                        p8 next = regex_peek_at(1);

                        character = next == 'n'    ? '\n'
                                    : next == 't'  ? '\t'
                                    : next == 'r'  ? '\r'
                                                   : next;
                        regex_pattern_at++;
                }

                regex_pattern_at++;

                if (regex_peek() == '-' && regex_peek_at(1) && regex_peek_at(1) != ']')
                {
                        p8 last = regex_peek_at(1);

                        regex_pattern_at += 2;

                        for (b32 c = character; c <= (b32)last; c++)
                                regex_set_add_folded(which, (p8)c);

                        continue;
                }

                regex_set_add_folded(which, character);
        }

        regex_broken = true;
        return which;
}

static fn regex_emit_class_escape(p8 which)
{
        b32 set = regex_new_set();
        bool negate = which == 'W' || which == 'S';
        bool space = which == 's' || which == 'S';

        for (b32 c = 0; c < 256; c++)
        {
                bool wanted = space ? text_space((p8)c) : text_word((p8)c);

                if (wanted != negate)
                        regex_set_add(set, (p8)c);
        }

        b32 at = regex_emit(REGEX_SET);

        regex_code[at].set = set;
}

static fn regex_emit_literal(p8 character)
{
        b32 at = regex_emit(REGEX_CHAR);

        regex_code[at].value = regex_icase ? text_lower(character) : character;
}

// True where a * or a ^ has nothing to its left, which in basic syntax is
// what makes it an ordinary character rather than an operator.
static bool regex_at_branch_start(positive at)
{
        if (at == 0)
                return true;

        if (regex_extended)
                return regex_pattern[at - 1] == '(' || regex_pattern[at - 1] == '|';

        if (at >= 2 && regex_pattern[at - 2] == '\\' &&
            (regex_pattern[at - 1] == '(' || regex_pattern[at - 1] == '|'))
                return true;

        return false;
}

static bool regex_at_branch_stop(positive at)
{
        if (at + 1 >= regex_pattern_length)
                return true;

        if (regex_extended)
                return regex_pattern[at + 1] == ')' || regex_pattern[at + 1] == '|';

        return regex_pattern[at + 1] == '\\' && at + 2 < regex_pattern_length &&
               (regex_pattern[at + 2] == ')' || regex_pattern[at + 2] == '|');
}

static b32 regex_parse_atom()
{
        b32 start = regex_length_code;
        p8 character = regex_peek();

        if (character == '.')
        {
                regex_pattern_at++;
                regex_emit(REGEX_ANY);
                return start;
        }

        if (character == '[')
        {
                regex_pattern_at++;

                b32 set = regex_parse_set();
                b32 at = regex_emit(REGEX_SET);

                regex_code[at].set = set;
                return start;
        }

        if (character == '^' && (regex_extended || regex_at_branch_start(regex_pattern_at)))
        {
                regex_pattern_at++;
                regex_emit(REGEX_BOL);
                return start;
        }

        if (character == '$' && (regex_extended || regex_at_branch_stop(regex_pattern_at)))
        {
                regex_pattern_at++;
                regex_emit(REGEX_EOL);
                return start;
        }

        if (regex_extended && character == '(')
        {
                regex_pattern_at++;

                b32 group = regex_group_count < REGEX_GROUP_MAX ? ++regex_group_count : 0;
                b32 open = regex_emit(REGEX_SAVE);

                regex_code[open].value = (p8)(group * 2);
                regex_parse_alternation();

                if (regex_peek() == ')')
                        regex_pattern_at++;
                else
                        regex_broken = true;

                b32 shut = regex_emit(REGEX_SAVE);

                regex_code[shut].value = (p8)(group * 2 + 1);
                return start;
        }

        if (character == '\\')
        {
                p8 next = regex_peek_at(1);

                if (!next)
                {
                        regex_pattern_at++;
                        regex_emit_literal('\\');
                        return start;
                }

                if (!regex_extended && next == '(')
                {
                        regex_pattern_at += 2;

                        b32 group = regex_group_count < REGEX_GROUP_MAX ? ++regex_group_count : 0;
                        b32 open = regex_emit(REGEX_SAVE);

                        regex_code[open].value = (p8)(group * 2);
                        regex_parse_alternation();

                        if (regex_peek() == '\\' && regex_peek_at(1) == ')')
                                regex_pattern_at += 2;
                        else
                                regex_broken = true;

                        b32 shut = regex_emit(REGEX_SAVE);

                        regex_code[shut].value = (p8)(group * 2 + 1);
                        return start;
                }

                regex_pattern_at += 2;

                if (next >= '1' && next <= '9')
                {
                        b32 at = regex_emit(REGEX_BACK);

                        regex_code[at].value = (p8)(next - '0');
                        return start;
                }

                if (next == 'w' || next == 'W' || next == 's' || next == 'S')
                {
                        regex_emit_class_escape(next);
                        return start;
                }

                if (next == 'b' || next == 'B' || next == '<' || next == '>')
                {
                        b32 at = regex_emit(REGEX_EDGE);

                        regex_code[at].value = next == 'b'   ? REGEX_EDGE_WORD
                                               : next == 'B' ? REGEX_EDGE_NOT_WORD
                                               : next == '<' ? REGEX_EDGE_START
                                                             : REGEX_EDGE_STOP;
                        return start;
                }

                if (regex_escapes && next == 'n')
                {
                        regex_emit_literal('\n');
                        return start;
                }

                if (regex_escapes && next == 't')
                {
                        regex_emit_literal('\t');
                        return start;
                }

                regex_emit_literal(next);
                return start;
        }

        regex_pattern_at++;
        regex_emit_literal(character);
        return start;
}

// The atom that was just compiled, if it is one instruction that eats one
// character. Those become REPEAT; everything else has to loop through a split.
static bool regex_atom_is_simple(b32 start)
{
        if (regex_length_code != start + 1)
                return false;

        p8 code = regex_code[start].code;

        return code == REGEX_CHAR || code == REGEX_ANY || code == REGEX_SET;
}

static fn regex_make_repeat(b32 start, b32 low, b32 high)
{
        regex_instruction atom = regex_code[start];

        regex_code[start].code = REGEX_REPEAT;
        regex_code[start].kind = atom.code;
        regex_code[start].value = atom.value;
        regex_code[start].set = atom.set;
        regex_code[start].low = low;
        regex_code[start].high = high;
        regex_code[start].x = start + 1;
}

// A counted repetition of something that captures: n mandatory copies, then
// either a loop or m - n optional ones, each entered through a split that
// jumps past all of them.
static fn regex_repeat_block(b32 start, b32 low, b32 high)
{
        b32 stop = regex_length_code;
        b32 splits[REGEX_REPEAT_MAX];
        b32 split_count = 0;

        if (low > REGEX_REPEAT_MAX || high > REGEX_REPEAT_MAX)
        {
                regex_broken = true;
                return;
        }

        if (low == 0 && high == 0)
        {
                regex_length_code = start;
                return;
        }

        for (b32 i = 1; i < low; i++)
                regex_copy_block(start, stop);

        if (low == 0)
        {
                // Nothing is mandatory, so the one copy already there becomes
                // the loop body and the split in front of it can skip it.
                regex_insert(start, 1);

                b32 body = start + 1;
                b32 shut = regex_length_code;

                regex_code[start].code = REGEX_SPLIT;
                regex_code[start].loop = true;
                regex_code[start].x = body;

                if (high < 0)
                {
                        b32 back = regex_emit(REGEX_JUMP);

                        regex_code[back].x = start;
                        regex_code[start].y = regex_length_code;
                        return;
                }

                regex_code[start].y = 0;
                splits[split_count++] = start;

                for (b32 i = 1; i < high && split_count < REGEX_REPEAT_MAX; i++)
                {
                        b32 split = regex_emit(REGEX_SPLIT);

                        regex_code[split].x = split + 1;
                        splits[split_count++] = split;
                        regex_copy_block(body, shut);
                }

                for (b32 i = 0; i < split_count; i++)
                        regex_code[splits[i]].y = regex_length_code;

                return;
        }

        if (high < 0)
        {
                b32 split = regex_emit(REGEX_SPLIT);
                b32 body = regex_length_code;

                regex_copy_block(start, stop);

                b32 back = regex_emit(REGEX_JUMP);

                regex_code[back].x = split;
                regex_code[split].loop = true;
                regex_code[split].x = body;
                regex_code[split].y = regex_length_code;
                return;
        }

        for (b32 i = low; i < high && split_count < REGEX_REPEAT_MAX; i++)
        {
                b32 split = regex_emit(REGEX_SPLIT);

                regex_code[split].x = split + 1;
                splits[split_count++] = split;
                regex_copy_block(start, stop);
        }

        for (b32 i = 0; i < split_count; i++)
                regex_code[splits[i]].y = regex_length_code;
}

// {n}, {n,}, {n,m} -- and in basic syntax the braces themselves are escaped.
static bool regex_parse_interval(b32 address_to low, b32 address_to high)
{
        positive at = regex_pattern_at + (regex_extended ? 1 : 2);
        b32 first = 0;
        b32 second = -1;
        bool have_first = false;

        while (at < regex_pattern_length && text_digit(regex_pattern[at]))
        {
                first = first * 10 + (regex_pattern[at] - '0');
                have_first = true;
                at++;
        }

        if (!have_first)
                return false;

        if (at < regex_pattern_length && regex_pattern[at] == ',')
        {
                at++;

                if (at < regex_pattern_length && text_digit(regex_pattern[at]))
                {
                        second = 0;

                        while (at < regex_pattern_length && text_digit(regex_pattern[at]))
                        {
                                second = second * 10 + (regex_pattern[at] - '0');
                                at++;
                        }
                }
        }
        else
        {
                second = first;
        }

        if (regex_extended)
        {
                if (at >= regex_pattern_length || regex_pattern[at] != '}')
                        return false;

                at++;
        }
        else
        {
                if (at + 1 >= regex_pattern_length || regex_pattern[at] != '\\' ||
                    regex_pattern[at + 1] != '}')
                        return false;

                at += 2;
        }

        // A repeated single character is a REPEAT instruction and can count
        // as high as it likes; a repeated group is copies of a block, and
        // that is what REGEX_REPEAT_MAX bounds, in regex_repeat_block.
        if (first > REGEX_COUNT_MAX || second > REGEX_COUNT_MAX)
                return false;

        address_to low = first;
        address_to high = second;
        regex_pattern_at = at;
        return true;
}

static fn regex_parse_piece()
{
        b32 start = regex_parse_atom();

        for (;;)
        {
                p8 character = regex_peek();
                b32 low = 0;
                b32 high = -1;
                bool counted = false;

                if (character == '*')
                {
                        regex_pattern_at++;
                        counted = true;
                }
                else if (regex_extended && character == '+')
                {
                        regex_pattern_at++;
                        low = 1;
                        counted = true;
                }
                else if (regex_extended && character == '?')
                {
                        regex_pattern_at++;
                        high = 1;
                        counted = true;
                }
                else if (!regex_extended && character == '\\' &&
                         (regex_peek_at(1) == '+' || regex_peek_at(1) == '?'))
                {
                        if (regex_peek_at(1) == '+')
                                low = 1;
                        else
                                high = 1;

                        regex_pattern_at += 2;
                        counted = true;
                }
                else if ((regex_extended && character == '{') ||
                         (!regex_extended && character == '\\' && regex_peek_at(1) == '{'))
                {
                        if (!regex_parse_interval(address_of low, address_of high))
                                return;

                        counted = true;
                }

                if (!counted)
                        return;

                if (regex_atom_is_simple(start))
                {
                        regex_make_repeat(start, low, high);
                        continue;
                }

                regex_repeat_block(start, low, high);
        }
}

static bool regex_branch_over()
{
        if (!regex_more())
                return true;

        p8 character = regex_peek();

        if (regex_extended)
                return character == '|' || character == ')';

        return character == '\\' && (regex_peek_at(1) == '|' || regex_peek_at(1) == ')');
}

static fn regex_parse_branch()
{
        while (!regex_branch_over() && !regex_broken)
                regex_parse_piece();
}

static bool regex_at_alternation()
{
        if (regex_extended)
                return regex_peek() == '|';

        return regex_peek() == '\\' && regex_peek_at(1) == '|';
}

static b32 regex_parse_alternation()
{
        b32 start = regex_length_code;
        b32 jumps[REGEX_JUMPS_MAX];
        b32 jump_count = 0;

        regex_parse_branch();

        while (regex_at_alternation() && !regex_broken)
        {
                regex_pattern_at += regex_extended ? 1 : 2;

                regex_insert(start, 1);

                for (b32 i = 0; i < jump_count; i++)
                        jumps[i]++;

                regex_code[start].code = REGEX_SPLIT;
                regex_code[start].x = start + 1;

                b32 skip = regex_emit(REGEX_JUMP);

                regex_code[start].y = skip + 1;

                if (jump_count < REGEX_JUMPS_MAX)
                        jumps[jump_count++] = skip;

                regex_parse_branch();
        }

        for (b32 i = 0; i < jump_count; i++)
                regex_code[jumps[i]].x = regex_length_code;

        return start;
}

typedef struct
{
        regex_instruction address_to code;
        p8(address_to sets)[32];
        p8 address_to first;
        p8 address_to last;
        b32 length;
        b32 groups;
        bool extended;
        bool icase;
        bool first_known;
        bool last_known;
        bool anchored;
        bool alternates;
        positive slot_used;
        b32 loops[REGEX_LOOPS_KEPT];
        b32 loop_count;
} regex_program;

static fn regex_select(regex_program address_to which)
{
        regex_code = which->code;
        regex_sets = which->sets;
        regex_first = which->first;
        regex_last = which->last;
        regex_length_code = which->length;
        regex_group_count = which->groups;
        regex_extended = which->extended;
        regex_icase = which->icase;
        regex_first_known = which->first_known;
        regex_last_known = which->last_known;
        regex_anchored = which->anchored;
        regex_alternates = which->alternates;
        regex_slot_used = which->slot_used;
        regex_loop_count = which->loop_count;

        for (b32 i = 0; i < which->loop_count && i < REGEX_LOOPS_KEPT; i++)
                regex_loop_list[i] = which->loops[i];
}

// Takes what was just compiled out of the pool's free space and hands back a
// handle to it, so the next compile starts after it rather than over it.
static fn regex_keep(regex_program address_to which)
{
        which->code = regex_code;
        which->sets = regex_sets;
        which->first = regex_first;
        which->last = regex_last;
        which->length = regex_length_code;
        which->groups = regex_group_count;
        which->extended = regex_extended;
        which->icase = regex_icase;
        which->first_known = regex_first_known;
        which->last_known = regex_last_known;
        which->anchored = regex_anchored;
        which->alternates = regex_alternates;
        which->slot_used = regex_slot_used;
        which->loop_count = regex_loop_count;

        for (b32 i = 0; i < regex_loop_count && i < REGEX_LOOPS_KEPT; i++)
                which->loops[i] = regex_loop_list[i];

        regex_pool_used += regex_length_code;
        regex_pool_sets += regex_set_count;
}

static fn regex_first_add(p8 character)
{
        regex_first[character] = 1;

        if (!regex_icase)
                return;

        if (character >= 'a' && character <= 'z')
                regex_first[character - 32] = 1;

        if (character >= 'A' && character <= 'Z')
                regex_first[character + 32] = 1;
}

// True where a match could begin without eating a character, which is what
// makes the table useless and says so.
static bool regex_first_walk(b32 pc)
{
        if (regex_visited[pc])
                return false;

        regex_visited[pc] = 1;

        regex_instruction address_to inst = regex_code + pc;

        switch (inst->code)
        {
        case REGEX_SAVE:
                return regex_first_walk(pc + 1);

        case REGEX_JUMP:
                return regex_first_walk(inst->x);

        case REGEX_SPLIT:
        {
                bool one = regex_first_walk(inst->x);
                bool two = regex_first_walk(inst->y);

                return one || two;
        }

        case REGEX_CHAR:
                regex_first_add(inst->value);
                return false;

        case REGEX_ANY:
                for (b32 c = 0; c < 256; c++)
                        regex_first[c] = 1;

                return false;

        case REGEX_SET:
                for (b32 c = 0; c < 256; c++)
                        if (regex_set_has(inst->set, (p8)c))
                                regex_first[c] = 1;

                return false;

        case REGEX_REPEAT:
                if (inst->kind == REGEX_ANY)
                {
                        for (b32 c = 0; c < 256; c++)
                                regex_first[c] = 1;
                }
                else if (inst->kind == REGEX_SET)
                {
                        for (b32 c = 0; c < 256; c++)
                                if (regex_set_has(inst->set, (p8)c))
                                        regex_first[c] = 1;
                }
                else
                {
                        regex_first_add(inst->value);
                }

                return inst->low == 0 ? regex_first_walk(inst->x) : false;

        default:
                return true;
        }
}

// True when DONE is reachable from here without eating a character, which is
// what makes the instruction in front of it able to be a match's last one.
static bool regex_tail_walk(b32 pc)
{
        if (regex_visited[pc])
                return false;

        regex_visited[pc] = 1;

        regex_instruction address_to inst = regex_code + pc;

        switch (inst->code)
        {
        case REGEX_DONE:
                return true;

        case REGEX_SAVE:
        case REGEX_BOL:
        case REGEX_EOL:
        case REGEX_EDGE:
                return regex_tail_walk(pc + 1);

        case REGEX_JUMP:
                return regex_tail_walk(inst->x);

        case REGEX_SPLIT:
        {
                bool one = regex_tail_walk(inst->x);
                bool two = regex_tail_walk(inst->y);

                return one || two;
        }

        case REGEX_REPEAT:
                return inst->low == 0 ? regex_tail_walk(inst->x) : false;

        case REGEX_BACK:
                return true;

        default:
                return false;
        }
}

static fn regex_last_add(b32 which, p8 kind, p8 value, b32 set)
{
        if (kind == REGEX_ANY)
        {
                for (b32 c = 0; c < 256; c++)
                        regex_last[c] = 1;

                return;
        }

        if (kind == REGEX_SET)
        {
                for (b32 c = 0; c < 256; c++)
                        if (regex_set_has(set, (p8)c))
                                regex_last[c] = 1;

                return;
        }

        regex_last[value] = 1;

        if (!regex_icase)
                return;

        if (value >= 'a' && value <= 'z')
                regex_last[value - 32] = 1;

        if (value >= 'A' && value <= 'Z')
                regex_last[value + 32] = 1;
}

/*
        What a match can end with.

        regex_search_longest asks the machine to finish at every position past
        the one it already found, and on a line of any length that is most of
        the work sed does. Nearly all of those questions have the same answer
        for a reason the machine has to run to discover: the character before
        the proposed end is not one the pattern could have ended on. This is
        that answer, worked out once.
*/
static fn regex_find_last()
{
        memory_fill(regex_last, 0, 256);
        regex_last_known = true;

        for (b32 i = 0; i < regex_length_code; i++)
        {
                p8 code = regex_code[i].code;

                if (code == REGEX_BACK)
                {
                        regex_last_known = false;
                        return;
                }

                if (code != REGEX_CHAR && code != REGEX_ANY && code != REGEX_SET &&
                    code != REGEX_REPEAT)
                        continue;

                b32 after = code == REGEX_REPEAT ? regex_code[i].x : i + 1;

                memory_fill(regex_visited, 0, (positive)regex_length_code);

                if (!regex_tail_walk(after))
                        continue;

                regex_last_add(i, code == REGEX_REPEAT ? regex_code[i].kind : code,
                               regex_code[i].value, regex_code[i].set);
        }
}

static fn regex_finish()
{
        regex_slot_used = (positive)(regex_group_count + 1) * 2;
        regex_loop_count = 0;

        for (b32 i = 0; i < regex_length_code; i++)
                if (regex_code[i].loop)
                {
                        if (regex_loop_count >= REGEX_LOOPS_KEPT)
                        {
                                regex_loop_count = -1;
                                break;
                        }

                        regex_loop_list[regex_loop_count++] = i;
                }

        if (regex_loop_count < 0)
                for (b32 i = 0; i < regex_length_code; i++)
                        regex_loop_at[i] = 0;

        // Only the pattern that begins with ^ and has no branch in front of
        // it: an alternation puts a split there instead, and one of its
        // branches may not be anchored at all.
        regex_anchored = regex_length_code > 1 && regex_code[1].code == REGEX_BOL;

        memory_fill(regex_visited, 0, (positive)regex_length_code);
        regex_first_known = !regex_first_walk(0);

        // Where the match can end in more than one place. A pattern of fixed
        // length always matches as far as it can, and asking for a longer
        // match than the one just found would be asking for nothing.
        regex_alternates = false;

        for (b32 i = 0; i < regex_length_code; i++)
                if (regex_code[i].code == REGEX_SPLIT ||
                    (regex_code[i].code == REGEX_REPEAT &&
                     regex_code[i].low != regex_code[i].high))
                        regex_alternates = true;

        if (regex_alternates)
                regex_find_last();
}

static bool regex_compile(string_address pattern, bool extended, bool icase, bool escapes)
{
        regex_code = regex_store + regex_pool_used;
        regex_sets = regex_set_store + regex_pool_sets;
        b32 tables = regex_first_used < REGEX_FIRST_MAX ? regex_first_used++
                                                        : REGEX_FIRST_MAX - 1;

        regex_first = regex_first_store[tables];
        regex_last = regex_last_store[tables];
        memory_fill(regex_first, 0, 256);
        regex_last_known = false;
        regex_length_code = 0;
        regex_set_count = 0;
        regex_group_count = 0;
        regex_extended = extended;
        regex_icase = icase;
        regex_escapes = escapes;
        regex_broken = false;
        regex_alternates = false;
        regex_pattern = pattern;
        regex_pattern_length = string_length(pattern);
        regex_pattern_at = 0;

        b32 open = regex_emit(REGEX_SAVE);

        regex_code[open].value = 0;
        regex_parse_alternation();

        b32 shut = regex_emit(REGEX_SAVE);

        regex_code[shut].value = 1;
        regex_emit(REGEX_DONE);

        if (regex_pattern_at < regex_pattern_length)
                regex_broken = true;

        regex_finish();
        return !regex_broken;
}

static bool regex_single(regex_instruction address_to inst, p8 character)
{
        if (inst->kind == REGEX_ANY)
                return true;

        if (inst->kind == REGEX_SET)
                return regex_set_has(inst->set, character);

        return (regex_icase ? text_lower(character) : character) == inst->value;
}

static b32 regex_run(b32 pc, positive sp)
{
        for (;;)
        {
                regex_instruction address_to inst = regex_code + pc;

                switch (inst->code)
                {
                case REGEX_DONE:
                        // Set only while a longer match than the one already
                        // found is being asked for; see regex_search_longest.
                        if (regex_stop_wanted != TEXT_UNSET && sp != regex_stop_wanted)
                                return 0;

                        return 1;

                case REGEX_CHAR:
                {
                        if (sp >= regex_text_length)
                                return 0;

                        p8 character = regex_text[sp];

                        if (regex_icase)
                                character = text_lower(character);

                        if (character != inst->value)
                                return 0;

                        sp++;
                        pc++;
                        continue;
                }

                case REGEX_ANY:
                        if (sp >= regex_text_length)
                                return 0;

                        sp++;
                        pc++;
                        continue;

                case REGEX_SET:
                        if (sp >= regex_text_length || !regex_set_has(inst->set, regex_text[sp]))
                                return 0;

                        sp++;
                        pc++;
                        continue;

                case REGEX_BOL:
                        if (sp != 0)
                                return 0;

                        pc++;
                        continue;

                case REGEX_EOL:
                        if (sp != regex_text_length)
                                return 0;

                        pc++;
                        continue;

                case REGEX_EDGE:
                {
                        bool before = sp > 0 && text_word(regex_text[sp - 1]);
                        bool after = sp < regex_text_length && text_word(regex_text[sp]);
                        bool wanted;

                        if (inst->value == REGEX_EDGE_WORD)
                                wanted = before != after;
                        else if (inst->value == REGEX_EDGE_NOT_WORD)
                                wanted = before == after;
                        else if (inst->value == REGEX_EDGE_START)
                                wanted = !before && after;
                        else
                                wanted = before && !after;

                        if (!wanted)
                                return 0;

                        pc++;
                        continue;
                }

                case REGEX_JUMP:
                        pc = inst->x;
                        continue;

                case REGEX_SAVE:
                {
                        positive was = regex_slots[inst->value];

                        regex_slots[inst->value] = sp;

                        if (regex_run(pc + 1, sp))
                                return 1;

                        regex_slots[inst->value] = was;
                        return 0;
                }

                case REGEX_SPLIT:
                {
                        // A group that can match nothing -- \(a*\)* -- would
                        // take the body branch at the same position forever.
                        // One position per loop instruction is enough to stop
                        // it, because a second try there cannot go anywhere
                        // the first did not.
                        if (inst->loop)
                        {
                                if (regex_loop_at[pc] == sp + 1)
                                {
                                        pc = inst->y;
                                        continue;
                                }

                                positive was = regex_loop_at[pc];

                                regex_loop_at[pc] = sp + 1;

                                if (regex_run(inst->x, sp))
                                {
                                        regex_loop_at[pc] = was;
                                        return 1;
                                }

                                regex_loop_at[pc] = was;
                                pc = inst->y;
                                continue;
                        }

                        if (regex_run(inst->x, sp))
                                return 1;

                        pc = inst->y;
                        continue;
                }

                case REGEX_REPEAT:
                {
                        positive taken = 0;
                        positive limit = inst->high < 0 ? regex_text_length : (positive)inst->high;

                        while (taken < limit && sp + taken < regex_text_length &&
                               regex_single(inst, regex_text[sp + taken]))
                                taken++;

                        if (taken < (positive)inst->low)
                                return 0;

                        for (;;)
                        {
                                if (regex_run(inst->x, sp + taken))
                                        return 1;

                                if (taken == (positive)inst->low)
                                        return 0;

                                taken--;
                        }
                }

                case REGEX_BACK:
                {
                        positive from = regex_slots[inst->value * 2];
                        positive to = regex_slots[inst->value * 2 + 1];

                        if (from == TEXT_UNSET || to == TEXT_UNSET || to < from)
                                return 0;

                        positive span = to - from;

                        if (sp + span > regex_text_length)
                                return 0;

                        for (positive i = 0; i < span; i++)
                        {
                                p8 one = regex_text[from + i];
                                p8 two = regex_text[sp + i];

                                if (regex_icase)
                                {
                                        one = text_lower(one);
                                        two = text_lower(two);
                                }

                                if (one != two)
                                        return 0;
                        }

                        sp += span;
                        pc++;
                        continue;
                }

                default:
                        return 0;
                }
        }
}

static fn regex_clear_state()
{
        for (positive i = 0; i < regex_slot_used; i++)
                regex_slots[i] = TEXT_UNSET;

        for (b32 i = 0; i < regex_loop_count; i++)
                regex_loop_at[regex_loop_list[i]] = 0;
}

// Leftmost: the first position where the whole pattern succeeds.
static bool regex_search(string_address text, positive length, positive from)
{
        regex_text = text;
        regex_text_length = length;

        for (positive at = from; at <= length; at++)
        {
                if (regex_first_known)
                {
                        while (at < length && !regex_first[text[at]])
                                at++;

                        // The table exists only when a match must eat a
                        // character, so there is nothing left to try.
                        if (at == length)
                                return false;
                }

                regex_clear_state();

                if (regex_run(0, at))
                        return true;

                if (regex_anchored)
                        return false;
        }

        return false;
}

/*
        Leftmost longest, for the callers that care where the match ended.

        Backtracking takes the first branch that works, and POSIX wants the
        longest: a\|ab against "ab" is "a" to this machine and "ab" to every
        sed on the planet. Once a match is known, the longest one starting at
        the same place is found by asking the machine to finish at each later
        position in turn, highest first, which is a question it can answer
        because DONE can be made to refuse.

        Greedy is not the same as longest, and not only for alternations:
        a*a*\(ab\)* against "ab" gives the first star the a and leaves the
        third with nothing, which is one character where GNU matches two. Any
        pattern whose length can vary pays for this; one of fixed length
        cannot end anywhere else and skips it.
*/
static bool regex_search_longest(string_address text, positive length, positive from)
{
        if (!regex_search(text, length, from))
                return false;

        if (!regex_alternates)
                return true;

        positive at = regex_slots[0];
        positive to = regex_slots[1];
        positive kept[REGEX_SLOT_MAX];

        for (positive i = 0; i < REGEX_SLOT_MAX; i++)
                kept[i] = regex_slots[i];

        for (positive stop = length; stop > to; stop--)
        {
                if (regex_last_known && !regex_last[text[stop - 1]])
                        continue;

                regex_clear_state();
                regex_stop_wanted = stop;

                bool got = regex_run(0, at);

                regex_stop_wanted = TEXT_UNSET;

                if (got)
                        return true;
        }

        for (positive i = 0; i < REGEX_SLOT_MAX; i++)
                regex_slots[i] = kept[i];

        return true;
}

/*
        What the kernel's stat says, read where the kernel actually puts it.

        struct file's status is laid out differently from the kernel's on
        arm64 and riscv64 -- library.c says as much where it declares it, and
        pins only the size -- so wc, which needs the mode as well to know
        whether a size is meaningful, reads the raw buffer itself.
*/
#if X64
#define TEXT_STAT_MODE 24
#else
#define TEXT_STAT_MODE 16
#endif
#define TEXT_STAT_SIZE 48

static bool text_regular_size(positive handle, positive address_to size)
{
        p8 raw[256];

        memory_fill(raw, 0, sizeof(raw));

        if (system_call_2(syscall(fstat), handle, (positive)raw) < 0)
                return false;

        p32 mode = address_to(p32 address_to)(raw + TEXT_STAT_MODE);

        if ((mode & 0170000) != 0100000)
                return false;

        address_to size = address_to(positive address_to)(raw + TEXT_STAT_SIZE);
        return true;
}

#define TEXT_FILES_MAX 1024

static b32 text_files[TEXT_FILES_MAX];
static b32 text_files_count;

static fn text_file_add(b32 which)
{
        if (text_files_count < TEXT_FILES_MAX)
                text_files[text_files_count++] = which;
}

static string_address text_file_name(b32 which)
{
        return text_files_count ? text_argument(text_files[which]) : null;
}

static fn text_banner(b32 which, bool first)
{
        string_address name = text_file_name(which);

        if (!first)
                text_put_character('\n');

        text_put_string("==> ");
        text_put_string(name ? name : (string_address)"standard input");
        text_put_string(" <==\n");
}

/*
        wc

        The padding is the whole difficulty. GNU decides one width for every
        column before it prints anything: one when a single count was asked
        for and there is one input, otherwise the digits it would take to
        write the total size of every input, and seven when any of them is
        something whose size cannot be known ahead of reading it -- a pipe,
        or a device.
*/
/*
        cat.

        Byte for byte when it is only moving a file, which is what it is asked
        to do nearly every time: no line splitting, no scanning, one buffer out
        for one buffer in. The flags all need to look at the bytes, so asking
        for any of them turns on the slower walk and none of them costs
        anything when they are not asked for.
*/
#define CAT_NUMBER 1     // -n, every line
#define CAT_NUMBER_FULL 2 // -b, only the ones with something on them
#define CAT_ENDS 4       // -E
#define CAT_TABS 8       // -T
#define CAT_SHOW 16      // -v
#define CAT_SQUEEZE 32   // -s

static positive cat_flags;
static positive cat_line_number;
static bool cat_blank_before;
static bool cat_at_line_start;

// A byte as -v spells it: control characters as ^X, the high half as M- and
// then the same rule again. Tab and newline are only touched by -T and by
// nothing, which is why they are not here.
static fn cat_visible(p8 value)
{
        if (value >= 128)
        {
                text_put_string((string_address) "M-");
                value -= 128;
        }

        if (value == 127)
        {
                text_put_string((string_address) "^?");
                return;
        }

        if (value < 32)
        {
                text_put_character('^');
                text_put_character((p8)(value + 64));
                return;
        }

        text_put_character(value);
}

static fn cat_number()
{
        p8 digits[24];
        positive at = sizeof(digits);
        positive value = cat_line_number;

        if (!value)
                digits[--at] = '0';

        while (value)
        {
                digits[--at] = (p8)('0' + value % 10);
                value /= 10;
            }

        // Six wide and right aligned, then a tab, which is what GNU does and
        // what anything reading the output will expect.
        for (positive pad = sizeof(digits) - at; pad < 6; pad++)
                text_put_character(' ');

        text_put(digits + at, sizeof(digits) - at);
        text_put_character('\t');

        cat_line_number++;
}

static fn cat_plain()
{
        while (text_fill())
        {
                text_put(text_input.buffer + text_input.position,
                         text_input.filled - text_input.position);
                text_input.position = text_input.filled;
        }
}

static fn cat_walked()
{
        while (text_fill())
        {
                while (text_input.position < text_input.filled)
                {
                        p8 value = text_input.buffer[text_input.position++];

                        if (cat_at_line_start)
                        {
                                bool blank = value == '\n';

                                // -s: any run of blank lines becomes one.
                                if ((cat_flags & CAT_SQUEEZE) && blank &&
                                    cat_blank_before)
                                        continue;

                                cat_blank_before = blank;

                                if ((cat_flags & CAT_NUMBER_FULL) ? !blank
                                                                  : (cat_flags &
                                                                     CAT_NUMBER))
                                        cat_number();

                                cat_at_line_start = false;
                        }

                        if (value == '\n')
                        {
                                if (cat_flags & CAT_ENDS)
                                        text_put_character('$');

                                text_put_character('\n');
                                cat_at_line_start = true;
                                continue;
                        }

                        if (value == '\t')
                        {
                                if (cat_flags & CAT_TABS)
                                        text_put_string((string_address) "^I");
                                else
                                        text_put_character('\t');

                                continue;
                        }

                        if (cat_flags & CAT_SHOW)
                        {
                                cat_visible(value);
                                continue;
                        }

                        text_put_character(value);
                }
        }
}

static b32 text_cat()
{
        b32 first = 1;
        b32 inputs = 0;

        text_begin("cat");

        cat_flags = 0;
        cat_line_number = 1;
        cat_blank_before = false;
        cat_at_line_start = true;

        while (first < text_argument_count)
        {
                string_address argument = text_argument(first);
                string_address letter;

                if (argument[0] != '-' || !argument[1])
                        break;

                if (argument[1] == '-' && !argument[2])
                {
                        first++;
                        break;
                }

                for (letter = argument + 1; string_get(letter); letter++)
                        switch (string_get(letter))
                        {
                        case 'n': cat_flags |= CAT_NUMBER; break;
                        case 'b': cat_flags |= CAT_NUMBER_FULL; break;
                        case 'E': cat_flags |= CAT_ENDS; break;
                        case 'T': cat_flags |= CAT_TABS; break;
                        case 'v': cat_flags |= CAT_SHOW; break;
                        case 's': cat_flags |= CAT_SQUEEZE; break;
                        case 'e': cat_flags |= CAT_SHOW | CAT_ENDS; break;
                        case 't': cat_flags |= CAT_SHOW | CAT_TABS; break;
                        case 'A':
                                cat_flags |= CAT_SHOW | CAT_ENDS | CAT_TABS;
                                break;
                        // Unbuffered, which this always is.
                        case 'u': break;
                        default:
                        {
                                p8 named[3] = {'-', string_get(letter), 0};

                                text_error(named, "invalid option");
                                text_status = 1;
                                return 1;
                        }
                        }

                first++;
        }

        // -b wins over -n, as it does everywhere else.
        if (cat_flags & CAT_NUMBER_FULL)
                cat_flags &= ~(positive)CAT_NUMBER;

        for (b32 i = first; i < text_argument_count; i++)
                inputs++;

        if (!inputs)
        {
                if (text_open(null))
                        cat_flags ? cat_walked() : cat_plain();

                text_close();
                text_flush();

                return text_status;
        }

        for (b32 i = first; i < text_argument_count; i++)
        {
                if (!text_open(text_argument(i)))
                        continue;

                cat_flags ? cat_walked() : cat_plain();
                text_close();
        }

        text_flush();

        return text_status;
}

static b32 text_wc()
{
        bool want_lines = false, want_words = false, want_bytes = false, want_chars = false;
        positive total_lines = 0, total_words = 0, total_bytes = 0;
        positive width = 1;
        positive known = 0;
        bool unknown = false;

        text_begin("wc");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] && !text_files_count)
                {
                        if (argument[1] == '-' && !argument[2])
                                continue;

                        for (positive c = 1; argument[c]; c++)
                                switch (argument[c])
                                {
                                case 'l': want_lines = true; break;
                                case 'w': want_words = true; break;
                                case 'c': want_bytes = true; break;
                                case 'm': want_chars = true; break;
                                default: break;
                                }

                        continue;
                }

                text_file_add(i);
        }

        if (!want_lines && !want_words && !want_bytes && !want_chars)
        {
                want_lines = true;
                want_words = true;
                want_bytes = true;
        }

        b32 selected = (b32)want_lines + (b32)want_words + (b32)want_bytes + (b32)want_chars;
        b32 inputs = text_files_count ? text_files_count : 1;

        if (selected > 1 || inputs > 1)
        {
                for (b32 i = 0; i < inputs; i++)
                {
                        string_address name = text_files_count ? text_argument(text_files[i]) : null;
                        positive size = 0;

                        if (!name || (name[0] == '-' && !name[1]))
                        {
                                if (!text_regular_size(0, address_of size))
                                        unknown = true;
                                else
                                        known += size;

                                continue;
                        }

                        bipolar probe = text_open_handle(name, FILE_READ, 0);

                        if (probe < 0)
                                continue;

                        if (!text_regular_size((positive)probe, address_of size))
                                unknown = true;
                        else
                                known += size;

                        system_call_1(syscall(close), (positive)probe);
                }

                if (unknown)
                {
                        width = 7;
                }
                else
                {
                        positive digits = 1;
                        positive scale = 10;

                        while (known >= scale)
                        {
                                digits++;
                                scale *= 10;
                        }

                        width = digits;
                }
        }

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_files_count ? text_argument(text_files[i]) : null;
                positive lines = 0, words = 0, bytes = 0;
                bool inside = false;

                if (!text_open(name))
                        continue;

                while (text_fill())
                {
                        p8 address_to at = text_input.buffer + text_input.position;
                        positive left = text_input.filled - text_input.position;

                        bytes += left;

                        for (positive c = 0; c < left; c++)
                        {
                                p8 character = at[c];

                                if (character == '\n')
                                        lines++;

                                if (text_space(character))
                                {
                                        inside = false;
                                }
                                else if (!inside)
                                {
                                        inside = true;
                                        words++;
                                }
                        }

                        text_input.position = text_input.filled;
                }

                text_close();

                total_lines += lines;
                total_words += words;
                total_bytes += bytes;

                bool leading = true;

                if (want_lines)
                {
                        text_put_number(lines, width);
                        leading = false;
                }

                if (want_words)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(words, width);
                        leading = false;
                }

                if (want_chars)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(bytes, width);
                        leading = false;
                }

                if (want_bytes)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(bytes, width);
                }

                if (name)
                {
                        text_put_character(' ');
                        text_put_string(name);
                }

                text_put_character('\n');
        }

        if (text_files_count > 1)
        {
                bool leading = true;

                if (want_lines)
                {
                        text_put_number(total_lines, width);
                        leading = false;
                }

                if (want_words)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(total_words, width);
                        leading = false;
                }

                if (want_chars)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(total_bytes, width);
                        leading = false;
                }

                if (want_bytes)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(total_bytes, width);
                }

                text_put_string(" total\n");
        }

        return text_done(text_status);
}

static b32 text_rev()
{
        text_begin("rev");

        for (b32 i = 1; i < text_argument_count; i++)
                text_file_add(i);

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        for (positive c = text_line_length; c > 0; c--)
                                text_put_character(text_line[c - 1]);

                        if (text_line_ended)
                                text_put_character('\n');
                }

                text_close();
        }

        return text_done(text_status);
}

static b32 text_head()
{
        positive count = 10;
        bool by_bytes = false;
        bool quiet = false;
        bool loud = false;

        text_begin("head");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                if (text_digit(argument[1]))
                {
                        text_number_of(argument + 1, address_of count);
                        continue;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'q')
                        {
                                quiet = true;
                                continue;
                        }

                        if (flag == 'v')
                        {
                                loud = true;
                                continue;
                        }

                        if (flag == 'n' || flag == 'c')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                by_bytes = flag == 'c';

                                if (!value || !text_number_of(value, address_of count))
                                {
                                        text_error(null, "invalid number of lines");
                                        return text_done(1);
                                }

                                break;
                        }
                }
        }

        b32 inputs = text_files_count ? text_files_count : 1;
        bool headers = (text_files_count > 1 || loud) && !quiet;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                if (headers)
                        text_banner(i, i == 0);

                if (by_bytes)
                {
                        positive left = count;

                        while (left && text_fill())
                        {
                                positive have = text_input.filled - text_input.position;
                                positive take = have < left ? have : left;

                                text_put(text_input.buffer + text_input.position, take);
                                text_input.position += take;
                                left -= take;
                        }
                }
                else
                {
                        positive done = 0;

                        while (done < count && text_line_next())
                        {
                                text_put_line();
                                done++;
                        }
                }

                text_close();
        }

        return text_done(text_status);
}

/*
        Lines held whole, because sort and tail both need every line at once
        and a line is a slice of the arena rather than a string: nothing here
        writes a terminator, so a line with a NUL in it survives.
*/
#define TEXT_LINES_MAX (1 << 20)

typedef struct
{
        p8 address_to at;
        positive length;
        bool ended;
} text_slice;

static text_slice address_to text_lines;
static positive text_lines_count;

static bool text_lines_ready()
{
        if (text_lines)
                return true;

        text_lines = (text_slice address_to)text_arena_take(TEXT_LINES_MAX * sizeof(text_slice));
        return text_lines != null;
}

static bool text_lines_gather()
{
        if (!text_lines_ready())
                return false;

        while (text_line_next())
        {
                if (text_lines_count >= TEXT_LINES_MAX)
                {
                        text_error(null, "too many lines");
                        return false;
                }

                p8 address_to room = (p8 address_to)text_arena_take(text_line_length + 1);

                if (!room)
                        return false;

                memory_copy(room, text_line, text_line_length);
                text_lines[text_lines_count].at = room;
                text_lines[text_lines_count].length = text_line_length;
                text_lines[text_lines_count].ended = text_line_ended;
                text_lines_count++;
        }

        return true;
}

static fn text_put_slice(text_slice address_to line)
{
        text_put(line->at, line->length);

        if (line->ended)
                text_put_character('\n');
}

static bool text_read_at(positive handle, positive offset, p8 address_to into, positive want)
{
        positive have = 0;

        system_call_3(syscall(lseek), handle, offset, FILE_SEEK_SET);

        while (have < want)
        {
                bipolar got = system_call_3(syscall(read), handle,
                                            (positive)(into + have), want - have);

                if (got <= 0)
                        return false;

                have += (positive)got;
        }

        return true;
}

/*
        Where the last count lines begin, found from the end.

        A file that can be seeked does not have to be read to be tailed, and
        reading it is not merely slower: everything held at once is bounded by
        the arena, so a log big enough would fail rather than be slow. This
        walks back a buffer at a time looking for newlines and only the tail
        of the file is ever touched.

        The newline that ends the file is not one of the ones being counted --
        it terminates the last line rather than starting one.
*/
static positive text_tail_start(positive handle, positive size, positive count)
{
        p8 window[TEXT_READ_MAX];
        positive at = size;
        positive found = 0;
        bool skipped = false;

        if (!count)
                return size;

        while (at)
        {
                positive take = at < TEXT_READ_MAX ? at : TEXT_READ_MAX;

                at -= take;

                if (!text_read_at(handle, at, window, take))
                        return 0;

                for (positive i = take; i > 0; i--)
                {
                        if (window[i - 1] != '\n')
                                continue;

                        if (!skipped && at + i == size)
                        {
                                skipped = true;
                                continue;
                        }

                        found++;

                        if (found == count)
                                return at + i;
                }
        }

        return 0;
}

static fn text_stream_from(positive start)
{
        system_call_3(syscall(lseek), text_input.handle, start, FILE_SEEK_SET);
        text_input.filled = 0;
        text_input.position = 0;
        text_input.finished = false;

        while (text_fill())
        {
                text_put(text_input.buffer + text_input.position,
                         text_input.filled - text_input.position);
                text_input.position = text_input.filled;
        }
}

static b32 text_tail()
{
        positive count = 10;
        bool by_bytes = false;
        bool from_start = false;
        bool quiet = false;
        bool loud = false;

        text_begin("tail");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                if (text_digit(argument[1]) || argument[1] == '+')
                {
                        from_start = argument[1] == '+';
                        text_number_of(argument + 1 + (from_start ? 1 : 0), address_of count);
                        continue;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'q')
                        {
                                quiet = true;
                                continue;
                        }

                        if (flag == 'v')
                        {
                                loud = true;
                                continue;
                        }

                        if (flag == 'n' || flag == 'c')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                by_bytes = flag == 'c';

                                if (!value)
                                {
                                        text_error(null, "invalid number of lines");
                                        return text_done(1);
                                }

                                if (value[0] == '+')
                                {
                                        from_start = true;
                                        value++;
                                }
                                else if (value[0] == '-')
                                {
                                        value++;
                                }

                                if (!text_number_of(value, address_of count))
                                {
                                        text_error(null, "invalid number of lines");
                                        return text_done(1);
                                }

                                break;
                        }
                }
        }

        b32 inputs = text_files_count ? text_files_count : 1;
        bool headers = (text_files_count > 1 || loud) && !quiet;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                if (headers)
                        text_banner(i, i == 0);

                text_lines_count = 0;
                text_arena_used = text_lines ? TEXT_LINES_MAX * sizeof(text_slice) : 0;

                positive size = 0;
                bool seekable = !from_start &&
                                text_regular_size(text_input.handle, address_of size);

                if (seekable)
                {
                        text_stream_from(by_bytes
                                             ? (count < size ? size - count : 0)
                                             : text_tail_start(text_input.handle, size, count));
                        text_close();
                        continue;
                }

                if (by_bytes)
                {
                        // Bytes rather than lines, so the whole input is held
                        // and the tail of it handed back.
                        if (!text_lines_ready())
                                return text_done(1);

                        p8 address_to held = (p8 address_to)text_arena_take(0);
                        positive have = 0;

                        while (text_fill())
                        {
                                positive left = text_input.filled - text_input.position;
                                p8 address_to room = (p8 address_to)text_arena_take(left);

                                if (!room)
                                        return text_done(1);

                                if (!have)
                                        held = room;

                                memory_copy(room, text_input.buffer + text_input.position, left);
                                have += left;
                                text_input.position = text_input.filled;
                        }

                        if (from_start)
                        {
                                positive skip = count ? count - 1 : 0;

                                if (skip < have)
                                        text_put(held + skip, have - skip);
                        }
                        else
                        {
                                positive take = count < have ? count : have;

                                text_put(held + have - take, take);
                        }
                }
                else if (from_start)
                {
                        positive seen = 0;

                        while (text_line_next())
                        {
                                seen++;

                                if (seen >= (count ? count : 1))
                                        text_put_line();
                        }
                }
                else
                {
                        if (!text_lines_gather())
                                return text_done(1);

                        positive first = text_lines_count > count ? text_lines_count - count : 0;

                        for (positive c = first; c < text_lines_count; c++)
                                text_put_slice(text_lines + c);
                }

                text_close();
        }

        return text_done(text_status);
}

static b32 text_tee()
{
        bool append = false;
        positive handles[TEXT_FILES_MAX];
        b32 handle_count = 0;

        text_begin("tee");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] && !text_files_count)
                {
                        for (positive c = 1; argument[c]; c++)
                                if (argument[c] == 'a')
                                        append = true;

                        continue;
                }

                text_file_add(i);
        }

        for (b32 i = 0; i < text_files_count; i++)
        {
                string_address name = text_argument(text_files[i]);
                bipolar target = text_open_handle(
                    name, append ? TEXT_APPEND : TEXT_WRITE, 0666);

                if (target < 0)
                {
                        text_error(name, "Cannot open file");
                        text_status = 1;
                        continue;
                }

                handles[handle_count++] = (positive)target;
        }

        if (!text_open(null))
                return text_done(1);

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;

                text_write_raw(1, at, left);

                for (b32 i = 0; i < handle_count; i++)
                        text_write_raw(handles[i], at, left);

                text_input.position = text_input.filled;
        }

        for (b32 i = 0; i < handle_count; i++)
                system_call_1(syscall(close), handles[i]);

        return text_done(text_status);
}

static b32 text_nl()
{
        positive width = 6;
        positive number = 1;
        positive step = 1;
        p8 style = 't';
        string_address separator = "\t";
        p8 justify = 'r';
        bool zeros = false;

        text_begin("nl");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                p8 flag = argument[1];
                string_address value = argument[2] ? argument + 2 : null;

                if (flag == 'b' || flag == 'w' || flag == 's' || flag == 'v' ||
                    flag == 'i' || flag == 'n')
                {
                        if (!value)
                                value = text_argument(++i);

                        if (!value)
                                continue;

                        if (flag == 'b')
                                style = value[0];
                        else if (flag == 'w')
                                text_number_of(value, address_of width);
                        else if (flag == 's')
                                separator = value;
                        else if (flag == 'v')
                                text_number_of(value, address_of number);
                        else if (flag == 'i')
                                text_number_of(value, address_of step);
                        else
                        {
                                justify = value[0];
                                zeros = value[0] == 'r' && value[1] == 'z';

                                if (value[0] == 'l')
                                        justify = 'l';
                        }
                }
        }

        b32 inputs = text_files_count ? text_files_count : 1;
        positive separator_length = string_length(separator);

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        bool numbered = style == 'a' ||
                                        (style == 't' && text_line_length != 0);

                        if (style == 'n')
                                numbered = false;

                        if (numbered)
                        {
                                if (justify == 'l')
                                {
                                        positive was = text_out_used;

                                        text_put_number(number, 1);

                                        positive wrote = text_out_used - was;

                                        while (wrote < width)
                                        {
                                                text_put_character(' ');
                                                wrote++;
                                        }
                                }
                                else if (zeros)
                                {
                                        p8 digits[24];
                                        positive have = 0;
                                        positive value = number;

                                        if (!value)
                                                digits[have++] = '0';

                                        while (value)
                                        {
                                                digits[have++] = (p8)('0' + value % 10);
                                                value /= 10;
                                        }

                                        for (positive c = have; c < width; c++)
                                                text_put_character('0');

                                        while (have)
                                                text_put_character(digits[--have]);
                                }
                                else
                                {
                                        text_put_number(number, width);
                                }

                                text_put(separator, separator_length);
                                number += step;
                        }
                        else
                        {
                                // The columns stay, so an unnumbered line
                                // lines up under a numbered one.
                                for (positive c = 0; c < width + separator_length; c++)
                                        text_put_character(' ');
                        }

                        text_put_line();
                }

                text_close();
        }

        return text_done(text_status);
}

static b32 text_fold()
{
        positive width = 80;
        bool spaces = false;
        bool bytes = false;

        text_begin("fold");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (text_digit(argument[1]))
                {
                        text_number_of(argument + 1, address_of width);
                        continue;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 's')
                        {
                                spaces = true;
                                continue;
                        }

                        if (flag == 'b')
                        {
                                bytes = true;
                                continue;
                        }

                        if (flag == 'w')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                if (value)
                                        text_number_of(value, address_of width);

                                break;
                        }
                }
        }

        if (!width)
                width = 1;

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        positive from = 0;

                        while (from < text_line_length)
                        {
                                positive column = 0;
                                positive at = from;
                                positive gap = 0;

                                // Columns, not bytes, unless -b: a tab moves
                                // to the next stop of eight and a backspace
                                // moves back, which is what makes the width
                                // mean what it looks like on a terminal.
                                while (at < text_line_length)
                                {
                                        p8 character = text_line[at];
                                        positive after = column + 1;

                                        if (!bytes)
                                        {
                                                if (character == '\t')
                                                        after = (column / 8 + 1) * 8;
                                                else if (character == '\b')
                                                        after = column ? column - 1 : 0;
                                                else if (character == '\r')
                                                        after = 0;
                                        }

                                        if (after > width && at > from)
                                                break;

                                        column = after;
                                        at++;

                                        if (spaces && text_blank(character))
                                                gap = at;
                                }

                                if (at >= text_line_length)
                                {
                                        text_put(text_line + from, text_line_length - from);

                                        if (text_line_ended)
                                                text_put_character('\n');

                                        break;
                                }

                                if (spaces && gap > from)
                                        at = gap;

                                text_put(text_line + from, at - from);
                                text_put_character('\n');
                                from = at;
                        }

                        if (!text_line_length && text_line_ended)
                                text_put_character('\n');
                }

                text_close();
        }

        return text_done(text_status);
}

/*
        A list of positions -- 1,3-5,7- -- which cut takes for both fields and
        characters. The open ended tail is kept as a number rather than as
        marks, because "7-" means every field a line happens to have.
*/
#define TEXT_LIST_MAX 4096

static p8 text_list[TEXT_LIST_MAX];
static positive text_list_open;

static bool text_list_parse(string_address spec)
{
        positive at = 0;

        while (spec[at])
        {
                positive first = 0;
                positive last = 0;
                bool have_first = false;
                bool have_last = false;

                while (text_digit(spec[at]))
                {
                        first = first * 10 + (positive)(spec[at] - '0');
                        have_first = true;
                        at++;
                }

                if (spec[at] == '-')
                {
                        at++;

                        while (text_digit(spec[at]))
                        {
                                last = last * 10 + (positive)(spec[at] - '0');
                                have_last = true;
                                at++;
                        }

                        if (!have_first)
                                first = 1;

                        if (!have_last)
                        {
                                if (!text_list_open || first < text_list_open)
                                        text_list_open = first;

                                last = first;
                        }
                }
                else
                {
                        if (!have_first)
                                return false;

                        last = first;
                }

                if (!first)
                        return false;

                for (positive i = first; i <= last && i < TEXT_LIST_MAX; i++)
                        text_list[i] = 1;

                if (spec[at] == ',')
                {
                        at++;
                        continue;
                }

                if (spec[at])
                        return false;
        }

        return true;
}

static bool text_list_has(positive which)
{
        if (text_list_open && which >= text_list_open)
                return true;

        return which < TEXT_LIST_MAX && text_list[which];
}

static b32 text_cut()
{
        p8 delimiter = '\t';
        bool by_field = false;
        bool by_character = false;
        bool only_delimited = false;
        bool have_list = false;

        text_begin("cut");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 's')
                        {
                                only_delimited = true;
                                continue;
                        }

                        if (flag == 'n')
                                continue;

                        if (flag == 'd' || flag == 'f' || flag == 'c' || flag == 'b')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(1);
                                }

                                if (flag == 'd')
                                {
                                        delimiter = value[0];
                                }
                                else
                                {
                                        by_field = flag == 'f';
                                        by_character = !by_field;
                                        have_list = true;

                                        if (!text_list_parse(value))
                                        {
                                                text_error(null, "invalid list");
                                                return text_done(1);
                                        }
                                }

                                break;
                        }
                }
        }

        if (!have_list)
        {
                text_error(null, "you must specify a list of bytes, characters, or fields");
                return text_done(1);
        }

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        if (by_character)
                        {
                                for (positive c = 0; c < text_line_length; c++)
                                        if (text_list_has(c + 1))
                                                text_put_character(text_line[c]);

                                text_put_character('\n');
                                continue;
                        }

                        if (by_field)
                        {
                                bool split = false;

                                for (positive c = 0; c < text_line_length; c++)
                                        if (text_line[c] == delimiter)
                                        {
                                                split = true;
                                                break;
                                        }

                                // A line with no delimiter is one whole field,
                                // and is printed unchanged unless -s says not
                                // to print it at all.
                                if (!split)
                                {
                                        if (!only_delimited)
                                        {
                                                text_put(text_line, text_line_length);
                                                text_put_character('\n');
                                        }

                                        continue;
                                }

                                positive at = 0;
                                positive which = 1;
                                bool wrote = false;

                                while (at <= text_line_length)
                                {
                                        positive from = at;

                                        while (at < text_line_length && text_line[at] != delimiter)
                                                at++;

                                        if (text_list_has(which))
                                        {
                                                if (wrote)
                                                        text_put_character(delimiter);

                                                text_put(text_line + from, at - from);
                                                wrote = true;
                                        }

                                        if (at == text_line_length)
                                                break;

                                        at++;
                                        which++;
                                }

                                text_put_character('\n');
                        }
                }

                text_close();
        }

        return text_done(text_status);
}

/*
        tr's sets.

        Expanded once into a plain array of bytes, because everything tr does
        -- translate, delete, squeeze -- is an index into the first set and a
        lookup at the same index in the second, and a set that has been
        expanded makes both of those a subscript.
*/
#define TEXT_SET_MAX 1024

static p8 text_set_one[TEXT_SET_MAX];
static positive text_set_one_length;
static p8 text_set_two[TEXT_SET_MAX];
static positive text_set_two_length;

static fn text_set_put(p8 address_to into, positive address_to have, p8 character)
{
        if (address_to have < TEXT_SET_MAX)
                into[address_to have] = character;

        address_to have += 1;
}

static bool text_set_class(p8 address_to into, positive address_to have, string_address name)
{
        for (b32 c = 0; c < 256; c++)
        {
                p8 character = (p8)c;
                bool wanted = false;

                if (string_compare(name, "alpha") == 0)
                        wanted = (character >= 'a' && character <= 'z') ||
                                 (character >= 'A' && character <= 'Z');
                else if (string_compare(name, "digit") == 0)
                        wanted = text_digit(character);
                else if (string_compare(name, "alnum") == 0)
                        wanted = text_digit(character) ||
                                 (character >= 'a' && character <= 'z') ||
                                 (character >= 'A' && character <= 'Z');
                else if (string_compare(name, "upper") == 0)
                        wanted = character >= 'A' && character <= 'Z';
                else if (string_compare(name, "lower") == 0)
                        wanted = character >= 'a' && character <= 'z';
                else if (string_compare(name, "space") == 0)
                        wanted = text_space(character);
                else if (string_compare(name, "blank") == 0)
                        wanted = text_blank(character);
                else if (string_compare(name, "print") == 0)
                        wanted = character >= 32 && character < 127;
                else if (string_compare(name, "graph") == 0)
                        wanted = character > 32 && character < 127;
                else if (string_compare(name, "cntrl") == 0)
                        wanted = character < 32 || character == 127;
                else if (string_compare(name, "punct") == 0)
                        wanted = character > 32 && character < 127 &&
                                 !text_digit(character) &&
                                 !(character >= 'a' && character <= 'z') &&
                                 !(character >= 'A' && character <= 'Z');
                else if (string_compare(name, "xdigit") == 0)
                        wanted = text_digit(character) ||
                                 (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F');
                else
                        return false;

                if (wanted)
                        text_set_put(into, have, character);
        }

        return true;
}

static p8 text_escape(string_address spec, positive address_to at)
{
        p8 character = spec[address_to at];

        if (character != '\\' || !spec[address_to at + 1])
        {
                address_to at += 1;
                return character;
        }

        p8 next = spec[address_to at + 1];

        address_to at += 2;

        switch (next)
        {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'a': return 7;
        case 'b': return 8;
        case 'f': return 12;
        case 'v': return 11;
        case '\\': return '\\';
        default: break;
        }

        if (next >= '0' && next <= '7')
        {
                positive value = (positive)(next - '0');
                positive taken = 1;

                while (taken < 3 && spec[address_to at] >= '0' && spec[address_to at] <= '7')
                {
                        value = value * 8 + (positive)(spec[address_to at] - '0');
                        address_to at += 1;
                        taken++;
                }

                return (p8)value;
        }

        return next;
}

static fn text_set_build(string_address spec, p8 address_to into, positive address_to have)
{
        positive at = 0;
        positive length = string_length(spec);

        address_to have = 0;

        while (at < length)
        {
                if (spec[at] == '[' && spec[at + 1] == ':')
                {
                        p8 name[16];
                        positive fill = 0;
                        positive scan = at + 2;

                        while (scan < length && spec[scan] != ':' && fill < 15)
                                name[fill++] = spec[scan++];

                        name[fill] = '\0';

                        if (scan + 1 < length && spec[scan] == ':' && spec[scan + 1] == ']' &&
                            text_set_class(into, have, name))
                        {
                                at = scan + 2;
                                continue;
                        }
                }

                if (spec[at] == '[' && spec[at + 1] == '=' && at + 3 < length &&
                    spec[at + 3] == '=' && spec[at + 4] == ']')
                {
                        text_set_put(into, have, spec[at + 2]);
                        at = at + 5;
                        continue;
                }

                positive was = at;
                p8 first = text_escape(spec, address_of at);

                // [x*n] repeats, and [x*] repeats until the other set runs out
                // -- which here means until the array is full, since a set
                // longer than the one it pads against is simply unused.
                if (spec[was] == '[' && spec[was + 1] && spec[was + 2] == '*')
                {
                        p8 repeated = spec[was + 1];
                        positive scan = was + 3;
                        positive count = 0;
                        bool digits = false;
                        positive base = 10;

                        if (spec[scan] == '0')
                                base = 8;

                        while (scan < length && spec[scan] >= '0' && spec[scan] <= '9')
                        {
                                count = count * base + (positive)(spec[scan] - '0');
                                digits = true;
                                scan++;
                        }

                        if (scan < length && spec[scan] == ']')
                        {
                                if (!digits)
                                        count = TEXT_SET_MAX - address_to have;

                                for (positive i = 0; i < count; i++)
                                        text_set_put(into, have, repeated);

                                at = scan + 1;
                                continue;
                        }
                }

                if (at < length && spec[at] == '-' && at + 1 < length && spec[at + 1] != '\0')
                {
                        positive after = at + 1;
                        p8 last = text_escape(spec, address_of after);

                        for (b32 c = first; c <= (b32)last; c++)
                                text_set_put(into, have, (p8)c);

                        at = after;
                        continue;
                }

                text_set_put(into, have, first);
        }

        if (address_to have > TEXT_SET_MAX)
                address_to have = TEXT_SET_MAX;
}

static b32 text_tr()
{
        bool remove = false;
        bool squeeze = false;
        bool complement = false;
        string_address first = null;
        string_address second = null;

        text_begin("tr");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] && !first)
                {
                        if (argument[1] == '-' && !argument[2])
                                continue;

                        bool flags = true;

                        for (positive c = 1; argument[c]; c++)
                                if (argument[c] != 'd' && argument[c] != 's' &&
                                    argument[c] != 'c' && argument[c] != 'C')
                                        flags = false;

                        if (flags)
                        {
                                for (positive c = 1; argument[c]; c++)
                                        switch (argument[c])
                                        {
                                        case 'd': remove = true; break;
                                        case 's': squeeze = true; break;
                                        case 'c':
                                        case 'C': complement = true; break;
                                        default: break;
                                        }

                                continue;
                        }
                }

                if (!first)
                        first = argument;
                else if (!second)
                        second = argument;
        }

        if (!first)
        {
                text_error(null, "missing operand");
                return text_done(1);
        }

        text_set_build(first, text_set_one, address_of text_set_one_length);

        if (second)
                text_set_build(second, text_set_two, address_of text_set_two_length);

        p8 in_first[256];
        p8 in_second[256];
        p8 mapped[256];

        memory_fill(in_first, 0, sizeof(in_first));
        memory_fill(in_second, 0, sizeof(in_second));

        for (positive i = 0; i < text_set_one_length && i < TEXT_SET_MAX; i++)
                in_first[text_set_one[i]] = 1;

        if (complement)
                for (b32 c = 0; c < 256; c++)
                        in_first[c] = (p8)!in_first[c];

        for (positive i = 0; i < text_set_two_length && i < TEXT_SET_MAX; i++)
                in_second[text_set_two[i]] = 1;

        for (b32 c = 0; c < 256; c++)
                mapped[c] = (p8)c;

        // A second set shorter than the first is padded with its last
        // character, which is what makes tr a-z x work.
        if (second && !remove && text_set_two_length)
        {
                if (complement)
                {
                        p8 last = text_set_two[(text_set_two_length < TEXT_SET_MAX
                                                    ? text_set_two_length
                                                    : TEXT_SET_MAX) - 1];

                        for (b32 c = 0; c < 256; c++)
                                if (in_first[c])
                                        mapped[c] = last;
                }
                else
                {
                        positive have = text_set_one_length < TEXT_SET_MAX ? text_set_one_length
                                                                          : TEXT_SET_MAX;
                        positive room = text_set_two_length < TEXT_SET_MAX ? text_set_two_length
                                                                          : TEXT_SET_MAX;

                        for (positive i = 0; i < have; i++)
                                mapped[text_set_one[i]] =
                                    text_set_two[i < room ? i : room - 1];
                }
        }

        // Without a second set, squeezing looks at the first one.
        p8 address_to squeezed = second && !remove ? in_second : in_first;

        if (!text_open(null))
                return text_done(1);

        b32 last_written = -1;

        while (text_fill())
        {
                p8 address_to at = text_input.buffer + text_input.position;
                positive left = text_input.filled - text_input.position;

                for (positive c = 0; c < left; c++)
                {
                        p8 character = at[c];

                        if (remove && in_first[character])
                                continue;

                        p8 out = remove ? character : mapped[character];

                        if (squeeze && squeezed[out] && (b32)out == last_written)
                                continue;

                        text_put_character(out);
                        last_written = out;
                }

                text_input.position = text_input.filled;
        }

        return text_done(text_status);
}

static b32 text_uniq()
{
        bool counting = false;
        bool repeated_only = false;
        bool unique_only = false;
        bool fold = false;
        positive skip_fields = 0;
        positive skip_characters = 0;
        positive compare_width = 0;
        bool bounded = false;

        text_begin("uniq");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'c')
                        {
                                counting = true;
                                continue;
                        }

                        if (flag == 'd')
                        {
                                repeated_only = true;
                                continue;
                        }

                        if (flag == 'u')
                        {
                                unique_only = true;
                                continue;
                        }

                        if (flag == 'i')
                        {
                                fold = true;
                                continue;
                        }

                        if (flag == 'f' || flag == 's' || flag == 'w')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);
                                positive number = 0;

                                if (!value || !text_number_of(value, address_of number))
                                {
                                        text_error(null, "invalid number");
                                        return text_done(1);
                                }

                                if (flag == 'f')
                                        skip_fields = number;
                                else if (flag == 's')
                                        skip_characters = number;
                                else
                                {
                                        compare_width = number;
                                        bounded = true;
                                }

                                break;
                        }
                }
        }

        if (!text_open(text_files_count ? text_argument(text_files[0]) : null))
                return text_done(1);

        // uniq's second operand is where the answer goes, not another input.
        if (text_files_count > 1)
        {
                string_address name = text_argument(text_files[1]);
                bipolar target = text_open_handle(name, TEXT_WRITE, 0666);

                if (target < 0)
                {
                        text_error(name, "Cannot open file");
                        return text_done(1);
                }

                text_out_handle = (positive)target;
        }

        static p8 held[TEXT_LINE_MAX];
        positive held_length = 0;
        bool held_ended = false;
        bool have_held = false;
        positive count = 0;

        for (;;)
        {
                bool more = text_line_next();

                if (more)
                {
                        positive skip = 0;

                        // The compared part starts after the skipped fields
                        // and then after the skipped characters, which is the
                        // order POSIX puts them in.
                        for (positive f = 0; f < skip_fields; f++)
                        {
                                while (skip < text_line_length && text_blank(text_line[skip]))
                                        skip++;

                                while (skip < text_line_length && !text_blank(text_line[skip]))
                                        skip++;
                        }

                        skip += skip_characters;

                        if (skip > text_line_length)
                                skip = text_line_length;

                        positive held_skip = 0;

                        for (positive f = 0; f < skip_fields; f++)
                        {
                                while (held_skip < held_length && text_blank(held[held_skip]))
                                        held_skip++;

                                while (held_skip < held_length && !text_blank(held[held_skip]))
                                        held_skip++;
                        }

                        held_skip += skip_characters;

                        if (held_skip > held_length)
                                held_skip = held_length;

                        positive one = text_line_length - skip;
                        positive two = held_length - held_skip;

                        if (bounded)
                        {
                                if (one > compare_width)
                                        one = compare_width;

                                if (two > compare_width)
                                        two = compare_width;
                        }

                        bool same = have_held && one == two;

                        for (positive c = 0; same && c < one; c++)
                        {
                                p8 a = text_line[skip + c];
                                p8 b = held[held_skip + c];

                                if (fold)
                                {
                                        a = text_lower(a);
                                        b = text_lower(b);
                                }

                                if (a != b)
                                        same = false;
                        }

                        if (same)
                        {
                                count++;
                                continue;
                        }
                }

                if (have_held)
                {
                        bool show = true;

                        if (repeated_only && count < 2)
                                show = false;

                        if (unique_only && count > 1)
                                show = false;

                        if (show)
                        {
                                if (counting)
                                {
                                        text_put_number(count, 7);
                                        text_put_character(' ');
                                }

                                text_put(held, held_length);

                                if (held_ended)
                                        text_put_character('\n');
                        }
                }

                if (!more)
                        break;

                memory_copy(held, text_line, text_line_length);
                held_length = text_line_length;
                held_ended = text_line_ended;
                have_held = true;
                count = 1;
        }

        text_close();
        return text_done(text_status);
}

/*
        grep

        Several patterns -- from -e, from -f, or from a pattern argument with
        newlines in it -- are joined into one alternation rather than kept as
        several programs, because a line has to be tried against all of them
        anyway and the machine already knows how to choose a branch.
*/
#define GREP_PATTERN_MAX 16384

static p8 grep_pattern[GREP_PATTERN_MAX];
static positive grep_pattern_length;
static bool grep_pattern_any;

static fn grep_pattern_put(p8 character)
{
        if (grep_pattern_length < GREP_PATTERN_MAX - 1)
                grep_pattern[grep_pattern_length++] = character;
}

static fn grep_pattern_add(string_address text, positive length, bool fixed, bool extended)
{
        positive from = 0;

        for (positive at = 0; at <= length; at++)
        {
                if (at != length && text[at] != '\n')
                        continue;

                if (grep_pattern_any)
                {
                        if (!extended)
                                grep_pattern_put('\\');

                        grep_pattern_put('|');
                }

                for (positive c = from; c < at; c++)
                {
                        p8 character = text[c];

                        // A fixed string becomes a pattern that cannot mean
                        // anything but itself. In basic syntax only these six
                        // are operators, and escaping the rest would turn
                        // them into the GNU extensions instead.
                        if (fixed && (character == '.' || character == '[' ||
                                      character == ']' || character == '*' ||
                                      character == '\\' || character == '^' ||
                                      character == '$'))
                                grep_pattern_put('\\');

                        grep_pattern_put(character);
                }

                grep_pattern_any = true;
                from = at + 1;
        }

        grep_pattern[grep_pattern_length] = '\0';
}

static b32 text_grep()
{
        bool extended = false;
        bool fixed = false;
        bool icase = false;
        bool invert = false;
        bool numbered = false;
        bool counting = false;
        bool listing = false;
        bool quiet = false;
        bool no_names = false;
        bool with_names = false;
        bool quietly = false;
        bool whole_line = false;
        bool whole_word = false;
        bool have_pattern = false;
        b32 pattern_from = -1;

        text_begin("grep");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        if (!have_pattern && pattern_from < 0)
                        {
                                pattern_from = i;
                                have_pattern = true;
                                continue;
                        }

                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                        {
                                if (!have_pattern && pattern_from < 0)
                                {
                                        pattern_from = j;
                                        have_pattern = true;
                                        continue;
                                }

                                text_file_add(j);
                        }

                        break;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'e' || flag == 'f')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(2);
                                }

                                if (flag == 'e')
                                {
                                        grep_pattern_add(value, string_length(value),
                                                         fixed, extended);
                                }
                                else
                                {
                                        if (!text_open(value))
                                                return text_done(2);

                                        while (text_line_next())
                                                grep_pattern_add(text_line, text_line_length,
                                                                 fixed, extended);

                                        text_close();
                                }

                                have_pattern = true;
                                break;
                        }

                        switch (flag)
                        {
                        case 'E': extended = true; break;
                        case 'F': fixed = true; break;
                        case 'G': extended = false; break;
                        case 'i':
                        case 'y': icase = true; break;
                        case 'v': invert = true; break;
                        case 'n': numbered = true; break;
                        case 'c': counting = true; break;
                        case 'l': listing = true; break;
                        case 'q': quiet = true; break;
                        case 'h': no_names = true; break;
                        case 'H': with_names = true; break;
                        case 's': quietly = true; break;
                        case 'x': whole_line = true; break;
                        case 'w': whole_word = true; break;
                        default: break;
                        }
                }
        }

        if (pattern_from >= 0 && !grep_pattern_any)
        {
                string_address value = text_argument(pattern_from);

                grep_pattern_add(value, string_length(value), fixed, extended);
        }

        if (!have_pattern)
        {
                text_error(null, "no pattern given");
                return text_done(2);
        }

        // -x and -w are the pattern with something wrapped around it, which
        // is cheaper than a second answer from the machine.
        if (whole_line || whole_word)
        {
                p8 around[GREP_PATTERN_MAX];
                positive have = 0;
                string_address head = whole_line ? (extended ? "^(" : "^\\(")
                                                 : (extended ? "(^|\\W)(" : "\\(^\\|\\W\\)\\(");
                string_address tail = whole_line ? (extended ? ")$" : "\\)$")
                                                 : (extended ? ")(\\W|$)" : "\\)\\(\\W\\|$\\)");

                for (positive c = 0; head[c]; c++)
                        around[have++] = head[c];

                for (positive c = 0; c < grep_pattern_length; c++)
                        around[have++] = grep_pattern[c];

                for (positive c = 0; tail[c]; c++)
                        around[have++] = tail[c];

                around[have] = '\0';
                memory_copy(grep_pattern, around, have + 1);
                grep_pattern_length = have;
        }

        if (!regex_compile(grep_pattern, extended, icase, false))
        {
                text_error(null, "invalid regular expression");
                return text_done(2);
        }

        b32 inputs = text_files_count ? text_files_count : 1;
        bool names = (text_files_count > 1 || with_names) && !no_names;
        bool found_any = false;
        b32 trouble = 0;

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_files_count ? text_argument(text_files[i]) : null;
                positive matches = 0;
                positive number = 0;

                if (!text_open(name))
                {
                        trouble = 2;

                        if (quietly)
                                text_status = 0;

                        continue;
                }

                while (text_line_next())
                {
                        number++;

                        bool hit = regex_search(text_line, text_line_length, 0);

                        if (hit == invert)
                                continue;

                        matches++;
                        found_any = true;

                        if (quiet)
                        {
                                text_close();
                                return text_done(0);
                        }

                        if (counting || listing)
                                continue;

                        if (names)
                        {
                                text_put_string(name);
                                text_put_character(':');
                        }

                        if (numbered)
                        {
                                text_put_number(number, 1);
                                text_put_character(':');
                        }

                        text_put_line();

                        if (!text_line_ended)
                                text_put_character('\n');
                }

                text_close();

                if (listing)
                {
                        if (matches)
                        {
                                text_put_string(name ? name : (string_address)"(standard input)");
                                text_put_character('\n');
                        }

                        continue;
                }

                if (counting)
                {
                        if (names)
                        {
                                text_put_string(name);
                                text_put_character(':');
                        }

                        text_put_number(matches, 1);
                        text_put_character('\n');
                }
        }

        if (trouble)
                return text_done(2);

        return text_done(found_any ? 0 : 1);
}

/*
        sed

        The script is parsed once into a flat array of commands and the lines
        are then run through that array, which is why a block is stored as the
        index one past its closing brace rather than as a nested list: skipping
        a group whose address did not match is an assignment.

        What is here: addresses -- a line number, $, a regular expression, a
        range of any two of those, and ! -- and the commands s y p P d D q =
        n N h H g G x a i c, with { } around any of them. What is not: b, t
        and labels, which are a jump table this does not have.
*/
#define SED_COMMANDS_MAX 256
#define SED_PROGRAMS_MAX 32
#define SED_TEXT_MAX 16384
#define SED_MAPS_MAX 8
#define SED_SCRIPT_MAX 16384

enum
{
        SED_ADDRESS_NONE = 0,
        SED_ADDRESS_LINE,
        SED_ADDRESS_LAST,
        SED_ADDRESS_REGEX
};

typedef struct
{
        p8 kind;
        p8 first_type;
        p8 second_type;
        bool negate;
        bool active;
        positive first_line;
        positive second_line;
        b32 first_regex;
        b32 second_regex;
        b32 pattern;
        b32 map;
        b32 text;
        bool global;
        bool printing;
        positive which;
        b32 block_stop;
} sed_command;

static sed_command sed_commands[SED_COMMANDS_MAX];
static b32 sed_command_count;
static regex_program sed_programs[SED_PROGRAMS_MAX];
static b32 sed_program_count;
static p8 sed_maps[SED_MAPS_MAX][256];
static b32 sed_map_count;
static p8 sed_text[SED_TEXT_MAX];
static positive sed_text_used;
static p8 sed_script[SED_SCRIPT_MAX];
static positive sed_script_length;
static positive sed_at;
static bool sed_broken;

static p8 sed_space[TEXT_LINE_MAX];
static positive sed_space_length;
static p8 sed_work[TEXT_LINE_MAX];
static p8 sed_hold[TEXT_LINE_MAX];
static positive sed_hold_length;
static positive sed_number;
static bool sed_quiet;
static bool sed_last;
static bool sed_space_ended;

static fn sed_script_put(p8 character)
{
        if (sed_script_length < SED_SCRIPT_MAX - 1)
                sed_script[sed_script_length++] = character;
}

static fn sed_script_add(string_address text)
{
        if (sed_script_length)
                sed_script_put('\n');

        for (positive i = 0; text[i]; i++)
                sed_script_put(text[i]);

        sed_script[sed_script_length] = '\0';
}

static b32 sed_text_add(string_address from, positive length)
{
        b32 at = (b32)sed_text_used;

        if (sed_text_used + length + 1 >= SED_TEXT_MAX)
        {
                sed_broken = true;
                return 0;
        }

        memory_copy(sed_text + sed_text_used, from, length);
        sed_text_used += length;
        sed_text[sed_text_used++] = '\0';
        return at;
}

/*
        An empty regular expression is the last one that ran.

        //p and s//x/ both mean "the pattern from just now", which is why the
        answer here is an index of -1 rather than a compiled empty pattern
        that would match everywhere. A script that starts with one has nothing
        to refer back to, and GNU refuses it while reading the script rather
        than while running it.
*/
static b32 sed_recent = -1;
static bool sed_failed;

static bool sed_use_regex(b32 which)
{
        if (which >= 0)
        {
                sed_recent = which;
                return true;
        }

        if (sed_recent >= 0)
                return true;

        sed_failed = true;
        return false;
}

static b32 sed_compile_regex(string_address pattern, bool icase)
{
        // Refused when it runs rather than when it is read, because a script
        // whose input is empty never reaches the command that would complain.
        if (!pattern[0])
                return -1;

        if (sed_program_count >= SED_PROGRAMS_MAX)
        {
                sed_broken = true;
                return 0;
        }

        if (!regex_compile(pattern, false, icase, true))
        {
                sed_broken = true;
                return 0;
        }

        regex_keep(sed_programs + sed_program_count);
        return sed_program_count++;
}

static p8 sed_peek()
{
        return sed_at < sed_script_length ? sed_script[sed_at] : 0;
}

static fn sed_skip_blanks()
{
        while (sed_at < sed_script_length && text_blank(sed_script[sed_at]))
                sed_at++;
}

// Everything up to the next unescaped delimiter, with an escaped delimiter
// becoming the character itself -- s,a\,b,x, has a comma in its pattern.
static positive sed_take_until(p8 delimiter, p8 address_to into, positive room)
{
        positive have = 0;

        while (sed_at < sed_script_length && sed_script[sed_at] != delimiter)
        {
                if (sed_script[sed_at] == '\\' && sed_at + 1 < sed_script_length)
                {
                        if (sed_script[sed_at + 1] == delimiter)
                        {
                                if (have < room - 1)
                                        into[have++] = delimiter;

                                sed_at += 2;
                                continue;
                        }

                        if (have < room - 2)
                        {
                                into[have++] = '\\';
                                into[have++] = sed_script[sed_at + 1];
                        }

                        sed_at += 2;
                        continue;
                }

                if (have < room - 1)
                        into[have++] = sed_script[sed_at];

                sed_at++;
        }

        into[have] = '\0';

        if (sed_at < sed_script_length)
                sed_at++;
        else
                sed_broken = true;

        return have;
}

// y takes its two sets as bytes, not as a pattern, so the escapes in them
// have nobody else to expand them.
static positive sed_unescape(p8 address_to text, positive length)
{
        positive have = 0;

        for (positive i = 0; i < length; i++)
        {
                if (text[i] == '\\' && i + 1 < length)
                {
                        p8 next = text[++i];

                        text[have++] = next == 'n'   ? '\n'
                                       : next == 't' ? '\t'
                                       : next == 'r' ? '\r'
                                                     : next;
                        continue;
                }

                text[have++] = text[i];
        }

        text[have] = '\0';
        return have;
}

static bool sed_parse_address(p8 address_to type, positive address_to line, b32 address_to which)
{
        p8 character = sed_peek();

        if (text_digit(character))
        {
                positive value = 0;

                while (text_digit(sed_peek()))
                {
                        value = value * 10 + (positive)(sed_peek() - '0');
                        sed_at++;
                }

                address_to type = SED_ADDRESS_LINE;
                address_to line = value;
                return true;
        }

        if (character == '$')
        {
                sed_at++;
                address_to type = SED_ADDRESS_LAST;
                return true;
        }

        if (character == '/' || character == '\\')
        {
                p8 delimiter = '/';
                p8 pattern[1024];

                if (character == '\\')
                {
                        sed_at++;
                        delimiter = sed_peek();
                }

                sed_at++;
                sed_take_until(delimiter, pattern, sizeof(pattern));

                bool icase = false;

                while (sed_peek() == 'I' || sed_peek() == 'M')
                {
                        icase = icase || sed_peek() == 'I';
                        sed_at++;
                }

                address_to type = SED_ADDRESS_REGEX;
                address_to which = sed_compile_regex(pattern, icase);
                return true;
        }

        return false;
}

static fn sed_parse()
{
        b32 open_blocks[32];
        b32 open_count = 0;

        sed_at = 0;

        while (sed_at < sed_script_length && !sed_broken)
        {
                sed_skip_blanks();

                p8 character = sed_peek();

                if (character == '\n' || character == ';')
                {
                        sed_at++;
                        continue;
                }

                if (!character)
                        break;

                if (character == '#')
                {
                        while (sed_at < sed_script_length && sed_script[sed_at] != '\n')
                                sed_at++;

                        continue;
                }

                if (sed_command_count >= SED_COMMANDS_MAX)
                {
                        sed_broken = true;
                        return;
                }

                sed_command address_to command = sed_commands + sed_command_count;

                command->kind = 0;
                command->first_type = SED_ADDRESS_NONE;
                command->second_type = SED_ADDRESS_NONE;
                command->negate = false;
                command->active = false;
                command->which = 1;

                if (sed_parse_address(address_of command->first_type,
                                      address_of command->first_line,
                                      address_of command->first_regex))
                {
                        sed_skip_blanks();

                        if (sed_peek() == ',')
                        {
                                sed_at++;
                                sed_skip_blanks();

                                if (!sed_parse_address(address_of command->second_type,
                                                       address_of command->second_line,
                                                       address_of command->second_regex))
                                {
                                        sed_broken = true;
                                        return;
                                }
                        }
                }

                sed_skip_blanks();

                if (sed_peek() == '!')
                {
                        command->negate = true;
                        sed_at++;
                        sed_skip_blanks();

                        if (sed_peek() == '!')
                        {
                                sed_broken = true;
                                return;
                        }
                }

                p8 kind = sed_peek();

                sed_at++;
                command->kind = kind;

                if (kind == '{')
                {
                        if (open_count < 32)
                                open_blocks[open_count++] = sed_command_count;

                        sed_command_count++;
                        continue;
                }

                if (kind == '}')
                {
                        if (open_count)
                                sed_commands[open_blocks[--open_count]].block_stop =
                                    sed_command_count + 1;

                        sed_command_count++;
                        continue;
                }

                if (kind == 's')
                {
                        p8 delimiter = sed_peek();
                        p8 pattern[1024];
                        p8 replacement[1024];

                        sed_at++;
                        sed_take_until(delimiter, pattern, sizeof(pattern));

                        positive have = sed_take_until(delimiter, replacement, sizeof(replacement));
                        bool icase = false;

                        command->global = false;
                        command->printing = false;
                        command->which = 0;

                        for (;;)
                        {
                                p8 flag = sed_peek();

                                if (flag == 'g')
                                        command->global = true;
                                else if (flag == 'p')
                                        command->printing = true;
                                else if (flag == 'i' || flag == 'I')
                                        icase = true;
                                else if (flag == 'm' || flag == 'M')
                                        (void)flag;
                                else if (text_digit(flag))
                                {
                                        positive value = 0;

                                        while (text_digit(sed_peek()))
                                        {
                                                value = value * 10 + (positive)(sed_peek() - '0');
                                                sed_at++;
                                        }

                                        command->which = value;
                                        continue;
                                }
                                else
                                        break;

                                sed_at++;
                        }

                        if (!command->which)
                                command->which = 1;

                        command->pattern = sed_compile_regex(pattern, icase);
                        command->text = sed_text_add(replacement, have);
                        sed_command_count++;
                        continue;
                }

                if (kind == 'y')
                {
                        p8 delimiter = sed_peek();
                        p8 from[512];
                        p8 to[512];

                        sed_at++;

                        positive one = sed_unescape(
                            from, sed_take_until(delimiter, from, sizeof(from)));
                        positive two = sed_unescape(
                            to, sed_take_until(delimiter, to, sizeof(to)));

                        if (sed_map_count >= SED_MAPS_MAX)
                        {
                                sed_broken = true;
                                return;
                        }

                        b32 map = sed_map_count++;

                        for (b32 c = 0; c < 256; c++)
                                sed_maps[map][c] = (p8)c;

                        for (positive c = 0; c < one && c < two; c++)
                                sed_maps[map][from[c]] = to[c];

                        command->map = map;
                        sed_command_count++;
                        continue;
                }

                if (kind == 'a' || kind == 'i' || kind == 'c')
                {
                        p8 body[1024];
                        positive have = 0;

                        if (sed_peek() == '\\')
                                sed_at++;

                        if (sed_peek() == '\n')
                                sed_at++;

                        sed_skip_blanks();

                        while (sed_at < sed_script_length && sed_script[sed_at] != '\n')
                        {
                                if (sed_script[sed_at] == '\\' && sed_at + 1 < sed_script_length)
                                {
                                        sed_at++;

                                        if (sed_script[sed_at] == 'n')
                                        {
                                                body[have++] = '\n';
                                                sed_at++;
                                                continue;
                                        }
                                }

                                if (have < sizeof(body) - 1)
                                        body[have++] = sed_script[sed_at];

                                sed_at++;
                        }

                        body[have] = '\0';
                        command->text = sed_text_add(body, have);
                        sed_command_count++;
                        continue;
                }

                if (kind == 'q')
                {
                        sed_skip_blanks();

                        positive value = 0;

                        while (text_digit(sed_peek()))
                        {
                                value = value * 10 + (positive)(sed_peek() - '0');
                                sed_at++;
                        }

                        command->which = value;
                        sed_command_count++;
                        continue;
                }

                if (kind == 'p' || kind == 'P' || kind == 'd' || kind == 'D' ||
                    kind == '=' || kind == 'n' || kind == 'N' || kind == 'h' ||
                    kind == 'H' || kind == 'g' || kind == 'G' || kind == 'x')
                {
                        sed_command_count++;
                        continue;
                }

                sed_broken = true;
                return;
        }
}

static bool sed_address_matches(p8 type, positive line, b32 which)
{
        if (type == SED_ADDRESS_LINE)
                return sed_number == line;

        if (type == SED_ADDRESS_LAST)
                return sed_last;

        if (type == SED_ADDRESS_REGEX)
        {
                if (!sed_use_regex(which))
                        return false;

                regex_select(sed_programs + sed_recent);
                return regex_search(sed_space, sed_space_length, 0);
        }

        return false;
}

static bool sed_selects(sed_command address_to command)
{
        bool answer;

        if (command->first_type == SED_ADDRESS_NONE)
        {
                answer = true;
        }
        else if (command->second_type == SED_ADDRESS_NONE)
        {
                answer = sed_address_matches(command->first_type, command->first_line,
                                             command->first_regex);
        }
        else if (!command->active)
        {
                answer = sed_address_matches(command->first_type, command->first_line,
                                             command->first_regex);

                if (answer)
                {
                        command->active = true;

                        // A range whose end is a line already passed is one
                        // line long, which is the only way the end can be
                        // decided without seeing another line.
                        if (command->second_type == SED_ADDRESS_LINE &&
                            command->second_line <= sed_number)
                                command->active = false;
                }
        }
        else
        {
                answer = true;

                if (command->second_type == SED_ADDRESS_LINE)
                {
                        if (sed_number >= command->second_line)
                                command->active = false;
                }
                else if (sed_address_matches(command->second_type, command->second_line,
                                             command->second_regex))
                {
                        command->active = false;
                }
        }

        return command->negate ? !answer : answer;
}

static fn sed_put_space()
{
        text_put(sed_space, sed_space_length);
        text_put_character('\n');
}

static bool sed_substitute(sed_command address_to command)
{
        string_address replacement = sed_text + command->text;
        positive at = 0;
        positive have = 0;
        positive seen = 0;
        positive after_last = TEXT_UNSET;
        bool changed = false;

        if (!sed_use_regex(command->pattern))
                return false;

        regex_select(sed_programs + sed_recent);

        while (at <= sed_space_length)
        {
                if (!regex_search_longest(sed_space, sed_space_length, at))
                        break;

                positive from = regex_slots[0];
                positive to = regex_slots[1];

                memory_copy(sed_work + have, sed_space + at, from - at);
                have += from - at;

                // An empty match sitting where the last one ended is not a
                // second match: s/a*/X/g over "aaa" is one X, not two.
                if (from == to && from == after_last)
                {
                        if (from >= sed_space_length)
                        {
                                at = from;
                                break;
                        }

                        sed_work[have++] = sed_space[from];
                        at = from + 1;
                        continue;
                }

                seen++;

                bool now = command->global ? seen >= command->which : seen == command->which;

                if (now)
                {
                        for (positive c = 0; replacement[c]; c++)
                        {
                                p8 character = replacement[c];
                                positive copy_from = TEXT_UNSET;
                                positive copy_to = TEXT_UNSET;

                                if (character == '&')
                                {
                                        copy_from = from;
                                        copy_to = to;
                                }
                                else if (character == '\\' && replacement[c + 1])
                                {
                                        p8 next = replacement[++c];

                                        if (next >= '0' && next <= '9')
                                        {
                                                copy_from = regex_slots[(next - '0') * 2];
                                                copy_to = regex_slots[(next - '0') * 2 + 1];

                                                if (copy_from == TEXT_UNSET ||
                                                    copy_to == TEXT_UNSET)
                                                        continue;
                                        }
                                        else
                                        {
                                                sed_work[have++] = next == 'n'   ? '\n'
                                                                   : next == 't' ? '\t'
                                                                   : next == 'r' ? '\r'
                                                                                 : next;
                                                continue;
                                        }
                                }
                                else
                                {
                                        sed_work[have++] = character;
                                        continue;
                                }

                                memory_copy(sed_work + have, sed_space + copy_from,
                                            copy_to - copy_from);
                                have += copy_to - copy_from;
                        }

                        changed = true;
                }
                else
                {
                        memory_copy(sed_work + have, sed_space + from, to - from);
                        have += to - from;
                }

                // An empty match would sit where it is forever, so the
                // character under it moves across and the search goes on past
                // it. s/x*/-/g on "abc" is what this is for.
                after_last = to;

                if (to == from)
                {
                        if (from < sed_space_length)
                                sed_work[have++] = sed_space[from];

                        at = from + 1;
                }
                else
                {
                        at = to;
                }

                if (!command->global && seen >= command->which)
                        break;
        }

        if (!changed)
                return false;

        if (at < sed_space_length)
        {
                memory_copy(sed_work + have, sed_space + at, sed_space_length - at);
                have += sed_space_length - at;
        }

        memory_copy(sed_space, sed_work, have);
        sed_space_length = have;
        return true;
}

static b32 text_sed()
{
        bool have_script = false;
        b32 leaving = -1;

        text_begin("sed");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        if (!have_script)
                        {
                                sed_script_add(argument);
                                have_script = true;
                                continue;
                        }

                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                        {
                                if (!have_script)
                                {
                                        sed_script_add(text_argument(j));
                                        have_script = true;
                                        continue;
                                }

                                text_file_add(j);
                        }

                        break;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'n')
                        {
                                sed_quiet = true;
                                continue;
                        }

                        if (flag == 'r' || flag == 'E' || flag == 's' || flag == 'u')
                                continue;

                        if (flag == 'e' || flag == 'f')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(1);
                                }

                                if (flag == 'e')
                                {
                                        sed_script_add(value);
                                }
                                else
                                {
                                        if (!text_open(value))
                                                return text_done(1);

                                        while (text_line_next())
                                        {
                                                text_line[text_line_length] = '\0';
                                                sed_script_add(text_line);
                                        }

                                        text_close();
                                }

                                have_script = true;
                                break;
                        }
                }
        }

        if (!have_script)
        {
                text_error(null, "no script");
                return text_done(1);
        }

        sed_parse();

        if (sed_broken)
        {
                text_error(null, "unsupported or invalid script");
                return text_done(1);
        }

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs && leaving < 0; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        memory_copy(sed_space, text_line, text_line_length);
                        sed_space_length = text_line_length;
                        sed_space_ended = text_line_ended;
                        sed_number++;
                        sed_last = !text_fill() && i == inputs - 1;

                        bool dropped = false;
                        b32 pc = 0;
                        b32 appended = -1;

                        while (pc < sed_command_count)
                        {
                                sed_command address_to command = sed_commands + pc;

                                if (command->kind == '}')
                                {
                                        pc++;
                                        continue;
                                }

                                if (!sed_selects(command))
                                {
                                        pc = command->kind == '{' ? command->block_stop : pc + 1;
                                        continue;
                                }

                                p8 kind = command->kind;

                                pc++;

                                if (kind == '{')
                                        continue;

                                if (kind == 's')
                                {
                                        if (sed_substitute(command) && command->printing)
                                                sed_put_space();

                                        continue;
                                }

                                if (kind == 'y')
                                {
                                        for (positive c = 0; c < sed_space_length; c++)
                                                sed_space[c] = sed_maps[command->map][sed_space[c]];

                                        continue;
                                }

                                if (kind == 'p')
                                {
                                        sed_put_space();
                                        continue;
                                }

                                if (kind == 'P')
                                {
                                        positive stop = 0;

                                        while (stop < sed_space_length && sed_space[stop] != '\n')
                                                stop++;

                                        text_put(sed_space, stop);
                                        text_put_character('\n');
                                        continue;
                                }

                                if (kind == '=')
                                {
                                        text_put_number(sed_number, 1);
                                        text_put_character('\n');
                                        continue;
                                }

                                if (kind == 'd')
                                {
                                        dropped = true;
                                        break;
                                }

                                if (kind == 'D')
                                {
                                        positive stop = 0;

                                        while (stop < sed_space_length && sed_space[stop] != '\n')
                                                stop++;

                                        if (stop >= sed_space_length)
                                        {
                                                dropped = true;
                                                break;
                                        }

                                        memory_copy(sed_work, sed_space + stop + 1,
                                                    sed_space_length - stop - 1);
                                        sed_space_length -= stop + 1;
                                        memory_copy(sed_space, sed_work, sed_space_length);
                                        pc = 0;
                                        continue;
                                }

                                if (kind == 'q')
                                {
                                        leaving = (b32)command->which;
                                        break;
                                }

                                if (kind == 'n')
                                {
                                        if (!sed_quiet)
                                                sed_put_space();

                                        if (!text_line_next())
                                        {
                                                dropped = true;
                                                break;
                                        }

                                        memory_copy(sed_space, text_line, text_line_length);
                                        sed_space_length = text_line_length;
                                        sed_space_ended = text_line_ended;
                                        sed_number++;
                                        sed_last = !text_fill() && i == inputs - 1;
                                        continue;
                                }

                                if (kind == 'N')
                                {
                                        if (!text_line_next())
                                        {
                                                sed_last = true;
                                                break;
                                        }

                                        sed_space[sed_space_length++] = '\n';
                                        memory_copy(sed_space + sed_space_length, text_line,
                                                    text_line_length);
                                        sed_space_length += text_line_length;
                                        sed_space_ended = text_line_ended;
                                        sed_number++;
                                        sed_last = !text_fill() && i == inputs - 1;
                                        continue;
                                }

                                if (kind == 'h' || kind == 'H')
                                {
                                        if (kind == 'H')
                                                sed_hold[sed_hold_length++] = '\n';
                                        else
                                                sed_hold_length = 0;

                                        memory_copy(sed_hold + sed_hold_length, sed_space,
                                                    sed_space_length);
                                        sed_hold_length += sed_space_length;
                                        continue;
                                }

                                if (kind == 'g' || kind == 'G')
                                {
                                        if (kind == 'g')
                                                sed_space_length = 0;
                                        else
                                                sed_space[sed_space_length++] = '\n';

                                        memory_copy(sed_space + sed_space_length, sed_hold,
                                                    sed_hold_length);
                                        sed_space_length += sed_hold_length;
                                        continue;
                                }

                                if (kind == 'x')
                                {
                                        memory_copy(sed_work, sed_space, sed_space_length);
                                        memory_copy(sed_space, sed_hold, sed_hold_length);
                                        memory_copy(sed_hold, sed_work, sed_space_length);

                                        positive swap = sed_space_length;

                                        sed_space_length = sed_hold_length;
                                        sed_hold_length = swap;
                                        continue;
                                }

                                if (kind == 'i')
                                {
                                        text_put_string(sed_text + command->text);
                                        text_put_character('\n');
                                        continue;
                                }

                                if (kind == 'a')
                                {
                                        appended = command->text;
                                        continue;
                                }

                                if (kind == 'c')
                                {
                                        // A range prints its replacement once,
                                        // when the range closes.
                                        if (command->second_type == SED_ADDRESS_NONE ||
                                            !command->active)
                                        {
                                                text_put_string(sed_text + command->text);
                                                text_put_character('\n');
                                        }

                                        dropped = true;
                                        break;
                                }
                        }

                        if (sed_failed)
                                break;

                        if (!sed_quiet && !dropped)
                        {
                                text_put(sed_space, sed_space_length);

                                if (sed_space_ended || leaving >= 0)
                                        text_put_character('\n');
                        }

                        if (appended >= 0)
                        {
                                text_put_string(sed_text + appended);
                                text_put_character('\n');
                        }

                        if (leaving >= 0 || sed_failed)
                                break;
                }

                text_close();

                if (sed_failed)
                        break;
        }

        if (sed_failed)
        {
                text_error(null, "no previous regular expression");
                return text_done(1);
        }

        return text_done(leaving > 0 ? leaving : text_status);
}

/*
        sort

        A merge sort over an array of indices, because it is stable and
        because a comparison here reads two slices of the arena rather than
        two pointers -- moving the lines themselves would move a megabyte to
        answer a question about twenty bytes.

        The key is where all the difficulty is. Without -t a field is a run of
        blanks followed by a run of non-blanks, and the blanks belong to the
        field that follows them, which is why -k2 on "  a  b" starts at the
        two spaces before b and not at b. That is what -b is for, and what
        makes -k2 and -k2b different keys.
*/
#define SORT_KEYS_MAX 8

typedef struct
{
        positive first_field;
        positive first_char;
        positive second_field;
        positive second_char;
        bool numeric;
        bool reverse;
        bool skip_blanks_first;
        bool skip_blanks_second;
        bool fold;
        bool given;
} sort_key;

static sort_key sort_keys[SORT_KEYS_MAX];
static b32 sort_key_count;
static bool sort_numeric;
static bool sort_reverse;
static bool sort_fold;
static bool sort_skip_blanks;
static bool sort_unique;
static bool sort_stable;
static bool sort_have_separator;
static p8 sort_separator;

static positive sort_field_start(p8 address_to at, positive length, positive field)
{
        positive scan = 0;

        if (sort_have_separator)
        {
                for (positive i = 1; i < field && scan < length; i++)
                {
                        while (scan < length && at[scan] != sort_separator)
                                scan++;

                        if (scan < length)
                                scan++;
                }

                return scan;
        }

        for (positive i = 1; i < field && scan < length; i++)
        {
                while (scan < length && text_blank(at[scan]))
                        scan++;

                while (scan < length && !text_blank(at[scan]))
                        scan++;
        }

        return scan;
}

static positive sort_field_stop(p8 address_to at, positive length, positive field)
{
        positive scan = 0;

        if (sort_have_separator)
        {
                for (positive i = 0; i < field && scan < length; i++)
                {
                        while (scan < length && at[scan] != sort_separator)
                                scan++;

                        if (i + 1 < field && scan < length)
                                scan++;
                }

                return scan;
        }

        for (positive i = 0; i < field && scan < length; i++)
        {
                while (scan < length && text_blank(at[scan]))
                        scan++;

                while (scan < length && !text_blank(at[scan]))
                        scan++;
        }

        return scan;
}

static fn sort_key_span(sort_key address_to key, p8 address_to at, positive length,
                        positive address_to from, positive address_to to)
{
        positive begin = 0;
        positive finish = length;

        if (key->first_field)
        {
                begin = sort_field_start(at, length, key->first_field);

                if (key->skip_blanks_first)
                        while (begin < length && text_blank(at[begin]))
                                begin++;

                if (key->first_char > 1)
                {
                        positive step = key->first_char - 1;
                        positive limit = sort_field_stop(at, length, key->first_field);

                        begin += step;

                        if (begin > limit)
                                begin = limit;
                }
        }

        if (key->second_field)
        {
                if (key->second_char)
                {
                        finish = sort_field_start(at, length, key->second_field);

                        if (key->skip_blanks_second)
                                while (finish < length && text_blank(at[finish]))
                                        finish++;

                        finish += key->second_char;

                        positive limit = sort_field_stop(at, length, key->second_field);

                        if (finish > limit)
                                finish = limit;
                }
                else
                {
                        finish = sort_field_stop(at, length, key->second_field);
                }
        }

        if (begin > length)
                begin = length;

        if (finish > length)
                finish = length;

        if (finish < begin)
                finish = begin;

        address_to from = begin;
        address_to to = finish;
}

// A number without a number parser: sign, then integer digits with the
// leading zeros dropped, then the fraction, compared as text.
static bipolar sort_compare_number(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        positive at_a = 0, at_b = 0;
        bool minus_a = false, minus_b = false;

        while (at_a < la && text_blank(a[at_a]))
                at_a++;

        while (at_b < lb && text_blank(b[at_b]))
                at_b++;

        // A leading plus is not a sign here: GNU sort reads +7 as no number
        // at all, which sorts it with the zeros rather than after the sixes.
        if (at_a < la && a[at_a] == '-')
        {
                minus_a = true;
                at_a++;
        }

        if (at_b < lb && b[at_b] == '-')
        {
                minus_b = true;
                at_b++;
        }

        positive int_a = at_a, int_b = at_b;

        while (at_a < la && text_digit(a[at_a]))
                at_a++;

        while (at_b < lb && text_digit(b[at_b]))
                at_b++;

        positive int_a_stop = at_a, int_b_stop = at_b;
        positive frac_a = at_a, frac_a_stop = at_a;
        positive frac_b = at_b, frac_b_stop = at_b;

        if (at_a < la && a[at_a] == '.')
        {
                frac_a = ++at_a;

                while (at_a < la && text_digit(a[at_a]))
                        at_a++;

                frac_a_stop = at_a;
        }

        if (at_b < lb && b[at_b] == '.')
        {
                frac_b = ++at_b;

                while (at_b < lb && text_digit(b[at_b]))
                        at_b++;

                frac_b_stop = at_b;
        }

        while (int_a < int_a_stop && a[int_a] == '0')
                int_a++;

        while (int_b < int_b_stop && b[int_b] == '0')
                int_b++;

        bool zero_a = int_a == int_a_stop;
        bool zero_b = int_b == int_b_stop;

        for (positive i = frac_a; zero_a && i < frac_a_stop; i++)
                if (a[i] != '0')
                        zero_a = false;

        for (positive i = frac_b; zero_b && i < frac_b_stop; i++)
                if (b[i] != '0')
                        zero_b = false;

        if (zero_a && zero_b)
                return 0;

        if (minus_a != minus_b)
                return minus_a ? -1 : 1;

        bipolar sign = minus_a ? -1 : 1;
        positive digits_a = int_a_stop - int_a;
        positive digits_b = int_b_stop - int_b;

        if (digits_a != digits_b)
                return digits_a < digits_b ? -sign : sign;

        for (positive i = 0; i < digits_a; i++)
                if (a[int_a + i] != b[int_b + i])
                        return a[int_a + i] < b[int_b + i] ? -sign : sign;

        positive frac = 0;

        for (;;)
        {
                p8 one = frac_a + frac < frac_a_stop ? a[frac_a + frac] : '0';
                p8 two = frac_b + frac < frac_b_stop ? b[frac_b + frac] : '0';

                if (frac_a + frac >= frac_a_stop && frac_b + frac >= frac_b_stop)
                        return 0;

                if (one != two)
                        return one < two ? -sign : sign;

                frac++;
        }
}

static bipolar sort_compare_bytes(p8 address_to a, positive la, p8 address_to b, positive lb,
                                  bool fold)
{
        positive shared = la < lb ? la : lb;

        for (positive i = 0; i < shared; i++)
        {
                p8 one = a[i];
                p8 two = b[i];

                if (fold)
                {
                        one = one >= 'a' && one <= 'z' ? (p8)(one - 32) : one;
                        two = two >= 'a' && two <= 'z' ? (p8)(two - 32) : two;
                }

                if (one != two)
                        return one < two ? -1 : 1;
        }

        if (la == lb)
                return 0;

        return la < lb ? -1 : 1;
}

/*
        The first key's bounds, found once per line rather than once per
        comparison.

        Finding where -k3 starts means walking the line counting fields, and a
        merge sort asks about a line some twenty times. Caching only the first
        key is enough: the second is consulted only where the first ties.
*/
static positive address_to sort_span_from;
static positive address_to sort_span_to;

static bipolar sort_compare_keys(positive left, positive right)
{
        text_slice address_to a = text_lines + left;
        text_slice address_to b = text_lines + right;

        for (b32 i = 0; i < sort_key_count; i++)
        {
                sort_key address_to key = sort_keys + i;
                positive from_a, to_a, from_b, to_b;

                if (!i && sort_span_from)
                {
                        from_a = sort_span_from[left];
                        to_a = sort_span_to[left];
                        from_b = sort_span_from[right];
                        to_b = sort_span_to[right];
                }
                else
                {
                        sort_key_span(key, a->at, a->length, address_of from_a,
                                      address_of to_a);
                        sort_key_span(key, b->at, b->length, address_of from_b,
                                      address_of to_b);
                }

                bipolar answer = key->numeric
                                     ? sort_compare_number(a->at + from_a, to_a - from_a,
                                                           b->at + from_b, to_b - from_b)
                                     : sort_compare_bytes(a->at + from_a, to_a - from_a,
                                                          b->at + from_b, to_b - from_b,
                                                          key->fold);

                if (answer)
                        return key->reverse ? -answer : answer;
        }

        if (!sort_key_count)
        {
                positive from_a = 0, from_b = 0;

                if (sort_skip_blanks)
                {
                        while (from_a < a->length && text_blank(a->at[from_a]))
                                from_a++;

                        while (from_b < b->length && text_blank(b->at[from_b]))
                                from_b++;
                }

                bipolar answer = sort_numeric
                                     ? sort_compare_number(a->at + from_a, a->length - from_a,
                                                           b->at + from_b, b->length - from_b)
                                     : sort_compare_bytes(a->at + from_a, a->length - from_a,
                                                          b->at + from_b, b->length - from_b,
                                                          sort_fold);

                if (answer)
                        return sort_reverse ? -answer : answer;
        }

        return 0;
}

// The last resort, which is the whole line compared as bytes when every key
// said the two were the same. -u drops what the keys called equal, so it
// stops before this.
static bipolar sort_compare(positive left, positive right)
{
        bipolar answer = sort_compare_keys(left, right);

        if (answer)
                return answer;

        if (sort_unique || sort_stable)
                return 0;

        text_slice address_to a = text_lines + left;
        text_slice address_to b = text_lines + right;

        answer = sort_compare_bytes(a->at, a->length, b->at, b->length, false);
        return sort_reverse ? -answer : answer;
}

static positive address_to sort_order;
static positive address_to sort_spare;

static fn sort_merge(positive from, positive middle, positive to)
{
        positive left = from;
        positive right = middle;
        positive at = from;

        while (left < middle && right < to)
        {
                if (sort_compare(sort_order[right], sort_order[left]) < 0)
                        sort_spare[at++] = sort_order[right++];
                else
                        sort_spare[at++] = sort_order[left++];
        }

        while (left < middle)
                sort_spare[at++] = sort_order[left++];

        while (right < to)
                sort_spare[at++] = sort_order[right++];

        for (positive i = from; i < to; i++)
                sort_order[i] = sort_spare[i];
}

static fn sort_run(positive from, positive to)
{
        if (to - from < 2)
                return;

        positive middle = from + (to - from) / 2;

        sort_run(from, middle);
        sort_run(middle, to);
        sort_merge(from, middle, to);
}

static bool sort_parse_key(string_address spec)
{
        if (sort_key_count >= SORT_KEYS_MAX)
                return false;

        sort_key address_to key = sort_keys + sort_key_count;
        positive at = 0;

        key->first_field = 0;
        key->first_char = 0;
        key->second_field = 0;
        key->second_char = 0;
        key->numeric = sort_numeric;
        key->reverse = sort_reverse;
        key->fold = sort_fold;
        key->skip_blanks_first = sort_skip_blanks;
        key->skip_blanks_second = sort_skip_blanks;

        while (text_digit(spec[at]))
                key->first_field = key->first_field * 10 + (positive)(spec[at++] - '0');

        if (!key->first_field)
                return false;

        if (spec[at] == '.')
        {
                at++;

                while (text_digit(spec[at]))
                        key->first_char = key->first_char * 10 + (positive)(spec[at++] - '0');
        }

        while (spec[at] && spec[at] != ',')
        {
                if (spec[at] == 'n')
                        key->numeric = true;
                else if (spec[at] == 'r')
                        key->reverse = true;
                else if (spec[at] == 'f')
                        key->fold = true;
                else if (spec[at] == 'b')
                        key->skip_blanks_first = true;

                at++;
        }

        if (spec[at] == ',')
        {
                at++;

                while (text_digit(spec[at]))
                        key->second_field = key->second_field * 10 + (positive)(spec[at++] - '0');

                if (spec[at] == '.')
                {
                        at++;

                        while (text_digit(spec[at]))
                                key->second_char =
                                    key->second_char * 10 + (positive)(spec[at++] - '0');
                }

                while (spec[at])
                {
                        if (spec[at] == 'n')
                                key->numeric = true;
                        else if (spec[at] == 'r')
                                key->reverse = true;
                        else if (spec[at] == 'f')
                                key->fold = true;
                        else if (spec[at] == 'b')
                                key->skip_blanks_second = true;

                        at++;
                }
        }

        sort_key_count++;
        return true;
}

static b32 text_sort()
{
        text_begin("sort");

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] != '-' || !argument[1])
                {
                        text_file_add(i);
                        continue;
                }

                if (argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                for (positive c = 1; argument[c]; c++)
                {
                        p8 flag = argument[c];

                        if (flag == 'k' || flag == 't' || flag == 'o')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(2);
                                }

                                if (flag == 'k')
                                {
                                        if (!sort_parse_key(value))
                                        {
                                                text_error(null, "invalid key");
                                                return text_done(2);
                                        }
                                }
                                else if (flag == 't')
                                {
                                        sort_have_separator = true;
                                        sort_separator = value[0] == '\\' && value[1] == 't'
                                                             ? '\t'
                                                             : value[0];
                                }

                                break;
                        }

                        switch (flag)
                        {
                        case 'n': sort_numeric = true; break;
                        case 'r': sort_reverse = true; break;
                        case 'f': sort_fold = true; break;
                        case 'b': sort_skip_blanks = true; break;
                        case 'u': sort_unique = true; break;
                        case 's': sort_stable = true; break;
                        default: break;
                        }
                }
        }

        // The global flags are the default for every key, and -n after -k on
        // the command line still has to reach the key in front of it.
        for (b32 i = 0; i < sort_key_count; i++)
        {
                sort_keys[i].numeric = sort_keys[i].numeric || sort_numeric;
                sort_keys[i].reverse = sort_keys[i].reverse || sort_reverse;
                sort_keys[i].fold = sort_keys[i].fold || sort_fold;
        }

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                if (!text_lines_gather())
                        return text_done(2);

                text_close();
        }

        sort_order = (positive address_to)text_arena_take(
            (text_lines_count + 1) * sizeof(positive));
        sort_spare = (positive address_to)text_arena_take(
            (text_lines_count + 1) * sizeof(positive));

        if (!sort_order || !sort_spare)
                return text_done(2);

        for (positive i = 0; i < text_lines_count; i++)
                sort_order[i] = i;

        if (sort_key_count)
        {
                sort_span_from = (positive address_to)text_arena_take(
                    (text_lines_count + 1) * sizeof(positive));
                sort_span_to = (positive address_to)text_arena_take(
                    (text_lines_count + 1) * sizeof(positive));

                if (!sort_span_from || !sort_span_to)
                        return text_done(2);

                for (positive i = 0; i < text_lines_count; i++)
                        sort_key_span(sort_keys, text_lines[i].at, text_lines[i].length,
                                      sort_span_from + i, sort_span_to + i);
        }

        sort_run(0, text_lines_count);

        for (positive i = 0; i < text_lines_count; i++)
        {
                text_slice address_to line = text_lines + sort_order[i];

                if (sort_unique && i &&
                    !sort_compare_keys(sort_order[i - 1], sort_order[i]))
                        continue;

                text_put(line->at, line->length);
                text_put_character('\n');
        }

        return text_done(text_status);
}
