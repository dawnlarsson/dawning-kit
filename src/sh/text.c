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

/*
        What ends a line, which -z says is a NUL and everything else leaves
        alone.

        One byte rather than a flag threaded through thirteen tools, and it is
        set after the arguments are read rather than where -z is seen: grep -f
        and sed -f both read their file with the same reader, and GNU splits
        those on a newline whatever -z says. Assigning it in the switch would
        make grep -z -f and grep -f -z two different commands.
*/
static p8 text_delimiter = '\n';

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
                /*
                        Where the line ends, found by the library rather than
                        here. This is the loop under every tool in this file
                        that works a line at a time, so it is the one worth
                        handing over: memory_first_of compares thirty two bytes
                        at once where this compared one, and it is the same
                        answer either way.
                */
                string_address found = memory_first_of(at, text_delimiter, left);
                positive take = found ? (positive)(found - at) : left;

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
                text_put_character(text_delimiter);
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

#define TEXT_ARGUMENTS_MAX 1024

static string_address text_arguments[TEXT_ARGUMENTS_MAX];
static p8 text_spellings[TEXT_ARGUMENTS_MAX][4];
static bool text_arguments_own;

static string_address text_argument(b32 which)
{
        if (text_arguments_own)
                return which >= 0 && which < text_argument_count ? text_arguments[which]
                                                                 : null;

        return program_argument(which);
}

static fn text_begin(string_address name)
{
        text_name = name;
        text_argument_count = program_argument_count();
        text_arguments_own = false;
}

/*
        The long spellings.

        Every tool below was written to its short flags, so a long name is
        turned into the letter it stands for before the parser ever sees it:
        --ignore-case arrives at grep's switch as -i. The table belongs to
        the tool and sits beside that switch, which is the only way the two
        stay in step.

        A name with no short spelling at all -- cut's --complement, grep's
        --label -- is given a byte no keyboard sends, so it reaches the same
        switch without also making "cut -\002" mean something.

        GNU takes any unambiguous abbreviation of a long name. This wants the
        whole name. An abbreviation table is a second copy of the first, and
        the once it disagrees is the once somebody typed --no and meant
        --no-messages.
*/
enum
{
        TEXT_LONG_ALONE = 0,
        TEXT_LONG_NEEDS = 1,
        TEXT_LONG_MAYBE = 2
};

typedef struct
{
        string_address name;
        p8 letter;
        p8 wants;
        // A name whose argument is optional means two different things
        // depending on whether one came: --check is -c and --check=quiet is
        // -C. That second letter goes here, and takes the value with it.
        p8 valued;
} text_long;

static bool text_long_match(string_address argument, string_address name,
                            string_address address_to value)
{
        positive c = 0;

        for (; name[c]; c++)
                if (argument[c + 2] != name[c])
                        return false;

        if (!argument[c + 2])
        {
                address_to value = null;
                return true;
        }

        if (argument[c + 2] != '=')
                return false;

        address_to value = argument + c + 3;
        return true;
}

// The rewritten vector is handed back through text_argument, so the loops in
// every tool keep reading arguments the one way they already do.
static bool text_expand_long(text_long address_to table)
{
        b32 count = program_argument_count();
        b32 out = 0;
        b32 spelled = 0;
        bool any = false;

        for (b32 i = 0; i < count; i++)
        {
                string_address argument = program_argument(i);

                if (i == 0 || argument[0] != '-' || argument[1] != '-' || !argument[2])
                {
                        if (out >= TEXT_ARGUMENTS_MAX)
                        {
                                text_error(null, "too many arguments");
                                return false;
                        }

                        text_arguments[out++] = argument;

                        // Everything after a bare -- is an operand, even when
                        // it is spelled like a long option.
                        if (i && argument[0] == '-' && argument[1] == '-' && !argument[2])
                        {
                                for (b32 j = i + 1; j < count; j++)
                                {
                                        if (out >= TEXT_ARGUMENTS_MAX)
                                        {
                                                text_error(null, "too many arguments");
                                                return false;
                                        }

                                        text_arguments[out++] = program_argument(j);
                                }

                                i = count;
                        }

                        continue;
                }

                b32 which = -1;
                string_address value = null;

                for (b32 t = 0; table[t].name; t++)
                        if (text_long_match(argument, table[t].name, address_of value))
                        {
                                which = t;
                                break;
                        }

                if (which < 0)
                {
                        text_error(argument, "unrecognized option");
                        return false;
                }

                if (value && table[which].wants == TEXT_LONG_ALONE)
                {
                        text_error(argument, "option does not take an argument");
                        return false;
                }

                p8 letter = table[which].letter;

                if (value && table[which].wants == TEXT_LONG_MAYBE)
                {
                        // Nothing wants the value: the name having had one is
                        // the whole of what it said.
                        if (!table[which].valued)
                                value = null;
                        else
                                letter = table[which].valued;
                }

                if (!value && table[which].wants == TEXT_LONG_NEEDS)
                {
                        if (i + 1 >= count)
                        {
                                text_error(argument, "option requires an argument");
                                return false;
                        }

                        value = program_argument(++i);
                }

                if (out + 2 > TEXT_ARGUMENTS_MAX || spelled >= TEXT_ARGUMENTS_MAX)
                {
                        text_error(null, "too many arguments");
                        return false;
                }

                text_spellings[spelled][0] = '-';
                text_spellings[spelled][1] = letter;
                text_spellings[spelled][2] = '\0';
                text_arguments[out++] = (string_address)text_spellings[spelled++];

                if (value && out < TEXT_ARGUMENTS_MAX)
                        text_arguments[out++] = value;

                any = true;
        }

        if (!any)
                return true;

        text_arguments_own = true;
        text_argument_count = out;
        return true;
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

static text_long cat_long_options[] = {
    {"show-all", 'A', TEXT_LONG_ALONE},
    {"number-nonblank", 'b', TEXT_LONG_ALONE},
    {"show-ends", 'E', TEXT_LONG_ALONE},
    {"number", 'n', TEXT_LONG_ALONE},
    {"squeeze-blank", 's', TEXT_LONG_ALONE},
    {"show-tabs", 'T', TEXT_LONG_ALONE},
    {"show-nonprinting", 'v', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_cat()
{
        b32 first = 1;
        b32 inputs = 0;

        text_begin("cat");

        if (!text_expand_long(cat_long_options))
                return text_done(1);

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

enum
{
        WC_LONG_DEBUG = 1
};

static text_long wc_long_options[] = {
    {"lines", 'l', TEXT_LONG_ALONE},
    {"words", 'w', TEXT_LONG_ALONE},
    {"bytes", 'c', TEXT_LONG_ALONE},
    {"chars", 'm', TEXT_LONG_ALONE},
    {"max-line-length", 'L', TEXT_LONG_ALONE},
    // --debug names the counting strategy on the error stream and leaves
    // the counts alone, and there is one strategy here to name. A byte no
    // keyboard sends, so wc -W stays the error GNU makes of it.
    {"debug", WC_LONG_DEBUG, TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_wc()
{
        bool want_lines = false, want_words = false, want_bytes = false, want_chars = false;
        bool want_longest = false;
        positive total_lines = 0, total_words = 0, total_bytes = 0, total_longest = 0;
        positive width = 1;
        positive known = 0;
        bool unknown = false;

        text_begin("wc");

        if (!text_expand_long(wc_long_options))
                return text_done(1);

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
                                case 'L': want_longest = true; break;
                                case WC_LONG_DEBUG: break;
                                default: break;
                                }

                        continue;
                }

                text_file_add(i);
        }

        if (!want_lines && !want_words && !want_bytes && !want_chars && !want_longest)
        {
                want_lines = true;
                want_words = true;
                want_bytes = true;
        }

        b32 selected = (b32)want_lines + (b32)want_words + (b32)want_bytes +
                       (b32)want_chars + (b32)want_longest;
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
                positive longest = 0, column = 0;
                bool inside = false;

                if (!text_open(name))
                        continue;

                /*
                        wc -c on a regular file does not have to read it.

                        The size is in the inode, and asking the kernel for it
                        is one syscall against ninety two megabytes of reading
                        -- 0.3 ms against 4.1 on this machine, and the gap only
                        widens with the file. GNU does the same thing, which is
                        why its -c looks impossibly fast beside anything that
                        counts honestly.

                        Only where nothing else was asked for: -l, -w, -m and
                        -L all have to see the bytes. And only from where the
                        handle already stands, because something may have read
                        part of it already -- a pipe has no size at all and
                        text_regular_size says so by refusing.
                */
                if (want_bytes && !want_lines && !want_words && !want_chars &&
                    !want_longest)
                {
                        positive size = 0;

                        if (text_regular_size(text_input.handle, address_of size))
                        {
                                bipolar at = system_call_3(syscall(lseek),
                                                           text_input.handle, 0, 1);

                                if (at >= 0 && (positive)at <= size)
                                {
                                        bytes = size - (positive)at;
                                        system_call_3(syscall(lseek),
                                                      text_input.handle, 0, 2);
                                        text_input.finished = true;
                                        goto counted;
                                }
                        }
                }

                while (text_fill())
                {
                        p8 address_to at = text_input.buffer + text_input.position;
                        positive left = text_input.filled - text_input.position;

                        bytes += left;

                        /*
                                wc -l on its own, which is most of what wc is
                                asked for, needs nothing carried from one byte
                                to the next: no word state, no column, only how
                                many newlines went past. memory_count is that
                                loop and nothing else, so it can run at the
                                speed the bytes arrive -- 71 GB/s on a 9950X
                                against 15.8 for the loop below, which the
                                compiler has to write generally because it also
                                counts words and measures the longest line.

                                Reached only when nothing else was asked for.
                                Adding -w or -L puts the general loop back,
                                which is right: those need every byte anyway.
                        */
                        if (!want_words && !want_chars && !want_longest)
                        {
                                lines += memory_count(at, left, '\n');
                                text_input.position = text_input.filled;
                                continue;
                        }

                        for (positive c = 0; c < left; c++)
                        {
                                p8 character = at[c];

                                if (character == '\n')
                                        lines++;

                                /*
                                        -L is a width on a terminal rather
                                        than a count of bytes. A tab reaches
                                        the next stop eight columns apart; a
                                        return or a form feed starts the line
                                        over without being a line for the
                                        purpose of counting them; and a byte
                                        that would not show takes no room at
                                        all, which is why a line of control
                                        characters is nought columns wide and
                                        not as many as it has bytes.
                                */
                                if (character == '\n' || character == '\r' ||
                                    character == '\f')
                                {
                                        if (column > longest)
                                                longest = column;

                                        column = 0;
                                }
                                else if (character == '\t')
                                {
                                        column += 8 - column % 8;
                                }
                                else if (character >= 0x20 && character < 0x7f)
                                {
                                        column++;
                                }

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

        counted:
                text_close();

                if (column > longest)
                        longest = column;

                total_lines += lines;
                total_words += words;
                total_bytes += bytes;

                if (longest > total_longest)
                        total_longest = longest;

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
                        leading = false;
                }

                if (want_longest)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(longest, width);
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
                        leading = false;
                }

                // The total of the longest lines is the longest of them, not
                // their sum, which is the one column here that does not add up.
                if (want_longest)
                {
                        if (!leading)
                                text_put_character(' ');

                        text_put_number(total_longest, width);
                }

                text_put_string(" total\n");
        }

        return text_done(text_status);
}

// util-linux's rev, not coreutils': -0 rather than -z, and no -q or -v.
static text_long rev_long_options[] = {
    {"zero", '0', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_rev()
{
        text_begin("rev");

        if (!text_expand_long(rev_long_options))
                return text_done(1);

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] == '0' && !argument[2])
                {
                        text_delimiter = '\0';
                        continue;
                }

                text_file_add(i);
        }

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
                                text_put_character(text_delimiter);
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
                text_put_character(text_delimiter);
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
                        if (window[i - 1] != text_delimiter)
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

// Everything from start up to but not including stop, which is where head
// stops when the count it was given was counted from the end.
static fn text_stream_span(positive start, positive stop)
{
        system_call_3(syscall(lseek), text_input.handle, start, FILE_SEEK_SET);
        text_input.filled = 0;
        text_input.position = 0;
        text_input.finished = false;

        positive left = stop > start ? stop - start : 0;

        while (left && text_fill())
        {
                positive have = text_input.filled - text_input.position;
                positive take = have < left ? have : left;

                text_put(text_input.buffer + text_input.position, take);
                text_input.position += take;
                left -= take;
        }
}

static text_long head_long_options[] = {
    {"bytes", 'c', TEXT_LONG_NEEDS},
    {"lines", 'n', TEXT_LONG_NEEDS},
    {"quiet", 'q', TEXT_LONG_ALONE},
    {"silent", 'q', TEXT_LONG_ALONE},
    {"verbose", 'v', TEXT_LONG_ALONE},
    {"zero-terminated", 'z', TEXT_LONG_ALONE},
    {null, 0, 0}};

/*
        A count written with a minus in front of it names what to leave off
        the end rather than what to take from the front, so where head stops
        is found the same way tail finds where it starts.
*/
static fn text_head_short(positive count, bool by_bytes)
{
        positive size = 0;

        text_lines_count = 0;
        text_arena_used = text_lines ? TEXT_LINES_MAX * sizeof(text_slice) : 0;

        if (text_regular_size(text_input.handle, address_of size))
        {
                text_stream_span(0, by_bytes
                                        ? (count < size ? size - count : 0)
                                        : text_tail_start(text_input.handle, size, count));
                return;
        }

        if (!text_lines_ready())
                return;

        if (by_bytes)
        {
                p8 address_to held = (p8 address_to)text_arena_take(0);
                positive have = 0;

                while (text_fill())
                {
                        positive left = text_input.filled - text_input.position;
                        p8 address_to room = (p8 address_to)text_arena_take(left);

                        if (!room)
                                return;

                        if (!have)
                                held = room;

                        memory_copy(room, text_input.buffer + text_input.position, left);
                        have += left;
                        text_input.position = text_input.filled;
                }

                if (count < have)
                        text_put(held, have - count);

                return;
        }

        if (!text_lines_gather())
                return;

        positive stop = text_lines_count > count ? text_lines_count - count : 0;

        for (positive c = 0; c < stop; c++)
                text_put_slice(text_lines + c);
}

static b32 text_head()
{
        positive count = 10;
        bool by_bytes = false;
        bool from_end = false;
        bool quiet = false;
        bool loud = false;

        text_begin("head");

        if (!text_expand_long(head_long_options))
                return text_done(1);

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

                        if (flag == 'z')
                        {
                                text_delimiter = '\0';
                                continue;
                        }

                        if (flag == 'n' || flag == 'c')
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);

                                by_bytes = flag == 'c';

                                if (value && value[0] == '-')
                                {
                                        from_end = true;
                                        value++;
                                }

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

                if (from_end)
                {
                        text_head_short(count, by_bytes);
                        text_close();
                        continue;
                }

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
        The long spellings tail answers to.

        --pid, --sleep-interval and --max-unchanged-stats all say something
        about how to wait while following a file, and GNU itself warns and
        ignores them when it is not following. Nothing here follows, so they
        are taken and dropped.

        Not here, and deliberately: --zero-terminated, which is the line
        reader's business, and -F, which promises to reopen a file by name.
*/
enum
{
        TAIL_LONG_DISCARD = 1,
        TAIL_LONG_IGNORE = 2
};

static text_long tail_long_options[] = {
    {"bytes", 'c', TEXT_LONG_NEEDS},
    {"lines", 'n', TEXT_LONG_NEEDS},
    {"quiet", 'q', TEXT_LONG_ALONE},
    {"silent", 'q', TEXT_LONG_ALONE},
    {"verbose", 'v', TEXT_LONG_ALONE},
    {"follow", 'f', TEXT_LONG_MAYBE},
    {"retry", TAIL_LONG_IGNORE, TEXT_LONG_ALONE},
    {"pid", TAIL_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"sleep-interval", 's', TEXT_LONG_NEEDS},
    {"max-unchanged-stats", TAIL_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"debug", TAIL_LONG_IGNORE, TEXT_LONG_ALONE},
    {"zero-terminated", 'z', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_tail()
{
        positive count = 10;
        bool by_bytes = false;
        bool from_start = false;
        bool quiet = false;
        bool loud = false;

        text_begin("tail");

        if (!text_expand_long(tail_long_options))
                return text_done(1);

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

                        // -f waits for more to be written, which is a wait
                        // this does not do: the file is read to its end and
                        // that is where GNU would still be sitting.
                        if (flag == 'f' || flag == TAIL_LONG_IGNORE)
                                continue;

                        if (flag == 'z')
                        {
                                text_delimiter = '\0';
                                continue;
                        }

                        if (flag == TAIL_LONG_DISCARD || flag == 's')
                        {
                                if (!argument[c + 1])
                                        i++;

                                break;
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

/*
        The long spellings tee answers to.

        -i asks for SIGINT to be ignored while the copy runs, and -p for a
        pipe that has gone away not to end it. Neither is a fact about the
        bytes, and both are taken and dropped -- but a letter that is not one
        of tee's is refused, because a script that misspelled one is told so.
*/
static text_long tee_long_options[] = {
    {"append", 'a', TEXT_LONG_ALONE},
    {"ignore-interrupts", 'i', TEXT_LONG_ALONE},
    {"output-error", 'p', TEXT_LONG_MAYBE, 'p'},
    {null, 0, 0}};

static b32 text_tee()
{
        bool append = false;
        positive handles[TEXT_FILES_MAX];
        b32 handle_count = 0;

        text_begin("tee");

        if (!text_expand_long(tee_long_options))
                return text_done(1);

        for (b32 i = 1; i < text_argument_count; i++)
        {
                string_address argument = text_argument(i);

                if (argument[0] == '-' && argument[1] == '-' && !argument[2])
                {
                        for (b32 j = i + 1; j < text_argument_count; j++)
                                text_file_add(j);

                        break;
                }

                if (argument[0] == '-' && argument[1])
                {
                        for (positive c = 1; argument[c]; c++)
                        {
                                if (argument[c] == 'a')
                                        append = true;
                                else if (argument[c] == 'i' || argument[c] == 'p')
                                        (void)argument[c];
                                else
                                {
                                        p8 named[3] = {'-', argument[c], 0};

                                        text_error(named, "invalid option");
                                        return text_done(1);
                                }
                        }

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

/*
        nl

        Input is not one run of lines but a stack of sections: a line that is
        nothing but the delimiter three times over starts a header, twice a
        body, once a footer, and each of the three is numbered by its own
        rule. That is where every flag here comes from, and it is why -b takes
        a style rather than a yes or no.

        The delimiter line itself is neither numbered nor printed -- what
        comes out in its place is an empty line, measured, not a line of the
        padding an unnumbered line gets.
*/
static text_long nl_long_options[] = {
    {"body-numbering", 'b', TEXT_LONG_NEEDS},
    {"section-delimiter", 'd', TEXT_LONG_NEEDS},
    {"footer-numbering", 'f', TEXT_LONG_NEEDS},
    {"header-numbering", 'h', TEXT_LONG_NEEDS},
    {"line-increment", 'i', TEXT_LONG_NEEDS},
    {"join-blank-lines", 'l', TEXT_LONG_NEEDS},
    {"number-format", 'n', TEXT_LONG_NEEDS},
    {"no-renumber", 'p', TEXT_LONG_ALONE},
    {"number-separator", 's', TEXT_LONG_NEEDS},
    {"starting-line-number", 'v', TEXT_LONG_NEEDS},
    {"number-width", 'w', TEXT_LONG_NEEDS},
    {null, 0, 0}};

static regex_program nl_patterns[3];

// How many times over the delimiter is written, and nothing else on the line.
static b32 nl_section_of(p8 address_to delimiter)
{
        if (text_line_length != 2 && text_line_length != 4 && text_line_length != 6)
                return 0;

        for (positive c = 0; c < text_line_length; c += 2)
                if (text_line[c] != delimiter[0] || text_line[c + 1] != delimiter[1])
                        return 0;

        return (b32)(text_line_length / 2);
}

static b32 text_nl()
{
        positive width = 6;
        positive number = 1;
        positive step = 1;
        positive join = 1;
        positive blanks = 0;
        p8 styles[3] = {'n', 't', 'n'};
        b32 patterns[3] = {-1, -1, -1};
        b32 pattern_count = 0;
        b32 section = 1;
        p8 delimiter[2] = {'\\', ':'};
        string_address separator = "\t";
        p8 justify = 'r';
        bool zeros = false;
        bool keep_counting = false;

        text_begin("nl");

        if (!text_expand_long(nl_long_options))
                return text_done(1);

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

                p8 flag = argument[1];
                string_address value = argument[2] ? argument + 2 : null;

                if (flag == 'p')
                {
                        keep_counting = true;
                        continue;
                }

                if (flag != 'b' && flag != 'd' && flag != 'f' && flag != 'h' &&
                    flag != 'i' && flag != 'l' && flag != 'n' && flag != 's' &&
                    flag != 'v' && flag != 'w')
                {
                        p8 named[3] = {'-', flag, 0};

                        text_error(named, "invalid option");
                        return text_done(1);
                }

                if (!value)
                        value = text_argument(++i);

                if (!value)
                {
                        text_error(null, "option requires an argument");
                        return text_done(1);
                }

                if (flag == 'b' || flag == 'f' || flag == 'h')
                {
                        b32 which = flag == 'h' ? 0 : (flag == 'b' ? 1 : 2);

                        styles[which] = value[0];

                        if (value[0] != 'p')
                                continue;

                        if (pattern_count >= 3 ||
                            !regex_compile(value + 1, false, false, false))
                        {
                                text_error(value + 1, "invalid regular expression");
                                return text_done(1);
                        }

                        regex_keep(nl_patterns + pattern_count);
                        patterns[which] = pattern_count++;
                        continue;
                }

                if (flag == 'd')
                {
                        // One character given leaves the second as it was,
                        // so nl -d @ looks for @: and not for @@.
                        delimiter[0] = value[0];

                        if (value[0] && value[1])
                                delimiter[1] = value[1];

                        continue;
                }

                if (flag == 'w')
                        text_number_of(value, address_of width);
                else if (flag == 's')
                        separator = value;
                else if (flag == 'v')
                        text_number_of(value, address_of number);
                else if (flag == 'i')
                        text_number_of(value, address_of step);
                else if (flag == 'l')
                        text_number_of(value, address_of join);
                else
                {
                        justify = value[0];
                        zeros = value[0] == 'r' && value[1] == 'z';

                        if (value[0] == 'l')
                                justify = 'l';
                }
        }

        if (!join)
                join = 1;

        b32 inputs = text_files_count ? text_files_count : 1;
        positive separator_length = string_length(separator);

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                        continue;

                while (text_line_next())
                {
                        b32 marker = nl_section_of(delimiter);

                        if (marker)
                        {
                                // Three is the header, two the body, one the
                                // footer, which is the order they come in.
                                section = 3 - marker;
                                blanks = 0;

                                if (!keep_counting)
                                        number = 1;

                                text_put_character('\n');
                                continue;
                        }

                        p8 style = styles[section];
                        bool numbered = false;

                        if (style == 'a')
                        {
                                if (text_line_length)
                                {
                                        blanks = 0;
                                        numbered = true;
                                }
                                else if (++blanks >= join)
                                {
                                        blanks = 0;
                                        numbered = true;
                                }
                        }
                        else if (style == 't')
                        {
                                blanks = 0;
                                numbered = text_line_length != 0;
                        }
                        else if (style == 'p' && patterns[section] >= 0)
                        {
                                regex_select(nl_patterns + patterns[section]);
                                numbered = regex_search(text_line, text_line_length, 0);
                        }

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

static text_long fold_long_options[] = {
    {"bytes", 'b', TEXT_LONG_ALONE},
    {"characters", 'c', TEXT_LONG_ALONE},
    {"spaces", 's', TEXT_LONG_ALONE},
    {"width", 'w', TEXT_LONG_NEEDS},
    {null, 0, 0}};

static b32 text_fold()
{
        positive width = 80;
        bool spaces = false;
        bool bytes = false;

        text_begin("fold");

        if (!text_expand_long(fold_long_options))
                return text_done(1);

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

                        // -c counts characters where -b counts bytes, and
                        // every character here is one byte.
                        if (flag == 'b' || flag == 'c')
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

/*
        Where each range began, for --output-delimiter.

        cut -c1,2 and cut -c1-2 select the same two characters and GNU puts
        its output delimiter between the first pair and not the second, so
        the bitmap alone cannot answer. A range start already covered by an
        earlier range is not one, which is how GNU's merging of overlapping
        ranges shows up here; two overlapping ranges written the wrong way
        round is the one spec this still parts differently from GNU.
*/
static p8 text_list_begins[TEXT_LIST_MAX];

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

                if (first < TEXT_LIST_MAX && !text_list[first])
                        text_list_begins[first] = 1;

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

/*
        The long spellings cut answers to.

        --complement and --output-delimiter have no letter of their own, so
        they borrow bytes no keyboard sends.

        Not here, and deliberately: --zero-terminated, which is the line
        reader's business rather than cut's.
*/
enum
{
        CUT_LONG_COMPLEMENT = 1,
        CUT_LONG_OUTPUT = 2
};

static text_long cut_long_options[] = {
    {"bytes", 'b', TEXT_LONG_NEEDS},
    {"characters", 'c', TEXT_LONG_NEEDS},
    {"delimiter", 'd', TEXT_LONG_NEEDS},
    {"fields", 'f', TEXT_LONG_NEEDS},
    {"complement", CUT_LONG_COMPLEMENT, TEXT_LONG_ALONE},
    {"no-partial", 'n', TEXT_LONG_ALONE},
    {"only-delimited", 's', TEXT_LONG_ALONE},
    {"output-delimiter", CUT_LONG_OUTPUT, TEXT_LONG_NEEDS},
    {"zero-terminated", 'z', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_cut()
{
        p8 delimiter = '\t';
        bool by_field = false;
        bool by_character = false;
        bool only_delimited = false;
        bool have_list = false;
        bool complement = false;
        bool whitespace = false;
        bool have_delimiter = false;
        b32 kinds = 0;
        string_address separator = null;
        positive separator_length = 0;

        text_begin("cut");

        if (!text_expand_long(cut_long_options))
                return text_done(1);

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

                        // -n says a multibyte character is not to be split
                        // by -b, and every character here is one byte.
                        if (flag == 'n')
                                continue;

                        if (flag == CUT_LONG_COMPLEMENT)
                        {
                                complement = true;
                                continue;
                        }

                        if (flag == 'w')
                        {
                                whitespace = true;
                                continue;
                        }

                        if (flag == 'z')
                        {
                                text_delimiter = '\0';
                                continue;
                        }

                        if (flag == 'd' || flag == 'f' || flag == 'c' || flag == 'b' ||
                            flag == 'O' || flag == CUT_LONG_OUTPUT)
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
                                        have_delimiter = true;
                                }
                                // -O is not in cut's own help and cut takes it
                                // anyway, which is where this one comes from.
                                else if (flag == 'O' || flag == CUT_LONG_OUTPUT)
                                {
                                        separator = value;
                                        separator_length = string_length(value);
                                }
                                else
                                {
                                        by_field = flag == 'f';
                                        by_character = !by_field;
                                        have_list = true;
                                        kinds++;

                                        if (!text_list_parse(value))
                                        {
                                                text_error(null, "invalid list");
                                                return text_done(1);
                                        }
                                }

                                break;
                        }

                        {
                                p8 named[3] = {'-', flag, 0};

                                text_error(named, "invalid option");
                                return text_done(1);
                        }
                }
        }

        if (!have_list)
        {
                text_error(null, "you must specify a list of bytes, characters, or fields");
                return text_done(1);
        }

        /*
                Three ways of saying the same no. GNU refuses two lists of
                different kinds, a delimiter for something that has no fields
                in it, and -s for the same reason -- and refusing them is what
                stops cut -d: -c1 from quietly ignoring the -d.
        */
        if (kinds > 1)
        {
                text_error(null, "only one type of list may be specified");
                return text_done(1);
        }

        if (whitespace && have_delimiter)
        {
                text_error(null, "-d and -w are mutually exclusive");
                return text_done(1);
        }

        if (!by_field && (have_delimiter || only_delimited || whitespace))
        {
                text_error(null, "an input delimiter makes sense only when operating on fields");
                return text_done(1);
        }

        // -w splits on runs of blanks and joins with a tab, which is the one
        // place cut's two delimiters are not the same character.
        if (whitespace && !separator)
        {
                separator = "\t";
                separator_length = 1;
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
                                bool wrote = false;
                                bool ran = false;

                                for (positive c = 0; c < text_line_length; c++)
                                {
                                        bool take = text_list_has(c + 1) != complement;

                                        if (!take)
                                        {
                                                ran = false;
                                                continue;
                                        }

                                        if (separator && wrote &&
                                            (!ran || (!complement && text_list_begins[c + 1])))
                                                text_put(separator, separator_length);

                                        text_put_character(text_line[c]);
                                        wrote = true;
                                        ran = true;
                                }

                                text_put_character(text_delimiter);
                                continue;
                        }

                        if (by_field)
                        {
                                bool split = false;

                                for (positive c = 0; c < text_line_length; c++)
                                        if (whitespace ? text_blank(text_line[c])
                                                       : text_line[c] == delimiter)
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
                                                text_put_character(text_delimiter);
                                        }

                                        continue;
                                }

                                positive at = 0;
                                positive which = 1;
                                bool wrote = false;

                                while (at <= text_line_length)
                                {
                                        positive from = at;

                                        while (at < text_line_length &&
                                               (whitespace ? !text_blank(text_line[at])
                                                           : text_line[at] != delimiter))
                                                at++;

                                        if (text_list_has(which) != complement)
                                        {
                                                if (wrote)
                                                {
                                                        if (separator)
                                                                text_put(separator,
                                                                         separator_length);
                                                        else
                                                                text_put_character(delimiter);
                                                }

                                                text_put(text_line + from, at - from);
                                                wrote = true;
                                        }

                                        if (at == text_line_length)
                                                break;

                                        // A run of blanks is one delimiter,
                                        // where a run of colons is several.
                                        at++;

                                        if (whitespace)
                                                while (at < text_line_length &&
                                                       text_blank(text_line[at]))
                                                        at++;

                                        which++;
                                }

                                text_put_character(text_delimiter);
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

static text_long tr_long_options[] = {
    {"complement", 'c', TEXT_LONG_ALONE},
    {"delete", 'd', TEXT_LONG_ALONE},
    {"squeeze-repeats", 's', TEXT_LONG_ALONE},
    {"truncate-set1", 't', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_tr()
{
        bool remove = false;
        bool squeeze = false;
        bool complement = false;
        bool truncate = false;
        string_address first = null;
        string_address second = null;
        string_address extra = null;

        text_begin("tr");

        if (!text_expand_long(tr_long_options))
                return text_done(1);

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
                                    argument[c] != 'c' && argument[c] != 'C' &&
                                    argument[c] != 't')
                                        flags = false;

                        if (flags)
                        {
                                for (positive c = 1; argument[c]; c++)
                                        switch (argument[c])
                                        {
                                        case 'd': remove = true; break;
                                        case 's': squeeze = true; break;
                                        case 't': truncate = true; break;
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
                else if (!extra)
                        extra = argument;
        }

        if (!first)
        {
                text_error(null, "missing operand");
                return text_done(1);
        }

        /*
                How many sets each shape of tr wants, which it has to say out
                loud rather than quietly ignore the ones it did not use. Two
                to translate; one to delete or to squeeze; two to delete and
                squeeze at once, because the second is what gets squeezed.
        */
        if (extra || (remove && !squeeze && second))
        {
                text_error(extra ? extra : second, "extra operand");
                return text_done(1);
        }

        if (!second && !remove && !squeeze)
        {
                text_error(first, "missing operand after");
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

                        // -t stops padding: what the second set does not
                        // reach is left alone rather than mapped to its last.
                        if (truncate && have > room)
                                have = room;

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

/*
        The long spellings uniq answers to.

        --all-repeated and --group both take a word or no word at all and mean
        something different either way, so each has two letters: one for the
        bare name and one that arrives carrying the word.

        Not here, and deliberately: --zero-terminated, which is the line
        reader's business rather than uniq's.
*/
enum
{
        UNIQ_LONG_ALL = 1,
        UNIQ_LONG_GROUP = 2,
        UNIQ_LONG_GROUP_HOW = 3
};

static text_long uniq_long_options[] = {
    {"count", 'c', TEXT_LONG_ALONE},
    {"repeated", 'd', TEXT_LONG_ALONE},
    {"all-repeated", 'D', TEXT_LONG_MAYBE, UNIQ_LONG_ALL},
    {"group", UNIQ_LONG_GROUP, TEXT_LONG_MAYBE, UNIQ_LONG_GROUP_HOW},
    {"skip-fields", 'f', TEXT_LONG_NEEDS},
    {"ignore-case", 'i', TEXT_LONG_ALONE},
    {"skip-chars", 's', TEXT_LONG_NEEDS},
    {"unique", 'u', TEXT_LONG_ALONE},
    {"check-chars", 'w', TEXT_LONG_NEEDS},
    {"zero-terminated", 'z', TEXT_LONG_ALONE},
    {null, 0, 0}};

enum
{
        UNIQ_GROUP_NONE = 0,
        UNIQ_GROUP_SEPARATE,
        UNIQ_GROUP_PREPEND,
        UNIQ_GROUP_APPEND,
        UNIQ_GROUP_BOTH
};

static b32 text_uniq()
{
        bool counting = false;
        bool repeated_only = false;
        bool unique_only = false;
        bool fold = false;
        bool all_repeated = false;
        positive all_how = UNIQ_GROUP_NONE;
        bool grouping = false;
        positive group_how = UNIQ_GROUP_SEPARATE;
        positive skip_fields = 0;
        positive skip_characters = 0;
        positive compare_width = 0;
        bool bounded = false;

        text_begin("uniq");

        if (!text_expand_long(uniq_long_options))
                return text_done(1);

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

                        if (flag == 'D')
                        {
                                all_repeated = true;
                                continue;
                        }

                        if (flag == 'z')
                        {
                                text_delimiter = '\0';
                                continue;
                        }

                        if (flag == UNIQ_LONG_GROUP)
                        {
                                grouping = true;
                                continue;
                        }

                        if (flag == UNIQ_LONG_ALL || flag == UNIQ_LONG_GROUP_HOW)
                        {
                                string_address value = text_argument(++i);
                                positive how = UNIQ_GROUP_NONE;

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(1);
                                }

                                if (string_equals(value, "none"))
                                        how = UNIQ_GROUP_NONE;
                                else if (string_equals(value, "prepend"))
                                        how = UNIQ_GROUP_PREPEND;
                                else if (string_equals(value, "separate"))
                                        how = UNIQ_GROUP_SEPARATE;
                                else if (string_equals(value, "append") &&
                                         flag == UNIQ_LONG_GROUP_HOW)
                                        how = UNIQ_GROUP_APPEND;
                                else if (string_equals(value, "both") &&
                                         flag == UNIQ_LONG_GROUP_HOW)
                                        how = UNIQ_GROUP_BOTH;
                                else
                                {
                                        text_error(value, "invalid argument");
                                        return text_done(1);
                                }

                                if (flag == UNIQ_LONG_ALL)
                                {
                                        all_repeated = true;
                                        all_how = how;
                                }
                                else
                                {
                                        grouping = true;
                                        group_how = how;
                                }

                                break;
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

        if (all_repeated && counting)
        {
                text_error(null, "printing all duplicated lines and repeat counts is meaningless");
                return text_done(1);
        }

        if (grouping && (counting || repeated_only || unique_only || all_repeated))
        {
                text_error(null, "--group is mutually exclusive with -c/-d/-D/-u");
                return text_done(1);
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

        // uniq puts a terminator on every line it writes, including the last
        // one when the input did not have one. Every other tool here hands
        // back what it was given; measured, this one does not.
        static p8 held[TEXT_LINE_MAX];
        positive held_length = 0;
        bool have_held = false;
        bool shown_group = false;
        positive count = 0;
        positive gap = grouping ? group_how : all_how;

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

                                /*
                                        -D and --group both print every line
                                        of a group rather than one of them, so
                                        the rest of a group goes out as it
                                        arrives. -D cannot do that until the
                                        second line proves the group repeats,
                                        which is where the first one is caught
                                        up on.
                                */
                                if (all_repeated && count == 2)
                                {
                                        if (gap == UNIQ_GROUP_PREPEND ||
                                            (gap == UNIQ_GROUP_SEPARATE && shown_group))
                                                text_put_character('\n');

                                        shown_group = true;
                                        text_put(held, held_length);
                                        text_put_character(text_delimiter);
                                }

                                if ((all_repeated && count >= 2) || grouping)
                                {
                                        text_put(text_line, text_line_length);
                                        text_put_character(text_delimiter);
                                }

                                continue;
                        }
                }

                if (have_held && !all_repeated && !grouping)
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
                                text_put_character(text_delimiter);
                        }
                }

                if (!more)
                        break;

                memory_copy(held, text_line, text_line_length);
                held_length = text_line_length;
                have_held = true;
                count = 1;

                if (grouping)
                {
                        if (gap == UNIQ_GROUP_PREPEND || gap == UNIQ_GROUP_BOTH ||
                            ((gap == UNIQ_GROUP_SEPARATE || gap == UNIQ_GROUP_APPEND) &&
                             shown_group))
                                text_put_character('\n');

                        shown_group = true;
                        text_put(held, held_length);
                        text_put_character(text_delimiter);
                }
        }

        // append is separate with one more at the end, and both is prepend
        // with the same, which is why neither has a rule of its own above.
        if (grouping && shown_group &&
            (gap == UNIQ_GROUP_APPEND || gap == UNIQ_GROUP_BOTH))
                text_put_character('\n');

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

/*
        -B holds lines that have not matched, in case the next one does.

        A slot per line over one byte pool, not a line per fixed slot: -B 2
        over megabyte lines and -B 5000 over short ones both have to fit in
        the same place, and any single slot width is wrong for one of them.
        The oldest line goes when either the slots or the pool runs out, so a
        -B larger than what fits shows less context than GNU. Holding a
        megabyte per line asked for is the other answer and it is worse.
*/
#define GREP_HOLD_BYTES (1u << 20)
#define GREP_HOLD_LINES 8192

static p8 address_to grep_hold_pool;
static positive address_to grep_hold_at;
static positive address_to grep_hold_size;
static positive address_to grep_hold_number;
static positive grep_hold_slots;
static positive grep_hold_first;
static positive grep_hold_count;
static positive grep_hold_used;
static positive grep_hold_write;

static bool grep_hold_make(positive lines)
{
        if (lines > GREP_HOLD_LINES)
                lines = GREP_HOLD_LINES;

        grep_hold_slots = lines;
        grep_hold_pool = (p8 address_to)text_arena_take(GREP_HOLD_BYTES);
        grep_hold_at = (positive address_to)text_arena_take(lines * sizeof(positive));
        grep_hold_size = (positive address_to)text_arena_take(lines * sizeof(positive));
        grep_hold_number = (positive address_to)text_arena_take(lines * sizeof(positive));

        return grep_hold_pool && grep_hold_at && grep_hold_size && grep_hold_number;
}

static fn grep_hold_clear()
{
        grep_hold_first = 0;
        grep_hold_count = 0;
        grep_hold_used = 0;
        grep_hold_write = 0;
}

static fn grep_hold_put(string_address line, positive length, positive number)
{
        if (!grep_hold_slots)
                return;

        if (length > GREP_HOLD_BYTES)
        {
                grep_hold_clear();
                return;
        }

        while (grep_hold_count == grep_hold_slots ||
               grep_hold_used + length > GREP_HOLD_BYTES)
        {
                grep_hold_used -= grep_hold_size[grep_hold_first];
                grep_hold_first = (grep_hold_first + 1) % grep_hold_slots;
                grep_hold_count--;
        }

        positive slot = (grep_hold_first + grep_hold_count) % grep_hold_slots;
        positive at = grep_hold_write;
        positive head = GREP_HOLD_BYTES - at;

        if (head > length)
                head = length;

        memory_copy(grep_hold_pool + at, line, head);

        if (length > head)
                memory_copy(grep_hold_pool, line + head, length - head);

        grep_hold_at[slot] = at;
        grep_hold_size[slot] = length;
        grep_hold_number[slot] = number;
        grep_hold_write = (at + length) % GREP_HOLD_BYTES;
        grep_hold_used += length;
        grep_hold_count++;
}

static fn grep_hold_say(positive slot)
{
        positive at = grep_hold_at[slot];
        positive length = grep_hold_size[slot];
        positive head = GREP_HOLD_BYTES - at;

        if (head > length)
                head = length;

        text_put(grep_hold_pool + at, head);

        if (length > head)
                text_put(grep_hold_pool, length - head);
}

/*
        What goes in front of a line, which four flags all want a say in.

        The separator is ':' on a line that matched and '-' on one that is
        only there for context, which is how -A output reads as context at a
        glance. -T pads the numbers and puts a tab after the last field --
        the width is the decimal width of the file's size, and nineteen when
        the size is not knowable, which is what GNU prints down a pipe.
*/
static bool grep_names;
static bool grep_numbered;
static bool grep_offsets;
static bool grep_tabbed;
static bool grep_null_name;
static positive grep_column;

static fn grep_head(string_address name, p8 separator, positive number, positive offset)
{
        if (grep_names)
        {
                text_put_string(name);
                text_put_character(grep_null_name ? '\0' : separator);
        }

        if (grep_numbered)
        {
                text_put_number(number, grep_tabbed ? grep_column : 1);
                text_put_character(separator);
        }

        if (grep_offsets)
        {
                text_put_number(offset, grep_tabbed ? grep_column : 1);
                text_put_character(separator);
        }

        if (grep_tabbed && (grep_names || grep_numbered || grep_offsets))
                text_put_character('\t');
}

/*
        The globs --include and the two beside it are matched against.

        A file's own name and never the path it was found down: GNU matches
        d/sub/s.txt against s.txt and against nothing with a slash in it, so
        there is no path walking here and * does not have to stop anywhere.
*/
static positive text_glob_set(string_address pattern, positive at, p8 character,
                              bool address_to hit)
{
        bool negate = false;
        bool found = false;
        positive p = at + 1;
        bool first = true;

        if (pattern[p] == '!' || pattern[p] == '^')
        {
                negate = true;
                p++;
        }

        while (pattern[p] && (pattern[p] != ']' || first))
        {
                p8 low = pattern[p++];

                first = false;

                if (pattern[p] == '-' && pattern[p + 1] && pattern[p + 1] != ']')
                {
                        p8 high = pattern[p + 1];

                        p += 2;

                        if (character >= low && character <= high)
                                found = true;

                        continue;
                }

                if (character == low)
                        found = true;
        }

        // No closing bracket, so the '[' was never a set at all.
        if (!pattern[p])
                return 0;

        address_to hit = found != negate;
        return p + 1;
}

static bool text_glob(string_address pattern, string_address name)
{
        positive p = 0;
        positive n = 0;
        positive star = TEXT_UNSET;
        positive back = 0;

        while (name[n])
        {
                p8 want = pattern[p];

                if (want == '*')
                {
                        star = ++p;
                        back = n;
                        continue;
                }

                if (want == '[')
                {
                        bool hit = false;
                        positive after = text_glob_set(pattern, p, name[n],
                                                       address_of hit);

                        if (after)
                        {
                                if (hit)
                                {
                                        p = after;
                                        n++;
                                        continue;
                                }

                                if (star == TEXT_UNSET)
                                        return false;

                                p = star;
                                n = ++back;
                                continue;
                        }
                }

                if (want == '\\' && pattern[p + 1])
                        want = pattern[++p];

                if ((want == '?' || want == name[n]) && want)
                {
                        p++;
                        n++;
                        continue;
                }

                if (star == TEXT_UNSET)
                        return false;

                p = star;
                n = ++back;
        }

        while (pattern[p] == '*')
                p++;

        return !pattern[p];
}

/*
        -r turns a directory operand into the files under it, in the order the
        kernel hands them back: nothing here sorts, and neither does GNU's own
        walk, so the two agree line for line on the same directory.

        A symlink met on the way down is skipped, which is what -r means and
        what -R undoes. The one named on the command line is followed either
        way -- opening it is how it was found at all.
*/
#define GREP_PATHS_MAX (1 << 20)
#define GREP_GLOBS_MAX 32
#define GREP_GLOB_BYTES 4096

static string_address address_to grep_paths;
static positive grep_path_count;
static bool grep_recursive;
static bool grep_dereference;
static bool grep_expanded;
static bool grep_skip_directories;

static string_address grep_include[GREP_GLOBS_MAX];
static b32 grep_include_count;
static string_address grep_exclude[GREP_GLOBS_MAX];
static b32 grep_exclude_count;
static string_address grep_exclude_dir[GREP_GLOBS_MAX];
static b32 grep_exclude_dir_count;
static p8 grep_glob_pool[GREP_GLOB_BYTES];
static positive grep_glob_used;

// --exclude-dir=sub and --exclude-dir=sub/ name the same directory.
static string_address grep_glob_keep(string_address value)
{
        positive length = string_length(value);

        while (length && value[length - 1] == '/')
                length--;

        if (grep_glob_used + length + 1 > GREP_GLOB_BYTES)
                return value;

        p8 address_to room = grep_glob_pool + grep_glob_used;

        memory_copy(room, value, length);
        room[length] = '\0';
        grep_glob_used += length + 1;

        return (string_address)room;
}

static string_address grep_base(string_address path)
{
        string_address at = path;

        for (positive c = 0; path[c]; c++)
                if (path[c] == '/')
                        at = path + c + 1;

        return at;
}

static bool grep_globs_have(string_address address_to list, b32 count,
                            string_address name)
{
        for (b32 i = 0; i < count; i++)
                if (text_glob(list[i], name))
                        return true;

        return false;
}

static bool grep_wanted_file(string_address path)
{
        string_address name = grep_base(path);

        if (grep_include_count &&
            !grep_globs_have(grep_include, grep_include_count, name))
                return false;

        return !grep_globs_have(grep_exclude, grep_exclude_count, name);
}

static bool grep_wanted_directory(string_address path)
{
        return !grep_globs_have(grep_exclude_dir, grep_exclude_dir_count,
                                grep_base(path));
}

// The kernel's mode for a path, or zero when there is none to be had.
static p32 text_path_mode(string_address path)
{
        p8 raw[256];
        bipolar handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
                return 0;

        memory_fill(raw, 0, sizeof(raw));

        bipolar told = system_call_2(syscall(fstat), (positive)handle, (positive)raw);

        system_call_1(syscall(close), (positive)handle);

        if (told < 0)
                return 0;

        return address_to(p32 address_to)(raw + TEXT_STAT_MODE);
}

static bool grep_path_add(string_address path)
{
        if (grep_path_count >= GREP_PATHS_MAX)
        {
                text_error(null, "too many files");
                return false;
        }

        grep_paths[grep_path_count++] = path;
        return true;
}

// An empty prefix is grep -r with nothing named, where GNU walks the working
// directory and prints what it finds without a ./ in front of it.
static string_address grep_path_join(string_address directory, string_address name)
{
        positive have = directory ? string_length(directory) : 0;
        positive extra = string_length(name);

        while (have > 1 && directory[have - 1] == '/')
                have--;

        if (have == 1 && directory[0] == '/')
                have = 0;

        p8 address_to room = (p8 address_to)text_arena_take(have + extra + 2);

        if (!room)
                return null;

        memory_copy(room, directory, have);

        if (have || (directory && directory[0] == '/'))
                room[have++] = '/';

        memory_copy(room + have, name, extra);
        room[have + extra] = '\0';

        return (string_address)room;
}

#define GREP_DIRENT_BYTES 2048

static bool grep_walk(string_address path)
{
        bipolar handle = text_open_handle(
            path && path[0] ? path : (string_address) ".", FILE_READ, 0);
        p8 entries[GREP_DIRENT_BYTES];
        bool fine = true;

        if (handle < 0)
                return false;

        for (;;)
        {
                bipolar got = system_call_3(syscall(getdents64), (positive)handle,
                                            (positive)entries, sizeof(entries));

                if (got <= 0)
                        break;

                for (p8 address_to step = entries; step < entries + got;)
                {
                        struct linux_dirent64 address_to entry =
                            (struct linux_dirent64 address_to)step;
                        string_address name = (string_address)entry->d_name;
                        p8 kind = entry->d_type;

                        step += entry->d_reclen;

                        if (name[0] == '.' &&
                            (!name[1] || (name[1] == '.' && !name[2])))
                                continue;

                        if (kind == 10 && !grep_dereference)
                                continue;

                        string_address full = grep_path_join(path, name);

                        if (!full)
                                return false;

                        if (kind == 0 || kind == 10)
                        {
                                p32 mode = text_path_mode(full);

                                if (!mode)
                                        continue;

                                kind = (mode & 0170000) == 0040000 ? 4
                                     : (mode & 0170000) == 0100000 ? 8
                                                                   : 1;
                        }

                        if (kind == 4)
                        {
                                if (grep_wanted_directory(full) && !grep_walk(full))
                                        fine = false;

                                continue;
                        }

                        // Anything that is not a plain file is a device, and
                        // a walk does not read devices.
                        if (kind != 8)
                                continue;

                        if (grep_wanted_file(full) && !grep_path_add(full))
                                return false;
                }
        }

        system_call_1(syscall(close), (positive)handle);
        return fine;
}

/*
        The long spellings grep answers to.

        --label and the three that have no letter of their own borrow a byte
        no keyboard sends, so they reach the switch below without also making
        grep -\001 mean something.

        Not here, and deliberately: -P and --perl-regexp, which is a second
        regular expression language; and --color=always, whose escape codes
        would have to be woven through every line this prints for an answer
        no script reads. The when words that mean no colour are taken.
*/
enum
{
        GREP_LONG_LABEL = 1,
        GREP_LONG_BUFFERED = 2,
        GREP_LONG_UNFOLD = 3,
        GREP_LONG_BINARY_FILES = 4,
        GREP_LONG_SEPARATOR = 5,
        GREP_LONG_NO_SEPARATOR = 6,
        GREP_LONG_INCLUDE = 7,
        GREP_LONG_EXCLUDE = 8,
        GREP_LONG_EXCLUDE_DIR = 9,
        GREP_LONG_COLOUR = 10,
        GREP_LONG_COLOUR_WHEN = 11
};

static text_long grep_long_options[] = {
    {"extended-regexp", 'E', TEXT_LONG_ALONE},
    {"fixed-strings", 'F', TEXT_LONG_ALONE},
    {"basic-regexp", 'G', TEXT_LONG_ALONE},
    {"regexp", 'e', TEXT_LONG_NEEDS},
    {"file", 'f', TEXT_LONG_NEEDS},
    {"ignore-case", 'i', TEXT_LONG_ALONE},
    {"no-ignore-case", GREP_LONG_UNFOLD, TEXT_LONG_ALONE},
    {"word-regexp", 'w', TEXT_LONG_ALONE},
    {"line-regexp", 'x', TEXT_LONG_ALONE},
    {"no-messages", 's', TEXT_LONG_ALONE},
    {"invert-match", 'v', TEXT_LONG_ALONE},
    {"max-count", 'm', TEXT_LONG_NEEDS},
    {"byte-offset", 'b', TEXT_LONG_ALONE},
    {"line-number", 'n', TEXT_LONG_ALONE},
    {"line-buffered", GREP_LONG_BUFFERED, TEXT_LONG_ALONE},
    {"with-filename", 'H', TEXT_LONG_ALONE},
    {"no-filename", 'h', TEXT_LONG_ALONE},
    {"label", GREP_LONG_LABEL, TEXT_LONG_NEEDS},
    {"only-matching", 'o', TEXT_LONG_ALONE},
    {"quiet", 'q', TEXT_LONG_ALONE},
    {"silent", 'q', TEXT_LONG_ALONE},
    {"binary-files", GREP_LONG_BINARY_FILES, TEXT_LONG_NEEDS},
    {"text", 'a', TEXT_LONG_ALONE},
    {"binary", 'U', TEXT_LONG_ALONE},
    {"files-without-match", 'L', TEXT_LONG_ALONE},
    {"files-with-matches", 'l', TEXT_LONG_ALONE},
    {"count", 'c', TEXT_LONG_ALONE},
    {"initial-tab", 'T', TEXT_LONG_ALONE},
    {"null", 'Z', TEXT_LONG_ALONE},
    {"null-data", 'z', TEXT_LONG_ALONE},
    {"directories", 'd', TEXT_LONG_NEEDS},
    {"devices", 'D', TEXT_LONG_NEEDS},
    {"group-separator", GREP_LONG_SEPARATOR, TEXT_LONG_NEEDS},
    {"no-group-separator", GREP_LONG_NO_SEPARATOR, TEXT_LONG_ALONE},
    {"before-context", 'B', TEXT_LONG_NEEDS},
    {"after-context", 'A', TEXT_LONG_NEEDS},
    {"context", 'C', TEXT_LONG_NEEDS},
    {"recursive", 'r', TEXT_LONG_ALONE},
    {"dereference-recursive", 'R', TEXT_LONG_ALONE},
    {"include", GREP_LONG_INCLUDE, TEXT_LONG_NEEDS},
    {"exclude", GREP_LONG_EXCLUDE, TEXT_LONG_NEEDS},
    {"exclude-dir", GREP_LONG_EXCLUDE_DIR, TEXT_LONG_NEEDS},
    {"color", GREP_LONG_COLOUR, TEXT_LONG_MAYBE, GREP_LONG_COLOUR_WHEN},
    {"colour", GREP_LONG_COLOUR, TEXT_LONG_MAYBE, GREP_LONG_COLOUR_WHEN},
    {null, 0, 0}};

static b32 text_grep()
{
        bool extended = false;
        bool fixed = false;
        bool icase = false;
        bool invert = false;
        bool counting = false;
        bool listing = false;
        bool listing_without = false;
        bool quiet = false;
        bool no_names = false;
        bool with_names = false;
        bool quietly = false;
        bool whole_line = false;
        bool whole_word = false;
        bool only = false;
        bool have_pattern = false;
        bool never = false;
        positive limit = TEXT_UNSET;
        positive before = 0;
        positive after = 0;
        string_address label = null;
        string_address separator = "--";
        bool null_data = false;
        b32 pattern_from = -1;

        text_begin("grep");

        if (!text_expand_long(grep_long_options))
                return text_done(2);

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

                        if (flag == 'e' || flag == 'f' || flag == 'm' ||
                            flag == 'A' || flag == 'B' || flag == 'C' ||
                            flag == 'd' || flag == 'D' ||
                            flag == GREP_LONG_LABEL || flag == GREP_LONG_BINARY_FILES ||
                            flag == GREP_LONG_SEPARATOR || flag == GREP_LONG_INCLUDE ||
                            flag == GREP_LONG_EXCLUDE || flag == GREP_LONG_EXCLUDE_DIR ||
                            flag == GREP_LONG_COLOUR_WHEN)
                        {
                                string_address value = argument[c + 1] ? argument + c + 1
                                                                       : text_argument(++i);
                                positive number = 0;

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(2);
                                }

                                if (flag == 'e')
                                {
                                        grep_pattern_add(value, string_length(value),
                                                         fixed, extended);
                                        have_pattern = true;
                                }
                                else if (flag == 'f')
                                {
                                        if (!text_open(value))
                                                return text_done(2);

                                        // An empty pattern file matches
                                        // nothing at all, which is not the
                                        // same as an empty pattern.
                                        while (text_line_next())
                                                grep_pattern_add(text_line, text_line_length,
                                                                 fixed, extended);

                                        text_close();

                                        if (!grep_pattern_any)
                                                never = true;

                                        have_pattern = true;
                                }
                                else if (flag == GREP_LONG_LABEL)
                                {
                                        label = value;
                                }
                                else if (flag == GREP_LONG_SEPARATOR)
                                {
                                        separator = value;
                                }
                                /*
                                        -d, -D and --binary-files all name a
                                        kind of file to do something other
                                        than read. Nothing here is ever handed
                                        a directory or a device by the shell
                                        that is not, and every file is bytes,
                                        so the answer is the same whichever
                                        word came -- but the word still has to
                                        be one of the words, because a script
                                        that misspells it is told so by GNU.

                                        The three exit statuses below are not a
                                        pattern. They are what grep 3.11 did.
                                */
                                else if (flag == GREP_LONG_INCLUDE)
                                {
                                        if (grep_include_count < GREP_GLOBS_MAX)
                                                grep_include[grep_include_count++] = value;
                                }
                                else if (flag == GREP_LONG_EXCLUDE)
                                {
                                        if (grep_exclude_count < GREP_GLOBS_MAX)
                                                grep_exclude[grep_exclude_count++] = value;
                                }
                                else if (flag == GREP_LONG_EXCLUDE_DIR)
                                {
                                        if (grep_exclude_dir_count < GREP_GLOBS_MAX)
                                                grep_exclude_dir[grep_exclude_dir_count++] =
                                                    grep_glob_keep(value);
                                }
                                // Colour is a terminal's business, and the
                                // one word here that asks for it is refused
                                // rather than answered wrongly.
                                else if (flag == GREP_LONG_COLOUR_WHEN)
                                {
                                        if (!string_equals(value, "never") &&
                                            !string_equals(value, "no") &&
                                            !string_equals(value, "none") &&
                                            !string_equals(value, "auto") &&
                                            !string_equals(value, "tty") &&
                                            !string_equals(value, "if-tty"))
                                        {
                                                text_error(value, "invalid argument for --color");
                                                return text_done(2);
                                        }
                                }
                                else if (flag == 'd')
                                {
                                        if (string_equals(value, "recurse"))
                                                grep_recursive = true;
                                        else if (string_equals(value, "skip"))
                                                grep_skip_directories = true;
                                        else if (!string_equals(value, "read"))
                                        {
                                                text_error(value, "invalid argument for --directories");
                                                return text_done(1);
                                        }
                                }
                                else if (flag == 'D')
                                {
                                        if (!string_equals(value, "read") &&
                                            !string_equals(value, "skip"))
                                        {
                                                text_error(null, "unknown devices method");
                                                return text_done(2);
                                        }
                                }
                                else if (flag == GREP_LONG_BINARY_FILES)
                                {
                                        if (!string_equals(value, "binary") &&
                                            !string_equals(value, "text") &&
                                            !string_equals(value, "without-match"))
                                        {
                                                text_error(null, "unknown binary-files type");
                                                return text_done(2);
                                        }
                                }
                                else if (flag == 'm' || flag == 'A' || flag == 'B' ||
                                         flag == 'C')
                                {
                                        if (!text_number_of(value, address_of number))
                                        {
                                                text_error(null, "invalid context length argument");
                                                return text_done(2);
                                        }

                                        if (flag == 'm')
                                                limit = number;
                                        else if (flag == 'A')
                                                after = number;
                                        else if (flag == 'B')
                                                before = number;
                                        else
                                        {
                                                after = number;
                                                before = number;
                                        }
                                }

                                break;
                        }

                        switch (flag)
                        {
                        case 'E': extended = true; break;
                        case 'F': fixed = true; break;
                        case 'G': extended = false; break;
                        case 'i':
                        case 'y': icase = true; break;
                        case GREP_LONG_UNFOLD: icase = false; break;
                        case 'v': invert = true; break;
                        case 'n': grep_numbered = true; break;
                        case 'b': grep_offsets = true; break;
                        case 'c': counting = true; break;
                        case 'l': listing = true; break;
                        case 'L': listing_without = true; break;
                        case 'o': only = true; break;
                        case 'q': quiet = true; break;
                        case 'h': no_names = true; break;
                        case 'H': with_names = true; break;
                        case 's': quietly = true; break;
                        case 'x': whole_line = true; break;
                        case 'w': whole_word = true; break;
                        case 'T': grep_tabbed = true; break;
                        case 'r': grep_recursive = true; break;
                        case 'R':
                                grep_recursive = true;
                                grep_dereference = true;
                                break;
                        case GREP_LONG_COLOUR: break;
                        // -Z ends the file name, -z ends the line, and a
                        // caller may well want both.
                        case 'Z': grep_null_name = true; break;
                        case 'z': null_data = true; break;
                        // Everything here is bytes already: -a says read a
                        // binary file as text and -U says do not undo line
                        // endings, and neither describes anything this does.
                        case 'a':
                        case 'I':
                        case 'U':
                        case GREP_LONG_BUFFERED: break;
                        case GREP_LONG_NO_SEPARATOR: separator = null; break;
                        default:
                        {
                                p8 named[3] = {'-', flag, 0};

                                text_error(named, "invalid option");
                                return text_done(2);
                        }
                        }
                }
        }

        if (pattern_from >= 0 && !grep_pattern_any && !never)
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
        if ((whole_line || whole_word) && !never)
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

        if (!never && !regex_compile(grep_pattern, extended, icase, false))
        {
                text_error(null, "invalid regular expression");
                return text_done(2);
        }

        if (before && !grep_hold_make(before))
                return text_done(2);

        // Here rather than in the switch: -f has already been read above with
        // this same reader, and GNU splits a pattern file on newlines however
        // the input is going to be split.
        if (null_data)
                text_delimiter = '\0';

        // -m0 can match nothing, and a -f file with no patterns in it matches
        // nothing either. GNU answers both before it opens a single file --
        // measured: grep -c -f /dev/null prints no count and exits 1, and it
        // never complains about a file that is not there. -v is the one way
        // back in, because nothing is what every line then differs from.
        bool grouped = before || after;

        if ((limit == 0 || never) && !invert)
                return text_done(1);

        // Nothing named and no -r is the one way standard input is read;
        // -r with nothing named walks the working directory instead.
        bool from_stdin = !text_files_count && !grep_recursive;
        bool found_any = false;
        bool shown_any = false;
        b32 trouble = 0;

        grep_paths = (string_address address_to)text_arena_take(
            GREP_PATHS_MAX * sizeof(string_address));

        if (!grep_paths)
                return text_done(2);

        if (grep_recursive && !text_files_count)
        {
                grep_expanded = true;
                grep_walk((string_address) "");
        }

        for (b32 i = 0; i < text_files_count; i++)
        {
                string_address name = text_argument(text_files[i]);
                p32 mode = grep_recursive ? text_path_mode(name) : 0;

                if (grep_recursive && !mode)
                {
                        trouble = 2;

                        if (!quietly)
                                text_error(name, "No such file or directory");

                        continue;
                }

                if (grep_recursive && (mode & 0170000) == 0040000)
                {
                        if (!grep_wanted_directory(name))
                                continue;

                        grep_expanded = true;
                        grep_walk(name);
                        continue;
                }

                if (grep_wanted_file(name))
                        grep_path_add(name);
        }

        b32 inputs = from_stdin ? 1 : (b32)grep_path_count;

        grep_names = (grep_path_count > 1 || with_names || grep_expanded) && !no_names;

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = from_stdin ? null : grep_paths[i];
                positive matches = 0;
                positive number = 0;
                positive offset = 0;
                positive pending = 0;
                positive shown = 0;
                bool split = shown_any;

                if (!text_open(name))
                {
                        trouble = 2;

                        if (quietly)
                                text_status = 0;

                        continue;
                }

                // A directory reads as EISDIR rather than as bytes, which is
                // where GNU's message comes from and why -d skip has one to
                // suppress.
                if (name && (text_path_mode(name) & 0170000) == 0040000)
                {
                        text_close();

                        if (grep_skip_directories)
                                continue;

                        text_error(name, "Is a directory");
                        trouble = 2;
                        continue;
                }

                // GNU labels standard input, and -H on a pipe is the one way
                // to see it: the name is a real string everywhere below, so
                // nothing has to remember that it might not be.
                string_address shown_name = name ? name
                                                 : (label ? label
                                                          : (string_address) "(standard input)");

                if (grep_tabbed)
                {
                        positive size = 0;

                        grep_column = 19;

                        if (text_regular_size(text_input.handle, address_of size))
                        {
                                positive digits = 1;
                                positive scale = 10;

                                while (size >= scale)
                                {
                                        digits++;
                                        scale *= 10;
                                }

                                grep_column = digits;
                        }
                }

                grep_hold_clear();

                while (text_line_next())
                {
                        number++;

                        positive at = offset;

                        offset += text_line_length + (text_line_ended ? 1 : 0);

                        bool hit = !never &&
                                   regex_search(text_line, text_line_length, 0);

                        if (hit == invert)
                        {
                                if (pending)
                                {
                                        if (grouped && separator &&
                                            (split || (shown && number > shown + 1)))
                                        {
                                                text_put_string(separator);
                                                text_put_character('\n');
                                        }

                                        split = false;
                                        grep_head(shown_name, '-', number, at);
                                        text_put_line();

                                        if (!text_line_ended)
                                                text_put_character(text_delimiter);

                                        shown = number;
                                        shown_any = true;
                                        pending--;
                                }
                                else if (before)
                                {
                                        grep_hold_put(text_line, text_line_length, number);
                                }

                                if (limit != TEXT_UNSET && matches >= limit && !pending)
                                        break;

                                continue;
                        }

                        matches++;
                        found_any = true;

                        if (quiet)
                        {
                                text_close();
                                return text_done(0);
                        }

                        if (listing || listing_without)
                                break;

                        if (counting)
                        {
                                if (limit != TEXT_UNSET && matches >= limit)
                                        break;

                                continue;
                        }

                        if (before)
                        {
                                positive want = number > before ? number - before : 1;

                                for (positive k = 0; k < grep_hold_count; k++)
                                {
                                        positive slot = (grep_hold_first + k) % grep_hold_slots;
                                        positive n = grep_hold_number[slot];

                                        if (n < want || (shown && n <= shown))
                                                continue;

                                        if (grouped && separator &&
                                            (split || (shown && n > shown + 1)))
                                        {
                                                text_put_string(separator);
                                                text_put_character('\n');
                                        }

                                        split = false;
                                        grep_head(shown_name, '-', n, 0);
                                        grep_hold_say(slot);
                                        text_put_character(text_delimiter);
                                        shown = n;
                                        shown_any = true;
                                }
                        }

                        if (only)
                        {
                                positive from = 0;

                                while (from <= text_line_length &&
                                       regex_search_longest(text_line, text_line_length, from))
                                {
                                        positive begin = regex_slots[0];
                                        positive stop = regex_slots[1];

                                        if (stop == begin)
                                        {
                                                from = begin + 1;
                                                continue;
                                        }

                                        if (!invert)
                                        {
                                                grep_head(shown_name, ':', number, at + begin);
                                                text_put(text_line + begin, stop - begin);
                                                text_put_character(text_delimiter);
                                        }

                                        from = stop;
                                }

                                shown = number;
                                shown_any = true;
                                pending = after;

                                if (limit != TEXT_UNSET && matches >= limit && !pending)
                                        break;

                                continue;
                        }

                        if (grouped && separator &&
                            (split || (shown && number > shown + 1)))
                        {
                                text_put_string(separator);
                                text_put_character('\n');
                        }

                        split = false;
                        grep_head(shown_name, ':', number, at);
                        text_put_line();

                        if (!text_line_ended)
                                text_put_character(text_delimiter);

                        shown = number;
                        shown_any = true;
                        pending = after;

                        if (limit != TEXT_UNSET && matches >= limit && !pending)
                                break;
                }

                text_close();

                if (listing || listing_without)
                {
                        if ((matches != 0) == listing)
                        {
                                text_put_string(shown_name);
                                text_put_character(grep_null_name ? '\0' : '\n');
                        }

                        continue;
                }

                if (counting)
                {
                        if (grep_names)
                        {
                                text_put_string(shown_name);
                                text_put_character(grep_null_name ? '\0' : ':');
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
        range of any two of those, first~step, ,+N, ,~N, and ! -- and the
        commands s y p P d D q Q F = n N h H g G x a i c r w b t T :, with
        { } around any of them. A label is an index into the same flat array,
        which is what makes b and t an assignment to the counter.

        What is not: l, which is a way of looking at a line rather than a way
        of changing one, and e, which runs a shell.
*/
#define SED_COMMANDS_MAX 256
#define SED_PROGRAMS_MAX 32
#define SED_TEXT_MAX 16384
#define SED_MAPS_MAX 8
#define SED_SCRIPT_MAX 16384
#define SED_FILES_MAX 16
#define SED_APPENDS_MAX 32

enum
{
        SED_ADDRESS_NONE = 0,
        SED_ADDRESS_LINE,
        SED_ADDRESS_LAST,
        SED_ADDRESS_REGEX,
        // first~step, and the two an address range can end with: ,+N lines
        // further on and ,~N at the next line number N divides.
        SED_ADDRESS_STEP,
        SED_ADDRESS_AHEAD,
        SED_ADDRESS_MULTIPLE
};

typedef struct
{
        p8 kind;
        p8 first_type;
        p8 second_type;
        bool negate;
        bool active;
        // 0,/re/ is the one range that is open before its first line is read,
        // which is what lets its end match on line one.
        bool begins;
        positive first_line;
        positive first_step;
        positive second_line;
        positive stop;
        b32 first_regex;
        b32 second_regex;
        b32 pattern;
        b32 map;
        b32 text;
        b32 writer;
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
// A script that cannot be parsed is one, and a label nothing defines is four.
static b32 sed_broken_status = 1;

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
static bool sed_replaced;

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

/*
        The files w writes to, opened once and truncated once.

        Two w commands naming the same file are one file: opening it again
        for the second would put its output at the front of what the first had
        already written.
*/
typedef struct
{
        b32 name;
        bipolar handle;
} sed_file;

static sed_file sed_files[SED_FILES_MAX];
static b32 sed_file_count;

static b32 sed_file_of(p8 address_to name, positive length)
{
        for (b32 i = 0; i < sed_file_count; i++)
        {
                string_address had = sed_text + sed_files[i].name;
                positive c = 0;

                while (c < length && had[c] && had[c] == name[c])
                        c++;

                if (c == length && !had[c])
                        return i;
        }

        if (sed_file_count >= SED_FILES_MAX)
        {
                sed_broken = true;
                return 0;
        }

        b32 which = sed_file_count++;

        sed_files[which].name = sed_text_add(name, length);
        sed_files[which].handle = -1;
        return which;
}

static bool sed_extended;
static bool sed_separate;
static bool sed_null_data;
static string_address sed_in_place;

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

        if (!regex_compile(pattern, sed_extended, icase, true))
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

static positive sed_number_at()
{
        positive value = 0;

        while (text_digit(sed_peek()))
        {
                value = value * 10 + (positive)(sed_peek() - '0');
                sed_at++;
        }

        return value;
}

// Everything left on the line, which is what a file name is to r and w: a
// semicolon in one is part of the name and not the end of the command.
static positive sed_rest_of_line(p8 address_to into, positive room)
{
        positive have = 0;

        sed_skip_blanks();

        while (sed_at < sed_script_length && sed_script[sed_at] != '\n')
        {
                if (have < room - 1)
                        into[have++] = sed_script[sed_at];

                sed_at++;
        }

        into[have] = '\0';
        return have;
}

// A label, which ends where the command it names would have started.
static positive sed_label_of(p8 address_to into, positive room)
{
        positive have = 0;

        sed_skip_blanks();

        while (sed_at < sed_script_length && sed_script[sed_at] != '\n' &&
               sed_script[sed_at] != ';' && sed_script[sed_at] != '}')
        {
                if (have < room - 1)
                        into[have++] = sed_script[sed_at];

                sed_at++;
        }

        while (have && text_blank(into[have - 1]))
                have--;

        into[have] = '\0';
        return have;
}

static bool sed_parse_address(p8 address_to type, positive address_to line,
                              b32 address_to which, positive address_to step,
                              bool second)
{
        p8 character = sed_peek();

        if (second && (character == '+' || character == '~'))
        {
                sed_at++;
                address_to type = character == '+' ? SED_ADDRESS_AHEAD
                                                   : SED_ADDRESS_MULTIPLE;
                address_to line = sed_number_at();
                return true;
        }

        if (text_digit(character))
        {
                positive value = sed_number_at();

                if (!second && sed_peek() == '~')
                {
                        sed_at++;
                        address_to type = SED_ADDRESS_STEP;
                        address_to line = value;
                        address_to step = sed_number_at();
                        return true;
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
                command->begins = false;
                command->first_step = 0;
                command->writer = -1;
                command->which = 1;

                if (sed_parse_address(address_of command->first_type,
                                      address_of command->first_line,
                                      address_of command->first_regex,
                                      address_of command->first_step, false))
                {
                        sed_skip_blanks();

                        if (sed_peek() == ',')
                        {
                                sed_at++;
                                sed_skip_blanks();

                                if (!sed_parse_address(address_of command->second_type,
                                                       address_of command->second_line,
                                                       address_of command->second_regex,
                                                       address_of command->first_step, true))
                                {
                                        sed_broken = true;
                                        return;
                                }
                        }

                        // Line zero is not a line, so it is only an address at
                        // all as the open end of a range a pattern closes --
                        // or with a step, which makes it every step'th line.
                        if (command->first_type == SED_ADDRESS_STEP &&
                            !command->first_line && !command->first_step)
                        {
                                sed_broken = true;
                                return;
                        }

                        if (command->first_type == SED_ADDRESS_LINE &&
                            !command->first_line)
                        {
                                if (command->second_type != SED_ADDRESS_REGEX)
                                {
                                        sed_broken = true;
                                        return;
                                }

                                command->begins = true;
                                command->active = true;
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
                        command->writer = -1;
                        command->which = 0;

                        for (;;)
                        {
                                p8 flag = sed_peek();

                                if (flag == 'w')
                                {
                                        p8 name[TEXT_PATH_MAX];

                                        sed_at++;
                                        command->writer = sed_file_of(
                                            name, sed_rest_of_line(name, sizeof(name)));
                                        break;
                                }

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

                if (kind == 'q' || kind == 'Q')
                {
                        sed_skip_blanks();
                        command->which = sed_number_at();
                        sed_command_count++;
                        continue;
                }

                if (kind == 'r' || kind == 'w')
                {
                        p8 name[TEXT_PATH_MAX];
                        positive have = sed_rest_of_line(name, sizeof(name));

                        if (!have)
                        {
                                sed_broken = true;
                                return;
                        }

                        if (kind == 'w')
                                command->writer = sed_file_of(name, have);
                        else
                                command->text = sed_text_add(name, have);

                        sed_command_count++;
                        continue;
                }

                if (kind == ':' || kind == 'b' || kind == 't' || kind == 'T')
                {
                        p8 name[128];
                        positive have = sed_label_of(name, sizeof(name));

                        if (kind == ':' && !have)
                        {
                                sed_broken = true;
                                return;
                        }

                        command->text = sed_text_add(name, have);
                        sed_command_count++;
                        continue;
                }

                if (kind == 'p' || kind == 'P' || kind == 'd' || kind == 'D' ||
                    kind == '=' || kind == 'n' || kind == 'N' || kind == 'h' ||
                    kind == 'H' || kind == 'g' || kind == 'G' || kind == 'x' ||
                    kind == 'F')
                {
                        sed_command_count++;
                        continue;
                }

                sed_broken = true;
                return;
        }

        // Labels become indexes now that every command has one. A branch with
        // no label named goes to the end of the script, which is where the
        // cycle would have ended anyway.
        for (b32 i = 0; i < sed_command_count; i++)
        {
                sed_command address_to command = sed_commands + i;

                if (command->kind != 'b' && command->kind != 't' &&
                    command->kind != 'T')
                        continue;

                string_address want = sed_text + command->text;

                command->which = (positive)sed_command_count;

                if (!want[0])
                        continue;

                bool found = false;

                for (b32 c = 0; c < sed_command_count; c++)
                        if (sed_commands[c].kind == ':' &&
                            string_equals(sed_text + sed_commands[c].text, want))
                        {
                                command->which = (positive)c;
                                found = true;
                                break;
                        }

                if (!found)
                {
                        sed_broken = true;
                        sed_broken_status = 4;
                        return;
                }
        }
}

static bool sed_address_matches(p8 type, positive line, b32 which, positive step)
{
        if (type == SED_ADDRESS_LINE)
                return sed_number == line;

        if (type == SED_ADDRESS_STEP)
                return step ? sed_number >= line && !((sed_number - line) % step)
                            : sed_number == line;

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
                                             command->first_regex, command->first_step);
        }
        else if (!command->active)
        {
                positive by = command->second_line;
                bool counted = command->second_type == SED_ADDRESS_LINE ||
                               command->second_type == SED_ADDRESS_AHEAD ||
                               command->second_type == SED_ADDRESS_MULTIPLE;

                answer = sed_address_matches(command->first_type, command->first_line,
                                             command->first_regex, command->first_step);

                if (answer)
                {
                        command->active = true;

                        if (command->second_type == SED_ADDRESS_LINE)
                                command->stop = by;
                        else if (command->second_type == SED_ADDRESS_AHEAD)
                                command->stop = sed_number + by;
                        else if (command->second_type == SED_ADDRESS_MULTIPLE)
                                command->stop = by ? sed_number + by - sed_number % by
                                                   : sed_number;

                        // A range whose end is a line already passed is one
                        // line long, which is the only way the end can be
                        // decided without seeing another line.
                        if (counted && command->stop <= sed_number)
                                command->active = false;
                }
        }
        else
        {
                bool counted = command->second_type == SED_ADDRESS_LINE ||
                               command->second_type == SED_ADDRESS_AHEAD ||
                               command->second_type == SED_ADDRESS_MULTIPLE;

                answer = true;

                if (counted)
                {
                        if (sed_number >= command->stop)
                                command->active = false;
                }
                else if (sed_address_matches(command->second_type, command->second_line,
                                             command->second_regex, 0))
                {
                        command->active = false;
                }
        }

        return command->negate ? !answer : answer;
}

static fn sed_put_space()
{
        text_put(sed_space, sed_space_length);
        text_put_character(text_delimiter);
}

// /dev/stdout is sed's own output, and a second descriptor onto it would put
// what w wrote somewhere other than where the buffer had reached.
static fn sed_write_space(b32 which)
{
        string_address name = sed_text + sed_files[which].name;
        p8 mark = text_delimiter;

        if (string_equals(name, "/dev/stdout"))
        {
                text_put(sed_space, sed_space_length);

                if (sed_space_ended)
                        text_put_character(text_delimiter);

                return;
        }

        if (sed_files[which].handle < 0)
        {
                sed_files[which].handle = text_open_handle(name, TEXT_WRITE, 0644);

                if (sed_files[which].handle < 0)
                {
                        text_error(name, "couldn't open file");
                        sed_failed = true;
                        return;
                }
        }

        text_write_raw((positive)sed_files[which].handle, sed_space, sed_space_length);

        // A last line that came without one does not leave with one.
        if (sed_space_ended)
                text_write_raw((positive)sed_files[which].handle, address_of mark, 1);
}

// What r names, whole, wherever the cycle's output had reached. A name that
// is not there is nothing at all rather than a complaint.
static fn sed_put_file(string_address name)
{
        p8 window[8192];
        bipolar handle = text_open_handle(name, FILE_READ, 0);

        if (handle < 0)
                return;

        for (;;)
        {
                bipolar got = system_call_3(syscall(read), (positive)handle,
                                            (positive)window, sizeof(window));

                if (got <= 0)
                        break;

                text_put(window, (positive)got);
        }

        system_call_1(syscall(close), (positive)handle);
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

/*
        The long spellings sed answers to.

        --sandbox refuses e, r and w, which are three commands this sed does
        not have, so refusing them is what it already does.

        Not here, and deliberately: --debug, which prints the parsed program
        and then a trace of every line through it; --null-data, which is the
        line reader's business rather than sed's; and --follow-symlinks, which
        only means anything with -i and would need a path resolved to keep its
        promise -- claiming it and then replacing the link would be worse than
        not claiming it.
*/
enum
{
        SED_LONG_QUIET = 1,
        SED_LONG_INPLACE = 2
};

static text_long sed_long_options[] = {
    {"quiet", 'n', TEXT_LONG_ALONE},
    {"silent", 'n', TEXT_LONG_ALONE},
    {"expression", 'e', TEXT_LONG_NEEDS},
    {"file", 'f', TEXT_LONG_NEEDS},
    {"in-place", 'i', TEXT_LONG_MAYBE, SED_LONG_INPLACE},
    {"line-length", 'l', TEXT_LONG_NEEDS},
    {"regexp-extended", 'E', TEXT_LONG_ALONE},
    {"separate", 's', TEXT_LONG_ALONE},
    {"unbuffered", 'u', TEXT_LONG_ALONE},
    {"null-data", 'z', TEXT_LONG_ALONE},
    {"posix", SED_LONG_QUIET, TEXT_LONG_ALONE},
    {"sandbox", SED_LONG_QUIET, TEXT_LONG_ALONE},
    {null, 0, 0}};

/*
        Where -i writes before it is allowed to be the file.

        Beside the original rather than in a temporary directory, because the
        rename at the end has to stay inside one filesystem to be a rename at
        all. renameat2 rather than renameat, for the reason file.c gives where
        mv does the same thing: riscv64 never had renameat, and a flags word
        of zero is the same operation.
*/
static bool sed_temporary(string_address name, p8 address_to into, positive slot)
{
        positive length = string_length(name);
        positive cut = 0;

        for (positive i = 0; i < length; i++)
                if (name[i] == '/')
                        cut = i + 1;

        if (cut + 24 >= TEXT_PATH_MAX)
                return false;

        memory_copy(into, name, cut);

        positive at = cut;
        positive value = (positive)system_call_1(syscall(getpid), 0) * 31 + slot;

        into[at++] = 's';
        into[at++] = 'e';
        into[at++] = 'd';

        p8 digits[24];
        positive have = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (have)
                into[at++] = digits[--have];

        into[at] = '\0';
        return true;
}

static b32 text_sed()
{
        bool have_script = false;
        b32 leaving = -1;

        text_begin("sed");

        if (!text_expand_long(sed_long_options))
                return text_done(1);

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

                        if (flag == 'r' || flag == 'E')
                        {
                                sed_extended = true;
                                continue;
                        }

                        if (flag == 's')
                        {
                                sed_separate = true;
                                continue;
                        }

                        if (flag == 'z')
                        {
                                sed_null_data = true;
                                continue;
                        }

                        // -u asks for output a line at a time, which costs
                        // something only when somebody is reading it live;
                        // --posix and --sandbox both take away what is not
                        // here to take.
                        if (flag == 'u' || flag == SED_LONG_QUIET)
                                continue;

                        // -i takes its suffix joined on -- sed -in is in place
                        // with a backup called n, not -i -n -- so it reads the
                        // rest of the argument rather than the next one.
                        if (flag == 'i')
                        {
                                sed_in_place = argument[c + 1] ? argument + c + 1
                                                               : (string_address) "";
                                sed_separate = true;
                                break;
                        }

                        if (flag == 'e' || flag == 'f' || flag == 'l' ||
                            flag == SED_LONG_INPLACE)
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
                                        have_script = true;
                                }
                                else if (flag == SED_LONG_INPLACE)
                                {
                                        sed_in_place = value;
                                        sed_separate = true;
                                }
                                // -l is how wide the l command wraps, and
                                // there is no l command here to wrap.
                                else if (flag == 'l')
                                {
                                }
                                else
                                {
                                        // Four, not one: sed keeps its usage
                                        // errors and its I/O failures apart,
                                        // and a script file that is not there
                                        // is the second kind.
                                        if (!text_open(value))
                                                return text_done(4);

                                        while (text_line_next())
                                        {
                                                text_line[text_line_length] = '\0';
                                                sed_script_add(text_line);
                                        }

                                        text_close();
                                        have_script = true;
                                }

                                break;
                        }

                        {
                                p8 named[3] = {'-', flag, 0};

                                text_error(named, "invalid option");
                                return text_done(1);
                        }
                }
        }

        if (!have_script)
        {
                text_error(null, "no script");
                return text_done(1);
        }

        // After the script has been read, not while: -f reads its file with
        // the same reader and a script is lines however the input is split.
        if (sed_null_data)
                text_delimiter = '\0';

        sed_parse();

        if (sed_broken)
        {
                text_error(null, "unsupported or invalid script");
                return text_done(sed_broken_status);
        }

        // -i edits files, and there is nothing to edit when the input is a
        // pipe. GNU says so and stops with four.
        if (sed_in_place && !text_files_count)
        {
                text_error(null, "no input files");
                return text_done(4);
        }

        b32 inputs = text_files_count ? text_files_count : 1;

        for (b32 i = 0; i < inputs && leaving < 0; i++)
        {
                string_address name = text_files_count ? text_argument(text_files[i]) : null;
                p8 temporary[TEXT_PATH_MAX];
                bipolar written = -1;

                if (!text_open(name))
                {
                        text_status = 2;
                        continue;
                }

                if (sed_in_place)
                {
                        if (!sed_temporary(name, temporary, (positive)i))
                        {
                                text_close();
                                text_error(name, "cannot make a temporary file beside");
                                return text_done(4);
                        }

                        // O_EXCL, so a name that is somehow already taken is a
                        // failure rather than somebody else's file truncated.
                        written = text_open_handle(temporary, 01 | 0100 | 0200, 0600);

                        if (written < 0)
                        {
                                text_close();
                                text_error(temporary, "cannot create");
                                return text_done(4);
                        }

                        text_out_to((positive)written);
                }

                // -s, and -i with it, makes every file its own input: the line
                // numbers start again and $ is that file's last line.
                if (sed_separate)
                {
                        sed_number = 0;

                        for (b32 c = 0; c < sed_command_count; c++)
                                if (sed_commands[c].begins)
                                        sed_commands[c].active = true;
                }

                while (text_line_next())
                {
                        memory_copy(sed_space, text_line, text_line_length);
                        sed_space_length = text_line_length;
                        sed_space_ended = text_line_ended;
                        sed_number++;
                        sed_last = !text_fill() && (sed_separate || i == inputs - 1);

                        bool dropped = false;
                        b32 pc = 0;
                        p8 append_kind[SED_APPENDS_MAX];
                        b32 append_which[SED_APPENDS_MAX];
                        b32 append_count = 0;

                        sed_replaced = false;

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
                                        if (!sed_substitute(command))
                                                continue;

                                        sed_replaced = true;

                                        if (command->printing)
                                                sed_put_space();

                                        if (command->writer >= 0)
                                                sed_write_space(command->writer);

                                        continue;
                                }

                                if (kind == 'w')
                                {
                                        sed_write_space(command->writer);
                                        continue;
                                }

                                if (kind == 'r')
                                {
                                        if (append_count < SED_APPENDS_MAX)
                                        {
                                                append_kind[append_count] = 'r';
                                                append_which[append_count++] = command->text;
                                        }

                                        continue;
                                }

                                if (kind == ':')
                                        continue;

                                // t and T ask whether a substitution has taken
                                // since the line was read, and asking is what
                                // clears the answer.
                                if (kind == 'b' || kind == 't' || kind == 'T')
                                {
                                        bool take = kind == 'b' ||
                                                    (kind == 't') == sed_replaced;

                                        if (kind != 'b')
                                                sed_replaced = false;

                                        if (take)
                                                pc = (b32)command->which;

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
                                        text_put_character(text_delimiter);
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

                                if (kind == 'q' || kind == 'Q')
                                {
                                        leaving = (b32)command->which;
                                        dropped = kind == 'Q';
                                        break;
                                }

                                if (kind == 'F')
                                {
                                        text_put_string(name ? name
                                                             : (string_address) "-");
                                        text_put_character('\n');
                                        continue;
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
                                        sed_replaced = false;
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
                                        if (append_count < SED_APPENDS_MAX)
                                        {
                                                append_kind[append_count] = 'a';
                                                append_which[append_count++] = command->text;
                                        }

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
                                        text_put_character(text_delimiter);
                        }

                        for (b32 c = 0; c < append_count; c++)
                        {
                                if (append_kind[c] == 'r')
                                {
                                        sed_put_file(sed_text + append_which[c]);
                                        continue;
                                }

                                text_put_string(sed_text + append_which[c]);
                                text_put_character('\n');
                        }

                        if (leaving >= 0 || sed_failed)
                                break;
                }

                text_close();

                if (written >= 0)
                {
                        text_flush();
                        text_out_to(1);
                        system_call_1(syscall(close), (positive)written);

                        if (sed_in_place[0])
                        {
                                p8 kept[TEXT_PATH_MAX];
                                positive length = string_length(name);
                                positive extra = string_length(sed_in_place);

                                if (length + extra + 1 < TEXT_PATH_MAX)
                                {
                                        memory_copy(kept, name, length);
                                        memory_copy(kept + length, sed_in_place, extra);
                                        kept[length + extra] = '\0';
                                        system_call_5(syscall(renameat2), (positive)(bipolar)AT_FDCWD,
                                                      (positive)name, (positive)(bipolar)AT_FDCWD,
                                                      (positive)kept, 0);
                                }
                        }

                        system_call_5(syscall(renameat2), (positive)(bipolar)AT_FDCWD,
                                      (positive)temporary, (positive)(bipolar)AT_FDCWD,
                                      (positive)name, 0);
                }

                if (sed_failed)
                        break;
        }

        for (b32 c = 0; c < sed_file_count; c++)
                if (sed_files[c].handle >= 0)
                        system_call_1(syscall(close), (positive)sed_files[c].handle);

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

/*
        Which of the orderings a key is in, and which bytes it never sees.

        -n -g -h -M -V are one answer each and the last one written wins, the
        way GNU takes them. -d and -i are not orderings at all: they say which
        bytes the comparison is allowed to look at, so they sit beside the
        ordering rather than instead of it, and -f joins them.
*/
enum
{
        SORT_FOLD = 1,
        SORT_DICTIONARY = 2,
        SORT_PRINTABLE = 4
};

typedef struct
{
        positive first_field;
        positive first_char;
        positive second_field;
        positive second_char;
        p8 kind;
        positive how;
        bool reverse;
        bool skip_blanks_first;
        bool skip_blanks_second;
        bool given;
} sort_key;

static sort_key sort_keys[SORT_KEYS_MAX];
static b32 sort_key_count;
static p8 sort_kind;
static positive sort_how;
static bool sort_reverse;
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

// -d keeps blanks and letters and digits; -i keeps what a terminal would
// show. A byte neither keeps is not there at all, so the two sides walk at
// their own pace rather than in step.
static bool sort_letter_or_digit(p8 character)
{
        return text_digit(character) ||
               (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z');
}

static bool sort_looked_at(p8 character, positive how)
{
        // Alphanumeric, not a word character: sort -d keeps no underscore,
        // which is what separates it from every other definition here.
        if ((how & SORT_DICTIONARY) &&
            !(text_blank(character) || sort_letter_or_digit(character)))
                return false;

        if ((how & SORT_PRINTABLE) && (character < 0x20 || character >= 0x7f))
                return false;

        return true;
}

static bipolar sort_compare_bytes(p8 address_to a, positive la, p8 address_to b, positive lb,
                                  positive how)
{
        positive i = 0;
        positive j = 0;

        for (;;)
        {
                if (how & (SORT_DICTIONARY | SORT_PRINTABLE))
                {
                        while (i < la && !sort_looked_at(a[i], how))
                                i++;

                        while (j < lb && !sort_looked_at(b[j], how))
                                j++;
                }

                if (i == la || j == lb)
                        break;

                p8 one = a[i++];
                p8 two = b[j++];

                if (how & SORT_FOLD)
                {
                        one = one >= 'a' && one <= 'z' ? (p8)(one - 32) : one;
                        two = two >= 'a' && two <= 'z' ? (p8)(two - 32) : two;
                }

                if (one != two)
                        return one < two ? -1 : 1;
        }

        if (i == la && j == lb)
                return 0;

        return i == la ? -1 : 1;
}

/*
        -h, which is one comparison of the suffix and then an ordinary one.

        The suffix outranks the digits -- 2K is larger than 1000 -- but only
        when there were digits and they were not all zero, so 0K and 0 and a
        word with no number in it all rank together and the numeric compare
        underneath decides between them. Measured: GNU sort -h leaves K6 after
        0, which no reading of "K is a thousand" produces on its own.
*/
static bipolar sort_human_order(p8 address_to at, positive length)
{
        positive scan = 0;
        bipolar sign = 1;
        bool nonzero = false;

        while (scan < length && text_blank(at[scan]))
                scan++;

        // Only a minus: GNU reads +7 as no number at all, here as in every
        // other number this file reads.
        if (scan < length && at[scan] == '-')
        {
                sign = -1;
                scan++;
        }

        // The increment is its own statement. Written into the test it stops
        // happening the moment nonzero is true, and the loop never ends.
        while (scan < length && text_digit(at[scan]))
        {
                if (at[scan] != '0')
                        nonzero = true;

                scan++;
        }

        // The point is stepped over whether or not digits follow it, so 25.G
        // is twenty-five giga and not twenty-five: measured, and the reason
        // the suffix is looked for after the point rather than before it.
        if (scan < length && at[scan] == '.')
        {
                scan++;

                while (scan < length && text_digit(at[scan]))
                {
                        if (at[scan] != '0')
                                nonzero = true;

                        scan++;
                }
        }

        if (!nonzero)
                return 0;

        switch (scan < length ? at[scan] : 0)
        {
        case 'k':
        case 'K': return sign * 1;
        case 'M': return sign * 2;
        case 'G': return sign * 3;
        case 'T': return sign * 4;
        case 'P': return sign * 5;
        case 'E': return sign * 6;
        case 'Z': return sign * 7;
        case 'Y': return sign * 8;
        default: return 0;
        }
}

static bipolar sort_compare_human(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        bipolar one = sort_human_order(a, la);
        bipolar two = sort_human_order(b, lb);

        if (one != two)
                return one < two ? -1 : 1;

        return sort_compare_number(a, la, b, lb);
}

// -M, in the C locale, which is the only one this has. Anything that is not
// one of the twelve is month zero and ties with every other such line, so the
// last resort is what actually orders the text.
static string_address sort_months[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static b32 sort_month_of(p8 address_to at, positive length)
{
        positive scan = 0;

        while (scan < length && text_blank(at[scan]))
                scan++;

        if (length - scan < 3)
                return 0;

        for (b32 m = 0; m < 12; m++)
        {
                b32 c = 0;

                for (; c < 3; c++)
                {
                        p8 one = at[scan + c];

                        one = one >= 'a' && one <= 'z' ? (p8)(one - 32) : one;

                        if (one != sort_months[m][c])
                                break;
                }

                if (c == 3)
                        return m + 1;
        }

        return 0;
}

static bipolar sort_compare_month(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        b32 one = sort_month_of(a, la);
        b32 two = sort_month_of(b, lb);

        if (one == two)
                return 0;

        return one < two ? -1 : 1;
}

/*
        -V, which is Debian's version comparison and not a sort at all.

        A digit run is compared as a number with its leading zeros gone, and
        everything else a byte at a time under an order where '~' comes before
        the end of the string, a letter comes before every other punctuation,
        and a digit interrupts. That is what makes 1.9 come before 1.10 and
        1.0~rc1 before 1.0.

        On top of it, a file suffix -- a run of .name pieces at the end where
        each begins with a letter -- is set aside and the stems compared
        first, so foo.tar.gz and foo2.tar.gz order by foo against foo2. A
        version number's dots do not qualify, because .10 does not begin with
        a letter, which is the whole reason the rule is shaped that way.
*/
static b32 sort_version_order(p8 character)
{
        if (text_digit(character))
                return 0;

        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z'))
                return (b32)character;

        if (character == '~')
                return -1;

        return (b32)character + 256;
}

static bipolar sort_version_walk(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        positive i = 0;
        positive j = 0;

        while (i < la || j < lb)
        {
                bipolar first = 0;

                while ((i < la && !text_digit(a[i])) || (j < lb && !text_digit(b[j])))
                {
                        b32 one = i < la ? sort_version_order(a[i]) : 0;
                        b32 two = j < lb ? sort_version_order(b[j]) : 0;

                        if (one != two)
                                return one < two ? -1 : 1;

                        i++;
                        j++;
                }

                while (i < la && a[i] == '0')
                        i++;

                while (j < lb && b[j] == '0')
                        j++;

                while (i < la && j < lb && text_digit(a[i]) && text_digit(b[j]))
                {
                        if (!first)
                                first = (bipolar)a[i] - (bipolar)b[j];

                        i++;
                        j++;
                }

                if (i < la && text_digit(a[i]))
                        return 1;

                if (j < lb && text_digit(b[j]))
                        return -1;

                if (first)
                        return first < 0 ? -1 : 1;
        }

        return 0;
}

static bool sort_version_tail(p8 character)
{
        return text_word(character) || character == '~';
}

static positive sort_version_stem(p8 address_to at, positive length)
{
        // From one, never zero: a name that is all suffix has no stem left to
        // compare, and .bashrc is a name rather than a suffix of nothing.
        for (positive i = 1; i < length; i++)
        {
                if (at[i] != '.')
                        continue;

                positive j = i;

                while (j < length && at[j] == '.')
                {
                        positive c = j + 1;

                        if (c >= length || text_digit(at[c]) || !sort_version_tail(at[c]))
                                break;

                        c++;

                        while (c < length && sort_version_tail(at[c]))
                                c++;

                        j = c;
                }

                if (j == length)
                        return i;
        }

        return length;
}

static bipolar sort_compare_version(p8 address_to a, positive la, p8 address_to b, positive lb)
{
        if (!la || !lb)
                return la == lb ? 0 : (la ? 1 : -1);

        // A name that begins with a dot sorts before one that does not, and
        // "." before ".." before the rest of them.
        if (a[0] == '.' || b[0] == '.')
        {
                if ((a[0] == '.') != (b[0] == '.'))
                        return a[0] == '.' ? -1 : 1;

                bool one = la == 1;
                bool two = lb == 1;

                if (one || two)
                        return one == two ? 0 : (one ? -1 : 1);

                one = la == 2 && a[1] == '.';
                two = lb == 2 && b[1] == '.';

                if (one || two)
                        return one == two ? 0 : (one ? -1 : 1);
        }

        positive stem_a = sort_version_stem(a, la);
        positive stem_b = sort_version_stem(b, lb);
        bipolar answer = sort_version_walk(a, stem_a, b, stem_b);

        if (answer || (stem_a == la && stem_b == lb))
                return answer;

        return sort_version_walk(a, la, b, lb);
}

static bipolar sort_compare_kind(p8 kind, positive how, p8 address_to a, positive la,
                                 p8 address_to b, positive lb)
{
        if (kind == 'n')
                return sort_compare_number(a, la, b, lb);

        if (kind == 'h')
                return sort_compare_human(a, la, b, lb);

        if (kind == 'M')
                return sort_compare_month(a, la, b, lb);

        if (kind == 'V')
                return sort_compare_version(a, la, b, lb);

        return sort_compare_bytes(a, la, b, lb, how);
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

                bipolar answer = sort_compare_kind(key->kind, key->how,
                                                   a->at + from_a, to_a - from_a,
                                                   b->at + from_b, to_b - from_b);

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

                bipolar answer = sort_compare_kind(sort_kind, sort_how,
                                                   a->at + from_a, a->length - from_a,
                                                   b->at + from_b, b->length - from_b);

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

        answer = sort_compare_bytes(a->at, a->length, b->at, b->length, 0);
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
        key->kind = sort_kind;
        key->how = sort_how;
        key->reverse = sort_reverse;
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
                if (spec[at] == 'n' || spec[at] == 'h' ||
                    spec[at] == 'M' || spec[at] == 'V')
                        key->kind = spec[at];
                else if (spec[at] == 'r')
                        key->reverse = true;
                else if (spec[at] == 'f')
                        key->how |= SORT_FOLD;
                else if (spec[at] == 'd')
                        key->how |= SORT_DICTIONARY;
                else if (spec[at] == 'i')
                        key->how |= SORT_PRINTABLE;
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
                        if (spec[at] == 'n' || spec[at] == 'h' ||
                            spec[at] == 'M' || spec[at] == 'V')
                                key->kind = spec[at];
                        else if (spec[at] == 'r')
                                key->reverse = true;
                        else if (spec[at] == 'f')
                                key->how |= SORT_FOLD;
                        else if (spec[at] == 'd')
                                key->how |= SORT_DICTIONARY;
                        else if (spec[at] == 'i')
                                key->how |= SORT_PRINTABLE;
                        else if (spec[at] == 'b')
                                key->skip_blanks_second = true;

                        at++;
                }
        }

        sort_key_count++;
        return true;
}

/*
        The long spellings sort answers to.

        -S, -T, --batch-size, --compress-program and --parallel are taken and
        thrown away rather than refused. Every one of them tunes how much of
        a sort is kept in memory and how much goes to a temporary file, and
        there is one arena taken once here and no temporary file at all, so
        there is nothing for them to say -- but a script that passes -S 64M
        should still get its sorted output.

        Not here, and deliberately: --debug, which annotates every line with
        which bytes the key looked at; --random-sort and --random-source,
        which need a hash nothing else here wants; and --files0-from, which
        is a list of file names in a file.
*/
enum
{
        SORT_LONG_DISCARD = 1,
        SORT_LONG_ORDER = 2,
        SORT_LONG_CHECK = 3
};

static text_long sort_long_options[] = {
    {"ignore-leading-blanks", 'b', TEXT_LONG_ALONE},
    {"dictionary-order", 'd', TEXT_LONG_ALONE},
    {"ignore-case", 'f', TEXT_LONG_ALONE},
    {"ignore-nonprinting", 'i', TEXT_LONG_ALONE},
    {"human-numeric-sort", 'h', TEXT_LONG_ALONE},
    {"month-sort", 'M', TEXT_LONG_ALONE},
    {"numeric-sort", 'n', TEXT_LONG_ALONE},
    {"reverse", 'r', TEXT_LONG_ALONE},
    {"sort", SORT_LONG_ORDER, TEXT_LONG_NEEDS},
    {"check", 'c', TEXT_LONG_MAYBE, SORT_LONG_CHECK},
    {"version-sort", 'V', TEXT_LONG_ALONE},
    {"key", 'k', TEXT_LONG_NEEDS},
    {"merge", 'm', TEXT_LONG_ALONE},
    {"output", 'o', TEXT_LONG_NEEDS},
    {"stable", 's', TEXT_LONG_ALONE},
    {"buffer-size", SORT_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"field-separator", 't', TEXT_LONG_NEEDS},
    {"temporary-directory", SORT_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"compress-program", SORT_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"batch-size", SORT_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"parallel", SORT_LONG_DISCARD, TEXT_LONG_NEEDS},
    {"unique", 'u', TEXT_LONG_ALONE},
    {"zero-terminated", 'z', TEXT_LONG_ALONE},
    {null, 0, 0}};

// "sort: -:2: disorder: apple", which is the only thing -c has to say.
static fn sort_disorder(string_address name, positive number, text_slice address_to line)
{
        text_flush();
        text_error_raw(text_name);
        text_error_raw(": ");
        text_error_raw(name);
        text_error_raw(":");

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

        while (have)
        {
                p8 one[2] = {digits[--have], 0};

                text_error_raw(one);
        }

        text_error_raw(": disorder: ");
        text_write_raw(2, line->at, line->length);
        text_error_raw("\n");
}

static b32 text_sort()
{
        bool checking = false;
        bool checking_quiet = false;
        bool merging = false;
        bool null_data = false;
        string_address output = null;

        text_begin("sort");

        if (!text_expand_long(sort_long_options))
                return text_done(2);

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

                        if (flag == 'k' || flag == 't' || flag == 'o' ||
                            flag == 'S' || flag == 'T' ||
                            flag == SORT_LONG_DISCARD || flag == SORT_LONG_ORDER ||
                            flag == SORT_LONG_CHECK)
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
                                        // One byte, or the two that spell a
                                        // NUL. Anything longer is a separator
                                        // no line can be split on -- \t among
                                        // them, which GNU refuses and which a
                                        // literal tab is the way to ask for.
                                        bool escaped = value[0] == '\\' &&
                                                       value[1] == '0' && !value[2];

                                        if (!value[0])
                                        {
                                                text_error(null, "empty tab");
                                                return text_done(2);
                                        }

                                        if (value[1] && !escaped)
                                        {
                                                text_error(value, "multi-character tab");
                                                return text_done(2);
                                        }

                                        sort_separator = escaped ? '\0' : value[0];
                                }
                                else if (flag == 'o')
                                {
                                        output = value;
                                }
                                else if (flag == SORT_LONG_CHECK)
                                {
                                        checking = true;
                                        checking_quiet = string_equals(value, "quiet") ||
                                                         string_equals(value, "silent");

                                        if (!checking_quiet &&
                                            !string_equals(value, "diagnose-first"))
                                        {
                                                text_error(value, "invalid argument for --check");
                                                return text_done(2);
                                        }
                                }
                                else if (flag == SORT_LONG_ORDER)
                                {
                                        // --sort=WORD is the long options
                                        // spelled a third way, and the word
                                        // is what the letter would have been.
                                        if (string_equals(value, "numeric"))
                                                sort_kind = 'n';
                                        else if (string_equals(value, "human-numeric"))
                                                sort_kind = 'h';
                                        else if (string_equals(value, "month"))
                                                sort_kind = 'M';
                                        else if (string_equals(value, "version"))
                                                sort_kind = 'V';
                                        else if (string_equals(value, "general-numeric") ||
                                                 string_equals(value, "random"))
                                                sort_kind = 0;
                                        else
                                        {
                                                text_error(value, "invalid argument for --sort");
                                                return text_done(1);
                                        }
                                }

                                break;
                        }

                        switch (flag)
                        {
                        case 'n':
                        case 'h':
                        case 'M':
                        case 'V': sort_kind = flag; break;
                        // -g wants a floating point number parsed, and there
                        // is no floating point anywhere in this file.
                        case 'g': break;
                        case 'r': sort_reverse = true; break;
                        case 'f': sort_how |= SORT_FOLD; break;
                        case 'd': sort_how |= SORT_DICTIONARY; break;
                        case 'i': sort_how |= SORT_PRINTABLE; break;
                        case 'b': sort_skip_blanks = true; break;
                        case 'u': sort_unique = true; break;
                        case 's': sort_stable = true; break;
                        case 'm': merging = true; break;
                        case 'z': null_data = true; break;
                        case 'c': checking = true; break;
                        case 'C':
                                checking = true;
                                checking_quiet = true;
                                break;
                        default:
                        {
                                p8 named[3] = {'-', flag, 0};

                                text_error(named, "invalid option");
                                return text_done(2);
                        }
                        }
                }
        }

        // The global flags are the default for every key, and -n after -k on
        // the command line still has to reach the key in front of it.
        for (b32 i = 0; i < sort_key_count; i++)
        {
                if (!sort_keys[i].kind)
                        sort_keys[i].kind = sort_kind;

                sort_keys[i].reverse = sort_keys[i].reverse || sort_reverse;
                sort_keys[i].how |= sort_how;
        }

        if (null_data)
                text_delimiter = '\0';

        b32 inputs = text_files_count ? text_files_count : 1;
        positive address_to run_stop = (positive address_to)text_arena_take(
            ((positive)inputs + 1) * sizeof(positive));

        if (!run_stop)
                return text_done(2);

        for (b32 i = 0; i < inputs; i++)
        {
                if (!text_open(text_files_count ? text_argument(text_files[i]) : null))
                {
                        run_stop[i] = text_lines_count;
                        continue;
                }

                if (!text_lines_gather())
                        return text_done(2);

                text_close();
                run_stop[i] = text_lines_count;
        }

        // -c reads the lines and says whether they were already in order. It
        // never sorts, so it never allocates the index either.
        if (checking)
        {
                string_address name = text_files_count ? text_argument(text_files[0])
                                                       : (string_address) "-";

                for (positive i = 1; i < text_lines_count; i++)
                {
                        bipolar answer = sort_compare(i - 1, i);

                        if (answer < 0 || (!answer && !sort_unique))
                                continue;

                        if (!checking_quiet)
                                sort_disorder(name, i + 1, text_lines + i);

                        return text_done(1);
                }

                return text_done(0);
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

        /*
                -m does not sort. It takes whichever file has the smallest
                line at its front, over and over, which is the same answer as
                sorting when every file was in order and a different one when
                they were not -- and GNU's answer is the different one.
        */
        if (merging)
        {
                positive address_to head = (positive address_to)text_arena_take(
                    ((positive)inputs + 1) * sizeof(positive));

                if (!head)
                        return text_done(2);

                for (b32 r = 0; r < inputs; r++)
                        head[r] = r ? run_stop[r - 1] : 0;

                for (positive out = 0; out < text_lines_count; out++)
                {
                        b32 best = -1;

                        for (b32 r = 0; r < inputs; r++)
                        {
                                if (head[r] >= run_stop[r])
                                        continue;

                                if (best < 0 || sort_compare(head[r], head[best]) < 0)
                                        best = r;
                        }

                        sort_order[out] = head[best]++;
                }
        }
        else
        {
                sort_run(0, text_lines_count);
        }

        // -o is opened after every line has been read, so sort -o f f still
        // has a file to read.
        if (output)
        {
                bipolar handle = text_open_handle(output, TEXT_WRITE, 0666);

                if (handle < 0)
                {
                        text_error(output, "cannot open for writing");
                        return text_done(2);
                }

                text_out_to((positive)handle);
        }

        for (positive i = 0; i < text_lines_count; i++)
        {
                text_slice address_to line = text_lines + sort_order[i];

                if (sort_unique && i &&
                    !sort_compare_keys(sort_order[i - 1], sort_order[i]))
                        continue;

                text_put(line->at, line->length);
                text_put_character(text_delimiter);
        }

        return text_done(text_status);
}

// cmp -------------------------------------------------------------
/*
        Two files, byte for byte.

        Nothing at all when they are the same, where the first difference is
        when they are not, and a complaint on the error stream when one of
        them is a prefix of the other. -s answers with the status alone.

        -l lists every difference in octal, and is the only part that needs
        to know how large the files are before it starts: the columns are
        lined up to the width of the shorter one, which cannot be known from
        the bytes as they go past.

        Two operands after the names, and -i, say where in each file to start
        -- the byte numbers printed then count from there and not from the
        front -- and -n says how far to go. -b puts the differing bytes
        themselves beside their octal, the way a terminal can show them.
*/
#define CMP_BLOCK 65536

typedef struct
{
        positive handle;
        positive filled;
        positive position;
        bool finished;
        bool opened;
        string_address name;
        p8 buffer[CMP_BLOCK];
} cmp_side;

static cmp_side cmp_left;
static cmp_side cmp_right;

static bool cmp_open(cmp_side address_to side, string_address path)
{
        bipolar handle;

        side->filled = 0;
        side->position = 0;
        side->finished = false;
        side->opened = false;
        side->name = path;

        if (path[0] == '-' && !path[1])
        {
                side->handle = 0;
                return true;
        }

        handle = text_open_handle(path, FILE_READ, 0);

        if (handle < 0)
        {
                text_error(path, "No such file or directory");
                return false;
        }

        side->handle = (positive)handle;
        side->opened = true;

        return true;
}

static positive text_digits(p8 address_to into, positive value)
{
        p8 digits[24];
        positive have = 0;
        positive at = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + value % 10);
                value /= 10;
        }

        while (have)
                into[at++] = digits[--have];

        into[at] = end;

        return at;
}

static bipolar cmp_byte(cmp_side address_to side)
{
        if (side->position == side->filled)
        {
                bipolar got;

                if (side->finished)
                        return -1;

                got = system_call_3(syscall(read), side->handle,
                                    (positive)side->buffer, CMP_BLOCK);

                if (got <= 0)
                {
                        side->finished = true;
                        return -1;
                }

                side->filled = (positive)got;
                side->position = 0;
        }

        return side->buffer[side->position++];
}

// What is left of a side, which is the whole of it until -i has skipped
// something. Nothing, for a pipe: what is behind one has no length until it
// has ended.
static positive cmp_length(cmp_side address_to side)
{
        bipolar here = system_call_3(syscall(lseek), side->handle, 0, FILE_SEEK_CUR);
        bipolar last;

        if (here < 0)
                return 0;

        last = system_call_3(syscall(lseek), side->handle, 0, FILE_SEEK_END);
        system_call_3(syscall(lseek), side->handle, (positive)here, FILE_SEEK_SET);

        return last < 0 || last < here ? 0 : (positive)(last - here);
}

// Past a skip, by seeking where that is allowed and by reading where it is
// not. A skip past the end leaves the side empty rather than failing.
static fn cmp_pass(cmp_side address_to side, positive count)
{
        if (!count)
                return;

        if (system_call_3(syscall(lseek), side->handle, count, FILE_SEEK_CUR) >= 0)
                return;

        while (count-- && cmp_byte(side) >= 0)
                ;
}

// The line is left out when the differences were listed, because that is
// what the tool this is measured against does.
static fn cmp_ended(cmp_side address_to side, positive at, positive line,
                    bool newline, bool listing)
{
        p8 text[24];

        text_flush();
        text_error_raw("cmp: EOF on '");
        text_error_raw(side->name);
        text_error_raw("'");

        if (!at)
        {
                text_error_raw(" which is empty\n");
                return;
        }

        text_error_raw(" after byte ");
        text_digits(text, at);
        text_error_raw(text);

        if (!listing)
        {
                text_error_raw(", in line ");
                text_digits(text, newline ? line : line + 1);
                text_error_raw(text);
        }

        text_error_raw("\n");
}

// A byte the way cmp writes one: ^A for a control, M- in front of anything
// with the high bit set, and the byte itself when it is printable.
static positive cmp_shown(p8 address_to into, positive value)
{
        positive have = 0;

        if (value >= 128)
        {
                into[have++] = 'M';
                into[have++] = '-';
                value -= 128;
        }

        if (value == 127)
        {
                into[have++] = '^';
                into[have++] = '?';
        }
        else if (value < 32)
        {
                into[have++] = '^';
                into[have++] = (p8)(value + 64);
        }
        else
                into[have++] = (p8)value;

        into[have] = end;
        return have;
}

// A skip or a limit: a count, and one of the suffixes the tool this is
// measured against multiplies it by.
static bool cmp_count_of(string_address value, positive address_to result)
{
        string_address letters = (string_address) "KMGTPEZY";
        positive total = 0;
        positive seen = 0;
        positive at = 0;
        positive power = 0;
        positive by = 1024;

        if (!value)
                return false;

        for (; text_digit(value[at]); at++, seen++)
                total = total * 10 + (positive)(value[at] - '0');

        if (!seen)
                return false;

        if (!value[at])
        {
                address_to result = total;
                return true;
        }

        if (value[at] == 'k')
                power = 1;
        else
                for (positive step = 0; letters[step]; step++)
                        if (value[at] == letters[step])
                        {
                                power = step + 1;
                                break;
                        }

        if (!power)
                return false;

        if (value[at + 1] == 'B' && !value[at + 2])
                by = 1000;
        else if (value[at + 1])
                return false;

        for (positive step = 0; step < power; step++)
                total *= by;

        address_to result = total;
        return true;
}

static fn cmp_octal(positive value)
{
        p8 digits[4];
        positive have = 0;

        if (!value)
                digits[have++] = '0';

        while (value)
        {
                digits[have++] = (p8)('0' + (value & 7));
                value >>= 3;
        }

        for (positive pad = have; pad < 3; pad++)
                text_put_character(' ');

        while (have)
                text_put_character(digits[--have]);
}

static text_long cmp_long_options[] = {
    {"print-bytes", 'b', TEXT_LONG_ALONE},
    {"ignore-initial", 'i', TEXT_LONG_NEEDS},
    {"bytes", 'n', TEXT_LONG_NEEDS},
    {"quiet", 's', TEXT_LONG_ALONE},
    {"silent", 's', TEXT_LONG_ALONE},
    {"verbose", 'l', TEXT_LONG_ALONE},
    {null, 0, 0}};

static b32 text_cmp()
{
        bool silent = false;
        bool listing = false;
        bool shown = false;
        b32 index = 1;
        positive at = 0;
        positive lines = 0;
        positive skip_left = 0;
        positive skip_right = 0;
        positive limit = TEXT_UNSET;
        bool newline = true;
        positive width = 1;
        b32 answer = 0;

        text_begin("cmp");

        if (!text_expand_long(cmp_long_options))
                return text_done(2);

        while (index < text_argument_count)
        {
                string_address argument = text_argument(index);

                if (argument[0] != '-' || !argument[1])
                        break;

                if (argument[1] == '-' && !argument[2])
                {
                        index++;
                        break;
                }

                for (positive letter = 1; argument[letter]; letter++)
                {
                        if (argument[letter] == 's')
                                silent = true;
                        else if (argument[letter] == 'l')
                                listing = true;
                        else if (argument[letter] == 'b')
                                shown = true;
                        else if (argument[letter] == 'i' || argument[letter] == 'n')
                        {
                                bool skipping = argument[letter] == 'i';
                                string_address value = argument[letter + 1]
                                                           ? argument + letter + 1
                                                           : text_argument(++index);
                                positive one = 0;

                                if (!value)
                                {
                                        text_error(null, "option requires an argument");
                                        return text_done(2);
                                }

                                // -i takes one count for both sides or, with
                                // a colon between them, one for each.
                                positive split = 0;

                                while (value[split] && value[split] != ':')
                                        split++;

                                p8 head[32];

                                if (skipping && value[split] == ':')
                                {
                                        positive two = 0;

                                        if (split >= sizeof(head))
                                                split = sizeof(head) - 1;

                                        memory_copy(head, value, split);
                                        head[split] = end;

                                        if (!cmp_count_of(head, address_of one) ||
                                            !cmp_count_of(value + split + 1,
                                                          address_of two))
                                        {
                                                text_error(value, "invalid --ignore-initial value");
                                                return text_done(2);
                                        }

                                        skip_left = one;
                                        skip_right = two;
                                        break;
                                }

                                if (!cmp_count_of(value, address_of one))
                                {
                                        text_error(value, skipping
                                                              ? "invalid --ignore-initial value"
                                                              : "invalid --bytes value");
                                        return text_done(2);
                                }

                                if (skipping)
                                        skip_left = skip_right = one;
                                else
                                        limit = one;

                                break;
                        }
                        else
                        {
                                text_error(null, "invalid option");
                                return text_done(2);
                        }
                }

                index++;
        }

        b32 operands = text_argument_count - index;

        if (operands < 1 || operands > 4)
        {
                text_error(null, operands ? "extra operand" : "missing operand");
                return text_done(2);
        }

        // The third and fourth operands say the same thing -i does, and say
        // it last, so they win.
        for (b32 which = 2; which < operands; which++)
        {
                positive value = 0;

                if (!cmp_count_of(text_argument(index + which), address_of value))
                {
                        text_error(text_argument(index + which), "invalid byte count");
                        return text_done(2);
                }

                if (which == 2)
                        skip_left = value;
                else
                        skip_right = value;
        }

        // A second name that was not given is standard input, which is how
        // "cmp saved" reads a pipe against a file.
        if (!cmp_open(address_of cmp_left, text_argument(index)) ||
            !cmp_open(address_of cmp_right,
                      operands > 1 ? text_argument(index + 1)
                                   : (string_address) "-"))
                return text_done(2);

        cmp_pass(address_of cmp_left, skip_left);
        cmp_pass(address_of cmp_right, skip_right);

        if (listing)
        {
                positive one = cmp_length(address_of cmp_left);
                positive two = cmp_length(address_of cmp_right);
                positive smaller = one < two ? one : two;

                if (limit != TEXT_UNSET && limit < smaller)
                        smaller = limit;

                while (smaller >= 10)
                {
                        smaller /= 10;
                        width++;
                }
        }

        for (;;)
        {
                if (limit != TEXT_UNSET && at >= limit)
                        break;

                bipolar a = cmp_byte(address_of cmp_left);
                bipolar b = cmp_byte(address_of cmp_right);

                if (a < 0 && b < 0)
                        break;

                if (a < 0 || b < 0)
                {
                        if (!silent)
                                cmp_ended(a < 0 ? address_of cmp_left
                                                : address_of cmp_right,
                                          at, lines, newline, listing);

                        answer = 1;
                        break;
                }

                at++;
                newline = a == '\n';

                if (a != b)
                {
                        if (silent)
                                return text_done(1);

                        answer = 1;

                        p8 left[8];
                        p8 right[8];
                        positive wide = 0;

                        if (shown)
                        {
                                wide = cmp_shown(left, (positive)a);
                                cmp_shown(right, (positive)b);
                        }

                        if (!listing)
                        {
                                text_put_string(cmp_left.name);
                                text_put_character(' ');
                                text_put_string(cmp_right.name);
                                text_put_string(shown ? " differ: byte "
                                                      : " differ: char ");
                                text_put_number(at, 0);
                                text_put_string(", line ");
                                text_put_number(lines + 1, 0);

                                if (shown)
                                {
                                        text_put_string(" is ");
                                        cmp_octal((positive)a);
                                        text_put_character(' ');
                                        text_put_string(left);
                                        text_put_character(' ');
                                        cmp_octal((positive)b);
                                        text_put_character(' ');
                                        text_put_string(right);
                                }

                                text_put_character('\n');
                                break;
                        }

                        text_put_number(at, width);
                        text_put_character(' ');
                        cmp_octal((positive)a);
                        text_put_character(' ');

                        if (shown)
                        {
                                text_put_string(left);

                                // The listing's own column, which is four
                                // wide because M-^? is.
                                while (wide++ < 4)
                                        text_put_character(' ');

                                text_put_character(' ');
                        }

                        cmp_octal((positive)b);

                        if (shown)
                        {
                                text_put_character(' ');
                                text_put_string(right);
                        }

                        text_put_character('\n');
                }

                if (newline)
                        lines++;
        }

        return text_done(answer);
}

// expr ------------------------------------------------------------
/*
        One expression spread across the words, and its value on standard
        output.

        There is no lexer: the shell already separated the words, so a token
        is a word and an operator is a word that is nothing but the operator.
        What is left is the six levels of precedence below, weakest first,
        and a value that is either a number or a string and knows which.

        Status is not the usual one. Zero means the value is neither empty nor
        zero, one means it is, and two means the expression was not one.
*/
#define EXPR_ARENA 8192

typedef struct
{
        string_address text;
        bipolar number;
} expr_value;

static string_address expr_empty = (string_address) "";

static p8 expr_arena[EXPR_ARENA];
static positive expr_arena_used;
static b32 expr_at;
static b32 expr_count;
static b32 expr_fault;

// Inside a branch whose value is already decided. The words still have to be
// walked, so a malformed one is still refused, but dividing by zero in a half
// of an expression nobody will read is not an error.
static b32 expr_dead;

static fn expr_stop(string_address reason)
{
        if (!expr_fault)
                text_error(null, reason);

        expr_fault = 1;
}

static string_address expr_keep(string_address from, positive length)
{
        p8 address_to made;

        if (expr_arena_used + length + 1 > EXPR_ARENA)
        {
                expr_stop("expression too long");
                return expr_empty;
        }

        made = expr_arena + expr_arena_used;

        memory_copy(made, from, length);
        made[length] = end;
        expr_arena_used += length + 1;

        return made;
}

static string_address expr_shown(expr_value address_to value)
{
        p8 text[32];
        positive length;

        if (value->text)
                return value->text;

        if (value->number < 0)
        {
                text[0] = '-';
                length = 1 + text_digits(text + 1, (positive)(-value->number));
        }
        else
                length = text_digits(text, (positive)value->number);

        value->text = expr_keep(text, length);

        return value->text;
}

// A string is a number only when the whole of it is one; " 1" and "1x" are
// strings, which is why the walk has to reach the terminator.
static bool expr_integer(expr_value address_to value, bipolar address_to out)
{
        string_address at;
        bipolar made = 0;
        bool negative = false;
        bool any = false;

        if (!value->text)
        {
                address_to out = value->number;
                return true;
        }

        at = value->text;

        if (string_get(at) == '-' || string_get(at) == '+')
        {
                negative = string_get(at) == '-';
                at++;
        }

        while (string_get(at) >= '0' && string_get(at) <= '9')
        {
                made = made * 10 + (string_get(at) - '0');
                at++;
                any = true;
        }

        if (!any || string_get(at))
                return false;

        address_to out = negative ? -made : made;

        return true;
}

static bool expr_true(expr_value address_to value)
{
        bipolar number;

        if (!value->text)
                return value->number != 0;

        if (!string_get(value->text))
                return false;

        if (expr_integer(value, address_of number))
                return number != 0;

        return true;
}

static expr_value expr_zero()
{
        expr_value made = {null, 0};

        return made;
}

static string_address expr_word()
{
        return expr_at < expr_count ? text_argument(expr_at) : null;
}

static bool expr_is(string_address text)
{
        string_address word = expr_word();

        return word && !string_compare(word, text);
}

/*
        The match operator, and match, which is the same thing spelled out.

        Anchored at the start, which the engine here has no flag for -- so the
        match is searched for and then refused unless it began at nothing. A
        pattern with a group answers with what the group took, and one without
        answers with how many characters it took.
*/
static expr_value expr_matched(expr_value address_to subject,
                               expr_value address_to pattern)
{
        expr_value made = expr_zero();
        string_address text = expr_shown(subject);
        string_address rule = expr_shown(pattern);
        positive length = string_length(text);

        if (!regex_compile(rule, false, false, false))
        {
                if (!expr_dead)
                        expr_stop("invalid expression");

                return made;
        }

        if (!regex_search_longest(text, length, 0) || regex_slots[0])
        {
                if (regex_group_count)
                        made.text = expr_empty;

                return made;
        }

        if (regex_group_count)
        {
                positive from = regex_slots[2];
                positive to = regex_slots[3];

                made.text = from == TEXT_UNSET || to == TEXT_UNSET
                                ? expr_empty
                                : expr_keep(text + from, to - from);

                return made;
        }

        made.number = (bipolar)regex_slots[1];

        return made;
}

static expr_value expr_any();

static expr_value expr_primary()
{
        expr_value made = expr_zero();
        string_address word = expr_word();

        if (!word)
        {
                expr_stop("syntax error");
                return made;
        }

        if (!string_compare(word, "("))
        {
                expr_at++;
                made = expr_any();

                if (expr_fault)
                        return made;

                if (!expr_is(")"))
                {
                        expr_stop("syntax error");
                        return made;
                }

                expr_at++;

                return made;
        }

        if (!string_compare(word, "length"))
        {
                expr_value of;

                expr_at++;
                of = expr_primary();
                made.number = (bipolar)string_length(expr_shown(address_of of));

                return made;
        }

        if (!string_compare(word, "match"))
        {
                expr_value subject;
                expr_value pattern;

                expr_at++;
                subject = expr_primary();
                pattern = expr_primary();

                if (expr_fault)
                        return made;

                return expr_matched(address_of subject, address_of pattern);
        }

        if (!string_compare(word, "index"))
        {
                expr_value of;
                expr_value set;
                string_address text;
                string_address wanted;

                expr_at++;
                of = expr_primary();
                set = expr_primary();

                if (expr_fault)
                        return made;

                text = expr_shown(address_of of);
                wanted = expr_shown(address_of set);

                for (positive i = 0; string_get(text + i); i++)
                        for (positive j = 0; string_get(wanted + j); j++)
                                if (string_get(text + i) == string_get(wanted + j))
                                {
                                        made.number = (bipolar)(i + 1);
                                        return made;
                                }

                return made;
        }

        if (!string_compare(word, "substr"))
        {
                expr_value of;
                expr_value from;
                expr_value span;
                string_address text;
                positive whole;
                bipolar start;
                bipolar length;

                expr_at++;
                of = expr_primary();
                from = expr_primary();
                span = expr_primary();

                if (expr_fault)
                        return made;

                text = expr_shown(address_of of);
                whole = string_length(text);
                made.text = expr_empty;

                if (!expr_integer(address_of from, address_of start) ||
                    !expr_integer(address_of span, address_of length) ||
                    start < 1 || length < 1 || (positive)start > whole)
                        return made;

                if ((positive)start - 1 + (positive)length > whole)
                        length = (bipolar)(whole - ((positive)start - 1));

                made.text = expr_keep(text + start - 1, (positive)length);

                return made;
        }

        expr_at++;
        made.text = word;

        return made;
}

static expr_value expr_match_level()
{
        expr_value left = expr_primary();

        while (!expr_fault && expr_is(":"))
        {
                expr_value right;

                expr_at++;
                right = expr_primary();

                if (expr_fault)
                        break;

                left = expr_matched(address_of left, address_of right);
        }

        return left;
}

static bool expr_pair(expr_value address_to left, expr_value address_to right,
                      bipolar address_to a, bipolar address_to b)
{
        if (expr_integer(left, a) && expr_integer(right, b))
                return true;

        if (expr_dead)
        {
                address_to a = 0;
                address_to b = 0;

                return true;
        }

        expr_stop("non-integer argument");

        return false;
}

static expr_value expr_product_level()
{
        expr_value left = expr_match_level();

        while (!expr_fault && (expr_is("*") || expr_is("/") || expr_is("%")))
        {
                p8 operator = string_get(expr_word());
                expr_value right;
                bipolar a;
                bipolar b;

                expr_at++;
                right = expr_match_level();

                if (expr_fault ||
                    !expr_pair(address_of left, address_of right,
                               address_of a, address_of b))
                        break;

                if (operator != '*' && !b)
                {
                        if (!expr_dead)
                        {
                                expr_stop("division by zero");
                                break;
                        }

                        b = 1;
                }

                left.text = null;
                left.number = operator == '*' ? a * b
                              : operator == '/' ? a / b
                                                : a % b;
        }

        return left;
}

static expr_value expr_sum_level()
{
        expr_value left = expr_product_level();

        while (!expr_fault && (expr_is("+") || expr_is("-")))
        {
                bool adding = expr_is("+");
                expr_value right;
                bipolar a;
                bipolar b;

                expr_at++;
                right = expr_product_level();

                if (expr_fault ||
                    !expr_pair(address_of left, address_of right,
                               address_of a, address_of b))
                        break;

                left.text = null;
                left.number = adding ? a + b : a - b;
        }

        return left;
}

static bipolar expr_relation(string_address operator, bipolar order)
{
        if (!string_compare(operator, "="))
                return order == 0;

        if (!string_compare(operator, "!="))
                return order != 0;

        if (!string_compare(operator, "<"))
                return order < 0;

        if (!string_compare(operator, "<="))
                return order <= 0;

        if (!string_compare(operator, ">"))
                return order > 0;

        return order >= 0;
}

static expr_value expr_compare_level()
{
        expr_value left = expr_sum_level();

        while (!expr_fault && (expr_is("=") || expr_is("!=") || expr_is("<") ||
                               expr_is("<=") || expr_is(">") || expr_is(">=")))
        {
                string_address operator = expr_word();
                expr_value right;
                bipolar a;
                bipolar b;
                bipolar order;

                expr_at++;
                right = expr_sum_level();

                if (expr_fault)
                        break;

                // Two numbers are compared as numbers and anything else as
                // bytes, so 3 is below 10 but "3" is above "10".
                if (expr_integer(address_of left, address_of a) &&
                    expr_integer(address_of right, address_of b))
                        order = a < b ? -1 : a > b;
                else
                {
                        bipolar difference =
                            string_compare(expr_shown(address_of left),
                                           expr_shown(address_of right));

                        order = difference < 0 ? -1 : difference > 0;
                }

                left.text = null;
                left.number = expr_relation(operator, order);
        }

        return left;
}

static expr_value expr_both_level()
{
        expr_value left = expr_compare_level();

        while (!expr_fault && expr_is("&"))
        {
                bool decided = !expr_true(address_of left);
                expr_value right;

                expr_at++;
                expr_dead += decided;
                right = expr_compare_level();
                expr_dead -= decided;

                if (expr_fault)
                        break;

                if (decided || !expr_true(address_of right))
                        left = expr_zero();
        }

        return left;
}

static expr_value expr_any()
{
        expr_value left = expr_both_level();

        while (!expr_fault && expr_is("|"))
        {
                bool decided = expr_true(address_of left);
                expr_value right;

                expr_at++;
                expr_dead += decided;
                right = expr_both_level();
                expr_dead -= decided;

                if (expr_fault)
                        break;

                if (!decided)
                        left = expr_true(address_of right) ? right : expr_zero();
        }

        return left;
}

static b32 text_expr()
{
        expr_value result;

        text_begin("expr");

        expr_at = 1;
        expr_count = text_argument_count;
        expr_arena_used = 0;
        expr_fault = 0;
        expr_dead = 0;

        if (expr_at < expr_count && !string_compare(text_argument(expr_at), "--"))
                expr_at++;

        if (expr_at >= expr_count)
        {
                text_error(null, "missing operand");
                return text_done(2);
        }

        result = expr_any();

        if (!expr_fault && expr_at < expr_count)
                expr_stop("syntax error");

        if (expr_fault)
                return text_done(2);

        text_put_string(expr_shown(address_of result));
        text_put_character('\n');

        return text_done(expr_true(address_of result) ? 0 : 1);
}
